#pragma once

// ============================================================================
// ControlPlane — UDS-based daemon control interface
//
// 控制平面 —— 基于 Unix Domain Socket 的守护进程控制接口
// ============================================================================
//
// DESIGN / 设计思路
// -----------------
// The control plane runs as a background std::jthread.  It opens a Unix Domain
// Socket and uses poll() with a 100 ms timeout so the thread is sleeping
// almost all the time and CPU usage stays at zero between commands.
//
// 控制平面以后台 std::jthread 运行，打开一个 Unix Domain Socket，并以 100 ms
// 超时调用 poll()，使线程几乎始终处于休眠状态，无命令时 CPU 占用为 0。
//
// RAII SAFETY / RAII 安全
// ------------------------
// The listening socket fd is wrapped in FdGuard (closes on destruction).
// The socket file is unlinked both in the destructor and via std::atexit so
// that the file is removed even on abnormal termination.
//
// 监听 socket fd 由 FdGuard 包装（析构时 close）。socket 文件在析构函数和
// std::atexit 中都会被 unlink，确保异常退出时也能清理文件。
// ============================================================================

#include <functional>
#include <stop_token>
#include <string>
#include <unordered_map>

class ControlPlane {
public:
    /**
     * @param socket_path  Path for the UDS file.
     *                     Existing file is unlinked before binding.
     *                     默认路径 /tmp/workflow.sock；已存在时先 unlink。
     */
    explicit ControlPlane(std::string socket_path = "/tmp/workflow.sock");
    ~ControlPlane();

    // Non-copyable, non-movable — owns the listening fd and atexit registration.
    // 不可拷贝、不可移动 —— 独占监听 fd 和 atexit 注册。
    ControlPlane(const ControlPlane&)            = delete;
    ControlPlane& operator=(const ControlPlane&) = delete;
    ControlPlane(ControlPlane&&)                 = delete;
    ControlPlane& operator=(ControlPlane&&)      = delete;

    // -----------------------------------------------------------------------
    // register_command — install a handler for the given command keyword
    //
    // register_command — 为指定命令关键字安装处理函数
    //
    // The handler is called from the control-plane thread each time the
    // corresponding command is received.  It returns a response string that
    // is sent back to the CLI client.
    //
    // 每次收到对应命令时，在控制平面线程中调用 handler，返回的字符串回传给客户端。
    // -----------------------------------------------------------------------
    void register_command(const std::string& cmd,
                          std::function<std::string()> handler);

    // -----------------------------------------------------------------------
    // start_control_plane — main event loop; run inside a std::jthread
    //
    // start_control_plane — 主事件循环，在 std::jthread 中运行
    //
    // Blocks (sleeping in poll) until st.stop_requested() is true.
    // After returning, all resources are released by the destructor.
    //
    // 阻塞（在 poll 中休眠），直到 st.stop_requested() 为 true。
    // 返回后，析构函数负责释放所有资源。
    // -----------------------------------------------------------------------
    void start_control_plane(std::stop_token st);

private:
    // -----------------------------------------------------------------------
    // FdGuard — RAII wrapper around a raw file descriptor
    // FdGuard — 原始文件描述符的 RAII 包装
    // -----------------------------------------------------------------------
    struct FdGuard {
        int fd = -1;

        FdGuard() = default;
        explicit FdGuard(int f) noexcept : fd(f) {}
        ~FdGuard() noexcept { reset(); }

        FdGuard(const FdGuard&)            = delete;
        FdGuard& operator=(const FdGuard&) = delete;

        FdGuard(FdGuard&& o) noexcept : fd(o.fd) { o.fd = -1; }
        FdGuard& operator=(FdGuard&& o) noexcept {
            if (this != &o) { reset(); fd = o.fd; o.fd = -1; }
            return *this;
        }

        void reset() noexcept;

        [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
    };

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------
    void        setup_socket();
    void        handle_client(int client_fd) noexcept;
    std::string dispatch(const std::string& cmd);

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------
    std::string socket_path_;
    FdGuard     server_fd_;   ///< Listening socket — RAII closed on destruction

    std::unordered_map<std::string, std::function<std::string()>> handlers_;

    // -----------------------------------------------------------------------
    // atexit / crash cleanup
    // The static pointer lets the atexit handler reach the live instance.
    // 静态指针让 atexit 处理函数能访问到存活的实例。
    // -----------------------------------------------------------------------
    static ControlPlane* instance_;
    static void atexit_handler() noexcept;
};
