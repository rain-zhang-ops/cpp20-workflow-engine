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
    // 删除上次运行遗留的 socket 文件，使 bind() 能成功。
    ::unlink(socket_path_.c_str());

    setup_socket();

    // Register atexit handler for crash/abnormal-exit cleanup.
    // Store only one instance pointer; later construction overwrites earlier.
    // 注册 atexit 处理函数，用于崩溃/异常退出时的清理。
    instance_ = this;
    std::atexit(ControlPlane::atexit_handler);
}

// ============================================================================
// ControlPlane — destructor
// ============================================================================

ControlPlane::~ControlPlane()
{
    // Close the listening socket first so no new connections arrive.
    // 先关闭监听 socket，拒绝新连接。
    server_fd_.reset();

    // Remove the socket file from the filesystem.
    // 从文件系统删除 socket 文件。
    ::unlink(socket_path_.c_str());

    // Invalidate the static instance pointer so the atexit handler is a no-op.
    // 置空静态实例指针，使 atexit 处理函数成为空操作。
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
    // Create a Unix Domain Socket (stream).
    // 创建 Unix Domain Socket（流式）。
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("ControlPlane: socket() failed: ") +
            std::strerror(errno));
    }
    server_fd_ = FdGuard(fd);

    // Bind to the socket path.
    // 绑定到 socket 路径。
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error(
            "ControlPlane: socket path too long: " + socket_path_);
    }
    // Use strncpy to fill the fixed-size array.
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error(
            std::string("ControlPlane: bind() failed: ") +
            std::strerror(errno));
    }

    // Start listening (backlog = 8 is plenty for a CLI control interface).
    // 开始监听（backlog = 8 对 CLI 控制接口已够用）。
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
    // Store under the upper-cased command so matching is case-insensitive.
    // 以大写形式存储，使命令匹配不区分大小写。
    std::string key = cmd;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    handlers_[std::move(key)] = std::move(handler);
}

// ============================================================================
// start_control_plane — main event loop
// ============================================================================

void ControlPlane::start_control_plane(std::stop_token st)
{
    constexpr int POLL_TIMEOUT_MS = 100; ///< Wake up at most every 100 ms

    std::cout << "[ControlPlane] Listening on " << socket_path_ << "\n";

    while (!st.stop_requested()) {
        // --------------------------------------------------------------------------
        // Poll the listening socket with a 100 ms timeout.
        // This ensures zero CPU usage when idle and fast response when active.
        //
        // 以 100 ms 超时轮询监听 socket。
        // 确保空闲时 CPU 占用为 0，有连接时响应及时。
        // --------------------------------------------------------------------------
        pollfd pfd{};
        pfd.fd     = server_fd_.fd;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (rc < 0) {
            if (errno == EINTR) continue; // interrupted by signal — retry
            std::cerr << "[ControlPlane] poll() error: "
                      << std::strerror(errno) << "\n";
            break;
        }

        if (rc == 0) {
            // Timeout — no client; check stop_token and loop.
            // 超时 —— 无客户端；检查 stop_token 后继续循环。
            continue;
        }

        if (!(pfd.revents & POLLIN)) continue;

        // ------------------------------------------------------------------
        // Accept the incoming client connection.
        // 接受传入的客户端连接。
        // ------------------------------------------------------------------
        int client_fd = ::accept4(server_fd_.fd, nullptr, nullptr,
                                  SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::cerr << "[ControlPlane] accept4() error: "
                      << std::strerror(errno) << "\n";
            continue;
        }

        // Process client synchronously (CLI sends one command then disconnects).
        // 同步处理客户端（CLI 发送一条命令后立即断开）。
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
        // ------------------------------------------------------------------
        // Wait up to 3 seconds for data to arrive on the client socket.
        // 最多等待 3 秒，等待客户端 socket 上的数据到达。
        // ------------------------------------------------------------------
        constexpr int CLIENT_TIMEOUT_MS = 3000;
        pollfd pfd{};
        pfd.fd     = client_fd;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, CLIENT_TIMEOUT_MS);
        if (rc <= 0) {
            // Timeout or error — send a brief error reply.
            const char* msg = "ERROR: timeout waiting for command\n";
            (void)::write(client_fd, msg, std::strlen(msg));
            return;
        }

        // ------------------------------------------------------------------
        // Read the command.  Limit to 1024 bytes to prevent abuse.
        // 读取命令，限制 1024 字节防止滥用。
        // ------------------------------------------------------------------
        constexpr std::size_t MAX_CMD_LEN = 1024;
        char buf[MAX_CMD_LEN + 1]{};
        ssize_t n = ::read(client_fd, buf, MAX_CMD_LEN);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                const char* msg = "ERROR: no data\n";
                (void)::write(client_fd, msg, std::strlen(msg));
            }
            return;
        }
        if (n == 0) return; // Client closed before sending anything

        std::string cmd(buf, static_cast<std::size_t>(n));

        // Trim whitespace / newlines from both ends.
        // 去除两端的空白字符/换行符。
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        cmd.erase(cmd.begin(),
                  std::find_if(cmd.begin(), cmd.end(), not_space));
        cmd.erase(std::find_if(cmd.rbegin(), cmd.rend(), not_space).base(),
                  cmd.end());

        if (cmd.empty()) {
            const char* msg = "ERROR: empty command\n";
            (void)::write(client_fd, msg, std::strlen(msg));
            return;
        }

        if (cmd.size() > 256) {
            const char* msg = "ERROR: command too long\n";
            (void)::write(client_fd, msg, std::strlen(msg));
            return;
        }

        // ------------------------------------------------------------------
        // Dispatch and send response.
        // 分发并发送响应。
        // ------------------------------------------------------------------
        std::string response = dispatch(cmd);
        response += '\n'; // Ensure line-ending for CLI readability

        // Write the full response, retrying on EINTR.
        // 写入完整响应，遇到 EINTR 重试。
        const char* ptr  = response.data();
        std::size_t left = response.size();
        while (left > 0) {
            ssize_t written = ::write(client_fd, ptr, left);
            if (written < 0) {
                if (errno == EINTR) continue;
                break;
            }
            ptr  += written;
            left -= static_cast<std::size_t>(written);
        }

    } catch (const std::exception& e) {
        // Last-resort: log but do not propagate — we must not throw from noexcept.
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
    // Upper-case for case-insensitive matching.
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
