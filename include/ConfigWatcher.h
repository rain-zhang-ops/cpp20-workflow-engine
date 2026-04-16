#pragma once

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
 */
class ConfigWatcher {
public:
    /**
     * @param path      Absolute or relative path to the file to watch.
     * @param callback  Invoked on the watcher thread when the file is saved.
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
    std::jthread          thread_;   // declared last → destroyed first
};
