# Implied/Spread Trading Implementation Plan

**Date:** 2026-03-28
**Status:** Draft (revised after review)
**Scope:** SpreadBook abstraction, implied pricing, atomic multi-leg matching
**Estimate:** ~3,500 lines across 25 tasks in 8 phases
**Design invariant:** Single-threaded execution assumed throughout. SpreadBook,
implied price engine, and all MatchingEngine interactions run on one thread with
no concurrent access. This eliminates lock contention and simplifies atomicity
guarantees. If multi-threading is ever introduced, a full concurrency audit is
required.

## Problem

The exchange currently supports only outright (single-leg) instruments. Production
exchanges (CME, ICE) derive implied prices from spread relationships -- a calendar
spread bid at -5 combined with a front-month offer at 100 implies a back-month offer
at 105. Without implied pricing, the simulator cannot model real exchange behavior for
spread-heavy markets (interest rates, energy).

## Decision: SpreadBook Abstraction (Option C)

Minimal core invasion (two small engine changes), production-accurate architecture,
testable in isolation. See git history for full options analysis.

## Architecture

```
                         ExchangeSimulator
                         +--------------------------------------------------+
                         |                                                  |
  Order in  ------------>|  route_order(instrument_id, req)                 |
                         |    |                                             |
                         |    v                                             |
                         |  MatchingEngine[ES_front]  MatchingEngine[ES_back]
                         |    |  OrderBook (bids/asks)   |  OrderBook       |
                         |    |          \               |        /         |
                         |    |           v              v       v          |
                         |    |         SpreadBook[ES_CAL]                  |
                         |    |           |  spread OrderBook               |
                         |    |           |  strategy definitions            |
                         |    |           |  implied price engine            |
                         |    |           |                                 |
                         |    |           +---> apply_implied_fills()       |
                         |    |                   on leg engines             |
                         +--------------------------------------------------+

  Implied-out flow:                    Implied-in flow:
  1. Outright fill/BBO change          1. Spread order arrives
  2. SpreadBook recalculates           2. SpreadBook checks outright BBOs
     implied spread prices             3. If legs cross implied spread:
  3. If implied spread crosses:           apply_implied_fills(batch)
     execute spread match              4. Fill spread order
  4. apply_implied_fills(batch)
     on each outright engine

  Circular dependency handling:
  A->B + B->C + C->A can create infinite loops.
  Solution: depth-limited propagation (max_depth=3, configurable).
```

### Core Engine Changes (TWO methods)

**1. Batch implied fill (replaces single apply_implied_fill):**

```cpp
// Apply implied fills atomically: validate ALL legs, then apply ALL or NONE.
// LegFill = {OrderId, Price, Quantity, Side}.
// Returns false and applies nothing if any leg fails validation.
bool apply_implied_fills(std::span<const LegFill> fills, Timestamp ts);
```

Semantics: iterate fills, validate each (order exists, sufficient remaining qty,
correct side). If any validation fails, return false with zero side effects. On
success, apply all fills, emit events, update book levels. ~80 lines.

**2. Best-order accessor (~5 lines):**

```cpp
// Returns OrderId of head-of-queue order at best price level for given side.
// Returns nullopt if side is empty.
std::optional<OrderId> best_order_id(Side side) const;
```

SpreadBook needs this to identify which outright orders to include in implied
fills without reaching into OrderBook internals.

### Spread Events

New event types for implied fill attribution:
- `SpreadFill` or `implied` flag on existing `Trade`/`OrderFilled` events
- `ImpliedTopOfBook` update to market data listeners
- `ImpliedDepthUpdate` to market data listeners

### Non-Unity Leg Ratios

`SpreadStrategy` must include per-leg ratios. Two engine tiers:
- **2-leg simple:** direct price offset, ratio = 1:1 (calendar, intercommodity)
- **N-leg complex:** weighted combination (butterfly 1:-2:1, condor 1:-1:-1:1)

Implied price engine handles tick size normalization (leg tick != spread tick)
and lot size GCD calculation (legs with different lot sizes).

### SpreadBook Components

All in `exchange-sim/spread_book/`:

| File | Purpose | ~Lines |
|---|---|---|
| `spread_strategy.h` | Strategy def (legs, ratios, tick normalization, lot GCD) | 120 |
| `spread_instrument_config.h` | Spread instrument secdef + registration | 80 |
| `implied_price_engine.h` | 2-leg simple + N-leg complex implied pricing | 200 |
| `spread_book.h/.cc` | Spread order mgmt + matching + TIF handling | 600 |
| `circular_guard.h` | Depth-limited propagation guard | 40 |

## Task Breakdown

