# ICE Exchange Protocol Analysis

**Date:** 2026-03-25
**Scope:** ICE Futures Europe (IFEU), ICE Futures US (IFUS), ICE Endex (NDEX)
**Purpose:** Research for building an ICE exchange simulator on top of exchange-core

---

## 1. ICE iMpact Market Data Protocol

### 1.1 Overview

ICE iMpact is a proprietary binary multicast protocol used for real-time market data dissemination across all ICE futures venues. The current specification version is **1.1.50 (July 2024)**. Data is published over **UDP/IP multicast** using a push model — data is sent immediately when available.

Messages use a compact binary format: fixed-length binary and ASCII fields, with each message prefixed by a **2-byte length** and **2-byte message type code**.

### 1.2 Multicast Channel Architecture

ICE organizes multicast channels by **book type** and **market segment**. Each channel carries one of three depths:

| Channel Type | Description |
|---|---|
| **Full Order Depth (FOD)** | Complete MBO (market-by-order) book; every individual order visible |
| **Price Level (PL / Top 5)** | Aggregated MBP (market-by-price) top 5 levels |
| **Top of Book (TOB)** | Best bid/offer only (L1) |

Each market group has two feeds: a **live incremental feed** (UDP multicast) and a **snapshot feed** (TCP or UDP snapshot channel). Recovery happens via a separate **TCP Historical Replay service**.

### 1.3 Message Types

ICE iMpact uses single-character or short type codes. Key message categories:

| Message Category | Types | Direction |
|---|---|---|
| **Order book incremental** | Add/Modify Order (`E`), Order Withdrawal/Cancel (`F`) | Exchange → Subscriber |
| **Trade/Deal** | Deal (`T`) | Exchange → Subscriber |
| **Market Status** | Market Status (`M`) | Exchange → Subscriber |
| **Snapshot** | Market Snapshot Order (`D`), Market Snapshot Price Level | Exchange → Subscriber |
| **Bundle markers** | Bundle Start (`S`), Bundle End (`E`) | Exchange → Subscriber |
| **Security definitions** | Instrument definitions, security status | Exchange → Subscriber |

**Snapshot vs Incremental distinction:** The Snapshot Order message (`D`) is sent during the initial snapshot synchronization phase; the real-time Add/Modify Order message (`E`) is the incremental update used during live trading.

### 1.4 Sequence Numbering

Each **multicast block** (not each message) carries a header containing:

- **Session Number**: identifies the current active session (resets on reconnect)
- **Block Sequence Number**: monotonically increasing per channel
- **Number of Messages**: count of messages in this block
- **Sent Time**: millisecond or nanosecond timestamp

The sequence number is assigned once per block, not per message. Clients derive per-message sequence numbers from the block header. This design saves bandwidth at the cost of client-side reconstruction logic.

On session recovery (e.g., exchange restart), ICE sends a **sequence number reset message**. Retransmitted messages retain their original sequence numbers; clients check `SeqNum` to resolve duplicates.

### 1.5 Snapshot/Incremental Recovery Model

```
                    Start
                      |
              +--------v--------+
              |  Subscribe to   |  Subscribe to live incremental
              |  live channels  |  feed (buffer all messages)
              +--------+--------+
                       |
              +--------v--------+
              |  Connect to     |  TCP connection to snapshot
              |  snapshot       |  service for current state
              +--------+--------+
                       |
              +--------v--------+
              |  Apply snapshot |  Reconstruct full book from
              |  to local book  |  snapshot messages
              +--------+--------+
                       |
              +--------v--------+
              |  Apply buffered |  Apply all buffered incremental
              |  incrementals   |  messages with seqnum > snapshot
              +--------+--------+
                       |
              +--------v--------+
              |  Live (steady   |
              |  state)         |
              +-----------------+
```

If a gap is detected in the incremental sequence (missing block), the client can either:
1. Re-request from the **TCP Historical Replay** service for specific sequence ranges
2. Re-subscribe to the snapshot channel to full re-sync

### 1.6 Feed Segments (ICE Futures Europe)

