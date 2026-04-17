cmake_minimum_required(VERSION 3.20)

# ============================================================================
# workflow-cli — standalone CLI client for the control plane
# ============================================================================

add_executable(workflow-cli workflow-cli.cpp)

target_compile_features(workflow-cli PRIVATE cxx_std_20)

# workflow-cli uses only Linux socket APIs — no extra link dependencies needed.
# workflow-cli 仅使用 Linux socket API，无需额外链接依赖。
