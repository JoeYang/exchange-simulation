# CME Globex Exchange Simulator — Implementation Plan

**Date:** 2026-03-25
**Status:** Plan (merges Phase 3 CME design, tooling plan, and E2E testing plan)
**Supersedes:** `2026-03-25-phase3-cme-design.md` (partially built), `2026-03-25-tooling-plan.md`, `2026-03-25-exchange-testing-plan.md`

---

## 1. What's Already Built

| Component | Location | Status |
|-----------|----------|--------|
| exchange-core (matching engine) | `exchange-core/` | Done (Phase 1+2) |
| Multi-instrument framework | `exchange-sim/` | Done |
| CmeExchange CRTP class | `cme/cme_exchange.h` | Done |
| CME product configs (ES, NQ, CL, GC, ZN, ZB, MES, 6E) | `cme/cme_products.h` | Done |
| CmeSimulator wrapper | `cme/cme_simulator.h` | Done |
| CME lifecycle tests | `cme/cme_lifecycle_test.cc` | Done |
| OHLCV statistics | `exchange-core/ohlcv.h` | Done |
| iLink3 SBE schema (v5, 48 messages) | `cme/ilink3/ilinkbinary.xml` | Downloaded |
| MDP3 SBE schema (v13, 31 messages) | `cme/mdp3/templates_FixBinary.xml` | Downloaded |
| Journal test framework (87 scenarios) | `test-journals/` | Done |
| Visualization tools | `tools/` | Done |
| Performance benchmarks | `benchmarks/` | Done |

**What's missing:** The protocol layer (SBE encode/decode) and E2E testing that connects iLink3 order entry to MDP3 market data output.

---

## 2. Architecture (Target State)

```
    Client (test runner)                          Market Data Consumer (test verifier)
         │                                                    ▲
         │ iLink3 SBE                                         │ MDP3 SBE
         ▼                                                    │
    ┌────────────┐                                   ┌────────────────┐
    │ iLink3     │  decode    ┌──────────────┐       │ MDP3 Feed      │  encode
    │ Gateway    │──────────→ │ CmeSimulator │──────→│ Publisher      │──────→ MDP3 bytes
    │ (decoder)  │            │              │       │ (encoder)      │
    └────────────┘            │  ES ──┐      │       └────────────────┘
         │                    │  NQ   │engine│              ▲
         │ ExecutionReport    │  CL   │      │              │
         ▼ (iLink3 encode)   │  GC   │      │       engine callbacks
    ┌────────────┐            └───────┴──────┘       (OrderListener +
    │ Exec Report│                                    MdListener)
    │ Publisher  │
    └────────────┘
         │
         ▼ iLink3 SBE
    Client receives ack/fill/cancel confirmation
```

**The full pipeline for one order:**
1. Test runner encodes `NewOrderSingle514` (iLink3 SBE bytes)
2. iLink3 decoder → `OrderRequest` struct
3. `CmeSimulator.new_order(instrument, request)`
4. Engine processes → fires callbacks
5. `ExecReportPublisher` encodes → `ExecutionReport522` (iLink3 SBE bytes)
6. `MDP3FeedPublisher` encodes → `MDIncrementalRefreshBook46` (MDP3 SBE bytes)
7. Test runner decodes both, compares against EXPECT lines

---

## 3. Implementation Tasks

### Phase 4A — SBE Codec (Foundation)

These are the building blocks. No exchange or simulator dependency — pure encode/decode.

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 1 | SBE header + common types | `cme/codec/sbe_header.h` | ~100 | — | SBE message header, encoding helpers, byte order, group/var-data primitives |
| 2 | iLink3 message structs | `cme/codec/ilink3_messages.h` | ~200 | 1 | C++ structs for: NewOrderSingle514, OrderCancelRequest516, OrderCancelReplaceRequest515, OrderMassActionRequest529, ExecutionReportNew522, ExecutionReportFill524, ExecutionReportCancel525, ExecutionReportReject523, OrderCancelReject535 |
| 3 | MDP3 message structs | `cme/codec/mdp3_messages.h` | ~180 | 1 | C++ structs for: MDIncrementalRefreshBook46, MDIncrementalRefreshTradeSummary48, SecurityStatus30, SnapshotFullRefreshOrderBook53, MDInstrumentDefinitionFutures54 |
| 4 | iLink3 encoder | `cme/codec/ilink3_encoder.h/.cc` + test | ~250 | 2 | `OrderRequest` → SBE bytes for each order entry message. `OrderAccepted/Filled/Cancelled` → ExecutionReport SBE bytes |
| 5 | iLink3 decoder | `cme/codec/ilink3_decoder.h/.cc` + test | ~250 | 2 | SBE bytes → `OrderRequest` for order entry. SBE bytes → decoded ExecutionReport structs |
| 6 | MDP3 encoder | `cme/codec/mdp3_encoder.h/.cc` + test | ~250 | 3 | Engine callbacks (`DepthUpdate`, `Trade`, `TopOfBook`, `MarketStatus`) → MDP3 SBE bytes |
| 7 | MDP3 decoder | `cme/codec/mdp3_decoder.h/.cc` + test | ~250 | 3 | MDP3 SBE bytes → decoded structs for test verification |
| 8 | Codec round-trip tests | `cme/codec/codec_roundtrip_test.cc` | ~200 | 4,5,6,7 | Encode → decode → verify field equality for every message type |

