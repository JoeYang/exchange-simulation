# Exchange Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a high-performance C++ order matching engine with zero-allocation hot path, CRTP-based customization, and a deterministic journal-based test harness.

**Architecture:** Single orderbook per engine, single-threaded event loop. Intrusive data structures with pre-allocated object pools. Compile-time polymorphism via CRTP and template policies. Journal replay framework for deterministic testing.

**Tech Stack:** C++20, Bazel 9 (bzlmod), GoogleTest 1.17.0, FTXUI (visualization)

**Spec:** `docs/2026-03-24-exchange-core-design.md`

---

## Team & Workflow

```
                    Planner
                   /  |  |  \
               Dev1 Dev2 Dev3 Dev4
                   \  |  |  /
               Reviewer1  Reviewer2
```

- **4 devs** work in parallel using git worktrees for feature branches
- **2 reviewers** (one approval sufficient to merge)
- Sequential chains (e.g. Tasks 12->13->14) go to one dev in sequence
- Planner assigns tasks in real-time based on who finishes first
- Each task: branch from `main`, implement via TDD, pass review, rebase+merge, clean worktree

## Bazel 9 Conventions

Every `BUILD.bazel` file must begin with explicit `load()` statements:

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
```

Test deps always use `@googletest//:gtest_main`. All `#include` paths are workspace-relative (e.g. `#include "exchange-core/types.h"`).

## Dependency Graph

```
Group 1: [1] [2] [3] [4] [5] [6]                  <- all parallel, no deps
              |           |
Group 2: [7] [8] [9] [10] [11]                    <- all parallel, depend on G1
              |
Group 3: [12] --> [13] --> [14]                    <- sequential chain
                    |---> [15]                     <- cancel/modify after matching
                    |---> [16]                     <- TIF after post-match
          [17] [18]                                <- unit tests after 12-16

Group 4: [19] [20] [21] [22]                      <- all parallel, depend on G1 only
          [23]                                     <- after 19-22

Group 5: [24]-[30]                                 <- all parallel, depend on G3+G4
          [31]                                     <- after 24-30

Group 6: [32]                                      <- depends on G4 (recorded_event)
          [33]                                     <- depends on 32
          [34]                                     <- depends on 33 + G4 (journal_parser)
          [35]                                     <- depends on G1 (spsc_ring_buffer)
          [36]                                     <- depends on 35 + G1 (listeners)
          [37]                                     <- depends on 33 + 35
```

## Dev Assignment Strategy

| Phase | Dev 1 | Dev 2 | Dev 3 | Dev 4 |
|-------|-------|-------|-------|-------|
| G1 | Task 1 (types) | Task 2 (pool) | Task 3 (list) | Task 6 (spsc) |
| G1 cont. | Task 4 (events) | Task 5 (listeners) | -- | -- |
| G2 | Task 8 (orderbook) | Task 10 (FIFO) | Task 7 (composite) | Task 9 (stop book) |
| G2 cont. | -- | Task 11 (pro-rata) | -- | -- |
| G3 | Task 12->13->14 (engine seq.) | Task 19 (recorded event) | Task 21 (parser) | Task 22 (writer) |
| G3 cont. | Task 15 (cancel/modify) | Task 20 (recording listener) | -- | -- |
| G3 cont. | Task 16 (TIF/expiry) | Task 23 (test runner) | -- | -- |
| G3 cont. | Task 17 (engine tests core) | -- | -- | -- |
| G3 cont. | Task 18 (engine tests adv.) | -- | -- | -- |
| G5 | Task 24 | Task 25 | Task 26 | Task 27 |
| G5 cont. | Task 28 | Task 29 | Task 30 | -- |
| G5 cont. | Task 31 (integration) | -- | -- | -- |
| G6 | Task 32 | Task 35 | Task 33 (after 32) | Task 36 (after 35) |
| G6 cont. | Task 34 (after 33) | Task 37 (after 33+35) | -- | -- |

> **Note:** This is a starting plan. The planner reassigns dynamically as devs finish early.

---

## Group 1 -- Foundation (No Dependencies)

All 6 tasks can be done in parallel by 4 devs. Tasks 1, 2, 3, 6 start immediately; Tasks 4, 5 start as devs free up (or all 6 start if devs are fast enough since these are small).

---

### Task 1: Core Types, Enums, and Structs

**Group:** 1 | **Dependencies:** None | **Est. Lines:** ~120 | **Dev:** Any

**Files:**
- `exchange-core/types.h` (create)
- `exchange-core/types_test.cc` (create -- compile/sanity test)
- `exchange-core/BUILD.bazel` (modify -- add targets)

**BUILD.bazel changes:**

Add to `exchange-core/BUILD.bazel`:

```starlark
cc_library(
    name = "types",
    hdrs = ["types.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "types_test",
    srcs = ["types_test.cc"],
    deps = [
        ":types",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write header** `exchange-core/types.h`:

```cpp
#pragma once

#include <cstdint>

namespace exchange {

// --- Type aliases ---
using Price     = int64_t;   // fixed-point, 4 decimal places (100.5000 = 1005000)
using Quantity  = int64_t;   // fixed-point, 4 decimal places (1.0000 = 10000)
using OrderId   = uint64_t;  // engine-assigned, sequential starting at 1
using Timestamp = int64_t;   // epoch nanoseconds

constexpr int64_t PRICE_SCALE = 10000;

// --- Enumerations ---
enum class Side : uint8_t { Buy, Sell };

enum class OrderType : uint8_t { Limit, Market, Stop, StopLimit };

enum class TimeInForce : uint8_t { DAY, GTC, IOC, FOK, GTD };

enum class MatchAlgo : uint8_t { FIFO, ProRata };

enum class SmpAction : uint8_t {
    CancelNewest, CancelOldest, CancelBoth, Decrement, None
};

enum class ModifyPolicy : uint8_t {
    CancelReplace, AmendDown, RejectModify
};

enum class RejectReason : uint8_t {
    PoolExhausted, InvalidPrice, InvalidQuantity, InvalidTif, InvalidSide,
    UnknownOrder, PriceBandViolation, LevelPoolExhausted, ExchangeSpecific
};

enum class CancelReason : uint8_t {
    UserRequested, IOCRemainder, FOKFailed, Expired,
    SelfMatchPrevention, LevelPoolExhausted
};

// --- Core structs ---
struct PriceLevel;  // forward declaration

struct Order {
    OrderId id{0};
    uint64_t client_order_id{0};
    uint64_t account_id{0};
    Price price{0};
    Quantity quantity{0};
    Quantity filled_quantity{0};
    Quantity remaining_quantity{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Timestamp timestamp{0};
    Timestamp gtd_expiry{0};

    // Intrusive doubly-linked list hooks (within a price level)
    Order* prev{nullptr};
    Order* next{nullptr};

    // Back-pointer to owning price level
    PriceLevel* level{nullptr};
};

struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    uint32_t order_count{0};

    Order* head{nullptr};
    Order* tail{nullptr};

    // Intrusive doubly-linked list hooks (within bid/ask side)
    PriceLevel* prev{nullptr};
    PriceLevel* next{nullptr};
};

struct FillResult {
    Order* resting_order{nullptr};
    Price price{0};
    Quantity quantity{0};
    Quantity resting_remaining{0};
};

}  // namespace exchange
```

- [ ] **Write compile/sanity test** `exchange-core/types_test.cc`:

```cpp
#include "exchange-core/types.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

TEST(TypesTest, PriceScaleIsCorrect) {
    EXPECT_EQ(PRICE_SCALE, 10000);
}

TEST(TypesTest, SideEnumValues) {
    EXPECT_NE(Side::Buy, Side::Sell);
}

TEST(TypesTest, OrderTypeEnumValues) {
    EXPECT_NE(OrderType::Limit, OrderType::Market);
    EXPECT_NE(OrderType::Stop, OrderType::StopLimit);
}

TEST(TypesTest, OrderDefaultConstruction) {
    Order order{};
    EXPECT_EQ(order.id, 0u);
    EXPECT_EQ(order.price, 0);
    EXPECT_EQ(order.quantity, 0);
    EXPECT_EQ(order.filled_quantity, 0);
    EXPECT_EQ(order.remaining_quantity, 0);
    EXPECT_EQ(order.side, Side::Buy);
    EXPECT_EQ(order.type, OrderType::Limit);
    EXPECT_EQ(order.tif, TimeInForce::GTC);
    EXPECT_EQ(order.prev, nullptr);
    EXPECT_EQ(order.next, nullptr);
    EXPECT_EQ(order.level, nullptr);
}

TEST(TypesTest, PriceLevelDefaultConstruction) {
    PriceLevel level{};
    EXPECT_EQ(level.price, 0);
    EXPECT_EQ(level.total_quantity, 0);
    EXPECT_EQ(level.order_count, 0u);
    EXPECT_EQ(level.head, nullptr);
    EXPECT_EQ(level.tail, nullptr);
    EXPECT_EQ(level.prev, nullptr);
    EXPECT_EQ(level.next, nullptr);
}

TEST(TypesTest, FillResultDefaultConstruction) {
    FillResult fill{};
    EXPECT_EQ(fill.resting_order, nullptr);
    EXPECT_EQ(fill.price, 0);
    EXPECT_EQ(fill.quantity, 0);
    EXPECT_EQ(fill.resting_remaining, 0);
}

TEST(TypesTest, RejectReasonEnumValues) {
    // Ensure all values are distinct
    EXPECT_NE(static_cast<uint8_t>(RejectReason::PoolExhausted),
              static_cast<uint8_t>(RejectReason::InvalidPrice));
    EXPECT_NE(static_cast<uint8_t>(RejectReason::UnknownOrder),
              static_cast<uint8_t>(RejectReason::PriceBandViolation));
}

TEST(TypesTest, CancelReasonEnumValues) {
    EXPECT_NE(static_cast<uint8_t>(CancelReason::UserRequested),
              static_cast<uint8_t>(CancelReason::IOCRemainder));
    EXPECT_NE(static_cast<uint8_t>(CancelReason::FOKFailed),
              static_cast<uint8_t>(CancelReason::SelfMatchPrevention));
}

TEST(TypesTest, PriceRepresentation) {
    // 100.5000 should be represented as 1005000
    Price p = 1005000;
    EXPECT_EQ(p / PRICE_SCALE, 100);
    EXPECT_EQ(p % PRICE_SCALE, 5000);
}

}  // namespace
}  // namespace exchange
```

- [ ] **Update BUILD.bazel** -- add `types` library and `types_test` targets (keep existing targets intact)

- [ ] **Run test:** `bazel test //exchange-core:types_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(types): add core type aliases, enums, and structs

Foundation types for the matching engine: Price, Quantity, OrderId,
Timestamp aliases; Side, OrderType, TimeInForce, RejectReason, CancelReason
enumerations; Order, PriceLevel, FillResult structs with intrusive list
hooks. All types use default member initializers.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Object Pool

**Group:** 1 | **Dependencies:** None | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `exchange-core/object_pool.h` (create)
- `exchange-core/object_pool_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify -- add targets)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "object_pool",
    hdrs = ["object_pool.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "object_pool_test",
    srcs = ["object_pool_test.cc"],
    deps = [
        ":object_pool",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** `exchange-core/object_pool_test.cc`:

```cpp
#include "exchange-core/object_pool.h"

#include <gtest/gtest.h>
#include <set>

