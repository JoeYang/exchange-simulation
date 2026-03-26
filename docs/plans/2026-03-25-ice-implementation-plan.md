# ICE Exchange Simulator — Implementation Plan

**Date:** 2026-03-25
**Status:** Plan
**Prerequisite:** CME Phase 4 (iLink3/MDP3 codec, E2E test framework) — ICE builds on the same foundation
**Reference:** `docs/ice-exchange-analysis.md` for protocol research

---

## 1. What's Already Reusable

### 1.1 From exchange-core

Everything built in Phases 1 and 2 is directly reusable for ICE:

| Component | Location | ICE Reuse |
|---|---|---|
| MatchingEngine CRTP base | `exchange-core/matching_engine.h` | Full — identical CRTP pattern |
| FIFO algorithm | `exchange-core/match_algo.h` | Full — primary ICE algorithm |
| ProRata algorithm | `exchange-core/match_algo.h` | Partial — base for GTBPR |
| Order types (all 4) | `exchange-core/types.h` | Full |
| TIF types (all 5) | `exchange-core/types.h` | Full |
| SessionState machine | `exchange-core/matching_engine.h` | Full |
| Iceberg/display_qty | `exchange-core/types.h` | Full |
| SMP hooks | `exchange-core/matching_engine.h` | Full — is_self_match + get_smp_action |
| Dynamic price bands | `exchange-core/matching_engine.h` | Full — calculate_dynamic_bands |
| Mass cancel | `exchange-core/matching_engine.h` | Full |
| OHLCV statistics | `exchange-core/ohlcv.h` | Full |
| Max order size | `exchange-core/matching_engine.h` | Full |
| L1/L2/L3 market data | `exchange-core/listeners.h` | Full |

### 1.2 From CME Phase 4

The CME protocol work in Phase 4 builds infrastructure that ICE will reuse:

| Component | Location | ICE Reuse |
|---|---|---|
| SBE header/codec primitives | `cme/codec/sbe_header.h` | Full — SBE is SBE |
| TCP server | `tools/tcp_server.h` | Full — ICE FIX OS is TCP; BOE is TCP |
| UDP multicast publisher | `tools/udp_multicast.h` | Full — iMpact is UDP multicast |
| E2E test journal format | `test-harness/` | Extend with ICE-specific actions |
| E2E test runner base | `cme/e2e/e2e_test_runner.h` | Inherit/adapt for ICE |
| Journal scenarios (generic) | `test-journals/e2e/` | Directly reuse |

### 1.3 ICE vs CME Complexity Delta

ICE is significantly simpler than CME in most dimensions:

| Dimension | CME Complexity | ICE Complexity |
|---|---|---|
| Matching algorithms | 8+ variants | 2 (FIFO + GTBPR) |
| Closing mechanism | Full auction uncrossing | VWAP window — no auction needed |
| Order entry protocol | iLink3 SBE (proprietary, complex) | FIX 4.2 (well-known) + SBE BOE (new) |
| Market data protocol | MDP3 SBE (complex multicast) | iMpact binary (simpler message set) |
| Market maker priority | LMM tier in several algorithms | None |
| Implied matching | Multi-leg + inter-commodity | Calendar IN/OUT only (descoped for v1) |

**Rough complexity estimate:** ICE simulator ≈ 60% of the CME simulator effort.

---

## 2. Architecture

### 2.1 Component Diagram

```
         Client (test runner)                  Market Data Consumer
               |                                        ^
               | FIX 4.2 over TCP                      | iMpact UDP multicast
               v                                        |
       +------------------+                  +-------------------+
       | ICE FIX Gateway  |  decode          | iMpact Publisher  | encode
       | (FIX OS decoder) |----+             | (multicast feed)  |-------> iMpact bytes
       +------------------+    |             +-------------------+
                               |                      ^
                               v                      |
                    +---------------------+    engine callbacks
                    |   IceSimulator      |    (OrderListener +
                    |                     |     MdListener)
                    |  Brent ──┐          |
                    |  Gas     │  engine  |
                    |  Euribor │          |
                    |  Cocoa   │          |
                    └──────────┴──────────+
                               |
                               | ExecutionReport (FIX 4.2)
                               v
                    +------------------+
                    | FIX Exec Report  |
                    | Publisher        |
                    +------------------+
                               |
                               v FIX 4.2 text
                    Client receives ack/fill/cancel
```

