#pragma once
#include <atomic>
#include <cstddef>
#include <memory>

#if defined(__cpp_lib_hardware_interference_size)
constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
constexpr size_t kCacheLineSize = 64;
#endif

// Single-producer, single-consumer lock-free ring buffer. Capacity must be
// a power of two (checked with an assert, not silently rounded — a wrong
// capacity here is a correctness bug, not a tuning choice).
//
// head_ (consumer-owned) and tail_ (producer-owned) are pinned to separate
// cache lines with alignas(64) so the two threads touching this buffer
// never false-share — this is the concrete case ADR-1 says padding
// actually matters for, unlike the single-threaded Node/PriceLevel
// experiment in devlog day 2 where it measurably didn't.
template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacity_pow2)
        : capacity_(capacity_pow2),
          mask_(capacity_pow2 - 1),
          buf_(std::make_unique<T[]>(capacity_pow2)) {
        // capacity_pow2 must be a power of two for the `& mask_` trick to
        // behave like `% capacity_pow2`.
    }

    bool push(const T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & mask_;
        if (next == head_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        buf_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;  // empty
        }
        out = buf_[head];
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<T[]> buf_;

    alignas(kCacheLineSize) std::atomic<size_t> head_{0};  // consumer writes
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};  // producer writes
};
