# Exchange Core Gap Analysis: 10 Global Futures Exchanges

**Date:** 2026-03-24
**Baseline:** Phase 1 exchange-core (design spec dated 2026-03-24)
**Scope:** Feature gaps for building accurate simulators of 10 major global futures exchanges

---

## 1. Executive Summary

The Phase 1 exchange-core provides a solid foundation: a single-threaded, zero-allocation matching engine with FIFO and Pro-Rata algorithms, basic order types (Limit, Market, Stop, StopLimit), five TIF types (DAY, GTC, IOC, FOK, GTD), static price bands, and self-trade prevention. The CRTP extension model gives exchange-specific implementations clean hook points.

However, **the engine cannot currently simulate any of the 10 target exchanges with production accuracy**. The gaps fall into three severity tiers:

| Tier | Gap | Impact |
|------|-----|--------|
| **Critical** | No session state machine / auction support | Every exchange has opening/closing auctions; the engine has no concept of trading phases |
| **Critical** | No implied / spread trading | CME, Eurex, ICE, SGX, ASX all derive prices from spread order books |
| **Critical** | No iceberg / hidden quantity orders | 9 of 10 exchanges support iceberg orders natively |
| **High** | No dynamic price banding | All 10 exchanges use dynamic (not static) price bands anchored to last trade or reference price |
| **High** | Only 2 of 8+ matching algorithms | CME alone uses 8 algorithms; Eurex, ASX, SGX each have variants |
| **High** | No market maker priority | CME (LMM), Eurex (MM), SGX, HKEX all grant market makers priority allocation |
| **High** | Missing order types (MTL, MIT, OCO, Bracket) | SGX, LME, Eurex, CME each require additional native order types |
| **Medium** | No mass cancel / kill switch | Regulatory requirement at every exchange |
| **Medium** | No GTC persistence across sessions | GTC orders must survive session boundaries with re-validation |
| **Medium** | No position limits / margin hooks | All exchanges enforce position limits and pre-trade risk checks |

**Overall readiness score: ~25%** -- the core matching loop is correct and performant, but the surrounding infrastructure (sessions, auctions, risk controls, spread trading) that defines an exchange's behavior is entirely absent.

---

## 2. Feature Matrix

Legend: **S** = Supported, **R** = Required by this exchange, **-** = Not applicable/not needed, **P** = Partially supported

| Feature | Current | CME | Eurex | ICE | LME | ASX | HKEX | KRX | JPX | SGX | NSE |
|---------|---------|-----|-------|-----|-----|-----|------|-----|-----|-----|-----|
| **Session State Machine** | | | | | | | | | | | |
| Pre-open phase | - | R | R | R | R | R | R | R | R | R | R |
| Opening auction | - | R | R | R | R | R | R | R | R | R | R |
| Continuous trading | S | R | R | R | R | R | R | R | R | R | R |
| Closing auction | - | R | R | R | - | R | R | R | R | R | R |
| After-hours / T+1 | - | R | R | R | - | - | R | R | R | - | - |
| Halt / circuit breaker state | - | R | R | R | - | R | R | R | R | R | R |
| Volatility auction | - | R | R | - | - | R | R | - | R | R | - |
| **Order Types** | | | | | | | | | | | |
| Limit | S | R | R | R | R | R | R | R | R | R | R |
| Market | S | R | R | R | R | R | R | R | R | R | R |
| Stop | S | R | R | R | R | R | R | R | - | R | R |
| Stop Limit | S | R | R | R | R | R | R | R | - | R | R |
| Iceberg / Display Qty | - | R | - | R | R | R | R | - | - | R | R |
| Market-to-Limit (MTL) | - | - | - | - | - | - | - | - | - | R | - |
| Market-if-Touched (MIT) | - | R | - | - | - | - | - | - | - | - | - |
| One-Cancels-Other (OCO) | - | - | R | - | R | - | - | - | - | - | - |
| Book-or-Cancel (BOC) | - | - | R | - | - | - | - | - | - | - | - |
| Trailing Stop | - | - | - | - | - | - | - | - | - | - | - |
| Bracket | - | - | - | - | - | - | - | - | - | - | - |
| Implied / Strategy orders | - | R | R | R | - | R | - | - | - | R | - |
| **Time in Force** | | | | | | | | | | | |
| DAY | S | R | R | R | R | R | R | R | R | R | R |
| GTC | S | R | R | R | R | R | R | R | R | R | R |
| IOC | S | R | R | R | R | R | R | R | R | R | R |
| FOK | S | R | R | R | R | R | R | R | R | R | R |
| GTD | S | R | R | R | R | R | R | R | R | R | R |
| At-The-Open (ATO) | - | - | - | - | - | - | - | - | - | R | - |
| At-The-Close (ATC) | - | - | R | - | - | - | - | - | R | R | - |
| Good-for-Auction (GFA) | - | - | R | - | - | - | - | - | - | - | - |
| Session-specific (AM/PM) | - | - | - | - | - | - | - | - | R | - | - |
| **Matching Algorithms** | | | | | | | | | | | |
| FIFO (Price-Time) | S | R | R | R | R | R | R | R | R | R | R |
| Pro-Rata + FIFO remainder | S | R | R | - | - | - | - | - | - | - | - |
| FIFO + LMM priority | - | R | R | - | - | - | R | - | - | R | - |
| FIFO + Top Order + LMM | - | R | - | - | - | - | - | - | - | - | - |
| Threshold Pro-Rata | - | R | - | - | - | - | - | - | - | - | - |
| Threshold Pro-Rata + LMM | - | R | - | - | - | - | - | - | - | - | - |
| Configurable (split FIFO/PR) | - | R | - | - | - | - | - | - | - | - | - |
| Allocation (pure pro-rata) | - | R | - | - | - | - | - | - | - | - | - |
| Auction uncrossing | - | R | R | R | R | R | R | R | R | R | R |
| **Risk Controls** | | | | | | | | | | | |
| Static price bands | S | R | R | R | R | R | R | R | R | R | R |
| Dynamic price bands | - | R | R | R | R | R | R | R | R | R | R |
| Velocity logic | - | R | - | - | - | - | - | - | - | - | - |
| Daily price limits | - | R | R | R | - | - | - | R | R | R | R |
| Self-trade prevention | S | R | R | R | R | R | R | R | R | R | R |
| Max order size | - | R | R | R | R | R | R | R | R | R | R |
| Order rate throttle | - | R | R | R | R | R | R | R | R | R | R |
| Mass cancel / Kill switch | - | R | R | R | R | R | R | R | R | R | R |
| Market-wide circuit breaker | - | R | R | R | - | R | R | R | R | R | R |
| **Spread / Implied Trading** | | | | | | | | | | | |
| Calendar spreads | - | R | R | R | R | R | - | - | R | R | - |
| Implied-in / Implied-out | - | R | R | R | - | R | - | - | - | R | - |
| Strategy order books | - | R | R | R | R | R | - | - | R | R | - |
| **Market Data** | | | | | | | | | | | |
| L1 (TopOfBook) | S | R | R | R | R | R | R | R | R | R | R |
| L2 (Depth / MBP) | S | R | R | R | R | R | R | R | R | R | R |
| L3 (MBO) | S | R | - | R | R | R | - | - | - | - | - |
| Implied price dissemination | - | R | R | R | - | R | - | - | - | R | - |
| Settlement price calc | - | R | R | R | R | R | R | R | R | R | R |
| OHLCV statistics | - | R | R | R | R | R | R | R | R | R | R |
| Market status messages | - | R | R | R | R | R | R | R | R | R | R |
| Indicative auction price | - | R | R | R | R | R | R | R | R | R | R |
| **Infrastructure** | | | | | | | | | | | |
| Member/firm controls | - | R | R | R | R | R | R | R | R | R | R |
| Account hierarchy | - | R | R | R | R | R | R | R | R | R | R |
| Position limits | - | R | R | R | R | R | R | R | R | R | R |
| Margin hooks | - | R | R | R | R | R | R | R | R | R | R |
| Trade bust / adjustment | - | R | R | R | R | R | R | R | R | R | R |
| GTC cross-session persist | - | R | R | R | R | R | R | R | R | R | R |

