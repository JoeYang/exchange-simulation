# Exchange Core — Design Specification

**Date:** 2026-03-24
**Phase:** 1 — Core matching engine and test harness
**Status:** Draft

---

## 1. Overview

A high-performance C++ order matching engine focused on low latency and zero allocation
on the hot path. The engine handles order entry, orderbook matching, and publishes order
lifecycle events and market data via compile-time callbacks.

**Design principles:**
- Single orderbook per engine instance, single-threaded event loop
- Zero heap allocation after initialization (pre-allocated object pools)
- All polymorphism via CRTP — no virtual dispatch on the hot path
- Plain C++ structs for internal data modeling
- Nanosecond timestamps throughout
- Deterministic: same input sequence produces identical output, every time
- All callbacks fire synchronously within the calling method (new_order/cancel_order/modify_order)
- Callers must NOT re-enter the engine from within a callback

**Namespace:** All types live in `namespace exchange`.

---

## 2. Data Representation

### 2.1 Type Aliases

```cpp
using Price    = int64_t;   // fixed-point, 4 decimal places (100.5000 = 1005000)
using Quantity = int64_t;   // fixed-point, 4 decimal places (1.0000 = 10000)
using OrderId  = uint64_t;  // engine-assigned, sequential starting at 1
using Timestamp = int64_t;  // epoch nanoseconds

constexpr int64_t PRICE_SCALE = 10000;
```

### 2.2 Enumerations

```cpp
enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market, Stop, StopLimit };
enum class TimeInForce : uint8_t { DAY, GTC, IOC, FOK, GTD };
enum class MatchAlgo : uint8_t { FIFO, ProRata };
enum class SmpAction : uint8_t { CancelNewest, CancelOldest, CancelBoth, Decrement, None };
enum class ModifyPolicy : uint8_t { CancelReplace, AmendDown, RejectModify };
enum class RejectReason : uint8_t {
    PoolExhausted, InvalidPrice, InvalidQuantity, InvalidTif, InvalidSide,
    UnknownOrder, PriceBandViolation, LevelPoolExhausted, ExchangeSpecific
};
enum class CancelReason : uint8_t {
    UserRequested, IOCRemainder, FOKFailed, Expired, SelfMatchPrevention, LevelPoolExhausted
};
```

Note: `DuplicateClOrdId` detection is the protocol layer's responsibility, not the core engine's.
The engine treats `client_order_id` as opaque and never deduplicates.

### 2.3 Core Structs

Both `Order` and `PriceLevel` are defined in the same header (`types.h`) to resolve the
circular pointer dependency without forward declarations across files.

```cpp
struct PriceLevel;  // forward declaration

struct Order {
    OrderId id;                  // engine-assigned sequential
    uint64_t client_order_id;    // caller-provided, opaque, echoed in callbacks
    uint64_t account_id;         // for SMP grouping
    Price price;
    Quantity quantity;
    Quantity filled_quantity;
    Quantity remaining_quantity;  // quantity - filled_quantity
    Side side;
    OrderType type;
    TimeInForce tif;
    Timestamp timestamp;
    Timestamp gtd_expiry;        // only used for GTD orders

    // intrusive doubly-linked list hooks (within a price level)
    Order* prev;
    Order* next;

    // back-pointer to owning price level
    PriceLevel* level;
};

struct PriceLevel {
    Price price;
    Quantity total_quantity;      // sum of all remaining quantities at this level
    uint32_t order_count;

    Order* head;                 // first order (oldest in FIFO)
    Order* tail;                 // last order (newest)

    // intrusive doubly-linked list hooks (within bid/ask side)
    PriceLevel* prev;
    PriceLevel* next;
};

struct FillResult {
    Order* resting_order;        // the resting order that was filled
    Price price;                 // fill price
    Quantity quantity;            // fill quantity
    Quantity resting_remaining;  // resting order remaining after fill
};
```

---

## 3. Object Pool

```cpp
template <typename T, size_t Capacity>
class ObjectPool {
    std::array<T, Capacity> storage_;
    T* free_head_;               // free list threaded through storage
    size_t allocated_{0};

public:
    ObjectPool();                // initializes free list
    T* allocate();               // pop from free list, nullptr if exhausted
    void deallocate(T* p);       // push back to free list
    size_t available() const;
    size_t capacity() const;
    void reset();                // return all objects to free list
};
```

- Entire pool lives as a member (stack or embedding struct), zero heap allocation
- Free list threaded through the storage array using an intrusive `next` pointer
- `allocate()` and `deallocate()` are O(1)
- When pool is exhausted, `new_order` rejects with `RejectReason::PoolExhausted`

---

## 4. Orderbook Structure

### 4.1 Layout

```
Bids (descending price):         Asks (ascending price):
  best_bid_ -->                    best_ask_ -->
  [100.50] -> [O1]-[O2]-[O3]      [100.75] -> [O5]-[O6]
  [100.25] -> [O4]                 [101.00] -> [O7]-[O8]-[O9]
  [100.00] -> [O10]                [101.25] -> [O11]
```

- Two sorted intrusive doubly-linked lists of `PriceLevel` nodes
- Bids sorted descending: `best_bid_` points to highest price level
- Asks sorted ascending: `best_ask_` points to lowest price level
- Each `PriceLevel` contains an intrusive doubly-linked list of `Order` nodes in FIFO order
- `best_bid_` and `best_ask_` are O(1) access

### 4.2 Stop Book

Separate structure for stop orders — not visible on the orderbook.

```
Buy stops (ascending):            Sell stops (descending):
  [105.00] -> [S1]-[S2]            [99.00] -> [S4]-[S5]
  [106.00] -> [S3]                  [98.00] -> [S6]
```

