cmake_minimum_required(VERSION 3.20)

# ============================================================================
# json_taskflow_demo — JSON-driven Taskflow graph example
# ============================================================================

add_executable(json-taskflow-demo json_taskflow_demo.cpp)

target_compile_features(json-taskflow-demo PRIVATE cxx_std_20)

# Include both the project include/ dir (for WorkflowNode.h, ExecutionContext.h)
# and core/ (for GraphBuilder.h, PluginRegistry.h).
# 同时包含项目 include/ 目录（WorkflowNode.h、ExecutionContext.h）
# 和 core/ 目录（GraphBuilder.h、PluginRegistry.h）。
target_include_directories(json-taskflow-demo
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/core
)

target_link_libraries(json-taskflow-demo
    PRIVATE
        workflow-core          # GraphBuilder, PluginRegistry, ControlPlane
        nlohmann_json::nlohmann_json
        Taskflow
)
