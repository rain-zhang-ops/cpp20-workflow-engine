#pragma once

// ============================================================================
// PluginRegistry — maps node type names to factory functions
//
// PluginRegistry — 将节点类型名称映射到工厂函数
// ============================================================================
//
// DESIGN / 设计思路
// -----------------
// Before GraphBuilder constructs a tf::Taskflow, the engine registers factory
// lambdas for each known node type.  When GraphBuilder processes a JSON entry
// it calls PluginRegistry::create(type) which invokes the stored lambda and
// returns a freshly constructed WorkflowNode.
//
// 在 GraphBuilder 构建 tf::Taskflow 前，引擎为每种已知节点类型注册工厂 lambda。
// GraphBuilder 处理 JSON 条目时调用 PluginRegistry::create(type)，
// 触发存储的 lambda 并返回一个新构造的 WorkflowNode 实例。
//
// INTEGRATION WITH PluginManager / 与 PluginManager 集成
// -------------------------------------------------------
// When PluginManager loads a .so it can call
//   registry.register_node("TypeName", [&pm]() {
//       return std::make_unique<PluginNodeAdapter>(pm.getNode());
//   });
// so JSON-driven graphs can reference dynamically loaded plugins by type name.
// ============================================================================

#include "WorkflowNode.h"

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

class PluginRegistry {
public:
    /// Factory function signature: returns a newly constructed WorkflowNode.
    /// 工厂函数签名：返回一个新构造的 WorkflowNode。
    using FactoryFunc = std::function<std::unique_ptr<WorkflowNode>()>;

    // -----------------------------------------------------------------------
    // register_node — install a factory for the given type name
    //
    // register_node — 为指定类型名称安装工厂函数
    //
    // A second call with the same type overwrites the previous registration.
    // 以相同类型名再次调用会覆盖之前的注册。
    // -----------------------------------------------------------------------
    void register_node(const std::string& type, FactoryFunc factory);

    // -----------------------------------------------------------------------
    // create — instantiate a node of the given type
    //
    // create — 实例化指定类型的节点
    //
    // @throws std::runtime_error if the type has not been registered.
    //         若类型未注册，则抛出 std::runtime_error。
    // -----------------------------------------------------------------------
    [[nodiscard]] std::unique_ptr<WorkflowNode> create(const std::string& type) const;

    // -----------------------------------------------------------------------
    // has — return true if the type is registered
    // has — 若类型已注册则返回 true
    // -----------------------------------------------------------------------
    [[nodiscard]] bool has(const std::string& type) const noexcept;

private:
    mutable std::shared_mutex                            mutex_;
    std::unordered_map<std::string, FactoryFunc> factories_;
};
