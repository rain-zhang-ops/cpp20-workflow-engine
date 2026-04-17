#pragma once

#include <chrono>
#include <functional>

/**
 * RAII scoped timer.
 *
 * Measures the elapsed wall-clock time between construction and destruction
 * using std::chrono::high_resolution_clock, then invokes a user-supplied
 * callback with the duration expressed in nanoseconds.
 *
 * Typical usage:
 * @code
 *   {
 *       ScopedTimer t([](ScopedTimer::Duration ns) {
 *           metrics.record("node_exec", ns);
 *       });
 *       // ... timed work ...
 *   }  // <-- callback fires here with elapsed nanoseconds
 * @endcode
 *
 * The callback is optional; if an empty std::function is provided (or the
 * default-constructed overload is used) no action is taken on destruction.
 *
 * Thread-safety: each ScopedTimer instance must be used from a single thread.
 *
 * RAII 风格计时器，使用 std::chrono::high_resolution_clock 测量微秒级耗时。
 * 析构时将纳秒级耗时传递给回调函数。
 */
class ScopedTimer {
public:
    using Clock    = std::chrono::high_resolution_clock;
    using Duration = std::chrono::nanoseconds;
    using Callback = std::function<void(Duration)>;

    /** Construct with a callback invoked on destruction. */
    explicit ScopedTimer(Callback cb)
        : cb_(std::move(cb)), start_(Clock::now()) {}

    ~ScopedTimer() {
        if (cb_) {
            cb_(std::chrono::duration_cast<Duration>(Clock::now() - start_));
        }
    }

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
    ScopedTimer(ScopedTimer&&)                 = delete;
    ScopedTimer& operator=(ScopedTimer&&)      = delete;

    /** Read the elapsed time so far without stopping the timer. */
    [[nodiscard]] Duration elapsed() const noexcept {
        return std::chrono::duration_cast<Duration>(Clock::now() - start_);
    }

private:
    Callback          cb_;
    Clock::time_point start_;
};
