# Phase 2: Session State, Auctions, Iceberg — Design

**Date:** 2026-03-24
**Depends on:** Phase 1 exchange-core
**Goal:** Add the 3 critical architecture gaps that block all per-exchange implementations.

---

## 1. Session State Machine

### 1.1 SessionState Enum

```cpp
enum class SessionState : uint8_t {
    Closed,          // No orders accepted
    PreOpen,         // Orders accepted, no matching (auction collection)
    OpeningAuction,  // Uncrossing in progress
    Continuous,      // Normal matching (current behavior)
    PreClose,        // Orders accepted for closing auction, no matching
    ClosingAuction,  // Uncrossing in progress
    Halt,            // Trading halted (circuit breaker / volatility)
    VolatilityAuction, // Volatility interruption auction
};
```

### 1.2 Engine Changes

Add to MatchingEngine:
- `SessionState current_state_{SessionState::Continuous}` — backward compatible default
- `void set_session_state(SessionState new_state, Timestamp ts)` — transition method
- In `new_order()`: call `derived().is_order_allowed_in_phase(req, current_state_)` after existing validation
- In `new_order()`: if state is auction collection phase (PreOpen/PreClose), insert into book but DO NOT call `match_order()` — just fire OrderAccepted + L3/L2/L1 callbacks
- `SessionState session_state() const` — accessor

### 1.3 New CRTP Hooks

```cpp
// Called before transitioning state. Return false to block transition.
bool on_session_transition(SessionState old_state, SessionState new_state, Timestamp ts) {
    return true;
}

// Called during new_order validation. Return false to reject.
bool is_order_allowed_in_phase(const OrderRequest& req, SessionState state) {
    return true;  // default: all orders allowed in all phases
}
```

### 1.4 New Callback Event

```cpp
struct MarketStatus {
    SessionState state;
    Timestamp ts;
};
```

Add `on_market_status(const MarketStatus&)` to MarketDataListenerBase.

### 1.5 Behavior by Phase

| Phase | Accept Orders | Match | IOC/FOK Valid | Stop Triggers |
|-------|--------------|-------|---------------|---------------|
| Closed | No | No | No | No |
| PreOpen | Yes | No (collect) | No | No |
| OpeningAuction | No (uncrossing) | Auction algo | No | No |
| Continuous | Yes | Yes (FIFO/ProRata) | Yes | Yes |
| PreClose | Yes | No (collect) | No | No |
| ClosingAuction | No (uncrossing) | Auction algo | No | No |
| Halt | Configurable | No | No | No |
| VolatilityAuction | Yes (collect) | No | No | No |

The "No" entries are defaults — exchange implementations override via `is_order_allowed_in_phase()`.

---

## 2. Auction Uncrossing Algorithm

### 2.1 Equilibrium Price Calculation

The universal uncrossing algorithm (used by virtually all exchanges):

1. For each candidate price P from best ask to best bid:
   - Buy volume at P = sum of all buy orders with price >= P
   - Sell volume at P = sum of all sell orders with price <= P
   - Matched volume = min(buy_volume, sell_volume)
   - Imbalance = |buy_volume - sell_volume|

2. Select the price that:
   - **Maximizes matched volume** (primary criterion)
   - **Minimizes imbalance** (secondary — tiebreaker)
   - **Closest to reference price** (tertiary — tiebreaker)

3. Execute all matchable orders at the single auction price.

### 2.2 Implementation

```cpp
struct AuctionResult {
    Price price;
    Quantity matched_volume;
    Quantity buy_surplus;   // unfilled buy qty at auction price
    Quantity sell_surplus;  // unfilled sell qty at auction price
};

// New method on MatchingEngine
AuctionResult calculate_auction_price(Price reference_price) const;
void execute_auction(Timestamp ts);  // uncross at calculated price
```

### 2.3 Auction Execution Flow

```
set_session_state(OpeningAuction, ts)
  |
  +-> calculate_auction_price(reference) -> AuctionResult
  |
  +-> if matched_volume > 0:
  |     for each buy order with price >= auction_price (best first):
  |       for each sell order with price <= auction_price (best first):
  |         fill at auction_price (not at individual order prices)
  |         fire OrderFilled/PartiallyFilled, Trade, L3, L2
  |
  +-> fire TopOfBook with post-auction state
  |
  +-> set_session_state(Continuous, ts)  // or next phase
```

### 2.4 Indicative Price During Collection

During PreOpen/PreClose, the engine periodically calculates what the auction price
WOULD be and fires a callback:

```cpp
struct IndicativePrice {
    Price price;
    Quantity matched_volume;
    Quantity buy_surplus;
    Quantity sell_surplus;
    Timestamp ts;
};
```

Add `on_indicative_price(const IndicativePrice&)` to MarketDataListenerBase.
Call `derived().should_publish_indicative()` CRTP hook to control frequency.

---

## 3. Iceberg / Display Quantity Orders

