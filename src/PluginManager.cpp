#include "PluginManager.h"

// PluginManager 实现 — dlopen/dlsym 热加载，atomic<shared_ptr> 无锁节点替换
// 自定义 deleter 确保 .so 在最后一个引用释放后才执行 dlclose，避免 use-after-free

#include <dlfcn.h>

#include <stdexcept>

// ---------------------------------------------------------------------------
// reload — load new .so, create instance, atomically install it
// 热重载：dlopen 新 .so → dlsym 获取工厂/析构符号 → 创建节点 → 原子 store
// ---------------------------------------------------------------------------

void PluginManager::reload(const std::string& so_path)
{
    // RTLD_NOW: 立即解析所有符号，加载时快速发现损坏的 .so
    // RTLD_LOCAL: 不污染全局符号表，防止跨插件符号冲突
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        throw std::runtime_error(
            std::string("PluginManager: dlopen failed: ") + dlerror());

    using CreateFn  = WorkflowNode* (*)();
    using DestroyFn = void (*)(WorkflowNode*);

    auto* create_fn  = reinterpret_cast<CreateFn> (dlsym(handle, "create_node"));
    auto* destroy_fn = reinterpret_cast<DestroyFn>(dlsym(handle, "destroy_node"));

    if (!create_fn || !destroy_fn) {
        dlclose(handle);
        throw std::runtime_error(
            "PluginManager: create_node or destroy_node not found in " +
            so_path);
    }

    WorkflowNode* raw = create_fn();
    if (!raw) {
        dlclose(handle);
        throw std::runtime_error(
            "PluginManager: create_node returned nullptr from " + so_path);
    }

    // 自定义 deleter：按值捕获 handle 和 destroy_fn，
    // 保证引用计数归零时正确调用 destroy_node 再 dlclose
    auto new_node = std::shared_ptr<WorkflowNode>(
        raw,
        [handle, destroy_fn](WorkflowNode* p) noexcept {
            destroy_fn(p);
            dlclose(handle);
        });

    // 原子 store：旧节点 shared_ptr 引用计数递减；当计数归零时 deleter 触发 dlclose
    current_node_.store(std::move(new_node), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// getNode — lock-free atomic load
// memory_order_acquire 与 reload() 的 release 配对，保证可见最新节点
// ---------------------------------------------------------------------------

std::shared_ptr<WorkflowNode> PluginManager::getNode() const
{
    return current_node_.load(std::memory_order_acquire);
}