namespace exchange {
namespace {

struct TestItem {
    int value{0};
    TestItem* next{nullptr};  // required for free list
};

TEST(ObjectPoolTest, InitialState) {
    ObjectPool<TestItem, 10> pool;
    EXPECT_EQ(pool.capacity(), 10u);
    EXPECT_EQ(pool.available(), 10u);
}

TEST(ObjectPoolTest, AllocateSingleItem) {
    ObjectPool<TestItem, 10> pool;
    TestItem* item = pool.allocate();
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(pool.available(), 9u);
}

TEST(ObjectPoolTest, AllocateAndDeallocate) {
    ObjectPool<TestItem, 10> pool;
    TestItem* item = pool.allocate();
    ASSERT_NE(item, nullptr);
    pool.deallocate(item);
    EXPECT_EQ(pool.available(), 10u);
}

TEST(ObjectPoolTest, AllocateAllItems) {
    ObjectPool<TestItem, 4> pool;
    std::set<TestItem*> ptrs;
    for (size_t i = 0; i < 4; ++i) {
        TestItem* item = pool.allocate();
        ASSERT_NE(item, nullptr);
        ptrs.insert(item);
    }
    EXPECT_EQ(ptrs.size(), 4u);  // all unique pointers
    EXPECT_EQ(pool.available(), 0u);
}

TEST(ObjectPoolTest, ExhaustionReturnsNullptr) {
    ObjectPool<TestItem, 2> pool;
    pool.allocate();
    pool.allocate();
    EXPECT_EQ(pool.allocate(), nullptr);
    EXPECT_EQ(pool.available(), 0u);
}

TEST(ObjectPoolTest, DeallocateAndReallocate) {
    ObjectPool<TestItem, 2> pool;
    TestItem* a = pool.allocate();
    TestItem* b = pool.allocate();
    EXPECT_EQ(pool.allocate(), nullptr);

    pool.deallocate(a);
    EXPECT_EQ(pool.available(), 1u);

    TestItem* c = pool.allocate();
    EXPECT_NE(c, nullptr);
    EXPECT_EQ(c, a);  // reuses same slot
    (void)b;
}

TEST(ObjectPoolTest, ResetReturnsAllToFreeList) {
    ObjectPool<TestItem, 5> pool;
    for (size_t i = 0; i < 5; ++i) {
        pool.allocate();
    }
    EXPECT_EQ(pool.available(), 0u);
    pool.reset();
    EXPECT_EQ(pool.available(), 5u);
}

TEST(ObjectPoolTest, AllocatedItemsAreWithinStorage) {
    ObjectPool<TestItem, 10> pool;
    TestItem* item = pool.allocate();
    // Item should be usable
    item->value = 42;
    EXPECT_EQ(item->value, 42);
}

}  // namespace
}  // namespace exchange
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:object_pool_test` (should fail -- header doesn't exist)

- [ ] **Write implementation** `exchange-core/object_pool.h`:

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange {

// Pre-allocated fixed-size object pool with O(1) allocate/deallocate.
// T must have a `T* next` member for free-list threading.
template <typename T, size_t Capacity>
class ObjectPool {
public:
    ObjectPool() { reset(); }

    T* allocate() {
        if (free_head_ == nullptr) {
            return nullptr;
        }
        T* item = free_head_;
        free_head_ = free_head_->next;
        ++allocated_;
        return item;
    }

    void deallocate(T* p) {
        p->next = free_head_;
        free_head_ = p;
        --allocated_;
    }

    [[nodiscard]] size_t available() const { return Capacity - allocated_; }
    [[nodiscard]] constexpr size_t capacity() const { return Capacity; }

    void reset() {
        free_head_ = nullptr;
        allocated_ = 0;
        for (size_t i = 0; i < Capacity; ++i) {
            storage_[i].next = free_head_;
            free_head_ = &storage_[i];
        }
    }

private:
    std::array<T, Capacity> storage_{};
    T* free_head_{nullptr};
    size_t allocated_{0};
};

}  // namespace exchange
```

- [ ] **Run test to verify pass:** `bazel test //exchange-core:object_pool_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(object_pool): add pre-allocated fixed-size object pool

Zero-allocation object pool using intrusive free list threaded through
storage array. O(1) allocate/deallocate. Returns nullptr on exhaustion.
Requires T to have a T* next member.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Intrusive Linked List

**Group:** 1 | **Dependencies:** None | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `exchange-core/intrusive_list.h` (create)
- `exchange-core/intrusive_list_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify -- add targets)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "intrusive_list",
    hdrs = ["intrusive_list.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "intrusive_list_test",
    srcs = ["intrusive_list_test.cc"],
    deps = [
        ":intrusive_list",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** `exchange-core/intrusive_list_test.cc`:

```cpp
#include "exchange-core/intrusive_list.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

struct TestNode {
    int value{0};
    TestNode* prev{nullptr};
    TestNode* next{nullptr};
};

TEST(IntrusiveListTest, PushBackSingleNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1};

    intrusive_list::push_back(head, tail, &a);
    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(a.next, nullptr);
}

TEST(IntrusiveListTest, PushBackMultipleNodes) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2}, c{.value = 3};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::push_back(head, tail, &b);
    intrusive_list::push_back(head, tail, &c);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &c);
    EXPECT_EQ(a.next, &b);
    EXPECT_EQ(b.next, &c);
    EXPECT_EQ(c.prev, &b);
    EXPECT_EQ(b.prev, &a);
}

TEST(IntrusiveListTest, PushFrontSingleNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1};

    intrusive_list::push_front(head, tail, &a);
    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &a);
}

TEST(IntrusiveListTest, PushFrontMultipleNodes) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2}, c{.value = 3};

    intrusive_list::push_front(head, tail, &a);
    intrusive_list::push_front(head, tail, &b);
    intrusive_list::push_front(head, tail, &c);

    // Order should be c -> b -> a
    EXPECT_EQ(head, &c);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(c.next, &b);
    EXPECT_EQ(b.next, &a);
}

TEST(IntrusiveListTest, RemoveMiddleNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2}, c{.value = 3};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::push_back(head, tail, &b);
    intrusive_list::push_back(head, tail, &c);

    intrusive_list::remove(head, tail, &b);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &c);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(b.prev, nullptr);
    EXPECT_EQ(b.next, nullptr);
}

TEST(IntrusiveListTest, RemoveHeadNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::push_back(head, tail, &b);

    intrusive_list::remove(head, tail, &a);

    EXPECT_EQ(head, &b);
    EXPECT_EQ(tail, &b);
    EXPECT_EQ(b.prev, nullptr);
    EXPECT_EQ(b.next, nullptr);
}

TEST(IntrusiveListTest, RemoveTailNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::push_back(head, tail, &b);

    intrusive_list::remove(head, tail, &b);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(a.next, nullptr);
}

TEST(IntrusiveListTest, RemoveOnlyNode) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::remove(head, tail, &a);

    EXPECT_EQ(head, nullptr);
    EXPECT_EQ(tail, nullptr);
}

TEST(IntrusiveListTest, InsertBeforeHead) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::insert_before(head, tail, &a, &b);

    EXPECT_EQ(head, &b);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(b.next, &a);
    EXPECT_EQ(a.prev, &b);
}

TEST(IntrusiveListTest, InsertAfterTail) {
    TestNode* head = nullptr;
    TestNode* tail = nullptr;
    TestNode a{.value = 1}, b{.value = 2};

    intrusive_list::push_back(head, tail, &a);
    intrusive_list::insert_after(head, tail, &a, &b);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &b);
    EXPECT_EQ(a.next, &b);
    EXPECT_EQ(b.prev, &a);
}

}  // namespace
}  // namespace exchange
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:intrusive_list_test`

- [ ] **Write implementation** `exchange-core/intrusive_list.h`:

```cpp
#pragma once

namespace exchange {
namespace intrusive_list {

// All functions operate on a doubly-linked list identified by head/tail pointers.
// Node type T must have T* prev and T* next members.

template <typename T>
void push_back(T*& head, T*& tail, T* node) {
    node->prev = tail;
    node->next = nullptr;
    if (tail != nullptr) {
        tail->next = node;
    } else {
        head = node;
    }
    tail = node;
}

template <typename T>
void push_front(T*& head, T*& tail, T* node) {
    node->prev = nullptr;
    node->next = head;
    if (head != nullptr) {
        head->prev = node;
    } else {
        tail = node;
    }
    head = node;
}

template <typename T>
void remove(T*& head, T*& tail, T* node) {
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        head = node->next;
    }
    if (node->next != nullptr) {
        node->next->prev = node->prev;
    } else {
        tail = node->prev;
    }
    node->prev = nullptr;
    node->next = nullptr;
}

template <typename T>
void insert_before(T*& head, T*& /*tail*/, T* existing, T* node) {
    node->next = existing;
    node->prev = existing->prev;
    if (existing->prev != nullptr) {
        existing->prev->next = node;
    } else {
        head = node;
    }
    existing->prev = node;
}

template <typename T>
void insert_after(T*& /*head*/, T*& tail, T* existing, T* node) {
    node->prev = existing;
    node->next = existing->next;
    if (existing->next != nullptr) {
        existing->next->prev = node;
    } else {
        tail = node;
    }
    existing->next = node;
}

}  // namespace intrusive_list
}  // namespace exchange
```

- [ ] **Run test to verify pass:** `bazel test //exchange-core:intrusive_list_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(intrusive_list): add intrusive doubly-linked list operations

Template functions for push_back, push_front, remove, insert_before,
insert_after operating on nodes with prev/next pointers. Zero allocation,
O(1) operations. Used by orderbook for both price levels and orders.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Callback Event Structs

**Group:** 1 | **Dependencies:** None (uses types from Task 1, but only type aliases which are trivially replicated or included) | **Dev:** Any

> **Note:** This task depends on `types.h` being merged first since events reference `OrderId`, `Price`, `Quantity`, `Timestamp`, `Side`, `RejectReason`, `CancelReason`. If types.h is not yet merged, this task must wait. In practice, Task 1 finishes quickly and can be merged first.

**Files:**
- `exchange-core/events.h` (create)
- `exchange-core/events_test.cc` (create -- compile/sanity test)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "events",
    hdrs = ["events.h"],
    deps = [":types"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "events_test",
    srcs = ["events_test.cc"],
    deps = [
        ":events",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write header** `exchange-core/events.h`:

```cpp
#pragma once

#include "exchange-core/types.h"

namespace exchange {

// --- Order events (Section 6.1) ---

struct OrderAccepted {
    OrderId id;
    uint64_t client_order_id;
    Timestamp ts;
};

struct OrderRejected {
    uint64_t client_order_id;
    Timestamp ts;
    RejectReason reason;
};

struct OrderFilled {
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;
    Quantity quantity;
    Timestamp ts;
};

struct OrderPartiallyFilled {
    OrderId aggressor_id;
    OrderId resting_id;
    Price price;
    Quantity quantity;
    Quantity aggressor_remaining;
    Quantity resting_remaining;
    Timestamp ts;
};

struct OrderCancelled {
    OrderId id;
    Timestamp ts;
    CancelReason reason;
};

struct OrderCancelRejected {
    OrderId id;
    uint64_t client_order_id;
    Timestamp ts;
    RejectReason reason;
};

struct OrderModified {
    OrderId id;
    uint64_t client_order_id;
    Price new_price;
    Quantity new_qty;
    Timestamp ts;
};

struct OrderModifyRejected {
    OrderId id;
    uint64_t client_order_id;
    Timestamp ts;
    RejectReason reason;
};

// --- Market data events (Section 6.2) ---

struct TopOfBook {
    Price best_bid;
    Quantity bid_qty;
    Price best_ask;
    Quantity ask_qty;
    Timestamp ts;
};

struct DepthUpdate {
    Side side;
    Price price;
    Quantity total_qty;
    uint32_t order_count;
    enum Action : uint8_t { Add, Update, Remove } action;
    Timestamp ts;
};

struct OrderBookAction {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;
    enum Action : uint8_t { Add, Modify, Cancel, Fill } action;
    Timestamp ts;
};

struct Trade {
    Price price;
    Quantity quantity;
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;
    Timestamp ts;
};

}  // namespace exchange
```

- [ ] **Write compile/sanity test** `exchange-core/events_test.cc`:

```cpp
#include "exchange-core/events.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

TEST(EventsTest, OrderAcceptedConstruction) {
    OrderAccepted e{.id = 1, .client_order_id = 100, .ts = 5000};
    EXPECT_EQ(e.id, 1u);
    EXPECT_EQ(e.client_order_id, 100u);
    EXPECT_EQ(e.ts, 5000);
}

TEST(EventsTest, OrderRejectedConstruction) {
    OrderRejected e{.client_order_id = 1, .ts = 1000, .reason = RejectReason::InvalidPrice};
    EXPECT_EQ(e.reason, RejectReason::InvalidPrice);
}

TEST(EventsTest, OrderFilledConstruction) {
    OrderFilled e{.aggressor_id = 2, .resting_id = 1, .price = 1005000, .quantity = 10000, .ts = 2000};
    EXPECT_EQ(e.price, 1005000);
    EXPECT_EQ(e.quantity, 10000);
}

TEST(EventsTest, DepthUpdateActions) {
    DepthUpdate e{.side = Side::Buy, .price = 1000, .total_qty = 500, .order_count = 3,
                  .action = DepthUpdate::Add, .ts = 100};
    EXPECT_EQ(e.action, DepthUpdate::Add);
}

TEST(EventsTest, OrderBookActionValues) {
    OrderBookAction e{.id = 1, .side = Side::Sell, .price = 2000, .qty = 100,
                      .action = OrderBookAction::Fill, .ts = 200};
    EXPECT_EQ(e.action, OrderBookAction::Fill);
}

TEST(EventsTest, TradeConstruction) {
    Trade t{.price = 1005000, .quantity = 10000, .aggressor_id = 2,
            .resting_id = 1, .aggressor_side = Side::Sell, .ts = 3000};
    EXPECT_EQ(t.aggressor_side, Side::Sell);
}

TEST(EventsTest, OrderModifyRejectedConstruction) {
    OrderModifyRejected e{.id = 5, .client_order_id = 50, .ts = 9000,
                          .reason = RejectReason::UnknownOrder};
    EXPECT_EQ(e.reason, RejectReason::UnknownOrder);
}

}  // namespace
}  // namespace exchange
```

- [ ] **Run test:** `bazel test //exchange-core:events_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(events): add callback event structs for order and market data

