#include "ControlPlane.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>

// POSIX headers for SO_RCVTIMEO on the accepted socket
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>   // std::atexit
#include <cstring>   // std::strerror
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>

// ============================================================================
// Static member definition
// ============================================================================

ControlPlane* ControlPlane::instance_ = nullptr;

// ============================================================================
// ControlPlane — constructor
// ============================================================================

ControlPlane::ControlPlane(std::string socket_path)
    : socket_path_(std::move(socket_path))
    , ioc_()
    , acceptor_(ioc_)
{
    // Remove stale socket file from a previous run so bind() succeeds.
    ::unlink(socket_path_.c_str());

    Protocol::endpoint endpoint(socket_path_);

    boost::system::error_code ec;
    acceptor_.open(endpoint.protocol(), ec);
    if (ec)
        throw std::runtime_error("ControlPlane: acceptor open failed: " +
                                 ec.message());

    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);

    acceptor_.bind(endpoint, ec);
    if (ec)
        throw std::runtime_error("ControlPlane: bind failed: " + ec.message());

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec)
        throw std::runtime_error("ControlPlane: listen failed: " + ec.message());

    // Register atexit handler for crash/abnormal-exit cleanup.
    instance_ = this;
    std::atexit(ControlPlane::atexit_handler);
}

// ============================================================================
// ControlPlane — destructor
// ============================================================================

ControlPlane::~ControlPlane()
{
    boost::system::error_code ec;
    if (acceptor_.is_open())
        acceptor_.close(ec);

    ::unlink(socket_path_.c_str());

    if (instance_ == this)
        instance_ = nullptr;
}

// ============================================================================
// atexit_handler — called on normal process exit (or via atexit on abnormal)
// ============================================================================

void ControlPlane::atexit_handler() noexcept
{
    if (instance_)
        ::unlink(instance_->socket_path_.c_str());
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
// start_control_plane — async event loop
//
// Registers a std::stop_callback that stops the io_context when the jthread's
// stop token fires, then runs the async accept loop until stop is requested.
// ============================================================================

void ControlPlane::start_control_plane(std::stop_token st)
{
    std::cout << "[ControlPlane] Listening on " << socket_path_ << "\n";

    // When the owning jthread requests stop, cancel the io_context so that
    // the pending async_accept is aborted and ioc_.run() returns.
    std::stop_callback stop_cb(st, [this]() noexcept {
        ioc_.stop();
    });

    do_accept();
    ioc_.run();  // blocks until ioc_.stop() is called

    std::cout << "[ControlPlane] Stopping — cleaning up " << socket_path_ << "\n";

    boost::system::error_code ec;
    if (acceptor_.is_open())
        acceptor_.close(ec);

    ::unlink(socket_path_.c_str());
    if (instance_ == this)
        instance_ = nullptr;
}

// ============================================================================
// do_accept — post a single async_accept; re-arms itself on success
// ============================================================================

void ControlPlane::do_accept()
{
    acceptor_.async_accept(
        [this](boost::system::error_code ec, Protocol::socket peer) {
            if (ec) {
                // operation_aborted is expected when ioc_.stop() is called.
                if (ec != boost::asio::error::operation_aborted)
                    std::cerr << "[ControlPlane] accept error: "
                              << ec.message() << "\n";
                return;
            }

            handle_session(std::move(peer));
            do_accept();   // re-arm for the next connection
        });
}

// ============================================================================
// handle_session — synchronous command read and response write
//
// The accepted socket is switched to blocking mode so that a plain read_some()
// call blocks until data arrives (or the SO_RCVTIMEO deadline fires).
// ============================================================================

void ControlPlane::handle_session(Protocol::socket sock) noexcept
{
    try {
        boost::system::error_code ec;

        // Switch to blocking mode for straightforward synchronous I/O.
        sock.non_blocking(false, ec);
        if (ec) return;

        // Apply a 3-second receive deadline so a stalled client cannot block
        // the control-plane thread indefinitely.
        struct timeval tv{3, 0};
        ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     &tv, sizeof(tv));

        constexpr std::size_t MAX_CMD_LEN = 1024;
        char buf[MAX_CMD_LEN + 1]{};

        std::size_t n = sock.read_some(boost::asio::buffer(buf, MAX_CMD_LEN), ec);

        if (ec || n == 0) {
            const std::string_view msg = "ERROR: timeout waiting for command\n";
            boost::asio::write(sock, boost::asio::buffer(msg), ec);
            return;
        }

        std::string cmd(buf, n);

        // Trim leading and trailing whitespace.
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        cmd.erase(cmd.begin(),
                  std::find_if(cmd.begin(), cmd.end(), not_space));
        cmd.erase(std::find_if(cmd.rbegin(), cmd.rend(), not_space).base(),
                  cmd.end());

        if (cmd.empty()) {
            const std::string_view msg = "ERROR: empty command\n";
            boost::asio::write(sock, boost::asio::buffer(msg), ec);
            return;
        }

        if (cmd.size() > 256) {
            const std::string_view msg = "ERROR: command too long\n";
            boost::asio::write(sock, boost::asio::buffer(msg), ec);
            return;
        }

        std::string response = dispatch(cmd);
        response += '\n';
        boost::asio::write(sock, boost::asio::buffer(response), ec);

    } catch (const std::exception& e) {
        std::cerr << "[ControlPlane] handle_session exception: "
                  << e.what() << "\n";
    } catch (...) {
        std::cerr << "[ControlPlane] handle_session: unknown exception\n";
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
               "'. Known commands: RELOAD, STOP, STATUS";
    }

    try {
        return it->second();
    } catch (const std::exception& e) {
        return std::string("ERROR: handler threw: ") + e.what();
    } catch (...) {
        return "ERROR: handler threw unknown exception";
    }
}
