#include "ThreadPool.h"

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ThreadPool::ThreadPool(std::size_t thread_count)
{
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this](std::stop_token st) {
            workerLoop(st);
        });
    }
}

// ---------------------------------------------------------------------------
// Destructor — drain remaining tasks, then shut down all workers
// ---------------------------------------------------------------------------

ThreadPool::~ThreadPool()
{
    stop_.store(true, std::memory_order_release);

    // Push one nullptr poison-pill per worker *after* all real tasks.
    // Workers process real tasks in FIFO order and exit when they dequeue
    // a nullptr (poison pill).
    {
        std::lock_guard lock(tasks_mutex_);
        for (std::size_t i = 0; i < workers_.size(); ++i)
            tasks_.emplace(nullptr);
    }

    // Wake every worker that is blocking on the semaphore.
    for (std::size_t i = 0; i < workers_.size(); ++i)
        semaphore_.release();

    // jthread destructors call request_stop() + join() automatically.
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

void ThreadPool::workerLoop(std::stop_token /*st*/)
{
    while (true) {
        // Block here until a task (or poison-pill) is available — 0% CPU.
        semaphore_.acquire();

        std::function<void()> task;
        {
            std::lock_guard lock(tasks_mutex_);
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
            }
        }

        if (!task) break;   // nullptr poison-pill → exit

        task();
    }
}