ICE IFEU iMpact organizes products into multicast groups by asset class:
- **Oil and Refined Products** (Brent, WTI, Gasoil, heating oil)
- **Natural Gas** (UK NBP, Henry Hub, European hubs)
- **Power** (UK, German, Nordic power)
- **Emissions** (EUA, CER carbon allowances)
- **Soft Commodities** (Cocoa, Robusta Coffee, White Sugar, Feed Wheat)
- **Financial** (FTSE, MSCI, STIR: Euribor, SONIA, SOFR)

---

## 2. ICE Order Entry Protocol

### 2.1 Protocol Options

ICE operates two parallel order entry protocols:

| Protocol | Transport | Encoding | Status |
|---|---|---|---|
| **ICE FIX Order Server (FIX OS)** | TCP | FIX 4.2 text | Legacy; still active |
| **ICE Binary Order Entry (ICE BOE)** | TCP | SBE binary | New; go-live Oct 2025 (Basildon), Feb 2026 (Chicago) |

#### FIX OS (Legacy)

ICE FIX OS implements a **subset of FIX 4.2** with ICE-specific extensions. Standard FIX message types are used for order lifecycle:

| FIX MsgType | Message | Description |
|---|---|---|
| `D` | New Order Single | Submit new order |
| `G` | Order Cancel/Replace Request | Modify existing order |
| `F` | Order Cancel Request | Cancel existing order |
| `V` | Market Data Request | Subscribe to market data |
| `8` | Execution Report | Order acknowledgement, fill, cancel |
| `9` | Order Cancel Reject | Cancel rejection |

ICE-specific tags:
- **Tag 9821** — `SelfMatchPreventionID`: SMP tag value (up to 8 tags per firm)
- **Tag 9822** — `SmpInstruction`: cancel resting (`0`), cancel aggressor (`1`), cancel both (`2`)

#### ICE BOE (New Binary Protocol)

ICE BOE uses **Simple Binary Encoding (SBE)** over TCP. Architecture:

```
Client
  |
  | 1. IPRequest (BUS service)
  v
+--------------------+
| Binary Utility     |  Returns: dynamic IP, port, per-session token
| Service (BUS)      |
+--------------------+
  |
  | 2. TCP connect with assigned credentials
  v
+--------------------+
| Binary Order       |  One of N independent BGWs
| Gateway (BGW)      |  (ICE Chicago, Basildon, ENDEX Chicago silos)
+--------------------+
  |
  | SBE-encoded messages
  v
Exchange Matching Engine
```

Key BOE message types (analogous to FIX OS):
- **NewOrder** — submit a new order
- **OrderCancelRequest** — cancel by order ID
- **OrderCancelReplaceRequest** — modify price/quantity
- **MassOrderActionRequest** — mass cancel
- **ExecutionReport** — order accepted/filled/cancelled/rejected
- **OrderCancelReject** — cancel request rejected

Session lifecycle:
1. Obtain BGW address from BUS via `IPRequest`
2. Open TCP connection to assigned BGW
3. Exchange logon/logoff messages (FIXP-style session control)
4. Submit orders; receive execution reports synchronously on same TCP connection
5. Sequence numbers maintained per session; gaps trigger retransmission

---

## 3. Session Lifecycle

### 3.1 Trading Phases

ICE futures markets pass through the following phases each trading day:

```
+------------------+
|    Closed        |  No orders accepted; system maintenance window
+--------+---------+
         |
+--------v---------+
|    Pre-Open      |  Limit orders accepted; no matching occurs
|                  |  Uncrossing algorithm runs periodically
|                  |  Indicative opening price/volume published
|                  |  Market orders NOT permitted
+--------+---------+
         |
+--------v---------+
|  Continuous      |  Full matching; all order types
|  Trading         |  IPL (Interval Price Limits) active
+--------+---------+
         |
+--------v---------+
|  Settlement      |  Designated 2-5 minute settlement period
|  Period          |  VWAP of trades during window = settlement price
|                  |  Trading continues during settlement period
+--------+---------+
         |
+--------v---------+
|  After-Hours     |  ICE energy products trade nearly 23 hours/day
|  (T+1)           |  with short daily maintenance break
+--------+---------+
         |
+--------v---------+
|   Closed         |  End of day; GTC orders preserved
+------------------+
```

