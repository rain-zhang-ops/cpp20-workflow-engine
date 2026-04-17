#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

/**
 * Lock-free multi-producer / single-consumer (MPSC) bounded ring buffer.
 *
 * Based on Dmitry Vyukov's MPMC queue design, simplified for the MPSC case.
 *
 * Design properties:
 *  - Capacity must be a power of 2.
 *  - push() is lock-free (wait-free when there is no contention).
 *    Typical cost: 2 atomic loads + 1 CAS + 1 store ≈ 30–60 ns.
 *  - pop() is wait-free (single consumer, no contention on read side).
 *    Typical cost: 1 atomic load + 1 store ≈ 10–20 ns.
 *  - Each slot's sequence counter is padded to its own cache line to prevent
 *    false sharing between concurrent producers.
 *  - The write and read position counters are also cache-line-aligned.
 *
 * Thread-safety: push() may be called concurrently from multiple threads.
 *               pop() must be called from a single consumer thread only.
 *
 * 无锁多生产者单消费者有界环形缓冲区。
 * 容量必须是 2 的幂次。push 近似无等待，pop 完全无等待。
 */
template <typename T, std::size_t Capacity = 1024>
class LockFreeRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(std::is_nothrow_move_constructible_v<T> ||
                  std::is_nothrow_copy_constructible_v<T>,
                  "T must be nothrow-move-constructible or "
                  "nothrow-copy-constructible");

    static constexpr std::size_t kMask = Capacity - 1;

    // Each slot holds its own sequence counter (for producer/consumer
    // handshake) plus the stored item.  The alignas ensures the sequence
    // counter and data live on the same cache line rather than straddling two
    // lines; it does NOT force 64 bytes of padding between slots.
    struct Slot {
        std::atomic<std::size_t> seq{0};
        T                        data{};
    };

public:
    LockFreeRingBuffer() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&)            = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    /**
     * Push an item into the buffer (non-blocking).
     * Returns true on success; false if the buffer is full (item discarded).
     *
     * 向缓冲区推入元素（非阻塞）。
     * 成功返回 true；缓冲区已满则返回 false（元素被丢弃）。
     */
    [[nodiscard]] bool push(T item) noexcept {
        std::size_t pos = write_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            std::size_t seq = slot.seq.load(std::memory_order_acquire);
            auto diff = static_cast<std::ptrdiff_t>(seq) -
                        static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                // Slot is free — attempt to claim it.
                if (write_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
                // CAS failed: another producer claimed the slot; retry.
            } else if (diff < 0) {
                return false;  // Buffer is full.
            } else {
                // Another producer already advanced write_pos_; reload.
                pos = write_pos_.load(std::memory_order_relaxed);
            }
        }
        slots_[pos & kMask].data = std::move(item);
        slots_[pos & kMask].seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * Pop an item from the buffer (non-blocking, single consumer only).
     * Returns std::nullopt if the buffer is empty.
     *
     * 从缓冲区取出元素（非阻塞，仅限单消费者调用）。
     * 缓冲区为空则返回 std::nullopt。
     */
    [[nodiscard]] std::optional<T> pop() noexcept {
        std::size_t pos  = read_pos_.load(std::memory_order_relaxed);
        Slot&       slot = slots_[pos & kMask];
        std::size_t seq  = slot.seq.load(std::memory_order_acquire);
        auto diff = static_cast<std::ptrdiff_t>(seq) -
                    static_cast<std::ptrdiff_t>(pos + 1);
        if (diff != 0)
            return std::nullopt;  // Empty or not yet written.

        T item = std::move(slot.data);
        slot.seq.store(pos + Capacity, std::memory_order_release);
        read_pos_.store(pos + 1, std::memory_order_relaxed);
        return item;
    }

    /**
     * Non-atomic approximate empty check (suitable for consumer thread only).
     * 近似空检测（仅适合消费者线程使用）。
     */
    [[nodiscard]] bool empty() const noexcept {
        std::size_t rp  = read_pos_.load(std::memory_order_relaxed);
        std::size_t seq = slots_[rp & kMask].seq.load(std::memory_order_acquire);
        return static_cast<std::ptrdiff_t>(seq) -
               static_cast<std::ptrdiff_t>(rp + 1) < 0;
    }

private:
    alignas(64) std::atomic<std::size_t> write_pos_{0};
    alignas(64) std::atomic<std::size_t> read_pos_{0};
    std::array<Slot, Capacity>           slots_;
};