### Phase 4B — Protocol Publishers (Connects Engine to Wire)

These are engine listeners that produce SBE-encoded wire messages.

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 9 | iLink3 ExecutionReport publisher | `cme/ilink3_report_publisher.h/.cc` + test | ~200 | 4 | `OrderListenerBase` that encodes each callback as an iLink3 ExecutionReport |
| 10 | MDP3 market data publisher | `cme/mdp3_feed_publisher.h/.cc` + test | ~200 | 6 | `MarketDataListenerBase` that encodes each callback as MDP3 incremental message |
| 11 | iLink3 order decoder (gateway) | `cme/ilink3_gateway.h/.cc` + test | ~200 | 5 | Decodes incoming iLink3 SBE bytes → calls `CmeSimulator.new_order/cancel/modify` |

### Phase 4C — E2E Test Framework

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 12 | E2E journal format extension | `test-harness/` updates | ~200 | — | Add `ILINK3_NEW_ORDER`, `ILINK3_CANCEL`, `ILINK3_REPLACE`, `ILINK3_MASS_CANCEL` actions. Add `EXPECT EXEC_NEW`, `EXPECT EXEC_FILL`, `EXPECT MD_BOOK_ADD`, `EXPECT MD_TRADE` types |
| 13 | E2E test runner | `cme/e2e/e2e_test_runner.h/.cc` + test | ~350 | 9,10,11,12 | Full pipeline: parse journal → encode iLink3 → gateway → engine → publishers → decode → verify. The central integration piece. |
| 14 | E2E test harness (Bazel) | `cme/e2e/e2e_journal_test.cc` | ~80 | 13 | Discovers and runs all `test-journals/e2e/*.journal` and `test-journals/cme/*.journal` files |

### Phase 4D — E2E Test Journals

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 15 | E2E journal generator | `test-journals/generate_e2e_journals.cc` | ~500 | 13 | Runs scenarios through full E2E pipeline, captures correct EXPECT lines |
| 16 | Generic E2E journals | `test-journals/e2e/*.journal` | ~300 | 15 | 9 scenarios: add, fill, cancel, replace, partial, reject, mass cancel, auction, iceberg |
| 17 | CME-specific E2E journals | `test-journals/cme/*.journal` | ~400 | 15 | 9 scenarios: ES trading day, SMP, dynamic bands, multi-product, tick alignment, IOC in auction, DAY expiry, stop trigger, market sweep |

### Phase 4E — Network Transport (Optional — for live simulation)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 18 | TCP server (async, epoll) | `tools/tcp_server.h/.cc` + test | ~250 | — | Non-blocking TCP server for iLink3 gateway |
| 19 | UDP multicast publisher | `tools/udp_multicast.h/.cc` + test | ~150 | — | Multicast publisher for MDP3 feed |
| 20 | CME live simulator binary | `cme/cme_sim_runner.cc` | ~300 | 11,10,18,19 | `cme-sim --ilink3-port 9100 --mdp3-group 224.0.31.1:14310` |
| 21 | Sim config | `cme/sim_config.h` | ~80 | 20 | Runtime config: ports, multicast groups, products |

### Phase 4F — Replay & Diagnostics (Optional)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 22 | pcap reader | `tools/pcap_reader.h/.cc` + test | ~200 | — | Read pcap files, extract UDP/TCP payloads |
| 23 | MDP3 replayer | `tools/mdp3_replayer.cc` | ~150 | 7,22 | Replay MDP3 pcap, decode, compare with simulator |
| 24 | iLink3 replayer | `tools/ilink3_replayer.cc` | ~150 | 5,22 | Replay iLink3 order flow through simulator |
| 25 | Enhanced dashboard | `tools/exchange_dashboard.cc` | ~300 | — | FTXUI: all instruments, OHLCV, session state, throughput |
| 26 | Latency logger | `tools/latency_logger.h` | ~100 | — | Per-order entry→ack, entry→fill latency tracking |

