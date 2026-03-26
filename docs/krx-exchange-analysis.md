# KRX / KOSCOM Exchange Protocol Analysis

**Date:** 2026-03-26
**Scope:** Korea Exchange (KRX) order gateway + KOSCOM market data feed
**Purpose:** Research for building a KRX exchange simulator on top of exchange-core

---

## 1. Overview of the Korean Market Structure

The Korean capital markets have a unique two-party infrastructure split:

```
   [Buy-Side / Algo]
         |
         | Order submission (FIX/FAST or KMAP/PowerBASE binary)
         v
  +----------------+
  |  KRX EXTURE+   |  <-- Matching engine (equities in Seoul, derivatives in Busan)
  +----------------+
         |
         | Execution results / fills
         v
  +----------------+
  |    KOSCOM      |  <-- IT infrastructure operator, market data distributor
  |   STOCK-NET    |     (brokers receive data via KOSCOM; KOSCOM runs EXTURE+)
  +----------------+
         |
         | UDP broadcast real-time market data
         v
  [Broker Members / ISVs]
```

**Key distinction:** KOSCOM does not just distribute market data — it operates the KRX trading system (EXTURE+) on behalf of KRX. Market data is collected and re-distributed through KOSCOM's MDCS (Market Data Control System). Order gateway traffic goes directly to the EXTURE+ matching engine via STOCK-NET (KOSCOM's dedicated financial network).

---

## 2. KRX EXTURE+ Order Gateway

### 2.1 System Overview

EXTURE+ (also stylized as "EXTURE Plus") launched in **March 2014** as the successor to the original EXTURE system. It is built and operated by KOSCOM.

**Performance characteristics:**
- Throughput: **20,000+ TPS** (up from 250 TPS on legacy EXTURE — an 80× improvement)
- Order processing latency: **<70 µs** (down from 20,000 µs — a 285× improvement)
- Infrastructure: Seoul data center (equities/KOSPI/KOSDAQ) + Busan data center (derivatives), with active-active backup

**Scope:** A single platform for all KRX asset classes:
- Equities (KOSPI, KOSDAQ, KONEX)
- Bonds (government, small lot, REPO)
- Derivatives (futures, options — see Section 4)
- Commodities (gold, oil)
- Carbon emissions
- OTC CCP clearing

### 2.2 Protocol

**Order entry protocol: FIX + FAST**

KRX adopted **FIX (Financial Information eXchange)** for order entry and **FAST (FIX Adapted for Streaming)** for market data encoding when EXTURE+ was launched. This was a deliberate move away from the fully proprietary protocols used in the prior EXTURE system.

- **Primary order gateway API names:** `KMAP` (KRX Market Access Protocol) and `KRX/KOSCOM PowerBASE` Order-Entry API. PowerBASE is the KOSCOM-branded middleware. At the wire level, these use FIX/FAST encoding.
- **Transport:** TCP/IP over STOCK-NET dedicated network. Member firms connect via KOSCOM's STOCK-NET leased-line network, not the public internet.
- **ISV Certification:** Third-party vendors must obtain KRX ISV certification. Raptor (now part of Infiex) was one of the first ISVs certified on EXTURE+ in 2014, providing FIX/OUCH-to-KRX-native DMA translation. Samsung Futures partnered with Trading Technologies to offer TT platform connectivity to KRX.

### 2.3 Session Management

FIX standard session management applies:
- **Logon** (MsgType=`A`) to establish session
- **Heartbeat** (MsgType=`0`) at configurable interval
- **TestRequest** / **Heartbeat** round-trip for liveness checking
- **Sequence number reset** on session re-establishment
- **Logout** (MsgType=`5`) for graceful disconnect

No SBE or iLink3 style session has been confirmed for EXTURE+. The protocol is standard FIX 4.x session layer with FAST encoding at the transport/serialization layer.

### 2.4 Connectivity and Co-location

