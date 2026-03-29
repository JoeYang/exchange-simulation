# Master Plan: Exchange Matching Engine

> Purpose: Reproduce this project from scratch using lessons learned.
> This plan is a repeatable playbook for building multi-exchange matching
> engine simulators using Claude Code agent teams.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Phase 0: Harness & Governance](#2-phase-0-harness--governance)
3. [Phase 1: Research Protocol](#3-phase-1-research-protocol)
4. [Phase 2: Exchange Core (TDD)](#4-phase-2-exchange-core-tdd)
5. [Phase 3: Core Verification Gate](#5-phase-3-core-verification-gate)
6. [Phase 4: Tooling & Infrastructure](#6-phase-4-tooling--infrastructure)
7. [Phase 5: Per-Exchange Implementation](#7-phase-5-per-exchange-implementation)
8. [Phase 6: Portable Journal Testing](#8-phase-6-portable-journal-testing)
9. [Agent Team Structure](#9-agent-team-structure)
10. [Token Optimization](#10-token-optimization)
11. [Lessons Learned](#11-lessons-learned)
12. [Feature Inventory](#12-feature-inventory)

---

## 1. Project Overview

### What We Built

A C++ low-latency exchange matching engine simulator supporting 3 exchanges
(CME, ICE, KRX) with full protocol stacks, 8 matching algorithms, implied/spread
trading, and end-to-end tooling.

### Final Stats

| Metric | Value |
|--------|-------|
| C++ source lines | 79,025 |
| Source files | 273 |
| Test targets | 124 |
| Commits | 272 |
| Plans executed | 12 |
| Exchanges | 3 (CME, ICE, KRX) |
| Matching algorithms | 8 |
| Protocol codecs | 6 |
| E2E journal tests | 43 |

### Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    tools/ (16K lines)                     │
│  trader, observer, reconciler, viz, replayers, CLI        │
├──────────┬──────────┬──────────┬────────────────────────┤
│ cme/     │ ice/     │ krx/     │ exchange-sim/ (5K)     │
│ (11K)    │ (10K)    │ (9K)     │ spread_book/           │
│ CmeExch  │ IceExch  │ KrxExch  │ SpreadBook             │
│ SBE      │ FIX+bin  │ FIX+FAST │ ImpliedEngine          │
│ iLink3   │ iMpact   │ FAST     │ SpreadSimulator        │
│ MDP3     │          │          │                        │
├──────────┴──────────┴──────────┴────────────────────────┤
│              exchange-core/ (15K lines)                   │
│  MatchingEngine, OrderBook, Sessions, Auctions,          │
│  SMP, Iceberg, Stops, Bands, Throttling, GTC, LMM       │
├──────────────────────────────────────────────────────────┤
│              test-harness/ + test-journals/ (6K)          │
│  E2E journal runner, action/expect DSL, 43 journals      │
└──────────────────────────────────────────────────────────┘
```

---

## 2. Phase 0: Harness & Governance

> Goal: Set boundaries, rules, and acceptance criteria BEFORE writing code.

### Day 0 Deliverables

```
project-root/
├── CLAUDE.md               ← commit rules, model selection, error recovery
├── .claude/skills/
│   └── exchange-smoke-test.md   ← acceptance criteria as a runnable skill
├── smoke_test_all.sh       ← skeleton with stubs (exit 0 = no exchange yet)
├── setup.sh / build.sh / test.sh
├── MODULE.bazel            ← Bazel 9 workspace
└── docs/
    └── architecture.md     ← target architecture (ASCII diagrams)
```

### CLAUDE.md Rules (non-negotiable)

- Commit size: target <200 lines, hard max 400 lines
- Conventional commits: `feat(scope):`, `fix(scope):`, `test(scope):`
- No direct commits to main — all work in git worktrees
- Smoke test gate: `./smoke_test_all.sh <exchange>` must pass before merge
- Run tests after every individual change — no batching
- Stop after 3 failed attempts, escalate to human
- Run formatters and linters before every commit

### Smoke Test: Define FIRST

The smoke test defines what "done" means for each exchange:

```
Pass criteria (per exchange):
  1. Sim binary starts without crashing
  2. Observer receives STATUS messages (market data flowing)
  3. Trader connects, sends orders, receives acks
  4. Two traders cross — TRADE visible in observer
  5. Observer journal has >0 lines
```

Write the smoke test skeleton on day 0. It will fail (no exchanges yet).
As each phase completes, more checks should pass.

### Agent Team Template

Define once, reuse for every phase:

```
Standard Team:
  lead       — coordinator only, NEVER writes code
  planner    — produces plan, reviewed by researcher
  researcher — reviews plan, suggests alternatives
  dev-1..N   — implement in isolated git worktrees
  reviewer   — strict-code-reviewer after each merge

Rules:
  - No agent writes directly to main
  - All work in worktrees, merged via lead
  - Sequential dependency chains go to ONE dev
  - Smoke test runs after every merge to main
  - One reviewer approval is sufficient to merge
  - Planner has autonomy to reassign based on who finishes first
```

---

## 3. Phase 1: Research Protocol

> Goal: Build a complete feature inventory for each target exchange
> BEFORE writing any implementation code.

### Why This Matters

In our actual build, we retrofitted 20+ features through "gap closure" phases
because initial research was incomplete. The gap closure plan alone (20 tasks)
cost as much as the original CME implementation. Features added late:

- Trade bust + TradeRegistry
- Rate throttle + RateTracker
- GTC persistence
- Daily price limits + LockLimit state
- VI auto-trigger
- LMM priority algorithms (3 variants)
- Pro-rata algorithms (4 variants)
- Position tracking
- apply_implied_fills (core API change for spreads)

Every one of these should have been identified in the research phase.

### Research Checklist (per exchange)

For EACH target exchange, the research agent must produce a document covering:

```
1. MATCHING BEHAVIOR
   □ Primary matching algorithm (FIFO, ProRata, GTBPR, etc.)
   □ Secondary/hybrid algorithms for specific product groups
   □ Market maker priority rules (LMM, top-order, PMM)
   □ Allocation rounding rules
   □ Minimum allocation thresholds

2. ORDER TYPES
   □ Limit, Market, Stop, StopLimit — which are supported?
   □ Iceberg / display qty — min display, reveal rules
   □ Market-to-Limit, Market-if-Touched, OCO, BOC
   □ Trailing stop, bracket orders
   □ Phase-specific restrictions (what's rejected in PreOpen?)

3. TIME IN FORCE
   □ DAY, GTC, IOC, FOK, GTD — which are supported?
   □ ATO, ATC, GFA — auction-specific TIFs
   □ Session-specific TIFs (AM/PM sessions)
   □ GTC persistence across sessions — what gets restored?

4. SESSION LIFECYCLE
   □ Full state machine: PreOpen → Auction → Continuous → Close
   □ Number of sessions per day (regular, extended, night)
   □ Exact transition rules and times
   □ What happens to resting orders at each transition?
   □ Volatility interruption (VI) trigger conditions
   □ VI duration and re-entry conditions
   □ Circuit breaker rules (market-wide vs per-instrument)

5. RISK CONTROLS
   □ Dynamic price bands (percentage or fixed-point? symmetric?)
   □ Daily price limits (how many tiers? what happens at breach?)
   □ Self-match prevention (what fields match? what actions?)
   □ Rate throttling (per-account? per-session? what window?)
   □ Position limits (pre-trade? what granularity?)
   □ Max order size / max notional
   □ Order-to-trade ratio limits
   □ Kill switch / mass cancel mechanisms

6. TRADE MANAGEMENT
   □ Trade bust / adjustment — who can trigger? what's reversed?
   □ Trade reporting format
   □ Settlement price calculation (VWAP, last trade, other)

7. SPREAD / IMPLIED TRADING
   □ Supported spread types (calendar, butterfly, condor, crack)
   □ Implied-in and implied-out — both directions?
   □ Circular propagation rules
   □ Spread TIF restrictions
   □ Spread-specific risk controls
   □ Which products have spreads?

8. PROTOCOLS
   □ Order entry protocol (FIX version, binary, proprietary)
   □ Market data protocol (binary, FIX, FAST, ITCH, etc.)
   □ Security definition channel
   □ Snapshot/recovery mechanism
   □ Sequence number gap handling

9. PRODUCTS
   □ Full product list with: symbol, tick size, lot size, max order size
   □ Product groups and their matching algorithm assignments
   □ Contract months / expiry conventions
   □ Settlement type (cash vs physical)
```

### Research Output Format

Each exchange gets a file: `docs/research/<exchange>-analysis.md`

The research document must include:
- Feature inventory (complete checklist above)
- Comparison with exchange-core capabilities
- Gap analysis: what's missing in core that this exchange needs
- Unique features not seen in other exchanges
- Protocol specification references

### Research Review Process

```
research agent ──→ produces exchange-analysis.md
                      │
reviewer agent ──→ reviews for completeness
                      │
                   Are all 9 categories covered?
                   Are there ambiguities that need clarification?
                   Does the gap analysis identify ALL core changes needed?
                      │
                   Triage feedback → accept/reject each suggestion
                      │
                   GATE: Research doc approved before any planning
```

### Critical Rule: Core API Freeze

The research phase must identify ALL public methods needed on MatchingEngine.
After Phase 2 (core implementation), the MatchingEngine public API is FROZEN.

If an exchange needs a new core method (like we needed `apply_implied_fills`
and `best_order_id` for spreads), that must be identified HERE, not during
exchange implementation.

---

## 4. Phase 2: Exchange Core (TDD)

> Goal: Build the matching engine by making journal tests pass.
> Write ALL tests first, then implement to make them green.

### Step 1: Define Core Types (1 commit, ~200 lines)

```
exchange-core/
├── types.h              ← Price, Quantity, Side, OrderType, TIF, etc.
├── events.h             ← OrderAccepted, OrderFilled, Trade, etc.
├── listeners.h          ← OrderListenerBase, MarketDataListenerBase
└── BUILD.bazel
```

No logic yet — just the vocabulary that everything else uses.

### Step 2: Write the Journal Test Portfolio FIRST (~30-40 journals)

Before writing MatchingEngine, write journals that SPECIFY every behavior:

```
test-journals/core/
├── 01_single_order.journal         ← one order, verify accepted
├── 02_crossing_orders.journal      ← buy+sell same price, verify fill
├── 03_partial_fill.journal         ← buy 10, sell 3, verify partial
├── 04_price_priority.journal       ← 3 prices, verify best fills first
├── 05_time_priority_fifo.journal   ← same price, verify FIFO order
├── 06_prorata_basic.journal        ← same price, verify proportional
├── 07_prorata_remainder.journal    ← rounding remainder to FIFO
├── 08_iceberg_basic.journal        ← display qty, verify tranche reveal
├── 09_iceberg_back_of_queue.journal
├── 10_stop_trigger.journal         ← stop order triggers on trade
├── 11_stop_cascade.journal         ← chain of stops triggering
├── 12_smp_cancel_newest.journal
├── 13_smp_cancel_oldest.journal
├── 14_smp_cancel_both.journal
├── 15_smp_decrement.journal
├── 16_session_preopen_reject.journal  ← IOC rejected in PreOpen
├── 17_auction_collect.journal         ← orders collected, not matched
├── 18_auction_uncross.journal         ← equilibrium price calculation
├── 19_auction_iceberg.journal
├── 20_band_reject.journal             ← price outside dynamic bands
├── 21_daily_limit_lock.journal        ← hit daily limit → LockLimit
├── 22_vi_trigger.journal              ← VI auto-trigger on price move
├── 23_mass_cancel.journal
├── 24_cancel_replace.journal
├── 25_rate_throttle.journal
├── 26_position_limit.journal
├── 27_gtc_persist.journal             ← survive session boundary
├── 28_trade_bust.journal
├── 29_market_order.journal
├── 30_fok_fill_or_kill.journal
├── 31_ioc_immediate_cancel.journal
├── 32_gtd_expiry.journal
├── 33_max_order_size.journal
├── 34_lmm_priority.journal
├── 35_multi_instrument_isolation.journal
└── 36_implied_fill_atomic.journal     ← batch fill all-or-nothing
```

These journals use a GENERIC action format (protocol-agnostic):

```
# 02_crossing_orders.journal
ACTION NEW_ORDER cl_ord_id=1 side=BUY price=100.00 qty=10 account=1
EXPECT ORDER_ACCEPTED cl_ord_id=1
ACTION NEW_ORDER cl_ord_id=2 side=SELL price=100.00 qty=10 account=2
EXPECT ORDER_ACCEPTED cl_ord_id=2
EXPECT FILL cl_ord_id=1 price=100.00 qty=10
EXPECT FILL cl_ord_id=2 price=100.00 qty=10
EXPECT TRADE price=100.00 qty=10
```

### Step 3: Implement Core to Make Journals Pass

Now implementation is mechanical — make the next journal green:

```
exchange-core/
├── types.h
├── events.h
├── listeners.h
├── object_pool.h         ← pre-allocated slab allocator
├── intrusive_list.h      ← lock-free linked list
├── orderbook.h           ← bid/ask sorted price levels
├── stop_book.h           ← stop order management
├── rate_tracker.h        ← per-account rate limiting
├── position_tracker.h    ← net position tracking
├── trade_registry.h      ← trade bust support
├── ohlcv.h               ← OHLCV accumulator
├── order_persistence.h   ← GTC serialization
├── session_manager.h     ← session state machine (9 states)
├── match_algo.h          ← 8 matching algorithms
├── matching_engine.h     ← CRTP template, the core
├── composite_listener.h  ← fan-out to multiple listeners
└── BUILD.bazel
```

### Step 4: Matching Algorithm Sub-Portfolio

Each algorithm gets its own dedicated test:

```
test-journals/algorithms/
├── fifo_3_orders.journal
├── prorata_equal_size.journal
├── prorata_unequal_size.journal
├── prorata_remainder.journal
├── threshold_prorata_min_alloc.journal
├── allocation_largest_remainder.journal
├── split_fifo_prorata_60_40.journal
├── fifo_lmm_priority.journal
├── fifo_top_lmm_3phase.journal
└── gtbpr_time_weighted.journal
```

### Implementation Order

```
Commit  1: types.h, events.h, listeners.h
Commit  2: object_pool.h, intrusive_list.h
Commit  3: orderbook.h (bid/ask levels)
Commit  4: FifoMatch algorithm
Commit  5: matching_engine.h — new_order + cancel_order (journals 01-05 pass)
Commit  6: session_manager.h (journals 16-17 pass)
Commit  7: auction calculate + execute (journal 18 pass)
Commit  8: iceberg support (journals 08-09 pass)
Commit  9: stop_book.h (journals 10-11 pass)
Commit 10: SMP (journals 12-15 pass)
Commit 11: dynamic bands + daily limits (journals 20-21 pass)
Commit 12: VI trigger (journal 22 pass)
Commit 13: rate_tracker.h (journal 25 pass)
Commit 14: position_tracker.h (journal 26 pass)
Commit 15: GTC persistence (journal 27 pass)
Commit 16: trade bust (journal 28 pass)
Commit 17: ProRataMatch + ThresholdProRata + Allocation + Split (algo journals)
Commit 18: FifoLmmMatch + FifoTopLmmMatch (algo journals)
Commit 19: modify_order (journal 24 pass)
Commit 20: mass_cancel (journal 23 pass)
Commit 21: apply_implied_fills (journal 36 pass)
```

~21 commits, ~200 lines each = ~4,200 lines for core.
Every commit makes new journals pass. No commit breaks existing ones.

---

## 5. Phase 3: Core Verification Gate

> Goal: Prove the core is complete before building anything on top.

### Exit Criteria

```
□ All 36+ core journals pass
□ All 10 algorithm journals pass
□ Every MatchingEngine public method has at least one journal test
□ Every CRTP hook has a test (even if just the default behavior)
□ Failure injection tests:
  □ Invalid price (0, negative, misaligned)
  □ Invalid quantity (0, negative, misaligned)
  □ Order in wrong session state
  □ Pool exhaustion (max orders reached)
  □ Rate limit exceeded
  □ Position limit exceeded
  □ Self-match at every SMP mode
□ Code review by strict-code-reviewer agent
□ MatchingEngine public API is FROZEN from this point forward
```

### API Freeze Checklist

Document every public method. If a future exchange needs something not here,
it must be done through CRTP hooks, NOT by adding to MatchingEngine:

```
Frozen API:
  new_order, cancel_order, modify_order
  trigger_expiry, mass_cancel, mass_cancel_all
  restore_order, bust_trade
  set_session_state, calculate_auction_price, execute_auction
  publish_indicative_price
  apply_implied_fills, for_each_level, best_order_id
```

---

## 6. Phase 4: Tooling & Infrastructure

> Goal: Build the tools that make exchange development fast.
> Tools come BEFORE exchanges.

### Why Tools First

When you start implementing CME, you should IMMEDIATELY be able to:
- Run `smoke_test_all.sh cme` and see WHERE it fails
- Use the observer to watch market data
- Use the trader to inject orders
- Use the reconciler to verify journal correctness

We built tools AFTER CME and ICE. This meant 2 exchanges were tested only
through unit tests. The 5 smoke test bugs (found in Phase 12) prove that
was insufficient.

### Build Order

```
Phase 4a: Test Harness (~300 lines)
  - Generic journal runner (protocol-agnostic action/expect DSL)
  - Journal file parser
  - Test-harness BUILD targets
  Gate: can parse and run core journals

Phase 4b: Network Transport (~400 lines)
  - TcpServer / TcpClient (for FIX order entry)
  - UdpMulticastPublisher / UdpMulticastReceiver (for market data)
  - McastSeqHeader (4-byte sequence prefix)
  - ShmTransport (shared-memory for local dashboard)
  Gate: round-trip tests for TCP and UDP

Phase 4c: Sim Infrastructure (~300 lines)
  - ExchangeSimulator base (multi-instrument engine wrapper)
  - CompositeListener (fan-out)
  - EngineConfig defaults
  Gate: can create simulator, add instruments, run sessions

Phase 4d: Observer Skeleton (~200 lines)
  - exchange-observer binary with --exchange dispatch table
  - DisplayState, TUI renderer
  - --transitions flag, --journal flag
  - Stubs for cme/ice/krx (print "not implemented")
  Gate: binary builds, shows usage

Phase 4e: Trader Skeleton (~200 lines)
  - exchange-trader binary with --exchange dispatch table
  - SimClient (codec-agnostic TCP client)
  - TradingStrategy interface (random-walk, market-maker)
  - Stubs for cme/ice/krx (print "not implemented")
  Gate: binary builds, shows usage

Phase 4f: Reconciler (~200 lines)
  - Journal reconciler (8 invariant checks)
  - Trade matching, ordering, fill consistency
  Gate: can validate a core journal

Phase 4g: Smoke Test Wiring
  - Update smoke_test_all.sh stubs to try starting each sim
  - All tests should show "FAIL — not implemented" (not crash)
  Gate: smoke_test_all.sh runs without errors for all exchanges
```

---

## 7. Phase 5: Per-Exchange Implementation

> Goal: Add one exchange at a time. Each follows the same sprint structure.
> Smoke test must pass after each sprint.

### Sprint Template (repeat for CME, ICE, KRX, ...)

```
Sprint 1: Exchange Research (if not done in Phase 1)
  └─ Produce docs/research/<exchange>-analysis.md
  └─ Identify core CRTP hooks this exchange needs
  └─ GATE: Research doc reviewed and approved

Sprint 2: Exchange Policy + Products (~200 lines, 1 agent)
  └─ <Exchange>Exchange CRTP class (override hooks)
  └─ Product definitions (symbols, ticks, lots)
  └─ Session schedule
  └─ Unit tests for policy overrides
  └─ GATE: All core journals pass with this exchange policy

Sprint 3: Protocol Codecs (~400 lines, 2-3 agents parallel)
  └─ Agent A: Order entry encoder/decoder
  └─ Agent B: Market data encoder/decoder
  └─ Round-trip unit tests for each codec
  └─ GATE: Codec tests pass

Sprint 4: Sim Runner + Gateway (~200 lines, 1 agent)
  └─ Wire codec + engine + publisher + TCP/UDP
  └─ Session lifecycle (auto-transition to Continuous)
  └─ GATE: smoke_test_all.sh <exchange> — sim starts, observer gets STATUS

Sprint 5: Trader + Observer Integration (~100 lines, 1 agent)
  └─ Add --exchange <name> to trader binary
  └─ Add --exchange <name> to observer binary
  └─ GATE: smoke_test_all.sh <exchange> — FULL PIPELINE PASSES

Sprint 6: E2E Journals (~200 lines, 1-2 agents)
  └─ Port generic journals to exchange protocol format
  └─ Add exchange-specific journals
  └─ GATE: All journals pass + smoke test still green

Sprint 7: Advanced Features (as needed)
  └─ Secdef channel
  └─ Snapshot recovery
  └─ Spread definitions
  └─ GATE: smoke test green after each feature
```

### Parallelism Rules

```
PARALLEL-SAFE (different files):
  - CME codec agent + ICE codec agent + KRX codec agent
  - Sprint 3 agents within same exchange (order entry vs market data)

NOT PARALLEL-SAFE (same files):
  - Two agents editing exchange_observer.cc
  - Two agents editing tools/BUILD.bazel
  - Sim runner wiring (touches too many files)

When in doubt: serialize. Rebase conflicts cost more than waiting.
```

### Key Learnings from Our Build

**What to watch for per exchange:**

| Exchange | Pitfall We Hit | Prevention |
|----------|---------------|------------|
| CME | SMP party_details_list_req_id hardcoded | Test SMP with different accounts in smoke test |
| ICE | FIX exec publisher keyed by wrong ID | Use two-phase pending→active pattern from day 1 |
| KRX | Missing register_order in sim runner | Copy ICE sim runner as template, don't write from scratch |
| ALL | IceCodec missing FIX tag 1 (Account) | Smoke test with 2 traders, different accounts |
| ALL | Market-makers don't cross at same ref_price | Offset ref_prices in smoke test |

---

## 8. Phase 6: Portable Journal Testing

> Goal: Write behavior tests ONCE, run against every exchange.

### The Problem We Had

We wrote 43 exchange-specific journal files:
- 18 CME journals using `ILINK3_NEW_ORDER`, `MDP3_QUOTE`
- 12 ICE journals using `ICE_FIX_NEW_ORDER`, `IMPACT_QUOTE`
- 13 KRX journals using `KRX_FIX_NEW_ORDER`, `FAST_QUOTE`

The BEHAVIOR being tested is identical. Only the protocol encoding differs.

### The Solution: Protocol Adapter

```
Generic journal (protocol-agnostic):
  ACTION NEW_ORDER instrument=ES side=BUY price=5000.00 qty=10 account=1
  EXPECT FILL price=5000.00 qty=10

Exchange adapter (plugged in at runtime):
  --exchange cme → translates to iLink3 SBE
  --exchange ice → translates to FIX 4.2
  --exchange krx → translates to FIX 4.2

Same 30 journals × 3 exchanges = 90 tests from 30 files
```

### Why This Catches More Bugs

The FIX exec publisher order-ID keying bug would have been caught immediately:
- `02_crossing_orders.journal` runs against CME → PASS
- `02_crossing_orders.journal` runs against ICE → FAIL (no fill)
- Root cause isolated to ICE-specific codec/publisher layer

Instead, we found it months later via manual smoke testing.

### Exchange-Specific Journals

Some behaviors are exchange-specific and don't port:
- KRX sidecar activation
- KRX tiered daily limits (8%→15%→20%)
- ICE circuit breaker
- ICE GTBPR matching

These stay as exchange-specific journal files alongside the portable ones.

---

## 9. Agent Team Structure

### Team Template

```
┌─────────────────────────────────────────────────┐
│                  LEAD (coordinator)              │
│  - Never writes code                            │
│  - Spawns agents, tracks progress               │
│  - Merges worktrees to main                     │
│  - Runs smoke test after each merge             │
│  - Updates swim lane display                    │
├────────────┬────────────┬───────────────────────┤
│ dev-1      │ dev-2      │ dev-3                 │
│ (worktree) │ (worktree) │ (worktree)            │
│            │            │                       │
│ Sequential │ Sequential │ Independent           │
│ chain      │ chain      │ tasks                 │
├────────────┴────────────┴───────────────────────┤
│                 reviewer                         │
│  - Reviews each merge before it lands on main   │
│  - One approval sufficient                      │
└─────────────────────────────────────────────────┘
```

### Assignment Rules

1. **Sequential dependency chains → one dev.** Don't split
   `encoder → decoder → publisher → gateway` across agents.
   Merge conflicts cost more than waiting.

2. **Independent tasks → parallel agents.** CME codec + ICE codec + KRX codec
   can run simultaneously.

3. **Same-file edits → serialize.** If two tasks touch `exchange_observer.cc`,
   assign them to the same agent sequentially.

4. **Continue agents, don't respawn.** Use `SendMessage` to give an existing
   agent its next task. Avoids ~20K tokens of cold-start overhead per agent.

5. **Planner has autonomy.** Reassign based on who finishes first, as long
   as dependency order is respected.

### Swim Lane Format

```
[Phase Name] — Swim Lane
═══════════════════════════════════════════
dev-1 (role)  │ [current]-- → next → ...
dev-2 (role)  │  done OK → [current]-- → ...
reviewer-1    │ .. standing by
═══════════════════════════════════════════
Progress: X/Y complete  ██████░░░░  Z%
```

---

## 10. Token Optimization

### Cost Breakdown (actual vs optimized)

```
Phase                  Actual Tokens  Optimized (est)  Savings
─────────────────────  ─────────────  ───────────────  ───────
Harness/governance        ~100K           50K           50%
Research (ad-hoc)         ~150K          100K           33%
Core (3 passes)           ~600K          400K           33%
Gap closure               ~150K            0K          100%
Tooling                   ~300K          200K           33%
CME                       ~300K          200K           33%
ICE                       ~300K          200K           33%
KRX                       ~300K          200K           33%
Smoke test debugging      ~500K            0K          100%
─────────────────────  ─────────────  ───────────────  ───────
Total                    ~2,700K        1,350K          50%
```

### Specific Strategies

**1. Model selection by task type**

| Task | Model | Why |
|------|-------|-----|
| Architecture, hard debugging | Opus | Deep reasoning needed |
| Feature implementation, docs | Sonnet | Good enough, 3x cheaper |
| Journals, BUILD edits, config | Haiku | Mechanical, 15x cheaper |

We used Opus for everything. Haiku could handle ~30% of tasks.

**2. Agent continuation instead of respawning**

Each agent spawn costs ~20K tokens (system prompt + file reads).
For sequential tasks: `SendMessage` to existing agent.
For 5 sequential tasks: saves ~80K tokens per chain.

**3. Include patterns in prompts, not file paths**

Instead of: "Read cme_spreads.h, ice_spreads.h, spread_strategy.h"
Write: "Follow this pattern: `SpreadInstrumentConfig{id=3001, ...}`"

Saves ~1-2K tokens per file not read.

**4. Front-load the smoke test**

The 5 smoke test bugs cost ~500K tokens to debug iteratively.
With smoke test from Phase 0: caught at introduction time, ~5K each = 25K total.
Savings: ~475K tokens.

**5. Portable journals eliminate duplicate test writing**

30 portable journals instead of 43 exchange-specific ones.
Each journal: ~50 lines = ~200 tokens to write.
Savings: 13 fewer journals × 200 = 2,600 tokens (small, but the DEBUGGING
savings from catching bugs earlier is 10-100x larger).

---

## 11. Lessons Learned

### What Worked

1. **Plan-first with researcher review** — caught circular propagation risk
   in implied trading before any code was written.

2. **CRTP exchange abstraction** — adding ICE and KRX required zero changes
   to exchange-core. The 15K-line core serves all 3 exchanges.

3. **Team parallelism** — 3 agents cutting wall-clock time for the 25-task
   implied trading phase.

4. **Journal-based E2E testing** — 43 tests exercising full pipelines without
   mocking. Readable, debuggable, fast.

5. **Incremental commits** — 272 commits, each a single logical change.
   Git bisect is practical.

### What Went Wrong

1. **Smoke test came last** — 5 integration bugs found in Phase 12 that
   should have been caught in Phases 4-5. Cost: ~500K tokens to debug.

2. **Research was incomplete** — 20+ features retrofitted through gap closure.
   Cost: an entire extra phase.

3. **Agents duplicated file reads** — 15+ agents each reading the same 10
   files. Cost: ~150K wasted tokens.

4. **Plans were over-decomposed** — 24 tasks for 650 lines. Consolidated
   to 7 at execution time. Wasted planning effort.

5. **Protocol bugs invisible to unit tests** — IceCodec missing FIX tag 1
   passed all unit tests. Only caught by live smoke test. Lesson: integration
   tests must use the REAL pipeline.

### Rules for Next Time

1. **Smoke test before first line of code.**
2. **Journals are the spec, not the afterthought.**
3. **Research EVERYTHING before implementing ANYTHING.**
4. **Core API freeze after Phase 2 — no exceptions.**
5. **Portable journals eliminate the protocol bug class.**
6. **Same-file edits are never parallel.**
7. **Continue agents, don't respawn.**

---

## 12. Feature Inventory

### Core Features (exchange-agnostic, in exchange-core/)

| Category | Features |
|----------|----------|
| Matching | FIFO, ProRata, ThresholdProRata, Allocation, SplitFifoProRata, FifoLmm, FifoTopLmm, GTBPR |
| Order Types | Limit, Market, Stop, StopLimit, Iceberg |
| TIF | DAY, GTC, IOC, FOK, GTD |
| Sessions | 9 states: Closed, PreOpen, OpeningAuction, Continuous, PreClose, ClosingAuction, Halt, VolatilityAuction, LockLimit |
| Auctions | Collect, calculate equilibrium, uncross, indicative price |
| Risk | Dynamic bands, daily limits, SMP (4 modes), rate throttle, position limits, max order size |
| Trade Mgmt | Trade bust, GTC persistence, mass cancel |
| Market Data | L1 (TopOfBook), L2 (DepthUpdate), L3 (OrderBookAction), Trade, MarketStatus |
| Spreads | SpreadBook, ImpliedPriceEngine, CircularGuard, MultiLegCoordinator, atomic batch fills |

### CRTP Hooks (overridable per exchange)

| Hook | Default | Overridden By |
|------|---------|---------------|
| is_self_match | false | CME, ICE, KRX |
| get_smp_action | CancelNewest | ICE (configurable) |
| calculate_dynamic_bands | static | CME (%), ICE (fixed), KRX (%) |
| should_trigger_volatility_auction | false | KRX (dual threshold) |
| on_daily_limit_hit | no-op | KRX (tiered widening) |
| is_order_allowed_in_phase | true | CME, ICE, KRX |
| on_validate_order | true | KRX (sidecar) |
| is_rate_check_enabled | false | all (when configured) |
| is_position_check_enabled | false | all (when configured) |
| get_modify_policy | CancelReplace | all |

### Per-Exchange Features

| Feature | CME | ICE | KRX |
|---------|-----|-----|-----|
| Matching | FIFO/ProRata/LMM | FIFO/GTBPR | FIFO |
| Order Entry | iLink3 SBE | FIX 4.2 | FIX 4.2 |
| Market Data | MDP3 SBE | iMpact binary | FAST 1.1 |
| Secdef | MDP3 multicast | iMpact inline | FAST multicast |
| Recovery | MDP3 snapshot mcast | TCP snapshot | FAST snapshot mcast |
| Dynamic Bands | % of last trade | Fixed-point IPL | % of last trade |
| SMP | CancelNewest | Configurable | CancelNewest |
| VI Trigger | Single threshold | Single threshold | Dual (dynamic+static) |
| Daily Limits | Single tier | Single tier | 3 tiers (8/15/20%) |
| Circuit Breaker | N/A | Halt/resume | Sidecar |
| Settlement | N/A | VWAP | N/A |
| Spreads | 6 (cal/bf/condor/crack) | 4 (cal/crack) | 4 (cal/bf/inter) |
| Products | 8 | 10 | 10 |
| E2E Journals | 18 | 12 | 13 |

### Still Missing (Future Work)

| Feature | Priority | Exchanges |
|---------|----------|-----------|
| Mass cancel spread orders | Medium | CME, ICE |
| Spread orders in observer TUI | Medium | All |
| Spread-aware journal reconciler | Medium | All |
| Auction + spread interaction | Low | CME, ICE |
| Stop trigger on implied fills | Low | ICE |
| Market-to-Limit order type | Low | SGX |
| OCO / bracket orders | Low | Eurex |
| ATO/ATC TIF | Low | SGX, JPX |
| Velocity logic (anti-disruptive) | Low | CME |
| Member/firm account hierarchy | Low | All |
| Margin integration hooks | Low | All |
| Performance/latency benchmarks | Medium | All |
| 7 more exchanges | Future | Eurex, LME, ASX, HKEX, JPX, SGX, NSE |