There is no separate closing auction on most ICE products. Settlement price is determined by **VWAP during a designated window**, not by an uncrossing auction.

### 3.2 Product-Specific Session Hours

| Product Group | Pre-Open | Continuous | Settlement Period | Notes |
|---|---|---|---|---|
| Brent Crude (IFEU) | 23:45 Sun, London | 00:00-22:00 London | 19:28-19:30 London | ~22hr session; 2hr maintenance break |
| UK Natural Gas (IFEU) | — | 06:00-18:00 London | ~17:30 London | European gas hours |
| Power/Utilities (ENDEX) | — | 07:00-17:00 CET | Product-specific | Day-ahead and spot markets |
| London Cocoa (IFEU) | 09:15 London | 09:30-16:55 London | 16:48-16:50 London | Soft commodities: shorter hours |
| Robusta Coffee (IFEU) | 08:45 London | 09:00-17:30 London | 17:23-17:25 London | |
| White Sugar (IFEU) | 08:30 London | 08:45-17:00 London | 16:53-16:55 London | |
| Euribor (IFEU STIR) | 12:45 NY / 01:00 London | 01:00-21:00 London | 11:00 London (last trade) | Interest rate; global hours |
| MSCI/FTSE (IFEU) | — | 08:00-20:30 London | — | |

### 3.3 Halt and Circuit Breaker

When an **Interval Price Limit (IPL)** is triggered, the exchange enters a brief **hold period**:
- Orders that would trade outside the IPL band are rejected
- Trading within the band (or in the opposite direction) continues
- After the hold period, a new IPL is calculated and normal trading resumes

Hold periods are product-specific:
- **Energy futures**: 5 seconds
- **Soft commodities**: 15 seconds
- General range: 5-30 seconds depending on product

After the hold period, the IPL **recalculates every 15 seconds** (5 seconds for Index and US Dollar Index). This is a rolling dynamic band, not a static daily limit.

There is no volatility auction on ICE (unlike CME or Eurex). ICE simply holds and resumes.

---

## 4. Order Types

### 4.1 Standard Order Types

| Order Type | ICE Support | Notes |
|---|---|---|
| Limit | Yes | Full TIF flexibility |
| Market | Yes | Cannot be GTDate, GTC; only Day/IOC/FOK |
| Stop Limit | Yes | Cannot use IOC or FOK |
| Stop Market | Yes | Cannot use IOC, FOK, or GTDate |
| Iceberg (Reserve) | Yes | Only Day and GTC; prohibited for IOC, FOK, GTDate |

### 4.2 Time in Force

| TIF | ICE Support | Notes |
|---|---|---|
| Day | Yes | Expires at end of trading session |
| GTC (Good Till Cancelled) | Yes | Persists across sessions |
| IOC (Immediate or Cancel) | Yes | Fill immediately or cancel remainder |
| FOK (Fill or Kill) | Yes | Fill fully immediately or cancel entirely |
| GTDate (Good Till Date) | Yes | Expires at specified date/time |

**Phase restrictions (Pre-Open):**
- Market orders are rejected during pre-open
- IOC and FOK are rejected during pre-open (they cannot participate in the uncrossing algorithm)
- GTDate and Day orders can be submitted during pre-open

**TAS/TIC/BIC/TAPS products** (Trade-at-Settlement variants): only Day orders are accepted.

### 4.3 ICE-Specific Order Types

