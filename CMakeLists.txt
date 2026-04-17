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

# ---- main executable --------------------------------------------------------
add_executable(workflow-engine
    src/main.cpp
    src/ConfigWatcher.cpp
    src/ThreadPool.cpp
    src/PluginManager.cpp
    src/WorkflowEngine.cpp
)

target_include_directories(workflow-engine PRIVATE include)

target_link_libraries(workflow-engine
    PRIVATE
        dl        # dlopen / dlsym / dlclose
        pthread   # std::jthread / std::thread internals
)

# ---- example plugin shared library ------------------------------------------
add_library(example_plugin SHARED plugins/example_plugin.cpp)

target_include_directories(example_plugin PRIVATE include)

# Export all symbols so the engine can find create_node / destroy_node
set_target_properties(example_plugin PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)
