#include "PluginRegistry.h"

#include <stdexcept>

// ============================================================================
// register_node
// ============================================================================

void PluginRegistry::register_node(const std::string& type, FactoryFunc factory)
{
    factories_[type] = std::move(factory);
}

// ============================================================================
// create — look up factory and invoke it
// ============================================================================

std::unique_ptr<WorkflowNode> PluginRegistry::create(const std::string& type) const
{
    auto it = factories_.find(type);
    if (it == factories_.end()) {
        throw std::runtime_error(
            "PluginRegistry: unknown node type '" + type +
            "'. Did you forget to register it?");
    }
    return it->second();
}

// ============================================================================
// has
// ============================================================================

bool PluginRegistry::has(const std::string& type) const noexcept
{
    return factories_.count(type) > 0;
}
