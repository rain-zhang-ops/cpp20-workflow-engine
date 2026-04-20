#include "PluginRegistry.h"

// PluginRegistry 实现 — 工厂注册表，shared_mutex 保护并发读写
// create() 在锁外调用工厂函数，防止工厂内回调再次请求锁导致死锁

#include <mutex>
#include <shared_mutex>
#include <stdexcept>

// ============================================================================
// register_node
// ============================================================================

void PluginRegistry::register_node(const std::string& type, FactoryFunc factory)
{
    std::unique_lock lock(mutex_);
    factories_[type] = std::move(factory);
}

// ============================================================================
// create — look up factory and invoke it
// ============================================================================

std::unique_ptr<WorkflowNode> PluginRegistry::create(const std::string& type) const
{
    FactoryFunc fn;
    {
        std::shared_lock lock(mutex_);
        auto it = factories_.find(type);
        if (it == factories_.end()) {
            throw std::runtime_error(
                "PluginRegistry: unknown node type '" + type +
                "'. Did you forget to register it?");
        }
        fn = it->second;
    }
    // 锁外调用工厂函数：工厂可能触发 PluginManager::getNode()，
    // 若在锁内调用且工厂内部再次访问 registry 则会死锁。
    return fn();
}

// ============================================================================
// has
// ============================================================================

bool PluginRegistry::has(const std::string& type) const noexcept
{
    std::shared_lock lock(mutex_);
    return factories_.count(type) > 0;
}
