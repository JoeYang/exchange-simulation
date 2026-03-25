# Phase 3: Multi-Instrument Framework + CME Globex Simulator

**Date:** 2026-03-25
**Depends on:** Phase 1 + Phase 2
**Goal:** Build a multi-instrument exchange simulator framework and implement CME Globex as the first venue.

---

## 1. Multi-Instrument Simulator Framework

### 1.1 ExchangeSimulator Template

```cpp
// exchange-sim/exchange_simulator.h
namespace exchange {

using InstrumentId = uint32_t;

struct InstrumentConfig {
    InstrumentId id;
    std::string symbol;
    EngineConfig engine_config;
};

template <typename ExchangeImpl, typename OrderListenerT, typename MdListenerT,
          typename MatchAlgoT = FifoMatch,
          size_t MaxOrders = 10000, size_t MaxPriceLevels = 1000,
          size_t MaxOrderIds = 100000>
class ExchangeSimulator {
    using EngineType = MatchingEngine<ExchangeImpl, OrderListenerT, MdListenerT,
                                      MatchAlgoT, MaxOrders, MaxPriceLevels, MaxOrderIds>;

    std::unordered_map<InstrumentId, std::unique_ptr<EngineType>> engines_;
    OrderListenerT& order_listener_;
    MdListenerT& md_listener_;
    SessionState current_state_{SessionState::Closed};

public:
    ExchangeSimulator(OrderListenerT& ol, MdListenerT& ml);

    // Instrument management
    void add_instrument(const InstrumentConfig& cfg);
    void remove_instrument(InstrumentId id);
    EngineType* get_engine(InstrumentId id);
    size_t instrument_count() const;

    // Order routing
    void new_order(InstrumentId instrument, const OrderRequest& req);
    void cancel_order(InstrumentId instrument, OrderId id, Timestamp ts);
    void modify_order(InstrumentId instrument, const ModifyRequest& req);

    // Exchange-wide operations
    void set_session_state(SessionState state, Timestamp ts);
    void execute_all_auctions(Timestamp ts);  // each instrument uses own ref price
    void mass_cancel_all(Timestamp ts);
    void trigger_expiry(Timestamp now, TimeInForce tif);

    // State
    SessionState session_state() const;
};
```

### 1.2 Directory Structure

```
exchange-sim/
├── BUILD.bazel
├── exchange_simulator.h       # Multi-instrument framework
├── exchange_simulator_test.cc
├── instrument_config.h        # InstrumentConfig, InstrumentId
└── ohlcv.h                   # OHLCV statistics tracker
```

---

## 2. OHLCV Statistics (Core Addition)

```cpp
// exchange-core/ohlcv.h
struct OhlcvStats {
    Price open{0};
    Price high{0};
    Price low{0};
    Price close{0};
    Quantity volume{0};
    Quantity turnover{0};  // sum of price * qty for VWAP
    uint32_t trade_count{0};

    void on_trade(Price price, Quantity qty);
    void reset();
    Price vwap() const;  // turnover / volume
};
```

Tracks per-session OHLCV. Updated on every trade. Reset on session transition.

---

## 3. CME Globex Implementation

### 3.1 CME Session Schedule

```
Sunday 17:00 CT → Friday 16:00 CT (nearly 24h trading)

Daily cycle:
  17:00 CT  Pre-Open (order entry, no matching)
  17:00 CT  Opening (group-based staggered open)
  ~17:01 CT Continuous Trading
  16:00 CT  Pre-Close / Settlement
  16:00 CT  Closed (maintenance window)
  17:00 CT  Next session opens
```

Simplified for simulator:
```
Closed → PreOpen → OpeningAuction → Continuous → PreClose → ClosingAuction → Closed
```

### 3.2 CME Exchange Class

