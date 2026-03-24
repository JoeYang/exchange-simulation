#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace exchange {

// Lock-free single-producer single-consumer ring buffer.
//
// Key properties:
//   - Lock-free: uses std::atomic with acquire/release semantics, no mutexes.
//   - Cache-friendly: write_pos_ and read_pos_ occupy separate cache lines to
//     avoid false sharing between the producer and consumer threads.
//   - Power-of-2 capacity: enables bitwise modulo (pos & kMask) instead of
//     division, eliminating a branch on every push/pop.
//   - Fixed-size: entire buffer pre-allocated at construction; zero heap
//     allocation after the object is created.
//   - Single producer, single consumer: no CAS loops needed — just
//     load/store with appropriate memory ordering.
//
// Safety: try_push must only be called from the producer thread.
//         try_pop  must only be called from the consumer thread.
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity > 0, "Capacity must be positive");

    static constexpr size_t kMask = Capacity - 1;

    // Each atomic lives on its own cache line to prevent false sharing.
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
    alignas(64) std::array<T, Capacity> buffer_{};

public:
    // Attempt to enqueue item.  Returns false (without modifying the buffer)
    // if the buffer is full.  Must be called only by the producer thread.
    bool try_push(const T& item) {
        const size_t wp = write_pos_.load(std::memory_order_relaxed);
        const size_t rp = read_pos_.load(std::memory_order_acquire);
        if (wp - rp >= Capacity) {
            return false;  // full
        }
        buffer_[wp & kMask] = item;
        write_pos_.store(wp + 1, std::memory_order_release);
        return true;
    }

    // Attempt to dequeue into item.  Returns false (leaving item unchanged)
    // if the buffer is empty.  Must be called only by the consumer thread.
    bool try_pop(T& item) {
        const size_t rp = read_pos_.load(std::memory_order_relaxed);
        const size_t wp = write_pos_.load(std::memory_order_acquire);
        if (rp == wp) {
            return false;  // empty
        }
        item = buffer_[rp & kMask];
        read_pos_.store(rp + 1, std::memory_order_release);
        return true;
    }

    // Number of items currently in the buffer.
    // May be called from either thread; result is approximate when called
    // concurrently, but always within [0, Capacity].
    [[nodiscard]] size_t size() const {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] bool full() const { return size() >= Capacity; }
};

}  // namespace exchange