---

## 4. Dependency Graph

```
Phase 4A (Codec):
  [1] SBE Header
   ├→ [2] iLink3 Structs    ├→ [3] MDP3 Structs
   │   ├→ [4] iLink3 Enc    │   ├→ [6] MDP3 Enc
   │   └→ [5] iLink3 Dec    │   └→ [7] MDP3 Dec
   │        │                │        │
   └────────┴────────────────┴────────┘
                    │
              [8] Round-trip Tests

Phase 4B (Publishers):
  [4] ──→ [9]  Exec Report Publisher
  [6] ──→ [10] MDP3 Feed Publisher
  [5] ──→ [11] iLink3 Gateway (decoder)

Phase 4C (E2E Framework):
  [12] Journal Format Extension (parallel with 4A/4B)
       │
  [9] + [10] + [11] + [12] ──→ [13] E2E Test Runner
                                  │
                                [14] E2E Bazel Harness

Phase 4D (Journals):
  [13] ──→ [15] Journal Generator ──→ [16] Generic Journals
                                  ──→ [17] CME Journals

Phase 4E (Network — optional):
  [18] TCP Server  ─┐
  [19] UDP Multicast ├→ [20] Live Sim Binary → [21] Config
  [11] + [10] ──────┘

Phase 4F (Replay — optional):
  [22] pcap Reader → [23] MDP3 Replayer
                   → [24] iLink3 Replayer
  [25] Dashboard (independent)
  [26] Latency Logger (independent)
```

---

## 5. Dev Team Dispatch Strategy

With 4 devs:

| Wave | Dev 1 | Dev 2 | Dev 3 | Dev 4 |
|------|-------|-------|-------|-------|
| **Wave 1** | Task 1 (SBE header) | Task 12 (E2E journal format) | — | — |
| **Wave 2** | Task 2 (iLink3 structs) | Task 3 (MDP3 structs) | Task 12 cont. | — |
| **Wave 3** | Task 4 (iLink3 enc) | Task 5 (iLink3 dec) | Task 6 (MDP3 enc) | Task 7 (MDP3 dec) |
| **Wave 4** | Task 8 (round-trip) | Task 9 (exec publisher) | Task 10 (MDP3 publisher) | Task 11 (gateway) |
| **Wave 5** | Task 13 (E2E runner) | Task 15 (journal gen) | — | — |
| **Wave 6** | Task 16 (generic e2e) | Task 17 (CME e2e) | Task 18 (TCP) | Task 19 (UDP) |
| **Wave 7** | Task 20 (sim binary) | Task 14 (harness) | Task 25 (dashboard) | Task 26 (latency) |

**Critical path:** 1 → 2 → 4 → 9 → 13 → 15 → 17 (7 sequential steps)

---

## 6. E2E Journal Format Reference

### Actions (client sends)

```
ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY [display_qty=5000] [stop_price=49000000]

ACTION ILINK3_CANCEL ts=2000 instrument=ES cl_ord_id=2 orig_cl_ord_id=1

ACTION ILINK3_REPLACE ts=3000 instrument=ES cl_ord_id=3 orig_cl_ord_id=1 price=50010000 qty=10000

ACTION ILINK3_MASS_CANCEL ts=4000 instrument=ES account=FIRM_A

ACTION SESSION_START ts=0 state=PRE_OPEN
ACTION SESSION_OPEN ts=1000
ACTION SESSION_CLOSE ts=86400000
```

### Expects (verify output)

**Execution reports (iLink3 response):**
```
EXPECT EXEC_NEW instrument=ES ord_id=1 cl_ord_id=1 status=NEW
EXPECT EXEC_FILL instrument=ES ord_id=1 cl_ord_id=1 fill_price=50000000 fill_qty=10000 leaves_qty=0 status=FILLED
EXPECT EXEC_PARTIAL instrument=ES ord_id=1 cl_ord_id=1 fill_price=50000000 fill_qty=5000 leaves_qty=5000 status=PARTIAL
EXPECT EXEC_CANCELLED instrument=ES ord_id=1 cl_ord_id=1 status=CANCELLED
EXPECT EXEC_REPLACED instrument=ES ord_id=1 cl_ord_id=3 new_price=50010000 status=REPLACED
EXPECT EXEC_REJECTED instrument=ES cl_ord_id=1 reason=INVALID_PRICE
```

