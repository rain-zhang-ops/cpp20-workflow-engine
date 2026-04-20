#pragma once

// 线程池模块 — 基于 boost::asio::thread_pool 的固定大小任务调度器
// 主要职责：接受泛型可调用对象，封装为 std::future<T> 并提交到 Boost.Asio 工作队列

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
 * 固定大小线程池，底层由 boost::asio::thread_pool 驱动。
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
 *  pool_ is declared SECOND → destroyed FIRST (pool_'s destructor joins the
 *  worker threads; stop_ must still be alive at that point because the
 *  destructor body sets stop_ = true before calling pool_.join(), and members
 *  are destroyed in reverse declaration order after the destructor body).
 *
 * 成员声明顺序决定析构顺序：stop_ 先声明，故最后析构；pool_ 后声明，故先析构（join 工作线程）。
 * 这保证析构体中设置 stop_ = true 后 pool_.join() 仍能安全访问 stop_。
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
     * 提交可调用对象，返回 std::future<T>；线程池已停止时抛出 std::runtime_error。
     * @throws std::runtime_error if the pool is already stopped.
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

private:
    // stop_ 先声明，outlives pool_；enqueue() 在 pool_ 销毁后仍可安全检查 stop_。
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

    // packaged_task 本身不可拷贝，用 shared_ptr 包装使 lambda 满足可拷贝要求。
    auto task = std::make_shared<std::packaged_task<Ret()>>(
        [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });

    std::future<Ret> fut = task->get_future();
    boost::asio::post(pool_, [task]() { (*task)(); });
    return fut;
}