All event types from spec Sections 6.1 and 6.2: OrderAccepted, OrderRejected,
OrderFilled, OrderPartiallyFilled, OrderCancelled, OrderCancelRejected,
OrderModified, OrderModifyRejected, TopOfBook, DepthUpdate, OrderBookAction,
Trade. Plain structs with no methods for zero-copy event passing.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Listener Interfaces

**Group:** 1 | **Dependencies:** Task 4 (events.h) | **Dev:** Any

**Files:**
- `exchange-core/listeners.h` (create)
- `exchange-core/listeners_test.cc` (create -- compile test)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "listeners",
    hdrs = ["listeners.h"],
    deps = [":events"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "listeners_test",
    srcs = ["listeners_test.cc"],
    deps = [
        ":listeners",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write header** `exchange-core/listeners.h`:

```cpp
#pragma once

#include "exchange-core/events.h"

namespace exchange {

// Default no-op listener base classes. Derived classes override by name hiding.
// The engine calls methods on the concrete listener type directly via its
// template parameter -- no virtual dispatch.

class OrderListenerBase {
public:
    void on_order_accepted(const OrderAccepted&) {}
    void on_order_rejected(const OrderRejected&) {}
    void on_order_filled(const OrderFilled&) {}
    void on_order_partially_filled(const OrderPartiallyFilled&) {}
    void on_order_cancelled(const OrderCancelled&) {}
    void on_order_cancel_rejected(const OrderCancelRejected&) {}
    void on_order_modified(const OrderModified&) {}
    void on_order_modify_rejected(const OrderModifyRejected&) {}
};

class MarketDataListenerBase {
public:
    void on_top_of_book(const TopOfBook&) {}
    void on_depth_update(const DepthUpdate&) {}
    void on_order_book_action(const OrderBookAction&) {}
    void on_trade(const Trade&) {}
};

}  // namespace exchange
```

- [ ] **Write compile test** `exchange-core/listeners_test.cc`:

```cpp
#include "exchange-core/listeners.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Verify base classes can be instantiated and called (no-op)
TEST(ListenersTest, OrderListenerBaseNoOps) {
    OrderListenerBase listener;
    // All methods should compile and do nothing
    listener.on_order_accepted(OrderAccepted{});
    listener.on_order_rejected(OrderRejected{});
    listener.on_order_filled(OrderFilled{});
    listener.on_order_partially_filled(OrderPartiallyFilled{});
    listener.on_order_cancelled(OrderCancelled{});
    listener.on_order_cancel_rejected(OrderCancelRejected{});
    listener.on_order_modified(OrderModified{});
    listener.on_order_modify_rejected(OrderModifyRejected{});
}

TEST(ListenersTest, MarketDataListenerBaseNoOps) {
    MarketDataListenerBase listener;
    listener.on_top_of_book(TopOfBook{});
    listener.on_depth_update(DepthUpdate{});
    listener.on_order_book_action(OrderBookAction{});
    listener.on_trade(Trade{});
}

// Verify derived class can override by name hiding
class TestOrderListener : public OrderListenerBase {
public:
    int accepted_count{0};
    void on_order_accepted(const OrderAccepted&) { ++accepted_count; }
};

TEST(ListenersTest, DerivedOverrideByNameHiding) {
    TestOrderListener listener;
    listener.on_order_accepted(OrderAccepted{.id = 1, .client_order_id = 1, .ts = 100});
    EXPECT_EQ(listener.accepted_count, 1);

    // Non-overridden method still compiles and no-ops
    listener.on_order_rejected(OrderRejected{});
}

}  // namespace
}  // namespace exchange
```

- [ ] **Run test:** `bazel test //exchange-core:listeners_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(listeners): add order and market data listener base classes

OrderListenerBase and MarketDataListenerBase with default no-op
implementations. Derived classes override by name hiding for zero
virtual dispatch overhead. Engine calls methods directly via template
parameter type.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: SPSC Ring Buffer

**Group:** 1 | **Dependencies:** None | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `exchange-core/spsc_ring_buffer.h` (create)
- `exchange-core/spsc_ring_buffer_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "spsc_ring_buffer",
    hdrs = ["spsc_ring_buffer.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "spsc_ring_buffer_test",
    srcs = ["spsc_ring_buffer_test.cc"],
    deps = [
        ":spsc_ring_buffer",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** `exchange-core/spsc_ring_buffer_test.cc`:

```cpp
#include "exchange-core/spsc_ring_buffer.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace exchange {
namespace {

TEST(SpscRingBufferTest, InitialState) {
    SpscRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.full());
    EXPECT_EQ(rb.size(), 0u);
}

TEST(SpscRingBufferTest, PushAndPop) {
    SpscRingBuffer<int, 4> rb;
    EXPECT_TRUE(rb.try_push(42));
    EXPECT_EQ(rb.size(), 1u);

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_TRUE(rb.empty());
}

TEST(SpscRingBufferTest, FillToCapacity) {
    SpscRingBuffer<int, 4> rb;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(rb.try_push(i));
    }
    EXPECT_TRUE(rb.full());
    EXPECT_FALSE(rb.try_push(99));  // full
}

TEST(SpscRingBufferTest, WrapAround) {
    SpscRingBuffer<int, 4> rb;
    // Fill and drain twice to force wrap-around
    for (int round = 0; round < 2; ++round) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(rb.try_push(i + round * 10));
        }
        for (int i = 0; i < 4; ++i) {
            int val = -1;
            EXPECT_TRUE(rb.try_pop(val));
            EXPECT_EQ(val, i + round * 10);
        }
    }
    EXPECT_TRUE(rb.empty());
}

TEST(SpscRingBufferTest, PopFromEmpty) {
    SpscRingBuffer<int, 4> rb;
    int val = -1;
    EXPECT_FALSE(rb.try_pop(val));
    EXPECT_EQ(val, -1);  // unchanged
}

TEST(SpscRingBufferTest, ConcurrentProducerConsumer) {
    SpscRingBuffer<int, 1024> rb;
    constexpr int kCount = 10000;

    std::thread producer([&]() {
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_push(i)) {
                // spin
            }
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&]() {
        int val = 0;
        for (int i = 0; i < kCount; ++i) {
            while (!rb.try_pop(val)) {
                // spin
            }
            received.push_back(val);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(received[i], i);
    }
}

}  // namespace
}  // namespace exchange
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:spsc_ring_buffer_test`

- [ ] **Write implementation** `exchange-core/spsc_ring_buffer.h`:

```cpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>  // std::hardware_destructive_interference_size

namespace exchange {

// Lock-free single-producer single-consumer ring buffer.
// Capacity must be a power of 2.
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2");
    static_assert(Capacity > 0, "Capacity must be positive");

    static constexpr size_t kCacheLineSize = 64;

public:
    bool try_push(const T& item) {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        if (w - r >= Capacity) {
            return false;  // full
        }
        buffer_[w & (Capacity - 1)] = item;
        write_pos_.store(w + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        const size_t r = read_pos_.load(std::memory_order_relaxed);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        if (r >= w) {
            return false;  // empty
        }
        item = buffer_[r & (Capacity - 1)];
        read_pos_.store(r + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const {
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t r = read_pos_.load(std::memory_order_acquire);
        return w - r;
    }

    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] bool full() const { return size() >= Capacity; }

private:
    alignas(kCacheLineSize) std::atomic<size_t> write_pos_{0};
    alignas(kCacheLineSize) std::atomic<size_t> read_pos_{0};
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

}  // namespace exchange
```

- [ ] **Run test to verify pass:** `bazel test //exchange-core:spsc_ring_buffer_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(spsc_ring_buffer): add lock-free single-producer single-consumer ring buffer

Power-of-2 capacity with bitwise modulo. Cache-line aligned atomic
read/write positions to avoid false sharing. Acquire/release memory
ordering for lock-free correctness. Pre-allocated fixed-size buffer.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Group 2 -- Core Components (Depends on Group 1)

All 5 tasks can be done in parallel once Group 1 is merged.

---

### Task 7: Composite Listener

**Group:** 2 | **Dependencies:** Task 5 (listeners.h) | **Est. Lines:** ~150 | **Dev:** Any

**Files:**
- `exchange-core/composite_listener.h` (create)
- `exchange-core/composite_listener_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "composite_listener",
    hdrs = ["composite_listener.h"],
    deps = [":listeners"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "composite_listener_test",
    srcs = ["composite_listener_test.cc"],
    deps = [
        ":composite_listener",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** -- test that a composite listener fans out to multiple listeners. Create two counter-based test listeners, wrap them in a `CompositeOrderListener`, fire events, assert both receive them. Do the same for `CompositeMdListener`.

Test patterns:
```cpp
// Two listeners that count events
struct CountingOrderListener : OrderListenerBase {
    int filled_count{0};
    void on_order_filled(const OrderFilled&) { ++filled_count; }
};

TEST(CompositeListenerTest, FansOutOrderFilled) {
    CountingOrderListener a, b;
    CompositeOrderListener<CountingOrderListener, CountingOrderListener> composite(a, b);
    composite.on_order_filled(OrderFilled{});
    EXPECT_EQ(a.filled_count, 1);
    EXPECT_EQ(b.filled_count, 1);
}
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:composite_listener_test`

- [ ] **Write implementation** -- `CompositeOrderListener<Listeners...>` and `CompositeMdListener<Listeners...>` using `std::tuple` + `std::apply` with fold expressions for each event method. Constructor takes `Listeners&...` references. See spec Section 6.5 for the pattern.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:composite_listener_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(composite_listener): add variadic fan-out listeners

CompositeOrderListener and CompositeMdListener dispatch each event to all
wrapped listeners via fold expressions. Zero virtual overhead, listener
set fixed at compile time.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Orderbook

**Group:** 2 | **Dependencies:** Tasks 1 (types), 2 (pool), 3 (intrusive_list) | **Est. Lines:** ~200 | **Dev:** Any

**Files:**
- `exchange-core/orderbook.h` (create)
- `exchange-core/orderbook.cc` (create)
- `exchange-core/orderbook_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "orderbook",
    srcs = ["orderbook.cc"],
    hdrs = ["orderbook.h"],
    deps = [":types", ":object_pool", ":intrusive_list"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "orderbook_test",
    srcs = ["orderbook_test.cc"],
    deps = [
        ":orderbook",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** -- cover: insert bid order at new level, insert at existing level, insert ask, remove order (level remains), remove last order (level removed), best_bid/best_ask tracking, multiple levels sorted correctly.

Key test patterns:
```cpp
TEST(OrderbookTest, InsertBidCreatesLevel) { ... }
TEST(OrderbookTest, InsertMultipleBidsDescendingSort) { ... }
TEST(OrderbookTest, InsertAsksAscendingSort) { ... }
TEST(OrderbookTest, RemoveOrderUpdatesLevelQuantity) { ... }
TEST(OrderbookTest, RemoveLastOrderRemovesLevel) { ... }
TEST(OrderbookTest, BestBidBestAskTracking) { ... }
TEST(OrderbookTest, InsertOrderAtExistingLevel) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:orderbook_test`

- [ ] **Write implementation** -- `Orderbook` class manages bid/ask price level lists using intrusive_list operations. Uses an `ObjectPool<PriceLevel, MaxLevels>` passed by reference (or template parameter). Methods: `insert_order(Order*)`, `remove_order(Order*)`, `find_or_create_level(Side, Price)`, `remove_level_if_empty(PriceLevel*)`, `best_bid()`, `best_ask()`. Bids sorted descending, asks sorted ascending. Walk the level list to find insertion point.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:orderbook_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(orderbook): add bid/ask price level management

Sorted intrusive doubly-linked lists of price levels (bids descending,
asks ascending). Insert/remove orders with automatic level creation and
cleanup. O(1) best bid/ask access. Uses ObjectPool for level allocation.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 9: Stop Book

**Group:** 2 | **Dependencies:** Tasks 1 (types), 2 (pool), 3 (intrusive_list) | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `exchange-core/stop_book.h` (create)
- `exchange-core/stop_book.cc` (create)
- `exchange-core/stop_book_test.cc` (create)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "stop_book",
    srcs = ["stop_book.cc"],
    hdrs = ["stop_book.h"],
    deps = [":types", ":object_pool", ":intrusive_list"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "stop_book_test",
    srcs = ["stop_book_test.cc"],
    deps = [
        ":stop_book",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** -- cover: insert buy stop (ascending sort), insert sell stop (descending sort), trigger check with matching price, trigger check with no match, remove triggered stops, stop cascade (multiple levels triggered).

Key test patterns:
```cpp
TEST(StopBookTest, InsertBuyStopAscending) { ... }
TEST(StopBookTest, InsertSellStopDescending) { ... }
TEST(StopBookTest, TriggerBuyStopOnPriceRise) { ... }
TEST(StopBookTest, TriggerSellStopOnPriceDrop) { ... }
TEST(StopBookTest, NoTriggerWhenPriceNotReached) { ... }
TEST(StopBookTest, RemoveStopOrder) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:stop_book_test`

- [ ] **Write implementation** -- `StopBook` manages buy_stops (ascending) and sell_stops (descending) using the same intrusive list structure as the orderbook. Methods: `insert_stop(Order*)`, `remove_stop(Order*)`, `get_triggered_stops(Price last_trade_price)` returns a list of triggered orders. Buy stops trigger when `last_trade_price >= stop_price`, sell stops when `last_trade_price <= stop_price`.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:stop_book_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(stop_book): add stop order management with trigger detection

Separate sorted lists for buy stops (ascending) and sell stops (descending).
Insert/remove/trigger operations. Buy stops trigger on price rise,
sell stops on price drop. Returns triggered orders for re-entry into engine.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 10: FIFO Matching Algorithm

**Group:** 2 | **Dependencies:** Task 1 (types) | **Est. Lines:** ~150 | **Dev:** Any

**Files:**
- `exchange-core/match_algo.h` (create -- FIFO part only, ProRata added in Task 11)
- `exchange-core/match_algo_test.cc` (create -- FIFO tests only)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "match_algo",
    hdrs = ["match_algo.h"],
    deps = [":types"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "match_algo_test",
    srcs = ["match_algo_test.cc"],
    deps = [
        ":match_algo",
        ":intrusive_list",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests** -- set up a PriceLevel with manually-linked orders, call `FifoMatch::match()`, verify fills are in FIFO order (head to tail), quantity tracking, level exhaustion.

Key test patterns:
```cpp
TEST(FifoMatchTest, SingleOrderFullFill) { ... }
TEST(FifoMatchTest, SingleOrderPartialFill) { ... }
TEST(FifoMatchTest, MultipleOrdersFifoOrder) { ... }
TEST(FifoMatchTest, LevelExhaustion) { ... }
TEST(FifoMatchTest, AggressorExhaustedBeforeLevel) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:match_algo_test`

- [ ] **Write implementation** -- `FifoMatch::match(PriceLevel& level, Quantity& remaining, FillResult* results, size_t& count)`. Walk from `level.head` to `level.tail`, fill each order fully before moving to next. For each fill, populate a `FillResult`, decrement `remaining`, update `resting_order->filled_quantity` and `resting_order->remaining_quantity`.

```cpp
struct FifoMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count) {
        Order* order = level.head;
        while (order != nullptr && remaining > 0) {
            Quantity fill_qty = std::min(remaining, order->remaining_quantity);
            order->filled_quantity += fill_qty;
            order->remaining_quantity -= fill_qty;
            remaining -= fill_qty;

            results[count++] = FillResult{
                .resting_order = order,
                .price = level.price,
                .quantity = fill_qty,
                .resting_remaining = order->remaining_quantity,
            };

            order = order->next;
        }
    }
};
```

- [ ] **Run test to verify pass:** `bazel test //exchange-core:match_algo_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(match_algo): add FIFO matching algorithm

FifoMatch::match walks orders head-to-tail (time priority), filling
each fully before advancing. Populates FillResult array. Static policy
class for compile-time algorithm selection.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 11: Pro-Rata Matching Algorithm

**Group:** 2 | **Dependencies:** Task 10 (match_algo.h exists) | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `exchange-core/match_algo.h` (modify -- add ProRataMatch)
- `exchange-core/match_algo_test.cc` (modify -- add ProRata tests)

**Steps:**

- [ ] **Write failing tests** -- add to `match_algo_test.cc`:

Key test patterns:
```cpp
TEST(ProRataMatchTest, ProportionalAllocation) {
    // Two orders: 60% and 40% of level. Aggressor for 50 units.
    // Expected: floor(60*50/100)=30, floor(40*50/100)=20. Total=50, no remainder.
}
TEST(ProRataMatchTest, RemainderDistributedByFifo) {
    // Three orders: 33/33/34 of 100. Aggressor for 10 units.
    // floor(33*10/100)=3, floor(33*10/100)=3, floor(34*10/100)=3. Total=9, remainder=1.
    // Remainder goes to first order (FIFO).
}
TEST(ProRataMatchTest, SingleOrderGetsFullFill) { ... }
TEST(ProRataMatchTest, ZeroAllocationSkipped) {
    // Tiny order gets floor(...)=0, should not receive a fill or remainder.
}
TEST(ProRataMatchTest, EqualSizesDegenerate) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:match_algo_test`

- [ ] **Write implementation** -- add `ProRataMatch` to `match_algo.h`. Two passes: (1) proportional allocation `floor(order.remaining * aggressor_remaining / level.total)`, (2) remainder distributed one lot at a time in FIFO order to orders that received > 0 in pass 1.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:match_algo_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(match_algo): add pro-rata matching algorithm

ProRataMatch::match allocates proportionally across resting orders,
with remainder distributed by time priority. Orders receiving zero
allocation are skipped in remainder round. Multiply-first to avoid
premature truncation.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Group 3 -- Matching Engine (Depends on Group 2)

Tasks 12->13->14 are a **sequential chain** (assign to one dev). Tasks 15 and 16 branch from 13. Tasks 17 and 18 come after all of 12-16.

The matching engine is a single header file (`matching_engine.h`) built incrementally. Each task adds a section. Unimplemented methods are stubbed with `assert(false && "not yet implemented")`.

---

### Task 12: Engine -- Order Validation + Acceptance

**Group:** 3 | **Dependencies:** Group 2 complete | **Est. Lines:** ~200 | **Dev:** Sequential chain dev

**Files:**
- `exchange-core/matching_engine.h` (create -- partial, stubs for match/cancel/modify/expiry)
- `exchange-core/matching_engine_test.cc` (create -- validation tests only)
- `exchange-core/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "matching_engine",
    hdrs = ["matching_engine.h"],
    deps = [
        ":types",
        ":events",
        ":listeners",
        ":object_pool",
        ":intrusive_list",
        ":orderbook",
        ":stop_book",
        ":match_algo",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "matching_engine_test",
    srcs = ["matching_engine_test.cc"],
    deps = [
        ":matching_engine",
        "@googletest//:gtest_main",
    ],
)
```

**What this task adds:**
- `EngineConfig` struct
- `OrderRequest` and `ModifyRequest` structs
- `MatchingEngine` class template declaration with all members
- Full `new_order()` validation path (tick/lot/price-band/quantity/tif/pool/id-space checks)
- `OrderAccepted` and `OrderRejected` callbacks
- CRTP hooks: `on_validate_order()`, `is_tif_valid()` with defaults
- **Stubs** for: match_order(), cancel_order(), modify_order(), trigger_expiry()
- `active_order_count()`, `available_order_slots()`, `available_level_slots()` queries

**What remains stubbed:**
```cpp
void cancel_order(OrderId, Timestamp) { assert(false && "Task 15"); }
void modify_order(const ModifyRequest&) { assert(false && "Task 15"); }
void trigger_expiry(Timestamp, TimeInForce) { assert(false && "Task 16"); }
// match_order is private, stubbed as no-op (order just rests without matching)
```

**Steps:**

- [ ] **Write failing tests** -- validation rejection tests:

```cpp
TEST(MatchingEngineTest, AcceptsValidLimitOrder) { ... }
TEST(MatchingEngineTest, RejectsZeroQuantity) { ... }
TEST(MatchingEngineTest, RejectsZeroPriceLimitOrder) { ... }
TEST(MatchingEngineTest, RejectsNegativePrice) { ... }
TEST(MatchingEngineTest, RejectsTickSizeViolation) { ... }
TEST(MatchingEngineTest, RejectsLotSizeViolation) { ... }
TEST(MatchingEngineTest, RejectsPriceBandViolation) { ... }
TEST(MatchingEngineTest, RejectsWhenPoolExhausted) { ... }
TEST(MatchingEngineTest, RejectsWhenIdSpaceExhausted) { ... }
TEST(MatchingEngineTest, AssignsSequentialOrderIds) { ... }
```

Each test uses a concrete `TestExchange` CRTP derived class and a counting listener to verify callbacks.

- [ ] **Run test to verify failure:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Write implementation** -- `matching_engine.h` with the full class template, validation logic in `new_order()`, and stubs for unimplemented methods.

Key structure:
```cpp
template <typename Derived, typename OrderListenerT, typename MdListenerT,
          typename MatchAlgoT = FifoMatch,
          size_t MaxOrders = 100000, size_t MaxPriceLevels = 10000,
          size_t MaxOrderIds = 1000000>
class MatchingEngine {
    // ... members from spec Section 7.2 ...

    void new_order(const OrderRequest& req) {
        // 1. CRTP validation hooks
        // 2. Tick/lot/band validation
        // 3. Pool/ID-space check
        // 4. Allocate + assign ID
        // 5. Fire OrderAccepted
        // 6. (stub) match_order / insert into book
    }
};
```

- [ ] **Run test to verify pass:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(matching_engine): add order validation and acceptance path

MatchingEngine template with EngineConfig, OrderRequest, ModifyRequest.
Full validation in new_order: tick size, lot size, price bands, quantity,
pool capacity, ID space. CRTP hooks for on_validate_order and is_tif_valid.
Matching/cancel/modify/expiry stubbed for subsequent tasks.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 13: Engine -- Limit Order Matching

**Group:** 3 | **Dependencies:** Task 12 | **Est. Lines:** ~200 | **Dev:** Sequential chain dev (same as Task 12)

**Files:**
- `exchange-core/matching_engine.h` (modify -- add match_order, limit order insert)
- `exchange-core/matching_engine_test.cc` (modify -- add matching tests)

**What this task adds:**
- Private `match_order(Order*)` method -- walks opposite side, calls `MatchAlgoT::match()` per level
- Fill callback firing: `OrderFilled`, `OrderPartiallyFilled`, `Trade`, `OrderBookAction`, `DepthUpdate`
- Post-match: limit order with remaining quantity inserts into book
- `TopOfBook` callback when best bid/ask changes
- SMP check skeleton: calls `derived().is_self_match()` and `derived().get_smp_action()` with default implementations
- Level cleanup after fills (remove empty levels, deallocate to pool)

**What remains stubbed:**
- Market/Stop/StopLimit handling in new_order (Task 14)
- IOC/FOK post-match logic (Task 16)
- cancel_order, modify_order (Task 15)
- trigger_expiry (Task 16)

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(MatchingEngineTest, LimitOrderRestsOnEmptyBook) { ... }
TEST(MatchingEngineTest, LimitOrderFullFill) { ... }
TEST(MatchingEngineTest, LimitOrderPartialFill) { ... }
TEST(MatchingEngineTest, LimitOrderSweepsMultipleLevels) { ... }
TEST(MatchingEngineTest, FillCallbackOrder) { ... }  // verify OrderAccepted -> fills -> book add -> TopOfBook
TEST(MatchingEngineTest, TopOfBookUpdatedAfterFill) { ... }
TEST(MatchingEngineTest, SelfMatchPreventionCancelNewest) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Write implementation** -- add `match_order()` to matching_engine.h. Replace the stub. Follow the callback ordering contract from spec Section 6.3.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(matching_engine): add limit order matching and book insertion

Limit orders match against opposite side using MatchAlgoT::match per
price level. Fires OrderFilled/PartiallyFilled, Trade, OrderBookAction,
DepthUpdate in spec-defined order. Remaining quantity rests on book.
SMP check via CRTP hooks.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 14: Engine -- Market, Stop, and StopLimit Orders

**Group:** 3 | **Dependencies:** Task 13 | **Est. Lines:** ~180 | **Dev:** Sequential chain dev

**Files:**
- `exchange-core/matching_engine.h` (modify -- add market/stop/stoplimit handling)
- `exchange-core/matching_engine_test.cc` (modify -- add tests)

**What this task adds:**
- Market order handling in `new_order()`: match immediately, cancel any remainder (no resting market orders)
- Stop order handling: insert into stop book instead of matching
- StopLimit order handling: insert into stop book
- Stop trigger logic: after each trade, check stop book. Triggered stops convert to Market (Stop) or Limit (StopLimit) and re-enter. Iterative cascade (not recursive).
- `should_trigger_stop()` CRTP hook with default implementation

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(MatchingEngineTest, MarketOrderFillsAndCancelsRemainder) { ... }
TEST(MatchingEngineTest, MarketOrderNoLiquidityCancelled) { ... }
TEST(MatchingEngineTest, MarketOrderSweepsMultipleLevels) { ... }
TEST(MatchingEngineTest, StopOrderInsertsIntoStopBook) { ... }
TEST(MatchingEngineTest, StopOrderTriggersOnTrade) { ... }
TEST(MatchingEngineTest, StopLimitTriggersAndRests) { ... }
TEST(MatchingEngineTest, StopCascade) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Write implementation** -- branch in `new_order()` on `OrderType`. Add private `check_and_trigger_stops()` called after each trade. The cascade is a while loop: trigger stops, re-enter each as new_order (or internal equivalent), check again until no more triggers.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(matching_engine): add market, stop, and stop-limit order handling

Market orders match immediately with remainder cancelled. Stop/StopLimit
insert into stop book. After each trade, iterative cascade checks and
triggers stops. CRTP should_trigger_stop hook for custom logic.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 15: Engine -- Cancel and Modify

**Group:** 3 | **Dependencies:** Task 13 | **Est. Lines:** ~180 | **Dev:** Can be a different dev from 12-14 chain (branches from 13)

**Files:**
- `exchange-core/matching_engine.h` (modify -- replace cancel/modify stubs)
- `exchange-core/matching_engine_test.cc` (modify -- add tests)

**What this task adds:**
- `cancel_order(OrderId, Timestamp)` -- look up in order_index_, remove from book, fire OrderCancelled + market data callbacks. Reject with OrderCancelRejected if order not found.
- `modify_order(ModifyRequest)` -- cancel-replace: remove old order, create new with modified price/qty, match if more aggressive. AmendDown: reduce qty in-place (keep priority). RejectModify: reject all modifications. Reject with OrderModifyRejected if order not found.
- `get_modify_policy()` CRTP hook

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(MatchingEngineTest, CancelRestingOrder) { ... }
TEST(MatchingEngineTest, CancelUnknownOrderRejected) { ... }
TEST(MatchingEngineTest, CancelAlreadyFilledOrderRejected) { ... }
TEST(MatchingEngineTest, ModifyCancelReplace) { ... }
TEST(MatchingEngineTest, ModifyTriggersMatchAtNewPrice) { ... }
TEST(MatchingEngineTest, ModifyUnknownOrderRejected) { ... }
TEST(MatchingEngineTest, ModifySamePriceLosesPriority) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Write implementation** -- replace cancel/modify stubs. Follow callback ordering from spec Section 6.3.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(matching_engine): add cancel and modify order operations

cancel_order removes from book with OrderCancelled callback. modify_order
supports cancel-replace (default), amend-down, and reject policies via
CRTP get_modify_policy hook. Full callback ordering per spec.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 16: Engine -- TIF Handling and Expiry

**Group:** 3 | **Dependencies:** Task 13 | **Est. Lines:** ~120 | **Dev:** Can be a different dev from 12-14 chain

**Files:**
- `exchange-core/matching_engine.h` (modify -- add IOC/FOK post-match, replace trigger_expiry stub)
- `exchange-core/matching_engine_test.cc` (modify -- add tests)

**What this task adds:**
- IOC post-match: if remaining after match, cancel remainder with `CancelReason::IOCRemainder`
- FOK pre-match: check if total available quantity >= order quantity before matching. If not, reject entire order (cancel with `CancelReason::FOKFailed`)
- `trigger_expiry(Timestamp now, TimeInForce tif)` -- linear scan of order_index_, cancel matching orders with `CancelReason::Expired`

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(MatchingEngineTest, IOCFullFill) { ... }
TEST(MatchingEngineTest, IOCPartialFillCancelsRemainder) { ... }
TEST(MatchingEngineTest, FOKFullFill) { ... }
TEST(MatchingEngineTest, FOKInsufficientLiquidityCancelled) { ... }
TEST(MatchingEngineTest, DAYExpiryByTrigger) { ... }
TEST(MatchingEngineTest, GTDExpiryByTrigger) { ... }
TEST(MatchingEngineTest, GTDNotExpiredWhenBeforeExpiry) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Write implementation** -- add post-match TIF logic in `new_order()` after `match_order()` returns. Add `trigger_expiry()` method.

- [ ] **Run test to verify pass:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(matching_engine): add TIF handling and bulk expiry

IOC cancels remainder after matching. FOK checks total available
quantity before matching, cancels if insufficient. trigger_expiry
does linear scan for DAY/GTD orders past their expiry time.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 17: Engine Unit Tests -- Core Paths

**Group:** 3 | **Dependencies:** Tasks 12-16 all complete | **Est. Lines:** ~200 | **Dev:** Any

**Files:**
- `exchange-core/matching_engine_test.cc` (modify -- add comprehensive core path tests)

**Steps:**

- [ ] **Write tests** covering core scenarios end-to-end with full callback verification:

```cpp
TEST(MatchingEngineTest, FullFillCallbackSequence) {
    // Verify exact callback ordering: Accepted -> Filled -> Trade ->
    // OrderBookAction -> DepthUpdate -> TopOfBook per spec Section 6.3
}
TEST(MatchingEngineTest, PartialFillThenRest) { ... }
TEST(MatchingEngineTest, CancelCallbackSequence) { ... }
TEST(MatchingEngineTest, MarketOrderCallbackSequence) { ... }
TEST(MatchingEngineTest, MultiLevelSweepCallbacks) { ... }
TEST(MatchingEngineTest, BestBidAskUpdatesCorrectly) { ... }
TEST(MatchingEngineTest, LevelCreationAndDeletion) { ... }
TEST(MatchingEngineTest, PoolExhaustionDuringRest) { ... }  // level pool exhausted
```

- [ ] **Run tests:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(matching_engine): add comprehensive core path unit tests

End-to-end callback sequence verification for full fill, partial fill,
cancel, market order, multi-level sweep. Validates exact callback
ordering per spec Section 6.3.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 18: Engine Unit Tests -- Advanced Paths

**Group:** 3 | **Dependencies:** Tasks 12-16 all complete | **Est. Lines:** ~200 | **Dev:** Any (can parallelize with Task 17)

**Files:**
- `exchange-core/matching_engine_test.cc` (modify -- add advanced path tests)

**Steps:**

- [ ] **Write tests:**

```cpp
TEST(MatchingEngineTest, ModifyCancelReplaceCallbackSequence) { ... }
TEST(MatchingEngineTest, ModifyAmendDownPreservesPriority) { ... }
TEST(MatchingEngineTest, SmpCancelOldest) { ... }
TEST(MatchingEngineTest, SmpCancelBoth) { ... }
TEST(MatchingEngineTest, SmpDecrement) { ... }
TEST(MatchingEngineTest, StopTriggerCascade) { ... }
TEST(MatchingEngineTest, IOCFOKCombinedWithSMP) { ... }
TEST(MatchingEngineTest, ExpiryCallbackSequence) { ... }
TEST(MatchingEngineTest, CancelStopOrder) { ... }
TEST(MatchingEngineTest, ModifyStopOrder) { ... }
TEST(MatchingEngineTest, OrderIdSpaceExhaustionAfterManyOrders) { ... }
```

- [ ] **Run tests:** `bazel test //exchange-core:matching_engine_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(matching_engine): add advanced path unit tests

Covers modify policies, SMP actions (cancel newest/oldest/both/decrement),
stop trigger cascades, TIF+SMP interaction, expiry callbacks,
cancel/modify stop orders, ID space exhaustion.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Group 4 -- Test Harness (Depends on Group 1 Only)

All 4 initial tasks can be done in parallel (once Group 1 events.h is merged). Task 23 depends on 19-22.

> **Key:** Group 4 can start as soon as Group 1 merges, running in parallel with Groups 2 and 3.

---

### Task 19: Recorded Event Type

**Group:** 4 | **Dependencies:** Task 4 (events.h) | **Est. Lines:** ~120 | **Dev:** Any

**Files:**
- `test-harness/recorded_event.h` (create)
- `test-harness/recorded_event_test.cc` (create)
- `test-harness/BUILD.bazel` (create)

**BUILD.bazel** (new file):

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "recorded_event",
    hdrs = ["recorded_event.h"],
    deps = ["//exchange-core:events"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "recorded_event_test",
    srcs = ["recorded_event_test.cc"],
    deps = [
        ":recorded_event",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write tests** -- verify construction from each event type, equality comparison, to_string serialization:

```cpp
TEST(RecordedEventTest, ConstructFromOrderAccepted) { ... }
TEST(RecordedEventTest, ConstructFromTrade) { ... }
TEST(RecordedEventTest, EqualityComparison) { ... }
TEST(RecordedEventTest, InequalityOnDifferentTypes) { ... }
TEST(RecordedEventTest, ToStringFormatting) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //test-harness:recorded_event_test`

- [ ] **Write implementation** -- `RecordedEvent` as a `std::variant` over all event types. Add `operator==` (compare variant index + values) and `to_string()` (return human-readable representation for diff output).

- [ ] **Run test to verify pass:** `bazel test //test-harness:recorded_event_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(test_harness): add RecordedEvent variant type

std::variant over all order and market data event types. Supports
equality comparison for assertion and to_string for diff output.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 20: Recording Listeners

**Group:** 4 | **Dependencies:** Tasks 5 (listeners.h), 19 (recorded_event.h) | **Est. Lines:** ~150 | **Dev:** Any

**Files:**
- `test-harness/recording_listener.h` (create)
- `test-harness/recording_listener_test.cc` (create)
- `test-harness/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "recording_listener",
    hdrs = ["recording_listener.h"],
    deps = [
        ":recorded_event",
        "//exchange-core:listeners",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "recording_listener_test",
    srcs = ["recording_listener_test.cc"],
    deps = [
        ":recording_listener",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(RecordingOrderListenerTest, CapturesOrderAccepted) { ... }
TEST(RecordingOrderListenerTest, CapturesMultipleEvents) { ... }
TEST(RecordingOrderListenerTest, ClearResetsEvents) { ... }
TEST(RecordingMdListenerTest, CapturesTopOfBook) { ... }
TEST(RecordingMdListenerTest, CapturesTrade) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //test-harness:recording_listener_test`

- [ ] **Write implementation** -- `RecordingOrderListener` inherits `OrderListenerBase`, overrides each method to push a `RecordedEvent` into `std::vector<RecordedEvent>`. Same pattern for `RecordingMdListener`.

- [ ] **Run test to verify pass:** `bazel test //test-harness:recording_listener_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(test_harness): add recording listeners for event capture

RecordingOrderListener and RecordingMdListener capture all events
into a vector of RecordedEvent. Provides events(), clear(), size()
methods for test assertion.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 21: Journal Parser

**Group:** 4 | **Dependencies:** Task 4 (events.h, for field types) | **Est. Lines:** ~200 | **Dev:** Any

**Files:**
- `test-harness/journal_parser.h` (create)
- `test-harness/journal_parser.cc` (create)
- `test-harness/journal_parser_test.cc` (create)
- `test-harness/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "journal_parser",
    srcs = ["journal_parser.cc"],
    hdrs = ["journal_parser.h"],
    deps = [
        "//exchange-core:types",
        "//exchange-core:events",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "journal_parser_test",
    srcs = ["journal_parser_test.cc"],
    deps = [
        ":journal_parser",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(JournalParserTest, ParseConfigLine) { ... }
TEST(JournalParserTest, ParseNewOrderAction) { ... }
TEST(JournalParserTest, ParseCancelAction) { ... }
TEST(JournalParserTest, ParseModifyAction) { ... }
TEST(JournalParserTest, ParseExpectOrderAccepted) { ... }
TEST(JournalParserTest, ParseExpectTrade) { ... }
TEST(JournalParserTest, SkipsCommentsAndBlankLines) { ... }
TEST(JournalParserTest, GroupsActionsWithExpectations) { ... }
TEST(JournalParserTest, MalformedLineReportsError) { ... }
TEST(JournalParserTest, ParseFromString) { ... }  // parse from string, not file
```

- [ ] **Run test to verify failure:** `bazel test //test-harness:journal_parser_test`

- [ ] **Write implementation** -- line-by-line parser. Each line starts with `CONFIG`, `ACTION`, or `EXPECT`. Key-value extraction via string splitting. Build `ParsedConfig`, `ParsedAction`, `ParsedExpectation` structs. Group into `JournalEntry` (one action + its expectations).

- [ ] **Run test to verify pass:** `bazel test //test-harness:journal_parser_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(test_harness): add journal parser for .journal file format

Parses CONFIG, ACTION, and EXPECT lines with key=value extraction.
Groups actions with their expectations into JournalEntry sequences.
Error reporting for malformed lines with line numbers.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 22: Journal Writer

**Group:** 4 | **Dependencies:** Task 19 (recorded_event.h) | **Est. Lines:** ~150 | **Dev:** Any

**Files:**
- `test-harness/journal_writer.h` (create)
- `test-harness/journal_writer.cc` (create)
- `test-harness/journal_writer_test.cc` (create)
- `test-harness/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "journal_writer",
    srcs = ["journal_writer.cc"],
    hdrs = ["journal_writer.h"],
    deps = [
        ":recorded_event",
        ":journal_parser",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "journal_writer_test",
    srcs = ["journal_writer_test.cc"],
    deps = [
        ":journal_writer",
        ":journal_parser",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(JournalWriterTest, WritesConfigLine) { ... }
TEST(JournalWriterTest, WritesActionLine) { ... }
TEST(JournalWriterTest, WritesExpectLine) { ... }
TEST(JournalWriterTest, RoundTripWithParser) {
    // Write a journal to string, parse it back, verify identical structure
}
TEST(JournalWriterTest, WritesToFile) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //test-harness:journal_writer_test`

- [ ] **Write implementation** -- serialize `ParsedConfig`, `ParsedAction`, and `RecordedEvent` vectors to journal text format. One line per entry. Round-trip compatibility with the parser.

- [ ] **Run test to verify pass:** `bazel test //test-harness:journal_writer_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(test_harness): add journal writer for recording mode

Serializes config, actions, and recorded events to .journal text format.
Round-trip compatible with journal parser. Enables record-and-baseline
workflow for regression testing.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 23: Test Runner

**Group:** 4 | **Dependencies:** Tasks 19-22 all complete + Group 3 (matching_engine) | **Est. Lines:** ~200 | **Dev:** Any

> **Note:** The test runner needs both the harness pieces (19-22) AND the matching engine (Group 3) to be functional for integration tests. However, the runner's core logic (replay + compare) can be written and unit-tested with a mock engine or minimal engine first.

**Files:**
- `test-harness/test_runner.h` (create)
- `test-harness/test_runner.cc` (create)
- `test-harness/test_runner_test.cc` (create)
- `test-harness/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "test_runner",
    srcs = ["test_runner.cc"],
    hdrs = ["test_runner.h"],
    deps = [
        ":recorded_event",
        ":recording_listener",
        ":journal_parser",
        "//exchange-core:matching_engine",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "test_runner_test",
    srcs = ["test_runner_test.cc"],
    deps = [
        ":test_runner",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(TestRunnerTest, PassingScenario) { ... }
TEST(TestRunnerTest, FailingScenarioReportsDiff) { ... }
TEST(TestRunnerTest, MissingExpectedEventFails) { ... }
TEST(TestRunnerTest, ExtraActualEventFails) { ... }
TEST(TestRunnerTest, WrongEventTypeFails) { ... }
TEST(TestRunnerTest, WrongFieldValueFails) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //test-harness:test_runner_test`

- [ ] **Write implementation** -- `JournalTestRunner` replays actions into the engine, collects recorded events after each action, compares against expectations. Returns `TestResult` with diff on mismatch.

```cpp
struct TestResult {
    bool passed;
    size_t action_index;
    size_t event_index;
    std::string expected;
    std::string actual;
    std::string diff;
};

class JournalTestRunner {
public:
    template <typename EngineT>
    TestResult run(EngineT& engine,
                   RecordingOrderListener& order_listener,
                   RecordingMdListener& md_listener,
                   const std::vector<JournalParser::JournalEntry>& entries);
};
```

- [ ] **Run test to verify pass:** `bazel test //test-harness:test_runner_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(test_harness): add journal test runner with diff reporting

Replays journal actions into engine, compares recorded events against
expectations. TestResult includes action/event index and human-readable
diff on mismatch. Supports both FIFO and ProRata engine types.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Group 5 -- Journal Test Scenarios (Depends on Groups 3 + 4)

All journal-writing tasks (24-30) can be done in parallel. Task 31 depends on all of them.

Each task creates `.journal` files and adds them to a Bazel `filegroup` for the integration test.

> **Important:** Journal files follow the exact format from spec Section 8.3. Refer to the spec for field names and value formats. Each file should be self-contained with its own CONFIG line.

---

### Task 24: Happy Path Journals

**Group:** 5 | **Dependencies:** Groups 3+4 complete | **Est. Lines:** ~200 total across files | **Dev:** Any

**Files (create all):**
- `test-journals/basic_limit_buy.journal`
- `test-journals/basic_limit_sell.journal`
- `test-journals/limit_full_fill.journal`
- `test-journals/limit_partial_fill.journal`
- `test-journals/market_order_fill.journal`
- `test-journals/market_order_no_liquidity.journal`
- `test-journals/market_order_sweep.journal`
- `test-journals/multiple_fills.journal`
- `test-journals/BUILD.bazel` (create)

**BUILD.bazel** (new file):

```starlark
filegroup(
    name = "happy_path_journals",
    srcs = glob(["basic_*.journal", "limit_*.journal", "market_*.journal", "multiple_*.journal"]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "all_journals",
    srcs = glob(["*.journal"]),
    visibility = ["//visibility:public"],
)
```

**Steps:**

- [ ] **Write journal files** -- each file has CONFIG, ACTION, and EXPECT lines. Example for `limit_full_fill.journal`:

```
# Two opposite limit orders at same price -- full fill
CONFIG match_algo=FIFO tick_size=100 lot_size=10000

ACTION NEW_ORDER ts=1000 cl_ord_id=1 account_id=100 side=BUY price=1005000 qty=10000 type=LIMIT tif=GTC
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1000
EXPECT ORDER_BOOK_ACTION ord_id=1 side=BUY price=1005000 qty=10000 action=ADD ts=1000
EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=10000 count=1 action=ADD ts=1000
EXPECT TOP_OF_BOOK bid=1005000 bid_qty=10000 ask=0 ask_qty=0 ts=1000

ACTION NEW_ORDER ts=2000 cl_ord_id=2 account_id=200 side=SELL price=1005000 qty=10000 type=LIMIT tif=GTC
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2000
EXPECT ORDER_FILLED aggressor=2 resting=1 price=1005000 qty=10000 ts=2000
EXPECT TRADE price=1005000 qty=10000 aggressor=2 resting=1 aggressor_side=SELL ts=2000
EXPECT ORDER_BOOK_ACTION ord_id=1 side=BUY price=1005000 qty=10000 action=FILL ts=2000
EXPECT DEPTH_UPDATE side=BUY price=1005000 qty=0 count=0 action=REMOVE ts=2000
EXPECT TOP_OF_BOOK bid=0 bid_qty=0 ask=0 ask_qty=0 ts=2000
```

Refer to spec Section 8.8 for each scenario's description.

- [ ] **Verify journals parse:** `bazel test //test-harness:journal_parser_test` (if parser tests load these files)

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add happy path journal test scenarios

8 journal files covering basic limit order entry, full/partial fills,
market order fills/cancellation/sweep, and multi-level fills.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 25: Order Type Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~150 | **Dev:** Any

**Files (create all):**
- `test-journals/stop_trigger.journal`
- `test-journals/stop_limit_trigger.journal`
- `test-journals/stop_cascade.journal`
- `test-journals/cancel_stop_order.journal`
- `test-journals/modify_stop_order.journal`

**Steps:**

- [ ] **Write journal files** -- refer to spec Section 8.8 "Order types" for each scenario. Stop cascade journal should demonstrate multi-level trigger chain.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add stop and stop-limit order test scenarios

5 journal files covering stop trigger, stop-limit trigger and rest,
stop cascade (multi-level chain), cancel stop, modify stop.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 26: TIF Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~180 | **Dev:** Any

**Files (create all):**
- `test-journals/ioc_full_fill.journal`
- `test-journals/ioc_partial_cancel.journal`
- `test-journals/fok_full_fill.journal`
- `test-journals/fok_no_fill.journal`
- `test-journals/fok_multi_level.journal`
- `test-journals/gtd_expiry.journal`
- `test-journals/gtd_not_expired.journal`
- `test-journals/day_expiry.journal`

**Steps:**

- [ ] **Write journal files** -- refer to spec Section 8.8 "Time in force" for each scenario. The FOK multi-level test verifies that FOK checks total available quantity across all levels.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add time-in-force journal test scenarios

8 journal files: IOC full fill and partial cancel, FOK full fill/no fill/
multi-level, GTD expiry/not-expired, DAY expiry via trigger_expiry.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 27: Matching Algorithm Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~150 | **Dev:** Any

**Files (create all):**
- `test-journals/fifo_priority.journal`
- `test-journals/pro_rata_basic.journal`
- `test-journals/pro_rata_remainder.journal`
- `test-journals/pro_rata_single_order.journal`
- `test-journals/pro_rata_equal_sizes.journal`

> **Note:** Pro-rata journals must use `CONFIG match_algo=PRO_RATA` so the integration test instantiates the correct engine type.

**Steps:**

- [ ] **Write journal files** -- FIFO priority verifies earlier orders fill first. Pro-rata journals verify proportional allocation, remainder distribution, single-order edge case, equal-size degenerate case.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add matching algorithm journal test scenarios

5 journal files: FIFO priority verification, pro-rata proportional
allocation, remainder distribution by time priority, single-order
edge case, equal-sizes degenerate case.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 28: Cancel and Modify Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~180 | **Dev:** Any

**Files (create all):**
- `test-journals/cancel_resting.journal`
- `test-journals/cancel_unknown.journal`
- `test-journals/cancel_already_filled.journal`
- `test-journals/modify_price_change.journal`
- `test-journals/modify_qty_down.journal`
- `test-journals/modify_qty_up.journal`
- `test-journals/modify_triggers_fill.journal`
- `test-journals/modify_same_price.journal`
- `test-journals/modify_unknown.journal`

**Steps:**

- [ ] **Write journal files** -- refer to spec Section 8.8 "Cancel and modify". `modify_triggers_fill` demonstrates a cancel-replace where the new price crosses the spread.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add cancel and modify journal test scenarios

9 journal files: cancel resting/unknown/already-filled, modify price
change/qty down/qty up/triggers fill/same price/unknown.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 29: Failure and Edge Case Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~200 | **Dev:** Any

**Files (create all):**
- `test-journals/pool_exhaustion_orders.journal`
- `test-journals/pool_exhaustion_levels.journal`
- `test-journals/self_match_prevention.journal`
- `test-journals/price_band_reject.journal`
- `test-journals/tick_size_reject.journal`
- `test-journals/lot_size_reject.journal`
- `test-journals/zero_quantity.journal`
- `test-journals/zero_price_limit.journal`
- `test-journals/negative_price.journal`
- `test-journals/empty_book_cancel.journal`
- `test-journals/market_order_tif.journal`

> **Note:** Pool exhaustion journals need `CONFIG max_orders=2` (small pool) to test exhaustion within a few orders.

**Steps:**

- [ ] **Write journal files** -- each file tests one specific failure or edge case. Pool exhaustion uses small pool config. SMP uses a custom exchange with matching account IDs.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add failure and edge case journal test scenarios

11 journal files: pool exhaustion (orders and levels), self-match
prevention, price band/tick size/lot size/zero qty/zero price/
negative price rejection, empty book cancel, market order TIF behavior.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 30: Market Data Journals

**Group:** 5 | **Dependencies:** Groups 3+4 | **Est. Lines:** ~150 | **Dev:** Any

**Files (create all):**
- `test-journals/l1_updates.journal`
- `test-journals/l2_depth_updates.journal`
- `test-journals/l3_order_actions.journal`
- `test-journals/trade_events.journal`

**Steps:**

- [ ] **Write journal files** -- focus on verifying correct market data callbacks. L1 tests TopOfBook after each action. L2 tests DepthUpdate Add/Update/Remove transitions. L3 tests OrderBookAction for every order lifecycle event. Trade events verify Trade struct fields.

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add market data callback journal test scenarios

4 journal files: L1 TopOfBook updates, L2 DepthUpdate add/update/remove,
L3 OrderBookAction for all order events, Trade event field verification.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 31: Journal Integration Test

**Group:** 5 | **Dependencies:** Tasks 24-30 all complete | **Est. Lines:** ~100 | **Dev:** Any

**Files:**
- `test-harness/journal_integration_test.cc` (create)
- `test-harness/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_test(
    name = "journal_integration_test",
    srcs = ["journal_integration_test.cc"],
    data = ["//test-journals:all_journals"],
    deps = [
        ":test_runner",
        ":journal_parser",
        ":recording_listener",
        "//exchange-core:matching_engine",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write test** -- discovers all `.journal` files in the test-journals directory (via `TEST_SRCDIR` / `runfiles`), parses each, instantiates the correct engine type based on `match_algo` config, runs via `JournalTestRunner`, asserts pass:

```cpp
class JournalIntegrationTest : public ::testing::TestWithParam<std::string> {};

TEST_P(JournalIntegrationTest, ReplayJournal) {
    const std::string& path = GetParam();
    auto journal = JournalParser::parse(path);

    // Pick engine type based on config
    if (journal.config.match_algo == "PRO_RATA") {
        // Instantiate ProRata engine + run
    } else {
        // Instantiate FIFO engine + run
    }

    // Assert TestResult.passed with detailed failure message
}

INSTANTIATE_TEST_SUITE_P(AllJournals, JournalIntegrationTest,
    ::testing::ValuesIn(discover_journal_files()));
```

- [ ] **Run test:** `bazel test //test-harness:journal_integration_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
test(journals): add parameterized integration test for all journals

Discovers all .journal files, parses config to select engine type
(FIFO or ProRata), replays via JournalTestRunner, asserts pass with
detailed diff output on failure.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Group 6 -- Visualization Tools (Depends on Groups 1 + 4)

Task dependency chain within Group 6:
- 32 (orderbook state) -- depends on Task 19 (recorded_event)
- 33 (TUI renderer) -- depends on 32
- 34 (offline viz) -- depends on 33 + Task 21 (journal_parser)
- 35 (shm transport) -- depends on Task 6 (spsc_ring_buffer)
- 36 (shm listeners) -- depends on 35 + Task 5 (listeners)
- 37 (live viz) -- depends on 33 + 35

> **Note:** FTXUI must be added to MODULE.bazel before Tasks 33, 34, 37 can build.

**MODULE.bazel addition** (do this as part of Task 33):

```starlark
bazel_dep(name = "ftxui", version = "6.0.2")
```

---

### Task 32: Orderbook State Reconstructor

**Group:** 6 | **Dependencies:** Task 19 (recorded_event.h) | **Est. Lines:** ~200 | **Dev:** Any

**Files:**
- `tools/orderbook_state.h` (create)
- `tools/orderbook_state.cc` (create)
- `tools/orderbook_state_test.cc` (create)
- `tools/BUILD.bazel` (create)

**BUILD.bazel** (new file):

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

cc_library(
    name = "orderbook_state",
    srcs = ["orderbook_state.cc"],
    hdrs = ["orderbook_state.h"],
    deps = ["//test-harness:recorded_event"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "orderbook_state_test",
    srcs = ["orderbook_state_test.cc"],
    deps = [
        ":orderbook_state",
        "@googletest//:gtest_main",
    ],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(OrderbookStateTest, EmptyInitialState) { ... }
TEST(OrderbookStateTest, ApplyDepthUpdateAdd) { ... }
TEST(OrderbookStateTest, ApplyDepthUpdateRemove) { ... }
TEST(OrderbookStateTest, ApplyTopOfBook) { ... }
TEST(OrderbookStateTest, ApplyTrade) { ... }
TEST(OrderbookStateTest, ApplyOrderBookAction) { ... }
TEST(OrderbookStateTest, ReconstructAfterMultipleEvents) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //tools:orderbook_state_test`

- [ ] **Write implementation** -- `OrderbookState` class with `apply(const RecordedEvent&)` method. Maintains: sorted bid/ask depth levels (price -> qty, count), recent trades list, recent order events list, current best bid/ask. Reconstructs state purely from events (does NOT depend on matching engine).

- [ ] **Run test to verify pass:** `bazel test //tools:orderbook_state_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add orderbook state reconstructor from events

Rebuilds orderbook depth, trades, and order events from a stream of
RecordedEvents. Independent of matching engine -- works with both
live and journal event sources.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 33: TUI Renderer

**Group:** 6 | **Dependencies:** Task 32 (orderbook_state) | **Est. Lines:** ~200 | **Dev:** Any

**Files:**
- `tools/tui_renderer.h` (create)
- `tools/tui_renderer.cc` (create)
- `tools/BUILD.bazel` (modify)
- `MODULE.bazel` (modify -- add FTXUI dep)

**MODULE.bazel change:**

```starlark
bazel_dep(name = "ftxui", version = "6.0.2")
```

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "tui_renderer",
    srcs = ["tui_renderer.cc"],
    hdrs = ["tui_renderer.h"],
    deps = [
        ":orderbook_state",
        "@ftxui//...",  # exact target depends on ftxui's bzlmod structure
    ],
    visibility = ["//visibility:public"],
)
```

> **Note:** The exact FTXUI Bazel target path depends on how the FTXUI module exposes its libraries. Check the FTXUI BCR entry or MODULE.bazel after adding the dep. Common targets: `@ftxui//:screen`, `@ftxui//:dom`, `@ftxui//:component`.

**Steps:**

- [ ] **Add FTXUI to MODULE.bazel**

- [ ] **Write implementation** -- FTXUI components per spec Section 10.1 display layout:
  - `render_orderbook(const OrderbookState&)` -> ftxui::Element (bid/ask depth table)
  - `render_trades(const OrderbookState&)` -> ftxui::Element (recent trades)
  - `render_events(const OrderbookState&)` -> ftxui::Element (order event log)
  - `render_status(size_t action_idx, size_t total_actions, size_t orders, size_t max_orders, size_t levels, size_t max_levels)` -> ftxui::Element
  - `render_full(const OrderbookState&, ...)` -> ftxui::Element (compose all panels)

- [ ] **Verify build:** `bazel build //tools:tui_renderer`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add FTXUI-based TUI renderer components

Shared rendering components for orderbook depth, recent trades, order
event log, and status bar. Takes OrderbookState as input, returns
ftxui::Element. Used by both offline and live viewers.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 34: Offline Journal Visualizer

**Group:** 6 | **Dependencies:** Tasks 33 (tui_renderer), 21 (journal_parser) | **Est. Lines:** ~150 | **Dev:** Any

**Files:**
- `tools/viz_replay.cc` (create)
- `tools/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_binary(
    name = "exchange-viz-replay",
    srcs = ["viz_replay.cc"],
    deps = [
        ":tui_renderer",
        ":orderbook_state",
        "//test-harness:journal_parser",
        "//test-harness:recorded_event",
        "@ftxui//...",
    ],
)
```

**Steps:**

- [ ] **Write implementation** -- `main()` parses CLI args (journal path, -i for interactive). Loads journal via `JournalParser`. Steps through actions, applying corresponding EXPECT events to `OrderbookState`, rendering via `TuiRenderer` at each step. Interactive mode: FTXUI event loop with keyboard handlers (n/p/g/q).

- [ ] **Verify build and manual test:** `bazel build //tools:exchange-viz-replay && bazel-bin/tools/exchange-viz-replay test-journals/limit_full_fill.journal`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add offline journal visualizer

CLI tool that replays .journal files step-by-step, rendering orderbook
state via FTXUI. Supports interactive mode with keyboard navigation
(next/prev/goto/quit).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 35: Shared Memory Transport

**Group:** 6 | **Dependencies:** Task 6 (spsc_ring_buffer) | **Est. Lines:** ~180 | **Dev:** Any

**Files:**
- `tools/shm_transport.h` (create)
- `tools/shm_transport.cc` (create)
- `tools/shm_transport_test.cc` (create)
- `tools/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "shm_transport",
    srcs = ["shm_transport.cc"],
    hdrs = ["shm_transport.h"],
    deps = [
        "//exchange-core:spsc_ring_buffer",
        "//test-harness:recorded_event",
    ],
    linkopts = ["-lrt"],  # POSIX shm
    visibility = ["//visibility:public"],
)

cc_test(
    name = "shm_transport_test",
    srcs = ["shm_transport_test.cc"],
    deps = [
        ":shm_transport",
        "@googletest//:gtest_main",
    ],
    linkopts = ["-lrt"],
)
```

**Steps:**

- [ ] **Write failing tests:**

```cpp
TEST(ShmTransportTest, ProducerCreatesSegment) { ... }
TEST(ShmTransportTest, ConsumerAttachesToSegment) { ... }
TEST(ShmTransportTest, PublishAndPoll) { ... }
TEST(ShmTransportTest, ProducerDestructorCleansUp) { ... }
TEST(ShmTransportTest, PollFromEmptyReturnsFalse) { ... }
```

- [ ] **Run test to verify failure:** `bazel test //tools:shm_transport_test`

- [ ] **Write implementation** -- `ShmProducer` wraps `shm_open` + `mmap` + placement new of `SpscRingBuffer<RecordedEvent, 65536>`. `ShmConsumer` wraps `shm_open` + `mmap` (attach only). RAII cleanup.

- [ ] **Run test to verify pass:** `bazel test //tools:shm_transport_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add POSIX shared memory transport for SPSC ring buffer

ShmProducer creates shared memory segment with placement-new ring buffer.
ShmConsumer attaches. RAII cleanup with munmap + shm_unlink. Supports
zero-copy inter-process event streaming.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 36: Shared Memory Listeners

**Group:** 6 | **Dependencies:** Tasks 35 (shm_transport), 5 (listeners) | **Est. Lines:** ~80 | **Dev:** Any

**Files:**
- `tools/shm_listener.h` (create)
- `tools/shm_listener_test.cc` (create -- compile test)
- `tools/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_library(
    name = "shm_listener",
    hdrs = ["shm_listener.h"],
    deps = [
        ":shm_transport",
        "//exchange-core:listeners",
        "//test-harness:recorded_event",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "shm_listener_test",
    srcs = ["shm_listener_test.cc"],
    deps = [
        ":shm_listener",
        "@googletest//:gtest_main",
    ],
    linkopts = ["-lrt"],
)
```

**Steps:**

- [ ] **Write implementation** -- thin wrappers that convert each event to `RecordedEvent` and publish via `ShmProducer::publish()`:

```cpp
class SharedMemoryOrderListener : public OrderListenerBase {
    ShmProducer& producer_;
public:
    explicit SharedMemoryOrderListener(ShmProducer& producer)
        : producer_(producer) {}
    void on_order_accepted(const OrderAccepted& e) {
        producer_.publish(RecordedEvent{e});
    }
    // ... same for all methods
};
```

Same for `SharedMemoryMdListener`.

- [ ] **Write compile/integration test** -- verify that both listeners compile and can publish through an ShmProducer.

- [ ] **Run test:** `bazel test //tools:shm_listener_test`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add shared memory order and market data listeners

Thin wrappers that publish each event to ShmProducer as RecordedEvent.
Designed for inclusion in CompositeListener for zero-virtual-overhead
hot path publishing.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 37: Live TUI Viewer

**Group:** 6 | **Dependencies:** Tasks 33 (tui_renderer), 35 (shm_transport) | **Est. Lines:** ~120 | **Dev:** Any

**Files:**
- `tools/viz_live.cc` (create)
- `tools/BUILD.bazel` (modify)

**BUILD.bazel changes:**

```starlark
cc_binary(
    name = "exchange-viz-live",
    srcs = ["viz_live.cc"],
    deps = [
        ":tui_renderer",
        ":orderbook_state",
        ":shm_transport",
        "@ftxui//...",
    ],
    linkopts = ["-lrt"],
)
```

**Steps:**

- [ ] **Write implementation** -- `main()` parses CLI args (shm name). Creates `ShmConsumer`. Enters FTXUI event loop: polls ring buffer for events, applies to `OrderbookState`, renders via `TuiRenderer`. Uses FTXUI's `Loop` with a custom `CatchEvent` to handle 'q' for quit.

- [ ] **Verify build:** `bazel build //tools:exchange-viz-live`

- [ ] **Commit:**
```bash
git commit -m "$(cat <<'EOF'
feat(tools): add live TUI viewer via shared memory

Attaches to shared memory ring buffer, polls events, reconstructs
orderbook state, renders via FTXUI in real-time. Press q to quit.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Final Checklist

After all 37 tasks are merged:

- [ ] Run full test suite: `bazel test //...`
- [ ] Verify all journal integration tests pass: `bazel test //test-harness:journal_integration_test`
- [ ] Build all binaries: `bazel build //tools:exchange-viz-replay //tools:exchange-viz-live`
- [ ] Manual smoke test: replay a journal with the offline visualizer
- [ ] Remove placeholder files: `exchange-core/exchange_core.h`, `exchange-core/exchange_core.cc`, `exchange-core/exchange_core_test.cc` and their BUILD targets (defer until all tasks reference new targets)
- [ ] Update README.md with build/run instructions
- [ ] Update `docs/` if any design decisions diverged from the spec

---

## Appendix A: Complete BUILD.bazel Reference

### `exchange-core/BUILD.bazel` (final state)

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# --- Placeholder (remove after all tasks) ---
cc_library(
    name = "exchange-core",
    srcs = ["exchange_core.cc"],
    hdrs = ["exchange_core.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "exchange_core_test",
    srcs = ["exchange_core_test.cc"],
    deps = [
        ":exchange-core",
        "@googletest//:gtest_main",
    ],
)

# --- Task 1 ---
cc_library(
    name = "types",
    hdrs = ["types.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "types_test",
    srcs = ["types_test.cc"],
    deps = [":types", "@googletest//:gtest_main"],
)

# --- Task 2 ---
cc_library(
    name = "object_pool",
    hdrs = ["object_pool.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "object_pool_test",
    srcs = ["object_pool_test.cc"],
    deps = [":object_pool", "@googletest//:gtest_main"],
)

# --- Task 3 ---
cc_library(
    name = "intrusive_list",
    hdrs = ["intrusive_list.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "intrusive_list_test",
    srcs = ["intrusive_list_test.cc"],
    deps = [":intrusive_list", "@googletest//:gtest_main"],
)

# --- Task 4 ---
cc_library(
    name = "events",
    hdrs = ["events.h"],
    deps = [":types"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "events_test",
    srcs = ["events_test.cc"],
    deps = [":events", "@googletest//:gtest_main"],
)

# --- Task 5 ---
cc_library(
    name = "listeners",
    hdrs = ["listeners.h"],
    deps = [":events"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "listeners_test",
    srcs = ["listeners_test.cc"],
    deps = [":listeners", "@googletest//:gtest_main"],
)

# --- Task 6 ---
cc_library(
    name = "spsc_ring_buffer",
    hdrs = ["spsc_ring_buffer.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "spsc_ring_buffer_test",
    srcs = ["spsc_ring_buffer_test.cc"],
    deps = [":spsc_ring_buffer", "@googletest//:gtest_main"],
)

# --- Task 7 ---
cc_library(
    name = "composite_listener",
    hdrs = ["composite_listener.h"],
    deps = [":listeners"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "composite_listener_test",
    srcs = ["composite_listener_test.cc"],
    deps = [":composite_listener", "@googletest//:gtest_main"],
)

# --- Task 8 ---
cc_library(
    name = "orderbook",
    srcs = ["orderbook.cc"],
    hdrs = ["orderbook.h"],
    deps = [":types", ":object_pool", ":intrusive_list"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "orderbook_test",
    srcs = ["orderbook_test.cc"],
    deps = [":orderbook", "@googletest//:gtest_main"],
)

# --- Task 9 ---
cc_library(
    name = "stop_book",
    srcs = ["stop_book.cc"],
    hdrs = ["stop_book.h"],
    deps = [":types", ":object_pool", ":intrusive_list"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "stop_book_test",
    srcs = ["stop_book_test.cc"],
    deps = [":stop_book", "@googletest//:gtest_main"],
)

# --- Tasks 10-11 ---
cc_library(
    name = "match_algo",
    hdrs = ["match_algo.h"],
    deps = [":types"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "match_algo_test",
    srcs = ["match_algo_test.cc"],
    deps = [":match_algo", ":intrusive_list", "@googletest//:gtest_main"],
)

# --- Tasks 12-18 ---
cc_library(
    name = "matching_engine",
    hdrs = ["matching_engine.h"],
    deps = [
        ":types",
        ":events",
        ":listeners",
        ":object_pool",
        ":intrusive_list",
        ":orderbook",
        ":stop_book",
        ":match_algo",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "matching_engine_test",
    srcs = ["matching_engine_test.cc"],
    deps = [":matching_engine", "@googletest//:gtest_main"],
)
```

### `test-harness/BUILD.bazel` (final state)

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# --- Task 19 ---
cc_library(
    name = "recorded_event",
    hdrs = ["recorded_event.h"],
    deps = ["//exchange-core:events"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "recorded_event_test",
    srcs = ["recorded_event_test.cc"],
    deps = [":recorded_event", "@googletest//:gtest_main"],
)

# --- Task 20 ---
cc_library(
    name = "recording_listener",
    hdrs = ["recording_listener.h"],
    deps = [":recorded_event", "//exchange-core:listeners"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "recording_listener_test",
    srcs = ["recording_listener_test.cc"],
    deps = [":recording_listener", "@googletest//:gtest_main"],
)

# --- Task 21 ---
cc_library(
    name = "journal_parser",
    srcs = ["journal_parser.cc"],
    hdrs = ["journal_parser.h"],
    deps = ["//exchange-core:types", "//exchange-core:events"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "journal_parser_test",
    srcs = ["journal_parser_test.cc"],
    deps = [":journal_parser", "@googletest//:gtest_main"],
)

# --- Task 22 ---
cc_library(
    name = "journal_writer",
    srcs = ["journal_writer.cc"],
    hdrs = ["journal_writer.h"],
    deps = [":recorded_event", ":journal_parser"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "journal_writer_test",
    srcs = ["journal_writer_test.cc"],
    deps = [":journal_writer", ":journal_parser", "@googletest//:gtest_main"],
)

# --- Task 23 ---
cc_library(
    name = "test_runner",
    srcs = ["test_runner.cc"],
    hdrs = ["test_runner.h"],
    deps = [
        ":recorded_event",
        ":recording_listener",
        ":journal_parser",
        "//exchange-core:matching_engine",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "test_runner_test",
    srcs = ["test_runner_test.cc"],
    deps = [":test_runner", "@googletest//:gtest_main"],
)

# --- Task 31 ---
cc_test(
    name = "journal_integration_test",
    srcs = ["journal_integration_test.cc"],
    data = ["//test-journals:all_journals"],
    deps = [
        ":test_runner",
        ":journal_parser",
        ":recording_listener",
        "//exchange-core:matching_engine",
        "@googletest//:gtest_main",
    ],
)
```

### `test-journals/BUILD.bazel` (final state)

```starlark
filegroup(
    name = "all_journals",
    srcs = glob(["*.journal"]),
    visibility = ["//visibility:public"],
)
```

### `tools/BUILD.bazel` (final state)

```starlark
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# --- Task 32 ---
cc_library(
    name = "orderbook_state",
    srcs = ["orderbook_state.cc"],
    hdrs = ["orderbook_state.h"],
    deps = ["//test-harness:recorded_event"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "orderbook_state_test",
    srcs = ["orderbook_state_test.cc"],
    deps = [":orderbook_state", "@googletest//:gtest_main"],
)

# --- Task 33 ---
cc_library(
    name = "tui_renderer",
    srcs = ["tui_renderer.cc"],
    hdrs = ["tui_renderer.h"],
    deps = [
        ":orderbook_state",
        "@ftxui//:screen",
        "@ftxui//:dom",
        "@ftxui//:component",
    ],
    visibility = ["//visibility:public"],
)

# --- Task 34 ---
cc_binary(
    name = "exchange-viz-replay",
    srcs = ["viz_replay.cc"],
    deps = [
        ":tui_renderer",
        ":orderbook_state",
        "//test-harness:journal_parser",
        "//test-harness:recorded_event",
        "@ftxui//:screen",
        "@ftxui//:dom",
        "@ftxui//:component",
    ],
)

# --- Task 35 ---
cc_library(
    name = "shm_transport",
    srcs = ["shm_transport.cc"],
    hdrs = ["shm_transport.h"],
    deps = [
        "//exchange-core:spsc_ring_buffer",
        "//test-harness:recorded_event",
    ],
    linkopts = ["-lrt"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "shm_transport_test",
    srcs = ["shm_transport_test.cc"],
    deps = [":shm_transport", "@googletest//:gtest_main"],
    linkopts = ["-lrt"],
)

# --- Task 36 ---
cc_library(
    name = "shm_listener",
    hdrs = ["shm_listener.h"],
    deps = [
        ":shm_transport",
        "//exchange-core:listeners",
        "//test-harness:recorded_event",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "shm_listener_test",
    srcs = ["shm_listener_test.cc"],
    deps = [":shm_listener", "@googletest//:gtest_main"],
    linkopts = ["-lrt"],
)

# --- Task 37 ---
cc_binary(
    name = "exchange-viz-live",
    srcs = ["viz_live.cc"],
    deps = [
        ":tui_renderer",
        ":orderbook_state",
        ":shm_transport",
        "@ftxui//:screen",
        "@ftxui//:dom",
        "@ftxui//:component",
    ],
    linkopts = ["-lrt"],
)
```

### `MODULE.bazel` (final state)

```starlark
module(
    name = "exchange",
    version = "0.1.0",
)

bazel_dep(name = "rules_cc", version = "0.2.17")
bazel_dep(name = "googletest", version = "1.17.0")
bazel_dep(name = "ftxui", version = "6.0.2")
```
