# KRX Gap Closure Plan

**Date:** 2026-03-29
**Goal:** Bring KRX feature parity with CME and ICE across four areas: secdef, snapshot recovery, spread trading, and E2E journal coverage.

## Current State

KRX has a working simulator (`krx_sim_runner.cc`) with:
- FIX 4.4 order entry over TCP
- FAST 1.1 market data over UDP multicast (Quote, Trade, Status, Snapshot templates)
- 10 derivative products (futures, options, FX, bonds) in `krx_products.h`
- Config already has `secdef_group/port` and `snapshot_group/port` fields -- but **nothing uses them yet**
- `exchange_observer.cc` supports `--exchange cme` and `--exchange ice` but **not `--exchange krx`**
- 4 E2E journals vs 9 (CME) and 12 (ICE)

## Architecture Overview

```
                    KRX Sim Runner
                    +-----------+
                    |           |
    FIX 4.4 TCP    |  KrxSim   |   FAST 1.1 UDP
    <------------->|  (engine)  |---------------> incremental feed
                    |           |       |
                    +-----------+       |
                         |              +-------> secdef channel (new)
                         |              +-------> snapshot channel (new)
                         |
                    SpreadSimulator (new)
                         |
                    krx_spreads.h (new)
```

```
                    Exchange Observer
                    +----------------+
                    |                |
    secdef mcast -->| KrxSecdef      |  (new)
                    | Consumer       |
                    |                |
    snap mcast ---->| KrxRecovery    |  (new)
                    |                |
    incr mcast ---->| KrxVisitor     |  (new)
                    |                |
                    +----------------+
```

## Gap 1: KRX Secdef Channel

**What:** Publish FAST-encoded instrument definitions on a dedicated multicast channel. Build a consumer for `exchange_observer.cc`.

**Why:** CME publishes MDP3 secdef on `secdef_group:secdef_port` every 30s. ICE publishes iMpact InstrumentDefinition on the main channel. KRX config already has `secdef_group` (224.0.33.2) and `secdef_port` (16001) but the sim runner doesn't publish anything on them.

**Pattern to follow:** `cme_sim_runner.cc:70-82` (publish_secdefs) + `tools/cme_secdef.h` (CmeSecdefConsumer)

### Design Decisions

**New FAST template for secdef (TemplateId::InstrumentDef = 5)**

Pros: Reuses existing FAST codec infrastructure; consistent with other KRX FAST messages.
Cons: Adds another template to the codec (minor).
**Selected:** This approach keeps the KRX wire format uniform (FAST everywhere).

Alternative: Use a raw binary struct like ICE's InstrumentDefinition.
Rejected: ICE uses a fixed-layout binary protocol (iMpact); KRX already uses FAST. Mixing formats would add complexity.

### Tasks

| # | Task | Files | Est. Lines | Deps |
|---|------|-------|-----------|------|
| 1.1 | Add `FastInstrumentDef` struct + `TemplateId::InstrumentDef = 5` to `fast_types.h` | `krx/fast/fast_types.h` | ~30 | — |
| 1.2 | Add `encode_instrument_def()` to `fast_encoder.h` + unit test | `krx/fast/fast_encoder.h`, `krx/fast/fast_encoder_test.cc` | ~80 | 1.1 |
| 1.3 | Add `decode_instrument_def()` to `fast_decoder.h` + unit test | `krx/fast/fast_decoder.h`, `krx/fast/fast_decoder_test.cc` | ~80 | 1.1 |
| 1.4 | Wire secdef publishing into `krx_sim_runner.cc`: `publish_secdefs()` + periodic re-publish (30s timer) | `krx/krx_sim_runner.cc` | ~50 | 1.2 |
| 1.5 | Create `tools/krx_secdef.h` (KrxSecdefConsumer extends SecdefConsumer) | `tools/krx_secdef.h` | ~120 | 1.3 |
| 1.6 | Wire KRX into `exchange_observer.cc`: add `--exchange krx`, `resolve_krx_instrument_id()`, secdef consumer, KrxVisitor | `tools/exchange_observer.cc` | ~180 | 1.5 |