| Order Type | Description | Venue |
|---|---|---|
| **Iceberg (native)** | Exchange-managed hidden quantity; each disclosed tranche is a separate order in the book | IFEU, IFUS |
| **EFP (Exchange of Futures for Physical)** | Bilateral privately-negotiated exchange-for-physical transaction | IFEU, IFUS |
| **EFS (Exchange of Futures for Swap)** | Bilateral privately-negotiated futures-for-swap | IFEU, IFUS |
| **Block Trade** | Large prearranged OTC trade above minimum block size, reported to exchange | IFEU, IFUS |
| **Cross Order** | Prearranged trade within same firm or with named counterparty; requires a 5-second (futures) or 15-second (options) delay before submission | IFEU, IFUS |
| **Spread / Combination** | Calendar spreads, crack spreads, volatility spreads — exchange-defined and user-defined strategies | IFEU, IFUS |

**Iceberg timing rule:** When iceberg order functionality is used, buy and sell iceberg orders that could cross must be separated by the requisite time delay (5 seconds for futures, 15 seconds for options).

---

## 5. Matching Algorithms

ICE uses a clear product-category split:

| Product Category | Algorithm | Description |
|---|---|---|
| **Energy** (Brent, WTI, Gasoil, Natural Gas, Power) | **FIFO (Price-Time Priority)** | Pure price-then-time priority; no pro-rata component |
| **Agricultural/Softs** (Cocoa, Robusta Coffee, White Sugar, Feed Wheat, Cotton) | **FIFO (Price-Time Priority)** | Same as energy; FIFO applies to soft commodities too |
| **Short-Term Interest Rates (STIRs)** (Euribor, SONIA, SOFR, Eurodollar) | **GTBPR (Gradual Time-Based Pro Rata)** | Hybrid algorithm with priority component + time-weighted pro-rata |
| **Equity Index** (FTSE, MSCI, Russell) | **FIFO (Price-Time Priority)** | Price-time priority |

### 5.1 GTBPR (Gradual Time-Based Pro Rata) — STIR Products

The Gradual Time-Based Pro Rata algorithm used for STIRs:

```
Step 1: Priority Order Selection
  - Identify the "priority order" at the best price level
  - Priority order must exceed minimum size (collar)
  - Priority order is filled first, up to maximum priority allocation (cap)

Step 2: Pro-Rata with Time Weighting
  - All remaining resting orders (including the priority order, if not fully filled)
    participate in pro-rata allocation
  - Allocation weight = order_quantity * time_weight
  - Time weight increases for older orders (more time elapsed = higher weight)
  - Final allocation = (weighted_size / total_weighted_size) * remaining_incoming_qty

Step 3: Remainder allocation
  - Any unallocated quantity due to rounding goes to the oldest order
```

**Key parameters (set per product, not publicly disclosed):**
- Collar (minimum size for priority consideration)
- Cap (maximum priority allocation)
- Time weight decay factor

### 5.2 FIFO Algorithm Detail

ICE FIFO is strict price-time priority:
1. All orders at the best price are filled before moving to the next price level
2. Within a price level, orders are filled in strict submission time order
3. No minimum quantity allocation, no market maker priority
4. Iceberg orders: only the visible (display) quantity participates in queue; hidden quantity re-enters at back of queue when a tranche is exhausted

### 5.3 Implied Trading

ICE supports implied matching for energy and some other products:

```
Implied IN:  Spread order book + outright order = implied spread price
             (two outright orders → implies a spread)

Implied OUT: Spread order + one outright = implied outright price
             (spread order + one leg → implies the other leg outright)
```

The matching engine derives implied prices for markets within the same strip type (month, quarter, season, or cal). The implied range is configurable per product.

---

## 6. Self-Match Prevention (SMP)

### 6.1 Mechanism

ICE SMP is **tag-based** (not purely account-based), using the FIX tag 9821 / BOE field `SelfMatchPreventionID`:

- Each firm can register **up to 8 different SMP IDs**
- An order is flagged for SMP by including an SMP ID in the order message
- When a new order would match against a resting order from the same firm with the same SMP ID, the SMP action is triggered
- **SMP is optional per order**: orders without an SMP ID participate in normal matching

### 6.2 SMP Actions (Tag 9822 / BOE `SmpInstruction`)