### 2.2 IceExchange CRTP Class Hierarchy

```
MatchingEngine<IceExchange<...>, ...>    [exchange-core — generic]
        ^
        |  inherits via CRTP
        |
IceExchange<OL, ML, MatchAlgoT, ...>    [ice/ — ICE-specific policy]
        ^
        |  product configuration
        |
IceProducts (configs for Brent, Euribor, Cocoa, etc.)
```

Two CRTP template specializations for ICE:

```
IceExchange<OL, ML, FifoMatch>     — energy, softs, equity index
IceExchange<OL, ML, GtbprMatch>    — STIR products (Euribor, SONIA, SOFR)
```

### 2.3 Full Pipeline for One Order (FIX OS)

```
1. Test runner encodes FIX 4.2 NewOrderSingle (D message)
2. IceFIXGateway decodes → OrderRequest struct
3. IceSimulator.new_order(instrument, request)
4. IceExchange engine processes → fires callbacks
5. IceFIXExecPublisher encodes → FIX ExecutionReport (8 message, text)
6. IceImpactPublisher encodes → iMpact binary incremental message (UDP)
7. Test runner decodes both, compares against EXPECT lines
```

---

## 3. IceExchange CRTP Class Design

### 3.1 CRTP Hook Overrides

```cpp
template <typename OL, typename ML, typename MatchAlgoT = FifoMatch,
          size_t MaxOrders = 10000, ...>
class IceExchange : public MatchingEngine<
    IceExchange<OL, ML, MatchAlgoT, ...>, OL, ML, MatchAlgoT, ...> {
public:
    // SMP: ICE uses tag-based SMP (SMP ID stored in account_id for simulator;
    //       full implementation needs extended Order struct with smp_id field)
    bool is_self_match(const Order& aggressor, const Order& resting);

    // ICE SMP actions: RTO (cancel resting), ATO (cancel aggressor), CABO (cancel both)
    SmpAction get_smp_action();

    // ICE modify policy: cancel-replace (same as CME)
    ModifyPolicy get_modify_policy();

    // ICE phase validation:
    //   Pre-Open: reject Market, IOC, FOK
    //   Closed: reject all
    //   Continuous + Halt: allow all
    //   (ICE has no VolatilityAuction — uses IPL hold instead)
    bool is_order_allowed_in_phase(const OrderRequest& req, SessionState state);

    // ICE IPL dynamic bands:
    //   band = last_trade ± ipl_width_
    //   ipl_width_ set per product; recalculates on band breach after hold period
    std::pair<Price, Price> calculate_dynamic_bands(Price reference);

    // ICE settlement: VWAP of trades during settlement_window_ period
    //   Engine calls this at end of settlement period
    Price calculate_settlement_price();

    // ICE session transition:
    //   Key logic: no volatility auction state — IPL hold uses Halt state
    bool on_session_transition(SessionState old_state, SessionState new_state, Timestamp ts);

    // ICE circuit breaker: enter Halt for ipl_hold_ms_, then resume Continuous
    void on_circuit_breaker(int level, Price trigger_price);
};
```

### 3.2 CRTP Hook Comparison: ICE vs CME