Task 1.6 is close to the 200-line target. The KrxVisitor is structurally similar to CmeVisitor/IceVisitor but uses the FAST decoder's visitor pattern. If it runs long, split the KrxVisitor into its own header.

---

## Gap 2: KRX Snapshot Recovery

**What:** Publish periodic full-book snapshots on a dedicated multicast channel. Build a recovery consumer for `exchange_observer.cc`.

**Why:** CME publishes MDP3 SnapshotFullRefreshOrderBook53 on `snapshot_group:snapshot_port` every 5s. ICE uses TCP-based snapshot server. KRX config has `snapshot_group` (224.0.33.3) and `snapshot_port` (16002) but nothing publishes there.

**Pattern to follow:** `cme_sim_runner.cc:237-267` (snapshot publish loop) + `tools/cme_recovery.h` (CmeRecovery). KRX will use multicast snapshots (like CME), not TCP (like ICE).

### Design Decisions

**Multicast snapshot (like CME) vs TCP snapshot (like ICE)**

Pros (multicast): Consistent with the existing KRX multicast-only architecture; simpler sim runner (no TCP snapshot server to manage); `krx_sim_config.h` already has multicast group/port for snapshots.
Cons (multicast): Late joiners must wait up to 5s for a snapshot cycle.
**Selected:** Multicast. KRX already uses multicast for everything; adding a TCP snapshot server would be an unnecessary architectural divergence.

**Reuse existing FastSnapshot template (ID=4) vs new FullBookSnapshot template**

The existing `FastSnapshot` (template 4) only encodes top-of-book (bid/ask price+qty). For proper recovery, we need all depth levels plus per-level order count and a sequence number for gap detection.
**Selected:** New `FastFullSnapshot` struct and `TemplateId::FullSnapshot = 6`. The existing `FastSnapshot` (template 4) stays unchanged for backward compat with the incremental feed's periodic TOB updates.

### Tasks

| # | Task | Files | Est. Lines | Deps |
|---|------|-------|-----------|------|
| 2.1 | Add `FastFullSnapshot` struct + `TemplateId::FullSnapshot = 6` + `FastSnapshotLevel` to `fast_types.h` | `krx/fast/fast_types.h` | ~35 | — |
| 2.2 | Add `encode_full_snapshot()` + `decode_full_snapshot()` to encoder/decoder + tests | `krx/fast/fast_encoder.h`, `krx/fast/fast_decoder.h`, `krx/fast/fast_codec_test.cc` | ~150 | 2.1 |
| 2.3 | Wire snapshot publishing into `krx_sim_runner.cc`: iterate all levels per product, encode, multicast on snapshot channel every 5s | `krx/krx_sim_runner.cc` | ~60 | 2.2 |
| 2.4 | Create `tools/krx_recovery.h` (KrxRecovery extends RecoveryStrategy): join snapshot multicast, decode FastFullSnapshot, populate DisplayState | `tools/krx_recovery.h` | ~120 | 2.2 |
| 2.5 | Wire KRX recovery + gap detection into `exchange_observer.cc` | `tools/exchange_observer.cc` | ~40 | 2.4, 1.6 |

---

## Gap 3: KRX Spread Trading

**What:** Define KRX-specific spread instruments and register them in the KrxSimulator via SpreadSimulator.

**Why:** CME has 8 spread definitions across 4 types (calendar, butterfly, condor, inter-commodity). ICE has 4 spread definitions (calendar, crack). KRX has zero.

**Pattern to follow:** `cme/cme_spreads.h` (definitions) + `exchange-sim/spread_book/spread_simulator.h` (registration)

### Real KRX Spread Products

1. **KS-CAL (KOSPI200 Calendar Spread)**: Buy front-month KS, sell back-month KS. The most traded KRX spread.
2. **KTB-CAL (KTB 3Y Calendar Spread)**: Buy front-month KTB, sell back-month KTB.
3. **KS-BF (KOSPI200 Butterfly)**: +1 near KS, -2 mid KS, +1 far KS.
4. **KS-MKS (KOSPI200/Mini inter-product)**: Buy KS, sell MKS with 5x ratio (1 KS = 5 Mini).