| Value | Action | Description |
|---|---|---|
| `0` (default) | **Cancel Resting** (RTO — Resting Trade Only) | Remove the resting order from the book; incoming order proceeds |
| `1` | **Cancel Aggressor** (ATO — Aggressing Trade Only) | Reject the incoming order; resting order stays |
| `2` | **Cancel Both** (CABO — Cancel All Both Orders) | Remove both resting and incoming orders |

**Default behavior:** If an SMP ID is present but no `SmpInstruction` is specified, the **resting order is cancelled** (RTO).

### 6.3 Scope

SMP applies to **futures contracts** on ICE exchanges. Options SMP behavior may differ. The exchange validates SMP ID registration; firms must register SMP IDs with ICE before use.

---

## 7. Dynamic Circuit Breakers (Interval Price Limits + No Cancellation Ranges)

ICE operates **three layers** of price protection:

### 7.1 Reasonability Limits (RL)

The first line of defense — a **hard pre-trade filter**:
- RL is a price band above/below an exchange-set **anchor price**
- Orders with bids above the RL upper bound, or offers below the RL lower bound, are **rejected outright** by the ETS (Electronic Trading System)
- This prevents gross "fat finger" errors from ever entering the order book
- RL width is typically a multiple of the NCR width

### 7.2 No Cancellation Range (NCR)

The post-trade error policy boundary:
- NCR is a band above/below an anchor price within which executed trades **will not be cancelled or price-adjusted** under normal conditions
- Trades inside the NCR are considered valid even if one party claims an error
- Trades outside the NCR may be cancelled or price-adjusted by Market Supervision
- In volatile markets, Market Supervision can expand the NCR to **2x the standard level**
- If adjustment is made, the adjusted price = fair value ± NCR width

### 7.3 Interval Price Limits (IPL) — Dynamic Circuit Breaker

The in-session volatility control:

```
Normal Trading
      |
      | Order would trade OUTSIDE the current IPL band
      |
+-----v--------+
| HOLD PERIOD  |  (5 seconds energy; 15 seconds softs; 5-30 sec general range)
|              |  Orders that would trade inside the band or in the
|              |  opposite direction continue to be accepted
+-----+--------+
      |
      | Hold period expires### 9.4 ICE vs CME Gap Comparison

ICE is notably **simpler** than CME in several dimensions:

| Dimension | CME | ICE |
|---|---|---|
| Matching algorithms | 8+ (FIFO, ProRata, LMM, TPR, Configurable, etc.) | 2 (FIFO + GTBPR) |
| Closing mechanism | Opening + closing auctions | VWAP settlement window (no auction) |
| Circuit breaker | Velocity Logic + CME-specific price banding | IPL hold-and-resume (simpler) |
| Market maker priority | LMM (Liquidity-Making Maker) tier in some algorithms | None |
| Protocol | iLink3 SBE (complex, proprietary) | FIX 4.2 (legacy) + new SBE BOE |
| Implied matching | Complex multi-leg implied + inter-commodity | Calendar spreads + IN/OUT implied only |

This means an ICE simulator is achievable with significantly less effort than CME — the core algorithms are already present, and the main work is the protocol layer.
      |
+-----v--------+
| Recalculate  |  New IPL band anchored to last trade or best bid/offer
| IPL          |  Recalculates every 15 seconds (5 sec for index/USD)
+-----+--------+
      |
      | Normal trading resumes
```

IPL bands are **fully dynamic**: they track the market in real time and widen during volatile conditions. There is no single fixed daily limit (unlike some Asian exchanges). Market Supervision can manually intervene to adjust or disable IPL during extreme conditions.

### 7.4 Summary Comparison

| Mechanism | Purpose | Scope | Action |
|---|---|---|---|
| Reasonability Limit | Fat-finger prevention | Pre-trade (order entry) | Reject order |
| No Cancellation Range | Post-trade error policy | Post-trade | Determine if trade stands |
| Interval Price Limit | Intraday volatility control | In-session matching | Brief hold, then resume |

---

## 8. Product Types

### 8.1 Energy (ICE Futures Europe + ICE Endex)