- Same intrusive linked list structure as the main book
- Triggered when `last_trade_price >= lowest_buy_stop` or `last_trade_price <= highest_sell_stop`
- Triggered stops convert to Market (Stop) or Limit (StopLimit) orders and re-enter via `new_order`
- **Trigger cascade:** After each triggered stop produces a fill, re-check the stop book against
  the new `last_trade_price`. Repeat iteratively (not recursively) until no more stops trigger.
  This means a single incoming order can cause a chain of stop triggers and fills.
- Stop orders share the same `ObjectPool` — no separate allocation

### 4.3 Order Index

```cpp
std::array<Order*, MaxOrderIds> order_index_;
```

- Direct-index array: `order_index_[order_id]` = pointer to `Order` in pool
- O(1) guaranteed lookup for cancel/modify
- OrderIds are sequential starting at 1, so no hashing or collision handling
- Null entry = order does not exist (filled, cancelled, or never created)

**Important:** `MaxOrderIds` (index size) and `MaxOrders` (pool capacity) are separate template
parameters. `MaxOrders` is the maximum number of **concurrent** active orders (pool recycles slots).
`MaxOrderIds` is the maximum **lifetime** order count before the engine must be restarted.
Typical configuration: `MaxOrders = 100,000`, `MaxOrderIds = 1,000,000`.

---

## 5. Matching Algorithms

### 5.1 FIFO

Walk the resting side from best price. At each price level, fill orders from head
to tail (time priority). Fill each order fully before moving to the next. Advance to
next price level when current level is exhausted.

### 5.2 Pro-Rata

At each matchable price level, allocate the aggressor quantity proportionally across
all resting orders based on their remaining quantity:

```
allocation[i] = floor(order[i].remaining_quantity * aggressor_remaining / level.total_quantity)
```

Note: multiply first, divide second to avoid premature truncation. Orders receiving a
zero allocation are skipped and do not participate in the remainder round.

Remainder lots (due to integer rounding) are distributed one lot at a time in FIFO order
(time priority as secondary sort).

### 5.3 Policy Implementation

```cpp
struct FifoMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count);
};

struct ProRataMatch {
    static void match(PriceLevel& level, Quantity& remaining,
                      FillResult* results, size_t& count);
};
```

Matching algorithm is a compile-time template parameter. An exchange needing both FIFO
and pro-rata instruments creates two engine types.

---

## 6. Callback Events

### 6.1 Order Events

```cpp
struct OrderAccepted        { OrderId id; uint64_t client_order_id; Timestamp ts; };
struct OrderRejected        { uint64_t client_order_id; Timestamp ts; RejectReason reason; };
struct OrderFilled          { OrderId aggressor_id; OrderId resting_id; Price price; Quantity quantity; Timestamp ts; };
struct OrderPartiallyFilled { OrderId aggressor_id; OrderId resting_id; Price price; Quantity quantity; Quantity aggressor_remaining; Quantity resting_remaining; Timestamp ts; };
struct OrderCancelled       { OrderId id; Timestamp ts; CancelReason reason; };
struct OrderCancelRejected  { OrderId id; uint64_t client_order_id; Timestamp ts; RejectReason reason; };
struct OrderModified        { OrderId id; uint64_t client_order_id; Price new_price; Quantity new_qty; Timestamp ts; };
struct OrderModifyRejected  { OrderId id; uint64_t client_order_id; Timestamp ts; RejectReason reason; };
```

Note: `OrderExpired` is not a separate event. Expiry fires `OrderCancelled` with
`CancelReason::Expired`. This avoids redundant event types.

### 6.2 Market Data Events

```cpp
struct TopOfBook {                        // L1
    Price best_bid; Quantity bid_qty;
    Price best_ask; Quantity ask_qty;
    Timestamp ts;
};

struct DepthUpdate {                      // L2
    Side side; Price price;
    Quantity total_qty; uint32_t order_count;
    enum Action : uint8_t { Add, Update, Remove } action;
    Timestamp ts;
};

struct OrderBookAction {                  // L3
    OrderId id; Side side; Price price; Quantity qty;
    enum Action : uint8_t { Add, Modify, Cancel, Fill } action;
    Timestamp ts;
};

struct Trade {
    Price price; Quantity quantity;
    OrderId aggressor_id; OrderId resting_id;
    Side aggressor_side;
    Timestamp ts;
};
```

### 6.3 Callback Ordering Contract

For each engine operation, callbacks fire in a guaranteed order:

**new_order (limit that fills and rests):**
1. `OrderAccepted`
2. For each fill (in match order):
   a. `OrderPartiallyFilled` or `OrderFilled`
   b. `Trade`
   c. `OrderBookAction` (L3, resting order fill)
   d. `DepthUpdate` (L2, resting side level change)
3. `OrderBookAction` (L3, aggressor order add — if resting remainder)
4. `DepthUpdate` (L2, aggressor side level add — if resting remainder)
5. `TopOfBook` (L1, if best bid/ask changed)

**cancel_order:**
1. `OrderCancelled`
2. `OrderBookAction` (L3, cancel)
3. `DepthUpdate` (L2, level update/remove)
4. `TopOfBook` (L1, if best bid/ask changed)

**modify_order (cancel-replace):**
1. `OrderBookAction` (L3, cancel old)
2. `DepthUpdate` (L2, old level update/remove)
3. `OrderModified`
4. Any fills (same sequence as new_order fills)
5. `OrderBookAction` (L3, add new — if resting)
6. `DepthUpdate` (L2, new level add/update — if resting)
7. `TopOfBook` (L1, if best bid/ask changed)

### 6.4 Listener Interfaces

Listener base classes provide default no-op implementations. Derived classes override
methods by name hiding. No CRTP dispatch needed — the engine calls methods on the
concrete listener type directly via its template parameter.

```cpp
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
```

