#include "GraphBuilder.h"

#include "ExecutionContext.h"

#include <algorithm>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// GraphBuilder — constructor
// ============================================================================

GraphBuilder::GraphBuilder(PluginRegistry& registry)
    : registry_(registry)
{}

// ============================================================================
// check_cycles — Kahn's algorithm
//
// Builds an in-degree map and adjacency list from the deps arrays, then
// performs a BFS-based topological sort.  If any nodes remain after the
// sort (their in-degree never reached 0), they are part of a cycle.
//
// 从 deps 数组构建入度表和邻接表，然后执行基于 BFS 的拓扑排序。
// 排序后如有节点残留（入度未归零），则说明它们处于环路中。
// ============================================================================

void GraphBuilder::check_cycles(const nlohmann::json& nodes)
{
    // Collect all IDs first so we can validate dep references.
    // 先收集所有 ID，以便验证依赖引用。
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> adj; // id → successors

    for (const auto& node : nodes) {
        const std::string id = node["id"].get<std::string>();
        in_degree.emplace(id, 0); // ensure every node appears in the map
        adj.emplace(id, std::vector<std::string>{});
    }

    // Build adjacency list (dep → dependant) and compute in-degrees.
    // 构建邻接表（依赖 → 被依赖方）并计算入度。
    for (const auto& node : nodes) {
        const std::string id = node["id"].get<std::string>();
        if (!node.contains("deps")) continue;

        for (const auto& dep_json : node["deps"]) {
            const std::string dep = dep_json.get<std::string>();

            // Validate: dependency must exist in the same graph.
            // 验证：依赖必须存在于同一图中。
            if (!in_degree.count(dep)) {
                throw std::runtime_error(
                    "GraphBuilder: node '" + id +
                    "' references unknown dependency '" + dep + "'");
            }

            adj[dep].push_back(id);
            in_degree[id]++;
        }
    }

    // BFS / Kahn's topological sort.
    // BFS / Kahn 拓扑排序。
    std::queue<std::string> q;
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) q.push(id);
    }

    int visited = 0;
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        ++visited;

        for (const std::string& succ : adj[cur]) {
            if (--in_degree[succ] == 0) {
                q.push(succ);
            }
        }
    }

    // Any node with in_degree > 0 is part of a cycle.
    // 入度仍 > 0 的节点处于环路中。
    if (visited != static_cast<int>(in_degree.size())) {
        std::ostringstream oss;
        oss << "GraphBuilder: cycle detected involving nodes: [";
        bool first = true;
        for (const auto& [id, deg] : in_degree) {
            if (deg > 0) {
                if (!first) oss << ", ";
                oss << "'" << id << "'";
                first = false;
            }
        }
        oss << "]";
        throw std::runtime_error(oss.str());
    }
}

// ============================================================================
// build — parse JSON, validate, and construct tf::Taskflow
// ============================================================================

tf::Taskflow GraphBuilder::build(const std::string& json_str)
{
    // ------------------------------------------------------------------
    // 1. Parse JSON
    // ------------------------------------------------------------------
    nlohmann::json nodes;
    try {
        nodes = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(
            std::string("GraphBuilder: JSON parse error: ") + e.what());
    }

    if (!nodes.is_array()) {
        throw std::runtime_error(
            "GraphBuilder: top-level JSON must be an array of node descriptors");
    }

    // ------------------------------------------------------------------
    // 2. Validate all node IDs and dep references; detect cycles.
    //    验证所有节点 ID 和依赖引用；检测环路。
    // ------------------------------------------------------------------
    check_cycles(nodes);

    // ------------------------------------------------------------------
    // 3. Validate that all types are registered in the PluginRegistry.
    //    验证所有类型已在 PluginRegistry 中注册。
    // ------------------------------------------------------------------
    for (const auto& node : nodes) {
        const std::string type = node["type"].get<std::string>();
        if (!registry_.has(type)) {
            throw std::runtime_error(
                "GraphBuilder: node type '" + type +
                "' is not registered in PluginRegistry");
        }
    }

    // ------------------------------------------------------------------
    // 4. Build the Taskflow.
    //    构建 Taskflow。
    // ------------------------------------------------------------------
    tf::Taskflow flow;

    // Map node ID → tf::Task so we can wire precede() edges afterwards.
    // node ID → tf::Task 映射，用于后续连接 precede() 边。
    std::unordered_map<std::string, tf::Task> tasks;

    for (const auto& node : nodes) {
        const std::string id   = node["id"].get<std::string>();
        const std::string type = node["type"].get<std::string>();

        // Each task closure captures id and type by value.
        // The registry_ reference is safe: GraphBuilder outlives tf::Taskflow.
        //
        // 每个任务闭包按值捕获 id 和 type。
        // registry_ 引用安全：GraphBuilder 的生命周期长于 tf::Taskflow。
        tf::Task task = flow.emplace([this, id, type]() {
            // Create a fresh node instance for each execution.
            // 每次执行时创建新的节点实例。
            auto workflow_node = registry_.create(type);

            // Each task gets its own ExecutionContext for isolation.
            // 每个任务有独立的 ExecutionContext，保证隔离性。
            ExecutionContext ctx;
            workflow_node->execute(ctx);
        });

        task.name(id); // Label the task for visualisation / debugging
        tasks[id] = task;
    }

    // ------------------------------------------------------------------
    // 5. Wire dependency edges: dep.precede(dependant)
    //    连接依赖边：dep.precede(dependant)
    // ------------------------------------------------------------------
    for (const auto& node : nodes) {
        if (!node.contains("deps")) continue;

        const std::string id = node["id"].get<std::string>();
        for (const auto& dep_json : node["deps"]) {
            const std::string dep = dep_json.get<std::string>();
            // dep must finish before id starts
            tasks.at(dep).precede(tasks.at(id));
        }
    }

    return flow;
}

// ============================================================================
// run — execute with a tf::Executor
// ============================================================================

void GraphBuilder::run(tf::Taskflow& flow)
{
    tf::Executor executor;
    executor.run(flow).wait();
}
