#pragma once

// ============================================================================
// GraphBuilder — parses a JSON node list and builds a tf::Taskflow DAG
//
// GraphBuilder —— 解析 JSON 节点列表并构建 tf::Taskflow DAG
// ============================================================================
//
// DESIGN / 设计思路
// -----------------
// Input JSON format (array of node descriptors):
//
//   [
//     {"id": "node_A", "type": "Reader"},
//     {"id": "node_B", "type": "Processor", "deps": ["node_A"]},
//     {"id": "node_C", "type": "Writer",    "deps": ["node_B"]}
//   ]
//
// For each entry:
//  1. A tf::Task is created via tf::Taskflow::emplace().
//  2. The task closure looks up the factory in PluginRegistry, constructs the
//     node, and calls execute() with a fresh ExecutionContext.
//  3. Dependency edges are wired with taskA.precede(taskB).
//
// Before building, a Kahn's-algorithm topological sort checks for cycles.
// If a cycle is detected, std::runtime_error is thrown naming the involved nodes.
//
// 构建前，用 Kahn 算法做拓扑排序检测环路。若发现环路，抛出 std::runtime_error
// 并说明涉及哪些节点。
//
// ROBUSTNESS / 健壮性
// --------------------
// - Missing dep ID   → std::runtime_error naming the referencing node
// - Unknown type     → std::runtime_error from PluginRegistry::create()
// - Cycle in DAG     → std::runtime_error naming the nodes in the cycle
// ============================================================================

#include "PluginRegistry.h"

#include <nlohmann/json.hpp>
#include <taskflow/taskflow.hpp>

#include <string>

class GraphBuilder {
public:
    // -----------------------------------------------------------------------
    // @param registry  Node factory registry used to instantiate nodes.
    //                  构建节点实例所用的工厂注册表。
    // -----------------------------------------------------------------------
    explicit GraphBuilder(PluginRegistry& registry);

    // -----------------------------------------------------------------------
    // build — parse JSON and construct a tf::Taskflow
    //
    // build — 解析 JSON 并构建 tf::Taskflow
    //
    // @param json_str  JSON array string describing the DAG.
    //                  描述 DAG 的 JSON 数组字符串。
    // @returns         A fully wired tf::Taskflow ready to be executed.
    //                  一个完整连线的 tf::Taskflow，可直接执行。
    // @throws std::runtime_error on missing deps, unknown types, or cycles.
    //         缺失依赖、未知类型或存在环路时抛出 std::runtime_error。
    // -----------------------------------------------------------------------
    [[nodiscard]] tf::Taskflow build(const std::string& json_str);

    // -----------------------------------------------------------------------
    // run — execute the taskflow using a tf::Executor
    //
    // run — 使用 tf::Executor 执行 taskflow
    //
    // Blocks until all tasks complete.
    // 阻塞直到所有任务完成。
    // -----------------------------------------------------------------------
    void run(tf::Taskflow& flow);

private:
    // -----------------------------------------------------------------------
    // check_cycles — Kahn's algorithm topological sort
    //
    // check_cycles — Kahn 算法拓扑排序
    //
    // @throws std::runtime_error listing the nodes involved in a cycle.
    //         若存在环路，抛出异常并列出涉及节点。
    // -----------------------------------------------------------------------
    void check_cycles(const nlohmann::json& nodes);

    PluginRegistry& registry_;
};