```cpp
// cme/cme_exchange.h
namespace exchange {
namespace cme {

template <typename OrderListenerT, typename MdListenerT>
class CmeExchange : public MatchingEngine<
    CmeExchange<OrderListenerT, MdListenerT>,
    OrderListenerT, MdListenerT, FifoMatch> {

    using Base = MatchingEngine<CmeExchange, OrderListenerT, MdListenerT, FifoMatch>;

public:
    using Base::Base;

    // --- CRTP overrides ---

    // CME SMP: check account_id match
    bool is_self_match(const Order& aggressor, const Order& resting) {
        return aggressor.account_id == resting.account_id;
    }

    // CME default SMP action: cancel newest (aggressor)
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }

    // CME modify policy: cancel-replace (standard)
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }

    // CME order validation per phase
    bool is_order_allowed_in_phase(const OrderRequest& req, SessionState state) {
        switch (state) {
            case SessionState::PreOpen:
            case SessionState::PreClose:
            case SessionState::VolatilityAuction:
                // No IOC/FOK during auction collection
                return req.tif != TimeInForce::IOC && req.tif != TimeInForce::FOK;
            case SessionState::Closed:
                return false;
            default:
                return true;
        }
    }

    // CME dynamic price bands: ±X% of last trade or settlement
    std::pair<Price, Price> calculate_dynamic_bands(Price reference) {
        if (reference == 0) return {0, 0};
        Price band = reference * band_pct_ / 100;
        return {reference - band, reference + band};
    }

    // CME stop trigger: standard
    bool should_trigger_stop(Price last_trade, const Order& stop) {
        if (stop.side == Side::Buy) return last_trade >= stop.price;
        return last_trade <= stop.price;
    }

private:
    int64_t band_pct_{5};  // 5% default, configurable per product
};

}}  // namespace exchange::cme
```

### 3.3 CME Product Configuration

```cpp
struct CmeProductConfig {
    InstrumentId id;
    std::string symbol;         // "ESH6", "NQM6", "CLK6"
    std::string product_group;  // "Equity Index", "Energy", "Metals"
    Price tick_size;            // ES=25 (0.25 * 10000), CL=10 (0.01 * 10000)
    Quantity lot_size;          // 10000 (1 contract)
    Quantity max_order_size;
    int64_t band_pct;          // price band percentage
    Price daily_limit_up;      // lock limit up
    Price daily_limit_down;    // lock limit down
};
```

### 3.4 CME-Specific Features (Phase 3 Scope)

| Feature | Priority | Notes |
|---------|----------|-------|
| Basic FIFO matching | Already done | Default |
| SMP (cancel newest) | Already done via CRTP | Account-based |
| Session state lifecycle | Phase 2 done | Closed → PreOpen → Continuous → Closed |
| IOC/FOK rejection in auction | Phase 2 done | `is_order_allowed_in_phase` |
| Dynamic price bands | Phase 2 done | `calculate_dynamic_bands` |
| OHLCV statistics | This phase | New core addition |
| Multi-instrument routing | This phase | ExchangeSimulator |

**Deferred to Phase 4:**
- LMM priority (CME Lead Market Maker)
- Velocity logic
- 6 additional matching algorithms
- Implied/spread trading
- Per-order SMP instruction

---

## 4. Implementation Tasks

### Group A — Core Additions (parallel)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| A1 | OHLCV statistics | `exchange-core/ohlcv.h`, `ohlcv_test.cc` | ~120 | OhlcvStats struct with on_trade, reset, vwap. Tests. |
| A2 | InstrumentId type + config | `exchange-sim/instrument_config.h` | ~40 | InstrumentId, InstrumentConfig, CmeProductConfig |

### Group B — Multi-Instrument Framework (depends on A2)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| B1 | ExchangeSimulator template | `exchange-sim/exchange_simulator.h`, `exchange_simulator_test.cc` | ~200 | Multi-instrument routing, exchange-wide session state, test with 3 instruments |

### Group C — CME Implementation (depends on B1)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| C1 | CmeExchange CRTP class | `cme/cme_exchange.h`, `cme/BUILD.bazel` | ~120 | CRTP overrides for SMP, bands, phase validation |
| C2 | CME product configs | `cme/cme_products.h` | ~100 | ES, NQ, CL, GC, ZB configs with real tick sizes |
| C3 | CME simulator integration | `cme/cme_simulator.h`, `cme/cme_simulator_test.cc` | ~200 | CmeSimulator = ExchangeSimulator<CmeExchange>, test full lifecycle |

### Group D — CME Tests (depends on C3)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| D1 | CME session lifecycle test | `cme/cme_lifecycle_test.cc` | ~200 | Full day: PreOpen → auction → continuous → close for ES |
| D2 | CME multi-product test | `cme/cme_multi_product_test.cc` | ~200 | ES + NQ + CL simultaneous trading |
| D3 | CME journal scenarios | `test-journals/cme_*.journal` | ~200 | Journal tests for CME-specific behavior |

### Dependency Graph

```
Group A: [A1] [A2]                (parallel)
              |
Group B: [B1]                     (depends on A2)
              |
Group C: [C1] [C2]               (parallel, C1 depends on B1 conceptually)
              |
         [C3]                     (depends on C1 + C2 + B1)
              |
Group D: [D1] [D2] [D3]          (parallel, depend on C3)
```