### 6.5 Composite Listener (Fan-Out)

```cpp
template <typename... Listeners>
class CompositeOrderListener : public OrderListenerBase {
    std::tuple<Listeners...> listeners_;
public:
    void on_order_filled(const OrderFilled& e) {
        std::apply([&](auto&... l) { (l.on_order_filled(e), ...); }, listeners_);
    }
    // ... same fold expression for each event
};
```

Zero virtual overhead. Listener set fixed at compile time. Exchange implementations
can include a dynamic-dispatch listener in the composite if runtime registration is needed.

---

## 7. Matching Engine

### 7.1 Configuration

```cpp
struct EngineConfig {
    Price tick_size;            // minimum price increment
    Quantity lot_size;          // minimum quantity increment
    Price price_band_low;      // reject orders below this (0 = no band)
    Price price_band_high;     // reject orders above this (0 = no band)
};
```

### 7.2 Template Signature

```cpp
template <
    typename Derived,                           // CRTP exchange implementation
    typename OrderListenerT,                    // order event callbacks
    typename MdListenerT,                       // market data callbacks
    typename MatchAlgoT = FifoMatch,            // matching algorithm policy
    size_t MaxOrders = 100000,                  // concurrent order pool capacity
    size_t MaxPriceLevels = 10000,              // price level pool capacity
    size_t MaxOrderIds = 1000000                // lifetime order ID space
>
class MatchingEngine {
    EngineConfig config_;
    ObjectPool<Order, MaxOrders> order_pool_;
    ObjectPool<PriceLevel, MaxPriceLevels> level_pool_;

    PriceLevel* best_bid_{nullptr};
    PriceLevel* best_ask_{nullptr};
    PriceLevel* buy_stops_{nullptr};
    PriceLevel* sell_stops_{nullptr};

    Price last_trade_price_{0};
    OrderId next_order_id_{1};

    OrderListenerT& order_listener_;
    MdListenerT& md_listener_;

    std::array<Order*, MaxOrderIds> order_index_{};

public:
    MatchingEngine(EngineConfig config,
                   OrderListenerT& order_listener,
                   MdListenerT& md_listener);

    void new_order(const OrderRequest& req);
    void cancel_order(OrderId id, Timestamp ts);
    void modify_order(const ModifyRequest& req);
    void trigger_expiry(Timestamp now, TimeInForce tif);

    // Status queries (not hot path)
    size_t active_order_count() const;
    size_t available_order_slots() const;
    size_t available_level_slots() const;
};
```

### 7.3 Request Structs

```cpp
struct OrderRequest {
    uint64_t client_order_id;
    uint64_t account_id;
    Side side;
    OrderType type;
    TimeInForce tif;
    Price price;                // ignored for Market orders
    Quantity quantity;
    Price stop_price;           // only for Stop/StopLimit
    Timestamp timestamp;
    Timestamp gtd_expiry;       // only for GTD
};

struct ModifyRequest {
    OrderId order_id;
    uint64_t client_order_id;   // echoed in OrderModified/OrderModifyRejected
    Price new_price;
    Quantity new_quantity;
    Timestamp timestamp;
};
```

### 7.4 Order Processing Flow

```
new_order(req)
  |
  +-> derived().on_validate_order(req)           -- reject if false
  |
  +-> derived().is_tif_valid(req.tif)            -- reject if false
  |
  +-> validate tick_size, lot_size, price_band   -- reject if invalid
  |
  +-> validate quantity > 0, price > 0 (limit)   -- reject if invalid
  |
  +-> check next_order_id_ < MaxOrderIds         -- reject if exhausted
  |
  +-> allocate Order from pool                   -- reject if exhausted
  |
  +-> assign OrderId (sequential)
  |
  +-> fire OrderAccepted callback
  |
  +-> if Stop or StopLimit:
  |     insert into stop book
  |     return
  |
  +-> match_order(order):
  |     for each price level on the opposite side at matchable price:
  |       MatchAlgoT::match(level, remaining, results, count)
  |         for each fill:
  |           derived().is_self_match(aggressor, resting)
  |             -> if true:  apply derived().get_smp_action()
  |             -> if false: execute fill
  |           fire OrderFilled / OrderPartiallyFilled
  |           fire Trade
  |           fire OrderBookAction (L3)
  |           fire DepthUpdate (L2)
  |           update level totals, remove if empty
  |       check and trigger stops after trade (iterative cascade)
  |
  +-> post-match:
  |     FOK and not fully filled -> cancel entire order (undo all fills)
  |     IOC and has remaining    -> cancel remaining
  |     Limit with remaining     -> insert into book
  |       -> if level_pool_ exhausted: cancel remainder with LevelPoolExhausted
  |     Market with remaining    -> cancel remaining (no resting market)
  |     fire TopOfBook (L1) if best bid/ask changed
```

**trigger_expiry flow:**

```
trigger_expiry(now, tif)
  |
  +-> linear scan of order_index_
  |     for each active order where order.tif == tif:
  |       if tif == DAY:  cancel order
  |       if tif == GTD and order.gtd_expiry <= now:  cancel order
  |       fire OrderCancelled with CancelReason::Expired
  |       fire OrderBookAction, DepthUpdate, TopOfBook as needed
```

This is O(n) over the order index. For production use with very large `MaxOrderIds`,
a sorted expiry queue is a future optimization.

### 7.5 CRTP Extension Points

| Hook | Signature | Default | Purpose |
|------|-----------|---------|---------|
| `is_self_match` | `bool(const Order&, const Order&)` | `return false` (no SMP) | Returns true if orders belong to same entity |
| `get_smp_action` | `SmpAction()` | `CancelNewest` | What to do when SMP triggers |
| `get_modify_policy` | `ModifyPolicy()` | `CancelReplace` | Modify behavior |
| `on_validate_order` | `bool(const OrderRequest&)` | `return true` | Exchange-specific validation |
| `is_tif_valid` | `bool(TimeInForce)` | `return true` | Session state validation |
| `should_trigger_stop` | `bool(Price, const Order&)` | Standard comparison | Custom stop logic |

