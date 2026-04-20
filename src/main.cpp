#include "ConfigWatcher.h"
#include "PluginManager.h"
#include "PluginNodeAdapter.h"
#include "PluginRegistry.h"
#include "ThreadPool.h"
#include "WorkflowEngine.h"

// 主程序入口 — 组装引擎各组件并以守护进程模式运行 DAG 调度器
// 职责：信号处理、插件注册、热重载配置、控制平面启动、优雅关闭

// Control plane (UDS daemon thread)
#include "ControlPlane.h"

#include <atomic>
#include <csignal>
#include <future>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char* argv[])
{
    // Config file path: first argument or default relative to build dir
    const std::string config_path =
        (argc > 1) ? argv[1] : "../config/workflow.json";

    std::cout << "=== cpp20-workflow-engine ===\n"
              << "Config: " << config_path << "\n\n";

    // -----------------------------------------------------------------------
    // 0. Signal handling — register SIGTERM/SIGINT for graceful shutdown
    //
    //    sig_atomic_t 是信号处理函数中唯一保证 async-signal-safe 的类型。
    //    主循环以 100ms 间隔轮询此标志，无需引入复杂的信号机制。
    // -----------------------------------------------------------------------
    static volatile sig_atomic_t g_shutdown_requested = 0;

    struct SigHandler {
        static void handle(int) noexcept { g_shutdown_requested = 1; }
    };

    struct sigaction sa{};
    sa.sa_handler = SigHandler::handle;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT,  &sa, nullptr);

    // -----------------------------------------------------------------------
    // 1. Create the thread pool (hardware concurrency workers)
    // -----------------------------------------------------------------------
    ThreadPool pool;
    std::cout << "[main] ThreadPool created with "
              << std::thread::hardware_concurrency() << " worker(s)\n";

    // -----------------------------------------------------------------------
    // 2. Plugin registry — framework dispatch layer
    //
    //    注册表将类型名（如 "ExampleNode"）映射到工厂函数。
    //    WorkflowEngine 通过类型名创建节点，与 .so 加载细节解耦。
    //    扩展点：在 loadConfig() 前注册更多节点类型工厂。
    // -----------------------------------------------------------------------
    PluginRegistry registry;

    // -----------------------------------------------------------------------
    // 3. Plugin manager — owns the dlopen lifecycle for the example plugin
    //
    //    PluginManager 故意置于 WorkflowEngine 之外：
    //      a) 引擎保持对具体插件集无感知。
    //      b) 热重载（pm.reload）是应用层关注点。
    //      c) 多个 PluginManager 可并存，分别管理不同节点类型。
    // -----------------------------------------------------------------------
    PluginManager example_pm;
    const std::string example_so = "./libexample_plugin.so";
    example_pm.reload(example_so);

    // Register the "ExampleNode" type: each factory call creates an adapter
    // that forwards execute() to the single shared node held by example_pm.
    // When example_pm.reload() is called later the adapter picks up the new
    // instance automatically (PluginManager::getNode() is atomic).
    registry.register_node("ExampleNode", [&example_pm]() {
        return std::make_unique<PluginNodeAdapter>(example_pm.getNode());
    });

    // -----------------------------------------------------------------------
    // 4. Create the workflow engine (DAG scheduler)
    //    WorkflowEngine now depends only on ThreadPool and PluginRegistry —
    //    no knowledge of .so files or hot-reload mechanisms.
    // -----------------------------------------------------------------------
    WorkflowEngine engine(pool, registry);

    // -----------------------------------------------------------------------
    // 5. Load config (parses JSON, builds DAG from per-node "type" fields)
    // -----------------------------------------------------------------------
    try {
        engine.loadConfig(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[main] loadConfig failed: " << e.what() << '\n';
        return 1;
    }

    // -----------------------------------------------------------------------
    // 6. Set up ConfigWatcher for hot-reload
    //
    //    回调顺序：先 pm.reload() 替换节点实例，再 onConfigChanged() 重建 DAG。
    //    这样在引擎调度新 DAG 时，注册表工厂已指向最新 .so 实例。
    // -----------------------------------------------------------------------
    ConfigWatcher watcher(config_path, [&engine, &example_pm,
                                        &example_so, &config_path]() {
        // Reload the plugin first so the registry factory immediately serves
        // the new instance once the engine starts scheduling nodes.
        try {
            example_pm.reload(example_so);
            std::cout << "[main] Hot-reloaded plugin: " << example_so << '\n';
        } catch (const std::exception& e) {
            std::cerr << "[main] Plugin reload failed: " << e.what() << '\n';
        }
        // Rebuild the DAG from the updated config.
        engine.onConfigChanged(config_path);
    });
    std::cout << "[main] ConfigWatcher started for: " << config_path << "\n\n";

    // -----------------------------------------------------------------------
    // 7. Control plane — UDS daemon for remote CLI control
    // -----------------------------------------------------------------------
    std::atomic<bool> engine_stop_requested{false};

    ControlPlane control_plane;

    control_plane.register_command("RELOAD", [&engine, &example_pm,
                                               &example_so, &config_path]() -> std::string {
        try {
            example_pm.reload(example_so);
            engine.onConfigChanged(config_path);
            return "OK: reloaded plugin and DAG from " + config_path;
        } catch (const std::exception& e) {
            return std::string("ERROR: reload failed: ") + e.what();
        }
    });

    control_plane.register_command("STOP", [&engine_stop_requested]() -> std::string {
        engine_stop_requested.store(true, std::memory_order_release);
        return "OK: stop signal sent — engine will exit shortly";
    });

    control_plane.register_command("STATUS", [&engine]() -> std::string {
        const auto& m = engine.getMetrics();
        std::string info;
        info  = "STATUS: workflow-engine running\n";
        info += "  success_count    : " +
                std::to_string(m.success_count.load()) + "\n";
        info += "  failed_count     : " +
                std::to_string(m.failed_count.load()) + "\n";
        info += "  cancelled_count  : " +
                std::to_string(m.cancelled_count.load()) + "\n";
        info += "  hot_reload_count : " +
                std::to_string(m.hot_reload_count.load()) + "\n";
        return info;
    });

    std::jthread control_thread(
        [&control_plane](std::stop_token st) {
            control_plane.start_control_plane(std::move(st));
        });

    std::cout << "[main] ControlPlane started (socket: /tmp/workflow.sock)\n\n";

    // -----------------------------------------------------------------------
    // 8. Daemon mode — block main thread until stop signal or CLI STOP command
    // 100ms 轮询间隔：在信号/CLI 响应延迟与 CPU 占用之间取得平衡
    // -----------------------------------------------------------------------
    std::cout << "[main] Engine is running in daemon mode. "
                 "Waiting for CLI commands via socket...\n";

    while (!g_shutdown_requested &&
           !engine_stop_requested.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // --- Graceful shutdown (reached only if promise is fulfilled) ----------
    control_thread.request_stop();
    control_thread.join();

    std::cout << "\n[main] Done.\n";
    return 0;
}