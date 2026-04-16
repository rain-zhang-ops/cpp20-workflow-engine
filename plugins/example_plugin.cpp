#include "WorkflowNode.h"

#include <iostream>
#include <thread>

/**
 * A minimal example plugin that satisfies the WorkflowNode interface.
 *
 * execute() is thread-safe: it only reads node_id (immutable reference) and
 * writes to stdout (which is internally synchronised), so multiple DAG nodes
 * can call it concurrently on the same shared instance.
 */
class ExampleNode : public WorkflowNode {
public:
    void execute(std::string_view node_id) override {
        std::cout << "[ExampleNode] node='" << node_id
                  << "'  thread=" << std::this_thread::get_id() << '\n';
    }
};

// ---- C factory / destructor -------------------------------------------------

extern "C" WorkflowNode* create_node() {
    return new ExampleNode();
}

extern "C" void destroy_node(WorkflowNode* node) {
    delete node;
}
