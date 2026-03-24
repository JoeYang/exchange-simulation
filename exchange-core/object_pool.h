#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace exchange {

// ObjectPool<T, Capacity> — fixed-capacity pool with O(1) allocate/deallocate.
//
// Storage is a raw byte array sized for `Capacity` objects of type T. The free
// list is threaded through that storage: when a slot is free, its first
// sizeof(void*) bytes hold a pointer to the next free slot (via memcpy so no
// alignment UB or class-memaccess warnings fire). This requires
// sizeof(T) >= sizeof(T*), checked via static_assert.
//
// The pool itself is zero-heap: it may live on the stack or as a member of
// another struct. allocate() returns a raw pointer into the storage — callers
// that need a constructed object should placement-new into it. For trivial
// types the pointer may be used directly.
//
// All methods are noexcept. allocate() returns nullptr when exhausted.
template <typename T, std::size_t Capacity>
class ObjectPool {
    static_assert(Capacity > 0, "ObjectPool: Capacity must be > 0");
    static_assert(sizeof(T) >= sizeof(void*),
                  "ObjectPool: sizeof(T) must be >= sizeof(void*) to thread the free list");
    static_assert(std::is_trivially_destructible_v<T>,
        "ObjectPool: T must be trivially destructible; reset() cannot run destructors.");

public:
    ObjectPool() noexcept {
        build_free_list();
    }

    // Non-copyable, non-movable — the pool owns its raw storage.
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    // Pop a slot from the free list and return a pointer to its raw storage.
    // Returns nullptr when the pool is exhausted.
    // The caller is responsible for constructing a T at the returned address
    // (placement-new) if T is non-trivial.
    T* allocate() noexcept {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        void* slot = free_head_;
        void* next = nullptr;
        std::memcpy(&next, slot, sizeof(void*));
        free_head_ = static_cast<unsigned char*>(next);
        ++allocated_;
        return static_cast<T*>(slot);
    }

    // Push a previously allocated slot back onto the free list.
    // Undefined behaviour if p was not obtained from this pool, or if p is
    // pushed more than once without an intervening allocate().
    void deallocate(T* p) noexcept {
        void* old_head = free_head_;
        std::memcpy(static_cast<void*>(p), &old_head, sizeof(void*));
        free_head_ = static_cast<unsigned char*>(static_cast<void*>(p));
        --allocated_;
    }

    // Return all slots to the free list without running any destructors.
    // Callers must destroy live objects before calling reset() when T has a
    // non-trivial destructor.
    void reset() noexcept {
        allocated_ = 0;
        build_free_list();
    }

    std::size_t available() const noexcept {
        return Capacity - allocated_;
    }

    std::size_t capacity() const noexcept {
        return Capacity;
    }

private:
    // Thread the next-pointer through each slot using memcpy so that:
    //  - We do not violate strict aliasing (raw bytes, not T objects).
    //  - -Werror=class-memaccess does not fire (src/dst are void*).
    void build_free_list() noexcept {
        for (std::size_t i = 0; i + 1 < Capacity; ++i) {
            void* next = slot_ptr(i + 1);
            std::memcpy(slot_ptr(i), &next, sizeof(void*));
        }
        void* null_ptr = nullptr;
        std::memcpy(slot_ptr(Capacity - 1), &null_ptr, sizeof(void*));
        free_head_ = static_cast<unsigned char*>(slot_ptr(0));
    }

    void* slot_ptr(std::size_t idx) noexcept {
        return static_cast<void*>(&storage_[idx * slot_size_]);
    }

    // sizeof(T) is always a multiple of alignof(T) per C++ standard [basic.align],
    // so sizeof(T) is the correct stride between slots.
    static constexpr std::size_t slot_size_ = sizeof(T);

    alignas(T) std::array<unsigned char, Capacity * slot_size_> storage_;
    unsigned char* free_head_{nullptr};
    std::size_t    allocated_{0};
};

}  // namespace exchange
