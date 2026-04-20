#pragma once

// 文件监控模块 — 使用 Linux inotify 监视单个配置文件的写入完成事件
// 主要职责：检测 IN_CLOSE_WRITE 并触发回调；通过 eventfd + poll() 实现零 CPU 空闲消耗和 RAII 安全析构

#include <functional>
#include <string>
#include <thread>   // std::jthread

/**
 * ConfigWatcher monitors a single file for changes using Linux inotify.
 *
 * Design:
 *  - Watches only IN_CLOSE_WRITE events (file fully written and closed).
 *  - A std::jthread blocks inside poll() on {inotify_fd, event_fd}; CPU is
 *    0% when idle — no busy-poll or sleep loops.
 *  - Graceful shutdown: std::stop_callback writes to the eventfd, which
 *    unblocks poll() so the thread exits cleanly.
 *  - Strictly RAII: all file descriptors are closed in the destructor body
 *    after the background thread has been joined.
 *
 * 设计要点：
 *  - 仅监听 IN_CLOSE_WRITE，避免编辑器临时写入触发多余回调。
 *  - poll() 阻塞等待，无忙轮询；stop_callback 写 eventfd 使 poll() 立即返回。
 *  - 严格 RAII：jthread 析构后才关闭 fd，防止后台线程访问已关闭的 fd。
 */
class ConfigWatcher {
public:
    /**
     * @param path      Absolute or relative path to the file to watch.
     * @param callback  Invoked on the watcher thread when the file is saved.
     *                  回调在后台 jthread 中调用，需注意线程安全。
     * @throws std::runtime_error if inotify or eventfd cannot be created.
     */
    ConfigWatcher(const std::string& path, std::function<void()> callback);
    ~ConfigWatcher();

    ConfigWatcher(const ConfigWatcher&)            = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

private:
    void watchLoop(std::stop_token st);

    std::string           path_;
    std::function<void()> callback_;
    int                   inotify_fd_{-1};
    int                   watch_fd_{-1};
    int                   event_fd_{-1};
    std::jthread          thread_;   // 最后声明，最先析构，确保后台线程先于 fd 关闭
};