---

## 3. Core vs Exchange Layer Classification

The exchange-core must remain generic — it provides **mechanisms**, not **policies**. Each feature gap is classified into one of three layers:

| Layer | Description | Ownership |
|-------|-------------|-----------|
| **Core** | Generic mechanism needed by 7+ exchanges with uniform implementation | exchange-core library |
| **Core (extensible)** | Framework/hooks in core, venue-specific behavior via CRTP or config | Core provides hooks, exchange configures |
| **Exchange layer** | Venue-specific, varies significantly, or needed by < 5 exchanges | Per-exchange CRTP class or wrapper |

### 3.1 Core Layer — Generic Mechanisms to Add

These belong in exchange-core because they are universal and have a single correct implementation.

| Feature | Why Core | Notes |
|---------|----------|-------|
| **Session state machine framework** | All 10 exchanges need phases | Core provides: `SessionState` enum, `transition(old, new)` method, TIF validation per phase. Exchange provides: the schedule and transition rules via CRTP. |
| **Auction uncrossing algorithm** | All 10 exchanges need opening/closing auctions | Standard equilibrium price calculation (maximize volume, minimize imbalance, reference price tiebreak). The algorithm itself is universal — what varies is WHEN it runs (exchange layer). |
| **Iceberg/hidden quantity support** | 9/10 exchanges | Core adds `display_qty` field to Order, reveals next tranche after fill. Exchange sets minimum display qty via CRTP `get_min_display_qty()`. |
| **Mass cancel API** | All 10 exchanges, regulatory requirement | `mass_cancel(account_id)`, `mass_cancel_all()`. Pure engine operation. |
| **Max order size validation** | All 10 exchanges | Add `max_order_size` to EngineConfig. Core rejects orders exceeding it. |
| **OHLCV statistics accumulator** | All 10 exchanges | Core tracks open/high/low/close/volume/vwap per session. Reset on session transition. |
| **Market status callback** | All 10 exchanges | New callback: `on_market_status(SessionState, Timestamp)`. |
| **Trade bust/adjustment API** | All 10 exchanges | `bust_trade(trade_id)` reverses a trade, restores orders. Core mechanics. |
| **GTC order persistence hooks** | All 10 exchanges | Core provides `serialize_order()` / `restore_order()` for session boundary management. Exchange manages storage. |
| **Daily price limit** | 8/10 exchanges | Like price bands but absolute limits that trigger halt/lock-limit state. Core enforces; exchange configures levels. |
| **Order rate tracking** | All 10 exchanges | Core counts orders per account per interval. Exchange sets the threshold via config. |

### 3.2 Core (Extensible) — Framework with CRTP Hooks

Core provides the mechanism; exchange implementations customize behavior.

| Feature | Core Provides | Exchange Provides (CRTP) |
|---------|---------------|--------------------------|
| **Session state transitions** | State enum, validation framework, `transition()` | `on_session_transition(old, new)` — defines which TIFs are valid, whether matching occurs, whether new orders are accepted |
| **Auction trigger conditions** | Volatility auction framework, `enter_auction()` / `exit_auction()` | `should_trigger_volatility_auction(price, reference)` — thresholds and rules |
| **Dynamic price bands** | Band check infrastructure, `update_bands()` | `calculate_bands(reference_price)` — band width, reference price source, asymmetric bands |
| **Circuit breaker behavior** | Halt state, `trigger_circuit_breaker()` | `on_circuit_breaker(level)` — what happens (halt, auction, restrict range) per level |
| **Matching algorithm per phase** | Dispatch to different MatchAlgoT for auction vs continuous | `get_match_algo_for_phase(phase)` — or use template specialization |
| **Market maker priority** | LMM priority allocation in matching | `is_market_maker(order)`, `get_mm_priority_pct()` — who qualifies and their allocation share |
| **SMP variations** | Already extensible (cancel newest/oldest/both/decrement) | `is_self_match()`, `get_smp_action()` — already CRTP hooks |
| **Modify policy** | Already extensible | `get_modify_policy()` — already a CRTP hook |
| **Stop trigger logic** | Already extensible | `should_trigger_stop()` — already a CRTP hook |
| **Indicative auction price** | Calculation method, callback `on_indicative_price()` | `calculate_indicative_price()` — exchange may weight differently |
| **Settlement price** | Calculation hooks, `on_settlement_price()` | `calculate_settlement_price()` — VWAP, last trade, weighted average vary by venue |

### 3.3 Exchange Layer Only — Per-Venue Implementation

These do NOT belong in exchange-core. They are implemented in the per-exchange CRTP class or exchange wrapper.

| Feature | Why Exchange Layer | Example Venues |
|---------|-------------------|----------------|
| **Session schedule / timing** | Every exchange has unique times, holidays, early closes | CME 17:00-16:00 CT; Eurex 07:30-22:00 CET; HKEX T+1 session |
| **LME Ring/telephone trading** | Unique to LME, non-electronic | LME only |
| **JPX Itayose method specifics** | Unique auction variant | JPX only |
| **KRX sidecar mechanism** | Halts programme trading only, unique | KRX only |
| **HKEX VCM soft halt** | Restricts range instead of halting, unique | HKEX only |
| **NSE 3-tier circuit breaker percentages** | Specific thresholds (10%/15%/20%) | NSE only |
| **Random auction end time** | ASX, some others add random extension to prevent sniping | ASX, Eurex |
| **Exchange-specific order types** | Eurex BOC, SGX MTL — unique per venue | Per venue |
| **Session-specific TIFs** | JPX AM/PM session TIFs | JPX only |
| **ATO/ATC/GFA TIF rules** | Which TIF is valid in which phase — varies per venue | Eurex GFA, SGX ATO/ATC |
| **Implied/spread strategy definitions** | Which spreads exist, how legs combine — product-specific | CME, Eurex, ICE, SGX |
| **Member/firm hierarchy** | Access levels, permissions — venue-specific | All, but each is different |
| **Margin integration** | Margin methodology (SPAN, PRISMA, VaR) is venue-specific | All, but each uses different system |
| **Position limit levels** | Specific limits per product/account | All, but values are venue-specific |
| **Velocity logic rules** | CME-specific anti-disruptive trading | CME only |
| **Protocol-specific behavior** | FIX tags, ITCH fields, OUCH semantics | Per protocol |

### 3.4 New CRTP Extension Points Needed

These hooks need to be added to `MatchingEngine` for Phase 2:

| Hook | Signature | Default | Purpose |
|------|-----------|---------|---------|
| `on_session_transition` | `void(SessionState old_state, SessionState new_state)` | no-op | Exchange controls phase transition behavior |
| `is_order_allowed_in_phase` | `bool(const OrderRequest&, SessionState)` | `return true` | Reject orders invalid for current phase (e.g., IOC during auction) |
| `should_trigger_volatility_auction` | `bool(Price trade_price, Price reference)` | `return false` | Exchange defines VI thresholds |
| `calculate_dynamic_bands` | `std::pair<Price,Price>(Price reference)` | static bands | Exchange defines band calculation |
| `is_market_maker` | `bool(const Order&)` | `return false` | Identifies MM orders for priority |
| `get_mm_priority_pct` | `int()` | `0` | Percentage of level allocated to MMs |
| `get_min_display_qty` | `Quantity(const Order&)` | `0` (no iceberg) | Exchange sets min visible qty |
| `calculate_settlement_price` | `Price()` | `last_trade_price_` | Exchange defines settlement method |
| `on_circuit_breaker` | `void(int level, Price trigger_price)` | no-op | Exchange defines halt/auction behavior |
| `get_auction_match_algo` | `AuctionAlgo()` | `MaxVolume` | Exchange may use different uncrossing |

