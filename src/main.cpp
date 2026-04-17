#include "ConfigWatcher.h"
#include "PluginManager.h"
#include "ThreadPool.h"
#include "WorkflowEngine.h"

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

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
    // 6. Run the DAG once and wait for completion
    // -----------------------------------------------------------------------
    engine.run();
    engine.waitForCompletion();

    // -----------------------------------------------------------------------
    // 7. (Optional) Run a second time to demonstrate re-use
    // -----------------------------------------------------------------------
    std::cout << "\n[main] --- Second run ---\n";
    engine.run();
    engine.waitForCompletion();

    // -----------------------------------------------------------------------
    // 8. Print cumulative metrics across both runs
    // -----------------------------------------------------------------------
    std::cout << "\n[main] Cumulative metrics across all runs:\n";
    engine.getMetrics().print();

    std::cout << "\n[main] Done.\n";
    // ConfigWatcher destructor stops the inotify thread gracefully.
    // ThreadPool destructor drains remaining tasks and joins workers.
    return 0;
}