- **STOCK-NET:** KOSCOM's dedicated high-speed optical network connecting KRX, brokers, institutional investors, and regulatory bodies. Launched 1991; modernized with DWDM super high-speed optical equipment.
- **Co-location:** Proximity hosting adjacent to KRX's derivatives matching engine in Busan is available via the **Colt Busan Data Center** (partnership with KOSCOM since 2012) and also via KVH proximity services. Services include managed compute, redundant power, and direct rack connections.
- **International connectivity:** KVH / BSO provide cross-connect from Tokyo, Singapore, Hong Kong, Sydney, and Chicago to KOSCOM's Busan and Seoul facilities.

---

## 3. KOSCOM Market Data Feed (MDCS)

### 3.1 Overview

KOSCOM operates the **MDCS (Market Data Control System)** — referred to publicly as the "KOSCOM Market Data Service." Real-time data from all KRX markets (equities, derivatives, bonds, indices) is collected from EXTURE+ and distributed through MDCS.

**Feed tiers:**
| Tier | Transport | Description |
|------|-----------|-------------|
| Real-time (dedicated network) | UDP broadcast over STOCK-NET | Broker-connected members receive live tick data via UDP |
| Real-time (internet) | TCP/IP | Internet-based real-time distribution for non-member users |
| Batch | FTP | End-of-day historical data |

### 3.2 Encoding and Protocol

- **Encoding:** FAST (FIX Adapted for Streaming) encoding for the real-time feed. This is consistent with KRX's adoption of FIX/FAST throughout EXTURE+.
- **Transport (real-time):** **UDP broadcast** to broker members. If co-located inside the STOCK-NET broadcast segment, brokers receive tick data directly via UDP without additional hops.
- **Recovery/gap-fill:** Not publicly documented. Typical for UDP-based feeds — either TCP snapshot channel or retransmission request. Broker members receive full documentation from KOSCOM.
- **Port mapping:** UDP port assignments per data category may be remapped by brokers. Members must obtain port/channel documentation from their broker or directly from KOSCOM.

### 3.3 Data Content

MDCS distributes:
- Executed trades (trade price, quantity, aggressor side)
- Order book quotes (bid/ask, depth — Level 2 MBP available)
- Index data (KOSPI, KOSPI 200, KOSDAQ 150, sector indices)
- Disclosure and corporate action data
- Settlement prices and daily statistics (OHLCV)
- Clearing and settlement notification results

**Depth:** Level 2 (MBP) market depth is confirmed available. Level 3 MBO (per-order) is not publicly confirmed for KRX.

### 3.4 Additional Data Distribution Partners

- **ICE Data Services** redistributes KRX Level 1 and Level 2 data globally via ICE Connect.
- **Nasdaq / TickData** also carry historical KRX tick data.
- KOSCOM provides data to domestic/overseas information vendors and institutional investors.

---

## 4. KRX Derivatives Products

### 4.1 Stock Index Derivatives

| Product | Symbol | Underlying | Contract Size | Tick Size | Tick Value |
|---------|--------|------------|--------------|-----------|------------|
| KOSPI 200 Futures | KS | KOSPI 200 | Index × KRW 250,000 | 0.05 pts | KRW 12,500 |
| Mini KOSPI 200 Futures | MKS | KOSPI 200 | Index × KRW 50,000 | 0.02 pts | KRW 1,000 |
| KOSDAQ 150 Futures | KSQ | KOSDAQ 150 | Index × KRW 10,000 | 0.10 pts | KRW 1,000 |
| KOSPI 200 Options | — | KOSPI 200 | Index × KRW 250,000 | 0.05 pts (≥3.00 premium); 0.01 pts (<3.00) | KRW 25,000 / KRW 5,000 |
| Mini KOSPI 200 Options | — | KOSPI 200 | Index × KRW 50,000 | — | — |
| KOSPI 200 Weekly Options | — | KOSPI 200 | Index × KRW 250,000 | — | — |
| KOSDAQ 150 Options | — | KOSDAQ 150 | Index × KRW 10,000 | — | — |

