#include "WorkflowEngine.h"
#include "NodeExecutionWrapper.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

// ============================================================================
// Minimal recursive-descent JSON parser (no third-party deps)
// ============================================================================
namespace {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type        type{Type::Null};
    std::string str{};
    double      num{0.0};
    bool        boolean{false};

    // Using vectors of pairs for Object to avoid recursive type issues with map
    std::vector<JsonValue>                       arr{};
    std::vector<std::pair<std::string,JsonValue>> obj{};

    // Helpers
    [[nodiscard]] const JsonValue* get(std::string_view key) const noexcept {
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
};

// --- tokeniser helpers -------------------------------------------------------

static void skipWs(std::string_view s, std::size_t& pos) noexcept {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' ||
                               s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
}

static std::string parseString(std::string_view s, std::size_t& pos) {
    ++pos;  // skip opening "
    std::string result;
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  result += '"';  break;
                case '\\': result += '\\'; break;
                case '/':  result += '/';  break;
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += s[pos];
            }
        } else {
            result += s[pos];
        }
        ++pos;
    }
    if (pos < s.size()) ++pos;  // skip closing "
    return result;
}

static JsonValue parseValue(std::string_view s, std::size_t& pos);

static JsonValue parseObject(std::string_view s, std::size_t& pos) {
    JsonValue v;
    v.type = JsonValue::Type::Object;
    ++pos;  // skip '{'
    skipWs(s, pos);
    while (pos < s.size() && s[pos] != '}') {
        skipWs(s, pos);
        if (s[pos] != '"') break;   // malformed
        std::string key = parseString(s, pos);
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == ':') ++pos;
        v.obj.emplace_back(std::move(key), parseValue(s, pos));
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
        skipWs(s, pos);
    }
    if (pos < s.size()) ++pos;  // skip '}'
    return v;
}

static JsonValue parseArray(std::string_view s, std::size_t& pos) {
    JsonValue v;
    v.type = JsonValue::Type::Array;
    ++pos;  // skip '['
    skipWs(s, pos);
    while (pos < s.size() && s[pos] != ']') {
        v.arr.push_back(parseValue(s, pos));
        skipWs(s, pos);
        if (pos < s.size() && s[pos] == ',') ++pos;
        skipWs(s, pos);
    }
    if (pos < s.size()) ++pos;  // skip ']'
    return v;
}

static JsonValue parseValue(std::string_view s, std::size_t& pos) {
    skipWs(s, pos);
    if (pos >= s.size()) return {};

    const char c = s[pos];
    if (c == '"') {
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.str  = parseString(s, pos);
        return v;
    }
    if (c == '{') return parseObject(s, pos);
    if (c == '[') return parseArray(s, pos);
    if (c == 't') { JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = true;  pos += 4; return v; }
    if (c == 'f') { JsonValue v; v.type = JsonValue::Type::Bool; v.boolean = false; pos += 5; return v; }
    if (c == 'n') { JsonValue v; v.type = JsonValue::Type::Null;                    pos += 4; return v; }

    // number
    JsonValue v;
    v.type = JsonValue::Type::Number;
    std::size_t start = pos;
    while (pos < s.size() && (std::isdigit(static_cast<unsigned char>(s[pos])) ||
                               s[pos] == '.' || s[pos] == '-' || s[pos] == '+' ||
                               s[pos] == 'e' || s[pos] == 'E'))
        ++pos;
    v.num = std::stod(std::string(s.substr(start, pos - start)));
    return v;
}

// Utility: read whole file into string
static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // anonymous namespace

// ============================================================================
// WorkflowEngine
// ============================================================================

WorkflowEngine::WorkflowEngine(ThreadPool& pool, PluginRegistry& registry)
    : pool_(pool), registry_(registry)
{}