| Hook | CME Behavior | ICE Behavior | Delta |
|---|---|---|---|
| `is_self_match` | Same account_id | Same SMP ID (tag 9821) | Different — ICE needs SMP ID field on Order |
| `get_smp_action` | CancelNewest | Configurable (RTO/ATO/CABO) | Slightly different |
| `get_modify_policy` | CancelReplace | CancelReplace | Same |
| `is_order_allowed_in_phase` | Blocks Market/IOC/FOK in PreOpen | Same restrictions | Same |
| `calculate_dynamic_bands` | ±band_pct_ around last trade | ±ipl_width_ (fixed points, not pct) | Different calculation |
| `calculate_settlement_price` | Last trade price | VWAP in settlement window | Different |
| `on_circuit_breaker` | Not yet implemented | IPL hold N seconds then resume | New logic |
| `on_session_transition` | Allow-all default | No volatility auction | Simpler (omit VA state) |

---

## 4. New Components Required

### 4.1 GTBPR Match Algorithm

```
ice/gtbpr_match.h     (~100 lines)
```

A new `MatchAlgoT` implementing Gradual Time-Based Pro Rata for STIR products:

```
Algorithm at price level:
  1. Find priority order (first order at best price; size >= collar; allocation <= cap)
  2. Fill priority order first (up to cap quantity)
  3. Remaining quantity: pro-rata allocation across all resting orders
     weighted by: quantity * time_weight(age_in_seconds, time_weight_factor)
  4. Remainder (rounding): to oldest resting order
```

Parameters (configurable per product):
- `collar`: minimum order size for priority eligibility
- `cap`: maximum priority allocation per aggressor
- `time_weight_factor`: controls how aggressively older orders are favored

**Pros of a new MatchAlgo type:**
- Fits cleanly into the existing MatchAlgoT template parameter
- No changes to MatchingEngine required
- Testable independently

**Cons:**
- More complex internal state (needs order age tracking)

### 4.2 ICE FIX OS Codec

```
ice/fix/
  fix_parser.h/.cc          (~150 lines) — FIX 4.2 tag/value parser
  fix_encoder.h/.cc          (~150 lines) — FIX 4.2 tag/value encoder
  ice_fix_messages.h         (~100 lines) — ICE-specific message structs
  ice_fix_gateway.h/.cc      (~200 lines) — Decodes FIX → OrderRequest
  ice_fix_exec_publisher.h   (~150 lines) — Engine events → FIX Exec Reports
```

**Why FIX 4.2 first (not BOE):**
- FIX 4.2 is well-documented and widely understood
- BOE is very new (live Feb 2026) — specification not publicly available yet
- FIX allows full E2E testing without waiting for BOE spec access
- BOE can be added as Phase 5B after FIX is working

**Pros:** FIX 4.2 is simpler to implement than SBE; many open-source parsers exist
**Cons:** Text parsing is slower; not production-grade for latency-sensitive work

### 4.3 ICE iMpact Market Data Codec

```
ice/impact/
  impact_messages.h          (~120 lines) — iMpact binary structs (Add/Modify, Withdrawal, Deal, Status)
  impact_encoder.h/.cc        (~200 lines) — Engine events → iMpact binary messages
  impact_publisher.h          (~150 lines) — Packages incremental messages + UDP multicast
  impact_decoder.h/.cc        (~150 lines) — For test verification
```

**iMpact Message Types to Implement (v1):**

| Message | Type Code | Description |
|---|---|---|
| Bundle Start | `S` | Marks start of atomic update bundle |
| Bundle End | `E` | Marks end of bundle; includes sequence number |
| Add/Modify Order | `E` | New order or quantity update in book (MBO) |
| Order Withdrawal | `F` | Order cancelled or fully filled |
| Deal/Trade | `T` | Trade execution |
| Market Status | `M` | Session state change (pre-open, continuous, halt) |
| Snapshot Order | `D` | Full book snapshot during recovery |
| Price Level | — | Aggregated best bid/offer summary |

**Sequence numbering:** Assign block sequence number per bundle. Bundle contains N messages. Client derives per-message context from bundle header.

### 4.4 IceSimulator Wrapper

```
ice/ice_simulator.h     (~150 lines)
```

