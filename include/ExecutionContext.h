#pragma once

// ============================================================================
// ExecutionContext — thread-safe, ABI-safe shared data store for DAG nodes
//
// ExecutionContext（线程安全、ABI 安全的 DAG 节点共享数据存储）
// ============================================================================
//
// PURPOSE / 设计目的
// ------------------
// Different workflow nodes (potentially loaded from separate .so plugins) need
// a way to share intermediate results without unsafe direct C++ object passing
// across shared-library boundaries.
// 不同的工作流节点（可能来自不同的 .so 插件）需要一种方式共享中间结果，
// 同时避免跨共享库边界传递不安全的 C++ 对象。
//
// ABI SAFETY / ABI 安全
// ----------------------
// Passing std::any, std::string, or other complex C++ types across .so
// boundaries is dangerous because:
//   1. typeinfo addresses may differ between compilation units, breaking
//      std::any_cast and dynamic_cast.
//   2. Different allocators in each .so can cause heap corruption on free().
//
// 跨 .so 边界传递 std::any、std::string 等复杂 C++ 类型存在危险，原因：
//   1. 不同编译单元的 typeinfo 地址可能不同，导致 any_cast/dynamic_cast 失败。
//   2. 不同 .so 中的分配器不同，跨边界释放内存会引发堆损坏。
//
// SOLUTION / 解决方案
// --------------------
// All memory allocation and deallocation happens inside ExecutionContext, which
// lives in the main executable.  Plugins MUST NOT construct or destroy C++
// objects in the context directly.  Instead, they call the extern "C" helper
// functions declared at the bottom of this header.  Those functions accept only
// POD types (const char*, int64_t, double) at the boundary and forward to the
// C++ implementation inside the main binary.
//
// 所有内存分配和释放都在 ExecutionContext 内部完成（位于主程序）。
// 插件禁止直接构造或销毁 Context 中的 C++ 对象，必须调用本头文件末尾声明的
// extern "C" 辅助函数。这些函数在边界处只接受 POD 类型（const char*、int64_t、
// double），然后在主程序侧转换为 C++ 类型。
//
// MEMORY OWNERSHIP / 内存所有权
// --------------------------------
// The WorkflowEngine owns the single ExecutionContext instance for each run.
// Nodes receive a reference; they do not own it and must not delete it.
//
// WorkflowEngine 拥有每次运行的唯一 ExecutionContext 实例。
// 节点持有的是引用，不拥有所有权，禁止删除。
//
// USAGE FOR PLUGINS / 插件使用规范
// ----------------------------------
// Plugin code (in a .so) should use only the extern "C" functions:
//
//   void* ctx_ptr = static_cast<void*>(&ctx);   // receive ctx from execute()
//   ctx_set_string(ctx_ptr, "result", "done");
//
//   char buf[256];
//   if (ctx_get_string(ctx_ptr, "input", buf, sizeof(buf)) >= 0)
//       // use buf
//
// DO NOT do:  ctx.set("key", std::string("value"));  // unsafe across .so!
// ============================================================================

#include <any>
#include <boost/container/flat_map.hpp>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>

class ExecutionContext {
public:
    ExecutionContext()  = default;
    ~ExecutionContext() = default;

    // Non-copyable, non-movable — the engine holds one authoritative instance.
    // 不可拷贝、不可移动 —— 引擎持有唯一权威实例。
    ExecutionContext(const ExecutionContext&)            = delete;
    ExecutionContext& operator=(const ExecutionContext&) = delete;
    ExecutionContext(ExecutionContext&&)                 = delete;
    ExecutionContext& operator=(ExecutionContext&&)      = delete;

    // -----------------------------------------------------------------------
    // set — write (or overwrite) a value for the given key
    // set — 写入（或覆盖）指定 key 的值
    //
    // Uses std::unique_lock for exclusive write access.
    // 使用 unique_lock 进行独占写操作。
    // -----------------------------------------------------------------------
    void set(const std::string& key, std::any value);

    // -----------------------------------------------------------------------
    // get<T> — read a typed value; returns std::nullopt on miss or type error
    // get<T> — 读取有类型的值；key 不存在或类型不符时返回 std::nullopt
    //
    // Why shared_mutex / 为什么使用 shared_mutex：
    //   Workflow nodes run concurrently on multiple threads.  Most operations
    //   are reads (nodes checking upstream results).  shared_mutex allows N
    //   concurrent readers with zero contention, while writes are still
    //   exclusive.  A plain mutex would serialise all readers unnecessarily.
    //
    //   工作流节点并发运行，大多数操作为读（节点读取上游结果）。
    //   shared_mutex 允许 N 个读者并发访问，只有写操作互斥。
    //   普通 mutex 会不必要地序列化所有读者。
    // -----------------------------------------------------------------------
    template <typename T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const {
        std::shared_lock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return std::nullopt;
        const T* ptr = std::any_cast<T>(&it->second);
        if (!ptr) return std::nullopt;
        return *ptr;
    }

