# Exchange Simulation Exercise Plan

**Date:** 2026-03-27
**Status:** Plan

---

## 1. Overview

Build three binaries that together form a multi-client exchange simulation exercise:

1. **exchange-trader** -- autonomous trading client (TCP order entry, strategy engine, journal writer)
2. **exchange-observer** -- market data subscriber (UDP multicast, ANSI terminal display)
3. **journal-reconciler** -- post-trade reconciliation of trader journals vs observer journal

The simulation connects N trader clients to the running exchange (cme-sim or ice-sim) via TCP, while the observer subscribes to the UDP multicast market data feed and records what it sees. Both perspectives capture the same events independently. After the run, the reconciler merges all trader journals and cross-references them against the observer's journal to verify consistency.

```
                          +-----------+
                          |  Exchange  |
                          | (cme-sim  |
                          |  ice-sim) |
                          +-----+-----+
                 TCP order entry |  UDP multicast market data
                  (iLink3/FIX)  |  (MDP3/iMpact)
          +-----+-----+---------+--------+------------------+
          |     |      |        |        |                   |
       +--+--+ +--+--+ +--+--+ +--+--+ +--+--+     +-------+------+
       | T-1 | | T-2 | | T-3 | | T-4 | | T-5 |     | observer     |
       +--+--+ +--+--+ +--+--+ +--+--+ +--+--+     | (MD journal) |
          |       |       |       |       |          +-------+------+
          v       v       v       v       v                  |
        c1.j    c2.j    c3.j    c4.j    c5.j           observer.j
          |       |       |       |       |                  |
          +---+---+---+---+---+---+                          |
              |                                              |
              v   (consolidated traders)                     v
        +-----+----------------------------------------------+--+
        |              journal-reconciler                        |
        |  traders consolidated journal  <-->  observer journal  |
        +-------------------------------------------------------+
```

---

## 2. Design Decisions

### 2.1 TCP Client -- Reuse vs New

**Decision:** Extract `TcpClient` from `ilink3_send_order.cc` into a standalone header `tools/tcp_client.h`.

| Option | Pros | Cons |
|--------|------|------|
| Copy TcpClient into each binary | No shared code to maintain | Duplication across 1+ binaries |
| **Extract to `tcp_client.h`** | Single source of truth; testable in isolation; ~50 lines | New header file |
| Use raw sockets inline | No abstraction overhead | Harder to test; repeated boilerplate |

The existing `TcpClient` in `ilink3_send_order.cc` (lines 190-300) is already a clean RAII class with length-prefixed framing. We extract it as-is plus add `set_nonblocking()` for the poll loop.

### 2.2 Protocol Abstraction

**Decision:** Use a `ProtocolCodec` interface to abstract CME (iLink3 SBE) vs ICE (FIX 4.2) encoding/decoding.

```
+-------------------+
| ProtocolCodec     |  (abstract)
+-------------------+
| encode_new_order  |
| encode_cancel     |
| encode_replace    |
| decode_response   |
+-------------------+
        ^
        |
   +----+----+
   |         |
+--+--+   +--+--+
| CME |   | ICE |
+-----+   +-----+
```

This keeps the strategy and sim_client code exchange-agnostic. The codec is selected at startup via `--exchange cme|ice`.

### 2.3 Strategy Architecture

**Decision:** Strategies are pure functions that produce order actions given current state. No inheritance -- just a function pointer / `std::function`.

```cpp
using StrategyTick = std::function<std::vector<OrderAction>(const ClientState&)>;
```

| Option | Pros | Cons |
|--------|------|------|
| Virtual base class | Familiar OOP pattern | Heavier; overkill for 2 strategies |
| **Function pointer** | Simple; easy to test with deterministic seed | Less encapsulation |

Strategies receive a `const ClientState&` (open orders, position, last fill price, ref price) and return a vector of `OrderAction` (new/cancel/modify). The caller rate-limits to `--rate` actions/sec.

### 2.4 Journal Format

**Trader journals** use the ACTION/EXPECT format from `journal_parser.h`:

```
ACTION ILINK3_NEW_ORDER ts=1711500000000000000 client_id=1 instrument=ES cl_ord_id=1 side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT ORDER_ACCEPTED ts=1711500000012000000 order_id=42 cl_ord_id=1
EXPECT EXEC_FILL ts=1711500000015000000 order_id=42 cl_ord_id=1 fill_price=50000000 fill_qty=10000 leaves_qty=0
```

For ICE:
```
ACTION ICE_FIX_NEW_ORDER ts=1711500000000000000 client_id=2 instrument=B cl_ord_id=1 side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY
EXPECT ORDER_ACCEPTED ts=1711500000012000000 order_id=42 cl_ord_id=1
```

ACTION lines are written when the client sends an order. EXPECT lines are written when execution reports are received from the exchange (not predicted). Each EXPECT includes the nanosecond timestamp from the exec report.

**Observer journal** uses EXPECT-style lines for each decoded market data message:

```
EXPECT MD_TRADE ts=1711500000020000000 instrument=ES price=50000000 qty=10000 aggressor_side=BUY
EXPECT MD_BOOK_ADD ts=1711500000021000000 instrument=ES side=BUY price=49995000 qty=10000 order_count=1
EXPECT MD_BOOK_UPDATE ts=1711500000022000000 instrument=ES side=ASK price=50005000 qty=20000 order_count=2
EXPECT MD_BOOK_DELETE ts=1711500000023000000 instrument=ES side=BUY price=49990000
```

The reconciler cross-references consolidated trader journals against the observer journal.

---

## 3. File Structure

```
tools/
  tcp_client.h            -- Extracted TcpClient (RAII, length-prefix framing, non-blocking)
  sim_client.h            -- SimClient: TCP conn + ProtocolCodec + order state + journal writer
  trading_strategy.h      -- StrategyTick + random-walk + market-maker implementations
  exchange_trader.cc      -- main() for exchange-trader binary
  exchange_observer.cc    -- main() for exchange-observer binary (ANSI display + journal writer)
  journal_reconciler.h    -- Reconciler core: merge, match, invariant checks
  journal_reconciler.cc   -- main() for journal-reconciler binary
  BUILD.bazel             -- (amended with 3 new cc_binary + 2 cc_library + 4 cc_test targets)
```

**Estimated line counts:**

| File | Lines | Notes |
|------|-------|-------|
| `tcp_client.h` | ~80 | Extract from ilink3_send_order.cc + add set_nonblocking() |
| `sim_client.h` | ~200 | TCP connection, order state map, P&L tracking, journal writer |
| `trading_strategy.h` | ~200 | Strategy interface, random-walk, market-maker |
| `exchange_trader.cc` | ~250 | CLI parsing, signal handler, main poll loop |
| `exchange_observer.cc` | ~250 | CLI, multicast join, decode + ANSI display + journal writer |
| `journal_reconciler.h` | ~200 | Consolidate traders, match vs observer, 8 invariant checks |
| `journal_reconciler.cc` | ~150 | CLI parsing, read journals, print report |

---

## 4. Component Details

### 4.1 tcp_client.h

Extracted from `ilink3_send_order.cc:190-300`. Additions:

- `bool set_nonblocking()` -- sets `O_NONBLOCK` via `fcntl` for use in poll loops
- `ssize_t try_recv_message(char* buf, size_t max_len)` -- non-blocking recv that returns 0 on `EAGAIN`

### 4.2 sim_client.h -- SimClient

```
SimClient
  |-- TcpClient tcp_
  |-- ProtocolCodec* codec_         // CME or ICE encoder/decoder
  |-- map<uint64_t, OpenOrder> orders_  // keyed by cl_ord_id
  |-- int64_t position_             // net position in contracts
  |-- int64_t realized_pnl_         // realized P&L in fixed-point
  |-- uint32_t fill_count_
  |-- uint64_t next_cl_ord_id_
  |-- FILE* journal_fd_
```

