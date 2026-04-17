#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>

/**
 * Engine-wide atomic performance counters.
 *
 * Each counter is padded to occupy exactly one 64-byte hardware cache line.
 * This prevents false sharing: worker threads that concurrently increment
 * different counters will never invalidate each other's cache lines.
 *
 * All increments use std::memory_order_relaxed because the only ordering
 * requirement is that each add is atomic — no happens-before relationship
 * with other variables is needed.
 *
 * 引擎全局原子性能计数器。
 * 每个计数器独占一条 64 字节缓存行，防止并发更新不同计数器时产生伪共享。
 * 所有累加操作使用 memory_order_relaxed，满足原子性即可。
 */
struct EngineMetrics {
    static constexpr std::size_t kCacheLineSize = 64;

    /**
     * One atomic uint64_t padded to a full cache line.
     * 填充至一整条缓存行的单个原子 uint64_t 计数器。
     */
    struct alignas(kCacheLineSize) Counter {
        std::atomic<uint64_t> value{0};
        // Pad to exactly kCacheLineSize bytes so adjacent counters in this
        // struct each live on distinct cache lines.
        char _padding[kCacheLineSize - sizeof(std::atomic<uint64_t>)]{};

        void add(uint64_t n = 1) noexcept {
            value.fetch_add(n, std::memory_order_relaxed);
        }

        [[nodiscard]] uint64_t load() const noexcept {
            return value.load(std::memory_order_relaxed);
        }
    };

    static_assert(sizeof(Counter) == kCacheLineSize,
                  "Counter must be exactly one cache line (64 bytes)");

    Counter success_count;    ///< Nodes that completed successfully
    Counter failed_count;     ///< Nodes that exhausted all retries
    Counter cancelled_count;  ///< Nodes cancelled due to upstream failure
    Counter hot_reload_count; ///< Number of hot-reload events triggered

    /** Convenience increment helpers. */
    void recordSuccess()    noexcept { success_count.add(); }
    void recordFailure()    noexcept { failed_count.add(); }
    void recordCancelled()  noexcept { cancelled_count.add(); }
    void recordHotReload()  noexcept { hot_reload_count.add(); }

    /** Print a human-readable summary to stdout. */
    void print() const {
        std::cout << "[Metrics] success="    << success_count.load()
                  << "  failed="            << failed_count.load()
                  << "  cancelled="         << cancelled_count.load()
                  << "  hot_reloads="       << hot_reload_count.load()
                  << '\n';
    }
};
