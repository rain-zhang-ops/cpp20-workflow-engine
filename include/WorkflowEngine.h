#pragma once

#include "PluginManager.h"
#include "ThreadPool.h"
#include "WorkflowNode.h"

#include <atomic>
#include <latch>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/** Per-node static configuration parsed from JSON. */
struct NodeConfig {
    std::string              id;
    std::vector<std::string> dependencies;
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

private:
    // Internal per-node runtime state
    struct NodeState {
        std::string              id;
        std::vector<std::string> downstream;       // IDs of nodes that depend on this
        std::atomic<int>         uncompleted_deps{0};
        std::atomic<uint8_t>     status{NodeReady};
    };

    void scheduleNode(const std::string& node_id);
    void executeNode(const std::string& node_id);

    ThreadPool&    pool_;
    PluginManager& plugin_mgr_;

    std::vector<NodeConfig>                                   node_configs_;
    std::unordered_map<std::string, std::unique_ptr<NodeState>> node_states_;

    std::string                plugin_so_path_;
    std::unique_ptr<std::latch> completion_latch_;
};
