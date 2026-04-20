#pragma once

// ============================================================================
// PluginNodeAdapter — bridge between PluginManager and PluginRegistry
//
// PluginNodeAdapter —— PluginManager 与 PluginRegistry 之间的桥接适配器
// ============================================================================
//
// PROBLEM / 问题
// --------------
// PluginManager hot-loads a single .so and holds one WorkflowNode instance
// as an atomic<shared_ptr>.  PluginRegistry expects each factory call to
// return a std::unique_ptr<WorkflowNode>.
//
// PluginManager 热加载单个 .so 并以 atomic<shared_ptr> 持有一个 WorkflowNode 实例。
// PluginRegistry 期望每次工厂调用返回 std::unique_ptr<WorkflowNode>。
//
// SOLUTION / 解决方案
// -------------------
// PluginNodeAdapter wraps a shared_ptr<WorkflowNode> as a unique_ptr-
// compatible WorkflowNode that simply forwards execute() to the wrapped
// instance.  The adapter is created fresh on every factory call (trivial
// heap cost), while the underlying node — owned by PluginManager — is shared
// across concurrent executions as the plugin author intends.
//
// PluginNodeAdapter 将 shared_ptr<WorkflowNode> 包装为兼容 unique_ptr 的
// WorkflowNode，execute() 调用直接转发到被包装实例。
// 每次工厂调用创建一个新的适配器（开销极小），底层节点由 PluginManager 持有，
// 在并发执行间共享（这是插件作者的设计意图）。
//
// USAGE / 使用方式
// -----------------
//   PluginManager pm;
//   pm.reload("./libmy_plugin.so");
//
//   registry.register_node("MyNode", [&pm]() {
//       return std::make_unique<PluginNodeAdapter>(pm.getNode());
//   });
//
// Hot-reload: calling pm.reload() atomically swaps the underlying node.
// The next registry.create() call picks up the new instance automatically,
// while any in-flight adapter keeps the old .so alive until it is destroyed.
//
// 热重载：调用 pm.reload() 原子地交换底层节点。
// 下一次 registry.create() 调用自动获取新实例，
// 已在执行中的适配器保持旧 .so 存活直至析构。
// ============================================================================

#include "PluginManager.h"
#include "WorkflowNode.h"

#include <memory>

class PluginNodeAdapter final : public WorkflowNode {
public:
    /**
     * Construct with a shared node instance from PluginManager::getNode().
     * The shared_ptr keeps the underlying .so alive for the adapter's lifetime.
     *
     * 用 PluginManager::getNode() 返回的共享节点实例构造适配器。
     * shared_ptr 保证底层 .so 在适配器生命周期内存活。
     */
    explicit PluginNodeAdapter(std::shared_ptr<WorkflowNode> impl) noexcept
        : impl_(std::move(impl))
    {}

    /**
     * Forward execute() to the wrapped node.
     * Thread-safe as long as the wrapped WorkflowNode::execute() is thread-safe
     * (required by the WorkflowNode contract).
     *
     * 将 execute() 转发给被包装节点。
     * 只要被包装节点的 execute() 线程安全（WorkflowNode 契约要求），此处也线程安全。
     */
    void execute(ExecutionContext& ctx) override {
        if (impl_) impl_->execute(ctx);
    }

private:
    std::shared_ptr<WorkflowNode> impl_;
};
