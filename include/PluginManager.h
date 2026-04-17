#pragma once

#include "WorkflowNode.h"

#include <atomic>
#include <memory>
#include <string>

/**
 * PluginManager hot-loads a single .so plugin and exposes its WorkflowNode.
 *
 * Design:
 *  - reload() uses dlopen/dlsym to load a new .so, creates one WorkflowNode
 *    instance, then atomically swaps it into current_lib_.
 *  - The shared_ptr's custom deleter calls destroy_node() and dlclose() once
 *    all references to that instance are released — preventing use-after-free
 *    and .so unloading while code is still executing.
 *  - getNode() returns the current instance via an atomic load — lock-free
 *    and safe from any number of concurrent readers.
 *
 * Thread-safety: reload() and getNode() may be called concurrently.
 */
class PluginManager {
public:
    PluginManager()  = default;
    ~PluginManager() = default;

    PluginManager(const PluginManager&)            = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /**
     * Load (or reload) the plugin .so at so_path.
     * Creates one WorkflowNode instance and atomically installs it.
     * @throws std::runtime_error on dlopen / dlsym / create_node failure.
     */
    void reload(const std::string& so_path);

    /**
     * Atomically return a shared_ptr to the current WorkflowNode.
     * May return nullptr if reload() has never been called.
     */
    [[nodiscard]] std::shared_ptr<WorkflowNode> getNode() const;

private:
    // Stores the current WorkflowNode; custom deleter owns dlclose lifecycle.
    std::atomic<std::shared_ptr<WorkflowNode>> current_node_{};
};
