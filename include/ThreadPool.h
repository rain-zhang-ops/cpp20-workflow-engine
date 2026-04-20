#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>

/**
 * Fixed-size thread pool backed by boost::asio::thread_pool.
 *
 * Design:
 *  - Workers are managed by boost::asio::thread_pool; no manual semaphores,
 *    mutexes, or poison-pill shutdown sequences are needed — Boost.Asio handles
 *    all of that internally.
 *  - Tasks are submitted via boost::asio::post(), which is lock-free on the
 *    hot path and has lower overhead than a mutex-protected std::queue.
 *  - Returning std::future<T> is still supported through std::packaged_task
 *    wrapped in a shared_ptr (same copyable-lambda trick as before).
 *  - Shutdown: pool_.join() drains remaining handlers before destroying the
 *    worker threads, preserving the original graceful-shutdown semantics.
 *
 * NOTE: member declaration order matters for destruction.
 *  stop_ is declared FIRST → destroyed LAST.
 *  pool_ is declared SECOND → destroyed FIRST (joins workers before stop_
 *  is torn down, keeping the check in enqueue() safe).
 */
class ThreadPool {
public:
    explicit ThreadPool(
        std::size_t thread_count = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * Enqueue a callable and return a std::future for its result.
     * @throws std::runtime_error if the pool is already stopped.
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

private:
    // stop_ first so it outlives pool_ (workers never read stop_ directly,
    // but enqueue() must be able to check it after pool_ is destroyed).
    std::atomic<bool>        stop_{false};
    boost::asio::thread_pool pool_;
};

// ---- template implementation -----------------------------------------------

template <class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using Ret = std::invoke_result_t<F, Args...>;

    if (stop_.load(std::memory_order_acquire))
        throw std::runtime_error("ThreadPool has been stopped");

    // Wrap in a shared packaged_task so the lambda stored via post() is
    // copyable (packaged_task itself is move-only).
    auto task = std::make_shared<std::packaged_task<Ret()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });

    std::future<Ret> fut = task->get_future();
    boost::asio::post(pool_, [task]() { (*task)(); });
    return fut;
}
