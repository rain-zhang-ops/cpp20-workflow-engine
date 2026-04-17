// ============================================================================
// workflow-cli — Command-line client for the workflow engine control plane
//
// workflow-cli —— 工作流引擎控制平面命令行客户端
// ============================================================================
//
// USAGE / 使用方式
// ----------------
//   ./workflow-cli reload
//   ./workflow-cli stop
//   ./workflow-cli status
//   ./workflow-cli --socket /custom/path.sock status
//
// The tool connects to the UDS socket, sends the command (upper-cased),
// reads the response, prints it, and exits.
//
// 该工具连接到 UDS socket，发送命令（转为大写），读取响应，打印后退出。
//
// TIMEOUT / 超时
// --------------
// Connection attempt: non-blocking connect with 3-second poll timeout.
// Send / Receive:      poll with 3-second timeout on each operation.
//
// 连接尝试：非阻塞 connect + 3 秒 poll 超时。
// 发送/接收：每次操作 3 秒 poll 超时。
// ============================================================================

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Print a friendly error if the engine is not running.
/// 若引擎未启动，打印友好的错误提示。
void print_connection_error(const std::string& socket_path)
{
    std::cerr << "[workflow-cli] Cannot connect to " << socket_path << "\n"
              << "  引擎未启动，请先启动 workflow_engine。\n"
              << "  (Engine is not running — please start workflow_engine first.)\n";
}

/// Create and connect a non-blocking UDS socket.
/// Returns the fd on success, or -1 on failure.
/// 创建并连接非阻塞 UDS socket。成功返回 fd，失败返回 -1。
int connect_socket(const std::string& socket_path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::cerr << "[workflow-cli] socket() failed: "
                  << std::strerror(errno) << "\n";
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[workflow-cli] socket path too long\n";
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    // Non-blocking connect — EINPROGRESS means connect is in progress.
    // 非阻塞 connect —— EINPROGRESS 表示连接正在进行。
    int rc = ::connect(fd,
                       reinterpret_cast<const sockaddr*>(&addr),
                       sizeof(addr));
    if (rc == 0) {
        // Immediate success (Unix domain sockets often connect immediately).
        // 立即成功（Unix domain socket 通常立即连接）。
        return fd;
    }

    if (errno != EINPROGRESS) {
        print_connection_error(socket_path);
        ::close(fd);
        return -1;
    }

    // Wait up to 3 seconds for the connection to complete.
    // 等待最多 3 秒以完成连接。
    constexpr int CONNECT_TIMEOUT_MS = 3000;
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLOUT;

    int poll_rc = ::poll(&pfd, 1, CONNECT_TIMEOUT_MS);
    if (poll_rc <= 0) {
        print_connection_error(socket_path);
        ::close(fd);
        return -1;
    }

    // Verify the connection succeeded (getsockopt SO_ERROR).
    // 通过 getsockopt SO_ERROR 验证连接是否成功。
    int sock_err = 0;
    socklen_t len = sizeof(sock_err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
    if (sock_err != 0) {
        print_connection_error(socket_path);
        ::close(fd);
        return -1;
    }

    return fd;
}

/// Send all bytes in buf, retrying on EINTR.
/// Returns true on success, false on error.
/// 发送 buf 中的所有字节，遇到 EINTR 重试。成功返回 true，出错返回 false。
bool send_all(int fd, std::string_view data, int timeout_ms)
{
    const char* ptr  = data.data();
    std::size_t left = data.size();

    while (left > 0) {
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            std::cerr << "[workflow-cli] send timeout\n";
            return false;
        }

        ssize_t n = ::write(fd, ptr, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[workflow-cli] write() failed: "
                      << std::strerror(errno) << "\n";
            return false;
        }
        ptr  += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

/// Read the full response until EOF or timeout.
/// 读取完整响应，直到 EOF 或超时。
std::string recv_response(int fd, int timeout_ms)
{
    std::string response;
    char buf[4096];

    while (true) {
        pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc == 0) break;  // timeout — assume response is complete
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break; // EOF or error
        response.append(buf, static_cast<std::size_t>(n));
    }
    return response;
}

} // anonymous namespace

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[])
{
    std::string socket_path = "/tmp/workflow.sock";
    std::string command;

    // ------------------------------------------------------------------
    // Parse arguments:
    //   workflow-cli [--socket <path>] <command>
    // ------------------------------------------------------------------
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);

        if (arg == "--socket" || arg == "-s") {
            if (i + 1 >= argc) {
                std::cerr << "Usage: workflow-cli [--socket <path>] <command>\n";
                return 1;
            }
            socket_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: workflow-cli [--socket <path>] <command>\n"
                      << "  Commands: reload  stop  status\n"
                      << "  --socket <path>   Custom UDS socket path "
                         "(default: /tmp/workflow.sock)\n";
            return 0;
        } else {
            command = std::string(arg);
        }
    }

    if (command.empty()) {
        std::cerr << "Usage: workflow-cli [--socket <path>] <command>\n"
                  << "  Commands: reload  stop  status\n";
        return 1;
    }

    // Convert command to upper-case so the daemon matches it.
    // 将命令转为大写，使守护进程能匹配。
    std::transform(command.begin(), command.end(), command.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    // ------------------------------------------------------------------
    // Connect to the daemon socket.
    // 连接到守护进程 socket。
    // ------------------------------------------------------------------
    int fd = connect_socket(socket_path);
    if (fd < 0) return 1;

    // ------------------------------------------------------------------
    // Send command.
    // 发送命令。
    // ------------------------------------------------------------------
    constexpr int IO_TIMEOUT_MS = 3000;
    if (!send_all(fd, command, IO_TIMEOUT_MS)) {
        ::close(fd);
        return 1;
    }

    // Signal end-of-write so the server knows we won't send more.
    // 发送写半关闭信号，通知服务器不再发送数据。
    ::shutdown(fd, SHUT_WR);

    // ------------------------------------------------------------------
    // Read and print response.
    // 读取并打印响应。
    // ------------------------------------------------------------------
    std::string response = recv_response(fd, IO_TIMEOUT_MS);
    ::close(fd);

    if (response.empty()) {
        std::cerr << "[workflow-cli] No response from engine.\n";
        return 1;
    }

    std::cout << response;
    return 0;
}
