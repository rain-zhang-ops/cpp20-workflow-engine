#include "ConfigWatcher.h"
#include "PluginManager.h"
#include "ThreadPool.h"
#include "WorkflowEngine.h"

// Control plane (UDS daemon thread)
#include "ControlPlane.h"

#include <atomic>
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
    // -----------------------------------------------------------------------
    ConfigWatcher watcher(config_path, [&engine, &config_path]() {
        engine.onConfigChanged(config_path);
    });
    std::cout << "[main] ConfigWatcher started for: " << config_path << "\n\n";

    // -----------------------------------------------------------------------
    // 6. Control plane — UDS daemon for remote CLI control
    // -----------------------------------------------------------------------
    std::atomic<bool> engine_stop_requested{false};

    ControlPlane control_plane;

    control_plane.register_command("RELOAD", [&engine, &config_path]() -> std::string {
        try {
            engine.onConfigChanged(config_path);
            return "OK: reloaded config from " + config_path;
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
    // 7. Daemon mode — block main thread indefinitely
    //    DAG execution is now triggered exclusively via CLI commands.
    // -----------------------------------------------------------------------
    std::cout << "[main] Engine is running in daemon mode. "
                 "Waiting for CLI commands via socket...\n";

    std::promise<void>().get_future().wait();

    // --- Graceful shutdown (reached only if promise is fulfilled) ----------
    control_thread.request_stop();
    control_thread.join();

    std::cout << "\n[main] Done.\n";
    return 0;
}