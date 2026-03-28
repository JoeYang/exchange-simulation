# Per-Exchange Recovery Strategies for Exchange Observer

**Date:** 2026-03-28
**Status:** Draft
**Scope:** Observer startup recovery, sim-side snapshot channels, encoder additions

## Problem

The exchange observer currently starts with an empty book (`DisplayState`). If the
observer joins after the simulator has been running, it misses all prior depth updates
and displays a stale/incomplete book until enough incremental messages arrive to
reconstruct the current state. This is the classic "late joiner" problem.

## Observer Startup Sequence (Production-Accurate)

The observer must follow this sequence, matching how real exchange clients operate:

```
1. SECDEF       Join secdef channel, discover instruments (symbol → security_id,
                tick size, lot size, trading hours). Wait until complete.
                [Separate plan — see docs/plans/ for secdef implementation]

2. SNAPSHOT     Request/receive full book snapshot for the target instrument.
                This plan covers this step.

3. INCREMENTAL  Switch to incremental feed. Process updates normally.

4. GAP DETECT   Monitor sequence numbers. On gap detection:
                - Buffer incoming incrementals
                - Re-request snapshot (goto step 2)
                - Apply snapshot, then replay buffered incrementals
                - Resume normal processing
```

Note: Step 1 (secdef) is a prerequisite but is scoped as a separate plan.
This plan assumes instrument configuration is already known (from config or
a prior secdef phase) and focuses on steps 2-4.

## Gap Detection & Auto-Recovery

The observer must detect gaps in the incremental feed and automatically recover:

- **CME MDP3**: `rpt_seq` per instrument is monotonically increasing. If observer
  sees rpt_seq N+2 after N (gap of 1), it enters recovery mode.
- **ICE iMpact**: `sequence_number` in BundleEnd is the block sequence. Gaps
  indicate missed bundles.

