// ============================================================================
// json_taskflow_demo — demonstrates GraphBuilder and PluginRegistry
//
// json_taskflow_demo —— 演示 GraphBuilder 和 PluginRegistry 的使用
// ============================================================================
//
// Demonstrates:
//  1. Registering mock node types in PluginRegistry.
//  2. Building and running a valid DAG from JSON.
//  3. Error case: referencing a non-existent dependency ID.
//  4. Error case: circular dependency (A→B→C→A).
//
// 演示内容：
//  1. 在 PluginRegistry 中注册模拟节点类型。
//  2. 从 JSON 构建并运行合法的 DAG。
//  3. 错误用例：引用不存在的依赖 ID。
//  4. 错误用例：存在循环依赖（A→B→C→A）。
// ============================================================================

#include "GraphBuilder.h"
#include "PluginRegistry.h"

#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

// ============================================================================
// Mock node implementations
// ============================================================================

class ReaderNode : public WorkflowNode {
public:
    void execute(ExecutionContext& /*ctx*/) override {
        std::cout << "  [ReaderNode]    Reading data...\n";
    }
};

class ProcessorNode : public WorkflowNode {
public:
    void execute(ExecutionContext& /*ctx*/) override {
        std::cout << "  [ProcessorNode] Processing data...\n";
    }
};

class WriterNode : public WorkflowNode {
public:
    void execute(ExecutionContext& /*ctx*/) override {
        std::cout << "  [WriterNode]    Writing results...\n";
    }
};

// ============================================================================
// main
// ============================================================================

int main()
{
    std::cout << "=== json_taskflow_demo ===\n\n";

    // -----------------------------------------------------------------------
    // 1. Set up PluginRegistry with mock node factories.
    //    在 PluginRegistry 中注册模拟节点工厂。
    // -----------------------------------------------------------------------
    PluginRegistry registry;
    registry.register_node("Reader",    [] { return std::make_unique<ReaderNode>();    });
    registry.register_node("Processor", [] { return std::make_unique<ProcessorNode>(); });
    registry.register_node("Writer",    [] { return std::make_unique<WriterNode>();    });

    GraphBuilder builder(registry);

    // -----------------------------------------------------------------------
    // 2. Happy path: valid linear DAG  A → B → C
    //    正常路径：有效的线性 DAG  A → B → C
    // -----------------------------------------------------------------------
    {
        std::cout << "--- Test 1: valid linear DAG (Reader → Processor → Writer) ---\n";

        const std::string json = R"([
            {"id": "node_A", "type": "Reader"},
            {"id": "node_B", "type": "Processor", "deps": ["node_A"]},
            {"id": "node_C", "type": "Writer",    "deps": ["node_B"]}
        ])";

        auto t0 = std::chrono::high_resolution_clock::now();
        try {
            tf::Taskflow flow = builder.build(json);
            builder.run(flow);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            std::cout << "  [OK] DAG executed in " << us << " µs\n\n";
        } catch (const std::exception& e) {
            std::cout << "  [FAIL] Unexpected exception: " << e.what() << "\n\n";
        }
    }

    // -----------------------------------------------------------------------
    // 3. Happy path: diamond-shaped DAG (A -> B, A -> C; B,C -> D)
    //    正常路径：菱形 DAG
    // -----------------------------------------------------------------------
    {
        std::cout << "--- Test 2: diamond DAG (Reader → Processor, Reader → Writer → Processor#2) ---\n";

        registry.register_node("Aggregator", [] { return std::make_unique<WriterNode>(); });

        const std::string json = R"([
            {"id": "A", "type": "Reader"},
            {"id": "B", "type": "Processor", "deps": ["A"]},
            {"id": "C", "type": "Writer",    "deps": ["A"]},
            {"id": "D", "type": "Aggregator","deps": ["B", "C"]}
        ])";

        try {
            tf::Taskflow flow = builder.build(json);
            builder.run(flow);
            std::cout << "  [OK] Diamond DAG executed successfully\n\n";
        } catch (const std::exception& e) {
            std::cout << "  [FAIL] Unexpected exception: " << e.what() << "\n\n";
        }
    }

    // -----------------------------------------------------------------------
    // 4. Error: node references a non-existent dependency ID
    //    错误：节点引用了不存在的依赖 ID
    // -----------------------------------------------------------------------
    {
        std::cout << "--- Test 3: missing dependency ID (expected error) ---\n";

        const std::string json = R"([
            {"id": "X", "type": "Reader"},
            {"id": "Y", "type": "Processor", "deps": ["X", "DOES_NOT_EXIST"]}
        ])";

        try {
            tf::Taskflow flow = builder.build(json);
            std::cout << "  [FAIL] Expected an exception but none was thrown\n\n";
        } catch (const std::runtime_error& e) {
            std::cout << "  [OK] Caught expected error: " << e.what() << "\n\n";
        }
    }

    // -----------------------------------------------------------------------
    // 5. Error: circular dependency  A → B → C → A
    //    错误：循环依赖  A → B → C → A
    // -----------------------------------------------------------------------
    {
        std::cout << "--- Test 4: circular dependency A→B→C→A (expected error) ---\n";

        const std::string json = R"([
            {"id": "A", "type": "Reader",    "deps": ["C"]},
            {"id": "B", "type": "Processor", "deps": ["A"]},
            {"id": "C", "type": "Writer",    "deps": ["B"]}
        ])";

        try {
            tf::Taskflow flow = builder.build(json);
            std::cout << "  [FAIL] Expected an exception but none was thrown\n\n";
        } catch (const std::runtime_error& e) {
            std::cout << "  [OK] Caught expected error: " << e.what() << "\n\n";
        }
    }

    // -----------------------------------------------------------------------
    // 6. Error: unregistered node type
    //    错误：未注册的节点类型
    // -----------------------------------------------------------------------
    {
        std::cout << "--- Test 5: unknown node type (expected error) ---\n";

        const std::string json = R"([
            {"id": "Z", "type": "UnknownType"}
        ])";

        try {
            tf::Taskflow flow = builder.build(json);
            std::cout << "  [FAIL] Expected an exception but none was thrown\n\n";
        } catch (const std::runtime_error& e) {
            std::cout << "  [OK] Caught expected error: " << e.what() << "\n\n";
        }
    }

    std::cout << "=== All tests completed ===\n";
    return 0;
}