Key methods:
- `bool connect(host, port)` -- TCP connect
- `bool send_new_order(side, price, qty, type, tif)` -- encode + send + journal ACTION
- `bool send_cancel(cl_ord_id)` -- encode cancel + send + journal ACTION
- `bool send_replace(cl_ord_id, new_price, new_qty)` -- encode replace + send + journal ACTION
- `int poll_responses()` -- non-blocking recv, decode exec reports, update state, journal EXPECT
- `void cancel_all_open()` -- graceful shutdown: cancel each open order
- `std::string status_line()` -- formatted status for stderr display

### 4.3 trading_strategy.h

```cpp
struct OrderAction {
    enum Type { New, Cancel, Modify } type;
    Side side;
    Price price;
    Quantity qty;
    uint64_t target_cl_ord_id;  // for cancel/modify
    Price new_price;             // for modify
};

struct StrategyState {
    const std::unordered_map<uint64_t, OpenOrder>& open_orders;
    int64_t position;
    int64_t max_position;
    Price ref_price;
    Price spread;
    Price last_fill_price;
    Timestamp now;
};

using StrategyTick = std::vector<OrderAction>(*)(StrategyState& state, std::mt19937& rng);
```

**random-walk strategy:**
```
tick(state, rng):
  1. Count open orders per side
  2. If fewer than 3 on a side AND position allows:
     - Sample price from [ref_price - spread, ref_price + spread]
     - Snap to tick grid
     - Emit New order
  3. For each open order older than random(1-5s):
     - 80% probability: emit Cancel
     - 20% probability: emit Modify (resample price)
  4. If last_fill_price != 0:
     - Adapt ref_price = 0.9 * ref_price + 0.1 * last_fill_price
```

**market-maker strategy:**
```
tick(state, rng):
  1. Compute mid = ref_price, adjusted for position lean:
     - lean = position * tick_size / 10
     - bid_target = mid - spread/2 - lean
     - ask_target = mid + spread/2 - lean
  2. If no bid open or bid price != bid_target: cancel old bid, emit New bid
  3. If no ask open or ask price != ask_target: cancel old ask, emit New ask
  4. On fill (detected by comparing fill_count): re-quote filled side immediately
  5. Every N seconds: cancel and re-quote both sides
```

### 4.4 exchange_trader.cc -- Main Loop

```
main():
  1. Parse CLI args (--exchange, --host, --port, --client-id, --account,
     --instrument, --strategy, --ref-price, --spread, --rate, --max-position, --journal)
  2. Resolve instrument (cme_products / ice_products)
  3. Create ProtocolCodec (CME or ICE)
  4. Create SimClient, connect to exchange
  5. Install SIGINT handler (sets running_ = false)
  6. Main loop (while running_):
     a. client.poll_responses()         // non-blocking TCP recv
     b. If rate limiter allows:
        - actions = strategy(state, rng)
        - For each action: client.send_*()
     c. If 1 second elapsed: print status line to stderr
  7. Graceful shutdown: client.cancel_all_open(), print summary
```

Rate limiting: track `actions_this_second` counter, reset every second. Skip strategy tick if at limit.

### 4.5 exchange_observer.cc

CLI (updated with `--journal`):
```
exchange-observer --exchange cme|ice \
    --group 239.0.31.1 --port 14310 \
    --instrument ES \
    --journal /tmp/observer.journal
```

```
main():
  1. Parse CLI args (--exchange, --group, --port, --instrument, --journal)
  2. Create UdpMulticastReceiver, join group
  3. Open journal file for writing (if --journal specified)
  4. Allocate display state: 5-level book (bid/ask), last 10 trades, OHLCV, msg counter
  5. Install SIGINT handler
  6. Main loop (while running_):
     a. Set recv timeout to 100ms
     b. recv datagram
     c. Skip McastSeqHeader (4 bytes)
     d. Decode: MDP3 (decode_mdp3_message) or iMpact (decode_messages)
     e. Update display state from decoded messages
     f. Write EXPECT lines to journal for each decoded event:
        - RefreshBook46 entries -> MD_BOOK_ADD / MD_BOOK_UPDATE / MD_BOOK_DELETE
        - TradeSummary48 entries -> MD_TRADE
        - SecurityStatus30 -> MD_STATUS
     g. Every 100ms: render ANSI display to stderr
  7. On exit: print summary (total msgs, trades, final book state)
```

