# Exchange-Core Gap Closure Plan

**Date:** 2026-03-26
**Baseline:** Phase 1-3 exchange-core (session states, auctions, iceberg, dynamic bands, mass cancel, max order size, OHLCV, market status all complete)
**Goal:** Close 8 remaining core gaps so all 10 exchange simulators can be built

---

## 1. Scope

Features included (prioritized):

| ID | Feature | Exchanges | Priority |
|----|---------|-----------|----------|
| G1 | Trade bust/adjustment | 10/10 | P1 |
| G2 | Order rate throttling | 10/10 | P2 |
| G3 | GTC cross-session persistence | 10/10 | P3 |
| G4 | Daily price limits + lock state | 8/10 | P4 |
| G5 | Volatility auction auto-trigger | 7/10 | P5 |
| G6 | Market maker priority (LMM) | 4/10 | P6 |
| G7 | Additional matching algorithms | CME (critical) | P7 |
| G8 | Position limits hooks | 10/10 (infra) | P8 |

Explicitly excluded:
- Implied/spread trading (separate major project)
- Exchange-specific order types (MTL, MIT, OCO -- exchange-layer)
- Member/firm controls and account hierarchy (infrastructure, not matching)

---

## 2. Task Breakdown

### G1: Trade Bust/Adjustment (P1)

Reverses an executed trade: restores order quantities, removes fill from records.
All 10 exchanges need this (erroneous trade handling is regulatory).

#### G1-T1: Trade registry + bust types/events (~120 lines)

**Description:** Add a `TradeRegistry` that records executed trades by ID, plus new types and events for bust operations.

**Files to create/modify:**
- `exchange-core/types.h` -- add `TradeId` alias (`uint64_t`), `BustReason` enum
- `exchange-core/events.h` -- add `TradeBusted` event struct
- `exchange-core/listeners.h` -- add `on_trade_busted()` to `OrderListenerBase`
- `exchange-core/trade_registry.h` (new) -- `TradeRegistry` class: stores `TradeRecord` entries (aggressor_id, resting_id, price, qty, ts), indexed by TradeId

**Dependencies:** None

**Unit tests** (`exchange-core/trade_registry_test.cc`):
- Register a trade, look it up by ID
- Look up non-existent trade returns nullopt
- Register multiple trades, verify all retrievable
- Boundary: max trade ID range

**Journal tests:** None (types/registry only, no engine integration yet)

**Estimated lines:** ~120

---

#### G1-T2: `bust_trade()` engine method (~150 lines)

**Description:** Add `bust_trade(TradeId, Timestamp)` to `MatchingEngine`. Looks up trade in registry, restores `filled_quantity`/`remaining_quantity` on both orders (if still alive), re-inserts into book if removed, fires `TradeBusted` event. If an order was fully filled and deallocated, the bust still fires the event but cannot restore the order (partial bust).

**Files to modify:**
- `exchange-core/matching_engine.h` -- add `TradeRegistry` member, record trades in `match_order()` and `execute_auction()`, implement `bust_trade()`
- `exchange-core/matching_engine.h` -- add CRTP hook `on_validate_bust()` (default: `return true`)

**Dependencies:** G1-T1

**Unit tests** (`exchange-core/trade_bust_test.cc`):
- Bust a trade where both orders are still resting -- quantities restored
- Bust a trade where aggressor was fully filled (deallocated) -- resting side restored, event still fires
- Bust a trade where resting order was fully filled and removed -- order re-allocated and re-inserted at original price
- Bust non-existent trade ID -- rejected
- Bust same trade twice -- second bust rejected
- Bust in Closed session state -- rejected

**Journal tests** (`journals/trade_bust.journal`):
```
# Setup: two orders that cross, producing a trade
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=50000 ts=1
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1

ACTION NEW_ORDER cl_ord_id=2 side=SELL type=LIMIT price=1000000 qty=50000 ts=2
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2
EXPECT ORDER_FILLED aggressor=2 resting=1 price=1000000 qty=50000 ts=2
EXPECT TRADE price=1000000 qty=50000 aggressor=2 resting=1 aggressor_side=SELL ts=2

# Bust the trade
ACTION BUST_TRADE trade_id=1 ts=3
EXPECT TRADE_BUSTED trade_id=1 aggressor=2 resting=1 price=1000000 qty=50000 ts=3
```

**Estimated lines:** ~150

---

#### G1-T3: Journal/test-harness support for bust actions (~80 lines)

**Description:** Extend the journal parser and test runner to handle `BUST_TRADE` actions and `TRADE_BUSTED` expectations.

