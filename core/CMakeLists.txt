cmake_minimum_required(VERSION 3.20)

# ============================================================================
# workflow-core — static library with control plane and graph building modules
#
# workflow-core —— 包含控制平面和图构建模块的静态库
# ============================================================================
#
# Compiled sources:
#   ControlPlane.cpp  — UDS daemon thread (poll-based, RAII)
#   PluginRegistry.cpp — node type → factory function registry
#   GraphBuilder.cpp   — JSON → tf::Taskflow DAG builder
# ============================================================================

add_library(workflow-core STATIC
    ControlPlane.cpp
    PluginRegistry.cpp
    GraphBuilder.cpp
)

target_compile_features(workflow-core PUBLIC cxx_std_20)

# Headers in core/ and include/ are public so dependants get them automatically
# via target_link_libraries(... workflow-core).
# core/ 和 include/ 的头文件设为 PUBLIC，依赖方通过 target_link_libraries 自动获取。
target_include_directories(workflow-core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}       # core/*.h
        ${PROJECT_SOURCE_DIR}/include     # WorkflowNode.h, ExecutionContext.h, etc.
)

target_link_libraries(workflow-core
    PUBLIC
        nlohmann_json::nlohmann_json
        Taskflow
)