---

## 8. Test Harness — Journal Replay Framework

### 8.1 Purpose

Deterministic, reproducible testing of the matching engine via journaled action sequences.
Supports both hand-written test scenarios and recorded production replays.

### 8.2 Architecture

```
                  +--- actions --->  MatchingEngine
journal file ----|                       |
                  +--- expects           v
                        |         RecordingListener
                        |              |
                        v              v
                     [expected]  == [recorded]  --> PASS/FAIL with diff
```

### 8.3 Journal File Format

Line-based text format. Each line is either a CONFIG (engine setup), ACTION (input to
engine), or EXPECT (expected callback output). Comments start with `#`. Blank lines ignored.

**Header directives:**

```
CONFIG match_algo=FIFO max_orders=1000 max_levels=100 max_order_ids=10000
CONFIG tick_size=100 lot_size=10000 price_band_low=0 price_band_high=0
```

The test runner uses CONFIG to instantiate the correct engine type. `match_algo` determines
whether to use FifoMatch or ProRataMatch. If CONFIG is omitted, defaults are used (FIFO,
standard pool sizes).

**Example journal:**

```
# Basic limit order fill
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

ACTION CANCEL ts=3000 ord_id=1
EXPECT ORDER_CANCEL_REJECTED ord_id=1 ts=3000 reason=UNKNOWN_ORDER
```

**Supported actions:**

| Action | Fields |
|--------|--------|
| `NEW_ORDER` | ts, cl_ord_id, account_id, side, price, qty, type, tif, [stop_price], [gtd_expiry] |
| `CANCEL` | ts, ord_id |
| `MODIFY` | ts, ord_id, cl_ord_id, new_price, new_qty |
| `TRIGGER_EXPIRY` | ts, tif |

**Supported expects:** All callback event types from Section 6.

### 8.4 Recording Listener

```cpp
class RecordingOrderListener : public OrderListenerBase {
    std::vector<RecordedEvent> events_;
public:
    void on_order_accepted(const OrderAccepted& e);
    void on_order_rejected(const OrderRejected& e);
    // ... captures all events into events_ vector
    const std::vector<RecordedEvent>& events() const;
    void clear();
};

class RecordingMdListener : public MarketDataListenerBase {
    std::vector<RecordedEvent> events_;
public:
    void on_top_of_book(const TopOfBook& e);
    void on_depth_update(const DepthUpdate& e);
    // ... captures all events into events_ vector
    const std::vector<RecordedEvent>& events() const;
    void clear();
};
```

Both listeners record into the same `RecordedEvent` variant type with a shared event
cursor for ordering. The recording listeners allocate (std::vector), which is acceptable
since they are test infrastructure, not production hot path.

### 8.5 Journal Parser

```cpp
class JournalParser {
public:
    struct ParsedAction { /* action type + fields */ };
    struct ParsedExpectation { /* event type + fields */ };
    struct ParsedConfig { /* engine configuration fields */ };
    struct JournalEntry {
        std::vector<ParsedAction> actions;         // grouped: one action
        std::vector<ParsedExpectation> expectations; // followed by its expects
    };

    struct Journal {
        ParsedConfig config;
        std::vector<JournalEntry> entries;
    };

    static Journal parse(const std::string& file_path);
};
```

### 8.6 Test Runner

```cpp
class JournalTestRunner {
public:
    template <typename EngineT>
    TestResult run(EngineT& engine,
                   RecordingOrderListener& order_listener,
                   RecordingMdListener& md_listener,
                   const std::vector<JournalParser::JournalEntry>& journal);
};

struct TestResult {
    bool passed;
    size_t action_index;         // which action failed (if any)
    size_t event_index;          // which expected event mismatched
    std::string expected;        // string repr of expected event
    std::string actual;          // string repr of actual event
    std::string diff;            // human-readable diff
};
```

### 8.7 Record Mode

The recording listeners can serialize their captured events back to journal format.
This enables:

1. Run the engine with a set of actions (no EXPECT lines)
2. Capture all callback output
3. Write a new journal file with the recorded events as EXPECT lines
4. Use that journal as a regression baseline

```cpp
class JournalWriter {
public:
    static void write(const std::string& path,
                      const JournalParser::ParsedConfig& config,
                      const std::vector<JournalParser::ParsedAction>& actions,
                      const std::vector<RecordedEvent>& recorded_events);
};
```

### 8.8 Test Scenarios

The following journal files cover all engine behavior:

**Happy path:**
- `basic_limit_buy.journal` — single limit buy, rests on book
- `basic_limit_sell.journal` — single limit sell, rests on book
- `limit_full_fill.journal` — two opposite limits at same price, full fill
- `limit_partial_fill.journal` — partial fill, remainder rests
- `market_order_fill.journal` — market order fills against resting limit
- `market_order_no_liquidity.journal` — market order with empty book, cancelled
- `market_order_sweep.journal` — large market order fills across 3+ price levels
- `multiple_fills.journal` — aggressor fills across multiple price levels

**Order types:**
- `stop_trigger.journal` — stop order triggers on trade
- `stop_limit_trigger.journal` — stop-limit triggers and rests
- `stop_cascade.journal` — triggered stop causes fill which triggers more stops (multi-level)
- `cancel_stop_order.journal` — cancel a stop order before it triggers
- `modify_stop_order.journal` — modify a stop order before it triggers

