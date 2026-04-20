#pragma once

// ============================================================================
// ControlPlane — UDS-based daemon control interface (Boost.Asio)
//
// 控制平面 —— 基于 Unix Domain Socket 的守护进程控制接口（Boost.Asio）
// ============================================================================
//
// DESIGN / 设计思路
// -----------------
// The control plane runs as a background std::jthread.  It uses a
// boost::asio::io_context with an async accept loop on a Unix Domain Socket.
// When the std::stop_token fires, a stop_callback calls io_context::stop(),
// which cancels the pending async_accept and causes io_context::run() to
// return cleanly — no busy-polling or timed loops.
//
// 控制平面以后台 std::jthread 运行，在 Unix Domain Socket 上使用
// boost::asio::io_context 进行异步接受循环。当 std::stop_token 触发时，
// stop_callback 调用 io_context::stop()，取消挂起的 async_accept，使
// io_context::run() 干净返回，无需忙轮询或定时循环。
//
// RAII SAFETY / RAII 安全
// ------------------------
// The acceptor and socket are managed by Boost.Asio RAII types, which close
// the underlying file descriptors on destruction.  The socket file on disk is
// unlinked both in the destructor and via std::atexit.
//
// 接受器和套接字由 Boost.Asio RAII 类型管理，析构时关闭底层文件描述符。
// 磁盘上的 socket 文件在析构函数和 std::atexit 中都会被 unlink。
// ============================================================================

#include <functional>
#include <stop_token>
#include <string>
#include <unordered_map>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

class ControlPlane {
public:
    /**
     * @param socket_path  Path for the UDS file.
     *                     Existing file is unlinked before binding.
     *                     默认路径 /tmp/workflow.sock；已存在时先 unlink。
     */
    explicit ControlPlane(std::string socket_path = "/tmp/workflow.sock");
    ~ControlPlane();

    // Non-copyable, non-movable — owns the io_context and atexit registration.
    // 不可拷贝、不可移动 —— 独占 io_context 和 atexit 注册。
    ControlPlane(const ControlPlane&)            = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;
    ControlPlane(ControlPlane&&)                 = delete;
    ControlPlane& operator=(ControlPlane&&)      = delete;

    // -----------------------------------------------------------------------
    // register_command — install a handler for the given command keyword
    // register_command — 为指定命令关键字安装处理函数
    // -----------------------------------------------------------------------
    void register_command(const std::string& cmd,
                          std::function<std::string()> handler);

    // -----------------------------------------------------------------------
    // start_control_plane — async event loop; run inside a std::jthread
    //
    // start_control_plane — 异步事件循环，在 std::jthread 中运行
    //
    // Runs the io_context until st.stop_requested() fires (via stop_callback).
    // After returning, all resources are released by the destructor.
    //
    // 运行 io_context 直到 st.stop_requested() 触发（通过 stop_callback）。
    // 返回后，析构函数负责释放所有资源。
    // -----------------------------------------------------------------------
    void start_control_plane(std::stop_token st);

private:
    using Protocol = boost::asio::local::stream_protocol;

    void        do_accept();
    void        handle_session(Protocol::socket sock) noexcept;
    std::string dispatch(const std::string& cmd);

    std::string              socket_path_;
    boost::asio::io_context  ioc_;
    Protocol::acceptor       acceptor_;   ///< Async acceptor — RAII closed on destruction

    std::unordered_map<std::string, std::function<std::string()>> handlers_;

    // -----------------------------------------------------------------------
    // atexit / crash cleanup
    // The static pointer lets the atexit handler reach the live instance.
    // 静态指针让 atexit 处理函数能访问到存活的实例。
    // -----------------------------------------------------------------------
    static ControlPlane* instance_;
    static void atexit_handler() noexcept;
};