| Product | Venue | Settlement | Session |
|---|---|---|---|
| Brent Crude Futures | IFEU | Cash (ICE Brent Index) | ~22 hr/day; 19:28-19:30 London settlement |
| WTI Crude Futures | IFEU | Physical/EFP | ~22 hr/day; linked to NYMEX WTI |
| Gasoil Futures | IFEU | Physical delivery | Settlement by VWAP |
| UK Natural Gas (NBP) | IFEU | Cash | 06:00-18:00 London |
| Henry Hub Natural Gas | IFUS | Cash | US hours |
| UK Power (EFA blocks) | ENDEX | Physical / financial | Day-ahead, balance-of-month |
| European Natural Gas Hubs (TTF, ZEE, etc.) | ENDEX | Cash | 07:00-17:00 CET |
| Coal (API 2, API 4) | IFEU | Cash | London hours |
| Carbon (EUA) | IFEU | Physical | 07:00-17:00 CET |

### 8.2 Agricultural/Softs (ICE Futures Europe + ICE Futures US)

| Product | Venue | Settlement | Session |
|---|---|---|---|
| London Cocoa | IFEU | Physical (UK warehouse) | 09:30-16:55 London; settlement 16:48-16:50 |
| Robusta Coffee | IFEU | Physical | 09:00-17:30 London; settlement 17:23-17:25 |
| White Sugar (No. 5) | IFEU | Physical | 08:45-17:00 London; settlement 16:53-16:55 |
| Feed Wheat | IFEU | Physical | London hours |
| Arabica Coffee (No. 2) | IFUS | Physical | US hours |
| Raw Sugar (No. 11) | IFUS | Physical | US hours |
| Cotton (No. 2) | IFUS | Physical | US hours |
| FCOJ (Frozen OJ) | IFUS | Physical | US hours |
| Cocoa (NY) | IFUS | Physical | US hours |

### 8.3 Financial (ICE Futures Europe)

| Product | Venue | Algorithm | Session |
|---|---|---|---|
| Euribor (3M) | IFEU | GTBPR | 01:00-21:00 London |
| SONIA (3M) | IFEU | GTBPR | London hours |
| SOFR futures | IFEU/IFUS | GTBPR | |
| FTSE 100 Index Future | IFEU | FIFO | 01:00-21:00 London |
| MSCI World, EAFE, EM | IFEU | FIFO | Extended hours |
| MSCI Europe | IFEU | FIFO | European hours |
| Russell 2000 | IFEUS | FIFO | US hours |
| US Dollar Index | IFUS | FIFO | 20:00-20:00 NY |

---

## 9. Exchange-Core Gap Analysis

### 9.1 Gap Classification

**Legend:**
- **Core gap**: Must be added to exchange-core (affects all exchanges)
- **Exchange-layer**: Can be implemented via CRTP/IceExchange-style class
- **Already supported**: Feature exists in exchange-core

### 9.2 Feature Matrix — ICE vs exchange-core

