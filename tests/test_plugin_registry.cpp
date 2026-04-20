// Tests for PluginRegistry.

#include "PluginRegistry.h"
#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal concrete node used by tests
// ---------------------------------------------------------------------------

struct DummyNode final : WorkflowNode {
    void execute(ExecutionContext& /*ctx*/) override {}
};

// ---------------------------------------------------------------------------
// Basic registration and lookup
// ---------------------------------------------------------------------------

TEST(PluginRegistry, RegisterAndCreate)
{
    PluginRegistry reg;
    reg.register_node("Dummy", []() {
        return std::make_unique<DummyNode>();
    });

    EXPECT_TRUE(reg.has("Dummy"));
    auto node = reg.create("Dummy");
    EXPECT_NE(node.get(), nullptr);
}

TEST(PluginRegistry, HasReturnsFalseForUnknownType)
{
    PluginRegistry reg;
    EXPECT_FALSE(reg.has("NoSuchType"));
}

TEST(PluginRegistry, CreateUnknownTypeThrows)
{
    PluginRegistry reg;
    EXPECT_THROW(reg.create("NoSuchType"), std::runtime_error);
}

TEST(PluginRegistry, ReregistrationOverwritesPrevious)
{
    PluginRegistry reg;

    int first_count  = 0;
    int second_count = 0;

    reg.register_node("X", [&first_count]() {
        ++first_count;
        return std::make_unique<DummyNode>();
    });
    (void)reg.create("X");
    EXPECT_EQ(first_count,  1);
    EXPECT_EQ(second_count, 0);

    // Overwrite with a different factory.
    reg.register_node("X", [&second_count]() {
        ++second_count;
        return std::make_unique<DummyNode>();
    });
    (void)reg.create("X");
    EXPECT_EQ(first_count,  1);
    EXPECT_EQ(second_count, 1);
}

TEST(PluginRegistry, FactoryCalledFreshEachTime)
{
    PluginRegistry reg;
    int call_count = 0;
    reg.register_node("Counter", [&call_count]() {
        ++call_count;
        return std::make_unique<DummyNode>();
    });

    for (int i = 1; i <= 5; ++i) {
        (void)reg.create("Counter");
        EXPECT_EQ(call_count, i);
    }
}

// ---------------------------------------------------------------------------
// Thread-safety: concurrent registration and creation
// ---------------------------------------------------------------------------

TEST(PluginRegistry, ConcurrentRegisterAndCreate)
{
    PluginRegistry reg;
    reg.register_node("Base", []() { return std::make_unique<DummyNode>(); });

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        if (t % 2 == 0) {
            // writer: re-register (overwrite)
            threads.emplace_back([&reg]() {
                for (int i = 0; i < 100; ++i)
                    reg.register_node("Base",
                        []() { return std::make_unique<DummyNode>(); });
            });
        } else {
            // reader: create nodes
            threads.emplace_back([&reg]() {
                for (int i = 0; i < 100; ++i)
                    EXPECT_NE(reg.create("Base").get(), nullptr);
            });
        }
    }

    for (auto& th : threads) th.join();
    SUCCEED();
}