**Time in force:**
- `ioc_full_fill.journal` — IOC fully filled
- `ioc_partial_cancel.journal` — IOC partially filled, remainder cancelled
- `fok_full_fill.journal` — FOK fully filled
- `fok_no_fill.journal` — FOK cannot fill completely, entire order cancelled
- `fok_multi_level.journal` — FOK with sufficient total qty across levels but tests all-or-nothing
- `gtd_expiry.journal` — GTD order expires via trigger_expiry
- `gtd_not_expired.journal` — GTD order survives trigger_expiry when now < gtd_expiry
- `day_expiry.journal` — DAY order expires

**Matching algorithms:**
- `fifo_priority.journal` — earlier orders at same price fill first
- `pro_rata_basic.journal` — proportional allocation
- `pro_rata_remainder.journal` — remainder distributed by time priority
- `pro_rata_single_order.journal` — single order at level, gets full fill
- `pro_rata_equal_sizes.journal` — all orders same size, degenerates to FIFO for remainder

**Cancel and modify:**
- `cancel_resting.journal` — cancel a resting order
- `cancel_unknown.journal` — cancel nonexistent order, rejected
- `cancel_already_filled.journal` — cancel already-filled order, rejected
- `modify_price_change.journal` — modify price, loses priority
- `modify_qty_down.journal` — modify quantity down (for amend-down policy)
- `modify_qty_up.journal` — modify quantity up, loses priority
- `modify_triggers_fill.journal` — cancel-replace to more aggressive price crosses spread
- `modify_same_price.journal` — modify to same price (cancel-replace still loses priority)
- `modify_unknown.journal` — modify nonexistent order, rejected

**Edge cases and failures:**
- `pool_exhaustion_orders.journal` — exceed MaxOrders, new orders rejected
- `pool_exhaustion_levels.journal` — exceed MaxPriceLevels, remainder cancelled
- `self_match_prevention.journal` — SMP triggers cancel newest
- `price_band_reject.journal` — order outside price band rejected
- `tick_size_reject.journal` — price not aligned to tick size rejected
- `lot_size_reject.journal` — quantity not aligned to lot size rejected
- `zero_quantity.journal` — zero qty order rejected
- `zero_price_limit.journal` — zero price limit order rejected
- `negative_price.journal` — negative price order rejected
- `empty_book_cancel.journal` — cancel on empty book
- `market_order_tif.journal` — market order ignores TIF (always IOC-like behavior)

**Market data callbacks:**
- `l1_updates.journal` — verify TopOfBook after each action
- `l2_depth_updates.journal` — verify DepthUpdate add/update/remove
- `l3_order_actions.journal` — verify OrderBookAction for every order event
- `trade_events.journal` — verify Trade events on fills

---

## 9. SPSC Ring Buffer

A lock-free single-producer single-consumer ring buffer for zero-copy inter-process and
inter-thread communication. Lives in exchange-core as a reusable primitive.

### 9.1 Design

```cpp
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    alignas(64) std::atomic<size_t> write_pos_{0};  // cache-line aligned
    alignas(64) std::atomic<size_t> read_pos_{0};   // separate cache line
    alignas(64) std::array<T, Capacity> buffer_;

public:
    bool try_push(const T& item);    // returns false if full
    bool try_pop(T& item);           // returns false if empty
    size_t size() const;
    bool empty() const;
    bool full() const;
};
```

**Key properties:**
- Lock-free: uses `std::atomic` with acquire/release semantics, no mutexes
- Cache-friendly: write_pos and read_pos on separate cache lines to avoid false sharing
- Power-of-2 capacity: enables bitwise modulo (`pos & (Capacity - 1)`) instead of division
- Fixed-size: entire buffer is pre-allocated, zero heap allocation after construction
- Single producer, single consumer: no CAS loops needed, just load/store with memory ordering

### 9.2 Shared Memory Transport

For inter-process visualization, the ring buffer is placed in POSIX shared memory:

```cpp
// Producer side (engine process)
class ShmProducer {
    int fd_;
    void* mapped_;
    SpscRingBuffer<RecordedEvent, 65536>* ring_;

public:
    ShmProducer(const std::string& shm_name);  // shm_open + mmap + placement new
    ~ShmProducer();                              // munmap + shm_unlink
    bool publish(const RecordedEvent& event);
};

// Consumer side (viewer process)
class ShmConsumer {
    int fd_;
    void* mapped_;
    SpscRingBuffer<RecordedEvent, 65536>* ring_;

public:
    ShmConsumer(const std::string& shm_name);   // shm_open + mmap
    ~ShmConsumer();                              // munmap + close
    bool poll(RecordedEvent& event);
};
```

The `RecordedEvent` struct must be trivially copyable for shared memory transport. This is
already the case since all event fields are plain integers and enums.

---

## 10. Visualization Tools

### 10.1 Offline Journal Visualizer

A CLI tool that reads `.journal` files and replays them step-by-step, rendering the
orderbook state at each action.

**Usage:**

```bash
exchange-viz replay test-journals/limit_partial_fill.journal     # auto-step
exchange-viz replay test-journals/limit_partial_fill.journal -i  # interactive (keyboard step)
```

**Display layout (FTXUI):**

```
┌─ Orderbook ──────────────────────┐  ┌─ Recent Trades ──────────┐
│  BIDS            │  ASKS          │  │ 100.50 x 3   [id2 x id1]│
│  100.50    7  (1)│  101.00  10 (1)│  │                          │
│  100.25    5  (1)│  101.50  20 (2)│  │                          │
│  100.00   15  (3)│  102.00   5 (1)│  │                          │
└──────────────────┴────────────────┘  └──────────────────────────┘
┌─ Order Events ────────────────────────────────────────────────────┐
│ [ts=2000] ORDER_ACCEPTED id=2 cl_ord_id=2                        │
│ [ts=2000] PARTIAL_FILL aggressor=2 resting=1 @ 100.50 qty=3      │
│ [ts=2000] TRADE 100.50 x 3                                       │
└───────────────────────────────────────────────────────────────────┘
┌─ Status ──────────────────────────────────────────────────────────┐
│ Action 3/15 │ Orders: 4/1000 │ Levels: 6/100 │ [n]ext [p]rev [q]│
└───────────────────────────────────────────────────────────────────┘
```

