# Implied/Spread Trading Implementation Plan

**Date:** 2026-03-28
**Status:** Draft
**Scope:** SpreadBook abstraction, implied pricing, atomic multi-leg matching
**Estimate:** ~2,680 lines across 20 tasks in 8 phases

## Problem

The exchange currently supports only outright (single-leg) instruments. Production
exchanges (CME, ICE) derive implied prices from spread relationships -- a calendar
spread bid at -5 combined with a front-month offer at 100 implies a back-month offer
at 105. Without implied pricing, the simulator cannot model real exchange behavior for
spread-heavy markets (interest rates, energy).

## Options Analysis

### Option A: Inline in MatchingEngine

Embed spread logic directly into `match_continuous()`.

| Pros | Cons |
|---|---|
| No new components | Violates SRP; engine grows 500+ lines |
| Direct book access | Every outright instrument must know about every spread |
| | Cannot disable spreads without #ifdef |
| | Testing becomes combinatorial nightmare |

### Option B: External Spread Coordinator (pub/sub)

MatchingEngine publishes BBO updates; external coordinator subscribes, computes
implied prices, sends synthetic orders back.

| Pros | Cons |
|---|---|
| Zero engine changes | Race conditions between BBO publish and synthetic order |
| Clean separation | Double-matching latency (event round-trip) |
| | Cannot guarantee atomicity of multi-leg fills |
| | Synthetic order ID management is fragile |

### Option C: SpreadBook Abstraction (Recommended)

New `SpreadBook` component in `exchange-sim/` that sits alongside `MatchingEngine`
instances. It holds references to outright `OrderBook`s, manages its own spread
`OrderBook`, and coordinates atomic multi-leg fills via a single new engine method:
`apply_implied_fill()` (~60 lines).

| Pros | Cons |
|---|---|
| ONE core change (60 lines) | New component to maintain |
| Atomic multi-leg guarantee | Requires OrderBook read access |
| Clean enable/disable | Slightly more indirection than Option A |
| Testable in isolation | |
| Matches CME/ICE production architecture | |

**Decision: Option C.** Minimal core invasion, production-accurate architecture,
testable in isolation.

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
                         |    |           \              |       /          |
                         |    |            v             v      v           |
                         |    |         SpreadBook[ES_CAL]                  |
                         |    |           |  spread OrderBook               |
                         |    |           |  strategy definitions            |
                         |    |           |  implied price engine            |
                         |    |           |                                 |
                         |    |           +---> apply_implied_fill()        |
                         |    |                   on leg engines             |
                         |    |                                             |
                         +--------------------------------------------------+

  Data flow (implied-out):         Data flow (implied-in):
  ~~~~~~~~~~~~~~~~~~~~~~~~~~       ~~~~~~~~~~~~~~~~~~~~~~~~
  1. Outright fill/BBO change      1. Spread order arrives
  2. SpreadBook recalculates       2. SpreadBook checks outright BBOs
     implied spread prices         3. If legs cross implied spread:
  3. If implied spread crosses:       apply_implied_fill() on each
     execute spread match              outright engine
  4. apply_implied_fill() on       4. Fill spread order
     each outright engine

  Circular dependency handling:
  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  A->B spread + B->C spread + C->A spread can create infinite loops.
  Solution: depth-limited propagation (max_depth=3, configurable).
  Each implied price recalc carries a depth counter; propagation
  stops when depth reaches limit.
```

### Core Engine Change

Single new method on `MatchingEngine` (~60 lines):

```cpp
// Apply an implied fill to an outright order at the given price/qty.
// Called by SpreadBook after it determines a multi-leg match is valid.
// Bypasses normal order flow -- directly reduces resting qty and emits
// fill events. Returns false if order no longer exists or has
// insufficient remaining qty.
bool apply_implied_fill(OrderId order_id, Price fill_price,
                        Quantity fill_qty, Timestamp ts);