Analogous to `cme/cme_simulator.h` — multi-instrument wrapper:
- Holds a map of `instrument_id → IceExchange<...>` engine
- Routes `new_order(instrument, req)` calls to the correct engine
- Manages per-product EngineConfig (tick size, lot size, IPL width, matching algo)
- Schedules session transitions (pre-open → continuous → settlement → closed)

### 4.5 ICE Product Configurations

```
ice/ice_products.h     (~150 lines)
```

Configures products analogously to `cme/cme_products.h`:

```cpp
struct IceProductConfig {
    EngineConfig engine;         // tick_size, lot_size, max_order_size
    MatchAlgoType algo;          // FIFO or GTBPR
    Price ipl_width;             // interval price limit band half-width
    int ipl_hold_ms;             // hold period in ms (5000 or 15000)
    int ipl_recalc_ms;           // IPL recalculation interval (15000 or 5000)
    Price ncr_width;             // no cancellation range half-width
    Price rl_width;              // reasonability limit half-width
    int settlement_window_secs;  // settlement window duration (120-300 sec)
    // Session times are handled in IceSimulator
};

namespace ice_products {
    extern const IceProductConfig BRENT;    // FIFO, 5-sec IPL
    extern const IceProductConfig GASOIL;   // FIFO, 5-sec IPL
    extern const IceProductConfig NATURAL_GAS; // FIFO, 5-sec IPL
    extern const IceProductConfig COCOA;    // FIFO, 15-sec IPL
    extern const IceProductConfig ROBUSTA_COFFEE; // FIFO, 15-sec IPL
    extern const IceProductConfig WHITE_SUGAR;    // FIFO, 15-sec IPL
    extern const IceProductConfig EURIBOR;  // GTBPR, STIR
    extern const IceProductConfig SONIA;    // GTBPR, STIR
    extern const IceProductConfig FTSE100;  // FIFO, equity
    extern const IceProductConfig MSCI_WORLD; // FIFO, equity
}
```

---

## 5. E2E Test Framework Reuse

### 5.1 Journal Format Extension

The existing journal format (from Phase 4C) is extended with ICE-specific actions and expectations:

```
# Actions (client sends FIX OS messages)
ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=BRENT cl_ord_id=1 account=FIRM_A \
    side=BUY price=8200000000 qty=10000 type=LIMIT tif=DAY

ACTION ICE_FIX_CANCEL ts=2000 instrument=BRENT cl_ord_id=2 orig_cl_ord_id=1

ACTION ICE_FIX_REPLACE ts=3000 instrument=BRENT cl_ord_id=3 orig_cl_ord_id=1 \
    price=8210000000 qty=10000

ACTION ICE_FIX_MASS_CANCEL ts=4000 instrument=BRENT account=FIRM_A

# Expectations (verify FIX exec reports + iMpact market data)
EXPECT ICE_EXEC_NEW instrument=BRENT ord_id=1 cl_ord_id=1 status=NEW
EXPECT ICE_EXEC_FILL instrument=BRENT ord_id=1 cl_ord_id=1 \
    fill_price=8200000000 fill_qty=10000 status=FILLED
EXPECT ICE_EXEC_CANCELLED instrument=BRENT ord_id=1 status=CANCELLED

EXPECT ICE_MD_ADD instrument=BRENT side=BUY price=8200000000 qty=10000
EXPECT ICE_MD_REMOVE instrument=BRENT side=BUY price=8200000000
EXPECT ICE_MD_TRADE instrument=BRENT price=8200000000 qty=10000 aggressor=SELL
EXPECT ICE_MD_STATUS instrument=BRENT state=CONTINUOUS
```

### 5.2 Test Journal Scenarios

ICE-specific scenarios to cover:

| Journal | Description |
|---|---|
| `ice_e2e_brent_basic.journal` | Basic limit order lifecycle: add → fill → cancel |
| `ice_e2e_euribor_gtbpr.journal` | GTBPR matching with priority order allocation |
| `ice_e2e_smp.journal` | Self-match prevention (cancel resting, cancel aggressor, cancel both) |
| `ice_e2e_ipl_hold.journal` | IPL trigger: order outside band → hold → resume with new band |
| `ice_e2e_session_lifecycle.journal` | Pre-open (no matching) → continuous → settlement VWAP → closed |
| `ice_e2e_iceberg.journal` | Iceberg order: first tranche fills → next tranche queued at back |
| `ice_e2e_multi_product.journal` | Concurrent Brent + Euribor + Cocoa; no cross-instrument interference |
| `ice_e2e_mass_cancel.journal` | Mass cancel by account across multiple open orders |

---

## 6. Task Breakdown

### Phase 5A — ICE Core Algorithm (Foundation)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 28 | GTBPR matching algorithm | `ice/gtbpr_match.h` + test | ~200 | — | New MatchAlgoT for STIR products: priority order + time-weighted pro-rata. Unit tests cover collar/cap boundary, time weight aging, remainder allocation |
| 29 | IceExchange CRTP class | `ice/ice_exchange.h` + test | ~200 | 28 | CRTP overrides: ICE SMP, IPL dynamic bands, VWAP settlement, phase validation, circuit breaker (hold+resume). Tests cover all override behaviors |

### Phase 5B — ICE Product Configurations

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 30 | ICE product configs | `ice/ice_products.h` | ~150 | 29 | EngineConfig for Brent, Gasoil, Natural Gas, Cocoa, Robusta Coffee, White Sugar, Euribor, SONIA, FTSE100, MSCI World |
| 31 | IceSimulator wrapper | `ice/ice_simulator.h` + test | ~200 | 29, 30 | Multi-instrument wrapper; routes orders by instrument; manages session state per product |

### Phase 5C — FIX OS Protocol Codec

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 32 | FIX 4.2 parser | `ice/fix/fix_parser.h/.cc` + test | ~200 | — | Tag/value FIX 4.2 parser. Parses NewOrderSingle (D), Cancel (F), Replace (G), Execution Report (8). Round-trip tests |
| 33 | FIX 4.2 encoder | `ice/fix/fix_encoder.h/.cc` + test | ~150 | — | Encodes FIX Execution Reports from engine events. Tests encode every exec report type |
| 34 | ICE FIX Gateway | `ice/fix/ice_fix_gateway.h/.cc` + test | ~200 | 32 | Decodes incoming FIX messages → OrderRequest; calls IceSimulator |
| 35 | ICE FIX Exec Publisher | `ice/fix/ice_fix_exec_publisher.h` + test | ~150 | 33 | OrderListenerBase that encodes each callback as FIX Execution Report |

### Phase 5D — iMpact Market Data Codec

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 36 | iMpact message structs | `ice/impact/impact_messages.h` | ~120 | — | Binary structs for: Add/Modify Order (`E`), Withdrawal (`F`), Deal (`T`), Market Status (`M`), Bundle Start/End, Snapshot Order (`D`) |
| 37 | iMpact encoder | `ice/impact/impact_encoder.h/.cc` + test | ~200 | 36 | Engine callbacks → iMpact binary messages; assigns block sequence numbers; bundles related updates |
| 38 | iMpact decoder | `ice/impact/impact_decoder.h/.cc` + test | ~150 | 36 | iMpact bytes → decoded structs; for test verification |
| 39 | iMpact publisher | `ice/impact/impact_publisher.h` + test | ~150 | 37, `tools/udp_multicast.h` | MdListenerBase that encodes callbacks into iMpact bundles and publishes via UDP multicast |
| 40 | iMpact codec round-trip tests | `ice/impact/impact_codec_test.cc` | ~150 | 37, 38 | Encode → decode → verify field equality for every message type |