| Feature | ICE Requires | exchange-core Current | Classification | Notes |
|---|---|---|---|---|
| **FIFO matching** | Yes | Yes (FifoMatch) | Already supported | Primary algorithm for energy, softs, financial |
| **Pro-rata matching** | Yes (STIRs) | Yes (ProRataMatch) | Already supported | But GTBPR is a hybrid; current ProRataMatch is pure pro-rata — GTBPR needs extension |
| **GTBPR matching** | Yes (STIRs) | No — ProRataMatch is pure pro-rata | Core gap | GTBPR = priority + time-weighted pro-rata; needs new MatchAlgo implementation |
| **Limit orders** | Yes | Yes | Already supported | |
| **Market orders** | Yes | Yes | Already supported | |
| **Stop orders** | Yes | Yes | Already supported | |
| **Stop Limit orders** | Yes | Yes | Already supported | |
| **Iceberg/Reserve orders** | Yes | Yes (display_qty field) | Already supported | Core added iceberg in Phase 2 |
| **IOC** | Yes | Yes | Already supported | |
| **FOK** | Yes | Yes | Already supported | |
| **GTC** | Yes | Yes | Already supported | |
| **GTD** | Yes | Yes | Already supported | |
| **Day** | Yes | Yes | Already supported | |
| **Session state machine** | Yes | Yes (Phase 2) | Already supported | SessionState enum + transitions |
| **Pre-open phase** | Yes | Yes | Already supported | PreOpen state exists |
| **Phase-based order restrictions** | Yes | Yes | Already supported | is_order_allowed_in_phase CRTP hook |
| **Dynamic price bands** | Yes (IPL) | Yes (calculate_dynamic_bands) | Already supported | ICE IPL = different recalculation logic; exchange-layer |
| **No Cancellation Range** | Yes | Not directly | Exchange-layer | NCR is post-trade policy, not pre-trade filter; can be modeled as static price band for pre-trade (RL) |
| **Reasonability Limits** | Yes | Partially (price_band_low/high) | Already supported | RL is a static pre-trade band; existing price_band fields serve this |
| **SMP (tag-based)** | Yes | Partially | Exchange-layer | Core has SMP via is_self_match + get_smp_action; ICE adds multi-tag logic |
| **SMP cancel resting** | Yes | No (only CancelNewest/Oldest) | Exchange-layer | Add SmpAction::CancelResting semantics mapping |
| **SMP cancel aggressor** | Yes | No (CancelNewest maps to this for aggressive orders) | Already supported | CancelNewest = cancel aggressor in practice |
| **SMP cancel both** | Yes | Yes (SmpAction::CancelBoth) | Already supported | |
| **Mass cancel** | Yes | Yes (Phase 2) | Already supported | |
| **Max order size** | Yes | Yes (max_order_size in EngineConfig) | Already supported | |
| **Closing auction** | No | Yes | N/A — not needed | ICE uses VWAP settlement, not closing auction |
| **Volatility auction** | No | Yes | N/A — not needed | ICE uses IPL hold+resume, not auction |
| **Market maker priority** | No | Partial hook | N/A — not needed | ICE FIFO and GTBPR have no separate MM priority tier |
| **Implied/spread trading** | Yes | No | Exchange-layer | Implied IN/OUT matching engine; complex — exchange-layer |
| **Calendar spreads** | Yes | No | Exchange-layer | Spread instrument definitions + implied matching |
| **L1 market data** | Yes | Yes | Already supported | |
| **L2 (MBP) market data** | Yes | Yes | Already supported | |
| **L3 (MBO) market data** | Yes | Yes | Already supported | Full Order Depth is MBO |
| **OHLCV statistics** | Yes | Yes (ohlcv.h) | Already supported | |
| **Settlement price VWAP** | Yes | Partial (hook) | Exchange-layer | calculate_settlement_price CRTP hook; ICE uses VWAP in window |
| **GTC cross-session persistence** | Yes | Yes (hooks) | Already supported | |
| **Circuit breaker / halt** | Yes (IPL) | Yes (Halt state) | Already supported | Exchange-layer configures IPL parameters |
| **Trade bust API** | Yes | Not yet | Core gap | Required by all 10 exchanges; not yet in core |
| **Order rate throttle** | Yes | Not yet | Core gap | All exchanges require this |
| **Position limits hooks** | Yes | Not yet | Core gap | All exchanges require this |

### 9.3 ICE-Specific Gaps Summary

**Core gaps (affecting all exchanges, not yet implemented):**
1. **Trade bust/adjustment API** — `bust_trade(trade_id)` to reverse executed trades; required at all ICE venues
2. **Order rate throttle** — per-account order rate counting with configurable thresholds
3. **Position limit hooks** — pre-trade position check before order acceptance