```

This method: validates order exists, reduces remaining qty, emits
`OrderFilled`/`Trade` events, updates book levels, triggers stop checks.
No validation (price bands, session state) -- the SpreadBook is trusted.

### SpreadBook Components

All in `exchange-sim/spread_book/`:

| File | Purpose | ~Lines |
|---|---|---|
| `spread_strategy.h` | Strategy definition (legs, ratios, price formula) | 80 |
| `implied_price_engine.h` | Compute implied prices from outright BBOs | 150 |
| `spread_book.h` | Spread order management + matching coordination | 350 |
| `spread_book.cc` | Implementation | 200 |
| `circular_guard.h` | Depth-limited propagation guard | 40 |

## Task Breakdown

| # | Task | Phase | Dep | Component | ~Lines |
|---|---|---|---|---|---|
| 1 | `SpreadStrategy` struct: legs, ratios, implied price formula | P1 | -- | exchange-sim | 80 |
| 2 | `SpreadStrategyRegistry`: register/lookup strategies by spread ID | P1 | 1 | exchange-sim | 100 |
| 3 | Unit tests for strategy definitions and registry | P1 | 1,2 | test | 120 |
| 4 | `ImpliedPriceEngine`: compute implied bid/ask from outright BBOs | P2 | 1 | exchange-sim | 150 |
| 5 | `CircularGuard`: depth counter for propagation loops | P2 | -- | exchange-sim | 40 |
| 6 | Unit tests for implied price calc + circular guard | P2 | 4,5 | test | 150 |
| 7 | `apply_implied_fill()` on MatchingEngine | P3 | -- | exchange-core | 60 |
| 8 | Unit tests for `apply_implied_fill()` | P3 | 7 | test | 180 |
| 9 | `SpreadBook` core: spread-side OrderBook, order entry/cancel | P4 | 1,2 | exchange-sim | 250 |
| 10 | Implied-out matching: outright BBO change triggers spread match | P4 | 4,9 | exchange-sim | 150 |
| 11 | Implied-in matching: spread order triggers outright implied fills | P4 | 4,7,9 | exchange-sim | 150 |
| 12 | Atomic multi-leg fill coordinator (all-or-nothing) | P4 | 10,11 | exchange-sim | 100 |
| 13 | Unit tests for SpreadBook (implied-out, implied-in, atomic fills) | P4 | 9-12 | test | 250 |
| 14 | `ExchangeSimulator` integration: SpreadBook lifecycle, routing | P5 | 9,12 | exchange-sim | 120 |
| 15 | Integration tests: multi-instrument spread scenarios | P5 | 14 | test | 200 |
| 16 | CME spread types: calendar, butterfly, condor, crack | P6 | 2,14 | cme | 100 |
| 17 | ICE spread types: calendar, strip, crack | P6 | 2,14 | ice | 80 |
| 18 | E2E journal tests: spread fill journal invariants | P7 | 15-17 | test | 150 |
| 19 | Failure injection: partial fill rollback, circular loops, stale BBO | P7 | 13,15 | test | 150 |
| 20 | Performance benchmark: implied price recalc latency | P8 | 14 | benchmarks | 50 |
| | | | | **Total** | **~2,680** |

## Dependency DAG

```
Phase 1 (Strategy)          Phase 2 (Pricing)       Phase 3 (Engine)
  T1 ----+----> T3            T4 ----> T6             T7 ----> T8
  T2 ----+                    T5 ----> T6
  |      |                    |
  |      |                    |
  v      v                    v
Phase 4 (SpreadBook Core)
  T9 <-- T1,T2
  T10 <-- T4,T9
  T11 <-- T4,T7,T9
  T12 <-- T10,T11
  T13 <-- T9..T12
       |
       v
Phase 5 (Integration)       Phase 6 (Exchange-Specific)
  T14 <-- T9,T12               T16 <-- T2,T14
  T15 <-- T14                  T17 <-- T2,T14
       |                            |
       v                            v
Phase 7 (Validation)         Phase 8 (Perf)
  T18 <-- T15,T16,T17         T20 <-- T14
  T19 <-- T13,T15
```

**Parallelism:** Phases 1, 2, and 3 are independent -- all three can run
concurrently. Phase 6 tasks (T16, T17) are independent of each other.

## Dev Dispatch

| Wave | Tasks | Assignee | Rationale |
|---|---|---|---|
| 1 | T1, T2, T3 | Dev A | Strategy foundation -- sequential chain |
| 1 | T4, T5, T6 | Dev B | Pricing engine -- independent of strategy impl |
| 1 | T7, T8 | Dev C | Core engine change -- zero external deps |
| 2 | T9, T10, T11, T12, T13 | Dev A | SpreadBook core -- sequential chain, one dev avoids rebase conflicts |
| 3 | T14, T15 | Dev A | Integration -- depends on SpreadBook |
| 3 | T16 | Dev B | CME spreads -- independent |
| 3 | T17 | Dev C | ICE spreads -- independent |
| 4 | T18, T19 | Dev B | Validation tests |
| 4 | T20 | Dev C | Performance benchmark |

**Estimated waves:** 4 waves with 3 devs, ~10-12 working sessions.

## Success Criteria

1. `bazel test //...` passes with all 20 tasks merged
2. `apply_implied_fill()` is the ONLY change to `exchange-core/matching_engine.h`
3. Implied-out: outright order at 100 + calendar spread at -5 produces implied back-month at 105
4. Implied-in: spread order triggers atomic fills on both outright legs
5. Circular dependency: 3-way circular spread resolves within depth limit without infinite loop
6. Atomic guarantee: partial leg failure rolls back all fills (no torn state)
7. Journal reconciler validates spread fills (trade matching, book traceability)
8. Implied price recalc latency < 1us (single spread, warm cache)
9. CME calendar/butterfly/condor/crack spread types registered and tested
10. ICE calendar/strip/crack spread types registered and tested
