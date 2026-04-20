#include "ControlPlane.h"

// ControlPlane 实现 — Boost.Asio 异步 UDS 服务器，stop_token 驱动优雅关闭
// 关键设计：stop_callback 调用 ioc_.stop()，取消 async_accept 并使 ioc_.run() 返回

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
    // 先 unlink 旧 socket 文件，防止 bind() 因地址已占用而失败
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

    // atexit：进程异常退出时仍能 unlink socket 文件，防止遗留僵尸 socket
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
// 命令关键字统一转为大写，保证 CLI 输入大小写不敏感
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

    // stop_callback：stop_token 触发时停止 io_context，取消挂起的 async_accept
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
// 每次成功 accept 后再次调用自身，形成持续监听循环（非递归，由 io_context 调度）
// ============================================================================

void ControlPlane::do_accept()
{
    acceptor_.async_accept(
        [this](boost::system::error_code ec, Protocol::socket peer) {
            if (ec) {
                // operation_aborted 是 ioc_.stop() 取消 async_accept 的正常结果，不报错
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
// SO_RCVTIMEO 3 秒超时：防止滞留客户端长期阻塞控制平面线程
// ============================================================================

void ControlPlane::handle_session(Protocol::socket sock) noexcept
{
    try {
        boost::system::error_code ec;

        // 切换为阻塞模式：控制平面处理命令为短交互，同步 I/O 更简单可靠
        sock.non_blocking(false, ec);
        if (ec) return;

        // 3 秒接收超时：防止恶意/滞留客户端无限阻塞控制平面线程
        struct timeval tv{3, 0};
        ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     &tv, sizeof(tv));

        constexpr std::size_t MAX_CMD_LEN = 1024;
        char buf[MAX_CMD_LEN + 1]{};

        std::size_t n = sock.read_some(boost::asio::buffer(buf, MAX_CMD_LEN), ec);

        if (ec) {
            const std::string_view msg = ec == boost::asio::error::timed_out
                ? "ERROR: timeout waiting for command\n"
                : "ERROR: failed to receive command\n";
            boost::asio::write(sock, boost::asio::buffer(msg), ec);
            return;
        }
        if (n == 0) return;

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