**Files to modify:**
- `test-harness/journal_parser.h` -- add `BustTrade` to `ParsedAction::Type`
- `test-harness/journal_parser.cc` -- parse `BUST_TRADE` action keyword
- `test-harness/test_runner.cc` -- handle `BustTrade` in `execute_action()`, handle `TRADE_BUSTED` in `expectation_to_event()`
- `test-harness/recorded_event.h` -- add `TradeBusted` to `RecordedEvent` variant
- `test-harness/recording_listener.h` -- record `TradeBusted` events

**Dependencies:** G1-T1, G1-T2

**Unit tests:** Covered by journal test execution

**Journal tests:** Validated by running `journals/trade_bust.journal`

**Estimated lines:** ~80

---

### G2: Order Rate Throttling (P2)

Per-account message rate tracking with configurable limits.
All 10 exchanges require this (regulatory: CFTC, ESMA, MAS, SEBI).

#### G2-T1: Rate tracker + throttle types (~130 lines)

**Description:** Add `RateTracker` class that counts messages per account within a sliding window. Add `ThrottleConfig` to `EngineConfig` (max_messages_per_interval, interval_ns). Add `RejectReason::RateThrottled` enum value.

**Files to create/modify:**
- `exchange-core/rate_tracker.h` (new) -- `RateTracker<MaxAccounts>` template: fixed-size array of `{count, window_start}` per account, `check_and_increment(account_id, ts)` returns bool
- `exchange-core/types.h` -- add `RateThrottled` to `RejectReason`

**Dependencies:** None

**Unit tests** (`exchange-core/rate_tracker_test.cc`):
- Single account: messages within limit accepted
- Single account: message exceeding limit rejected
- Window slides: after interval passes, counter resets
- Multiple accounts: independent tracking
- account_id=0 (untagged) -- never throttled (or always throttled, design decision)
- Boundary: exact limit count (N-th message accepted, N+1-th rejected)
- Boundary: timestamp exactly at window edge

**Journal tests:** None (standalone component)

**Estimated lines:** ~130

---

#### G2-T2: Integrate rate tracking into engine (~100 lines)

**Description:** Add `RateTracker` member to `MatchingEngine`. Call `check_and_increment()` in `new_order()`, `modify_order()`, and `cancel_order()`. Reject with `RateThrottled` if exceeded. Add CRTP hook `get_rate_limit_config()` so exchanges can customize.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add `RateTracker` member, check in `new_order()`/`modify_order()`/`cancel_order()`, add `ThrottleConfig` to `EngineConfig`, add CRTP hook `is_rate_check_enabled()` (default: false, so existing tests unaffected)

**Dependencies:** G2-T1

**Unit tests** (`exchange-core/rate_throttle_integration_test.cc`):
- Engine with rate limit: orders within limit accepted
- Engine with rate limit: order exceeding limit rejected with `RateThrottled`
- Modify and cancel also count toward rate
- Rate resets after window
- Rate disabled (default config): unlimited orders accepted

**Journal tests** (`journals/rate_throttle.journal`):
```
CONFIG tick_size=100 lot_size=10000 rate_limit=3 rate_interval=1000000000
# 3 orders in 1-second window
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=10000 ts=1
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1

ACTION NEW_ORDER cl_ord_id=2 side=BUY type=LIMIT price=990000 qty=10000 ts=2
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2

ACTION NEW_ORDER cl_ord_id=3 side=BUY type=LIMIT price=980000 qty=10000 ts=3
EXPECT ORDER_ACCEPTED ord_id=3 cl_ord_id=3 ts=3

# 4th order within same window -- rejected
ACTION NEW_ORDER cl_ord_id=4 side=BUY type=LIMIT price=970000 qty=10000 ts=4
EXPECT ORDER_REJECTED cl_ord_id=4 ts=4 reason=RATE_THROTTLED

# After window expires, orders accepted again
ACTION NEW_ORDER cl_ord_id=5 side=BUY type=LIMIT price=960000 qty=10000 ts=1000000005
EXPECT ORDER_ACCEPTED ord_id=4 cl_ord_id=5 ts=1000000005
```

**Estimated lines:** ~100

---

### G3: GTC Cross-Session Persistence (P3)

Serialize/restore orders for session boundary management.
All 10 exchanges need this -- GTC orders survive across trading days.

#### G3-T1: Order serialization types + interface (~100 lines)

**Description:** Add `SerializedOrder` struct (all Order fields in a POD-friendly layout) and `serialize_order()`/`deserialize_order()` free functions. These convert between the engine's intrusive-list `Order` and a flat, storable representation.

**Files to create/modify:**
- `exchange-core/order_persistence.h` (new) -- `SerializedOrder` struct, `serialize_order(const Order&) -> SerializedOrder`, `deserialize_order(const SerializedOrder&, Order*) -> void`

**Dependencies:** None

**Unit tests** (`exchange-core/order_persistence_test.cc`):
- Round-trip: serialize then deserialize, all fields match
- Iceberg order: display_qty and total_qty preserved
- GTD order: gtd_expiry preserved
- Partially filled order: filled_quantity, remaining_quantity correct after round-trip

