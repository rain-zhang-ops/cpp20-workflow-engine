#include "ExecutionContext.h"

#include <algorithm>
#include <cstring>
#include <mutex>

// ============================================================================
// ExecutionContext — C++ method implementations
// ============================================================================

void ExecutionContext::set(const std::string& key, std::any value)
{
    std::unique_lock lock(mutex_);
    data_.insert_or_assign(key, std::move(value));
}

bool ExecutionContext::has(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return data_.find(key) != data_.end();
}

void ExecutionContext::remove(const std::string& key)
{
    std::unique_lock lock(mutex_);
    data_.erase(key);
}

void ExecutionContext::clear()
{
    std::unique_lock lock(mutex_);
    data_.clear();
}

// ============================================================================
// ABI-safe C interface implementations
//
// Each function casts the opaque void* back to ExecutionContext*.  This cast
// is safe because the main binary is the only place that allocates the object,
// so the class layout is definitively known here.
//
// 每个函数将不透明的 void* 转换回 ExecutionContext*。
// 这个转换是安全的，因为只有主程序分配该对象，此处类布局确定已知。
// ============================================================================

static inline ExecutionContext* to_ctx(void* p)
{
    return static_cast<ExecutionContext*>(p);
}

// ---- string -----------------------------------------------------------------

extern "C" void ctx_set_string(void* ctx, const char* key, const char* value)
{
    if (!ctx || !key || !value) return;
    to_ctx(ctx)->set(std::string(key), std::string(value));
}

extern "C" int ctx_get_string(void* ctx, const char* key, char* buf, int buf_len)
{
    if (!ctx || !key || !buf || buf_len <= 0) return -1;

    auto opt = to_ctx(ctx)->get<std::string>(std::string(key));
    if (!opt) return -1;

    const std::string& s   = *opt;
    int                len = static_cast<int>(s.size());
    int                n   = std::min(len, buf_len - 1);
    std::memcpy(buf, s.data(), static_cast<std::size_t>(n));
    buf[n] = '\0';
    return n;
}

// ---- int64 ------------------------------------------------------------------

extern "C" void ctx_set_int64(void* ctx, const char* key, int64_t value)
{
    if (!ctx || !key) return;
    to_ctx(ctx)->set(std::string(key), value);
}

extern "C" int ctx_get_int64(void* ctx, const char* key, int64_t* out)
{
    if (!ctx || !key || !out) return -1;
    auto opt = to_ctx(ctx)->get<int64_t>(std::string(key));
    if (!opt) return -1;
    *out = *opt;
    return 0;
}

// ---- double -----------------------------------------------------------------

extern "C" void ctx_set_double(void* ctx, const char* key, double value)
{
    if (!ctx || !key) return;
    to_ctx(ctx)->set(std::string(key), value);
}

extern "C" int ctx_get_double(void* ctx, const char* key, double* out)
{
    if (!ctx || !key || !out) return -1;
    auto opt = to_ctx(ctx)->get<double>(std::string(key));
    if (!opt) return -1;
    *out = *opt;
    return 0;
}

// ---- presence / removal -----------------------------------------------------

extern "C" int ctx_has(void* ctx, const char* key)
{
    if (!ctx || !key) return 0;
    return to_ctx(ctx)->has(std::string(key)) ? 1 : 0;
}

extern "C" void ctx_remove(void* ctx, const char* key)
{
    if (!ctx || !key) return;
    to_ctx(ctx)->remove(std::string(key));
}