### 3.5 Design Principle Summary

```
+------------------------------------------------------------------+
|                    ExchangeImpl (CRTP derived)                    |
|  Per-venue: session schedule, TIF rules per phase, VI thresholds,|
|  MM identification, SMP policy, circuit breaker levels,          |
|  settlement method, implied strategy definitions, protocol layer |
+------------------------------------------------------------------+
         |  inherits via CRTP
         v
+------------------------------------------------------------------+
|              MatchingEngine<Derived, ...>  (exchange-core)        |
|                                                                   |
|  Generic: session state machine, auction uncrossing, iceberg,    |
|  dynamic bands framework, mass cancel, order rate tracking,      |
|  OHLCV stats, market status, trade bust, GTC persistence hooks,  |
|  daily price limits, max order size, L1/L2/L3 market data       |
+------------------------------------------------------------------+
         |  uses
         v
+------------------------------------------------------------------+
|              Foundation (exchange-core)                            |
|                                                                   |
|  OrderBook, StopBook, ObjectPool, IntrusiveList, SPSC buffer,   |
|  FifoMatch, ProRataMatch, Listeners, Events, Types               |
+------------------------------------------------------------------+
```

**The rule:** If a feature's *mechanism* is the same everywhere but its *parameters* vary, the mechanism goes in core and parameters go in CRTP. If both mechanism and parameters are venue-specific, it's exchange layer only.

---

## 4. Gap Analysis by Category (Detailed)

### 3.A Session State Machine

**Current state:** The engine has no concept of trading phases. It is always in "continuous trading" mode. There is no mechanism to transition between states, restrict order types by phase, or trigger auctions.

**What every exchange needs:**

All 10 exchanges operate with a well-defined state machine governing the trading day. The typical state graph is:

```
                        +------------------+
                        |  Start of Day    |
                        +--------+---------+
                                 |
                        +--------v---------+
                        |    Pre-Open      |  Orders accepted, no matching
                        +--------+---------+
                                 |
                        +--------v---------+
                        | Opening Auction   |  Call auction, uncrossing
                        +--------+---------+
                                 |
                        +--------v---------+
              +-------->| Continuous Trading|<--------+
              |         +----+--------+----+         |
              |              |        |              |
              |    +---------v--+  +--v-----------+  |
              |    | Volatility |  | Scheduled    |  |
              |    | Auction    |  | Intraday     |  |
              |    | (halt)     |  | Auction      |  |
              |    +-----+------+  +------+-------+  |
              |          |                |           |
              +----------+                +-----------+
                                 |
                        +--------v---------+
                        | Pre-Close        |  Orders accepted, no matching
                        +--------+---------+
                                 |
                        +--------v---------+
                        | Closing Auction   |  Call auction, uncrossing
                        +--------+---------+
                                 |
                        +--------v---------+
                        |   End of Day     |  DAY orders expire
                        +--------+---------+
                                 |
                        +--------v---------+  (CME, HKEX, KRX, JPX)
                        | After-Hours/T+1  |
                        +------------------+
```

**Exchange-specific session details:**

| Exchange | Sessions | Key Differences |
|----------|----------|-----------------|
| **CME** | Pre-Open, Open, Pre-Close, Close, Halt (DCB/Velocity) | Multiple market states managed by CME GCC; halt duration reduces to 5s near settlement |
| **Eurex** | Start-of-Day, Pre-Trading, Opening Auction, Continuous, Closing Auction, Post-Trading | Volatility interruptions move instrument to auction mid-session; extended VI for persistent imbalance |
| **ICE** | Pre-Trading Session, Opening Match, Open, Close | Pre-Trading accepts Limit orders only; Opening Match uncrosses at single price |
| **LME** | Pre-Open, Electronic Open (LMEselect), Ring Sessions (5-min per metal), Kerb, Close | Unique: Ring is open-outcry; LMEselect has SFPA (Systematic Fixed Price Auction) lasting 30 seconds |
| **ASX** | Pre-Open, Opening Auction, Continuous, Pre-Close, Closing Auction, Post-Close | Random end to Pre-Open/Pre-Close to prevent manipulation |
| **HKEX** | Pre-Open, Morning Session, Lunch Break, Afternoon Session, Closing Auction (CAS), T+1 Session | VCM does not apply during T+1 session or first/last minutes of sessions |
| **KRX** | Pre-Open Call Auction, Continuous (09:00-15:30), Closing Call Auction, Off-Hours (16:00-18:00) | Sidecar: 5-min programme trading halt on 5% KOSPI200 futures move; Circuit breaker: 20-min halt on 8% decline |
| **JPX** | Pre-Open (Itayose), Day Session (Zaraba), Closing Auction (Itayose), Night Session | DCB (Immediately Executable Price Range) halts trading for 30s+ when price exceeds range |
| **SGX** | Pre-Open (30 min), Non-Cancel Phase, Continuous, Pre-Close, Closing Routine | Random end to Pre-Open/Pre-Close phases; Equilibrium Price = max volume + min imbalance |
| **NSE** | Pre-Open Call Auction (9:00-9:15, new for F&O Dec 2025), Continuous (9:15-15:30) | Pre-Open has 3 sub-phases: Order Entry (random close 9:07-9:08), Matching, Buffer; no stop/IOC orders during auction |

**Implementation requirement:** A `TradingPhase` enum and state machine that:
1. Controls which order types and TIFs are accepted per phase
2. Accumulates orders during auction phases without matching
3. Triggers uncrossing at phase transitions
4. Supports scheduled and event-driven transitions (volatility interruption)
5. Emits market status change events

---

### 3.B Order Types Beyond Current Support

**Current:** Limit, Market, Stop, StopLimit

**Missing order types required across the 10 exchanges:**

#### Iceberg / Display Quantity Orders (Required by: CME, ICE, LME, ASX, HKEX, SGX, NSE)

The most critical missing order type. An iceberg order has a total quantity and a visible (display) quantity. Only the display portion is visible on the book. When the visible portion fills, a new "tranche" is automatically placed at the back of the queue.

```
Order fields needed:
  - display_quantity    (visible portion)
  - total_quantity      (full order size, including hidden)
  - refresh_quantity    (how much to replenish; often == display_quantity)
```

CME calls these "Display Quantity" orders. The LME replenishes in configurable increments (e.g., 50 lots). ASX calls them "Iceberg Orders" with automatic child order replenishment.

**Impact on matching:** When an iceberg tranche fills, the new tranche loses time priority (goes to back of queue at that price level). The engine must track hidden vs. displayed quantity separately and fire appropriate market data events (the hidden portion must not appear in L2/L3 feeds).

#### Market-to-Limit (MTL) (Required by: SGX)

A market order that, after matching the best opposite price level, converts its unfilled remainder to a limit order at the price of its last fill. If no fills occur, it is rejected.

#### Market-if-Touched (MIT) (Required by: CME)

Similar to a stop order, but with reversed trigger logic:
- Buy MIT: triggers when price drops to or below the trigger price
- Sell MIT: triggers when price rises to or above the trigger price

Once triggered, becomes a market order.

#### One-Cancels-Other (OCO) (Required by: Eurex, LME)

Two linked orders where execution or cancellation of one automatically cancels the other. Typically pairs a limit order with a stop order on the same side.

**Impact:** Requires an order linkage mechanism (parent-child or sibling relationships) that the current engine lacks entirely.

#### Book-or-Cancel (BOC) / Post-Only (Required by: Eurex)

An order that is rejected if it would immediately match (i.e., it must add liquidity). If the order's price would cross the spread, it is deleted without execution.