On gap detection:
1. Set `recovering = true`
2. Buffer all incoming incremental messages (don't apply to book)
3. Initiate snapshot recovery (CmeRecovery or IceRecovery)
4. Apply snapshot → set `recovering = false`
5. Replay buffered incrementals that have rpt_seq > snapshot rpt_seq
6. Resume normal incremental processing

## Design Overview

```
  Observer Lifecycle:

  [1. SECDEF]          [2. SNAPSHOT]              [3. INCREMENTAL]
  (separate plan)      (this plan)                (existing + gap detect)
       |                    |                          |
       v                    v                          v
  +-----------+     +----------------+     +----------------------+
  | Discover  |---->| CmeRecovery    |---->| Process incrementals |
  | instruments|    | (snapshot mcast)|    | Monitor rpt_seq      |
  +-----------+     +----------------+     +-------+--------------+
                    | IceRecovery    |             |
                    | (TCP snapshot) |    gap detected?
                    +----------------+             |
                    | NullRecovery   |             v
                    | (empty book)   |     +----------------+
                    +----------------+     | Auto-recover:  |
                                           | buffer + snap  |
                                           | + replay       |
                                           +-------+--------+
                                                   |
                                                   v
                                           resume incremental
```

### Data Flow — CME Snapshot Recovery

```
  cme_sim_runner                              exchange_observer
  +------------------+                        +------------------+
  | MatchingEngine   |                        |                  |
  | book_.best_bid() |---(every 5s)---------->| CmeRecovery      |
  | book_.best_ask() |  encode_snapshot()     | join snapshot    |
  |                  |  UdpMulticastPublisher  | multicast group  |
  |                  |  (snapshot channel)     | decode Snapshot53|
  +------------------+                        | populate Display |
                                              | State            |
                                              +------------------+
                                                     |
                                                     v
                                              (switch to incremental)
```

### Data Flow — ICE Snapshot Recovery

```
  ice_sim_runner                              exchange_observer
  +------------------+                        +------------------+
  | MatchingEngine   |                        |                  |
  | book_.best_bid() |<---(TCP request)-------| IceRecovery      |
  | book_.best_ask() |  "SNAP ES\n"          | connect to TCP   |
  |                  |-----(TCP response)---->| snapshot server   |
  |                  |  SnapshotOrder msgs    | decode snapshots  |
  |                  |  + end marker          | populate Display  |
  |                  |  TcpServer             | State             |
  +------------------+                        +------------------+
                                                     |
                                                     v
                                              (switch to incremental)
```

## Pros/Cons of Approach

### Strategy Pattern (chosen)

**Pros:**
- Clean separation of recovery logic per exchange protocol
- Observer main loop stays unchanged; recovery is a pre-step
- NullRecovery preserves backward compatibility with zero overhead
- Each strategy is independently testable

**Cons:**
- One more abstraction layer (RecoveryStrategy interface)
- Snapshot encoding duplicates some knowledge of the book structure

### Alternative: Replay from Journal

**Pros:** No sim-side changes needed
**Cons:** Requires journal to be available; slow for large histories; doesn't work if
observer was never running before. Rejected.

### Alternative: Incremental Gap Fill (request retransmit)

**Pros:** More production-realistic
**Cons:** Much more complex; requires message buffering, sequence tracking, and
retransmit infrastructure on both sides. Overkill for the simulator. Rejected.

## Component Design

### 1. RecoveryStrategy Interface

**File:** `tools/recovery_strategy.h`

```cpp
#pragma once
#include <string>

// Forward-declare DisplayState (defined in exchange_observer.cc, will be
// extracted to a header).
struct DisplayState;

class RecoveryStrategy {
public:
    virtual ~RecoveryStrategy() = default;

    // Called once at startup. Blocks until initial book state is recovered.
    // Populates the provided DisplayState with the recovered book.
    virtual void recover(const std::string& instrument, DisplayState& ds) = 0;
};
```

### 2. NullRecovery

**File:** `tools/null_recovery.h`

Trivial implementation — does nothing. Preserves current behavior.

```cpp
class NullRecovery : public RecoveryStrategy {
public:
    void recover(const std::string&, DisplayState&) override {}
};
```

### 3. CmeRecovery

**File:** `tools/cme_recovery.h`

```cpp
class CmeRecovery : public RecoveryStrategy {
    std::string snapshot_group_;
    uint16_t    snapshot_port_;
    int32_t     security_id_;

public:
    CmeRecovery(std::string group, uint16_t port, int32_t security_id);

    // Joins snapshot multicast group, waits for SnapshotFullRefreshOrderBook53
    // matching security_id, decodes all entries into DisplayState, returns.
    void recover(const std::string& instrument, DisplayState& ds) override;
};
```

**Recovery algorithm:**
1. Create `UdpMulticastReceiver`, join `snapshot_group_:snapshot_port_`
2. Set receive timeout (5s)
3. Loop: receive datagram, strip `McastSeqHeader`, decode with `decode_mdp3_message`
4. When `DecodedSnapshot53` arrives with matching `security_id`:
   - Clear bids/asks in `DisplayState`
   - For each `SnapshotOrderBookEntry`: determine side from `md_entry_type`,
     convert PRICE9 mantissa and qty, call `update_book_side()`
5. Leave snapshot group, return

### 4. IceRecovery

**File:** `tools/ice_recovery.h`

```cpp
class IceRecovery : public RecoveryStrategy {
    std::string snapshot_host_;
    uint16_t    snapshot_port_;
    int32_t     instrument_id_;

public:
    IceRecovery(std::string host, uint16_t port, int32_t instrument_id);

    // Connects to TCP snapshot server, sends instrument symbol,
    // receives SnapshotOrder messages, populates DisplayState.
    void recover(const std::string& instrument, DisplayState& ds) override;
};
```

**Recovery algorithm:**
1. Open TCP connection to `snapshot_host_:snapshot_port_`
2. Send request: `"SNAP <instrument>\n"` (length-prefixed per TcpServer protocol)
3. Read responses: length-prefixed frames containing iMpact messages
4. For each `SnapshotOrder`: determine side, convert price/qty, call
   `update_book_side()`
5. End marker: a `BundleEnd` message with `sequence_number == 0` signals done
6. Close connection, return

### 5. DisplayState Extraction

Currently `DisplayState`, `BookLevel`, `TradeEntry`, `update_book_side()`,
`record_trade()` are defined inside `exchange_observer.cc`. They need to be
extracted to a shared header so recovery strategies can use them.

**File:** `tools/display_state.h`

Extract: `BookLevel`, `TradeEntry`, `DisplayState`, `BOOK_DEPTH`, `TRADE_DEPTH`,
`update_book_side()`, `record_trade()`.

### 6. Book State Extraction from MatchingEngine

`OrderBook::best_bid()` and `OrderBook::best_ask()` return `PriceLevel*` pointers,
and `PriceLevel::next` allows iteration. However, `book_` is `protected` in
`MatchingEngine`, so only the derived engine classes (CmeEngine, IceEngine) can
access it.

**Chosen approach:** Add a public `for_each_level()` method to `MatchingEngine` that
iterates price levels and fires a callback. This avoids exposing the raw `OrderBook`
and keeps the API clean.

```cpp
// In matching_engine.h, public section:
template <typename Callback>
void for_each_level(Side side, Callback&& cb) const {
    const PriceLevel* lv = (side == Side::Buy)
        ? book_.best_bid() : book_.best_ask();
    for (; lv; lv = lv->next) {
        cb(lv->price, lv->total_quantity, lv->order_count);
    }
}
```

This is the minimal addition — one method, ~6 lines, no new types.

### 7. Encoder Additions

#### CME: `encode_snapshot()` in `cme/codec/mdp3_encoder.h`

Encodes a `SnapshotFullRefreshOrderBook53` with N `SnapshotOrderBookEntry` entries.

```
Wire layout: MessageHeader(8) + root(28) + GroupHeader(3) + N * entry(29)
```

Parameters:
- `char* buf` — output buffer
- `int32_t security_id`
- `uint64_t transact_time`
- `uint32_t last_msg_seq_num_processed` — rpt_seq from incremental channel
- Array of `(price, qty, side)` tuples from `for_each_level()`

Returns total bytes written. Caller ensures buffer is large enough.

Maximum entries per message: `MAX_SNAPSHOT_ENTRIES` (256). If the book exceeds this,
use multiple chunks (tot_num_reports > 1, current_chunk increments).

#### ICE: `encode_snapshot()` in `ice/impact/impact_encoder.h`

Encodes a `SnapshotOrder` message (already defined in `impact_messages.h`).

The existing `encode<SnapshotOrder>()` template in `impact_messages.h` already works
— it writes `ImpactMessageHeader(3) + SnapshotOrder(29) = 32 bytes`.

What's needed: a helper that encodes a full book as a sequence of `SnapshotOrder`
messages, bundled between `BundleStart` and `BundleEnd`.

```cpp
// Encode all levels for one side into a buffer as SnapshotOrder messages.
// Returns pointer past last byte written, or nullptr on overflow.
inline char* encode_snapshot_side(
    char* buf, size_t buf_len,
    int32_t instrument_id,
    exchange::Side side,
    const PriceLevel* first_level,
    int64_t& next_order_id);
```

### 8. Observer CLI Changes

New flags added to `ObserverConfig` and `parse_args()`:

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--recovery` | `none\|snapshot\|tcp` | `none` | Recovery mode |
| `--snapshot-group` | string | `239.0.31.2` | CME snapshot multicast group |
| `--snapshot-port` | uint16 | `14311` | Snapshot port (CME multicast or ICE TCP) |
| `--snapshot-host` | string | `127.0.0.1` | ICE snapshot TCP host |

Strategy factory logic:
```
if recovery == "none"     -> NullRecovery
if recovery == "snapshot" -> CmeRecovery(snapshot_group, snapshot_port, security_id)
if recovery == "tcp"      -> IceRecovery(snapshot_host, snapshot_port, instrument_id)
```

### 9. CME Sim Runner — Snapshot Channel

Add to `cme_sim_runner.cc`:

1. New fields in `SimConfig`: `snapshot_group` (default `"239.0.31.2"`),
   `snapshot_port` (default `14311`)
2. Second `UdpMulticastPublisher` for the snapshot channel
3. Timer in event loop: every 5 seconds, for each loaded instrument:
   - Call `engine->for_each_level(Side::Buy, ...)` and
     `engine->for_each_level(Side::Sell, ...)` to collect price levels
   - Call `encode_snapshot()` to produce `SnapshotFullRefreshOrderBook53`
   - Publish on snapshot multicast channel

### 10. ICE Sim Runner — TCP Snapshot Server

Add to `ice_sim_runner.cc`:

1. New field in `IceSimConfig`: `snapshot_port` (default `14401`)
2. Second `TcpServer` on the snapshot port
3. `on_message` callback:
   - Parse request: extract instrument symbol from `"SNAP <symbol>\n"`
   - Look up instrument_id in `symbol_map`
   - Get engine via `sim.get_fifo_engine(id)` or `sim.get_gtbpr_engine(id)`
   - Call `engine->for_each_level()` for both sides
   - Encode each level as `SnapshotOrder`, send on the TCP connection
   - Send end marker: `BundleEnd` with `sequence_number == 0`

## Task Breakdown

All tasks target < 200 lines. Dependencies shown with arrows.

```
  T1: display_state.h extraction
       |
       v
  T2: RecoveryStrategy + NullRecovery -----> T5: Observer CLI + integration
       |                                           ^        ^
       v                                           |        |
  T3: for_each_level() in MatchingEngine           |        |
       |         |                                 |        |
       v         v                                 |        |
  T4a: CME       T4b: ICE                         |        |
  encode_        encode_                           |        |
  snapshot       snapshot                          |        |
       |              |                            |        |
       v              v                            |        |
  T6a: CmeRecovery   T6b: IceRecovery             |        |
       |              |                            |        |
       +------+-------+                           |        |
              v                                    |        |
  T7: Observer integration (wire strategies) ------+        |
              |                                             |
              v                                             |
  T8a: CME sim snapshot channel ---------> T9: E2E test ----+
  T8b: ICE sim snapshot server ----------^
```

### T1: Extract DisplayState to shared header (~80 lines)

**Assignee:** Dev 1
**Files:** `tools/display_state.h` (new), `tools/exchange_observer.cc` (edit)

1. Create `tools/display_state.h` with `BookLevel`, `TradeEntry`, `DisplayState`,
   `BOOK_DEPTH`, `TRADE_DEPTH`, `update_book_side()`, `record_trade()`
2. Update `exchange_observer.cc` to `#include "tools/display_state.h"` and remove
   the inlined definitions
3. Update BUILD if needed
4. Run existing observer tests to confirm no regressions

### T2: RecoveryStrategy interface + NullRecovery (~40 lines)

**Assignee:** Dev 1 (after T1)
**Files:** `tools/recovery_strategy.h` (new), `tools/null_recovery.h` (new)

1. Define `RecoveryStrategy` abstract base class
2. Implement `NullRecovery`
3. Unit test: `NullRecovery::recover()` leaves `DisplayState` unchanged

### T3: Add `for_each_level()` to MatchingEngine (~30 lines)

**Assignee:** Dev 2
**Files:** `exchange-core/matching_engine.h` (edit)

1. Add public `for_each_level(Side, Callback)` method
2. Unit test: create a book with known levels, verify callback fires for each
   level in correct order (bids descending, asks ascending)

### T4a: CME `encode_snapshot()` (~120 lines)

**Assignee:** Dev 2 (after T3)
**Files:** `cme/codec/mdp3_encoder.h` (edit), `cme/codec/mdp3_encoder_test.cc` (edit)

1. Add `encode_snapshot()` function that encodes
   `SnapshotFullRefreshOrderBook53` with N `SnapshotOrderBookEntry` entries
2. Support chunking when entries > `MAX_SNAPSHOT_ENTRIES`
3. Unit test: encode a snapshot with 3 bid + 2 ask entries, decode with existing
   decoder, verify round-trip correctness

### T4b: ICE `encode_snapshot()` helper (~80 lines)

**Assignee:** Dev 3
**Files:** `ice/impact/impact_encoder.h` (edit), `ice/impact/impact_encoder_test.cc` (edit)

1. Add `encode_snapshot_orders()` helper that encodes a series of `SnapshotOrder`
   messages between `BundleStart` and `BundleEnd`
2. Unit test: encode 3 bid + 2 ask snapshot orders, decode with existing decoder,
   verify round-trip correctness

### T5: Observer CLI changes (~80 lines)

**Assignee:** Dev 1 (after T2)
**Files:** `tools/exchange_observer.cc` (edit)

1. Add `--recovery`, `--snapshot-group`, `--snapshot-port`, `--snapshot-host` flags
   to `ObserverConfig` and `parse_args()`
2. Add factory function: `create_recovery_strategy(cfg, instrument_id)` that
   returns `std::unique_ptr<RecoveryStrategy>`
3. In `main()`, after resolving instrument ID, create strategy and call
   `strategy->recover(instrument, ds)` before entering the event loop
4. Unit test: verify flag parsing, verify NullRecovery is default

### T6a: CmeRecovery implementation (~120 lines)

**Assignee:** Dev 2 (after T4a)
**Files:** `tools/cme_recovery.h` (new), `tools/cme_recovery.cc` (new),
           `tools/cme_recovery_test.cc` (new)

1. Implement `CmeRecovery::recover()`:
   - Join snapshot multicast group
   - Receive datagrams, strip `McastSeqHeader`, decode
   - On `DecodedSnapshot53` with matching `security_id`: populate `DisplayState`
   - Leave group, return
2. Unit test: publish a snapshot on loopback multicast, verify `CmeRecovery`
   populates `DisplayState` correctly
3. Failure test: timeout when no snapshot arrives within 5s (verify graceful
   fallback — empty book, log warning)
4. Failure test: snapshot with wrong `security_id` is ignored

### T6b: IceRecovery implementation (~120 lines)

**Assignee:** Dev 3 (after T4b)
**Files:** `tools/ice_recovery.h` (new), `tools/ice_recovery.cc` (new),
           `tools/ice_recovery_test.cc` (new)

1. Implement `IceRecovery::recover()`:
   - TCP connect to snapshot server
   - Send `"SNAP <instrument>\n"` as length-prefixed frame
   - Read length-prefixed responses, decode `SnapshotOrder` messages
   - On `BundleEnd` with `sequence_number == 0`: done
   - Populate `DisplayState`, close connection
2. Unit test: spin up a `TcpServer` that serves canned snapshot data, verify
   `IceRecovery` populates `DisplayState` correctly
3. Failure test: connection refused (verify graceful fallback — empty book,
   log warning)
4. Failure test: server sends no end marker within timeout (verify timeout
   handling)

### T7: Wire recovery into observer main (~40 lines)

**Assignee:** Dev 1 (after T5, T6a, T6b)
**Files:** `tools/exchange_observer.cc` (edit)

1. Include `cme_recovery.h` and `ice_recovery.h`
2. Update `create_recovery_strategy()` to instantiate `CmeRecovery` or
   `IceRecovery` based on `--exchange` + `--recovery` flags
3. Verify the factory selects the right strategy for each combination:
   - `--exchange cme --recovery snapshot` -> `CmeRecovery`
   - `--exchange ice --recovery tcp` -> `IceRecovery`
   - `--recovery none` (or omitted) -> `NullRecovery`
   - Invalid combos (e.g. `--exchange cme --recovery tcp`) -> error + exit

### T8a: CME sim snapshot channel (~150 lines)

**Assignee:** Dev 4
**Files:** `cme/sim_config.h` (edit), `cme/cme_sim_runner.cc` (edit)

1. Add `snapshot_group` and `snapshot_port` fields to `SimConfig`
2. Add `--snapshot-group` and `--snapshot-port` CLI flags
3. Create second `UdpMulticastPublisher` for the snapshot channel
4. Add snapshot timer in the event loop (every 5s):
   - For each loaded product/instrument:
     - Call `engine->for_each_level(Side::Buy, ...)` to collect bid levels
     - Call `engine->for_each_level(Side::Sell, ...)` to collect ask levels
     - Call `encode_snapshot()` to produce wire bytes
     - Publish on snapshot multicast channel
5. Unit test: start sim, submit orders, verify snapshot appears on snapshot
   multicast group with correct book state

### T8b: ICE sim TCP snapshot server (~150 lines)

**Assignee:** Dev 4 (after T8a, or parallel with separate dev)
**Files:** `ice/ice_sim_config.h` (edit), `ice/ice_sim_runner.cc` (edit)

1. Add `snapshot_port` field to `IceSimConfig`
2. Add `--snapshot-port` CLI flag
3. Create second `TcpServer` on the snapshot port
4. Implement `on_message` callback:
   - Parse `"SNAP <symbol>\n"` from the length-prefixed frame
   - Look up instrument_id in `symbol_map`
   - Determine if FIFO or GTBPR engine, get the engine pointer
   - Call `engine->for_each_level()` for both sides
   - Encode each level as `SnapshotOrder`, send as length-prefixed frames
   - Send end marker: `BundleEnd` with `sequence_number == 0`
5. Poll the snapshot TcpServer in the event loop alongside the FIX server
6. Unit test: start sim, submit orders, connect to snapshot port, send request,
   verify response contains correct book state

### T9: End-to-end recovery test (~120 lines)

**Assignee:** Dev 1 (after T7, T8a, T8b)
**Files:** `tools/exchange_observer_test.cc` (edit or new test file)

1. **CME E2E test:**
   - Start CME sim with snapshot channel enabled
   - Submit orders to build a known book state
   - Wait for at least one snapshot publication (>5s or trigger manually)
   - Start observer with `--recovery snapshot`
   - Verify observer's `DisplayState` matches the known book
   - Send incremental updates, verify they apply correctly on top of recovered state
2. **ICE E2E test:**
   - Start ICE sim with snapshot server enabled
   - Submit orders to build a known book state
   - Start observer with `--recovery tcp`
   - Verify observer's `DisplayState` matches the known book
   - Send incremental updates, verify they apply correctly on top of recovered state
3. **NullRecovery E2E test:**
   - Start sim, submit orders, start observer with `--recovery none`
   - Verify book starts empty, incrementals build it up

## Assignment Summary

| Dev | Tasks | Estimated Lines |
|-----|-------|-----------------|
| Dev 1 | T1, T2, T5, T7, T9 | ~360 |
| Dev 2 | T3, T4a, T6a | ~270 |
| Dev 3 | T4b, T6b | ~200 |
| Dev 4 | T8a, T8b | ~300 |

### Critical Path

```
T1 -> T2 -> T5 -> T7 -> T9
T3 -> T4a -> T6a -> T7
T4b -> T6b -> T7
T8a, T8b -> T9
```

**Parallelism:** T1/T3/T4b can all start immediately. T8a/T8b can start as soon as
T3 lands. Dev 2 and Dev 3 are unblocked from the start. Dev 4 is blocked only on T3.

## Wire Format Summary

### CME Snapshot Multicast Packet

```
[McastSeqHeader(4)] [MessageHeader(8)] [SnapshotFullRefreshOrderBook53(28)]
  [GroupHeader(3)] [SnapshotOrderBookEntry(29)] * N
```

- Multicast group: `--snapshot-group` (default `239.0.31.2`)
- Port: `--snapshot-port` (default `14311`)
- Published every 5 seconds by the sim

### ICE Snapshot TCP Protocol

**Request** (client -> server):
```
[4-byte LE length] [payload: "SNAP ES\n"]
```

**Response** (server -> client):
```
[4-byte LE length] [BundleStart(3+14)]
[4-byte LE length] [SnapshotOrder(3+29)] * N
[4-byte LE length] [BundleEnd(3+4)]  (sequence_number=0 = end marker)
```

- TCP port: `--snapshot-port` (default `14401`)
- Request/response per connection

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Snapshot stale by the time observer processes it | Acceptable for sim; production would need seq num reconciliation |
| Large book exceeds UDP MTU | Chunk snapshots into multiple datagrams (CME supports `no_chunks`/`current_chunk`) |
| Race between snapshot and incremental | Observer applies snapshot then incrementals; minor staleness acceptable for sim |
| TCP snapshot server blocks event loop | Snapshot encoding is fast (< 1ms for 256 levels); non-blocking I/O via TcpServer |