### Phase 5E — E2E Test Framework

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 41 | ICE E2E journal format | `test-harness/` updates | ~100 | — | Add `ICE_FIX_*` action types and `ICE_EXEC_*`, `ICE_MD_*` expect types |
| 42 | ICE E2E test runner | `ice/e2e/ice_e2e_test_runner.h/.cc` + test | ~300 | 34, 35, 39, 41 | Full pipeline: parse ICE journal → encode FIX → gateway → IceSimulator → publishers → decode → verify. Inherits from or adapts CME E2E runner |
| 43 | ICE E2E test harness | `ice/e2e/ice_e2e_journal_test.cc` | ~80 | 42 | Discovers and runs all `test-journals/ice/*.journal` |

### Phase 5F — E2E Test Journals

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 44 | ICE journal generator | `test-journals/generate_ice_journals.cc` | ~400 | 42 | Runs ICE scenarios through pipeline, captures correct EXPECT lines |
| 45 | ICE-specific E2E journals | `test-journals/ice/*.journal` | ~500 | 44 | 8 scenarios: see Section 5.2 |

### Phase 5G — Live Simulator Binary (Optional)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 46 | ICE live simulator binary | `ice/ice_sim_runner.cc` | ~250 | 31, 34, 39, `tools/tcp_server.h`, `tools/udp_multicast.h` | `ice-sim --fix-port 9200 --impact-group 224.0.32.1:15000` |
| 47 | ICE sim config | `ice/ice_sim_config.h` | ~80 | 46 | Runtime config: port, multicast group, enabled products |

### Phase 5H — Testing Verification

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 48 | ICE test runner script | `test-journals/run_all_ice_tests.sh` | ~120 | 43 | Runs ALL ICE tests (unit + E2E journals) with color-coded summary. Models `run_all_journals.sh`. Discovers bazel test targets under `//ice/...` and journal files under `test-journals/ice/`. Reports pass/fail per target with failure details |
| 49 | GTBPR integration tests | `ice/gtbpr_integration_test.cc` + BUILD | ~300 | 30 | Engine-level GTBPR tests through real IceExchange (not isolated match algo). 10 scenarios: priority fill, time-weighted pro-rata, collar/cap boundaries (exact, +-1), FIFO vs GTBPR comparison (same orders → different fills), iceberg + GTBPR, sequential aggressors. Uses test listeners to verify fill callbacks |
| 50 | GTBPR E2E journal scenarios | `test-journals/ice/ice_e2e_gtbpr_*.journal` (4 files) | ~400 | 42, 44, 45 | Full pipeline GTBPR tests: FIX→IceExchange→iMpact. 4 journals: priority fill + cap enforcement, time-weighted pro-rata allocation, collar/cap boundary conditions, FIFO vs GTBPR comparison (BRENT vs EURIBOR with identical orders proving different allocations) |

---

## 7. Dependency Graph

```
Phase 5A (Algorithm + CRTP):
  [28] GTBPR Match
   └→ [29] IceExchange CRTP

Phase 5B (Products + Simulator):
  [29] ──→ [30] Product Configs
  [29] + [30] ──→ [31] IceSimulator

Phase 5C (FIX OS Codec):
  [32] FIX Parser    (independent)
  [33] FIX Encoder   (independent)
  [32] ──→ [34] FIX Gateway
  [33] ──→ [35] FIX Exec Publisher

Phase 5D (iMpact Codec):
  [36] iMpact Structs    (independent)
  [36] ──→ [37] iMpact Encoder
  [36] ──→ [38] iMpact Decoder
  [37] + udp_multicast ──→ [39] iMpact Publisher
  [37] + [38] ──→ [40] Round-trip Tests

Phase 5E (E2E Framework):
  [41] Journal Format Extension (parallel with 5A-5D)
  [34] + [35] + [39] + [41] + [31] ──→ [42] E2E Test Runner
                                         ──→ [43] E2E Bazel Harness

Phase 5F (Test Journals):
  [42] ──→ [44] Journal Generator ──→ [45] ICE Journals

Phase 5G (Live Sim — optional):
  [31] + [34] + [39] + tcp_server + udp_multicast ──→ [46] Live Binary ──→ [47] Config

Phase 5H (Testing Verification):
  [43] ──→ [48] ICE Test Runner Script
  [30] ──→ [49] GTBPR Integration Tests
  [42] + [44] + [45] ──→ [50] GTBPR E2E Journals
```