#### Implied / Strategy Orders (Required by: CME, Eurex, ICE, ASX, SGX)

See Section 3.G for full analysis. Strategy orders are multi-leg instruments where the exchange maintains separate strategy order books and derives implied prices.

#### Undisclosed Orders (Required by: ASX)

Similar to icebergs but with zero display quantity -- the order price is visible but the entire quantity is hidden. Lower priority than displayed orders at the same price.

---

### 3.C TIF Types Beyond Current Support

**Current:** DAY, GTC, IOC, FOK, GTD

**Missing TIFs:**

| TIF | Description | Required By |
|-----|-------------|-------------|
| **ATO (At-The-Open)** | Participates only in the opening auction; cancelled if not filled during auction uncrossing | SGX |
| **ATC (At-The-Close)** | Participates only in the closing auction; cancelled if not filled during closing uncrossing | Eurex, JPX, SGX |
| **GFA (Good-for-Auction)** | Valid only during any auction phase (opening, closing, or volatility); cancelled when continuous trading resumes | Eurex |
| **Session-specific** | Valid only for a specific named session (e.g., Morning Close, Afternoon Close, Evening Close) | JPX (On Close - Morning, On Close - Afternoon, On Close - Evening) |

**Implementation requirement:** TIF validation must be session-phase-aware. For example:
- ATO orders must be rejected outside the opening auction phase
- IOC/FOK orders must be rejected during auction collection phases (NSE, Eurex, SGX explicitly prohibit these)
- Stop orders must be rejected during pre-open at NSE
- The existing `is_tif_valid()` CRTP hook is correctly placed but needs access to current trading phase

---

### 3.D Matching Algorithm Variations

**Current:** FIFO (Price-Time Priority), Pro-Rata with FIFO remainder distribution

**CME alone requires 8 matching algorithms.** The following are missing:

#### 1. FIFO with LMM Priority (CME, Eurex, HKEX, SGX)

Before FIFO matching, designated Lead Market Maker (LMM) orders at the best price receive a configurable percentage of the aggressor quantity. Multiple LMMs are matched among themselves by time priority.

```
Algorithm steps:
  1. LMM allocation: min(LMM_pct * aggressor_qty, LMM_resting_qty) to LMM orders
  2. Remaining quantity matched FIFO across all orders (including unfilled LMM portion)
```

#### 2. FIFO with Top Order and LMM Priority (CME)

The first order to establish a new best price level ("Top Order") receives priority allocation before LMM and FIFO steps.

```
Algorithm steps:
  1. Top Order allocation: min(TOP_pct * aggressor_qty, top_order_remaining)
  2. LMM allocation on remainder
  3. FIFO on remainder
```

Only one buy and one sell order can have TOP priority at any time.

#### 3. Threshold Pro-Rata (CME)

Pro-rata allocation with a minimum allocation threshold. If an order's pro-rata share is below the configurable minimum, it receives zero allocation. Remainder distributed FIFO.

```
Algorithm steps:
  1. Calculate pro-rata shares for all orders
  2. Zero out shares below minimum threshold (e.g., 2 lots)
  3. Redistribute zeroed shares via FIFO among remaining orders
```

#### 4. Threshold Pro-Rata with LMM (CME)

Combines threshold pro-rata with LMM priority:
```
  1. LMM allocation
  2. Threshold Pro-Rata on remainder
  3. FIFO on sub-threshold remainder
```

#### 5. Configurable / Split FIFO-ProRata (CME)

A hybrid algorithm where a configurable percentage (e.g., 40%) is allocated via FIFO and the remainder via pro-rata. Used for CME agricultural products.

```
Algorithm steps:
  1. FIFO_pct * aggressor_qty allocated by FIFO
  2. Remaining (1 - FIFO_pct) * aggressor_qty allocated by Pro-Rata
  3. Sub-threshold remainder by FIFO
```

#### 6. Allocation / Pure Pro-Rata (CME)

Similar to current ProRata but without the FIFO remainder step -- remainder goes to the largest resting order instead of the oldest.

#### 7. Auction Uncrossing Algorithm (ALL exchanges)

A fundamentally different matching approach used during auction phases:

```
  1. Collect all orders during the auction collection period
  2. Calculate the equilibrium price that:
     a. Maximizes executable volume
     b. Minimizes order imbalance (surplus)
     c. Is closest to a reference price (last trade, previous close)
  3. Execute all matchable orders at the single equilibrium price
  4. Unmatched orders carry forward to continuous trading
```

Each exchange has subtle variations in the equilibrium price selection criteria:
- **SGX/NSE:** Max volume, then min imbalance, then closest to reference
- **Eurex:** Max volume, then min surplus, then market pressure (buy surplus = higher price)
- **JPX (Itayose):** Max volume, then closest to last contract price
- **LME (SFPA):** 30-second fixed price auction at discovered price

**Implementation approach:** The matching algorithm should become a more flexible policy that varies by trading phase. During auctions, the engine accumulates orders and delegates to an uncrossing algorithm at phase transition.

---

### 3.E Auction Mechanisms

**Current state:** The engine has NO auction support whatsoever. This is the single largest gap.

**What is needed:**

#### Opening Auction

Every exchange begins the day with an opening auction (call auction):

1. **Collection Phase (Pre-Open):** Orders enter an order book but no matching occurs. The system continuously calculates and disseminates an Indicative Opening Price (IOP) and Indicative Opening Volume (IOV).

2. **Non-Cancel Phase (SGX, ASX):** A brief period where order entry and amendments are frozen. Prevents last-second manipulation.

3. **Uncrossing:** All orders are matched at a single equilibrium price. The algorithm maximizes traded volume.

4. **Transition:** Unmatched orders carry forward to continuous trading. The opening price is published.

**Key exchange-specific rules:**

| Exchange | Pre-Open Duration | Random End? | Stop Orders Allowed? | IOC/FOK Allowed? |
|----------|-------------------|-------------|----------------------|------------------|
| CME | Varies by product group | No | Yes (held) | No |
| Eurex | Product-specific | Yes | No (Stop Market only) | No |
| ICE | Configurable | No | No | No |
| ASX | ~10 min | Yes (+/- random) | No | No |
| SGX | 30 min | Yes | No | No |
| NSE | 8 min (F&O) | Yes (7th-8th min) | No | No |
| JPX | Product-specific | No | No | No |
| KRX | ~30 min | No | No | No |

#### Closing Auction

Most exchanges (CME, Eurex, ASX, HKEX, KRX, JPX, SGX, NSE) have a closing auction that determines the daily settlement/closing price. The mechanism is similar to the opening auction but with exchange-specific rules:

- **JPX:** Uses Itayose method; separate closing auctions for AM, PM, and Night sessions
- **HKEX:** Closing Auction Session (CAS) with specific order type restrictions
- **Eurex:** ATC (At-The-Close) TIF restricts orders to closing auction only

#### Volatility Auction / Circuit Breaker Auction

When a dynamic circuit breaker or volatility interruption triggers:

1. Continuous trading halts
2. The instrument enters an auction state
3. Orders accumulate without matching
4. After a minimum duration (30s for JPX DCB, configurable for Eurex), uncrossing is attempted
5. If the uncrossing price is within acceptable range, trading resumes in continuous mode
6. If not, the auction extends (Eurex: extended VI; JPX: reference price updated)

**Implementation priority:** This is the #1 gap to close. Without auction support, the engine cannot simulate a single complete trading session for any exchange.

---

### 3.F Risk Controls & Circuit Breakers

**Current state:** Static price bands only (`price_band_low`, `price_band_high` in `EngineConfig`). Self-trade prevention with 5 actions (CancelNewest, CancelOldest, CancelBoth, Decrement, None).

#### Dynamic Price Banding (Required by: ALL)

All 10 exchanges use dynamic price bands that move with the market, not static bands set at engine startup.