// ---------------------------------------------------------------------------
// loadConfig — parse JSON, rebuild DAG
//
// Expected JSON format:
//   {
//     "nodes": [
//       {"id": "A", "type": "MyNodeType", "dependencies": [],        "max_retries": 2},
//       {"id": "B", "type": "OtherType",  "dependencies": ["A"],     "max_retries": 1},
//       {"id": "C", "type": "MyNodeType", "dependencies": ["A", "B"]               }
//     ]
//   }
//
// All "type" values must already be registered in the PluginRegistry supplied
// at construction.  Plugin loading is the caller's responsibility.
// ---------------------------------------------------------------------------

void WorkflowEngine::loadConfig(const std::string& json_path)
{
    const std::string text = readFile(json_path);
    std::size_t pos = 0;
    const JsonValue root = parseValue(text, pos);

    if (root.type != JsonValue::Type::Object)
        throw std::runtime_error("workflow.json: root must be a JSON object");

    // --- nodes array ---------------------------------------------------------
    const JsonValue* nodes_val = root.get("nodes");
    if (!nodes_val || nodes_val->type != JsonValue::Type::Array)
        throw std::runtime_error("workflow.json: missing \"nodes\" array");

    node_configs_.clear();
    for (const JsonValue& node_jv : nodes_val->arr) {
        if (node_jv.type != JsonValue::Type::Object) continue;

        NodeConfig cfg;

        const JsonValue* id_val = node_jv.get("id");
        if (!id_val || id_val->type != JsonValue::Type::String)
            throw std::runtime_error("workflow.json: node missing \"id\"");
        cfg.id = id_val->str;

        const JsonValue* type_val = node_jv.get("type");
        if (!type_val || type_val->type != JsonValue::Type::String)
            throw std::runtime_error(
                "workflow.json: node \"" + cfg.id + "\" missing \"type\"");
        cfg.type = type_val->str;

        const JsonValue* deps_val = node_jv.get("dependencies");
        if (deps_val && deps_val->type == JsonValue::Type::Array) {
            for (const JsonValue& dep : deps_val->arr) {
                if (dep.type == JsonValue::Type::String)
                    cfg.dependencies.push_back(dep.str);
            }
        }

        const JsonValue* retries_val = node_jv.get("max_retries");
        if (retries_val && retries_val->type == JsonValue::Type::Number)
            cfg.max_retries = static_cast<int>(retries_val->num);

        node_configs_.push_back(std::move(cfg));
    }

    // --- build NodeRuntimeState map ------------------------------------------
    node_states_.clear();
    for (const NodeConfig& cfg : node_configs_) {
        auto state        = std::make_unique<NodeRuntimeState>();
        state->id         = cfg.id;
        state->type       = cfg.type;
        state->max_retries = cfg.max_retries;
        node_states_[cfg.id] = std::move(state);
    }

    // populate downstream adjacency lists
    for (const NodeConfig& cfg : node_configs_) {
        for (const std::string& dep : cfg.dependencies)
            node_states_.at(dep)->downstream.push_back(cfg.id);
    }

    std::cout << "[WorkflowEngine] Loaded config: " << json_path
              << "  nodes=" << node_configs_.size() << '\n';
}

// ---------------------------------------------------------------------------
// run — reset state and schedule all root nodes
// ---------------------------------------------------------------------------

void WorkflowEngine::run()
{
    if (node_configs_.empty())
        throw std::runtime_error("WorkflowEngine: no nodes loaded — call loadConfig first");

    const int n = static_cast<int>(node_configs_.size());
    completion_latch_ = std::make_unique<std::latch>(n);

    // Reset per-run state
    for (const NodeConfig& cfg : node_configs_) {
        NodeRuntimeState& state = *node_states_[cfg.id];
        state.uncompleted_deps.store(
            static_cast<int>(cfg.dependencies.size()),
            std::memory_order_relaxed);
        state.node_state.store(NodeState::Pending, std::memory_order_relaxed);
    }

    logger_.info("[WorkflowEngine] Starting DAG with " +
                 std::to_string(n) + " nodes");
    std::cout << "[WorkflowEngine] Starting DAG with " << n << " nodes\n";

    // Clear any data left over from the previous run so nodes always start
    // with a clean shared context.
    // 清除上次运行遗留的数据，确保节点始终从干净的共享上下文开始。
    execution_ctx_.clear();

    // Schedule root nodes (no dependencies)
    for (const NodeConfig& cfg : node_configs_) {
        if (cfg.dependencies.empty())
            scheduleNode(cfg.id);
    }
}