**Journal tests:** None (serialization utility)

**Estimated lines:** ~100

---

#### G3-T2: Engine restore_order() method (~150 lines)

**Description:** Add `restore_order(const SerializedOrder&, Timestamp)` to `MatchingEngine`. Re-validates the order against current session rules (price bands, lot size, TIF validity), allocates from pool, assigns the original order ID (or a new one if the original slot is taken), inserts into book. Fires `OrderAccepted` on success, `OrderRejected` on validation failure. Add CRTP hook `on_validate_restore(const SerializedOrder&)` for exchange-specific re-validation.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add `restore_order()` method, CRTP hook `on_validate_restore()`

**Dependencies:** G3-T1

**Unit tests** (`exchange-core/order_restore_test.cc`):
- Restore a limit buy order -- appears on book at correct price/qty
- Restore preserves original order ID
- Restore order with invalid price (outside current bands) -- rejected
- Restore order with expired GTD -- rejected
- Restore into full pool -- rejected
- Restore with duplicate order ID -- rejected
- Restore iceberg order -- display_qty tranche visible, hidden quantity tracked

**Journal tests** (`journals/gtc_persistence.journal`):
```
ACTION SET_SESSION_STATE state=CONTINUOUS ts=1

ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=50000 tif=GTC ts=2
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=2

# Simulate session boundary: close, then reopen
ACTION SET_SESSION_STATE state=CLOSED ts=100

ACTION SET_SESSION_STATE state=CONTINUOUS ts=200

# Restore the GTC order
ACTION RESTORE_ORDER ord_id=1 cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=50000 filled_qty=0 tif=GTC ts=201
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=201

# Verify it matches
ACTION NEW_ORDER cl_ord_id=2 side=SELL type=LIMIT price=1000000 qty=50000 ts=300
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=300
EXPECT ORDER_FILLED aggressor=2 resting=1 price=1000000 qty=50000 ts=300
EXPECT TRADE price=1000000 qty=50000 aggressor=2 resting=1 aggressor_side=SELL ts=300
```

**Estimated lines:** ~150

---

#### G3-T3: Journal/test-harness support for restore actions (~70 lines)

**Description:** Extend journal parser and test runner to handle `RESTORE_ORDER` actions.

**Files to modify:**
- `test-harness/journal_parser.h` -- add `RestoreOrder` to `ParsedAction::Type`
- `test-harness/journal_parser.cc` -- parse `RESTORE_ORDER` keyword
- `test-harness/test_runner.cc` -- handle `RestoreOrder` in `execute_action()`

**Dependencies:** G3-T1, G3-T2

**Estimated lines:** ~70

---

### G4: Daily Price Limits + Lock State (P4)

Absolute price limits that trigger halt/lock-limit state.
8/10 exchanges (CME, Eurex, ICE, KRX, JPX, SGX, NSE, HKEX).

#### G4-T1: DailyPriceLimits config + lock state types (~80 lines)

**Description:** Add `DailyPriceLimits` struct (upper_limit, lower_limit, lock_duration_ns) to `EngineConfig`. Add `SessionState::LockLimit` enum value. Add `LockLimitTriggered` event.

**Files to modify:**
- `exchange-core/types.h` -- add `LockLimit` to `SessionState`
- `exchange-core/events.h` -- add `LockLimitTriggered` event (side, limit_price, ts)
- `exchange-core/listeners.h` -- add `on_lock_limit_triggered()` to `MarketDataListenerBase`
- `exchange-core/matching_engine.h` -- add `DailyPriceLimits` to `EngineConfig`

**Dependencies:** None

**Unit tests:** None (types only)

**Journal tests:** None (types only)

**Estimated lines:** ~80

---

#### G4-T2: Daily limit enforcement in engine (~150 lines)

**Description:** In `new_order()` and `modify_order()`, check order price against daily limits. If a trade would execute at the limit price, transition to `LockLimit` state (orders accepted but no matching until unlocked). Add CRTP hook `on_lock_limit(Side, Price, Timestamp)` for exchange-specific behavior (CME expands limits in tiers). Add `unlock_limit(Timestamp)` method.

**Files to modify:**
- `exchange-core/matching_engine.h` -- daily limit price check in validation, lock state transition in `match_order()` when trade hits limit, `unlock_limit()` method, CRTP hook `on_lock_limit()`

**Dependencies:** G4-T1

**Unit tests** (`exchange-core/daily_limits_test.cc`):
- Order at limit price accepted, matching occurs up to limit
- Order beyond daily limit rejected with `PriceBandViolation`
- Trade at limit price triggers LockLimit state
- In LockLimit: orders accepted (queue), no matching occurs
- unlock_limit resumes matching
- Daily limits of 0 = no limit (backward compatible)
- Modify order to exceed daily limit -- rejected