**Features:**
- Reconstructs orderbook state from L2/L3 events at each step
- Interactive mode: `n` = next action, `p` = previous (rewind by replaying from start),
  `q` = quit, `g` = go to action number
- Shows bid/ask depth with order count per level
- Shows recent trades
- Shows order event log
- Status bar with pool utilization and navigation

**Implementation:** The visualizer reuses `JournalParser` from the test harness. It does
NOT depend on the matching engine — it reconstructs state purely from the journal events.
This means it works for both recorded production journals and hand-written test journals.

### 10.2 Live TUI Viewer

A separate process that reads events from a shared memory ring buffer and renders the
same TUI display in real-time.

**Usage:**

```bash
exchange-viz live /exchange-events    # reads from shm ring buffer
```

**Shared Memory Listener:**

```cpp
class SharedMemoryOrderListener : public OrderListenerBase {
    ShmProducer& producer_;
public:
    explicit SharedMemoryOrderListener(ShmProducer& producer);
    void on_order_accepted(const OrderAccepted& e) {
        producer_.publish(RecordedEvent{e});
    }
    // ... same for all events
};

class SharedMemoryMdListener : public MarketDataListenerBase {
    ShmProducer& producer_;
public:
    explicit SharedMemoryMdListener(ShmProducer& producer);
    void on_top_of_book(const TopOfBook& e) {
        producer_.publish(RecordedEvent{e});
    }
    // ... same for all events
};
```

These listeners are included in a `CompositeListener` alongside the production listeners.
The cost on the hot path is one `try_push` per event (a single atomic store if the buffer
is not full — typically < 10ns).

**Viewer architecture:**

```
Engine process:                         Viewer process:
  MatchingEngine                          exchange-viz live /exchange-events
    |                                          |
    v                                          v
  CompositeListener                       ShmConsumer
    |-> ProductionListener                     |
    |-> SharedMemoryOrderListener ──shm──>  poll loop
    |-> SharedMemoryMdListener   ──shm──>     |
                                               v
                                          FTXUI TUI render
```

The viewer polls the ring buffer in a loop, reconstructs orderbook state from events,
and re-renders the FTXUI display. If the viewer falls behind, events are dropped (ring
buffer full → `try_push` returns false). This is acceptable for visualization — the viewer
catches up on the next successful read.

**Shared TUI code:** Both the offline visualizer and live viewer share the same FTXUI
rendering components. The only difference is the event source (journal parser vs. shm consumer).

---

## 11. Directory Structure

```
exchange/
├── MODULE.bazel
├── .bazelrc
├── BUILD.bazel
├── exchange-core/
│   ├── BUILD.bazel
│   ├── types.h                  # Price, Quantity, OrderId, Timestamp, enums, Order, PriceLevel, FillResult
│   ├── object_pool.h            # ObjectPool<T, Capacity>
│   ├── object_pool_test.cc
│   ├── intrusive_list.h         # Intrusive doubly-linked list operations
│   ├── intrusive_list_test.cc
│   ├── spsc_ring_buffer.h       # Lock-free SPSC ring buffer
│   ├── spsc_ring_buffer_test.cc
│   ├── events.h                 # All callback event structs
│   ├── listeners.h              # OrderListenerBase, MarketDataListenerBase
│   ├── composite_listener.h     # CompositeOrderListener, CompositeMdListener
│   ├── composite_listener_test.cc
│   ├── orderbook.h              # OrderBook (bid/ask price level management)
│   ├── orderbook.cc
│   ├── orderbook_test.cc
│   ├── stop_book.h              # Stop order management
│   ├── stop_book.cc
│   ├── stop_book_test.cc
│   ├── match_algo.h             # FifoMatch, ProRataMatch policies
│   ├── match_algo_test.cc
│   ├── matching_engine.h        # MatchingEngine template
│   └── matching_engine_test.cc  # Unit tests using direct C++ assertions
│
├── test-harness/
│   ├── BUILD.bazel
│   ├── recorded_event.h         # RecordedEvent variant type
│   ├── recording_listener.h     # RecordingOrderListener, RecordingMdListener
│   ├── recording_listener_test.cc
│   ├── journal_parser.h         # Parse .journal files
│   ├── journal_parser.cc
│   ├── journal_parser_test.cc
│   ├── journal_writer.h         # Write recorded events to .journal format
│   ├── journal_writer.cc
│   ├── journal_writer_test.cc
│   ├── test_runner.h            # Replay journal + assert
│   ├── test_runner.cc
│   └── test_runner_test.cc
│
├── test-journals/
│   └── *.journal                # Test scenario files (see Section 8.8)
│
├── tools/
│   ├── BUILD.bazel
│   ├── shm_transport.h          # ShmProducer, ShmConsumer (POSIX shm wrappers)
│   ├── shm_transport.cc
│   ├── shm_transport_test.cc
│   ├── shm_listener.h           # SharedMemoryOrderListener, SharedMemoryMdListener
│   ├── tui_renderer.h           # Shared FTXUI rendering components
│   ├── tui_renderer.cc
│   ├── orderbook_state.h        # Reconstructed orderbook state from events
│   ├── orderbook_state.cc
│   ├── orderbook_state_test.cc
│   ├── viz_replay.cc            # Offline journal visualizer (main)
│   └── viz_live.cc              # Live shared memory viewer (main)
│
└── docs/
    └── 2026-03-24-exchange-core-design.md
```

