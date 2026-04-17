#pragma once

#include <cstdint>
#include <string_view>

// Node status bit flags
enum NodeStatus : uint8_t {
    NodeReady    = 0x01,
    NodeRunning  = 0x02,
    NodeFinished = 0x04,
    NodeFailed   = 0x08
};

/**
 * Abstract base class for all workflow plugin nodes.
 *
 * Plugins must export the following C symbols:
 *   extern "C" WorkflowNode* create_node();
 *   extern "C" void          destroy_node(WorkflowNode*);
 *
 * execute() is called from the thread pool and must be thread-safe:
 * multiple concurrent invocations on the same instance are possible.
 */
class WorkflowNode {
public:
    virtual ~WorkflowNode() = default;

    /**
     * Perform this node's work.
     * @param node_id  The DAG node identifier assigned by the engine.
     */
    virtual void execute(std::string_view node_id) = 0;
};