**Journal tests** (`journals/daily_price_limits.journal`):
```
CONFIG tick_size=100 lot_size=10000 daily_limit_high=1100000 daily_limit_low=900000

ACTION SET_SESSION_STATE state=CONTINUOUS ts=1

# Order at limit -- accepted
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1100000 qty=10000 ts=2
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=2

# Order beyond limit -- rejected
ACTION NEW_ORDER cl_ord_id=2 side=BUY type=LIMIT price=1110000 qty=10000 ts=3
EXPECT ORDER_REJECTED cl_ord_id=2 ts=3 reason=PRICE_BAND_VIOLATION

# Sell at lower limit, trade triggers lock-limit
ACTION NEW_ORDER cl_ord_id=3 side=SELL type=LIMIT price=900000 qty=10000 ts=4
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=3 ts=4

ACTION NEW_ORDER cl_ord_id=4 side=BUY type=LIMIT price=900000 qty=10000 ts=5
EXPECT ORDER_ACCEPTED ord_id=3 cl_ord_id=4 ts=5
EXPECT ORDER_FILLED aggressor=3 resting=2 price=900000 qty=10000 ts=5
EXPECT TRADE price=900000 qty=10000 aggressor=3 resting=2 aggressor_side=BUY ts=5
EXPECT LOCK_LIMIT_TRIGGERED side=SELL limit_price=900000 ts=5
EXPECT MARKET_STATUS state=LOCK_LIMIT ts=5
```

**Estimated lines:** ~150

---

#### G4-T3: Journal/test-harness support for daily limits (~70 lines)

**Description:** Extend journal parser for `daily_limit_high`/`daily_limit_low` config, `LOCK_LIMIT_TRIGGERED` event, and `LOCK_LIMIT` session state.

**Files to modify:**
- `test-harness/journal_parser.h/cc` -- parse daily limit config fields
- `test-harness/test_runner.cc` -- handle `LOCK_LIMIT_TRIGGERED` expectation, parse `LOCK_LIMIT` state
- `test-harness/recorded_event.h` -- add `LockLimitTriggered` to variant
- `test-harness/recording_listener.h` -- record `LockLimitTriggered` events

**Dependencies:** G4-T1, G4-T2

**Estimated lines:** ~70

---

### G5: Volatility Auction Auto-Trigger (P5)

When a trade price hits a volatility interruption (VI) threshold relative to a reference price, auto-transition to VolatilityAuction state.
7/10 exchanges (CME, Eurex, ASX, HKEX, JPX, SGX, NSE).

#### G5-T1: VI config + CRTP hooks (~80 lines)

**Description:** Add `VolatilityConfig` to `EngineConfig` (vi_threshold_ticks, vi_auction_duration_ns, reference_source enum). Add CRTP hook `should_trigger_volatility_auction(Price trade_price, Price reference_price)` (already declared in gap analysis but not yet implemented). Default returns false.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add `VolatilityConfig` to `EngineConfig`, ensure CRTP hook `should_trigger_volatility_auction()` exists with default `return false`

**Dependencies:** None

**Unit tests:** None (config/hooks only)

**Estimated lines:** ~80

---

#### G5-T2: Auto-trigger in match_order() (~120 lines)

**Description:** After each fill in `match_order()`, check `should_trigger_volatility_auction(last_trade_price_, reference_price)`. If triggered: stop matching, transition to `VolatilityAuction` state, fire `MarketStatus` event. Remaining aggressor quantity rests on book (auction collection). The engine already handles VolatilityAuction as a collection phase.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add VI check after fill in `match_order()`, add `vi_reference_price_` member updated at session transitions and auction executions

**Dependencies:** G5-T1

**Unit tests** (`exchange-core/volatility_auction_test.cc`):
- Trade that exceeds VI threshold triggers auction transition
- Trade within threshold -- no transition
- VI disabled (threshold=0) -- no transition
- VI triggers mid-fill: aggressor partial fill, remaining stops matching
- VI during auction phase -- no re-trigger (already in auction)
- Multiple fills at same level, VI check after each individual fill

**Journal tests** (`journals/volatility_auction_trigger.journal`):
```
CONFIG tick_size=100 lot_size=10000

ACTION SET_SESSION_STATE state=CONTINUOUS ts=1

# Reference price is 1000000 (set via last trade)
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=10000 ts=2
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=2

ACTION NEW_ORDER cl_ord_id=2 side=SELL type=LIMIT price=1000000 qty=10000 ts=3
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=3
EXPECT ORDER_FILLED aggressor=2 resting=1 price=1000000 qty=10000 ts=3
EXPECT TRADE price=1000000 qty=10000 aggressor=2 resting=1 aggressor_side=SELL ts=3

# Large price move triggers VI
ACTION NEW_ORDER cl_ord_id=3 side=BUY type=LIMIT price=1200000 qty=10000 ts=10
EXPECT ORDER_ACCEPTED ord_id=3 cl_ord_id=3 ts=10

ACTION NEW_ORDER cl_ord_id=4 side=SELL type=LIMIT price=1200000 qty=10000 ts=11
EXPECT ORDER_ACCEPTED ord_id=4 cl_ord_id=4 ts=11
EXPECT ORDER_FILLED aggressor=4 resting=3 price=1200000 qty=10000 ts=11
EXPECT TRADE price=1200000 qty=10000 aggressor=4 resting=3 aggressor_side=SELL ts=11
EXPECT MARKET_STATUS state=VOLATILITY_AUCTION ts=11
```

