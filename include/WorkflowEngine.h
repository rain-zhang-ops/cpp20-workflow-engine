#pragma once

#include "AsyncLogger.h"
#include "EngineMetrics.h"
#include "ExecutionContext.h"
#include "NodeState.h"
#include "PluginManager.h"
#include "ScopedTimer.h"
#include "ThreadPool.h"
#include "WorkflowNode.h"

#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/** Per-node static configuration parsed from JSON. */
struct NodeConfig {
    std::string              id;
    std::vector<std::string> dependencies;
    int                      max_retries{0};  ///< Extra attempts on failure
};

/**
 * WorkflowEngine schedules a DAG of plugin nodes using a ThreadPool.
 *
 * Design:
 *  - Each NodeState carries an atomic<int> uncompleted_deps counter.
 *  - Root nodes (zero deps) are pushed to the pool immediately on run().
 *  - When a node finishes, it atomically decrements each downstream counter;
 *    whenever a counter reaches 0 that node is immediately enqueued.
 *  - A std::latch counts down completions; waitForCompletion() blocks on it.
 *  - onConfigChanged() calls PluginManager::reload() so future node
 *    executions transparently pick up the new plugin without interrupting
 *    any already-running DAG tasks.
 *
 * Thread-safety: run(), waitForCompletion(), and onConfigChanged() may be
 * called from different threads, but run() must not be called concurrently
 * with itself.
 */
class WorkflowEngine {
public:
    WorkflowEngine(ThreadPool& pool, PluginManager& plugin_mgr);

    /**
     * Parse JSON config and (re)build the internal DAG.
     * Also calls plugin_mgr_.reload() with the plugin path from the config.
     */
    void loadConfig(const std::string& json_path);

    /** Schedule all root nodes and start executing the DAG. */
    void run();

    /** Block until every node in the DAG has finished. */
    void waitForCompletion();

    /**
     * Callback suitable for ConfigWatcher: re-parses the config file and
     * hot-reloads the plugin so subsequent DAG runs use the new code.
     */
    void onConfigChanged(const std::string& json_path);

    /**
     * Return a reference to the engine-wide performance counters.
     * Caller may read values at any time (all loads are atomic).
     *
     * 返回引擎全局性能计数器的常量引用，调用方可随时读取（所有读取均为原子）。
     */
    [[nodiscard]] const EngineMetrics& getMetrics() const noexcept {
        return metrics_;
    }

private:
    // Internal per-node runtime state (renamed from NodeState to avoid clash
    // with the public NodeState enum class defined in NodeState.h).
    struct NodeRuntimeState {
        std::string              id;
        std::vector<std::string> downstream;       // IDs of nodes that depend on this
        std::atomic<int>         uncompleted_deps{0};
        std::atomic<NodeState>   node_state{NodeState::Pending};
        int                      max_retries{0};
        // Timestamp recorded when the node is enqueued — used to compute
        // the queue-wait duration logged alongside the execution duration.
        std::chrono::high_resolution_clock::time_point enqueue_time{};
    };

    void scheduleNode(const std::string& node_id);
    void executeNode(const std::string& node_id);
    /** Atomically mark node (and all its transitive descendants) Cancelled. */
    void cancelNodeAndDownstream(const std::string& node_id);

    ThreadPool&    pool_;
    PluginManager& plugin_mgr_;

    // Shared context for a single DAG run.  Created fresh in run() and
    // passed by reference to every node's execute().
    //
    // 单次 DAG 运行的共享上下文。在 run() 中重新创建，
    // 并以引用方式传递给每个节点的 execute()。
    ExecutionContext execution_ctx_;

    std::vector<NodeConfig>                                             node_configs_;
    std::unordered_map<std::string, std::unique_ptr<NodeRuntimeState>> node_states_;

    std::string                 plugin_so_path_;
    std::unique_ptr<std::latch> completion_latch_;

    // Observability — both owned by WorkflowEngine.
    // AsyncLogger contains a std::jthread: declare it last so it is destroyed
    // first (before node_states_ / completion_latch_ are torn down).
    EngineMetrics metrics_;
    AsyncLogger   logger_;
};