Journal output per message type:

| MDP3 Message | Observer Journal Line |
|---|---|
| RefreshBook46 (Add action) | `EXPECT MD_BOOK_ADD ts=... instrument=... side=... price=... qty=... order_count=...` |
| RefreshBook46 (Change action) | `EXPECT MD_BOOK_UPDATE ts=... instrument=... side=... price=... qty=... order_count=...` |
| RefreshBook46 (Delete action) | `EXPECT MD_BOOK_DELETE ts=... instrument=... side=... price=...` |
| TradeSummary48 | `EXPECT MD_TRADE ts=... instrument=... price=... qty=... aggressor_side=...` |
| SecurityStatus30 | `EXPECT MD_STATUS ts=... instrument=... state=...` |

For ICE iMpact:

| iMpact Message | Observer Journal Line |
|---|---|
| AddModifyOrder | `EXPECT MD_BOOK_ADD ts=... instrument=... side=... price=... qty=...` |
| OrderWithdrawal | `EXPECT MD_BOOK_DELETE ts=... instrument=... side=... price=...` |
| DealTrade | `EXPECT MD_TRADE ts=... instrument=... price=... qty=...` |
| MarketStatus | `EXPECT MD_STATUS ts=... instrument=... state=...` |

ANSI display (no FTXUI dependency -- raw escape codes for portability):
```
ES  Live Market Data                    msgs/sec: 1234
-----------------------------------------------------
         BID                  ASK
  Qty    Price     |    Price    Qty
  150    5002.50   |    5002.75  120
   80    5002.25   |    5003.00   95
   45    5002.00   |    5003.25   60
   30    5001.75   |    5003.50   40
   15    5001.50   |    5003.75   25

Last 10 Trades:
  5002.75  x10  BUY   12:34:56.123
  5002.50  x5   SELL  12:34:55.987
  ...

OHLCV: O=5000.00 H=5005.00 L=4998.50 C=5002.75 V=1234
```

### 4.6 journal_reconciler.cc

CLI (updated -- reconciles traders vs observer):
```
journal-reconciler \
    --observer-journal /tmp/observer.journal \
    --client-journals /tmp/client_1.journal,/tmp/client_2.journal,...,/tmp/client_5.journal
```

```
main():
  1. Parse CLI args (--observer-journal, --client-journals)
  2. For each client journal:
     a. Read all lines, parse ACTION and EXPECT lines
     b. Tag each entry with client_id (from filename or ACTION field)
  3. Read observer journal (EXPECT-only lines)
  4. Consolidate all trader entries, sort by timestamp
  5. Run reconciliation checks (see Section 4.7 for invariants)
  6. Print report
```

### 4.7 Reconciliation Invariants

The reconciler verifies these invariants between the consolidated trader journals and the observer journal:

**Invariant 1: Trade Matching**
Every `MD_TRADE` in the observer journal must match exactly 2 `EXEC_FILL` entries in the consolidated trader journals (one aggressor, one resting). Match criteria: price and quantity must be equal. Ordering within the same nanosecond is by fill sequence.

```
Observer:  EXPECT MD_TRADE ts=T1 price=50000000 qty=10000
Traders:   EXPECT EXEC_FILL ts=T1-delta order_id=42 fill_price=50000000 fill_qty=10000 (aggressor)
           EXPECT EXEC_FILL ts=T1-delta order_id=17 fill_price=50000000 fill_qty=10000 (resting)
```

**Invariant 2: Book Add Traceability**
Every `MD_BOOK_ADD` in the observer journal must trace to a client `ACTION *_NEW_ORDER` that was subsequently acknowledged (`EXPECT ORDER_ACCEPTED`). The price and side must match.