**Estimated lines:** ~120

---

### G6: Market Maker Priority (LMM) (P6)

Designated market makers receive priority allocation before FIFO matching.
4/10 exchanges (CME, Eurex, HKEX, SGX).

#### G6-T1: LMM match algorithm (~150 lines)

**Description:** Add `FifoLmmMatch` static policy class. Before FIFO matching at a level, allocates `mm_priority_pct * aggressor_qty` to orders flagged as market-maker (identified by a flag on `Order`). MM orders matched among themselves by time priority. Remainder matched FIFO across all orders.

**Files to create/modify:**
- `exchange-core/types.h` -- add `bool is_market_maker{false}` field to `Order`
- `exchange-core/match_algo.h` -- add `FifoLmmMatch` struct with `match()` taking an additional `mm_priority_pct` parameter, or reading from a static config

**Design decision:** Since `MatchAlgoT::match()` is a static interface, and LMM percentage is exchange-configurable, we parameterize through a template parameter or an extra argument. Chosen approach: add an optional `mm_priority_pct` parameter (default 0) to `match()` signatures. When 0, behaves identically to `FifoMatch`.

**Dependencies:** None

**Unit tests** (`exchange-core/lmm_match_test.cc`):
- Level with 1 MM and 2 regular orders, 40% MM priority: MM gets 40% of fill, rest FIFO
- MM priority fills less than MM resting -- MM gets full resting, remainder FIFO
- No MM orders at level -- pure FIFO behavior
- All orders are MM -- FIFO among MMs
- MM priority 0% -- pure FIFO
- MM priority 100% -- MM gets everything, remainder FIFO among non-MM
- Rounding: fractional lot allocations rounded down, remainder distributed FIFO

**Journal tests:** None (algorithm tested in isolation)

**Estimated lines:** ~150

---

#### G6-T2: Engine integration + CRTP hooks (~100 lines)

**Description:** Add CRTP hooks `is_market_maker(const Order&)` (default: false) and `get_mm_priority_pct()` (default: 0). In `new_order()`, set `order->is_market_maker = derived().is_market_maker(req)`. Wire `FifoLmmMatch` through a new `MatchingEngine` variant or dispatch in `match_order()`.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add CRTP hooks, set MM flag on order, pass mm_priority_pct to match algorithm
- `exchange-core/types.h` -- add `is_market_maker` to `OrderRequest`

**Dependencies:** G6-T1

**Unit tests** (`exchange-core/lmm_engine_test.cc`):
- Exchange with LMM: MM order gets priority allocation
- Exchange without LMM (default hooks): pure FIFO behavior preserved
- MM order modified -- still recognized as MM after modification

**Journal tests** (`journals/lmm_priority.journal`):
```
CONFIG tick_size=100 lot_size=10000 match_algo=FIFO_LMM

# MM order rests
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=100000 is_mm=true ts=1
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=1

# Regular order rests
ACTION NEW_ORDER cl_ord_id=2 side=BUY type=LIMIT price=1000000 qty=100000 ts=2
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=2

# Aggressor sells 100000: MM gets 40% (40000), then FIFO for rest
ACTION NEW_ORDER cl_ord_id=3 side=SELL type=LIMIT price=1000000 qty=100000 ts=3
EXPECT ORDER_ACCEPTED ord_id=3 cl_ord_id=3 ts=3
EXPECT ORDER_PARTIALLY_FILLED aggressor=3 resting=1 price=1000000 qty=40000 ...
EXPECT ORDER_PARTIALLY_FILLED aggressor=3 resting=1 price=1000000 qty=60000 ...
```

**Estimated lines:** ~100

---

### G7: Additional Matching Algorithms (P7)

CME needs Threshold ProRata, FIFO+LMM, Top+LMM, Allocation, Split FIFO/ProRata.

#### G7-T1: ThresholdProRataMatch algorithm (~120 lines)

**Description:** Pro-rata with minimum allocation threshold. Orders receiving less than `min_threshold` lots get zero allocation. Remainder redistributed via FIFO.

**Files to create/modify:**
- `exchange-core/match_algo.h` -- add `ThresholdProRataMatch` struct

