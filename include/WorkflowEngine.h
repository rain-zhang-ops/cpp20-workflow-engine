#pragma once

#include "AsyncLogger.h"
#include "EngineMetrics.h"
#include "ExecutionContext.h"
#include "NodeState.h"
#include "PluginRegistry.h"
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

/**
 * Per-node static configuration parsed from JSON.
 *
 * Each node carries its own `type` which names the factory registered in
 * PluginRegistry.  This allows a single DAG to compose nodes of different
 * types — e.g. {"type": "Reader"}, {"type": "Transformer"}, {"type": "Writer"}.
 *
 * 每个节点携带自己的 `type` 字段，对应 PluginRegistry 中注册的工厂名称。
 * 这使一个 DAG 可以组合不同类型的节点。
 */
struct NodeConfig {
    std::string              id;
    std::string              type;            ///< Registered factory name in PluginRegistry
    std::vector<std::string> dependencies;
    int                      max_retries{0};  ///< Extra attempts on failure
};

/**
 * WorkflowEngine schedules a DAG of typed plugin nodes using a ThreadPool.
 *
 * Design:
 *  - Each node carries a `type` field that names a factory in PluginRegistry.
 *    A single DAG can therefore compose multiple different node types.
 *  - Each NodeState carries an atomic<int> uncompleted_deps counter.
 *  - Root nodes (zero deps) are pushed to the pool immediately on run().
 *  - When a node finishes, it atomically decrements each downstream counter;
 *    whenever a counter reaches 0 that node is immediately enqueued.
 *  - A std::latch counts down completions; waitForCompletion() blocks on it.
 *  - Plugin lifecycle (dlopen / hot-reload) is the caller's responsibility.
 *    Register node factories in PluginRegistry before calling loadConfig().
 *    Hot-reload: call PluginManager::reload() externally, then optionally
 *    call loadConfig() again to rebuild the DAG topology.
 *
 * Thread-safety: run(), waitForCompletion(), and onConfigChanged() may be
 * called from different threads, but run() must not be called concurrently
 * with itself.
 */
class WorkflowEngine {
public:
    WorkflowEngine(ThreadPool& pool, PluginRegistry& registry);

    /**
     * Parse JSON config and (re)build the internal DAG.
     * The JSON must describe each node with at least an "id", "type", and
     * optional "dependencies" / "max_retries" fields.
     * All types referenced by nodes must already be registered in the
     * PluginRegistry supplied at construction time.
     *
     * Plugin loading / hot-reload is intentionally NOT performed here —
     * that is the caller's responsibility (see PluginManager, PluginNodeAdapter).
     *
     * 解析 JSON 配置并（重新）构建内部 DAG。
     * JSON 中每个节点需包含 "id"、"type" 字段，以及可选的
     * "dependencies" / "max_retries" 字段。
     * 所有节点引用的类型必须已在构造时传入的 PluginRegistry 中注册。
     * 插件加载/热重载故意不在此处执行——由调用方负责。
     */
    void loadConfig(const std::string& json_path);

    /** Schedule all root nodes and start executing the DAG. */
    void run();

    /** Block until every node in the DAG has finished. */
    void waitForCompletion();

    /**
     * Callback suitable for ConfigWatcher: re-parses the JSON config file and
     * rebuilds the DAG topology so subsequent run() calls use the new graph.
     *
     * Note: plugin hot-reload (updating a PluginManager) is the caller's
     * responsibility and should be done before or after this call as needed.
     *
     * ConfigWatcher 回调：重新解析 JSON 配置文件并重建 DAG 拓扑，
     * 使后续的 run() 调用使用新图。
     * 插件热重载（更新 PluginManager）是调用方的职责。
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
        std::string              type;              // node type for PluginRegistry dispatch
        std::vector<std::string> downstream;        // IDs of nodes that depend on this
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
    PluginRegistry& registry_;

    // Shared context for a single DAG run.  Created fresh in run() and
    // passed by reference to every node's execute().
    //
    // 单次 DAG 运行的共享上下文。在 run() 中重新创建，
    // 并以引用方式传递给每个节点的 execute()。
    ExecutionContext execution_ctx_;

    std::vector<NodeConfig>                                             node_configs_;
    std::unordered_map<std::string, std::unique_ptr<NodeRuntimeState>> node_states_;

    std::unique_ptr<std::latch> completion_latch_;

    // Observability — both owned by WorkflowEngine.
    // AsyncLogger contains a std::jthread: declare it last so it is destroyed
    // first (before node_states_ / completion_latch_ are torn down).
    EngineMetrics metrics_;
    AsyncLogger   logger_;
};
