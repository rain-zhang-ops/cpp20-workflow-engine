#pragma once

#include <cstdint>
#include <string>
#include <variant>

// ---------------------------------------------------------------------------
// C++23 std::expected detection
// Use it when available; fall back to std::variant<std::monostate, NodeError>
// on C++20-only compilers.
// ---------------------------------------------------------------------------
#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#  include <expected>
#  define WORKFLOW_HAS_EXPECTED 1
#else
#  define WORKFLOW_HAS_EXPECTED 0
#endif

/**
 * Lifecycle state of a workflow DAG node.
 * 工作流 DAG 节点的生命周期状态。
 */
enum class NodeState : uint8_t {
    Pending   = 0,  ///< Waiting for upstream dependencies to complete
    Running   = 1,  ///< Currently executing in the thread pool
    Success   = 2,  ///< Execution completed successfully
    Failed    = 3,  ///< Execution failed (all retries exhausted)
    Cancelled = 4,  ///< Skipped due to upstream failure (fail-fast)
};

/**
 * Error information returned by a single node execution attempt.
 * 节点单次执行尝试返回的错误信息。
 */
struct NodeError {
    int         code{0};
    std::string message;
};

// ---------------------------------------------------------------------------
// NodeResult — zero-overhead error handling
//
//  C++23: std::expected<void, NodeError>
//  C++20: std::variant<std::monostate, NodeError>
//         (std::monostate == success; NodeError == failure)
//
// A thin set of helper functions provides a uniform API regardless of which
// backend is in use.
// ---------------------------------------------------------------------------

#if WORKFLOW_HAS_EXPECTED

using NodeResult = std::expected<void, NodeError>;

[[nodiscard]] inline NodeResult makeNodeSuccess() noexcept { return {}; }

[[nodiscard]] inline NodeResult
makeNodeError(int code, std::string msg) noexcept {
    return std::unexpected(NodeError{code, std::move(msg)});
}

[[nodiscard]] inline bool nodeResultOk(const NodeResult& r) noexcept {
    return r.has_value();
}

[[nodiscard]] inline const NodeError&
nodeResultError(const NodeResult& r) noexcept {
    return r.error();
}

#else  // C++20 fallback

using NodeResult = std::variant<std::monostate, NodeError>;

[[nodiscard]] inline NodeResult makeNodeSuccess() noexcept {
    return std::monostate{};
}

[[nodiscard]] inline NodeResult
makeNodeError(int code, std::string msg) noexcept {
    return NodeError{code, std::move(msg)};
}

[[nodiscard]] inline bool nodeResultOk(const NodeResult& r) noexcept {
    return std::holds_alternative<std::monostate>(r);
}

[[nodiscard]] inline const NodeError&
nodeResultError(const NodeResult& r) noexcept {
    return std::get<NodeError>(r);
}

#endif  // WORKFLOW_HAS_EXPECTED