**Market data (MDP3 multicast):**
```
EXPECT MD_BOOK_ADD instrument=ES side=BUY level=1 price=50000000 qty=10000 num_orders=1
EXPECT MD_BOOK_UPDATE instrument=ES side=BUY level=1 price=50000000 qty=20000 num_orders=2
EXPECT MD_BOOK_DELETE instrument=ES side=BUY level=1 price=50000000
EXPECT MD_TRADE instrument=ES price=50000000 qty=10000 aggressor_side=SELL
EXPECT MD_STATUS instrument=ES state=PRE_OPEN
```

### Example: Complete E2E Journal

```
# CME ES: Add limit order, verify execution report AND market data
CONFIG match_algo=FIFO instrument=ES tick_size=2500 lot_size=10000

ACTION SESSION_START ts=0 state=CONTINUOUS

# Client sends NewOrderSingle for ES
ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 account=FIRM_A side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY

# Client receives ExecutionReport (acknowledgement)
EXPECT EXEC_NEW instrument=ES ord_id=1 cl_ord_id=1 status=NEW

# Market data consumers see the book update
EXPECT MD_BOOK_ADD instrument=ES side=BUY level=1 price=50000000 qty=10000 num_orders=1

# Another client sends a crossing sell
ACTION ILINK3_NEW_ORDER ts=2000 instrument=ES cl_ord_id=2 account=FIRM_B side=SELL price=50000000 qty=10000 type=LIMIT tif=DAY

# Both clients receive execution reports
EXPECT EXEC_NEW instrument=ES ord_id=2 cl_ord_id=2 status=NEW
EXPECT EXEC_FILL instrument=ES ord_id=1 cl_ord_id=1 fill_price=50000000 fill_qty=10000 status=FILLED
EXPECT EXEC_FILL instrument=ES ord_id=2 cl_ord_id=2 fill_price=50000000 fill_qty=10000 status=FILLED

# Market data shows trade and book removal
EXPECT MD_BOOK_DELETE instrument=ES side=BUY level=1 price=50000000
EXPECT MD_TRADE instrument=ES price=50000000 qty=10000 aggressor_side=SELL
```

---

## 7. Success Criteria

| Criterion | Verification |
|-----------|-------------|
| iLink3 round-trip: encode → decode = original | Task 8 tests |
| MDP3 round-trip: encode → decode = original | Task 8 tests |
| Every order add produces MD_BOOK_ADD | E2E journals (Task 16) |
| Every cancel produces MD_BOOK_DELETE | E2E journals |
| Every fill produces MD_TRADE + EXEC_FILL | E2E journals |
| Rejected orders produce EXEC_REJECTED and NO market data | E2E journals |
| Iceberg shows only display_qty in market data | CME E2E journals (Task 17) |
| Auction fills at single equilibrium price in market data | CME E2E journals |
| Cross-instrument isolation | CME E2E journals |
| SMP produces EXEC_CANCELLED, no MD_TRADE | CME E2E journals |
| Full CME trading day lifecycle | CME E2E journals |

---

## 8. File Structure (Target)

```
cme/
├── cme_exchange.h              # CRTP class (done)
├── cme_products.h              # Product configs (done)
├── cme_simulator.h             # Multi-instrument wrapper (done)
├── cme_sim_runner.cc           # Live simulator binary (Task 20)
├── sim_config.h                # Runtime config (Task 21)
├── PROTOCOLS.md                # Protocol documentation (done)
├── ilink3/
│   └── ilinkbinary.xml         # SBE schema (done)
├── mdp3/
│   └── templates_FixBinary.xml # SBE schema (done)
├── codec/
│   ├── BUILD.bazel
│   ├── sbe_header.h            # Task 1
│   ├── ilink3_messages.h       # Task 2
│   ├── ilink3_encoder.h/.cc    # Task 4
│   ├── ilink3_decoder.h/.cc    # Task 5
│   ├── mdp3_messages.h         # Task 3
│   ├── mdp3_encoder.h/.cc      # Task 6
│   ├── mdp3_decoder.h/.cc      # Task 7
│   └── codec_roundtrip_test.cc # Task 8
├── ilink3_report_publisher.h   # Task 9
├── mdp3_feed_publisher.h       # Task 10
├── ilink3_gateway.h            # Task 11
└── e2e/
    ├── BUILD.bazel
    ├── e2e_test_runner.h/.cc   # Task 13
    └── e2e_journal_test.cc     # Task 14

test-journals/
├── e2e/                         # Generic E2E scenarios (Task 16)
│   ├── e2e_basic_limit.journal
│   ├── e2e_fill.journal
│   ├── e2e_cancel.journal
│   └── ...
├── cme/                         # CME-specific E2E scenarios (Task 17)
│   ├── cme_e2e_es_trading_day.journal
│   ├── cme_e2e_smp.journal
│   └── ...
└── (existing 87 engine-level journals)
```
