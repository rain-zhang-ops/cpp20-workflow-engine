// Tests for ThreadPool.

#include "ThreadPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Basic enqueue and future retrieval
// ---------------------------------------------------------------------------

TEST(ThreadPool, EnqueueReturnsCorrectValue)
{
    ThreadPool pool(2);
    auto fut = pool.enqueue([]() { return 42; });
    EXPECT_EQ(fut.get(), 42);
}

TEST(ThreadPool, EnqueueVoidTask)
{
    ThreadPool pool(2);
    std::atomic<bool> ran{false};
    auto fut = pool.enqueue([&ran]() { ran.store(true); });
    fut.get();
    EXPECT_TRUE(ran.load());
}

// ---------------------------------------------------------------------------
// Concurrent tasks
// ---------------------------------------------------------------------------

TEST(ThreadPool, ConcurrentTasksAllRun)
{
    ThreadPool pool(4);
    constexpr int kTasks = 100;
    std::atomic<int> counter{0};

    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.enqueue([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    for (auto& f : futures) f.get();
    EXPECT_EQ(counter.load(), kTasks);
}

TEST(ThreadPool, TasksRunInFiniteTime)
{
    ThreadPool pool(2);
    auto start = std::chrono::steady_clock::now();
    auto fut   = pool.enqueue([]() { return true; });
    EXPECT_TRUE(fut.get());
    auto elapsed = std::chrono::steady_clock::now() - start;
    // Should finish within 1 second.
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}

// ---------------------------------------------------------------------------
// Exception propagation through futures
// ---------------------------------------------------------------------------

TEST(ThreadPool, ExceptionPropagatesViaFuture)
{
    ThreadPool pool(2);
    auto fut = pool.enqueue([]() -> int {
        throw std::runtime_error("oops");
        return 0;
    });

    EXPECT_THROW(fut.get(), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Single-threaded pool works correctly
// ---------------------------------------------------------------------------

TEST(ThreadPool, SingleThreadPool)
{
    ThreadPool pool(1);
    std::vector<int> order;
    std::mutex mtx;
    constexpr int kTasks = 10;

    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);

    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(pool.enqueue([i, &order, &mtx]() {
            std::lock_guard lock(mtx);
            order.push_back(i);
        }));
    }

    for (auto& f : futures) f.get();

    ASSERT_EQ(static_cast<int>(order.size()), kTasks);
    // With a single thread tasks must run in FIFO order.
    for (int i = 0; i < kTasks; ++i)
        EXPECT_EQ(order[i], i);
}
