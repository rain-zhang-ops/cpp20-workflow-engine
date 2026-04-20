#include "PluginRegistry.h"

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
    // Invoke factory outside the lock — factory may call back into the engine.
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
