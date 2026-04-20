#include "ThreadPool.h"

// ThreadPool 实现 — 构造/析构逻辑
// 析构时先设置 stop_ 标志，再调用 pool_.join() 优雅排空所有待执行任务，确保无任务丢失。

// ---------------------------------------------------------------------------
// Constructor — create a boost::asio::thread_pool with the requested number
// of worker threads (at least 1 if hardware_concurrency() returns 0).
// 构造：至少创建 1 个工作线程，防止 hardware_concurrency() 返回 0 导致空线程池。
// ---------------------------------------------------------------------------

ThreadPool::ThreadPool(std::size_t thread_count)
    : pool_(thread_count > 0 ? thread_count : 1)
{}

// ---------------------------------------------------------------------------
// Destructor — signal no more tasks, then join all workers gracefully.
//
// pool_.join() stops the io_context (preventing new tasks from being started)
// and blocks until all currently-running handlers have completed.  Because
// WorkflowEngine always calls waitForCompletion() before the ThreadPool is
// destroyed, the pool is idle at this point and join() returns immediately.
//
// stop_ 在 pool_.join() 之前置 true，防止 enqueue() 在 join 期间再次提交任务。
// ---------------------------------------------------------------------------

ThreadPool::~ThreadPool()
{
    stop_.store(true, std::memory_order_release);
    pool_.join();
}
