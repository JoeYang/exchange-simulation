# Exchange Tooling Plan

**Date:** 2026-03-25
**Status:** Plan

---

## 1. Current Tooling (Built)

| Tool | Location | Purpose |
|------|----------|---------|
| `exchange-viz-replay` | `tools/viz_replay.cc` | Offline journal visualizer (FTXUI TUI) |
| `exchange-viz-live` | `tools/viz_live.cc` | Live shared memory viewer (ANSI terminal) |
| `exchange_benchmark` | `benchmarks/exchange_benchmark.cc` | Latency microbenchmark (8 scenarios) |
| `throughput_boundary` | `benchmarks/throughput_boundary.cc` | Saturation point finder |
| `generate_stress_journals` | `test-journals/generate_stress_journals.cc` | Stress test journal generator |
| `generate_auction_journals` | `test-journals/generate_auction_journals.cc` | Auction scenario generator |
| `generate_economics_journals` | `test-journals/generate_economics_journals.cc` | Economics verification generator |
| `generate_coverage_journals` | `test-journals/generate_coverage_journals.cc` | Coverage gap journal generator |
| `run_all_journals.sh` | `test-journals/run_all_journals.sh` | Pretty journal test runner |
| `run_benchmark.sh` | `benchmarks/run_benchmark.sh` | Benchmark runner with CPU pinning |

**Supporting libraries:**
- `OrderbookState` — reconstruct book from events
- `TuiRenderer` — FTXUI rendering components
- `ShmTransport` — shared memory ring buffer transport
- `ShmListeners` — event publishers to shared memory

---

## 2. Planned Tooling (Phase 3-4)

### 2.1 SBE Codec Generator

**Purpose:** Parse iLink3 and MDP3.0 SBE XML schema files and generate zero-copy C++ encoders/decoders for all message types.

**Why:** The CME protocol uses Simple Binary Encoding (SBE) — a fixed-layout binary format for ultra-low-latency serialization. We need codecs to:
- Encode engine callbacks as MDP3 market data messages
- Decode iLink3 order entry messages into OrderRequest structs
- Validate messages against the schema

**Input:** `cme/ilink3/ilinkbinary.xml`, `cme/mdp3/templates_FixBinary.xml`

**Output:** Generated C++ headers with zero-copy encode/decode for each message type.

```
cme/generated/
├── ilink3/
│   ├── NewOrderSingle514.h
│   ├── OrderCancelRequest516.h
│   ├── ExecutionReportNew522.h
│   ├── ... (48 messages)
│   └── ilink3_messages.h    # umbrella include
└── mdp3/
    ├── MDIncrementalRefreshBook46.h
    ├── MDIncrementalRefreshTradeSummary48.h
    ├── ... (31 messages)
    └── mdp3_messages.h      # umbrella include
```

**Implementation approach:**
- **Option A:** Use an existing SBE tool (e.g., `real-logic/simple-binary-encoding` Java tool)
- **Option B:** Write our own XML parser → C++ codegen in Python or C++
- **Recommended: Option B** — gives us full control, avoids Java dependency

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T1 | `tools/sbe_codegen.py` | ~500 | Parse SBE XML schema, generate C++ headers |
| T2 | `cme/generated/ilink3/*.h` | ~2000 (generated) | iLink3 message codecs |
| T3 | `cme/generated/mdp3/*.h` | ~1500 (generated) | MDP3 message codecs |
| T4 | `tools/sbe_codegen_test.cc` | ~200 | Verify generated code compiles and round-trips |

### 2.2 Protocol Adapter Layer

**Purpose:** Bridge between the matching engine's internal events and SBE-encoded wire protocol messages.

```
                    iLink3 Messages (SBE bytes)
                           |
                    +------v-------+
                    | iLink3Adapter |  decode → OrderRequest
                    +------+-------+
                           |
                    +------v-------+
                    | MatchingEngine|
                    +------+-------+
                           |
                    +------v-------+
                    | MDP3Publisher |  encode ← engine callbacks
                    +------+-------+
                           |
                    MDP3 Messages (SBE bytes)
```