```
exchange/
├── MODULE.bazel
├── .bazelrc
├── BUILD.bazel
├── exchange-core/
│   ├── BUILD.bazel
│   ├── types.h                  # Price, Quantity, OrderId, Timestamp, enums, Order, PriceLevel, FillResult
│   ├── object_pool.h            # ObjectPool<T, Capacity>
│   ├── object_pool_test.cc
│   ├── intrusive_list.h         # Intrusive doubly-linked list operations
│   ├── intrusive_list_test.cc
│   ├── events.h                 # All callback event structs
│   ├── listeners.h              # OrderListenerBase, MarketDataListenerBase
│   ├── composite_listener.h     # CompositeOrderListener, CompositeMdListener
│   ├── composite_listener_test.cc
│   ├── orderbook.h              # OrderBook (bid/ask price level management)
│   ├── orderbook.cc
│   ├── orderbook_test.cc
│   ├── stop_book.h              # Stop order management
│   ├── stop_book.cc
│   ├── stop_book_test.cc
│   ├── match_algo.h             # FifoMatch, ProRataMatch policies
│   ├── match_algo_test.cc
│   ├── matching_engine.h        # MatchingEngine template
│   └── matching_engine_test.cc  # Unit tests using direct C++ assertions
│
├── test-harness/
│   ├── BUILD.bazel
│   ├── recorded_event.h         # RecordedEvent variant type
│   ├── recording_listener.h     # RecordingOrderListener, RecordingMdListener
│   ├── recording_listener_test.cc
│   ├── journal_parser.h         # Parse .journal files
│   ├── journal_parser.cc
│   ├── journal_parser_test.cc
│   ├── journal_writer.h         # Write recorded events to .journal format
│   ├── journal_writer.cc
│   ├── journal_writer_test.cc
│   ├── test_runner.h            # Replay journal + assert
│   ├── test_runner.cc
│   └── test_runner_test.cc
│
├── test-journals/
│   └── *.journal                # Test scenario files (see Section 8.8)
│
└── docs/
    └── 2026-03-24-exchange-core-design.md
```

---

## 12. Implementation Tasks

Tasks are ordered by dependency. Tasks within the same group have no dependencies on
each other and can be implemented in parallel. Each task targets under 200 lines of code.

### Group 1 — Foundation (no dependencies)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 1 | Core types, enums, and structs | `types.h` | ~120 | Type aliases, all enumerations, constants, Order, PriceLevel, FillResult structs |
| 2 | Object pool | `object_pool.h`, `object_pool_test.cc` | ~180 | Template pool with free list, allocate/deallocate/reset, tests for full lifecycle + exhaustion |
| 3 | Intrusive linked list | `intrusive_list.h`, `intrusive_list_test.cc` | ~180 | insert_before, insert_after, remove, push_back, push_front operations + tests |
| 4 | Callback event structs | `events.h` | ~100 | All event structs from Section 6.1 and 6.2, including OrderModifyRejected |
| 5 | Listener interfaces | `listeners.h` | ~60 | OrderListenerBase and MarketDataListenerBase with default no-ops |
| 6 | SPSC ring buffer | `spsc_ring_buffer.h`, `spsc_ring_buffer_test.cc` | ~180 | Lock-free SPSC ring buffer with power-of-2 capacity, cache-line aligned atomics, tests for push/pop/full/empty/wrap-around |

### Group 2 — Core Components (depends on Group 1)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 7 | Composite listener | `composite_listener.h`, `composite_listener_test.cc` | ~150 | CompositeOrderListener and CompositeMdListener with fold-expression dispatch + tests |
| 8 | Orderbook | `orderbook.h`, `orderbook.cc`, `orderbook_test.cc` | ~200 | Insert/remove orders, maintain sorted price levels, best bid/ask, level aggregation. Tests for insert, remove, price level creation/deletion |
| 9 | Stop book | `stop_book.h`, `stop_book.cc`, `stop_book_test.cc` | ~180 | Insert stop orders, trigger check, remove triggered orders. Tests for trigger logic and cascade |
| 10 | FIFO matching algorithm | `match_algo.h` (FIFO part), `match_algo_test.cc` (FIFO tests) | ~150 | FifoMatch::match implementation + tests for single fill, multi-fill, level exhaustion |
| 11 | Pro-rata matching algorithm | `match_algo.h` (ProRata part), `match_algo_test.cc` (ProRata tests) | ~180 | ProRataMatch::match with remainder distribution + tests for proportional split, rounding, single-order edge case |

### Group 3 — Matching Engine (depends on Group 2)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 12 | Engine: order validation + acceptance | `matching_engine.h` (partial) | ~200 | EngineConfig, OrderRequest, ModifyRequest, new_order validation path (tick/lot/band/tif/pool/id-space checks), OrderAccepted/Rejected callbacks, CRTP hooks. Unimplemented methods stubbed with assert(false). |
| 13 | Engine: limit order matching | `matching_engine.h` (partial) | ~200 | Limit order match-and-insert flow, fill callbacks, market data callbacks, SMP check |
| 14 | Engine: market/stop/stop-limit | `matching_engine.h` (partial) | ~180 | Market order handling (fill or cancel), stop/stop-limit insertion and trigger logic with iterative cascade |
| 15 | Engine: cancel and modify | `matching_engine.h` (partial) | ~180 | cancel_order, modify_order (cancel-replace + amend-down policies), reject paths including OrderModifyRejected |
| 16 | Engine: TIF handling + expiry | `matching_engine.h` (partial) | ~120 | IOC/FOK post-match logic, trigger_expiry for DAY/GTD bulk expiration (linear scan) |
| 17 | Engine unit tests: core paths | `matching_engine_test.cc` (partial) | ~200 | Basic limit fill, market fill, cancel, reject paths |
| 18 | Engine unit tests: advanced paths | `matching_engine_test.cc` (partial) | ~200 | Modify, SMP, stop triggers, TIF expiry, pool exhaustion |

