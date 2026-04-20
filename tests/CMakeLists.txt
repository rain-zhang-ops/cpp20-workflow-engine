cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Google Test via FetchContent
# ============================================================================
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)
# Prevent GTest from overriding our compiler/linker settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ============================================================================
# Test executable
# ============================================================================
add_executable(workflow-tests
    test_execution_context.cpp
    test_lock_free_ring_buffer.cpp
    test_plugin_registry.cpp
    test_thread_pool.cpp
    test_workflow_engine.cpp
    # Engine sources (not in a library, so we compile them directly)
    ${CMAKE_SOURCE_DIR}/src/ExecutionContext.cpp
    ${CMAKE_SOURCE_DIR}/src/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/WorkflowEngine.cpp
)

target_include_directories(workflow-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/core
)

target_link_libraries(workflow-tests
    PRIVATE
        GTest::gtest_main
        workflow-core
        dl
        pthread
)

# Register all test cases with CTest.
include(GoogleTest)
gtest_discover_tests(workflow-tests)