**iLink3 Adapter (Order Entry):**
```cpp
// cme/ilink3_adapter.h
class ILink3Adapter {
public:
    // Decode SBE bytes → OrderRequest
    OrderRequest decode_new_order(const char* buffer, size_t len);
    ModifyRequest decode_modify(const char* buffer, size_t len);
    OrderId decode_cancel(const char* buffer, size_t len);

    // Encode engine callbacks → SBE bytes (Execution Reports)
    size_t encode_execution_report_new(const OrderAccepted& event, char* buffer);
    size_t encode_execution_report_fill(const OrderFilled& event, char* buffer);
    size_t encode_execution_report_cancel(const OrderCancelled& event, char* buffer);
    size_t encode_order_cancel_reject(const OrderCancelRejected& event, char* buffer);
};
```

**MDP3 Publisher (Market Data):**
```cpp
// cme/mdp3_publisher.h
class MDP3Publisher : public MarketDataListenerBase {
    // Encodes market data events as MDP3 SBE messages
    // Can publish to: multicast, shared memory, or file

    void on_depth_update(const DepthUpdate& e) override;
    void on_trade(const Trade& e) override;
    void on_top_of_book(const TopOfBook& e) override;
    void on_market_status(const MarketStatus& e) override;
};
```

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T5 | `cme/ilink3_adapter.h/.cc` | ~300 | Decode iLink3 → OrderRequest, encode → ExecutionReport |
| T6 | `cme/ilink3_adapter_test.cc` | ~200 | Round-trip encode/decode tests |
| T7 | `cme/mdp3_publisher.h/.cc` | ~250 | Encode engine callbacks → MDP3 messages |
| T8 | `cme/mdp3_publisher_test.cc` | ~200 | Verify correct encoding of each event type |

### 2.3 Network Transport

**Purpose:** Send and receive SBE-encoded messages over the network (TCP for iLink3, multicast UDP for MDP3).

```
Clients (FIX gateways)  ←TCP→  [iLink3 Gateway]  →  MatchingEngine
                                                          ↓
Market Data Consumers   ←UDP←  [MDP3 Multicast]   ←  MDP3Publisher
```

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T9 | `tools/tcp_server.h/.cc` | ~200 | Async TCP server (epoll-based, for iLink3) |
| T10 | `tools/udp_multicast.h/.cc` | ~150 | UDP multicast publisher (for MDP3) |
| T11 | `cme/ilink3_gateway.h/.cc` | ~300 | TCP server + iLink3Adapter + engine routing |
| T12 | `cme/mdp3_feed.h/.cc` | ~200 | MDP3Publisher + UDP multicast |

### 2.4 Simulation Runner

**Purpose:** A top-level binary that runs a full CME exchange simulation with:
- Multiple instruments loaded from config
- Session lifecycle management (timed transitions)
- iLink3 gateway accepting client connections
- MDP3 feed publishing market data
- Shared memory visualization

```
cme/cme_sim_runner.cc — main binary

Usage:
  cme-sim --products cme/cme_products.json \
          --ilink3-port 9100 \
          --mdp3-group 224.0.31.1:14310 \
          --viz-shm /cme-viz
```

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T13 | `cme/cme_sim_runner.cc` | ~300 | Main binary: load products, start gateway + feed |
| T14 | `cme/sim_config.h` | ~80 | Runtime configuration (ports, groups, products) |

### 2.5 Replay Tools

**Purpose:** Replay recorded market data or order flow through the simulator for backtesting and validation.

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T15 | `tools/pcap_reader.h/.cc` | ~200 | Read pcap files, extract UDP/TCP payloads |
| T16 | `tools/mdp3_replayer.cc` | ~150 | Replay MDP3 pcap through decoder, compare with simulator output |
| T17 | `tools/ilink3_replayer.cc` | ~150 | Replay iLink3 order flow through simulator |

### 2.6 Monitoring & Diagnostics

