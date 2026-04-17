#pragma once

#include <cstdint>
#include <string_view>

// Forward-declare ExecutionContext so that WorkflowNode.h remains self-contained
// and plugins can include it without pulling in the full ExecutionContext header.
// 前向声明 ExecutionContext，使 WorkflowNode.h 保持自包含，
// 插件包含此头文件时无需引入完整的 ExecutionContext 头文件。
class ExecutionContext;

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
 *
 * ABI NOTE / ABI 注意事项:
 *   The ExecutionContext reference is always allocated and owned by the main
 *   binary.  Plugin code must NOT call any C++ member functions on it directly
 *   (different typeinfo / allocators across .so boundaries).  Use the
 *   extern "C" helpers declared in ExecutionContext.h instead.
 *
 *   ExecutionContext 引用始终由主程序分配和拥有。
 *   插件代码不得直接调用其 C++ 成员函数（跨 .so 边界存在 typeinfo/分配器差异）。
 *   请改用 ExecutionContext.h 中声明的 extern "C" 辅助函数。
 */
class WorkflowNode {
public:
    virtual ~WorkflowNode() = default;

    /**
     * Perform this node's work.
     * @param ctx  Shared execution context for the current DAG run.
     *             Use the extern "C" ctx_* helpers to read/write data safely.
     *
     * 执行该节点的工作。
     * @param ctx  当前 DAG 运行的共享执行上下文。
     *             请使用 extern "C" ctx_* 辅助函数安全地读写数据。
     */
    virtual void execute(ExecutionContext& ctx) = 0;
};