**Weekly options:** KOSPI 200 has both Thursday-expiry and Monday-expiry weekly options. KRX announced plans (May 2025) to add Monday/Thursday weeklies for KOSDAQ 150 and dividend futures.

**Single stock futures/options:** Underlying stocks selected semi-annually (March, September) from KOSPI 200 and KOSDAQ Global Index constituents.

### 4.2 Interest Rate Derivatives

| Product | Symbol | Contract Size | Tick Size | Tick Value | Price Limit |
|---------|--------|--------------|-----------|------------|-------------|
| 3-Year KTB Futures | KTB | KRW 100 million | 0.01 pts | KRW 10,000 | ±1.5% (dynamic ±0.5%) |
| 5-Year KTB Futures | — | KRW 100 million | 0.01 pts | — | — |
| 10-Year KTB Futures | LKTB | KRW 100 million | 0.01 pts | KRW 10,000 | ±2.7% (dynamic ±0.9%) |

### 4.3 Currency Derivatives

| Product | Symbol | Contract Size | Tick Size | Tick Value | Price Limit |
|---------|--------|--------------|-----------|------------|-------------|
| USD/KRW Futures | USD | USD 10,000 | 0.1 KRW | KRW 1,000 | ±4.5% (dynamic ±1.0%) |
| JPY Futures | JPY | JPY 1,000,000 | — | — | — |
| EUR Futures | EUR | EUR 10,000 | — | — | — |
| USD Options | — | USD 10,000 | — | — | — |

USD futures listing: 12 consecutive months + 8 quarterly months.

---

## 5. Trading Session Schedule

All times are Korea Standard Time (KST = UTC+9).

### 5.1 Equities (KOSPI / KOSDAQ / KONEX)

```
  07:30                Pre-order entry (no matching)
  08:30–09:00          Opening call auction (single-price)
    08:50–09:00          Indicative price display window
  09:00–15:20          Continuous auction (price-time FIFO)
  15:20–15:30          Closing call auction (single-price)
  15:30                Market close (equities)
```

**Daily price limit:** ±30% of previous close for stocks, KDRs, ETFs, ETNs.

### 5.2 Derivatives (Futures and Options) — Regular Session

```
  08:00–09:00          Opening call auction (single-price)
  09:00–15:35 (15:20 on last trading day)   Continuous auction
  15:35–15:45          Closing call auction (single-price)
  15:45                Regular session close
```

**KOSPI 200 Futures regular session hours: 08:00–15:45 KST**
*(Capital Futures shows 07:45–14:45 UTC+8 which maps to 08:45–15:45 KST — slight discrepancies exist in third-party sources; official KRX source is 09:00–15:45 for regular with pre-market auction starting 08:00.)*

### 5.3 Derivatives Night Session (launched June 9, 2025)

The KRX launched an independent (non-EUREX-linked) night session on June 9, 2025. The prior EUREX-link arrangement ended June 5, 2025.

```
  17:50–18:00          Opening call auction (quote receipt)
  18:00                Opening price determination
  18:00–05:50          Continuous trading (night session)
  05:50–06:00          Closing call auction (quote receipt)
  06:00                Closing price determination (end of night session)
```

**Combined daily coverage:** Regular session (08:00–15:45) + Night session (18:00–06:00) = approximately 19 hours.
**Account:** Single trading account is used for both sessions.
**Trading day convention:** Night session trades are recorded on the same calendar trading day as the preceding regular session.
**Night session products (10 products):**
1. KOSPI 200 Futures
2. Mini KOSPI 200 Futures
3. KOSDAQ 150 Futures
4. KOSPI 200 Options
5. Mini KOSPI 200 Options
6. KOSPI 200 Weekly Options
7. KOSDAQ 150 Options
8. U.S. Dollar Futures
9. 3-Year KTB Futures
10. 10-Year KTB Futures

---

## 6. Order Types and Time-in-Force

### 6.1 Order Types (Derivatives — Regular Session)

KRX officially publishes the following order types for derivatives trading:

| Order Type | KRX Name | Description |
|-----------|----------|-------------|
| **Limit** | Limit Order | Price and quantity specified; matched at ≤ ask (buy) or ≥ bid (sell). Unmatched remainder rests in book. |
| **Market** | Market Order | Quantity specified, no price. Matched at best available price. Available for near-month contracts only. |
| **Immediately Executable Limit** | — | Hybrid: price not specified (like market) but matching price locks to best price at time of entry. Unexecuted remainder queued at that locked price. |
| **Limit-to-Market-on-Close** | LOC | Limit order that auto-converts to market order for inclusion in closing single-price auction if unfilled during continuous session. |
| **Fill-or-Kill** | FOK | Cancelled in full if not immediately fully executed on entry. |

**Notes:**
- **GTC (Good-till-Cancelled):** Confirmed present (exchange-gap-analysis.md lists KRX as requiring GTC). Details on cross-session persistence for derivatives GTC not publicly documented.
- **IOC (Immediate-or-Cancel):** Not explicitly named in KRX official English documentation reviewed, though standard FIX connectivity (used by ISVs) would map it. KRX's "Immediately Executable Limit" may be the native equivalent.
- **DAY:** Default TIF for limit orders — unmatched residual rests until end-of-session.
- **GTD (Good-till-Date):** Not explicitly confirmed for KRX derivatives.
- **Stop / Stop-Limit:** Confirmed required in exchange-gap-analysis.md; KRX official English docs do not detail the trigger mechanism publicly.

### 6.2 Equities Order Types

| Order Type | Description |
|-----------|-------------|
| Limit Buy/Sell, Short Sell | Standard limit order |
| Market Buy/Sell, Short Sell | Market order |
| Limit-on-Close (LOC) | Execute at closing auction price |
| Limit-on-Open (LOO) | Execute at opening auction price |
| Market-on-Close (MOC) | Market order at close |
| Market-on-Open (MOO) | Market order at open |

**Equities tick size schedule (price-dependent):**

| Price Range (KRW) | Tick Size (KRW) |
|-------------------|-----------------|
| < 1,000 | 1 |
| 1,000–4,999 | 5 |
| 5,000–9,990 | 10 |
| 10,000–49,950 | 50 |
| 50,000–99,900 | 100 |
| 100,000–499,500 | 500 |
| ≥ 500,000 | 1,000 |

**Equities lot size:** 10 shares (1 share if base price > KRW 50,000).

---

## 7. KRX Trading Rules and Mechanisms

### 7.1 Matching Algorithm

**Primary algorithm: Price-Time Priority (FIFO)**

KRX uses strict price-time FIFO matching for continuous trading across all markets (equities and derivatives). Priority is: (1) best price, (2) earliest time at that price. No pro-rata allocation, no market-maker priority.

**Auction matching:** Single-price auction (call auction / batch auction). The exchange determines a single equilibrium price that maximizes executable volume, using price-time priority to resolve ties among orders at the clearing price. Used for:
- Opening auction
- Closing auction
- Post-circuit-breaker resumption (10-minute auction)
- Post-sidecar resumption (5-minute auction)
- Volatility interruption (VI) cooldown auctions (2 minutes)

**Settlement:** Cash settlement (for derivatives). Final settlement price is calculated from the underlying index value.

### 7.2 Circuit Breakers

**Market-wide circuit breaker (KOSPI-based):**

| Trigger | Condition | Response |
|---------|-----------|----------|
| Level 1 | KOSPI falls ≥10% from previous close for 1 continuous minute | Trading suspended 20 minutes; resumption via 10-minute single-price auction |
| — | Not exercised after 14:20 KST | No activation in final hour |

After the 20-minute suspension, the first price is determined via a 10-minute single-price auction, then continuous trading resumes.

**Tiered derivatives price limits (KOSPI 200 futures/options as example):**