| Task | Files | Est. Lines | Description |
|------|-------|-----------|-------------|
| T18 | `tools/exchange_dashboard.cc` | ~300 | Enhanced FTXUI dashboard: all instruments, OHLCV, session state, throughput |
| T19 | `tools/latency_logger.h` | ~100 | Per-order latency logging (entry → ack, entry → fill) |
| T20 | `tools/event_logger.h/.cc` | ~150 | Structured event log (JSON or binary) for post-analysis |

---

## 3. Implementation Priority

### Phase 3 (Current — with CME simulator)

| Priority | Tasks | Description |
|----------|-------|-------------|
| **P1** | T1-T4 | SBE codec generator — unblocks all protocol work |
| **P2** | T5-T8 | Protocol adapters — encode/decode iLink3 + MDP3 |
| **P3** | T13-T14 | Simulation runner — runnable CME simulator binary |

### Phase 4 (Network + Replay)

| Priority | Tasks | Description |
|----------|-------|-------------|
| **P4** | T9-T12 | Network transport (TCP + UDP multicast) |
| **P5** | T15-T17 | Replay tools (pcap reader, MDP3/iLink3 replay) |
| **P6** | T18-T20 | Monitoring & diagnostics |

### Dependency Graph

```
Phase 3:
  [T1] SBE Codegen
    |
    +→ [T2] iLink3 generated headers
    +→ [T3] MDP3 generated headers
    |       |
    +→ [T4] Codegen tests
            |
  [T5] iLink3 Adapter ←──── T2
  [T6] iLink3 Adapter Tests
  [T7] MDP3 Publisher  ←──── T3
  [T8] MDP3 Publisher Tests
            |
  [T13] Sim Runner ←── T5 + T7 + CME Simulator
  [T14] Sim Config

Phase 4:
  [T9]  TCP Server
  [T10] UDP Multicast
            |
  [T11] iLink3 Gateway ←── T5 + T9
  [T12] MDP3 Feed      ←── T7 + T10
            |
  [T15] pcap Reader
  [T16] MDP3 Replayer  ←── T3 + T15
  [T17] iLink3 Replayer ←── T2 + T15
            |
  [T18] Dashboard
  [T19] Latency Logger
  [T20] Event Logger
```

---

## 4. Effort Estimates

| Phase | Tasks | Est. Lines | Description |
|-------|-------|-----------|-------------|
| Phase 3 Tooling | T1-T8, T13-T14 | ~5,000 | SBE codegen, adapters, sim runner |
| Phase 4 Tooling | T9-T20 | ~4,000 | Network, replay, monitoring |
| **Total** | **T1-T20** | **~9,000** | All tooling |

---

## 5. File Structure (Target)

```
exchange/
├── exchange-core/        # Matching engine (done)
├── exchange-sim/         # Multi-instrument framework (done)
├── test-harness/         # Journal replay framework (done)
├── test-journals/        # 87 test scenarios (done)
├── cme/
│   ├── cme_exchange.h    # CME CRTP class (done)
│   ├── cme_products.h    # Product configs (done)
│   ├── cme_simulator.h   # CME simulator wrapper (in progress)
│   ├── ilink3/
│   │   ├── ilinkbinary.xml          # SBE schema
│   │   └── ilink3_adapter.h/.cc     # Protocol adapter
│   ├── mdp3/
│   │   ├── templates_FixBinary.xml  # SBE schema
│   │   └── mdp3_publisher.h/.cc     # Market data encoder
│   ├── generated/                    # SBE codegen output
│   │   ├── ilink3/*.h
│   │   └── mdp3/*.h
│   ├── cme_sim_runner.cc            # Main simulator binary
│   └── sim_config.h                 # Runtime config
├── tools/
│   ├── sbe_codegen.py               # SBE XML → C++ codegen
│   ├── tcp_server.h/.cc             # TCP server
│   ├── udp_multicast.h/.cc          # UDP multicast publisher
│   ├── pcap_reader.h/.cc            # pcap file reader
│   ├── exchange_dashboard.cc        # Enhanced monitoring TUI
│   ├── latency_logger.h             # Per-order latency tracking
│   ├── event_logger.h/.cc           # Structured event log
│   └── (existing viz tools)
├── benchmarks/                       # Performance benchmarks (done)
└── docs/                             # Design specs and analysis
```