**Invariant 3: Book Delete Traceability**
Every `MD_BOOK_DELETE` in the observer journal must trace to either:
- A client `ACTION *_CANCEL` (user cancel), or
- An `EXEC_FILL` with `leaves_qty=0` (fully filled, removed from book)

**Invariant 4: Event Ordering**
The sequence of market-observable events in the observer journal must be consistent with the consolidated trader journal. Specifically:
- If trader A's new order appears before trader B's cancel in wall-clock time, and both produce observable book events, the observer must see them in the same order.
- Timestamps may differ due to client->exchange->multicast latency, but the relative ordering of events is ground truth.

**Invariant 5: No Phantom Events**
Every event in the observer journal must be traceable to a client action. An `MD_TRADE` with no matching `EXEC_FILL` pair is a phantom. An `MD_BOOK_ADD` with no matching `ORDER_ACCEPTED` is a phantom. Phantoms indicate a bug.

**Invariant 6: No Lost Events**
Every client action that was acknowledged by the exchange must have a corresponding observable effect in the observer journal:
- `ORDER_ACCEPTED` -> `MD_BOOK_ADD` (unless immediately filled, which produces `MD_TRADE` instead)
- `EXEC_FILL` -> `MD_TRADE`
- `ORDER_CANCELLED` -> `MD_BOOK_DELETE`
- `ORDER_REJECTED` -> no observer event expected (correct -- rejects are not market-observable)

**Invariant 7: Final Book State**
The observer's accumulated book state (net of all `MD_BOOK_ADD`, `MD_BOOK_UPDATE`, `MD_BOOK_DELETE` events) must equal the net of all client open orders minus fills and cancels. At simulation end, if all clients cancel their open orders (graceful shutdown), the observer's book should be empty.

**Invariant 8: Fill Price/Quantity Consistency**
For every matched trade, the fill price in the trader's `EXEC_FILL` must exactly equal the price in the observer's `MD_TRADE`. Quantities must also match exactly (no rounding or truncation).

**Report output:**

```
Reconciliation Report
=====================
Clients: 5
Observer events:   2,450

Trade Matching (Invariant 1):
  Observer trades:     200
  Matched (2 fills):   198  (99.0%)
  Unmatched:             2  (1.0%)

Book Traceability (Invariants 2-3):
  MD_BOOK_ADD:         800
  Traced to NEW_ORDER: 800  (100.0%)
  MD_BOOK_DELETE:      750
  Traced to CANCEL:    600  (80.0%)
  Traced to FILL:      150  (20.0%)

Event Ordering (Invariant 4):
  Order violations:      0

Phantom Events (Invariant 5):
  Phantom trades:        0
  Phantom book adds:     0

Lost Events (Invariant 6):
  ORDER_ACCEPTED w/o MD_BOOK_ADD or MD_TRADE:   0
  EXEC_FILL w/o MD_TRADE:                       0
  ORDER_CANCELLED w/o MD_BOOK_DELETE:            0

Final Book State (Invariant 7):
  Observer book levels remaining:  0  (clean shutdown)

Fill Consistency (Invariant 8):
  Price mismatches:    0
  Qty mismatches:      0

Latency Distribution (client action -> observer sees effect):
  p50:    45 us
  p95:   120 us
  p99:   350 us
  max:   800 us
```

---

## 5. Task Breakdown

### Task Dependency DAG

```
T1 (tcp_client.h)
 |
 +---> T3 (sim_client.h) ----+
 |                            |
T2 (trading_strategy.h)      |
 |                            v
 +---> T7 (exchange_trader.cc)

T4 (exchange_observer.cc + journal writer)

T5 (journal_reconciler.h -- invariant checks)
 |
 +--> T6 (journal_reconciler.cc main + BUILD)

T8 (BUILD.bazel) depends on T1-T7
T9 (integration test) depends on T7, T4, T6

T8 depends on T7 depends on T3 + T2
T3 depends on T1
T4, T5 are independent of each other and of T1-T3
T6 depends on T5
T9 depends on T7, T4, T6
```

### Task List