### 3.1 Order Struct Changes

Add to `Order`:
```cpp
Quantity display_qty;    // visible quantity (0 = fully visible, no iceberg)
Quantity total_qty;      // original total quantity (same as quantity for non-iceberg)
```

### 3.2 Behavior

- **Non-iceberg (display_qty == 0):** Behaves exactly as today. No change.
- **Iceberg (display_qty > 0):**
  - `remaining_quantity` starts at `display_qty` (only visible portion)
  - `total_qty` tracks the full remaining amount including hidden
  - When a fill exhausts the visible tranche (`remaining_quantity == 0`):
    - Reveal next tranche: `remaining_quantity = min(display_qty, total_qty - filled_quantity)`
    - Order goes to BACK of queue at its price level (loses time priority on reveal)
    - Fire L3 OrderBookAction (Add — new tranche) and L2 DepthUpdate
  - Market data reports `display_qty` in depth, not hidden quantity

### 3.3 OrderRequest Changes

Add to `OrderRequest`:
```cpp
Quantity display_qty;  // 0 = fully visible (default), > 0 = iceberg
```

### 3.4 Validation

- `display_qty` must be 0 or > 0
- If > 0: `display_qty` must be <= `quantity` and aligned to `lot_size`
- `derived().get_min_display_qty(req)` CRTP hook for exchange-specific minimum

---

## 4. Supporting Features (Quick Wins)

### 4.1 Mass Cancel

```cpp
// Cancel all orders for an account
size_t mass_cancel(uint64_t account_id, Timestamp ts);

// Cancel all orders
size_t mass_cancel_all(Timestamp ts);
```

Returns count of cancelled orders. Fires OrderCancelled for each.

### 4.2 Max Order Size

Add `Quantity max_order_size` to EngineConfig (0 = no limit).
Check in `new_order()` validation.

### 4.3 Dynamic Price Bands

Replace static band check with:
```cpp
auto [low, high] = derived().calculate_dynamic_bands(last_trade_price_);
```

Default implementation returns the static bands from EngineConfig (backward compatible).

---

## 5. Implementation Tasks

### Group A — Foundation (no dependencies, parallel)

| # | Task | Est. Lines | Description |
|---|------|-----------|-------------|
| A1 | SessionState enum + MarketStatus event | ~80 | Add enum to types.h, MarketStatus to events.h, on_market_status to listeners |
| A2 | Iceberg fields on Order + OrderRequest | ~60 | Add display_qty, total_qty fields, update validation |
| A3 | Mass cancel API | ~120 | mass_cancel(account), mass_cancel_all() on engine + tests |
| A4 | Max order size + dynamic bands hooks | ~80 | Add to EngineConfig, validation, CRTP hook for dynamic bands |

### Group B — Session State Machine (depends on A1)

| # | Task | Est. Lines | Description |
|---|------|-----------|-------------|
| B1 | Session state tracking in engine | ~150 | current_state_, set_session_state(), on_session_transition CRTP hook, is_order_allowed_in_phase hook, phase-aware new_order (collect vs match) |
| B2 | Session state tests | ~200 | Test state transitions, order rejection by phase, collect-only in PreOpen |

### Group C — Auction (depends on B1)

| # | Task | Est. Lines | Description |
|---|------|-----------|-------------|
| C1 | Auction price calculation | ~180 | calculate_auction_price() — walk all price levels, find equilibrium |
| C2 | Auction execution | ~200 | execute_auction() — fill at single price, callbacks, state transition |
| C3 | Indicative price callback | ~80 | IndicativePrice event, on_indicative_price, recalculate during collection |
| C4 | Auction tests | ~200 | Test uncrossing with various book states, indicative price updates |

### Group D — Iceberg Matching (depends on A2)

| # | Task | Est. Lines | Description |
|---|------|-----------|-------------|
| D1 | Iceberg matching logic | ~150 | Display qty in matching, tranche reveal, re-queue at back, L3/L2 callbacks |
| D2 | Iceberg tests | ~200 | Test iceberg fill, tranche reveal, priority loss, market data correctness |

### Group E — Integration Tests (depends on all above)

| # | Task | Est. Lines | Description |
|---|------|-----------|-------------|
| E1 | Journal scenarios: session state + auction | ~200 | Opening auction, closing auction, phase transitions, order rejection by phase |
| E2 | Journal scenarios: iceberg orders | ~150 | Iceberg resting, partial fill, tranche reveal, full fill |

### Dependency Graph

```
Group A: [A1] [A2] [A3] [A4]          (all parallel)
              |       |
Group B: [B1] -> [B2]                  (session state, depends on A1)
              |
Group C: [C1] -> [C2] -> [C3]          (auction, depends on B1)
          [C4] (after C1-C3)

Group D: [D1] -> [D2]                  (iceberg, depends on A2)

Group E: [E1] [E2]                      (after C4 and D2)
```