**Dependencies:** None (uses existing `FillResult`, `PriceLevel`, `Order` types)

**Unit tests** (`exchange-core/threshold_prorata_test.cc`):
- 3 orders, threshold=2 lots: orders below threshold get 0, remainder via FIFO
- All orders above threshold -- standard pro-rata
- All orders below threshold -- all via FIFO
- Single order at level -- gets everything regardless of threshold
- Exact threshold boundary: order gets exactly threshold amount

**Estimated lines:** ~120

---

#### G7-T2: FifoTopLmmMatch algorithm (~130 lines)

**Description:** Top Order + LMM + FIFO. The first order at a new best price ("top order") gets priority allocation before LMM and FIFO.

**Files to create/modify:**
- `exchange-core/match_algo.h` -- add `FifoTopLmmMatch` struct
- `exchange-core/types.h` -- add `bool is_top_order{false}` to `Order`

**Dependencies:** G6-T1 (uses LMM priority logic)

**Unit tests** (`exchange-core/top_lmm_match_test.cc`):
- Level with top order, MM, and regular: top gets priority, then MM, then FIFO
- No top order at level -- LMM + FIFO only
- Top order fully filled by its allocation -- remainder to LMM + FIFO
- Top order and MM are same order -- gets both allocations

**Estimated lines:** ~130

---

#### G7-T3: AllocationMatch (pure pro-rata) + SplitFifoProRataMatch (~120 lines)

**Description:**
- `AllocationMatch`: Like ProRata but remainder goes to largest resting order (not FIFO).
- `SplitFifoProRataMatch`: Configurable split (e.g., 40% FIFO, 60% ProRata).

**Files to create/modify:**
- `exchange-core/match_algo.h` -- add both structs

**Dependencies:** None

**Unit tests** (`exchange-core/allocation_match_test.cc`, `exchange-core/split_match_test.cc`):
- AllocationMatch: remainder to largest order, not oldest
- AllocationMatch: tie on size breaks to FIFO
- SplitMatch: 40/60 split correctly divides aggressor quantity
- SplitMatch: FIFO portion fills in time order, ProRata portion proportional
- SplitMatch: 0% FIFO = pure ProRata, 100% FIFO = pure FIFO

**Estimated lines:** ~120

---

### G8: Position Limits Hooks (P8)

Pre-trade position check with configurable limits.
All 10 exchanges (infrastructure hooks, exchange sets specific limits).

#### G8-T1: Position tracker + types (~120 lines)

**Description:** Add `PositionTracker<MaxAccounts>` template that tracks net position (long - short) per account. Updated on fills. Add `PositionLimitConfig` (max_long, max_short per account, or a single max_net_position).

**Files to create/modify:**
- `exchange-core/position_tracker.h` (new) -- `PositionTracker` class: `update_fill(account_id, side, qty)`, `get_net_position(account_id)`, `would_exceed_limit(account_id, side, qty, limit)`
- `exchange-core/types.h` -- add `PositionLimitExceeded` to `RejectReason`

**Dependencies:** None

**Unit tests** (`exchange-core/position_tracker_test.cc`):
- Buy fills increase long position
- Sell fills increase short position
- Net position correctly computed (long - short)
- `would_exceed_limit` returns true when adding qty would breach limit
- `would_exceed_limit` returns false within limit
- Multiple accounts tracked independently
- Position after bust (decrease) -- tested with explicit calls

**Estimated lines:** ~120

---

#### G8-T2: Engine integration + CRTP hooks (~100 lines)

**Description:** Add `PositionTracker` member to `MatchingEngine`. Before matching in `new_order()`, call CRTP hook `check_position_limit(account_id, side, qty)` (default: return true / no limit). After fills, update position tracker. On bust, reverse position. Add CRTP hook `get_position_limit(account_id)` for exchange-specific limits.

**Files to modify:**
- `exchange-core/matching_engine.h` -- add position tracker member, pre-trade check in `new_order()`, post-fill update, CRTP hooks

**Dependencies:** G8-T1

**Unit tests** (`exchange-core/position_limit_engine_test.cc`):
- Order that would exceed position limit rejected
- Order within limit accepted and fills update position
- Position limit disabled (default) -- all orders accepted
- Bust reverses position correctly