---

## 8. Dev Team Dispatch Strategy

With 4 devs (same team as CME Phase 4):

| Wave | Dev 1 | Dev 2 | Dev 3 | Dev 4 |
|------|-------|-------|-------|-------|
| **Wave 1** | Task 28 (GTBPR algo) | Task 32 (FIX parser) | Task 33 (FIX encoder) | Task 36 (iMpact structs) |
| **Wave 2** | Task 29 (IceExchange CRTP) | Task 34 (FIX gateway) | Task 35 (FIX exec pub) | Task 37 (iMpact encoder) |
| **Wave 3** | Task 30 (products) | Task 38 (iMpact decoder) | Task 39 (iMpact pub) | Task 40 (codec roundtrip) |
| **Wave 4** | Task 31 (IceSimulator) | Task 41 (journal format) | Task 42 (E2E runner) | Task 43 (E2E harness) |
| **Wave 5** | Task 44 (journal gen) | Task 45 (ICE journals) | — | — |
| **Wave 6** | Task 46 (live sim) | Task 47 (sim config) | — | — |
| **Wave 7** | Task 48 (ICE test runner) | Task 49 (GTBPR integration) | Task 50 (GTBPR E2E journals) | — |

**Critical path:** 28 → 29 → 31 → 42 → 44 → 45 (6 sequential steps, shorter than CME's 7)

---

## 9. File Structure (Target)

```
ice/
├── ice_exchange.h          # IceExchange CRTP class (Task 29)
├── ice_products.h          # Product configs (Task 30)
├── ice_simulator.h         # Multi-instrument wrapper (Task 31)
├── ice_sim_runner.cc        # Live simulator binary (Task 46, optional)
├── ice_sim_config.h         # Runtime config (Task 47, optional)
├── gtbpr_match.h            # GTBPR matching algorithm (Task 28)
├── fix/
│   ├── BUILD.bazel
│   ├── fix_parser.h/.cc     # FIX 4.2 parser (Task 32)
│   ├── fix_encoder.h/.cc    # FIX 4.2 encoder (Task 33)
│   ├── ice_fix_messages.h   # ICE FIX message structs
│   ├── ice_fix_gateway.h/.cc# FIX → OrderRequest (Task 34)
│   └── ice_fix_exec_publisher.h # Events → FIX (Task 35)
├── impact/
│   ├── BUILD.bazel
│   ├── impact_messages.h    # iMpact binary structs (Task 36)
│   ├── impact_encoder.h/.cc # Events → iMpact (Task 37)
│   ├── impact_decoder.h/.cc # iMpact → structs (Task 38)
│   ├── impact_publisher.h   # Multicast publisher (Task 39)
│   └── impact_codec_test.cc # Round-trip tests (Task 40)
└── e2e/
    ├── BUILD.bazel
    ├── ice_e2e_test_runner.h/.cc  # ICE E2E runner (Task 42)
    └── ice_e2e_journal_test.cc    # Bazel harness (Task 43)

test-journals/
├── run_all_ice_tests.sh       # ICE test runner (Task 48)
└── ice/
    ├── ice_e2e_brent_basic.journal
    ├── ice_e2e_euribor_gtbpr.journal
    ├── ice_e2e_smp.journal
    ├── ice_e2e_ipl_hold.journal
    ├── ice_e2e_session_lifecycle.journal
    ├── ice_e2e_iceberg.journal
    ├── ice_e2e_multi_product.journal
    ├── ice_e2e_mass_cancel.journal
    ├── ice_e2e_gtbpr_priority.journal    # Task 50
    ├── ice_e2e_gtbpr_prorata.journal     # Task 50
    ├── ice_e2e_gtbpr_boundaries.journal  # Task 50
    └── ice_e2e_gtbpr_vs_fifo.journal     # Task 50
```

---

## 10. Success Criteria

| Criterion | Verification |
|---|---|
| GTBPR: priority order fills before pro-rata | Task 28 unit tests |
| GTBPR: time-weighted allocation favors older orders | Task 28 unit tests |
| FIFO products use strict price-time priority | Task 29 unit tests |
| IPL hold triggered when order exceeds band | Task 29 unit tests |
| IPL resumes after hold period with new band | Task 29 unit tests |
| VWAP settlement calculated from trades in window | Task 29 unit tests |
| SMP RTO: resting order cancelled, aggressor proceeds | Task 45 ICE journals |
| SMP ATO: aggressor cancelled, resting stays | Task 45 ICE journals |
| SMP CABO: both cancelled | Task 45 ICE journals |
| Pre-open: Market, IOC, FOK rejected | Task 45 ICE journals |
| Iceberg: only display qty shown in iMpact; next tranche at queue back | Task 45 ICE journals |
| FIX round-trip: NewOrderSingle → ExecReport correct | Task 45 ICE journals |
| iMpact round-trip: encode → decode = original | Task 40 round-trip tests |
| Every order add produces ICE_MD_ADD | Task 45 ICE journals |
| Every fill produces ICE_MD_TRADE + ICE_EXEC_FILL | Task 45 ICE journals |
| Multi-product: Brent and Euribor fully isolated | Task 45 ICE journals |
| GTBPR: priority fill through full engine lifecycle | Task 49 integration tests |
| GTBPR: collar boundary at exact value (eligible) and collar-1 (ineligible) | Task 49 integration tests |
| GTBPR: cap exhaustion — priority fill exactly capped | Task 49 integration tests |
| GTBPR: FIFO vs GTBPR same orders → different allocations | Task 49 integration tests + Task 50 E2E journals |
| GTBPR: time-weighted pro-rata verified through full FIX→engine→iMpact pipeline | Task 50 GTBPR E2E journals |
| GTBPR: collar/cap boundary conditions verified E2E | Task 50 GTBPR E2E journals |
| ICE test runner: single script runs all ICE tests with summary | Task 48 test runner script |

---

## 11. Open Questions / Decisions Required

1. **GTBPR parameters**: Collar, cap, and time weight factor values are not publicly documented. For the simulator, these should be configurable per product. We will use placeholder values (collar=5 lots, cap=200 lots, time_weight_factor=0.1) for testing. Confirm with team if different values are needed.

2. **SMP ID implementation**: The current `Order` struct uses `account_id` for SMP. To support ICE's multi-tag SMP (up to 8 IDs per firm), we need to add an `smp_id` field to `Order` or `OrderRequest`. This is a **core change** that also affects the CME implementation (CME uses account_id today — that would remain unchanged). Decision: add `smp_id` as an optional field to `OrderRequest` (0 = no SMP), map to `Order.account_id` for existing CME behavior.

3. **BOE vs FIX for Phase 5**: Plan proposes FIX 4.2 first (Task 32-35). BOE spec is not publicly available (live Feb 2026). If BOE spec becomes available, add Phase 5H for BOE codec as an extension. No action needed now.

4. **Implied matching**: ICE implied IN/OUT for energy strips is descoped for v1. If needed later, it would be a separate `IceImpliedEngine` component alongside `IceSimulator`.

5. **iMpact specification access**: The official ICE iMpact Multicast Feed Message Specification (v1.1.50) is only available to ICE Technology ISV partners. The codec implementation (Tasks 36-40) should be based on the publicly observable binary format from Databento's IFEU.IMPACT dataset and vendor handler documentation. Flag if direct ICE spec access is available.