| Task | Description | File(s) | Est. Lines | Depends On | Dev |
|------|-------------|---------|------------|------------|-----|
| T1 | Extract TcpClient to header + unit test | `tcp_client.h`, `tcp_client_test.cc` | ~120 | -- | Dev A |
| T2 | Trading strategies + unit tests (deterministic seed) | `trading_strategy.h`, `trading_strategy_test.cc` | ~200 + ~150 | -- | Dev B |
| T3 | SimClient: TCP conn + order state + journal writer + unit test | `sim_client.h`, `sim_client_test.cc` | ~200 + ~150 | T1 | Dev A |
| T4 | exchange-observer binary (ANSI display + journal writer) + test | `exchange_observer.cc`, `exchange_observer_test.cc` | ~250 + ~100 | -- | Dev C |
| T5 | Reconciler core: consolidate traders, match vs observer, 8 invariant checks + unit test | `journal_reconciler.h`, `journal_reconciler_test.cc` | ~200 + ~180 | -- | Dev D |
| T6 | journal-reconciler binary (CLI + report) + BUILD target | `journal_reconciler.cc` | ~150 | T5 | Dev D |
| T7 | exchange-trader binary (CLI + main loop) | `exchange_trader.cc` | ~250 | T2, T3 | Dev A |
| T8 | BUILD.bazel targets for all 3 binaries + libraries | `tools/BUILD.bazel` | ~80 | T1-T7 | Dev A |
| T9 | Integration test: 5 traders + observer, 5s run, reconcile | `tools/sim_integration_test.sh` | ~100 | T7, T4, T6 | Dev B |

### Commit Sequence

Each task = one commit. Commits must keep the build green.

```
Commit 1: feat(tools): extract TcpClient to standalone header      [T1]
Commit 2: feat(tools): add trading strategy interface + impls       [T2]
Commit 3: feat(tools): add SimClient (order state + journal)        [T3]
Commit 4: feat(tools): add exchange-observer with journal output    [T4]
Commit 5: feat(tools): add journal reconciler core (8 invariants)   [T5]
Commit 6: feat(tools): add journal-reconciler binary                [T6]
Commit 7: feat(tools): add exchange-trader binary                   [T7]
Commit 8: feat(tools): add BUILD targets for simulation binaries    [T8]
Commit 9: test(tools): add 5-trader simulation integration test     [T9]
```

---

## 6. Dev Dispatch Strategy (4 Devs)

### Phase 1 -- Parallel Foundation (T1, T2, T4, T5)

All four tasks are independent. Maximum parallelism.

```
Dev A: T1 (tcp_client.h)                      -- no deps
Dev B: T2 (trading_strategy.h)                -- no deps
Dev C: T4 (exchange_observer + journal)       -- no deps
Dev D: T5 (reconciler core, 8 invariants)     -- no deps
```

### Phase 2 -- Sequential Build-Up (T3, T6)

```
Dev A: T3 (sim_client.h)            -- depends on T1 (own work)
Dev D: T6 (journal-reconciler main) -- depends on T5 (own work)
Dev B: idle (review T1, T4)
Dev C: idle (review T2, T5)
```

### Phase 3 -- Assembly (T7, T8, T9)

```
Dev A: T7 (exchange_trader.cc)      -- depends on T2 + T3
Dev A: T8 (BUILD.bazel)             -- depends on all library tasks
Dev B: T9 (integration: 5 traders + observer + reconcile)
Dev C: code review
Dev D: code review
```

### Timeline (Estimated)

```
          Dev A        Dev B        Dev C        Dev D
Phase 1   T1           T2           T4           T5
Phase 2   T3           review       review       T6
Phase 3   T7 + T8      T9           review       review
```

---

## 7. Bazel BUILD Targets (Preview)

