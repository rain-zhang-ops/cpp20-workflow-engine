#include "WorkflowEngine.h"

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

WorkflowEngine::WorkflowEngine(ThreadPool& pool, PluginManager& plugin_mgr)
    : pool_(pool), plugin_mgr_(plugin_mgr)
{}

// ---------------------------------------------------------------------------
// loadConfig — parse JSON, rebuild DAG, hot-load plugin
// ---------------------------------------------------------------------------

void WorkflowEngine::loadConfig(const std::string& json_path)
{
    const std::string text = readFile(json_path);
    std::size_t pos = 0;
    const JsonValue root = parseValue(text, pos);

    if (root.type != JsonValue::Type::Object)
        throw std::runtime_error("workflow.json: root must be a JSON object");

    // --- plugin path ---------------------------------------------------------
    const JsonValue* plugin_val = root.get("plugin");
    if (!plugin_val || plugin_val->type != JsonValue::Type::String)
        throw std::runtime_error("workflow.json: missing \"plugin\" string");
    plugin_so_path_ = plugin_val->str;

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

        const JsonValue* deps_val = node_jv.get("dependencies");
        if (deps_val && deps_val->type == JsonValue::Type::Array) {
            for (const JsonValue& dep : deps_val->arr) {
                if (dep.type == JsonValue::Type::String)
                    cfg.dependencies.push_back(dep.str);
            }
        }
        node_configs_.push_back(std::move(cfg));
    }

    // --- build NodeState map --------------------------------------------------
    node_states_.clear();
    for (const NodeConfig& cfg : node_configs_) {
        auto state = std::make_unique<NodeState>();
        state->id = cfg.id;
        node_states_[cfg.id] = std::move(state);
    }

    // populate downstream adjacency lists
    for (const NodeConfig& cfg : node_configs_) {
        for (const std::string& dep : cfg.dependencies)
            node_states_.at(dep)->downstream.push_back(cfg.id);
    }

    // --- load plugin ----------------------------------------------------------
    plugin_mgr_.reload(plugin_so_path_);

    std::cout << "[WorkflowEngine] Loaded config: " << json_path
              << "  plugin=" << plugin_so_path_
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
        NodeState& state = *node_states_[cfg.id];
        state.uncompleted_deps.store(
            static_cast<int>(cfg.dependencies.size()),
            std::memory_order_relaxed);
        state.status.store(NodeReady, std::memory_order_relaxed);
    }

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
    std::cout << "[WorkflowEngine] DAG complete\n";
}

// ---------------------------------------------------------------------------
// onConfigChanged — hot-reload callback for ConfigWatcher
// ---------------------------------------------------------------------------

void WorkflowEngine::onConfigChanged(const std::string& json_path)
{
    std::cout << "[WorkflowEngine] Config changed, reloading plugin...\n";
    try {
        // Re-parse config to pick up any new plugin path.
        const std::string text = readFile(json_path);
        std::size_t pos = 0;
        const JsonValue root = parseValue(text, pos);

        const JsonValue* plugin_val = root.get("plugin");
        if (plugin_val && plugin_val->type == JsonValue::Type::String &&
            !plugin_val->str.empty())
        {
            plugin_so_path_ = plugin_val->str;
            plugin_mgr_.reload(plugin_so_path_);
            std::cout << "[WorkflowEngine] Hot-reloaded plugin: "
                      << plugin_so_path_ << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "[WorkflowEngine] onConfigChanged error: " << e.what() << '\n';
    }
}

// ---------------------------------------------------------------------------
// scheduleNode — push a ready node onto the thread pool
// ---------------------------------------------------------------------------

void WorkflowEngine::scheduleNode(const std::string& node_id)
{
    NodeState& state = *node_states_[node_id];
    state.status.store(NodeRunning, std::memory_order_relaxed);
    pool_.enqueue([this, node_id]() { executeNode(node_id); });
}

// ---------------------------------------------------------------------------
// executeNode — run the plugin, then trigger downstream nodes
// ---------------------------------------------------------------------------

void WorkflowEngine::executeNode(const std::string& node_id)
{
    NodeState& state = *node_states_[node_id];

    std::cout << "[WorkflowEngine] Node '" << node_id << "' starting\n";

    // Atomically capture the current plugin instance.
    // If a hot-reload happens mid-execution, this node uses its own copy of
    // the shared_ptr and the old .so stays alive until we release it here.
    auto node = plugin_mgr_.getNode();
    if (node) {
        node->execute(execution_ctx_);
    } else {
        std::cerr << "[WorkflowEngine] Node '" << node_id
                  << "': no plugin loaded!\n";
        state.status.store(NodeFailed, std::memory_order_relaxed);
        completion_latch_->count_down();
        return;
    }

    state.status.store(NodeFinished, std::memory_order_relaxed);
    std::cout << "[WorkflowEngine] Node '" << node_id << "' finished\n";

    completion_latch_->count_down();

    // Decrement dependency counter of every downstream node; schedule those
    // that become ready (counter reaches 0).
    for (const std::string& ds_id : state.downstream) {
        NodeState& ds = *node_states_[ds_id];
        if (ds.uncompleted_deps.fetch_sub(1, std::memory_order_acq_rel) == 1)
            scheduleNode(ds_id);
    }
}