| Level | Limit | Response |
|-------|-------|----------|
| Level 1 | ±8% from prior close | 5-minute suspension |
| Level 2 | ±15% from prior close | 5-minute suspension |
| Level 3 | ±20% from prior close | Trading halt for the session |

KTB futures and currency futures have their own dynamic price limits (±1.5%, ±2.7%, ±4.5% respectively) with additional dynamic intraday bands.

### 7.3 Sidecar (Programme Trading Mechanism)

A "sidecar" suspends programme trading (basket/algorithmic orders) specifically, without halting the entire market:

- **Trigger:** KOSPI 200 Futures rises or falls ≥5% from base price and holds for 1 continuous minute (some sources quote 4% — the current threshold is 5% for sell-side sidecar)
- **Effect:** Programme trading orders are delayed/halted for 5 minutes
- **Resumption:** 5-minute single-price auction, then continuous trading
- **Restriction:** Not applicable after 14:50 KST

### 7.4 Volatility Interruption (VI)

KRX uses two types of VI (share-level, not index-level) for individual equities:

| Type | Trigger | Duration | Effect |
|------|---------|----------|--------|
| **Dynamic VI** | Price moves ≥3% from last traded price | 2 minutes | Single-price auction cooldown |
| **Static VI** | Price moves ±10% from previous day close | 2 minutes | Single-price auction cooldown |

During VI, order submissions and cancellations are still accepted; only execution is suspended. KOSPI 200 futures use dynamic VI only; individual equities use both.

### 7.5 Opening and Closing Auction Mechanism

**Opening auction (equities):**
- Quote receipt: 08:30–09:00 (last 10 minutes display indicative price: 08:50–09:00)
- Single-price determination at 09:00

**Closing auction (equities):**
- Call auction: 15:20–15:30
- Trades are recorded at the single closing price
- Closing price = single-price auction clearing price

**Closing price uses:** The closing price is the reference for next-day price limits and margin calculations.

---

## 8. Public Resources and Documentation

### 8.1 Official Sources

| Resource | URL / Location | Access |
|----------|----------------|--------|
| Global KRX website | `global.krx.co.kr` | Public (but many pages redirect for non-KR IPs) |
| KRX Open API | `openapi.krx.co.kr` | Requires KRX account/API key; T-1 data only (no real-time orders) |
| KRX Data Marketplace | `data.krx.co.kr` | Public reference/historical; live feeds require subscription |
| KOSCOM English portal | `english.koscom.co.kr` | Marketing-level only |
| KOSCOM MDCS portal | `data.koscom.co.kr` | Requires JS/login; subscriber access |
| FIX Trading Community (KRX group) | `fixtrading.org/groups/korea-exchange/` | Private group; requires membership |

### 8.2 Third-Party Technical References

| Resource | Notes |
|----------|-------|
| Capital Futures contract specs | Good source for futures contract specs (Taiwan-based broker supporting KRX) |
| Samsung Futures English site | Authoritative contract specs for KOSPI 200 |
| MarketsWiki — KRX articles | Useful background on history and products |
| CSIData KRX commodity factsheet | Lists all derivatives products with codes |
| Raptor/Infiex (ISV) | Confirms FIX+FAST protocol usage in EXTURE+ |
| Trading Technologies (TT) documentation | Samsung Futures is the KRX bridge; TT connects via KMAP/PowerBASE |
| KOFIA English guidelines | "Guidelines on Trading Derivatives on the Exchange" (English PDF, older) |

### 8.3 GitHub / Open Source

No publicly available KRX protocol parser, decoder, or order connector was found on GitHub as of March 2026. The KRX Open API (REST/HTTP) has Node.js and Rust wrappers for historical data (`krx-stock-api`, `krx-rs`) but these are T-1 data only, not real-time order entry.

**Conclusion:** KRX protocol schemas are not publicly downloadable. No ITCH/OUCH/SBE schemas exist for KRX (KRX uses FIX/FAST, not Nasdaq-family protocols). No OMI (Open Markets Initiative) Lua dissectors were found for EXTURE+.

---