**Journal tests** (`journals/position_limits.journal`):
```
CONFIG tick_size=100 lot_size=10000 position_limit=100000

ACTION SET_SESSION_STATE state=CONTINUOUS ts=1

# Build up position to limit
ACTION NEW_ORDER cl_ord_id=1 side=BUY type=LIMIT price=1000000 qty=100000 account_id=1 ts=2
EXPECT ORDER_ACCEPTED ord_id=1 cl_ord_id=1 ts=2

ACTION NEW_ORDER cl_ord_id=2 side=SELL type=LIMIT price=1000000 qty=100000 account_id=2 ts=3
EXPECT ORDER_ACCEPTED ord_id=2 cl_ord_id=2 ts=3
EXPECT ORDER_FILLED aggressor=2 resting=1 price=1000000 qty=100000 ts=3
EXPECT TRADE price=1000000 qty=100000 aggressor=2 resting=1 aggressor_side=SELL ts=3

# Account 1 is now long 100000 -- at limit. New buy rejected.
ACTION NEW_ORDER cl_ord_id=3 side=BUY type=LIMIT price=1000000 qty=10000 account_id=1 ts=4
EXPECT ORDER_REJECTED cl_ord_id=3 ts=4 reason=POSITION_LIMIT_EXCEEDED
```

**Estimated lines:** ~100

---

## 3. Task Dependency DAG

```
G1-T1 (trade types) -----> G1-T2 (bust_trade) -----> G1-T3 (journal support)

G2-T1 (rate tracker) -----> G2-T2 (engine integration)

G3-T1 (serialization) ----> G3-T2 (restore_order) ----> G3-T3 (journal support)

G4-T1 (limit types) ------> G4-T2 (enforcement) ------> G4-T3 (journal support)

G5-T1 (VI config) --------> G5-T2 (auto-trigger)

                             G6-T1 (LMM match) --------> G6-T2 (engine integration)
                                   |
                                   v
                             G7-T2 (Top+LMM match)

G7-T1 (threshold prorata) ----\
G7-T2 (top+LMM) ---- depends on G6-T1
G7-T3 (allocation+split) ----/   (all 3 are independent of each other except G7-T2)

G8-T1 (position tracker) ----> G8-T2 (engine integration)
```

Cross-group dependencies:
- G7-T2 depends on G6-T1 (LMM algorithm)
- All other groups (G1-G8) are independent of each other

---

## 4. Dev Team Dispatch Strategy (4 Devs)

### Sprint 1 (Parallel Start -- All Independent Roots)

| Dev | Tasks | Estimated Lines | Notes |
|-----|-------|----------------|-------|
| Dev A | G1-T1, G1-T2, G1-T3 | ~350 | Trade bust (full chain) |
| Dev B | G2-T1, G2-T2 | ~230 | Rate throttling (full chain) |
| Dev C | G3-T1, G3-T2, G3-T3 | ~320 | GTC persistence (full chain) |
| Dev D | G4-T1, G4-T2, G4-T3 | ~300 | Daily price limits (full chain) |

All four chains are fully independent -- no blocking.

### Sprint 2 (After Sprint 1 Completes)

| Dev | Tasks | Estimated Lines | Notes |
|-----|-------|----------------|-------|
| Dev A | G5-T1, G5-T2 | ~200 | Volatility auction trigger |
| Dev B | G6-T1, G6-T2 | ~250 | LMM priority |
| Dev C | G7-T1, G7-T3 | ~240 | Threshold ProRata + Allocation + Split |
| Dev D | G8-T1, G8-T2 | ~220 | Position limits |

### Sprint 3 (Only If G6-T1 Finishes Before G7-T2 Needed)

| Dev | Tasks | Estimated Lines | Notes |
|-----|-------|----------------|-------|
| Dev C (or whoever is free) | G7-T2 | ~130 | Top+LMM (depends on G6-T1) |

**Alternative dispatch:** If Dev B finishes G6-T1 early in Sprint 2, Dev C can start G7-T2 immediately after, avoiding a Sprint 3.

---

## 5. New File Structure

```
exchange-core/
  trade_registry.h           (new, G1-T1)
  rate_tracker.h             (new, G2-T1)
  order_persistence.h        (new, G3-T1)
  position_tracker.h         (new, G8-T1)
  types.h                    (modified: TradeId, BustReason, LockLimit, is_market_maker, PositionLimitExceeded, RateThrottled)
  events.h                   (modified: TradeBusted, LockLimitTriggered)
  listeners.h                (modified: on_trade_busted, on_lock_limit_triggered)
  match_algo.h               (modified: FifoLmmMatch, ThresholdProRataMatch, FifoTopLmmMatch, AllocationMatch, SplitFifoProRataMatch)
  matching_engine.h          (modified: bust_trade, restore_order, rate tracking, daily limits, VI trigger, LMM dispatch, position limits)

test-harness/
  journal_parser.h/cc        (modified: BustTrade, RestoreOrder action types; new config fields)
  test_runner.cc             (modified: execute BustTrade/RestoreOrder; expect TradeBusted/LockLimitTriggered)
  recorded_event.h           (modified: TradeBusted, LockLimitTriggered variants)
  recording_listener.h       (modified: record new event types)

exchange-core/ (test files)
  trade_registry_test.cc     (new, G1-T1)
  trade_bust_test.cc         (new, G1-T2)
  rate_tracker_test.cc       (new, G2-T1)
  rate_throttle_integration_test.cc  (new, G2-T2)
  order_persistence_test.cc  (new, G3-T1)
  order_restore_test.cc      (new, G3-T2)
  daily_limits_test.cc       (new, G4-T2)
  volatility_auction_test.cc (new, G5-T2)
  lmm_match_test.cc          (new, G6-T1)
  lmm_engine_test.cc         (new, G6-T2)
  threshold_prorata_test.cc  (new, G7-T1)
  top_lmm_match_test.cc      (new, G7-T2)
  allocation_match_test.cc   (new, G7-T3)
  split_match_test.cc        (new, G7-T3)
  position_tracker_test.cc   (new, G8-T1)
  position_limit_engine_test.cc (new, G8-T2)

journals/
  trade_bust.journal         (new, G1)
  rate_throttle.journal      (new, G2)
  gtc_persistence.journal    (new, G3)
  daily_price_limits.journal (new, G4)
  volatility_auction_trigger.journal (new, G5)
  lmm_priority.journal       (new, G6)
  position_limits.journal    (new, G8)
```

