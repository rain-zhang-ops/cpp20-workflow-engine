#pragma once

#include "ExecutionContext.h"
#include "NodeState.h"
#include "WorkflowNode.h"

#include <exception>
#include <string>

/**
 * NodeExecutionWrapper provides a hardened execution shell for a single
 * workflow node invocation:
 *
 *  - Sandbox: wraps the plugin's execute() in a broad try-catch(...) so that
 *    no exception can propagate to the thread pool and crash the process.
 *  - Retry: re-attempts execution up to max_retries additional times on
 *    failure before transitioning to NodeState::Failed.
 *  - State machine: tracks the node's lifecycle through
 *      Pending → Running → Success | Failed
 *
 * Thread-safety: each instance is single-use and is consumed from one thread.
 *
 * NodeExecutionWrapper 为单次工作流节点调用提供硬化执行外壳：
 *  - 沙箱：用宽泛的 try-catch(...) 包裹插件的 execute()，确保任何异常都
 *    不会传播到线程池，从而避免进程崩溃。
 *  - 重试：失败时最多额外重试 max_retries 次，再转为 NodeState::Failed。
 *  - 状态机：跟踪节点生命周期 Pending → Running → Success | Failed。
 */
class NodeExecutionWrapper {
public:
    NodeExecutionWrapper(WorkflowNode*    node,
                         ExecutionContext& ctx,
                         int              max_retries = 0) noexcept
        : node_(node), ctx_(ctx), max_retries_(max_retries) {}

    /**
     * Execute the node with sandbox protection and retry logic.
     * Returns the final NodeResult (success or the last error encountered).
     *
     * 执行节点，含沙箱保护和重试逻辑，返回最终 NodeResult。
     */
    NodeResult execute() noexcept {
        state_ = NodeState::Running;

        const int max_attempts = max_retries_ + 1;
        NodeResult last_result = makeNodeError(0, "not executed");

        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            last_result = executeOnce();
            if (nodeResultOk(last_result)) {
                state_ = NodeState::Success;
                return last_result;
            }
            // On non-final failures the loop continues (retry).
        }

        state_ = NodeState::Failed;
        return last_result;
    }

    /** Current state of this execution. */
    [[nodiscard]] NodeState state() const noexcept { return state_; }

private:
    /** One sandboxed attempt — never throws. */
    NodeResult executeOnce() noexcept {
        try {
            if (!node_)
                return makeNodeError(1, "null plugin node");
            node_->execute(ctx_);
            return makeNodeSuccess();
        } catch (const std::exception& e) {
            return makeNodeError(2,
                std::string("plugin exception: ") + e.what());
        } catch (...) {
            return makeNodeError(3, "plugin threw unknown exception");
        }
    }

    WorkflowNode*    node_;
    ExecutionContext& ctx_;
    int              max_retries_;
    NodeState        state_{NodeState::Pending};
};