## 9. Exchange-Core Gap Analysis: KRX vs Current Engine

### 9.1 What exchange-core Already Supports

| KRX Requirement | Engine Status | Notes |
|----------------|---------------|-------|
| FIFO / Price-Time matching | **Supported** | Core matching algorithm matches KRX |
| Limit orders | **Supported** | — |
| Market orders | **Supported** | — |
| Stop / Stop-Limit | **Supported** | — |
| DAY TIF | **Supported** | — |
| GTC TIF | **Supported** | — |
| IOC TIF | **Supported** | — |
| FOK TIF | **Supported** | — |
| Self-trade prevention | **Supported** | — |
| L1 / L2 market data | **Supported** | — |
| OHLCV accumulator | **Supported** | Added in Phase 2 |
| Mass cancel | **Supported** | Added in Phase 2 |
| Iceberg orders | **Supported** | Added in Phase 2 |
| Session state machine | **Supported** | Added in Phase 2 |
| Dynamic price bands | **Supported** | Added in Phase 2 |
| Opening/closing auction | **Supported** | Added in Phase 2 |
| Circuit breaker framework | **Supported** | Added in Phase 2 |
| Volatility auction | **Supported** | Added in Phase 2 |
| SMP | **Supported** | — |

### 9.2 Gaps — What Needs CRTP Overrides in KrxExchange

These are **exchange-layer** items: the core mechanism exists, but KRX-specific parameters and logic must be configured via the CRTP `KrxExchange` class.