### Group 4 — Test Harness (depends on Group 1 only)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 19 | Recorded event type | `recorded_event.h` | ~120 | RecordedEvent variant (std::variant or tagged union) covering all event types, equality comparison, to_string |
| 20 | Recording listeners | `recording_listener.h`, `recording_listener_test.cc` | ~150 | RecordingOrderListener + RecordingMdListener, capture to vector, clear/size, tests |
| 21 | Journal parser | `journal_parser.h`, `journal_parser.cc`, `journal_parser_test.cc` | ~200 | Parse CONFIG, ACTION and EXPECT lines, key=value extraction, error reporting on malformed lines |
| 22 | Journal writer | `journal_writer.h`, `journal_writer.cc`, `journal_writer_test.cc` | ~150 | Serialize config + actions + recorded events to .journal text format, round-trip test with parser |
| 23 | Test runner | `test_runner.h`, `test_runner.cc`, `test_runner_test.cc` | ~200 | Replay actions into engine, collect recorded events, compare against expectations, produce TestResult with diff |

### Group 5 — Journal Test Scenarios (depends on Groups 3 + 4)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 24 | Happy path journals | `test-journals/basic_*.journal`, `limit_*.journal`, `market_*.journal`, `multiple_fills.journal` | ~200 | 8 journal files covering basic order entry and fills |
| 25 | Order type journals | `test-journals/stop_*.journal`, `cancel_stop_order.journal`, `modify_stop_order.journal` | ~150 | 5 journal files for stop and stop-limit behavior |
| 26 | TIF journals | `test-journals/ioc_*.journal`, `fok_*.journal`, `gtd_*.journal`, `day_*.journal` | ~180 | 8 journal files for all TIF behaviors |
| 27 | Matching algo journals | `test-journals/fifo_*.journal`, `pro_rata_*.journal` | ~150 | 5 journal files for FIFO and pro-rata matching |
| 28 | Cancel/modify journals | `test-journals/cancel_*.journal`, `modify_*.journal` | ~180 | 9 journal files for cancel and modify paths |
| 29 | Failure/edge case journals | `test-journals/pool_*.journal`, `self_match_*.journal`, `*_reject.journal`, `empty_*.journal`, `market_order_tif.journal` | ~200 | 11 journal files for error handling and edge cases |
| 30 | Market data journals | `test-journals/l1_*.journal`, `l2_*.journal`, `l3_*.journal`, `trade_*.journal` | ~150 | 4 journal files verifying all market data callback correctness |
| 31 | Journal integration test | `test-harness/journal_integration_test.cc` | ~100 | Bazel test that loads and runs all .journal files, maps filenames to engine types (pro_rata_* -> ProRata engine, all others -> FIFO), fails if any scenario fails |

### Group 6 — Visualization Tools (depends on Groups 1 + 4 for offline, Group 6a for live)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| 32 | Orderbook state reconstructor | `tools/orderbook_state.h`, `tools/orderbook_state.cc`, `tools/orderbook_state_test.cc` | ~200 | Reconstructs orderbook state (depth, trades, events) from a stream of RecordedEvents. Stateful: apply events incrementally. Tests for add/fill/cancel/level transitions. |
| 33 | TUI renderer | `tools/tui_renderer.h`, `tools/tui_renderer.cc` | ~200 | FTXUI components: orderbook depth panel, recent trades panel, order event log, status bar. Takes OrderbookState as input, returns ftxui::Element. Shared by both offline and live viewers. |
| 34 | Offline journal visualizer | `tools/viz_replay.cc` | ~150 | Main binary: loads journal via JournalParser, replays action-by-action, feeds events to OrderbookState, renders via TuiRenderer. Interactive keyboard controls (next/prev/goto/quit). |
| 35 | Shared memory transport | `tools/shm_transport.h`, `tools/shm_transport.cc`, `tools/shm_transport_test.cc` | ~180 | ShmProducer + ShmConsumer wrapping POSIX shm_open/mmap over SpscRingBuffer. Tests for create/attach/publish/poll lifecycle. |
| 36 | Shared memory listeners | `tools/shm_listener.h` | ~80 | SharedMemoryOrderListener + SharedMemoryMdListener: publish events to ShmProducer. Thin wrappers. |
| 37 | Live TUI viewer | `tools/viz_live.cc` | ~120 | Main binary: attaches to shm via ShmConsumer, polls events, feeds to OrderbookState, renders via TuiRenderer. |

### Dependency Graph

```
Group 1: [1] [2] [3] [4] [5] [6]          (all parallel)
              |           |
Group 2: [7] [8] [9] [10] [11]            (all parallel, depend on G1)
              |
Group 3: [12] -> [13] -> [14]             (sequential: validation -> matching -> order types)
                   |
                   +-> [15]                (cancel/modify needs matching)
                   +-> [16]                (TIF needs post-match logic)
          [17] [18] (after 12-16)          (unit tests need full engine)

Group 4: [19] [20] [21] [22]              (all parallel, depend on G1 only)
          [23] (after 19-22)               (test runner needs all harness pieces)

Group 5: [24]-[30]                         (all parallel, depend on G3 + G4)
          [31] (after 24-30)               (integration test needs all journals)

Group 6: [32] (depends on G4: recorded_event)
          [33] (depends on 32)
          [34] (depends on 33 + G4: journal_parser)
          [35] (depends on G1: spsc_ring_buffer)
          [36] (depends on 35 + G1: listeners)
          [37] (depends on 33 + 35)
```