    // -----------------------------------------------------------------------
    // has — return true if the key exists
    // has — key 存在时返回 true
    // -----------------------------------------------------------------------
    [[nodiscard]] bool has(const std::string& key) const;

    // -----------------------------------------------------------------------
    // remove — erase a key from the store (no-op if absent)
    // remove — 从存储中删除 key（不存在时不做任何操作）
    // -----------------------------------------------------------------------
    void remove(const std::string& key);

    // -----------------------------------------------------------------------
    // clear — reset the entire context (called between runs by the engine)
    // clear — 清空整个 context（引擎在每次运行之间调用）
    // -----------------------------------------------------------------------
    void clear();

private:
    // mutable so that const get() can acquire a shared lock.
    // mutable 使 const 的 get() 方法也能获取共享锁。
    mutable std::shared_mutex mutex_;

    // boost::container::flat_map stores key-value pairs in a contiguous sorted
    // array.  For workflow contexts with O(10–100) keys the cache-friendly
    // layout outperforms std::unordered_map despite O(log n) vs O(1) lookups,
    // because sequential access patterns dominate.
    //
    // boost::container::flat_map 将键值对存储在连续排序数组中。
    // 对于拥有 O(10–100) 个键的工作流上下文，连续内存布局在实践中优于
    // std::unordered_map，因为顺序访问模式占主导，缓存命中率更高。
    boost::container::flat_map<std::string, std::any> data_;
};

// ============================================================================
// ABI-safe C interface — for use by plugin .so files ONLY
//
// ABI 安全 C 接口 —— 仅供插件 .so 文件使用
//
// All functions take `void* ctx` which must be a valid ExecutionContext*.
// The cast back to ExecutionContext* happens inside the main binary where the
// class layout is definitively known.
//
// 所有函数接收 `void* ctx`，必须是有效的 ExecutionContext* 指针。
// 转换回 ExecutionContext* 发生在主程序内部，此处类布局是确定已知的。
// ============================================================================

extern "C" {

// ---- string values ----------------------------------------------------------

/**
 * Store a NUL-terminated C string in the context.
 * Internally converted to std::string on the main-binary side.
 *
 * 在 context 中存储一个 NUL 结尾的 C 字符串。
 * 在主程序侧内部转换为 std::string。
 */
void ctx_set_string(void* ctx, const char* key, const char* value);

/**
 * Retrieve a string value into caller-provided buffer.
 * Returns the number of characters written (excluding NUL) on success,
 * or -1 if the key does not exist or does not hold a std::string.
 * The buffer is always NUL-terminated when buf_len > 0.
 *
 * 将字符串值读入调用方提供的缓冲区。
 * 成功时返回写入的字符数（不含 NUL），key 不存在或类型不符时返回 -1。
 * 当 buf_len > 0 时，缓冲区始终以 NUL 结尾。
 */
int ctx_get_string(void* ctx, const char* key, char* buf, int buf_len);

// ---- int64 values -----------------------------------------------------------

/** Store a 64-bit signed integer. / 存储 64 位有符号整数。 */
void ctx_set_int64(void* ctx, const char* key, int64_t value);

/**
 * Retrieve a 64-bit integer into *out.
 * Returns 0 on success, -1 if key is absent or wrong type.
 *
 * 将 64 位整数写入 *out。
 * 成功返回 0，key 不存在或类型不符返回 -1。
 */
int ctx_get_int64(void* ctx, const char* key, int64_t* out);

// ---- double values ----------------------------------------------------------

/** Store a double-precision float. / 存储双精度浮点数。 */
void ctx_set_double(void* ctx, const char* key, double value);

/**
 * Retrieve a double into *out.
 * Returns 0 on success, -1 if key is absent or wrong type.
 *
 * 将双精度浮点数写入 *out。
 * 成功返回 0，key 不存在或类型不符返回 -1。
 */
int ctx_get_double(void* ctx, const char* key, double* out);

// ---- presence / removal -----------------------------------------------------

/**
 * Returns 1 if the key exists in the context, 0 otherwise.
 * key 存在于 context 中时返回 1，否则返回 0。
 */
int ctx_has(void* ctx, const char* key);

/**
 * Remove the key from the context (no-op if absent).
 * 从 context 中删除 key（不存在时不做任何操作）。
 */
void ctx_remove(void* ctx, const char* key);

} // extern "C"