| Feature | Required KrxExchange Override | Complexity |
|---------|-------------------------------|------------|
| **Session schedule** | Opening auction: 08:00–09:00 derivatives / 08:30–09:00 equities; closing 15:20–15:30 / 15:35–15:45; night 18:00–06:00 | Low |
| **Tiered price limits** | Level 1/2/3 thresholds (±8%/±15%/±20%) with session halt at L3 | Medium |
| **Dynamic intraday bands** | Bond futures: ±0.5%/±0.9% dynamic; FX: ±1.0% dynamic bands around last trade | Medium |
| **Sidecar mechanism** | Programme trading pause (5 minutes) triggered at ±5% futures move — unique to KRX | Medium |
| **VI thresholds** | Dynamic VI: 3% from last traded price; Static VI: ±10% from prior close; 2-minute auction | Low |
| **Tick size per product** | Derivatives: fixed tick per product (0.05, 0.02, 0.01, etc.); equities: 7-tier price-dependent table | Low |
| **Limit-to-Market-on-Close order** | LOC conversion logic at closing auction entry | Medium |
| **Immediately Executable Limit** | Locks price at best-at-entry; residual rests at that locked price | Low-Medium |
| **No iceberg needed** | KRX derivatives do not use iceberg orders (confirmed not required) | N/A (disable) |
| **Settlement price hook** | Cash settlement; final settlement = last reference index value | Low |
| **Night session state** | Two-phase session per day: regular + night; same account; night has opening/closing auction | Medium |
| **Daily price limits for equities** | ±30% hard limit (broader than derivatives' tiered structure) | Low |

### 9.3 Gaps — Items That Need New Core Features (if not already added in Phase 2)

Cross-reference with Phase 2 additions from exchange-gap-analysis.md:

| Feature | Status | Notes |
|---------|--------|-------|
| **Multi-session per calendar day** | May need extension | KRX has two sessions per day (regular + night) sharing same account and position state |
| **Programme trading flag / sidecar** | New core hook needed | `is_programme_order(order)` flag + `sidecar_active_` state that pauses only flagged orders |
| **Limit-to-Market-on-Close TIF** | Not in current core | New TIF variant: `LOC` — treated as DAY until closing auction entry, then converts to market |
| **Immediately Executable Limit** | Partial | Could be approximated as IOC with partial-fill allowed, but KRX semantics (residual rests) differ |

### 9.4 Protocol Layer: What Needs Building

For a KRX simulator analogous to the CME (iLink3 + MDP3) and ICE (FIX + iMpact) simulators:

| Layer | CME Analog | ICE Analog | KRX Equivalent | Status |
|-------|-----------|-----------|----------------|--------|
| Order entry protocol | iLink3 (SBE) | FIX 4.2 | **FIX/FAST** (KMAP/PowerBASE wire format) | Not built |
| Market data protocol | MDP3 (SBE, UDP multicast) | iMpact (binary, UDP multicast) | **FAST over UDP** (KOSCOM MDCS) | Not built |
| Session layer | iLink3 session | FIX session | **FIX session** (same as ICE) | Reuse ICE FIX session code |
| Execution report | SBE ExecutionReport | FIX ExecReport (35=8) | **FIX ExecReport** | Reuse ICE FIX gateway |
| Market data messages | SBE MBP/MBO | Binary structs | FAST-encoded quote/trade/status | Not built |

**Key insight:** The KRX order entry protocol is standard FIX with FAST encoding — it is closer to ICE's FIX gateway than to CME's SBE/iLink3. The ICE FIX session code (`ice/fix/`) can be heavily reused or adapted.

### 9.5 Complexity Assessment vs CME and ICE

| Dimension | CME | ICE | KRX |
|-----------|-----|-----|-----|
| Order entry protocol complexity | High (SBE iLink3, custom session) | Medium (FIX 4.2, standard session) | Medium (FIX/FAST, standard session) |
| Market data protocol complexity | High (SBE MDP3, multicast blocks) | High (proprietary binary iMpact, multicast) | Medium (FAST encoding, UDP) |
| Matching algorithm complexity | High (8 algorithms, implied, GTBPR) | High (GTBPR, FIFO, dual-pool) | Low (pure FIFO only) |
| Session/auction complexity | High (multiple auction types, night session) | Medium (standard opening/closing) | Medium (2 sessions/day, sidecar, VI) |
| Product breadth | High (8 product types) | Medium (10 products) | Medium (10+ derivatives + equities) |
| Public protocol documentation | High (full SBE schemas) | Low (gated; OMI dissectors only) | Low (gated; no public schemas) |
| **Overall** | **High** | **High** | **Medium-Low** |

**Summary:** KRX is significantly simpler than CME or ICE from a matching algorithm and protocol perspective. The primary unique challenges are:
1. The sidecar mechanism (no equivalent in CME or ICE)
2. The dual-session (regular + night) calendar structure
3. The tiered price limit system (±8%/±15%/±20%)
4. Protocol schemas being non-public (similar constraint to ICE; same workaround applies)

---

## 10. Implementation Recommendations

### 10.1 What to Reuse from ICE

- `ice/fix/fix_parser.h` + `fix_encoder.h` — standard FIX 4.2 session/application parsing works for KRX
- `ice/fix/fix_gateway.h` — session lifecycle (logon, heartbeat, sequence numbers)
- `ice/fix/fix_exec_publisher.h` — FIX execution report generation

### 10.2 New Files Needed for KRX

```
krx/
  krx_exchange.h          — KrxExchange CRTP (session schedule, VI thresholds, sidecar)
  krx_products.h          — 10+ derivatives products with tick/limit configs
  krx_simulator.h         — KrxSimulator wrapping FIFO engine (simpler than ICE dual-pool)
  fix/
    krx_fix_gateway.h     — Reuse/adapt ice/fix/fix_gateway.h for KMAP protocol
    krx_exec_publisher.h  — FIX execution reports (adapt from ICE)
  fast/
    krx_fast_feed.h       — FAST-encoded market data publisher (UDP)
    krx_fast_decoder.h    — FAST decoder for market data ingestion
  e2e/
    krx_e2e_runner.cc     — E2E test harness
    krx_sim_runner.cc     — Live simulator binary
```

### 10.3 Certification and Access

To implement production-grade KRX connectivity:
1. Apply for KRX ISV certification through the KRX Member Services portal
2. Obtain KMAP API specification from KRX/KOSCOM (member-only documentation)
3. Use KOSCOM's test environment for conformance testing
4. Conduct simulation testing via KOSCOM's conformance test system before live access

Without official certification, a simulator can be built based on publicly known FIX/FAST semantics and reverse-engineered market behavior — the same approach taken for ICE.

---

## 11. Key Unknowns and Caveats

| Item | Status | Mitigation |
|------|--------|------------|
| Exact KMAP/PowerBASE message field definitions | **Unknown** (member-only) | Use standard FIX tags; adapt from known KRX FIX behavior |
| FAST template IDs for KRX MDCS market data | **Unknown** | FAST templates are negotiated/downloaded at session start; implement standard FAST with template loading |
| Night session order type restrictions | **Unknown** | Assume same order types as regular session |
| GTC cross-session behavior for night session | **Unknown** | Likely: DAY orders expire at regular session end; GTC survives to night session and next day |
| Iceberg order support | **Likely not supported** for derivatives (confirmed as "not required" in gap analysis) | Disable iceberg in KrxExchange config |
| 5-Year KTB futures specs | **Partial** — product exists; tick/limit details not publicly confirmed | Use 3-Year KTB specs as proxy |
| FX futures (JPY, EUR) specs | **Partial** | Similar structure to USD; use USD specs as template |
| Sidecar threshold (4% vs 5%) | **Ambiguous** — sources conflict | 5% appears to be current threshold based on most recent sources |

---

*Sources:*
- [Global KRX Technology / EXTURE+](https://global.krx.co.kr/contents/GLB/04/0402/0402020000/GLB0402020000.jsp)
- [KOSCOM KRX Trading System](https://www.koscom.co.kr/eng/main/contents.do?menuNo=300005)
- [KOSCOM Market Data Service](https://www.koscom.co.kr/eng/main/contents.do?menuNo=300126)
- [KOSCOM STOCK-NET](https://www.koscom.co.kr/eng/main/contents.do?menuNo=300122)
- [Global KRX Derivatives Order Types](https://global.krx.co.kr/contents/GLB/06/0603/0603010200/GLB0603010200.jsp)
- [Global KRX Derivatives Trading Hours](https://global.krx.co.kr/contents/GLB/06/0603/0603010300/GLB0603010300.jsp)
- [KRX Night Session Launch (Global Exchanges)](https://globalexchanges.com/latest-news/korean-republic-krx-derivatives-market-launches-night-session/141976/)
- [KRX After-Hours Session (Korea Times)](https://www.koreatimes.co.kr/business/banking-finance/20250609/krx-opens-independently-run-after-hours-trading-session-for-derivatives)
- [KOSPI 200 Futures specs (Samsung Futures)](https://www.ssfutures.com/ssf/eng/product/viewProduct.cmd?viewPage=ENG_030201&gtyp=fmast&gcode=KOSPI)
- [KRX Contract Specs (Capital Futures)](https://www.capitalfutures.com.tw/en-us/productinformation/contract?area=jp-kr&ex=krx)
- [KRX Real Trading overview](https://realtrading.com/trading-markets/krx/)
- [KRX derivatives rule improvements (Global Exchanges)](https://globalexchanges.com/latest-news/korean-republic-krx-publishes-annual-update-to-single-stock-futures-options-and-other-improvements-to-derivatives-market-rules/135676/)
- [Raptor ISV certification press release](https://www.raptorfintech.com/our-news-4)
- [Dynamic/Static VI research (MDPI)](https://www.mdpi.com/1911-8074/15/3/105)
- [KVH/KOSCOM proximity services](https://www.businesswire.com/news/home/20120227005739/en/KVH-and-Koscom-to-Launch-Proximity-Services-for-KRXs-Derivatives-Trading)
- [KOFIA Guidelines on Trading Derivatives](https://eng.kofia.or.kr/brd/m_15/down.do?seq=167&file_seq=1&data_tp=A)
- [KRX tick data via UDP (quant-book)](http://quant-book.wikidot.com/post:tick-data-from-krx)
