#include "ConfigWatcher.h"

// ConfigWatcher 实现 — inotify + eventfd 双 fd 监控，poll() 零空闲 CPU
// stop_callback 写 eventfd 使 poll() 立即返回，实现无延迟优雅关闭

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ConfigWatcher::ConfigWatcher(const std::string& path,
                             std::function<void()> callback)
    : path_(path), callback_(std::move(callback))
{
    // --- inotify setup ---------------------------------------------------
    // IN_NONBLOCK：非阻塞模式，由 poll() 控制读取时机
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0)
        throw std::runtime_error(
            std::string("inotify_init1 failed: ") + std::strerror(errno));

    // 监控父目录而非文件本身：编辑器保存时常先 unlink 再创建新文件，
    // 直接监控文件 fd 会因 inode 替换而丢失事件。
    auto dir = std::filesystem::path(path_).parent_path();
    if (dir.empty()) dir = ".";

    watch_fd_ = inotify_add_watch(inotify_fd_, dir.c_str(), IN_CLOSE_WRITE);
    if (watch_fd_ < 0) {
        close(inotify_fd_);
        throw std::runtime_error(
            std::string("inotify_add_watch failed: ") + std::strerror(errno));
    }

    // --- eventfd for cancellation ----------------------------------------
    // EFD_NONBLOCK：stop_callback 写入后 poll() 立即检测到 POLLIN，无需等待下次超时
    event_fd_ = eventfd(0, EFD_NONBLOCK);
    if (event_fd_ < 0) {
        inotify_rm_watch(inotify_fd_, watch_fd_);
        close(inotify_fd_);
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    // stop_callback：stop_token 触发时写 eventfd，解除 poll() 阻塞
    thread_ = std::jthread([this](std::stop_token st) {
        std::stop_callback sc{st, [this]() noexcept {
            const uint64_t val = 1;
            if (write(event_fd_, &val, sizeof(val)) < 0) {
                std::cerr << "[ConfigWatcher] eventfd write error: "
                          << std::strerror(errno) << "\n";
            }
        }};
        watchLoop(st);
    });
}

// ---------------------------------------------------------------------------
// Destructor
// 先 request_stop + join，确保后台线程退出后再关闭 fd，避免 EBADF 竞态
// ---------------------------------------------------------------------------

ConfigWatcher::~ConfigWatcher()
{
    thread_.request_stop();
    thread_.join();

    if (watch_fd_  >= 0) inotify_rm_watch(inotify_fd_, watch_fd_);
    if (inotify_fd_ >= 0) close(inotify_fd_);
    if (event_fd_  >= 0) close(event_fd_);
}

// ---------------------------------------------------------------------------
// Watch loop (runs on background jthread)
// poll() 永久阻塞（timeout=-1），仅在 inotify 或 eventfd 有事件时唤醒，CPU 消耗为零
// ---------------------------------------------------------------------------

void ConfigWatcher::watchLoop(std::stop_token st)
{
    const auto filename =
        std::filesystem::path(path_).filename().string();

    alignas(inotify_event) char buf[4096];

    std::array<pollfd, 2> fds{};
    fds[0].fd     = inotify_fd_;
    fds[0].events = POLLIN;
    fds[1].fd     = event_fd_;
    fds[1].events = POLLIN;

    while (!st.stop_requested()) {
        const int ret = poll(fds.data(), fds.size(), -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) break;  // eventfd 触发：stop 请求，退出循环

        if (fds[0].revents & POLLIN) {
            const ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len < 0) continue;

            for (const char* ptr = buf; ptr < buf + len; ) {
                const auto* ev =
                    reinterpret_cast<const inotify_event*>(ptr);

                // 只响应目标文件的 IN_CLOSE_WRITE，忽略目录中其他文件的事件
                if ((ev->mask & IN_CLOSE_WRITE) &&
                    ev->len > 0 &&
                    filename == ev->name)
                {
                    callback_();
                }

                ptr += sizeof(inotify_event) + ev->len;
            }
        }
    }
}