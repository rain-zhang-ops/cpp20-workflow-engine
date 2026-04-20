// Tests for LockFreeRingBuffer.

#include "LockFreeRingBuffer.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Single-threaded push / pop
// ---------------------------------------------------------------------------

TEST(LockFreeRingBuffer, PushAndPop)
{
    LockFreeRingBuffer<int, 8> buf;
    EXPECT_TRUE(buf.push(1));
    EXPECT_TRUE(buf.push(2));
    EXPECT_TRUE(buf.push(3));

    auto v1 = buf.pop();
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);

    auto v2 = buf.pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 2);

    auto v3 = buf.pop();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 3);
}

TEST(LockFreeRingBuffer, PopEmptyReturnsNullopt)
{
    LockFreeRingBuffer<int, 4> buf;
    EXPECT_FALSE(buf.pop().has_value());
}

TEST(LockFreeRingBuffer, PushFullReturnsFalse)
{
    LockFreeRingBuffer<int, 4> buf;
    EXPECT_TRUE(buf.push(1));
    EXPECT_TRUE(buf.push(2));
    EXPECT_TRUE(buf.push(3));
    EXPECT_TRUE(buf.push(4));
    EXPECT_FALSE(buf.push(5));  // buffer full
}

TEST(LockFreeRingBuffer, EmptyCheck)
{
    LockFreeRingBuffer<int, 4> buf;
    EXPECT_TRUE(buf.empty());
    (void)buf.push(42);
    EXPECT_FALSE(buf.empty());
    (void)buf.pop();
    EXPECT_TRUE(buf.empty());
}

TEST(LockFreeRingBuffer, FIFOOrder)
{
    LockFreeRingBuffer<int, 16> buf;
    for (int i = 0; i < 10; ++i) (void)buf.push(i);
    for (int i = 0; i < 10; ++i) {
        auto v = buf.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
}

// ---------------------------------------------------------------------------
// Multi-producer / single-consumer
// ---------------------------------------------------------------------------

TEST(LockFreeRingBuffer, MultiProducerSingleConsumer)
{
    constexpr std::size_t kCapacity  = 1024;
    constexpr int         kProducers = 4;
    constexpr int         kPerProd   = 200;

    LockFreeRingBuffer<int, kCapacity> buf;
    std::atomic<int> pushed{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int t = 0; t < kProducers; ++t) {
        producers.emplace_back([&buf, &pushed]() {
            for (int i = 0; i < kPerProd; ++i) {
                while (!buf.push(1))
                    std::this_thread::yield();
                pushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    int consumed = 0;
    while (consumed < kProducers * kPerProd) {
        auto v = buf.pop();
        if (v) ++consumed;
        else std::this_thread::yield();
    }

    for (auto& p : producers) p.join();
    EXPECT_EQ(consumed, kProducers * kPerProd);
}
