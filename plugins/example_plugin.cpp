#include "ExecutionContext.h"
#include "WorkflowNode.h"

#include <cstdint>
#include <iostream>
#include <thread>

/**
 * A minimal example plugin demonstrating ABI-safe ExecutionContext usage.
 *
 * ABI SAFETY RULE / ABI 安全规则:
 *   This .so is compiled as a separate translation unit.  Directly touching
 *   ExecutionContext's C++ members (set/get/has/remove) would be unsafe
 *   because std::any typeinfo and allocator addresses may differ from the
 *   main binary.
 *
 *   此 .so 作为独立编译单元编译。直接调用 ExecutionContext 的 C++ 成员
 *   (set/get/has/remove) 是不安全的，因为 std::any 的 typeinfo 和分配器
 *   地址可能与主程序不同。
 *
 *   SOLUTION: cast &ctx to void* and use the extern "C" ctx_* helpers.
 *   These functions live in the main binary where all allocations happen.
 *
 *   解决方案：将 &ctx 转换为 void*，使用 extern "C" ctx_* 辅助函数。
 *   这些函数位于主程序中，所有内存分配均在那里完成。
 */
class ExampleNode : public WorkflowNode {
public:
    void execute(ExecutionContext& ctx) override {
        void* ctx_ptr = static_cast<void*>(&ctx);

        // ----------------------------------------------------------------
        // Write data into the shared context via the C interface.
        // 通过 C 接口将数据写入共享上下文。
        // ----------------------------------------------------------------
        ctx_set_string(ctx_ptr, "plugin_status", "running");
        ctx_set_int64 (ctx_ptr, "processed_items", 42);
        ctx_set_double(ctx_ptr, "score", 0.95);

        std::cout << "[ExampleNode] thread=" << std::this_thread::get_id()
                  << "  wrote plugin_status/processed_items/score to context\n";

        // ----------------------------------------------------------------
        // Read back data to verify round-trip correctness.
        // 读回数据以验证往返正确性。
        // ----------------------------------------------------------------
        char   status_buf[64] = {};
        int64_t processed_items = 0;
        double  score           = 0.0;

        if (ctx_get_string(ctx_ptr, "plugin_status", status_buf, sizeof(status_buf)) >= 0 &&
            ctx_get_int64 (ctx_ptr, "processed_items", &processed_items) == 0 &&
            ctx_get_double(ctx_ptr, "score", &score) == 0)
        {
            std::cout << "[ExampleNode] context read-back:"
                      << "  status='"    << status_buf << "'"
                      << "  items="      << processed_items
                      << "  score="      << score << '\n';
        }
    }
};

// ---- C factory / destructor -------------------------------------------------

extern "C" WorkflowNode* create_node() {
    return new ExampleNode();
}

extern "C" void destroy_node(WorkflowNode* node) {
    delete node;
}
