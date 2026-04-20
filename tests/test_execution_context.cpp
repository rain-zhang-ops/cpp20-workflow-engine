// Tests for ExecutionContext: set/get/has/remove/clear and ABI-safe C helpers.

#include "ExecutionContext.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Basic read/write
// ---------------------------------------------------------------------------

TEST(ExecutionContext, SetAndGetString)
{
    ExecutionContext ctx;
    ctx.set("key", std::string("hello"));
    auto val = ctx.get<std::string>("key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello");
}

TEST(ExecutionContext, SetAndGetInt64)
{
    ExecutionContext ctx;
    ctx.set("count", int64_t{42});
    auto val = ctx.get<int64_t>("count");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
}

TEST(ExecutionContext, SetAndGetDouble)
{
    ExecutionContext ctx;
    ctx.set("score", 3.14);
    auto val = ctx.get<double>("score");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 3.14);
}

TEST(ExecutionContext, GetMissingKeyReturnsNullopt)
{
    ExecutionContext ctx;
    EXPECT_FALSE(ctx.get<std::string>("missing").has_value());
}

TEST(ExecutionContext, GetWrongTypeReturnsNullopt)
{
    ExecutionContext ctx;
    ctx.set("n", int64_t{1});
    EXPECT_FALSE(ctx.get<std::string>("n").has_value());
}

TEST(ExecutionContext, HasAndRemove)
{
    ExecutionContext ctx;
    EXPECT_FALSE(ctx.has("x"));
    ctx.set("x", std::string("v"));
    EXPECT_TRUE(ctx.has("x"));
    ctx.remove("x");
    EXPECT_FALSE(ctx.has("x"));
}

TEST(ExecutionContext, RemoveMissingKeyIsNoOp)
{
    ExecutionContext ctx;
    EXPECT_NO_THROW(ctx.remove("ghost"));
}

TEST(ExecutionContext, Clear)
{
    ExecutionContext ctx;
    ctx.set("a", std::string("1"));
    ctx.set("b", std::string("2"));
    ctx.clear();
    EXPECT_FALSE(ctx.has("a"));
    EXPECT_FALSE(ctx.has("b"));
}

TEST(ExecutionContext, OverwriteValue)
{
    ExecutionContext ctx;
    ctx.set("k", std::string("first"));
    ctx.set("k", std::string("second"));
    auto val = ctx.get<std::string>("k");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "second");
}

// ---------------------------------------------------------------------------
// ABI-safe C interface
// ---------------------------------------------------------------------------

TEST(ExecutionContextC, SetGetString)
{
    ExecutionContext ctx;
    void* ptr = static_cast<void*>(&ctx);

    ctx_set_string(ptr, "s", "world");

    char buf[64]{};
    int n = ctx_get_string(ptr, "s", buf, sizeof(buf));
    EXPECT_GE(n, 0);
    EXPECT_STREQ(buf, "world");
}

TEST(ExecutionContextC, GetStringMissingKey)
{
    ExecutionContext ctx;
    void* ptr = static_cast<void*>(&ctx);
    char buf[32]{};
    EXPECT_EQ(ctx_get_string(ptr, "no_such_key", buf, sizeof(buf)), -1);
}

TEST(ExecutionContextC, SetGetInt64)
{
    ExecutionContext ctx;
    void* ptr = static_cast<void*>(&ctx);
    ctx_set_int64(ptr, "n", 99);
    int64_t out = 0;
    EXPECT_EQ(ctx_get_int64(ptr, "n", &out), 0);
    EXPECT_EQ(out, 99);
}

TEST(ExecutionContextC, SetGetDouble)
{
    ExecutionContext ctx;
    void* ptr = static_cast<void*>(&ctx);
    ctx_set_double(ptr, "d", 2.71828);
    double out = 0.0;
    EXPECT_EQ(ctx_get_double(ptr, "d", &out), 0);
    EXPECT_DOUBLE_EQ(out, 2.71828);
}

TEST(ExecutionContextC, HasAndRemove)
{
    ExecutionContext ctx;
    void* ptr = static_cast<void*>(&ctx);

    EXPECT_EQ(ctx_has(ptr, "x"), 0);
    ctx_set_string(ptr, "x", "v");
    EXPECT_EQ(ctx_has(ptr, "x"), 1);
    ctx_remove(ptr, "x");
    EXPECT_EQ(ctx_has(ptr, "x"), 0);
}

TEST(ExecutionContextC, NullContextIsHandledGracefully)
{
    EXPECT_NO_THROW(ctx_set_string(nullptr, "k", "v"));
    EXPECT_NO_THROW(ctx_set_int64(nullptr, "k", 0));
    EXPECT_NO_THROW(ctx_set_double(nullptr, "k", 0.0));

    char buf[8]{};
    EXPECT_EQ(ctx_get_string(nullptr, "k", buf, sizeof(buf)), -1);

    int64_t iv = 0;
    EXPECT_EQ(ctx_get_int64(nullptr, "k", &iv), -1);

    double dv = 0;
    EXPECT_EQ(ctx_get_double(nullptr, "k", &dv), -1);

    EXPECT_EQ(ctx_has(nullptr, "k"), 0);
    EXPECT_NO_THROW(ctx_remove(nullptr, "k"));
}

// ---------------------------------------------------------------------------
// Concurrency: simultaneous writers and readers must not corrupt state
// ---------------------------------------------------------------------------

TEST(ExecutionContext, ConcurrentWritesAndReads)
{
    ExecutionContext ctx;
    constexpr int kThreads = 8;
    constexpr int kIter    = 1000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ctx, t]() {
            for (int i = 0; i < kIter; ++i) {
                std::string key = "k" + std::to_string(t);
                ctx.set(key, int64_t{i});
                (void)ctx.get<int64_t>(key);
                (void)ctx.has(key);
            }
        });
    }

    for (auto& th : threads) th.join();
    // If we reach here without data race (TSan/ASan) the test passes.
    SUCCEED();
}