| # | Task | Phase | Dep | Component | ~Lines |
|---|---|---|---|---|---|
| 1 | `SpreadStrategy`: legs, ratios, tick normalization, lot GCD | P1 | -- | exchange-sim | 120 |
| 2 | `SpreadStrategyRegistry`: register/lookup by spread ID | P1 | 1 | exchange-sim | 100 |
| 3 | `SpreadInstrumentConfig`: spread secdef + registration in simulator | P1 | 1,2 | exchange-sim | 80 |
| 4 | Unit tests: strategy defs, registry, secdef | P1 | 1-3 | test | 150 |
| 5 | `ImpliedPriceEngine` (2-leg simple): price offset from outright BBOs | P2 | 1 | exchange-sim | 120 |
| 6 | `ImpliedPriceEngine` (N-leg complex): weighted combination, tick/lot normalization | P2 | 5 | exchange-sim | 80 |
| 7 | `CircularGuard`: depth counter for propagation loops | P2 | -- | exchange-sim | 40 |
| 8 | Unit tests: implied price calc (2-leg + N-leg) + circular guard | P2 | 5-7 | test | 180 |
| 9 | `apply_implied_fills(span<LegFill>)` batch method on MatchingEngine | P3 | -- | exchange-core | 80 |
| 10 | `best_order_id(Side)` accessor on MatchingEngine | P3 | -- | exchange-core | 5 |
| 11 | Unit tests: batch implied fills (all-or-nothing) + best_order_id | P3 | 9,10 | test | 200 |
| 12 | `SpreadBook` core: spread-side OrderBook, order entry/cancel | P4 | 1-3 | exchange-sim | 250 |
| 13 | Spread order TIF handling: IOC (partial OK) / FOK (all-or-nothing) | P4 | 12 | exchange-sim | 80 |
| 14 | Spread order modification: cancel-replace | P4 | 12 | exchange-sim | 60 |
| 15 | Implied-out matching: outright BBO change triggers spread match | P4 | 5,12 | exchange-sim | 150 |
| 16 | Implied-in matching: spread order triggers outright implied fills | P4 | 5,9,10,12 | exchange-sim | 150 |
| 17 | Atomic multi-leg fill coordinator (zero-or-all via batch method) | P4 | 15,16 | exchange-sim | 80 |
| 18 | Spread event attribution: SpreadFill, ImpliedTopOfBook, ImpliedDepthUpdate | P4 | 15,16 | exchange-sim | 100 |
| 19 | Unit tests: SpreadBook (implied-out, implied-in, TIF, cancel-replace, events) | P4 | 12-18 | test | 300 |
| 20 | `ExchangeSimulator` integration: SpreadBook lifecycle, routing | P5 | 12,17 | exchange-sim | 120 |
| 21 | Integration tests: multi-instrument spread scenarios | P5 | 20 | test | 200 |
| 22 | CME spread types: calendar, butterfly, condor, crack | P6 | 2,20 | cme | 100 |
| 23 | ICE spread types: calendar, strip, crack | P6 | 2,20 | ice | 80 |
| 24 | E2E journal tests: spread fill journal invariants | P7 | 21-23 | test | 150 |
| 25 | Failure injection: partial fill rollback, circular loops, stale BBO, TIF edge cases | P7 | 19,21 | test | 175 |
| | | | | **Total** | **~3,450** |

## Dependency DAG

```
Phase 1 (Strategy + Secdef)    Phase 2 (Pricing)        Phase 3 (Engine)
  T1 ----> T2 ----> T3           T5 ----> T6              T9  ---+---> T11
  T1 ----> T3                    T7 ------+---> T8         T10 --+
  T1..T3 ----> T4                T5,T6 ---+
       |                              |
       v                              v
Phase 4 (SpreadBook Core)
  T12 <-- T1,T2,T3
  T13 <-- T12                    (TIF handling)
  T14 <-- T12                    (cancel-replace)
  T15 <-- T5,T12                 (implied-out)
  T16 <-- T5,T9,T10,T12         (implied-in)
  T17 <-- T15,T16               (atomic coordinator)
  T18 <-- T15,T16               (events)
  T19 <-- T12..T18               (SpreadBook tests)
       |
       v
Phase 5 (Integration)          Phase 6 (Exchange-Specific)
  T20 <-- T12,T17                 T22 <-- T2,T20  (CME)
  T21 <-- T20                     T23 <-- T2,T20  (ICE)
       |                               |
       v                               v
Phase 7 (Validation)
  T24 <-- T21,T22,T23
  T25 <-- T19,T21
```

**Parallelism:** Phases 1, 2, and 3 are independent -- all three can run
concurrently. Within Phase 4, T13/T14 and T15/T16 can proceed in parallel
once T12 is done. Phase 6 tasks (T22, T23) are independent of each other.

## Dev Dispatch

| Wave | Tasks | Assignee | Rationale |
|---|---|---|---|
| 1 | T1, T2, T3, T4 | Dev A | Strategy + secdef foundation -- sequential |
| 1 | T5, T6, T7, T8 | Dev B | Pricing engine -- independent of strategy impl |
| 1 | T9, T10, T11 | Dev C | Core engine changes -- zero external deps |
| 2 | T12, T13, T14, T15, T16, T17, T18, T19 | Dev A | SpreadBook -- sequential, one dev avoids conflicts |
| 3 | T20, T21 | Dev A | Integration -- depends on SpreadBook |
| 3 | T22 | Dev B | CME spreads -- independent |
| 3 | T23 | Dev C | ICE spreads -- independent |
| 4 | T24, T25 | Dev B | Validation + failure injection |

**Estimated waves:** 4 waves with 3 devs, ~12-14 working sessions.

## Success Criteria

1. `bazel test //...` passes with all 25 tasks merged
2. `apply_implied_fills()` and `best_order_id()` are the ONLY changes to `exchange-core/`
3. Implied-out: outright order at 100 + calendar spread at -5 produces implied back-month at 105
4. Implied-in: spread order triggers atomic fills on both outright legs
5. Batch atomicity: `apply_implied_fills()` applies zero fills if any leg fails validation
6. Non-unity ratios: butterfly (1:-2:1) implied prices computed correctly
7. Circular dependency: 3-way circular spread resolves within depth limit
8. Spread TIF: IOC partially fills, FOK rejects if any leg insufficient
9. Spread cancel-replace modifies price/qty atomically
10. SpreadFill events carry implied attribution; ImpliedTopOfBook published to listeners
11. Journal reconciler validates spread fills
12. CME calendar/butterfly/condor/crack + ICE calendar/strip/crack tested