---

## 6. Summary Table

| Task ID | Feature | Files | Est. Lines | Dependencies | Sprint |
|---------|---------|-------|------------|--------------|--------|
| G1-T1 | Trade registry + types | trade_registry.h, types.h, events.h, listeners.h | ~120 | None | 1 |
| G1-T2 | bust_trade() engine method | matching_engine.h | ~150 | G1-T1 | 1 |
| G1-T3 | Journal support for bust | journal_parser, test_runner, recorded_event | ~80 | G1-T2 | 1 |
| G2-T1 | Rate tracker | rate_tracker.h, types.h | ~130 | None | 1 |
| G2-T2 | Rate tracking in engine | matching_engine.h | ~100 | G2-T1 | 1 |
| G3-T1 | Order serialization | order_persistence.h | ~100 | None | 1 |
| G3-T2 | restore_order() engine method | matching_engine.h | ~150 | G3-T1 | 1 |
| G3-T3 | Journal support for restore | journal_parser, test_runner | ~70 | G3-T2 | 1 |
| G4-T1 | Daily limit types | types.h, events.h, listeners.h, matching_engine.h | ~80 | None | 1 |
| G4-T2 | Daily limit enforcement | matching_engine.h | ~150 | G4-T1 | 1 |
| G4-T3 | Journal support for limits | journal_parser, test_runner, recorded_event | ~70 | G4-T2 | 1 |
| G5-T1 | VI config + hooks | matching_engine.h | ~80 | None | 2 |
| G5-T2 | VI auto-trigger | matching_engine.h | ~120 | G5-T1 | 2 |
| G6-T1 | LMM match algorithm | match_algo.h, types.h | ~150 | None | 2 |
| G6-T2 | LMM engine integration | matching_engine.h, types.h | ~100 | G6-T1 | 2 |
| G7-T1 | ThresholdProRataMatch | match_algo.h | ~120 | None | 2 |
| G7-T2 | FifoTopLmmMatch | match_algo.h, types.h | ~130 | G6-T1 | 3 |
| G7-T3 | AllocationMatch + SplitMatch | match_algo.h | ~120 | None | 2 |
| G8-T1 | Position tracker | position_tracker.h, types.h | ~120 | None | 2 |
| G8-T2 | Position limits in engine | matching_engine.h | ~100 | G8-T1 | 2 |

**Total: 20 tasks, ~2,360 estimated lines (including tests)**

---

## 7. Success Criteria

1. All 20 tasks have passing unit tests with edge cases and failure injection
2. All 7 journal test files execute successfully through the test harness
3. Existing tests continue to pass (no regressions)
4. `bazel test //...` green
5. Every new CRTP hook has a default that preserves backward compatibility (existing exchange implementations unchanged)
6. No task exceeds 200 lines (implementation) / 400 lines (with tests)
7. All new `RejectReason` values have corresponding journal string mappings
8. Trade bust correctly restores order state and book invariants
9. Rate throttling has zero overhead when disabled (default)
10. Position tracking has zero overhead when disabled (default)

---

## 8. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Trade bust of deallocated orders requires re-allocation | Complex state management | Keep order metadata in TradeRegistry even after deallocation; bust fires event but logs warning if order cannot be restored |
| Rate tracker memory for large MaxAccounts | Memory bloat | Use template parameter; default to 10000 accounts; document sizing |
| LMM match interacts with iceberg reveals | Edge case in fill loop | Test specifically: MM iceberg order at level, verify tranche reveal after MM allocation |
| Daily limit lock state interacts with VI auction | Two halt mechanisms racing | Daily limits take precedence (regulatory); if already in VI auction and daily limit hit, transition to LockLimit |
| restore_order during auction phase | Order goes to collection, not matching | Document: restored orders obey current phase rules, same as new orders |
