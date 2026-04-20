#include "ThreadPool.h"

// ---------------------------------------------------------------------------
// Constructor — create a boost::asio::thread_pool with the requested number
// of worker threads (at least 1 if hardware_concurrency() returns 0).
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
// ---------------------------------------------------------------------------

ThreadPool::~ThreadPool()
{
    stop_.store(true, std::memory_order_release);
    pool_.join();
}
