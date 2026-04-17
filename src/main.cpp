#include "ConfigWatcher.h"
#include "PluginManager.h"
#include "ThreadPool.h"
#include "WorkflowEngine.h"

// Control plane (UDS daemon thread)
// 控制平面（UDS 守护线程）
#include "ControlPlane.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char* argv[])
{
    // Config file path: first argument or default relative to build dir
    const std::string config_path =
        (argc > 1) ? argv[1] : "../config/workflow.json";

    std::cout << "=== cpp20-workflow-engine demo ===\n"
              << "Config: " << config_path << "\n\n";

    // -----------------------------------------------------------------------
    // 1. Create the thread pool (hardware concurrency workers)
    // -----------------------------------------------------------------------
    ThreadPool pool;
    std::cout << "[main] ThreadPool created with "
              << std::thread::hardware_concurrency() << " worker(s)\n";

    // -----------------------------------------------------------------------
    // 2. Create the plugin manager
    // -----------------------------------------------------------------------
    PluginManager plugin_mgr;

    // -----------------------------------------------------------------------
    // 3. Create the workflow engine (DAG scheduler)
    // -----------------------------------------------------------------------
    WorkflowEngine engine(pool, plugin_mgr);

    // -----------------------------------------------------------------------
    // 4. Load config (parses JSON, builds DAG, loads plugin via dlopen)
    // -----------------------------------------------------------------------
    try {
        engine.loadConfig(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[main] loadConfig failed: " << e.what() << '\n';
        return 1;
    }

    // -----------------------------------------------------------------------
    // 5. Set up ConfigWatcher for hot-reload
    //    The watcher fires engine.onConfigChanged() whenever the file is saved.
    // -----------------------------------------------------------------------
    ConfigWatcher watcher(config_path, [&engine, &config_path]() {
        engine.onConfigChanged(config_path);
    });
    std::cout << "[main] ConfigWatcher started for: " << config_path << "\n\n";

    // -----------------------------------------------------------------------
    // 6. Control plane — UDS daemon for remote CLI control
    //
    //    控制平面 —— 用于远程 CLI 控制的 UDS 守护进程
    //
    //    Runs in a std::jthread so it stops cleanly when jthread is destroyed.
    //    在 std::jthread 中运行，jthread 析构时自动干净停止。
    // -----------------------------------------------------------------------

    // Shared stop flag so the STOP command can signal main() to exit.
    // 共享停止标志，STOP 命令可通过它通知 main() 退出。
    std::atomic<bool> engine_stop_requested{false};

    ControlPlane control_plane;

    // RELOAD — hot-reload the plugin and re-parse the config.
    // RELOAD —— 热加载插件并重新解析配置。
    control_plane.register_command("RELOAD", [&engine, &config_path]() -> std::string {
        try {
            engine.onConfigChanged(config_path);
            return "OK: reloaded config from " + config_path;
        } catch (const std::exception& e) {
            return std::string("ERROR: reload failed: ") + e.what();
        }
    });

    // STOP — ask the engine to shut down gracefully.
    // STOP —— 请求引擎优雅关闭。
    control_plane.register_command("STOP", [&engine_stop_requested]() -> std::string {
        engine_stop_requested.store(true, std::memory_order_release);
        return "OK: stop signal sent — engine will exit shortly";
    });

    // STATUS — return runtime information about the engine.
    // STATUS —— 返回引擎运行时信息。
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

    // Launch the control plane in a background jthread.
    // stop_source is owned by the jthread; we keep a copy to request stop.
    // 在后台 jthread 中启动控制平面。
    // stop_source 由 jthread 持有；我们保留一份副本以便请求停止。
    std::jthread control_thread(
        [&control_plane](std::stop_token st) {
            control_plane.start_control_plane(std::move(st));
        });

    std::cout << "[main] ControlPlane started (socket: /tmp/workflow.sock)\n\n";

    // -----------------------------------------------------------------------
    // 7. Run the DAG once and wait for completion
    // -----------------------------------------------------------------------
    engine.run();
    engine.waitForCompletion();

    // -----------------------------------------------------------------------
    // 8. (Optional) Run a second time to demonstrate re-use
    // -----------------------------------------------------------------------
    std::cout << "\n[main] --- Second run ---\n";
    engine.run();
    engine.waitForCompletion();

    // -----------------------------------------------------------------------
    // 9. Print cumulative metrics across both runs
    // -----------------------------------------------------------------------
    std::cout << "\n[main] Cumulative metrics across all runs:\n";
    engine.getMetrics().print();

    // -----------------------------------------------------------------------
    // 10. Honour the STOP command if it was received.
    //     Stop the control plane thread before exiting.
    //     遵从收到的 STOP 命令；退出前停止控制平面线程。
    // -----------------------------------------------------------------------
    if (engine_stop_requested.load(std::memory_order_acquire)) {
        std::cout << "\n[main] STOP command received — shutting down.\n";
    }

    control_thread.request_stop();
    control_thread.join();

    std::cout << "\n[main] Done.\n";
    // ConfigWatcher destructor stops the inotify thread gracefully.
    // ThreadPool destructor drains remaining tasks and joins workers.
    return 0;
}
