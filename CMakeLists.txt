cmake_minimum_required(VERSION 3.20)
project(cpp20-workflow-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Place the binary and shared libraries in the build root so that
# workflow.json ("plugin": "libexample_plugin.so") is found by relative path
# when the demo is run from the build directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---- common compiler flags --------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

# ============================================================================
# Boost — networking (Asio), concurrency (thread_pool), and containers
# ============================================================================
# Boost.Asio is header-only; Boost.Container is header-only.
# We link Boost::system (Asio error codes) and Boost::thread (thread_pool
# internals).  All components share a single Boost include root.
# ============================================================================
find_package(Boost 1.74 REQUIRED COMPONENTS system thread)

# ============================================================================
# Third-party dependencies via FetchContent
# ============================================================================

include(FetchContent)

# ---- nlohmann/json ----------------------------------------------------------
# Header-only JSON library used by GraphBuilder.
# GraphBuilder 使用的纯头文件 JSON 库。
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
# Only build the header-only interface; skip tests and benchmarks.
# 仅构建纯头文件接口，跳过测试和基准测试。
set(JSON_BuildTests   OFF CACHE INTERNAL "")
set(JSON_Install      OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ---- taskflow ---------------------------------------------------------------
# Header-only task parallelism library used by GraphBuilder.
# GraphBuilder 使用的纯头文件任务并行库。
FetchContent_Declare(
    taskflow
    GIT_REPOSITORY https://github.com/taskflow/taskflow.git
    GIT_TAG        v3.7.0
    GIT_SHALLOW    TRUE
)
# Skip Taskflow's own examples and tests to keep build times short.
# 跳过 Taskflow 自带的示例和测试，保持构建时间短暂。
set(TF_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(TF_BUILD_TESTS    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(taskflow)

# ============================================================================
# Subdirectories
# ============================================================================

# core/ — ControlPlane, PluginRegistry, GraphBuilder static library
add_subdirectory(core)

# tools/ — workflow-cli CLI client
add_subdirectory(tools)

# examples/ — json_taskflow_demo
add_subdirectory(examples)

# tests/ — Google Test unit tests
enable_testing()
add_subdirectory(tests)

# ---- main executable --------------------------------------------------------
add_executable(workflow-engine
    src/main.cpp
    src/ConfigWatcher.cpp
    src/ThreadPool.cpp
    src/PluginManager.cpp
    src/WorkflowEngine.cpp
    src/ExecutionContext.cpp
)

target_include_directories(workflow-engine PRIVATE include)

target_link_libraries(workflow-engine
    PRIVATE
        workflow-core  # ControlPlane, PluginRegistry, GraphBuilder
        Boost::system  # Boost.Asio error codes
        Boost::thread  # boost::asio::thread_pool internals
        dl             # dlopen / dlsym / dlclose
        pthread        # std::jthread / std::thread internals
)

# Export all symbols from the main binary so that plugins loaded via dlopen
# can resolve the extern "C" ctx_* functions that live in the main binary.
# 导出主程序的所有符号，使通过 dlopen 加载的插件能够解析主程序中的 extern "C" ctx_* 函数。
target_link_options(workflow-engine PRIVATE -rdynamic)

# ---- example plugin shared library ------------------------------------------
add_library(example_plugin SHARED plugins/example_plugin.cpp)

target_include_directories(example_plugin PRIVATE include)

# Export all symbols so the engine can find create_node / destroy_node
set_target_properties(example_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ---- llm text analyzer plugin ------------------------------------------------
add_library(llm_text_analyzer_plugin SHARED plugins/llm_text_analyzer_plugin.cpp)
target_include_directories(llm_text_analyzer_plugin PRIVATE include)
target_link_libraries(llm_text_analyzer_plugin PRIVATE ssl crypto)
set_target_properties(llm_text_analyzer_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ---- llm audio editor plugin -------------------------------------------------
add_library(llm_audio_editor_plugin SHARED plugins/llm_audio_editor_plugin.cpp)
target_include_directories(llm_audio_editor_plugin PRIVATE include)
set_target_properties(llm_audio_editor_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