**Exchange-layer gaps (ICE-specific, implement via IceExchange CRTP):**
1. **GTBPR matching algorithm** — hybrid priority + time-weighted pro-rata for STIRs; can be added as a new `MatchAlgo` type
2. **ICE IPL (dynamic circuit breaker)** — different recalculation timing than CME velocity logic; implement in `calculate_dynamic_bands` + `on_circuit_breaker` hooks
3. **Multi-tag SMP** — multiple SMP IDs per firm; ICE's tag 9821/9822 semantics; implement via `is_self_match` CRTP hook checking SMP ID fields
4. **VWAP settlement** — settlement price = VWAP during designated 2-5 min window; implement in `calculate_settlement_price`
5. **Implied/spread matching** — ICE implied IN/OUT for energy strips; complex separate engine; out of scope for initial ICE simulator phase
6. **ICE session schedule** — product-specific trading hours with per-product settlement windows

**Already supported:**
- All basic order types (Limit, Market, Stop, Stop Limit, Iceberg)
- All TIF types (DAY, GTC, GTD, IOC, FOK)
- Session state machine (PreOpen, Continuous, Halt, Closed)
- Phase-based order restrictions
- FIFO and ProRata matching algorithms
- Dynamic price bands framework
- SMP (cancel resting, cancel aggressor, cancel both)
- Mass cancel
- L1/L2/L3 market data
- OHLCV statistics
- Max order size validation

### 9.4 ICE vs CME Gap Comparison

ICE is notably **simpler** than CME in several dimensions:

| Dimension | CME | ICE |
|---|---|---|
| Matching algorithms | 8+ (FIFO, ProRata, LMM, TPR, Configurable, etc.) | 2 (FIFO + GTBPR) |
| Closing mechanism | Opening + closing auctions | VWAP settlement window (no auction) |
| Circuit breaker | Velocity Logic + CME-specific price banding | IPL hold-and-resume (simpler) |
| Market maker priority | LMM (Liquidity-Making Maker) tier in some algorithms | None |
| Protocol | iLink3 SBE (complex, proprietary) | FIX 4.2 (legacy) + new SBE BOE |
| Implied matching | Complex multi-leg implied + inter-commodity | Calendar spreads + IN/OUT implied only |

This means an ICE simulator is achievable with significantly less effort than CME — the core algorithms are already present, and the main work is the protocol layer.

---

## Sources

- [ICE iMpact Market Data Handler SDK — OnixS](https://www.onixs.biz/ice-impact-multicast-price-feed.html)
- [ICE Binary Order Entry API — OnixS Technical Overview](https://www.onixs.biz/insights/why-the-ice-binary-order-entry-api-is-a-structural-shift-not-just-a-faster-interface)
- [ICE Self-Trade Prevention Functionality FAQ](https://www.ice.com/publicdocs/futures/IFEU_Self_Trade_Prevention_Functionality_FAQ.pdf)
- [ICE Futures US Interval Price Limit Levels](https://www.ice.com/publicdocs/futures_us/Futures_US_IPL_Levels.pdf)
- [ICE Futures US No Cancellation Ranges and Reasonability Limits](https://www.ice.com/publicdocs/futures_us/no_cancellation_range_and_reasonablity_limits.pdf)
- [ICE Supported Order Types — Trading Technologies](https://library.tradingtechnologies.com/user-setup/ice-supported-order-types.html)
- [ICE Three Month Euribor Futures Contract Specification](https://www.ice.com/products/38527986/Three-Month-Euribor-Futures)
- [ICE Brent Crude Futures Contract Specification](https://www.ice.com/products/219/Brent-Crude-Futures)
- [ICE Futures Implied Prices FAQ](https://www.ice.com/publicdocs/technology/Additional_Implieds_FAQ.pdf)
- [ICE Trading Procedures — ICE Futures Europe](https://www.ice.com/publicdocs/contractregs/185_XX_TRADING_PROCEDURES.pdf)
- [ICE Circular 25/075 — Binary Order API introduction](https://www.ice.com/publicdocs/circulars/25075.pdf)
- [ICE Futures Europe STIR GTBPR matching algorithm — Circular 21/020](https://www.ice.com/publicdocs/circulars/21020%20.pdf)
- [ICE Europe Commodities iMpact — Databento](https://databento.com/datasets/IFEU.IMPACT)
- [OnixS Java iMpact Handler Programming Guide](https://ref.onixs.biz/java-ice-impact-handler-guide/)
