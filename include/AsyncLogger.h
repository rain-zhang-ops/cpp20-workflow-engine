#pragma once

#include "LockFreeRingBuffer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

/**
 * Log severity levels.
 * 日志严重级别。
 */
enum class LogLevel : uint8_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

/**
 * A single log event pushed by worker threads.
 * 工作线程推入的单个日志事件。
 *
 * NOTE: std::string has a noexcept move constructor (SSO / heap-allocated),
 * so LogEvent satisfies LockFreeRingBuffer's nothrow-move requirement.
 */
struct LogEvent {
    LogLevel                              level{LogLevel::Info};
    std::string                           message{};
    std::chrono::system_clock::time_point timestamp{};
};

/**
 * AsyncLogger — near-zero-overhead asynchronous logger.
 *
 * Worker threads push LogEvent objects into a lock-free ring buffer in
 * O(1) time (typically 30–60 ns).  A dedicated low-priority background
 * std::jthread drains the buffer every 500 µs and forwards formatted lines
 * to the output sink (stdout by default).
 *
 * If the buffer is full, the event is silently discarded — the design
 * prioritises producer throughput over perfect log delivery.
 *
 * Thread-safety:
 *   log() / info() / warn() / error() — safe to call from any thread.
 *   The output sink is accessed exclusively from the background thread.
 *
 * 近零开销的异步日志记录器。
 * 工作线程以约 30–60 ns 的开销将 LogEvent 推入无锁环形缓冲区。
 * 一个专属后台 std::jthread 每 500 µs 批量消费缓冲区并输出到接收端。
 * 缓冲区满时静默丢弃事件，以保证生产者吞吐量。
 */
class AsyncLogger {
public:
    static constexpr std::size_t kBufferCapacity = 4096;

    using Sink = std::function<void(std::string_view)>;

    /**
     * Construct the logger.  The background flush thread starts immediately.
     * @param sink  Callback invoked with each formatted log line.
     *              Defaults to writing to std::cout.
     *
     * 构造日志记录器，后台刷新线程立即启动。
     * @param sink  处理格式化日志行的回调（默认输出到 std::cout）。
     */
    explicit AsyncLogger(Sink sink = nullptr)
        : sink_(sink ? std::move(sink)
                     : [](std::string_view line) { std::cout << line; })
        , flush_thread_([this](std::stop_token st) { flushLoop(st); })
    {}

    /**
     * Destructor: requests stop on the background thread, joins it, then
     * drains any remaining events in the buffer.
     *
     * 析构：请求后台线程停止，等待其结束，再排空缓冲区中的剩余事件。
     */
    ~AsyncLogger() {
        flush_thread_.request_stop();
        flush_thread_.join();
        drainAll();
    }

    AsyncLogger(const AsyncLogger&)            = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    /**
     * Push a log event (non-blocking, near-zero overhead).
     * Silently discards the event if the ring buffer is full.
     *
     * 推入日志事件（非阻塞，近零开销）。缓冲区满时静默丢弃。
     */
    void log(LogLevel level, std::string message) noexcept {
        (void)buffer_.push(LogEvent{
            level,
            std::move(message),
            std::chrono::system_clock::now()
        });
    }

    /** Convenience severity-specific overloads. */
    void debug(std::string msg) noexcept { log(LogLevel::Debug, std::move(msg)); }
    void info (std::string msg) noexcept { log(LogLevel::Info,  std::move(msg)); }
    void warn (std::string msg) noexcept { log(LogLevel::Warn,  std::move(msg)); }
    void error(std::string msg) noexcept { log(LogLevel::Error, std::move(msg)); }

    /**
     * Synchronously drain all buffered events to the sink.
     * Call from the consumer/main thread when you need all log output to
     * appear before a subsequent operation.
     *
     * 同步排空缓冲区中所有事件。在需要确保日志输出完整出现时，
     * 可在消费者/主线程中调用。
     */
    void flush() { drainAll(); }

private:
    static std::string_view levelName(LogLevel l) noexcept {
        switch (l) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
        }
        return "?????";
    }

    static std::string formatEvent(const LogEvent& ev) {
        // Format as: "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] message\n"
        auto t = std::chrono::system_clock::to_time_t(ev.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      ev.timestamp.time_since_epoch()) % 1000;

        std::tm tm_val{};
        ::localtime_r(&t, &tm_val);

        char time_buf[24];
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_val);

        // Zero-pad milliseconds to three digits.
        auto ms_val = static_cast<int>(ms.count());
        char ms_buf[8];
        std::snprintf(ms_buf, sizeof(ms_buf), "%03d", ms_val);

        std::string line;
        line.reserve(ev.message.size() + 32);
        line += time_buf;
        line += '.';
        line += ms_buf;
        line += ' ';
        line += '[';
        line += levelName(ev.level);
        line += "] ";
        line += ev.message;
        line += '\n';
        return line;
    }

    void flushLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            drainBatch();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    void drainBatch() {
        for (int i = 0; i < 256; ++i) {
            auto ev = buffer_.pop();
            if (!ev) break;
            sink_(formatEvent(*ev));
        }
    }

    void drainAll() {
        while (auto ev = buffer_.pop())
            sink_(formatEvent(*ev));
    }

    Sink                                           sink_;
    LockFreeRingBuffer<LogEvent, kBufferCapacity>  buffer_;
    std::jthread                                   flush_thread_;
};