```python
# tools/BUILD.bazel additions

cc_library(
    name = "tcp_client",
    hdrs = ["tcp_client.h"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "tcp_client_test",
    srcs = ["tcp_client_test.cc"],
    deps = [":tcp_client", ":tcp_server", "@googletest//:gtest_main"],
)

cc_library(
    name = "trading_strategy",
    hdrs = ["trading_strategy.h"],
    deps = ["//exchange-core:types"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "trading_strategy_test",
    srcs = ["trading_strategy_test.cc"],
    deps = [":trading_strategy", "@googletest//:gtest_main"],
)

cc_library(
    name = "sim_client",
    hdrs = ["sim_client.h"],
    deps = [
        ":tcp_client",
        ":trading_strategy",
        "//cme/codec:ilink3_encoder",
        "//cme/codec:ilink3_decoder",
        "//ice/fix:fix_encoder",
        "//ice/fix:fix_parser",
        "//cme:cme_products",
        "//ice:ice_products",
        "//exchange-core:types",
    ],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "sim_client_test",
    srcs = ["sim_client_test.cc"],
    deps = [
        ":sim_client",
        ":tcp_server",
        "@googletest//:gtest_main",
    ],
)

cc_library(
    name = "journal_reconciler_lib",
    hdrs = ["journal_reconciler.h"],
    deps = ["//test-harness:journal_parser"],
    visibility = ["//visibility:public"],
)

cc_test(
    name = "journal_reconciler_test",
    srcs = ["journal_reconciler_test.cc"],
    deps = [":journal_reconciler_lib", "@googletest//:gtest_main"],
)

cc_binary(
    name = "exchange-trader",
    srcs = ["exchange_trader.cc"],
    deps = [
        ":sim_client",
        ":trading_strategy",
    ],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "exchange-observer",
    srcs = ["exchange_observer.cc"],
    deps = [
        ":udp_multicast",
        "//cme/codec:mdp3_decoder",
        "//ice/impact:impact_decoder",
        "//cme:cme_products",
        "//ice:ice_products",
    ],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "journal-reconciler",
    srcs = ["journal_reconciler.cc"],
    deps = [":journal_reconciler_lib"],
    visibility = ["//visibility:public"],
)
```

---

## 8. Testing Strategy

### Unit Tests

| Test File | What It Covers |
|-----------|---------------|
| `tcp_client_test.cc` | Connect to a TcpServer, send/recv length-prefixed messages, non-blocking recv returns 0 on empty, connection refused handling |
| `trading_strategy_test.cc` | Deterministic output with fixed seed; random-walk generates 1-3 orders per side; market-maker always quotes bid+ask; position limit respected; ref_price adaptation |
| `sim_client_test.cc` | Journal output format correctness; order state tracking (new -> ack -> fill updates position/P&L); cancel removes from open orders; status line format |
| `journal_reconciler_test.cc` | Trader consolidation (merge sort by ts); trade matching (2 fills per MD_TRADE); book add traceability; book delete traceability; ordering invariant; phantom detection; lost event detection; final book state check; fill price/qty consistency; latency calculation; edge cases (empty journals, single trader, all rejects) |
| `exchange_observer_test.cc` | Journal output format for each MD message type; ANSI display rendering; instrument filtering; graceful shutdown writes final state |

### Failure Injection Tests

| Scenario | Binary | Expected Behavior |
|----------|--------|-------------------|
| Exchange TCP refuses connection | exchange-trader | Exits with error, no journal corruption |
| Exchange drops TCP mid-session | exchange-trader | Detects disconnect, cancels pending, prints partial summary |
| Malformed exec report (truncated SBE) | exchange-trader | Logs decode error, skips message, continues |
| Multicast socket bind fails (port in use) | exchange-observer | Exits with clear error message |
| Malformed MDP3 datagram (truncated) | exchange-observer | Logs decode error, increments error counter, continues |
| Gap in multicast sequence numbers | exchange-observer | Logs gap warning, continues (no replay mechanism) |
| Empty client journal | journal-reconciler | Reports 0 actions, all invariants trivially pass |
| Observer has trades with no matching fills | journal-reconciler | Reported as phantom (Invariant 5 violation) |
| Client fill with no observer MD_TRADE | journal-reconciler | Reported as lost event (Invariant 6 violation) |
| Rate limit exceeded (strategy produces > --rate) | exchange-trader | Actions capped at --rate/sec, excess silently dropped |