// ---------------------------------------------------------------------------
// waitForCompletion
// ---------------------------------------------------------------------------

void WorkflowEngine::waitForCompletion()
{
    if (completion_latch_)
        completion_latch_->wait();

    // Flush async log output before printing the summary so all node-level
    // log lines appear before the metrics line.
    logger_.flush();
    std::cout << "[WorkflowEngine] DAG complete\n";
    metrics_.print();
}

// ---------------------------------------------------------------------------
// onConfigChanged — hot-reload callback for ConfigWatcher
//
// Rebuilds the DAG topology from the updated JSON file.
// Plugin hot-reload (PluginManager::reload) is the caller's responsibility
// and should be performed before or after this call as needed.
// ---------------------------------------------------------------------------

void WorkflowEngine::onConfigChanged(const std::string& json_path)
{
    std::cout << "[WorkflowEngine] Config changed, rebuilding DAG...\n";
    try {
        loadConfig(json_path);
        metrics_.recordHotReload();
        logger_.info("[WorkflowEngine] DAG rebuilt from: " + json_path);
        std::cout << "[WorkflowEngine] DAG rebuilt from: " << json_path << '\n';
    } catch (const std::exception& e) {
        std::cerr << "[WorkflowEngine] onConfigChanged error: " << e.what() << '\n';
    }
}

// ---------------------------------------------------------------------------
// scheduleNode — record enqueue time and push onto the thread pool
// ---------------------------------------------------------------------------

