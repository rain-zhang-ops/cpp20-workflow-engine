// Tests for WorkflowEngine.

#include "WorkflowEngine.h"
#include "PluginRegistry.h"
#include "ThreadPool.h"
#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Minimal concrete nodes used by tests
// ---------------------------------------------------------------------------

struct SuccessNode final : WorkflowNode {
    void execute(ExecutionContext& /*ctx*/) override {}
};

struct CountingNode final : WorkflowNode {
    explicit CountingNode(std::atomic<int>& counter) : counter_(counter) {}
    void execute(ExecutionContext& /*ctx*/) override {
        counter_.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<int>& counter_;
};

struct ThrowingNode final : WorkflowNode {
    void execute(ExecutionContext& /*ctx*/) override {
        throw std::runtime_error("deliberate failure");
    }
};

// ---------------------------------------------------------------------------
// Helper: write a minimal workflow JSON to a temp file and return its path.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <fstream>
#include <string>

static std::string write_temp_json(const std::string& content)
{
    char tmpl[] = "/tmp/wf_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
        ::write(fd, content.c_str(), content.size());
        ::close(fd);
    }
    return std::string(tmpl);
}

// ---------------------------------------------------------------------------
// loadConfig — valid DAG
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, LoadValidConfig)
{
    ThreadPool    pool(2);
    PluginRegistry reg;
    reg.register_node("S", []() { return std::make_unique<SuccessNode>(); });

    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "S", "dependencies": []},
            {"id": "B", "type": "S", "dependencies": ["A"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    EXPECT_NO_THROW(engine.loadConfig(path));
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// loadConfig — duplicate node ID
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, LoadConfigDuplicateIdThrows)
{
    ThreadPool     pool(2);
    PluginRegistry reg;
    reg.register_node("S", []() { return std::make_unique<SuccessNode>(); });

    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "S", "dependencies": []},
            {"id": "A", "type": "S", "dependencies": []}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    EXPECT_THROW(engine.loadConfig(path), std::runtime_error);
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// loadConfig — unknown dependency reference
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, LoadConfigUnknownDepThrows)
{
    ThreadPool     pool(2);
    PluginRegistry reg;
    reg.register_node("S", []() { return std::make_unique<SuccessNode>(); });

    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "S", "dependencies": ["GHOST"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    EXPECT_THROW(engine.loadConfig(path), std::runtime_error);
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// loadConfig — cycle detection
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, LoadConfigCycleThrows)
{
    ThreadPool     pool(2);
    PluginRegistry reg;
    reg.register_node("S", []() { return std::make_unique<SuccessNode>(); });

    // A → B → A  (cycle)
    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "S", "dependencies": ["B"]},
            {"id": "B", "type": "S", "dependencies": ["A"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    EXPECT_THROW(engine.loadConfig(path), std::runtime_error);
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// run — all nodes execute
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, AllNodesExecute)
{
    ThreadPool     pool(4);
    PluginRegistry reg;
    std::atomic<int> counter{0};

    reg.register_node("C", [&counter]() {
        return std::make_unique<CountingNode>(counter);
    });

    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "C", "dependencies": []},
            {"id": "B", "type": "C", "dependencies": ["A"]},
            {"id": "C", "type": "C", "dependencies": ["A"]},
            {"id": "D", "type": "C", "dependencies": ["B", "C"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    engine.loadConfig(path);
    engine.run();
    engine.waitForCompletion();
    ::unlink(path.c_str());

    EXPECT_EQ(counter.load(), 4);
}

// ---------------------------------------------------------------------------
// run — failed node cancels downstream
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, FailedNodeCancelsDownstream)
{
    ThreadPool     pool(4);
    PluginRegistry reg;
    std::atomic<int> ran{0};

    reg.register_node("Fail",    []() { return std::make_unique<ThrowingNode>(); });
    reg.register_node("Counter", [&ran]() {
        return std::make_unique<CountingNode>(ran);
    });

    // A (fails) → B (should be cancelled)
    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "Fail",    "dependencies": []},
            {"id": "B", "type": "Counter", "dependencies": ["A"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    engine.loadConfig(path);
    engine.run();
    engine.waitForCompletion();
    ::unlink(path.c_str());

    // B was cancelled, so counter must still be 0.
    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(engine.getMetrics().failed_count.load(),    1u);
    EXPECT_EQ(engine.getMetrics().cancelled_count.load(), 1u);
}

// ---------------------------------------------------------------------------
// run — in-progress guard
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, RunWhileRunningThrows)
{
    ThreadPool     pool(1);
    PluginRegistry reg;

    // Slow node to keep the first run alive.
    struct SlowNode final : WorkflowNode {
        void execute(ExecutionContext&) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    };
    reg.register_node("Slow", []() { return std::make_unique<SlowNode>(); });

    const std::string json = R"({"nodes":[{"id":"X","type":"Slow","dependencies":[]}]})";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    engine.loadConfig(path);
    engine.run();  // starts the slow node

    // Second call must throw while first run is in progress.
    EXPECT_THROW(engine.run(), std::runtime_error);

    engine.waitForCompletion();
    ::unlink(path.c_str());
}

// ---------------------------------------------------------------------------
// run — metrics are correct for a fully successful DAG
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, MetricsSuccessCount)
{
    ThreadPool     pool(2);
    PluginRegistry reg;
    reg.register_node("S", []() { return std::make_unique<SuccessNode>(); });

    const std::string json = R"({
        "nodes": [
            {"id": "A", "type": "S", "dependencies": []},
            {"id": "B", "type": "S", "dependencies": ["A"]}
        ]
    })";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    engine.loadConfig(path);
    engine.run();
    engine.waitForCompletion();
    ::unlink(path.c_str());

    EXPECT_EQ(engine.getMetrics().success_count.load(),   2u);
    EXPECT_EQ(engine.getMetrics().failed_count.load(),    0u);
    EXPECT_EQ(engine.getMetrics().cancelled_count.load(), 0u);
}

// ---------------------------------------------------------------------------
// run — retry logic: node succeeds on second attempt
// ---------------------------------------------------------------------------

TEST(WorkflowEngine, RetryNodeSucceedsEventually)
{
    ThreadPool     pool(2);
    PluginRegistry reg;

    std::atomic<int> attempts{0};
    struct FlakyNode final : WorkflowNode {
        explicit FlakyNode(std::atomic<int>& a) : attempts_(a) {}
        void execute(ExecutionContext&) override {
            if (attempts_.fetch_add(1) == 0)
                throw std::runtime_error("first attempt fails");
            // second attempt succeeds
        }
        std::atomic<int>& attempts_;
    };
    reg.register_node("Flaky", [&attempts]() {
        return std::make_unique<FlakyNode>(attempts);
    });

    // One node, max_retries = 1 → up to 2 attempts total
    const std::string json = R"({"nodes":[{"id":"F","type":"Flaky","dependencies":[],"max_retries":1}]})";
    auto path = write_temp_json(json);

    WorkflowEngine engine(pool, reg);
    engine.loadConfig(path);
    engine.run();
    engine.waitForCompletion();
    ::unlink(path.c_str());

    EXPECT_EQ(engine.getMetrics().success_count.load(), 1u);
    EXPECT_EQ(engine.getMetrics().failed_count.load(),  0u);
}