### Integration Test

`sim_integration_test.sh`:
1. Start cme-sim on port 9200 with ES instrument
2. Start exchange-observer (multicast group, `--journal /tmp/observer.journal`)
3. Start 5 exchange-trader instances:
   - Traders 1-3: random-walk strategy, rate=5
   - Traders 4-5: market-maker strategy, rate=5
   - Each with `--journal /tmp/client_N.journal`
4. Run for 5 seconds, then SIGINT all traders (graceful cancel-all)
5. SIGINT observer after traders exit (observer captures final book drain)
6. Run journal-reconciler:
   ```
   journal-reconciler \
       --observer-journal /tmp/observer.journal \
       --client-journals /tmp/client_1.journal,...,/tmp/client_5.journal
   ```
7. Assert:
   - Trade match rate = 100% (no phantoms, no lost trades)
   - Book traceability = 100% (all adds/deletes traced)
   - Final book state = empty (clean shutdown)
   - Fill price/qty consistency = 100%
   - No ordering violations

---

## 9. Existing Code Reuse Map

| New Code | Reuses From |
|----------|------------|
| `tcp_client.h` | `ilink3_send_order.cc:190-300` (TcpClient class verbatim) |
| SimClient CME encoding | `ilink3_encoder.h` (encode_new_order, encode_cancel_order, encode_modify_order) |
| SimClient CME decoding | `ilink3_decoder.h` (decode_ilink3_message visitor) |
| SimClient ICE encoding | `fix_encoder.h` (encode_exec_new, etc. -- but client sends orders, so we need FIX NewOrderSingle encoder) |
| SimClient ICE decoding | `fix_parser.h` (parse_fix_message) |
| Observer CME decode | `mdp3_decoder.h` (decode_mdp3_message) |
| Observer ICE decode | `impact_decoder.h` (decode_messages) |
| Observer multicast | `udp_multicast.h` (UdpMulticastReceiver) |
| Instrument resolution | `cme_products.h`, `ice_products.h` |
| Journal format | `journal_parser.h` (ACTION/EXPECT line format) |
| Price conversion | `types.h` (PRICE_SCALE), `ilink3_encoder.h` (decimal_to_engine_price pattern) |

### ICE FIX Client-Side Gap

The existing `fix_encoder.h` encodes **execution reports** (exchange -> client). We need the reverse: **client -> exchange** FIX messages (NewOrderSingle 35=D, CancelRequest 35=F, ReplaceRequest 35=G). These are simpler than exec reports -- just tag=value construction using the same `detail::append_tag` + `detail::assemble_message` helpers already in `fix_encoder.h`. This will be part of T3 (SimClient), estimated ~50 additional lines.

---

## 10. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| ICE FIX client encoding not yet built | Certain | Medium | Simple to add (~50 lines) using existing helpers; T3 scope |
| Non-blocking TCP recv complexity | Low | Low | Existing TcpClient is clean; just add fcntl O_NONBLOCK |
| Strategy determinism for testing | Low | Medium | Seed via `std::mt19937(fixed_seed)` in tests |
| Integration test flakiness | Medium | Medium | Use generous timeouts (10s); check exit codes not timing; observer needs brief delay after traders exit to drain final multicast messages |
| Line count exceeding 200/commit | Medium | Medium | T7 (exchange_trader.cc, ~250 lines) and T4 (exchange_observer.cc, ~250 lines) are the largest; may need to split CLI parsing into separate commits |
| Reconciler invariant complexity (8 checks) | Medium | Medium | T5 is ~200 lines of logic + ~180 lines of tests; each invariant is independently testable with synthetic journals; unit tests use hand-crafted minimal journal snippets, not real exchange output |
| Multicast sequence gap handling | Low | Low | Observer logs gaps but does not attempt replay; reconciler will flag any resulting lost events |
| 5 concurrent TCP connections to exchange | Low | Low | cme-sim already supports concurrent clients via epoll (TcpServer); tested in existing E2E harness |
