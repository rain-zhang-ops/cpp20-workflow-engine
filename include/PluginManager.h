#pragma once

// 插件热重载模块 — 管理单个 .so 插件的 dlopen/dlclose 生命周期
// 主要职责：atomic<shared_ptr> 原子交换新旧节点实例，保证热重载时无锁且在途引用不丢失

#include "WorkflowNode.h"

#include <atomic>
#include <memory>
#include <string>

/**
 * PluginManager hot-loads a single .so plugin and exposes its WorkflowNode.
 *
 * 热加载单个 .so 插件并暴露其 WorkflowNode 实例。
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
 * 自定义 deleter 在最后一个引用释放时调用 destroy_node() + dlclose()，
 * 防止 .so 在执行期间被卸载（use-after-free）。
 *
 * Thread-safety: reload() and getNode() may be called concurrently.
 * 线程安全：reload() 与 getNode() 可并发调用。
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
     * 原子替换旧节点；旧节点的 .so 在最后一个持有者释放引用后才执行 dlclose。
     * @throws std::runtime_error on dlopen / dlsym / create_node failure.
     */
    void reload(const std::string& so_path);

    /**
     * Atomically return a shared_ptr to the current WorkflowNode.
     * 无锁 atomic load，可在任意线程安全调用。
     * May return nullptr if reload() has never been called.
     */
    [[nodiscard]] std::shared_ptr<WorkflowNode> getNode() const;

private:
    // atomic<shared_ptr>：reload() store 与 getNode() load 均无需互斥锁
    std::atomic<std::shared_ptr<WorkflowNode>> current_node_{};
};