### Design Decisions

**Spread instrument IDs: 3001+ (avoids collision with CME 1001+ and ICE 2001+)**

Pros: Clean namespace separation across exchanges.
Cons: None -- IDs are per-exchange anyway, but using distinct ranges makes debugging easier.
**Selected:** 3001-3099 calendars, 3101-3199 butterflies, 3201-3299 inter-product.

### Tasks

| # | Task | Files | Est. Lines | Deps |
|---|------|-------|-----------|------|
| 3.1 | Create `krx/krx_spreads.h` with 4 spread definitions (KS-CAL, KTB-CAL, KS-BF, KS-MKS) | `krx/krx_spreads.h` | ~100 | — |
| 3.2 | Unit test for spread definitions (verify tick/lot computation, strategy type) | `krx/krx_spreads_test.cc` | ~80 | 3.1 |
| 3.3 | Wire SpreadSimulator into KrxSimulator: load spreads, route spread orders | `krx/krx_simulator.h` (or new `krx/krx_spread_wiring.h`) | ~60 | 3.1 |
| 3.4 | E2E journal: `krx_e2e_spread_fill.journal` (calendar spread order + fill) | `test-journals/krx/krx_e2e_spread_fill.journal` | ~40 | 3.3 |

---

## Gap 4: Additional KRX E2E Journals

**What:** Expand from 4 to 14-16 journals to match CME (9) and ICE (12) coverage depth.

**Why:** Current coverage: basic lifecycle, sidecar, tiered limits, VI dual session. Missing: spread fills, multi-product isolation, auction lifecycle, mass cancel, replace order, partial fill, reject cases, rate throttle, position limits, GTC persistence.

### Journal List

Existing (4):
1. `krx_e2e_kospi200_basic.journal` -- basic limit order lifecycle
2. `krx_e2e_sidecar.journal` -- sidecar program trading halt
3. `krx_e2e_tiered_limits.journal` -- 3-tier price limit bands
4. `krx_e2e_vi_dual_session.journal` -- volatility interruption across sessions

New (10-12):

| # | Task | Journal File | Tests | Deps |
|---|------|-------------|-------|------|
| 4.1 | Spread fill lifecycle | `krx_e2e_spread_fill.journal` | calendar spread order, implied match, fill report | 3.3 |
| 4.2 | Multi-product isolation | `krx_e2e_multi_product.journal` | KS + KTB orders in parallel, fills don't cross-contaminate | — |
| 4.3 | Auction lifecycle (opening/closing) | `krx_e2e_auction_lifecycle.journal` | opening call auction -> matching -> continuous -> closing call | — |
| 4.4 | Mass cancel | `krx_e2e_mass_cancel.journal` | place 5 orders, mass cancel, verify all cancelled | — |
| 4.5 | Replace order (modify price/qty) | `krx_e2e_replace_order.journal` | place order, modify price, verify book update, modify qty | — |
| 4.6 | Partial fill + residual management | `krx_e2e_partial_fill.journal` | 3-contract buy, 1-contract sell x2, verify residual cancel | — |
| 4.7 | Reject cases | `krx_e2e_rejects.journal` | invalid price (not on tick), exceeds max order size, invalid side | — |
| 4.8 | Rate throttle | `krx_e2e_rate_throttle.journal` | burst of orders exceeding rate limit, verify throttle reject | — |
| 4.9 | Position limit | `krx_e2e_position_limit.journal` | accumulate to position limit, verify next order rejected | — |
| 4.10 | GTC order persistence across sessions | `krx_e2e_gtc_persistence.journal` | place GTC in regular, close session, open after-hours, verify GTC survives | — |
| 4.11 | Dynamic price band rejection | `krx_e2e_dynamic_band.journal` | trade at X, place order outside dynamic band, verify reject | — |
| 4.12 | Iceberg/reserve order | `krx_e2e_iceberg.journal` | place iceberg order, verify only display qty visible, partial fill replenish | — |

