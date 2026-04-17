#include "ConfigWatcher.h"

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
    inotify_fd_ = inotify_init1(IN_NONBLOCK);
    if (inotify_fd_ < 0)
        throw std::runtime_error(
            std::string("inotify_init1 failed: ") + std::strerror(errno));

    auto dir = std::filesystem::path(path_).parent_path();
    if (dir.empty()) dir = ".";

    watch_fd_ = inotify_add_watch(inotify_fd_, dir.c_str(), IN_CLOSE_WRITE);
    if (watch_fd_ < 0) {
        close(inotify_fd_);
        throw std::runtime_error(
            std::string("inotify_add_watch failed: ") + std::strerror(errno));
    }

    // --- eventfd for cancellation ----------------------------------------
    event_fd_ = eventfd(0, EFD_NONBLOCK);
    if (event_fd_ < 0) {
        inotify_rm_watch(inotify_fd_, watch_fd_);
        close(inotify_fd_);
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno));
    }

    // --- start background thread -----------------------------------------
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

        if (fds[1].revents & POLLIN) break;

        if (fds[0].revents & POLLIN) {
            const ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len < 0) continue;

            for (const char* ptr = buf; ptr < buf + len; ) {
                const auto* ev =
                    reinterpret_cast<const inotify_event*>(ptr);

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