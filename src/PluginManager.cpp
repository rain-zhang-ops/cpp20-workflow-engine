#include "PluginManager.h"

#include <dlfcn.h>

#include <stdexcept>

// ---------------------------------------------------------------------------
// reload — load new .so, create instance, atomically install it
// ---------------------------------------------------------------------------

void PluginManager::reload(const std::string& so_path)
{
    // RTLD_NOW: resolve all symbols immediately so we fail-fast on bad .so.
    // RTLD_LOCAL: don't pollute the global symbol table.
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

    // Custom deleter: calls destroy_node then dlclose.
    // Both destroy_fn and handle are captured by value so the deleter keeps
    // them alive for as long as *any* shared_ptr to this node exists.
    auto new_node = std::shared_ptr<WorkflowNode>(
        raw,
        [handle, destroy_fn](WorkflowNode* p) noexcept {
            destroy_fn(p);
            dlclose(handle);
        });

    // Atomic swap: any thread currently holding an old shared_ptr continues
    // using the old instance until it releases it.  When the last reference
    // to the old node drops, the custom deleter fires and dlclose is called.
    current_node_.store(std::move(new_node), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// getNode — lock-free atomic load
// ---------------------------------------------------------------------------

std::shared_ptr<WorkflowNode> PluginManager::getNode() const
{
    return current_node_.load(std::memory_order_acquire);
}
