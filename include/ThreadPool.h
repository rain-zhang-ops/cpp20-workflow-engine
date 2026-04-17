#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * Fixed-size thread pool backed by std::jthread workers.
 *
 * Design:
 *  - Workers block on a std::counting_semaphore; they are never busy-polling.
 *  - The semaphore is released once per enqueued task, waking exactly one
 *    worker — minimal context-switch overhead.
 *  - A nullptr "poison pill" is pushed per worker on shutdown so every worker
 *    exits after draining any remaining real tasks.
 *  - Task queue is protected by a std::mutex; the critical section only
 *    contains a push/pop, so lock contention is negligible.
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
    void workerLoop(std::stop_token st);

    // NOTE: members are destroyed in reverse declaration order.
    // stop_, tasks_, tasks_mutex_, semaphore_ must outlive all workers,
    // so workers_ is declared LAST → it is destroyed FIRST (joined before
    // the queue, mutex and semaphore are torn down).
    std::atomic<bool>                stop_{false};
    std::queue<std::function<void()>> tasks_;
    std::mutex                       tasks_mutex_;
    std::counting_semaphore<1048576> semaphore_{0};
    std::vector<std::jthread>        workers_;
};

// ---- template implementation -----------------------------------------------

template <class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>
{
    using Ret = std::invoke_result_t<F, Args...>;

    if (stop_.load(std::memory_order_acquire))
        throw std::runtime_error("ThreadPool has been stopped");

    // Wrap in a shared packaged_task so the lambda stored in std::function
    // is copyable (packaged_task itself is move-only).
    auto task = std::make_shared<std::packaged_task<Ret()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });

    std::future<Ret> fut = task->get_future();
    {
        std::lock_guard lock(tasks_mutex_);
        tasks_.emplace([task]() { (*task)(); });
    }
    semaphore_.release();
    return fut;
}