```
Current:
  reject if order.price < config.price_band_low
  reject if order.price > config.price_band_high
  (bands are fixed at engine startup)

Required:
  reference_price = last_trade_price (or auction price, or settlement price)
  dynamic_band_low  = reference_price - band_width
  dynamic_band_high = reference_price + band_width
  (bands move after every trade or periodically)
```

- **CME:** Price banding dynamically adjusts based on last price +/- fixed product-specific value. Bands can be temporarily expanded by CME GCC during high volatility.
- **Eurex:** Dynamic price range for volatility interruption; separate static range for extended VI.
- **JPX:** Immediately Executable Price Range = reference_price +/- (reference_price * percentage). Reference updates if halt extends.

#### Daily Price Limits (Required by: CME, Eurex, ICE, KRX, JPX, NSE)

Absolute upper and lower price boundaries for the day, typically set from previous session's settlement price. When hit, trading may halt entirely ("limit up/limit down" or "lock limit").

- **CME:** Three tiers of expanding limits (e.g., equity index futures: 7%, 13%, 20%)
- **KRX:** +/- 10%, 15%, 20% with associated circuit breaker halts
- **NSE:** Index-based: 10%, 15%, 20% of previous close
- **JPX:** Product-specific absolute limits

#### Velocity Logic (Required by: CME)

Unique to CME. Monitors how fast prices move, not just how far:

```
  Within a lookback window (e.g., last N seconds):
    if (highest_price - lowest_price) > velocity_threshold:
      trigger halt
```

This is separate from and complementary to price banding.

#### Self-Trade Prevention Variations

The current engine supports 5 SMP modes, which covers most exchanges. Notable variations:

| Exchange | SMP Name | Default Action | Notes |
|----------|----------|----------------|-------|
| CME | SMP | Cancel resting | Configurable: cancel aggressing or resting; uses SMP ID tag |
| ICE | STPF | Cancel resting (RRO) | Three modes: RRO (cancel resting), RTO (cancel taking), RBO (cancel both) |
| Eurex | STP | Cancel newest | Part of T7 platform |
| Others | Varies | Varies | Most default to cancel newest or cancel resting |

The current implementation is close but lacks:
- Per-order SMP instruction (CME allows different actions per order)
- SMP ID grouping across different firm IDs (CME supports cross-firm SMP)

#### Maximum Order Size Limits (Required by: ALL)

Missing entirely. Every exchange enforces maximum order quantity limits per product.

#### Order Rate Throttling (Required by: ALL)

Missing entirely. Every exchange throttles message rates per session/participant. Typical limits range from 50-1000 messages/second.

#### Mass Cancel / Kill Switch (Required by: ALL)

Missing entirely. Every exchange supports:
- Mass cancel by session
- Mass cancel by instrument
- Mass cancel by account/firm
- Kill switch: cancel all orders + lock out new order entry

This is a regulatory requirement (CFTC, ESMA, MAS, SEBI all mandate it).

---

### 3.G Spread / Implied Trading

**Current state:** No support whatsoever. The engine handles a single outright order book per instance.

**What is needed:**

#### Calendar Spreads (Required by: CME, Eurex, ICE, LME, ASX, JPX, SGX)

An exchange-defined strategy instrument representing the simultaneous purchase and sale of different expiry months:

```
  Calendar Spread (Jun-Sep) = Buy Jun Futures + Sell Sep Futures
  Spread Price = Jun Price - Sep Price
```

The exchange maintains a separate order book for each spread instrument. Orders in the spread book are "strategy orders" with multiple legs.

#### Implied Pricing (Required by: CME, Eurex, ICE, ASX, SGX)

The exchange derives (implies) prices from the relationship between outright and spread order books:

- **Implied-Out:** A combination of outright orders on different legs implies a spread price.
  ```
  If Buy Jun @ 100 and Sell Sep @ 95 exist as outrights,
  then an implied spread offer of Jun-Sep @ 5 is derived.
  ```

- **Implied-In:** A spread order combined with an outright order implies an outright on the other leg.
  ```
  If Buy Jun-Sep spread @ 5 and Sell Jun @ 100 exist,
  then an implied Sep bid @ 95 is derived.
  ```

**Implementation complexity:** This is one of the most complex features to implement correctly:

1. Requires multiple order books (one per instrument + one per strategy) linked together
2. Implied orders must be recalculated whenever any contributing book changes
3. Implied orders do not rest on the book -- they execute instantly if matchable
4. CME MBO data explicitly excludes implied orders from the order-level feed
5. Circular implication chains must be detected and broken

#### Strategy Order Books

Beyond calendar spreads, exchanges support various multi-leg strategies:
- Inter-commodity spreads (e.g., WTI vs Brent)
- Butterfly spreads
- Condor spreads
- Strips (multiple consecutive months)
- Custom strategies (ASX "Custom Market")

**Recommendation:** Implement a multi-book architecture where a `MatchingEngine` instance can be linked to other instances, with an implied pricing engine sitting above them.

---

### 3.H Market Data Enhancements

**Current state:** L1 (TopOfBook), L2 (DepthUpdate), L3 (OrderBookAction), Trade events. All are point-in-time updates with no aggregation.

**Missing:**

#### Implied Price Dissemination (CME, Eurex, ICE, ASX, SGX)

When implied pricing is active, the market data feed must disseminate:
- Implied bid/ask prices and quantities alongside regular book
- Whether a price level includes implied liquidity
- CME MDP 3.0 explicitly separates implied and direct book entries

#### Settlement Price Calculation (ALL)

Every exchange computes a daily settlement price used for margin calculations. The algorithm varies:
- **CME:** Typically the last trade price during the settlement window, or a VWAP, or a committee-determined price
- **Eurex:** Closing auction price, or last trade, or theoretical price
- **JPX:** Closing auction (Itayose) price

The engine needs hooks to capture data during a settlement window and compute the settlement price.

#### Opening/Closing Price Determination (ALL)

- Opening price = auction uncrossing price (or first trade price if no auction)
- Closing price = closing auction price (or last trade, or VWAP of final minutes)

#### OHLCV Statistics (ALL)

Missing entirely. The engine should track per-session:
- Open price, High price, Low price, Close/Last price
- Total volume (quantity traded)
- VWAP (volume-weighted average price)
- Trade count

#### Market Status Messages (ALL)

The engine currently emits no status/administrative messages. Exchanges publish:
- Trading phase changes (pre-open -> open -> continuous -> etc.)
- Halt/resume notifications
- Circuit breaker trigger messages
- Product status (active, suspended, expired)

---

### 3.I Other Infrastructure

#### Member/Firm-Level Controls (ALL)

The engine has `account_id` on orders (used for SMP grouping) but no concept of:
- Firm/member hierarchy
- Per-firm risk limits
- Per-firm order rate limits
- Firm-level mass cancel

#### Account Hierarchy (ALL)

Production exchanges enforce: Exchange -> Clearing Member -> Trading Member -> Trader -> Account. The current flat `account_id` is insufficient for:
- Position netting by account
- Risk limit aggregation by firm
- Regulatory reporting

#### Position Limits (ALL)

Every exchange enforces position limits (maximum net long or short position per account/firm/market). Missing entirely.

#### Margin Integration Hooks (ALL)

Pre-trade risk checks that validate whether a new order would cause a margin breach. Requires integration with a separate margin calculation engine.

#### Trade Bust / Adjustment (ALL)

The ability to reverse or modify a trade after execution (erroneous trade, clearly erroneous execution rules). The engine has no concept of post-trade modification.

#### GTC Order Management Across Sessions (ALL)

GTC and GTD orders must persist across session boundaries. At session start:
1. Reload persisted GTC/GTD orders
2. Re-validate each against current session rules
3. Re-enter valid orders into the book (respecting original time priority)