---

## Task Summary and Dev Dispatch

**Total tasks:** 24
**Estimated total lines:** ~1,200 (excluding journals which are documentation-like)

### Dependency Graph

```
Gap 1 (Secdef):       1.1 --> 1.2 --> 1.4
                       1.1 --> 1.3 --> 1.5 --> 1.6
                                                 |
Gap 2 (Snapshot):     2.1 --> 2.2 --> 2.3        |
                              2.2 --> 2.4 ------> 2.5

Gap 3 (Spreads):      3.1 --> 3.2
                       3.1 --> 3.3 --> 3.4

Gap 4 (Journals):     4.1 depends on 3.3
                       4.2-4.12 are independent of Gaps 1-3
```

### Dev Assignment (3 devs)

| Dev | Tasks | Rationale |
|-----|-------|-----------|
| **Dev A** | 1.1, 1.2, 1.3, 1.4, 2.1, 2.2, 2.3 | FAST codec expert. All encoder/decoder work + sim runner wiring. Sequential chain within each gap but can interleave 1.x and 2.x commits. |
| **Dev B** | 1.5, 1.6, 2.4, 2.5, 3.1, 3.2, 3.3, 3.4 | Tools + observer integration + spread definitions. Blocked on Dev A finishing 1.3 (for secdef consumer) and 2.2 (for recovery). Can start 3.x immediately in parallel. |
| **Dev C** | 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 4.10, 4.11, 4.12 | Journal author. All journals except 4.1 (which depends on spread wiring from Dev B). Can start immediately -- no code dependencies for journaling existing features. |

### Execution Order

**Phase 1 (parallel start):**
- Dev A: 1.1 -> 1.2 -> 1.3
- Dev B: 3.1 -> 3.2 -> 3.3
- Dev C: 4.2, 4.3, 4.4, 4.5 (independent journals)

**Phase 2 (after Phase 1):**
- Dev A: 1.4 -> 2.1 -> 2.2 -> 2.3
- Dev B: 1.5 -> 1.6 (blocked on Dev A's 1.3)
- Dev C: 4.6, 4.7, 4.8, 4.9 (more journals)

**Phase 3 (after Phase 2):**
- Dev A: done (or review support)
- Dev B: 2.4 -> 2.5 (blocked on Dev A's 2.2), then 3.4
- Dev C: 4.1 (blocked on Dev B's 3.3), 4.10, 4.11, 4.12

### Key Interfaces

**FastInstrumentDef (new FAST template)**
```
struct FastInstrumentDef {
    uint32_t instrument_id;     // unique ID from krx_products.h
    char     symbol[8];         // null-padded (e.g. "KS\0\0\0\0\0\0")
    char     description[32];   // null-padded
    uint8_t  product_group;     // KrxProductGroup as uint8_t
    int64_t  tick_size;         // engine fixed-point
    int64_t  lot_size;          // engine fixed-point
    int64_t  max_order_size;    // engine fixed-point
    uint32_t total_instruments; // total count (for completion detection)
    int64_t  timestamp;
};
```

**FastFullSnapshot (new FAST template)**
```
struct FastSnapshotLevel {
    int64_t  price;
    int64_t  quantity;
    uint32_t order_count;
};

struct FastFullSnapshot {
    uint32_t instrument_id;
    uint32_t seq_num;           // for gap detection
    uint8_t  num_bid_levels;
    uint8_t  num_ask_levels;
    FastSnapshotLevel bids[BOOK_DEPTH];  // BOOK_DEPTH = 5
    FastSnapshotLevel asks[BOOK_DEPTH];
    int64_t  timestamp;
};
```

**KrxSecdefConsumer** -- same interface as CmeSecdefConsumer/IceSecdefConsumer:
```
class KrxSecdefConsumer : public SecdefConsumer {
    discover(timeout_secs) -> map<string, InstrumentInfo>
};
```

**KrxRecovery** -- same interface as CmeRecovery:
```
class KrxRecovery : public RecoveryStrategy {
    recover(instrument, DisplayState&) -> void
};
```
