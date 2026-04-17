#include "ControlPlane.h"

// Linux-specific socket headers
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>   // std::atexit
#include <cstring>   // std::strerror, std::strcpy
#include <iostream>
#include <stdexcept>
#include <string>

// ============================================================================
// Static member definition
// ============================================================================

ControlPlane* ControlPlane::instance_ = nullptr;

// ============================================================================
// FdGuard::reset — close the fd if open
// ============================================================================

void ControlPlane::FdGuard::reset() noexcept
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// ============================================================================
// ControlPlane — constructor
// ============================================================================

ControlPlane::ControlPlane(std::string socket_path)
    : socket_path_(std::move(socket_path))
{
    // Remove stale socket file from a previous run so bind() succeeds.
    ::unlink(socket_path_.c_str());

    setup_socket();

    // Register atexit handler for crash/abnormal-exit cleanup.
    instance_ = this;
    std::atexit(ControlPlane::atexit_handler);
}

// ============================================================================
// ControlPlane — destructor
// ============================================================================

ControlPlane::~ControlPlane()
{
    server_fd_.reset();
    ::unlink(socket_path_.c_str());

    if (instance_ == this) {
        instance_ = nullptr;
    }
}

// ============================================================================
// atexit_handler — called on normal process exit (or via atexit on abnormal)
// ============================================================================

void ControlPlane::atexit_handler() noexcept
{
    if (instance_) {
        ::unlink(instance_->socket_path_.c_str());
    }
}

// ============================================================================
// setup_socket — create, configure, bind, and listen on the UDS socket
// ============================================================================

void ControlPlane::setup_socket()
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("ControlPlane: socket() failed: ") +
            std::strerror(errno));
    }
    server_fd_ = FdGuard(fd);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error(
            "ControlPlane: socket path too long: " + socket_path_);
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(
            std::string("ControlPlane: bind() failed: ") +
            std::strerror(errno));
    }

    if (::listen(fd, 8) < 0) {
        throw std::runtime_error(
            std::string("ControlPlane: listen() failed: ") +
            std::strerror(errno));
    }
}

// ============================================================================
// register_command
// ============================================================================

void ControlPlane::register_command(const std::string&          cmd,
                                    std::function<std::string()> handler)
{
    std::string key = cmd;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    handlers_[std::move(key)] = std::move(handler);
}

// ============================================================================
// write_all — helper to write all bytes, retrying on EINTR / partial writes
// ============================================================================

static void write_all(int fd, const char* data, std::size_t len) noexcept
{
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ControlPlane] write error: "
                      << std::strerror(errno) << "\n";
            return;
        }
        data += n;
        len  -= static_cast<std::size_t>(n);
    }
}

// ============================================================================
// start_control_plane — main event loop
// ============================================================================

void ControlPlane::start_control_plane(std::stop_token st)
{
    constexpr int POLL_TIMEOUT_MS = 100;

    std::cout << "[ControlPlane] Listening on " << socket_path_ << "\n";

    while (!st.stop_requested()) {
        pollfd pfd{};
        pfd.fd     = server_fd_.fd;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (rc < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[ControlPlane] poll() error: "
                      << std::strerror(errno) << "\n";
            break;
        }

        if (rc == 0) continue;

        if (!(pfd.revents & POLLIN)) continue;

        int client_fd = ::accept4(server_fd_.fd, nullptr, nullptr,
                                  SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::cerr << "[ControlPlane] accept4() error: "
                      << std::strerror(errno) << "\n";
            continue;
        }

        handle_client(client_fd);
        ::close(client_fd);
    }

    std::cout << "[ControlPlane] Stopping — cleaning up " << socket_path_ << "\n";
    server_fd_.reset();
    ::unlink(socket_path_.c_str());
    if (instance_ == this) instance_ = nullptr;
}

// ============================================================================
// handle_client — read command, dispatch, write response
// ============================================================================

void ControlPlane::handle_client(int client_fd) noexcept
{
    try {
        constexpr int CLIENT_TIMEOUT_MS = 3000;
        pollfd pfd{};
        pfd.fd     = client_fd;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, CLIENT_TIMEOUT_MS);
        if (rc <= 0) {
            const char* msg = "ERROR: timeout waiting for command\n";
            write_all(client_fd, msg, std::strlen(msg));
            return;
        }

        constexpr std::size_t MAX_CMD_LEN = 1024;
        char buf[MAX_CMD_LEN + 1]{};
        ssize_t n = ::read(client_fd, buf, MAX_CMD_LEN);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                const char* msg = "ERROR: no data\n";
                write_all(client_fd, msg, std::strlen(msg));
            }
            return;
        }
        if (n == 0) return;

        std::string cmd(buf, static_cast<std::size_t>(n));

        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        cmd.erase(cmd.begin(),
                  std::find_if(cmd.begin(), cmd.end(), not_space));
        cmd.erase(std::find_if(cmd.rbegin(), cmd.rend(), not_space).base(),
                  cmd.end());

        if (cmd.empty()) {
            const char* msg = "ERROR: empty command\n";
            write_all(client_fd, msg, std::strlen(msg));
            return;
        }

        if (cmd.size() > 256) {
            const char* msg = "ERROR: command too long\n";
            write_all(client_fd, msg, std::strlen(msg));
            return;
        }

        std::string response = dispatch(cmd);
        response += '\n';

        write_all(client_fd, response.data(), response.size());

    } catch (const std::exception& e) {
        std::cerr << "[ControlPlane] handle_client exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[ControlPlane] handle_client: unknown exception\n";
    }
}

// ============================================================================
// dispatch — upper-case the command and call the matching handler
// ============================================================================

std::string ControlPlane::dispatch(const std::string& cmd)
{
    std::string key = cmd;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    auto it = handlers_.find(key);
    if (it == handlers_.end()) {
        return "ERROR: unknown command '" + cmd +
               ". Known commands: RELOAD, STOP, STATUS";
    }

    try {
        return it->second();
    } catch (const std::exception& e) {
        return std::string("ERROR: handler threw: ") + e.what();
    } catch (...) {
        return "ERROR: handler threw unknown exception";
    }
}