The current engine has no persistence layer and no session lifecycle management.

---

## 5. Priority Recommendations (Revised with Core/Exchange Split)

### Phase 2 — Core Generic Mechanisms

These go into exchange-core. They are universal, have a single correct implementation, and unblock all exchange simulators.

| Priority | Feature | Layer | Effort | Justification |
|----------|---------|-------|--------|---------------|
| **P2-1** | Session state machine framework | **Core** | Medium | Provides `SessionState` enum, `transition()`, phase-aware TIF validation. Exchange defines schedule via CRTP. |
| **P2-2** | Auction uncrossing algorithm | **Core** | High | Standard equilibrium price calc (max volume, min imbalance). Exchange controls when it runs. |
| **P2-3** | Iceberg / display quantity orders | **Core** | Medium | Add `display_qty` to Order, tranche reveal on fill. Exchange sets min display via CRTP hook. |
| **P2-4** | Dynamic price band framework | **Core (extensible)** | Low | Core checks bands; exchange provides `calculate_dynamic_bands()` via CRTP. |
| **P2-5** | Daily price limits + halt state | **Core** | Medium | Core enforces limits and enters halt; exchange sets levels. |
| **P2-6** | Mass cancel / kill switch API | **Core** | Low | `mass_cancel(account)`, `mass_cancel_all()`. Pure engine operation. |
| **P2-7** | Max order size validation | **Core** | Low | Add to EngineConfig. Trivial. |
| **P2-8** | OHLCV statistics accumulator | **Core** | Low | Track open/high/low/close/volume/vwap per session. |
| **P2-9** | Market status callback | **Core** | Low | `on_market_status(state, ts)` — new event type. |
| **P2-10** | LMM priority in matching | **Core (extensible)** | Medium | Core allocates MM percentage; exchange identifies MMs via `is_market_maker()` CRTP hook. |

### Phase 3a — Core Extensible Frameworks

These add CRTP hooks to core that exchange implementations customize.

| Priority | Feature | Layer | Effort | Justification |
|----------|---------|-------|--------|---------------|
| **P3a-1** | Volatility auction framework | **Core (extensible)** | Medium | Core provides `enter_auction()` / `exit_auction()`. Exchange provides trigger thresholds via CRTP. |
| **P3a-2** | Circuit breaker framework | **Core (extensible)** | Medium | Core provides halt states. Exchange defines levels and behavior via `on_circuit_breaker()`. |
| **P3a-3** | Settlement price hooks | **Core (extensible)** | Low | Core provides `calculate_settlement_price()` hook. Exchange implements formula. |
| **P3a-4** | GTC persistence hooks | **Core** | Medium | `serialize_order()` / `restore_order()`. Exchange manages storage. |
| **P3a-5** | Trade bust/adjustment API | **Core** | Medium | `bust_trade(id)` reverses a trade. Core mechanics. |
| **P3a-6** | Order rate tracking | **Core** | Low | Core counts orders per account per interval. Exchange sets threshold. |
| **P3a-7** | Additional TIF types (ATO/ATC/GFA) | **Core** | Low | Add to enum. Phase validity controlled by exchange via `is_order_allowed_in_phase()`. |

### Phase 3b — Exchange Layer (Per-Venue Implementation)

These do NOT go into exchange-core. Built when implementing specific exchange simulators.

| Priority | Feature | Layer | Effort | First Needed By |
|----------|---------|-------|--------|-----------------|
| **P3b-1** | Calendar spread / strategy books | **Exchange** | Very High | CME, Eurex |
| **P3b-2** | Implied-in / implied-out pricing | **Exchange** | Very High | CME, Eurex |
| **P3b-3** | CME matching algorithm variants (6 more) | **Exchange** | High | CME only |
| **P3b-4** | Velocity logic | **Exchange** | Medium | CME only |
| **P3b-5** | Exchange-specific order types (MTL, MIT, OCO, BOC) | **Exchange** | Medium each | Per venue |
| **P3b-6** | Session schedules and holiday calendars | **Exchange** | Low each | Per venue |
| **P3b-7** | Per-order SMP instruction | **Exchange** | Low | CME |
| **P3b-8** | Member/firm hierarchy | **Exchange** | Medium | Per venue |
| **P3b-9** | Position limits (specific values) | **Exchange** | Medium | Per venue |
| **P3b-10** | Margin integration (SPAN/PRISMA/VaR) | **Exchange** | Medium | Per venue |
| **P3b-11** | JPX Itayose specifics | **Exchange** | Low | JPX only |
| **P3b-12** | KRX sidecar mechanism | **Exchange** | Low | KRX only |
| **P3b-13** | HKEX VCM soft halt | **Exchange** | Low | HKEX only |
| **P3b-14** | ASX random auction end | **Exchange** | Low | ASX only |
| **P3b-15** | Implied price dissemination | **Exchange** | Medium | With P3b-1/2 |
| **P3b-16** | NSE multi-tier circuit breakers | **Exchange** | Low | NSE only |

---

## 6. Per-Exchange Notes

### 5.1 CME (Globex)

**Matching complexity:** CME is by far the most complex matching venue. Eight different matching algorithms are used across different product groups:
- **Equity Index (ES, NQ):** FIFO with LMM
- **Treasury Futures:** FIFO with Top Order + LMM
- **Eurodollar/SOFR options:** Allocation (pure pro-rata)
- **Agricultural (grains):** Configurable split 40% FIFO / 60% Pro-Rata
- **Energy:** Threshold Pro-Rata with LMM

**Unique features:**
- **Velocity Logic:** Time-based volatility detection (price movement speed, not just distance)
- **Dynamic Circuit Breakers (DCB):** Halt trading and enter pre-open; duration shrinks near settlement
- **Market-if-Touched (MIT):** Unique order type not found on other exchanges
- **MBO (Market-by-Order) data:** Full order-level depth with anonymous OrderIDs
- **Implied pricing:** Extensive implied-in/implied-out across outright and spread books
- **SMP:** Per-order instruction (cancel aggressive vs. cancel resting); cross-firm SMP via SMP ID

**Simulator accuracy requirement:** High -- CME is the most heavily backtested exchange for US-based futures firms. Algorithm accuracy directly impacts fill simulation quality.