void WorkflowEngine::scheduleNode(const std::string& node_id)
{
    NodeRuntimeState& state = *node_states_[node_id];

    // Atomically transition Pending → Running.
    // If the CAS fails the node was already Cancelled by cascade cancellation;
    // in that case the latch was already counted down — do nothing.
    NodeState expected = NodeState::Pending;
    if (!state.node_state.compare_exchange_strong(
            expected, NodeState::Running,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    state.enqueue_time = std::chrono::high_resolution_clock::now();
    pool_.enqueue([this, node_id]() { executeNode(node_id); });
}

// ---------------------------------------------------------------------------
// executeNode — sandboxed execution with retries, timing, and cascade cancel
// ---------------------------------------------------------------------------

void WorkflowEngine::executeNode(const std::string& node_id)
{
    NodeRuntimeState& state = *node_states_[node_id];

    // Measure queue-wait time (enqueue → start of actual execution).
    auto queue_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now() - state.enqueue_time).count();

    logger_.info("[WorkflowEngine] Node '" + node_id +
                 "' starting  queue_wait=" + std::to_string(queue_wait_ns) + "ns");
    std::cout << "[WorkflowEngine] Node '" << node_id << "' starting\n";

    // Create a fresh node instance for this execution via the registry.
    // Using the per-node type means different nodes in the same DAG can
    // perform different operations.  For plugin-backed types the factory
    // typically wraps PluginManager::getNode() via PluginNodeAdapter, which
    // atomically captures the current shared_ptr — ensuring hot-reload safety:
    // the old .so stays alive until this unique_ptr is released.
    //
    // 通过注册表为本次执行创建新的节点实例。
    // 每节点独立 type 意味着同一 DAG 中的不同节点可执行不同操作。
    // 对于插件类型，工厂通常通过 PluginNodeAdapter 包装 PluginManager::getNode()，
    // 原子捕获当前 shared_ptr，确保热重载安全。
    std::unique_ptr<WorkflowNode> node;
    try {
        node = registry_.create(state.type);
    } catch (const std::exception& e) {
        // Registry lookup failure — treat as node failure.
        state.node_state.store(NodeState::Failed, std::memory_order_release);
        metrics_.recordFailure();
        logger_.error("[WorkflowEngine] Node '" + node_id +
                      "' registry error: " + e.what());
        std::cerr << "[WorkflowEngine] Node '" << node_id
                  << "' registry error: " << e.what() << '\n';
        completion_latch_->count_down();
        for (const std::string& ds_id : state.downstream)
            cancelNodeAndDownstream(ds_id);
        return;
    }

    // Execute with timing.
    // The ScopedTimer is scoped to just the wrapper.execute() call so that
    // exec_ns is valid by the time we use it in the log messages below.
    long long exec_ns = 0;
    NodeResult result = makeNodeError(0, "not executed");
    {
        ScopedTimer exec_timer([&](ScopedTimer::Duration d) {
            exec_ns = d.count();
        });
        NodeExecutionWrapper wrapper(node.get(), execution_ctx_,
                                     state.max_retries);
        result = wrapper.execute();
    }  // exec_timer destructs here → exec_ns is now valid

    if (nodeResultOk(result)) {
        state.node_state.store(NodeState::Success,
                               std::memory_order_release);
        metrics_.recordSuccess();
        logger_.info("[WorkflowEngine] Node '" + node_id +
                     "' finished (Success)  exec=" + std::to_string(exec_ns) + "ns");
        std::cout << "[WorkflowEngine] Node '" << node_id
                  << "' finished\n";

        completion_latch_->count_down();

        // Decrement dependency counter of every downstream node; schedule
        // those that become ready (counter reaches 0).
        for (const std::string& ds_id : state.downstream) {
            NodeRuntimeState& ds = *node_states_[ds_id];
            if (ds.uncompleted_deps.fetch_sub(1,
                    std::memory_order_acq_rel) == 1)
                scheduleNode(ds_id);
        }
    } else {
        const NodeError& err = nodeResultError(result);
        state.node_state.store(NodeState::Failed,
                               std::memory_order_release);
        metrics_.recordFailure();
        logger_.error("[WorkflowEngine] Node '" + node_id +
                      "' failed: " + err.message +
                      "  exec=" + std::to_string(exec_ns) + "ns");
        std::cerr << "[WorkflowEngine] Node '" << node_id
                  << "' failed: " << err.message << '\n';

        completion_latch_->count_down();

        // Fail-fast: cascade-cancel all transitive downstream nodes.
        for (const std::string& ds_id : state.downstream)
            cancelNodeAndDownstream(ds_id);
    }
}

// ---------------------------------------------------------------------------
// cancelNodeAndDownstream — atomically mark a node (and its descendants)
//   as Cancelled and count it down in the latch.
// ---------------------------------------------------------------------------

void WorkflowEngine::cancelNodeAndDownstream(const std::string& node_id)
{
    NodeRuntimeState& state = *node_states_[node_id];

    // Only transition from Pending; any other state means the node is already
    // handled (Running/Success/Failed/Cancelled) — don't interfere.
    NodeState expected = NodeState::Pending;
    if (!state.node_state.compare_exchange_strong(
            expected, NodeState::Cancelled,
            std::memory_order_acq_rel, std::memory_order_relaxed))
        return;

    metrics_.recordCancelled();
    logger_.warn("[WorkflowEngine] Node '" + node_id +
                 "' cancelled (upstream failure)");
    std::cout << "[WorkflowEngine] Node '" << node_id
              << "' cancelled (upstream failure)\n";

    completion_latch_->count_down();

    for (const std::string& ds_id : state.downstream)
        cancelNodeAndDownstream(ds_id);
}