Sources:
- [CME Matching Algorithm Overview](https://www.cmegroup.com/education/matching-algorithm-overview)
- [CME Supported Matching Algorithms](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457218479/Supported+Matching+Algorithms)
- [CME Velocity Logic](https://cmegroupclientsite.atlassian.net/wiki/display/EPICSANDBOX/Velocity+Logic)
- [CME Market and Instrument States](https://cmegroupclientsite.atlassian.net/wiki/display/EPICSANDBOX/Market+and+Instrument+States)
- [CME Self-Match Prevention](https://www.cmegroup.com/globex/trade-on-cme-globex/self-match-faq.html)
- [CME Market by Order (MBO)](https://www.cmegroup.com/articles/faqs/market-by-order-mbo.html)
- [CME Price Limits and Circuit Breakers](https://www.cmegroup.com/education/articles-and-reports/understanding-price-limits-and-circuit-breakers)
- [CME Dynamic Circuit Breakers FAQ](https://www.cmegroup.com/solutions/market-access/globex/trade-on-globex/faq-dynamic-circuit-breakers.html)

---

### 5.2 Eurex

**Platform:** T7 trading system (shared with Xetra for equities).

**Key features:**
- **Volatility Interruption (VI):** When a potential trade price deviates beyond the dynamic price range, trading halts and the instrument enters a volatility auction. There are two ranges:
  - Dynamic range: based on last trade price (triggers intraday VI)
  - Static range: based on last auction price (triggers extended VI)
- **Extended VI:** If the auction uncrossing price is still outside the wider static range, the VI continues until manually ended or the price normalizes.
- **Book-or-Cancel (BOC):** Unique order type that must add liquidity; rejected if it would match.
- **Market order handling:** Market orders that cannot execute within the "Market Order Matching Range" are booked as hidden limit orders at the range boundary.
- **Pro-Rata matching:** Used for fixed-income futures (Bund, Bobl, Schatz). Time allocation used for equity index futures.
- **OCO support:** Not natively supported on T7 for futures (supported via TT synthetic).
- **TIF types:** Includes Good-for-Auction (GFA) which is unique to Eurex.

**Simulator accuracy requirement:** High -- Eurex is the primary European derivatives venue.

Sources:
- [Eurex Trading Phases](https://www.eurex.com/ex-en/trade/trading-hours/trading-phases)
- [Eurex Order Types](https://www.eurex.com/ex-en/trade/order-book-trading/order-types)
- [Eurex Matching Principles](https://www.eurex.com/ex-en/trade/order-book-trading/matching-principles)
- [Eurex Volatility Interruption](https://www.eurex.com/ex-en/support/emergencies-and-safeguards/volatility-interruption-functionality)
- [Eurex Supported Order Types (TT)](https://library.tradingtechnologies.com/user-setup/eur-eurex-supported-order-types.html)

---

### 5.3 ICE (Intercontinental Exchange)

**Key features:**
- **Price-Time priority (FIFO):** ICE uses strict FIFO for all products.
- **Opening Match:** Dedicated period after pre-trading where accumulated limit orders uncross at a single price.
- **Iceberg orders:** Natively supported.
- **Stop Limit:** Supported, but system/implied prices do NOT trigger stops (important for spread instruments).
- **Self-Trade Prevention (STPF):** Three modes -- RRO (cancel resting), RTO (cancel taking), RBO (cancel both). Default is RRO.
- **Pre-Trading Session:** Only limit orders accepted; serves as order accumulation for opening match.
- **Spread trading:** Extensive support for exchange-defined strategies; implied pricing between outright and spread books.

**Unique quirk:** ICE's stop orders are triggered only by outright trade prices, NOT by implied/strategy prices. This is an important behavioral detail for accurate simulation.

Sources:
- [ICE Trading Rules](https://www.ice.com/publicdocs/rulebooks/futures_us/4_Trading.pdf)
- [ICE Supported Order Types (TT)](https://library.tradingtechnologies.com/user-setup/ice-supported-order-types.html)
- [ICE STPF FAQ](https://www.ice.com/publicdocs/futures/IFEU_Self_Trade_Prevention_Functionality_FAQ.pdf)

---

### 5.4 LME (London Metal Exchange)

**Unique market structure:** The LME is architecturally different from all other exchanges on this list due to its hybrid trading model:

1. **LMEselect (Electronic):** Standard electronic CLOB with FIFO matching
2. **The Ring (Open Outcry):** Physical trading floor; five-minute sessions per metal
3. **Telephone Market:** 24-hour inter-office OTC-style trading

**Key features:**
- **Systematic Fixed Price Auction (SFPA):** When orders match on LMEselect, a 30-second auction begins at the matched price. Other participants can join during this window. After 30 seconds, remaining orders are matched by time priority.
- **Prompt date structure:** Unlike standard monthly expiries, LME contracts have daily prompt dates out to 3 months, weekly out to 6 months, and monthly out to 123 months. This creates thousands of tradeable instruments per metal.
- **Iceberg orders:** Supported with configurable replenishment quantity.
- **OCO orders:** Supported on LMEselect.
- **Pre-Open:** Indicative Opening Price (IOP) calculated and disseminated.
- **Trade-at-Settlement (TAS):** Special order type that executes at the official settlement price (determined by Ring trading), not the electronic market price.

**Simulator challenge:** Accurately simulating the LME requires modeling the interaction between electronic and Ring-based price discovery. The SFPA mechanism is unique to LME.

Sources:
- [LME Guide to Market Structure](https://www.lme.com/-/media/Files/About/Regulation/Guide-to-Market-Structure_.pdf)
- [LME The Ring](https://www.lme.com/trading/trading-venues/the-ring)
- [LME Electronic Trading](https://www.lme.com/Trading/Trading-venues/Electronic)

---

### 5.5 ASX (formerly SFE)

**Platform:** ASX Trade 24 for derivatives.

**Key features:**
- **Price-Time priority:** Standard FIFO matching for futures.
- **Iceberg orders:** Supported with automatic child order replenishment.
- **Undisclosed orders:** Price visible, quantity hidden. Lower priority than displayed orders at same price.
- **Centre Point orders (equities):** Execute at midpoint of best bid/offer. (Not typically used for futures.)
- **Random auction endings:** Pre-Open and Pre-Close phases end at a random time within a window to prevent manipulation.
- **Calendar and inter-commodity spreads:** Supported with spread concessions.
- **Custom Market:** Allows custom multi-leg strategies beyond exchange-defined spreads.
- **Implied pricing:** Active for standard calendar spreads.

Sources:
- [ASX Trade Introduction](https://www.asxonline.com/content/dam/asxonline/public/documents/asx-trade-refresh-manuals/asx-trade-introduction-and-business-information.pdf)
- [ASX 24 Trading Mechanisms](https://www.asxenergy.com.au/trading/trading_mechanism)

---

### 5.6 HKEX (HKFE)

**Platform:** HKATS (Hong Kong Futures Automatic Trading System).

**Key features:**
- **Price/Time priority:** Strict FIFO matching.
- **T+1 (After-Hours) Session:** Night trading session for selected index futures (HSI, HHI, etc.).
- **Volatility Control Mechanism (VCM):**
  - Reference price = last traded price 5 minutes ago
  - Trigger: order price deviates >5% from reference
  - Cooling-off period: trading restricted to +/-5% of reference
  - Does NOT apply during T+1 session
  - Does NOT apply in first 15 min of morning/afternoon sessions or last 20 min
- **Closing Auction Session (CAS):** Mandatory closing auction for price-sensitive products.
- **Combo orders:** Multi-leg strategy orders.
- **Internal trades:** Cross orders within the same participant.

**Unique quirk:** VCM is a "soft" circuit breaker -- it restricts the price range rather than halting trading entirely.

Sources:
- [HKEX Trading Mechanism](https://www.hkex.com.hk/Services/Trading/Securities/Overview/Trading-Mechanism?sc_lang=en)
- [HKEX VCM Overview](https://www.hkex.com.hk/-/media/HKEX-Market/Services/Trading/Derivatives/Trading-Mechanism/Volatility-Control-Mechanism-(VCM)/VCM-CAS-Overview-Eng.pdf)
- [HKEX Overview (TT)](https://library.tradingtechnologies.com/user-setup/hke-hkex-overview.html)

---

### 5.7 KRX (Korea Exchange)

**Key features:**
- **Price/Time priority:** Standard FIFO matching for KOSPI/KOSDAQ futures.
- **Call auction:** Opening and closing call auctions.
- **Two-tier volatility controls:**
  1. **Sidecar:** Suspends programme trading for 5 minutes when KOSPI200 futures move 5%+ for 1 minute. Does not halt manual trading.
  2. **Circuit Breaker:** Full market halt for 20 minutes when KOSPI declines 8%+ for 1 minute. Three tiers: 8%, 15%, 20%.
- **Daily price limits:** +/- 30% for KOSPI/KOSDAQ stocks (futures limits vary by product).
- **Off-hours session:** 16:00-18:00 for single-price auction trading.
- **Stop-limit orders and mid-point orders** are available.

**Unique features:**
- Sidecar mechanism is unique to KRX -- it selectively suspends algorithmic/programme trading while allowing manual orders.
- Circuit breakers have been frequently activated in recent market events (2025-2026 volatility).

Sources:
- [KRX Global](https://global.krx.co.kr/)
- [Korea Exchange Wikipedia](https://en.wikipedia.org/wiki/Korea_Exchange)

---

### 5.8 JPX (Osaka Exchange)

**Platform:** J-GATE 3.0 (launched September 2021).

**Key features:**
- **Zaraba method:** Continuous trading with price-time priority (standard FIFO).
- **Itayose method:** Auction matching for opening, closing, and post-halt resumption. Equilibrium price maximizes traded volume and is closest to last contract price.
- **Immediately Executable Price Range (DCB):**
  - Reference price +/- percentage (e.g., 0.8% for Nikkei 225 futures)
  - If trade would occur outside range, trading halts for minimum 30 seconds (15s for index options)
  - If price still outside range after halt, reference price updates to boundary and halt continues
- **Night Session:** Extended trading hours (17:00-06:00 next day for major index futures).
- **Session-specific TIFs:** "On Close - Morning", "On Close - Afternoon", "On Close - Evening" -- three distinct closing auction opportunities.
- **Order types:** Limit and Market only natively on J-GATE. Stop orders are not natively supported (handled by broker systems).
- **Price limits / Circuit Breaker:** Absolute daily price limits set per product; separate from DCB.

**Unique quirk:** JPX does not natively support stop orders on J-GATE. This simplifies the simulator but means stop order simulation must be handled externally.

Sources:
- [JPX Order Types](https://www.jpx.co.jp/english/derivatives/rules/order-types/index.html)
- [JPX Trading Methods](https://www.jpx.co.jp/english/derivatives/rules/trading-methods/index.html)
- [JPX Immediately Executable Price Range](https://www.jpx.co.jp/english/derivatives/rules/price-range/)
- [JPX Price Limits / Circuit Breaker](https://www.jpx.co.jp/english/derivatives/rules/price-limit-cb/index.html)
- [JPX Overview (TT)](https://library.tradingtechnologies.com/user-setup/jpx-overview.html)

---

### 5.9 SGX (Singapore Exchange)

**Key features:**
- **Price-Time priority:** Standard FIFO matching.
- **Market-to-Limit (MTL):** Natively supported. A market order that converts unfilled remainder to limit at last fill price.
- **Opening Routine (30 min):** Pre-Open phase + Non-Cancel phase. Random end to Pre-Open prevents manipulation.
- **Equilibrium Price Algorithm:** Max tradable volume, then min imbalance, then closest to reference.
- **TIF types:** Includes On Open and On Close natively.
- **Implied pricing:** Supported for calendar spreads.
- **CME-SGX Mutual Offset System (MOS):** Positions in certain contracts can be transferred between CME and SGX for clearing. Not relevant for matching simulation but important for position management.
- **After-hours trading:** Product-specific extended hours.

Sources:
- [SGX Market Phases and Algorithm](https://rulebook.sgx.com/rulebook/practice-note-821-application-market-phases-and-algorithm)
- [SGX Trading Hours and Phases](https://rulebook.sgx.com/rulebook/regulatory-notice-821-trading-hours-market-phases-application-market-phases-and-principles)
- [SGX Supported Order Types (TT)](https://library.tradingtechnologies.com/user-setup/sgx-supported-order-types.html)

---

### 5.10 NSE (National Stock Exchange of India)

**Key features:**
- **Price/Time priority:** Standard FIFO matching.
- **F&O Pre-Open Session (new, Dec 2025):** 15-minute call auction for current-month index and stock futures.
  - Order Entry: 9:00-9:08 (random close between 7th-8th minute)
  - Matching: 9:08-9:12 (equilibrium price determination)
  - Buffer: 9:12-9:15 (transition to continuous trading)
  - Allowed: Limit and Market orders only
  - Prohibited: Stop-loss, SL-M, IOC orders
- **Circuit Breakers:** Index-based, market-wide. Three tiers:
  - 10% decline: 45-min halt (before 1pm), 15-min halt (1-2:30pm), no halt (after 2:30pm)
  - 15% decline: 1hr 45min halt (before 1pm), 45-min halt (1-2pm), remainder of day (after 2pm)
  - 20% decline: remainder of day
- **Daily price limits:** For individual stock futures: 20% of base price. Circuit filter limits at 10%/15%/20%.
- **Order types:** Limit, Market, Stop-Loss (trigger price + limit), Stop-Loss Market (trigger price, converts to market).
- **Iceberg ("Disclosed Quantity") orders:** Supported. Minimum disclosed quantity = 10% of total.

**Unique features:**
- Pre-Open session for F&O is very new (December 2025) and specific to current-month futures only.
- Circuit breakers are calculated daily from previous day's closing Nifty level.
- Relatively limited order type complexity compared to CME/Eurex.

Sources:
- [NSE F&O Pre-Open Session](https://www.nseindia.com/static/products-services/equity-derivatives-pre-open-session)
- [NSE Circuit Breakers](https://www.nseindia.com/products-services/equity-market-circuit-breakers)
- [NSE F&O Pre-Open Details (Enrich Money)](https://enrichmoney.in/blog-article/nse-fno-pre-open-session-december-2025)

---

## 7. Implementation Architecture Recommendations

### 6.1 Session State Machine Design

```
+------------------------------------------------------------------+
|                    ExchangeSession<Derived>                        |
|                                                                    |
|  TradingPhase current_phase_                                       |
|  MatchingEngine& engine_                                           |
|  AuctionBook auction_book_          // separate book for auction   |
|  ScheduledTransition transitions_[] // time-based phase changes    |
|                                                                    |
|  void transition_to(TradingPhase)                                  |
|  void on_timer(Timestamp)           // scheduled transitions       |
|  void trigger_volatility_halt()     // event-driven transition     |
|  bool accept_order(OrderRequest&)   // phase-aware validation      |
|  void uncross()                     // auction -> continuous        |
+------------------------------------------------------------------+

enum class TradingPhase : uint8_t {
    StartOfDay,
    PreOpen,          // orders accepted, no matching
    NonCancel,        // no order entry (SGX, ASX)
    OpeningAuction,   // uncrossing
    Continuous,       // normal matching
    VolatilityAuction,// halt -> auction
    PreClose,         // orders accepted, no matching
    ClosingAuction,   // uncrossing
    PostTrading,      // limited activity
    AfterHours,       // T+1 session (HKEX, KRX, JPX)
    Halt,             // circuit breaker halt
    EndOfDay
};
```

### 6.2 Order Extension for Iceberg

```cpp
struct Order {
    // ... existing fields ...
    Quantity display_quantity{0};    // 0 = not an iceberg
    Quantity hidden_quantity{0};     // remaining hidden portion
    bool is_iceberg() const { return display_quantity > 0; }
};
```

### 6.3 Multi-Book Architecture for Spreads

```
  +-------------+     +-------------+     +-------------+
  | Outright     |     | Outright     |     | Spread       |
  | Book (Jun)   |     | Book (Sep)   |     | Book (Jun-Sep)|
  +------+------+     +------+------+     +------+-------+
         |                    |                    |
         +--------------------+--------------------+
                              |
                    +---------v----------+
                    | Implied Pricing    |
                    | Engine             |
                    +--------------------+
```

---

## 8. Effort Estimates

| Phase | Feature Count | Estimated Lines of Code | Calendar Time |
|-------|--------------|-------------------------|---------------|
| Phase 2 (P2-1 through P2-10) | 10 features | ~4,000-6,000 | 4-6 weeks |
| Phase 3 (P3-1 through P3-16) | 16 features | ~8,000-12,000 | 8-12 weeks |
| **Total to full coverage** | **26 features** | **~12,000-18,000** | **12-18 weeks** |

The implied/spread trading system (P3-1, P3-2, P3-15) represents approximately 40% of the Phase 3 effort.

---

*This gap analysis was compiled from exchange documentation, regulatory filings, and trading technology references. Exchange rules and features are subject to change; verify against current exchange circulars before implementation.*
