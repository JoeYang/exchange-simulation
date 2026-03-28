# Security Definition (Secdef) Publishing and Consumption

**Date:** 2026-03-28
**Status:** Draft
**Scope:** Sim-side secdef publishing, observer-side secdef consumption, exchange-agnostic InstrumentInfo

## Problem

Instrument metadata (symbol, security_id, tick_size, lot_size, etc.) is currently hardcoded
in both the sim runners and the observer. The observer requires `--instrument ES` on the
command line and maps it to a hardcoded security_id from `cme_products.h` or `ice_products.h`.

In production, clients discover instruments dynamically via security definition messages:
- **CME MDP3:** `MDInstrumentDefinitionFuture54` on a dedicated multicast channel (channel 300)
- **ICE iMpact:** Inline instrument definition messages on the same multicast channel

Until secdef is implemented, the observer cannot:
1. Auto-discover available instruments without hardcoded product tables
2. Validate that its tick_size/lot_size assumptions match the sim's configuration
3. Transition to the recovery plan's full startup sequence (secdef -> snapshot -> incremental)

## Observer Startup Sequence (Context)

From the recovery plan (`2026-03-28-observer-recovery-plan.md`):

```
1. SECDEF       <-- THIS PLAN
2. SNAPSHOT     <-- Recovery plan
3. INCREMENTAL  <-- Existing
4. GAP DETECT   <-- Recovery plan
```

Secdef is step 1 -- the prerequisite for everything else.

## Design Overview

```
  Sim-Side Publishing:

  cme_sim_runner                          Multicast Channel (secdef)
  +-------------------+                   +----------------------+
  | load_products()   |                   |                      |
  | for each product: |----(every 30s)--->| MDInstrumentDef54    |
  |   encode_instr_   |                   | per instrument       |
  |   def(product)    |                   +----------------------+
  +-------------------+

  ice_sim_runner                          Multicast Channel (same as md)
  +-------------------+                   +----------------------+
  | load_products()   |                   |                      |
  | for each product: |----(every 30s)--->| InstrumentDef ('I')  |
  |   encode_instr_   |                   | per instrument       |
  |   def(product)    |                   +----------------------+
  +-------------------+


  Observer-Side Consumption:

  +------------------+     +-------------------+     +------------------+
  | CmeSecdef        |---->|                   |     |                  |
  | (join secdef     |     | SecdefConsumer     |---->| InstrumentInfo   |
  |  multicast,      |     | interface          |     | map: symbol ->   |
  |  decode msg 54)  |     |                   |     |  {security_id,   |
  +------------------+     |                   |     |   tick_size, ...} |
  | IceSecdef        |---->|                   |     +------------------+
  | (filter msg type |     +-------------------+            |
  |  on same channel)|                                      v
  +------------------+                               Observer main loop
                                                     (use InstrumentInfo
                                                      for filtering and
                                                      price display)
```

### Data Flow -- CME Secdef

```
  cme_sim_runner (startup + every 30s)
  +--------------------------------------+
  | for each CmeProductConfig:           |
  |   encode_instrument_definition()     |
  |   -> MDInstrumentDefinitionFuture54  |
  |   UdpMulticastPublisher              |
  |   (secdef channel: --secdef-group,   |
  |    --secdef-port)                    |
  +--------------------------------------+
              |
              | UDP multicast
              v
  exchange_observer (startup, before snapshot)
  +--------------------------------------+
  | CmeSecdefConsumer                    |
  |   join secdef multicast group        |
  |   decode_mdp3_message() visitor      |
  |   on DecodedInstrumentDef54:         |
  |     extract symbol, security_id,     |
  |     tick_size, lot_size, etc.        |
  |     -> InstrumentInfo                |
  +--------------------------------------+
```

### Data Flow -- ICE Secdef

```
  ice_sim_runner (startup + every 30s)
  +--------------------------------------+
  | for each IceProductConfig:           |
  |   encode_instrument_definition()     |
  |   -> InstrumentDefinition ('I')      |
  |   UdpMulticastPublisher              |
  |   (same iMpact channel)             |
  +--------------------------------------+
              |
              | UDP multicast (inline with md)
              v
  exchange_observer (startup, before snapshot)
  +--------------------------------------+
  | IceSecdefConsumer                    |
  |   listen on same iMpact channel      |
  |   decode_messages() visitor           |
  |   on_instrument_def():               |
  |     extract symbol, instrument_id,   |
  |     tick_size, lot_size, etc.        |
  |     -> InstrumentInfo                |
  +--------------------------------------+
```

## Pros/Cons

### Chosen Approach: Sim Publishes Secdef Messages on Wire

**Pros:**
- Production-accurate: matches how real CME/ICE clients discover instruments
- Observer becomes self-configuring -- no need to link against `cme_products.h`/`ice_products.h`
- Clean prerequisite for the recovery plan's startup sequence
- InstrumentInfo abstraction decouples observer from exchange-specific product structs
- Secdef consumer is independently testable (feed it canned wire bytes)

**Cons:**
- Adds a new multicast channel for CME (ICE reuses existing channel)
- Adds new iMpact message type (InstrumentDefinition) that doesn't exist yet
- 30-second repeat interval means late joiners wait up to 30s on startup

### Alternative: Observer Links Against Product Headers Directly

**Pros:** Zero network overhead, instant startup
**Cons:** Not production-accurate; observer must be recompiled when products change;
no dynamic discovery; doesn't satisfy the recovery plan's secdef prerequisite. Rejected.

### Alternative: TCP Request/Response for Secdef

**Pros:** Instant response, no waiting for periodic broadcast
**Cons:** Adds TCP server complexity to sim; not how real exchanges work (CME uses
multicast channel 300, ICE sends inline). Rejected.

## Component Design

### 1. InstrumentInfo Struct (Exchange-Agnostic)

**File:** `tools/instrument_info.h`

The common struct that both CmeSecdef and IceSecdef produce. The observer works
exclusively with this type -- it never sees CmeProductConfig or IceProductConfig.

```cpp
struct InstrumentInfo {
    uint32_t    security_id;         // CME security_id or ICE instrument_id
    std::string symbol;              // "ES", "B", etc.
    std::string description;         // "E-mini S&P 500", "Brent Crude Futures"
    std::string product_group;       // "Equity Index", "Energy"
    int64_t     tick_size;           // min price increment (engine fixed-point)
    int64_t     lot_size;            // min qty increment (engine fixed-point)
    int64_t     max_order_size;      // max qty per order (engine fixed-point)
    char        match_algorithm;     // 'F' (FIFO), 'P' (ProRata), etc.
    std::string currency;            // "USD", "GBP", etc.
    double      display_factor;      // price -> display conversion
};
```

### 2. SecdefConsumer Interface

**File:** `tools/secdef_consumer.h`

```cpp
#include "tools/instrument_info.h"
#include <unordered_map>
#include <string>

class SecdefConsumer {
public:
    virtual ~SecdefConsumer() = default;

    // Block until all instrument definitions are received (or timeout).
    // Returns the discovered instrument map.
    virtual std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) = 0;
};
```

The 35-second default timeout allows one full secdef cycle (30s) plus margin.

### 3. ICE iMpact InstrumentDefinition Message Type

**File:** `ice/impact/impact_messages.h` (edit)

ICE doesn't currently have an instrument definition message. Add one:

```cpp
enum class MessageType : char {
    // ... existing ...
    InstrumentDefinition = 'I',   // NEW
};

struct InstrumentDefinition {
    static constexpr char TYPE = static_cast<char>(MessageType::InstrumentDefinition);

    int32_t  instrument_id;       // offset 0
    char     symbol[8];           // offset 4, null-padded
    char     description[32];     // offset 12, null-padded
    char     product_group[16];   // offset 44, null-padded
    int64_t  tick_size;           // offset 60, engine fixed-point
    int64_t  lot_size;            // offset 68, engine fixed-point
    int64_t  max_order_size;      // offset 76, engine fixed-point
    uint8_t  match_algo;          // offset 84, 0=FIFO, 1=GTBPR
    char     currency[4];         // offset 85, null-padded ("USD\0", "GBP\0")
};
// sizeof = 89 bytes
```

This uses the existing `encode<T>()` / `decode<T>()` templates from `impact_messages.h`
with no special handling -- it's a flat, fixed-size message like all others.

### 4. CME Secdef Encoder

**File:** `cme/codec/mdp3_encoder.h` (edit)

Add `encode_instrument_definition()` that populates `MDInstrumentDefinitionFuture54`
from a `CmeProductConfig` and encodes it with all 4 repeating groups (NoEvents,
NoMDFeedTypes, NoInstAttrib, NoLotTypeRules).

The struct and decoder already exist (`mdp3_messages.h:311`, `mdp3_decoder.h:240`).
Only the encoder is missing.

```
Wire layout:
  MessageHeader(8) + root(224)
  + GroupHeader(3) + NoEvents entries (9B each)
  + GroupHeader(3) + NoMDFeedTypes entries (4B each)
  + GroupHeader(3) + NoInstAttrib entries (4B each)
  + GroupHeader(3) + NoLotTypeRules entries (5B each)
```

Key field mappings from `CmeProductConfig`:
- `symbol` -> `root.symbol` (20-char, space-padded)
- `instrument_id` -> `root.security_id`
- `tick_size` -> `root.min_price_increment` (converted via `engine_price_to_price9()`)
- `lot_size` -> NoLotTypeRules entry with `lot_type=1`, `min_lot_size = lot_size / PRICE_SCALE`
- `max_order_size` -> `root.max_trade_vol` (converted via `engine_qty_to_wire()`)
- `product_group` -> `root.security_group`
- `description` -> not directly in MDP3 (CME doesn't carry it); omit
- `match_algorithm` -> `root.match_algorithm` ('F' for FIFO)
- `band_pct` -> used to compute `high_limit_price` / `low_limit_price` at runtime

### 5. ICE Secdef Encoder

**File:** `ice/impact/impact_encoder.h` (edit)

Add `encode_instrument_definition()` that populates the new `InstrumentDefinition`
message from an `IceProductConfig`.

```cpp
inline size_t encode_instrument_definition(
    char* buf, size_t buf_len,
    const IceProductConfig& product,
    ImpactEncodeContext& ctx);
```

Key field mappings:
- `instrument_id` -> `msg.instrument_id`
- `symbol` -> `msg.symbol`
- `tick_size` -> `msg.tick_size` (pass-through, both engine fixed-point)
- `lot_size` -> `msg.lot_size`
- `max_order_size` -> `msg.max_order_size`
- `match_algo` -> `msg.match_algo` (0=FIFO, 1=GTBPR from `IceMatchAlgo`)
- `currency` -> hardcoded per product group (Energy="USD", Softs="GBP"/"USD", etc.)

Wraps the InstrumentDefinition in a BundleStart/BundleEnd like all other iMpact messages.

### 6. CmeSecdefConsumer

**File:** `tools/cme_secdef.h`

```cpp
class CmeSecdefConsumer : public SecdefConsumer {
    std::string secdef_group_;
    uint16_t    secdef_port_;

public:
    CmeSecdefConsumer(std::string group, uint16_t port);

    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) override;
};
```

**Algorithm:**
1. Create `UdpMulticastReceiver`, join `secdef_group_:secdef_port_`
2. Set receive timeout to `timeout_secs`
3. Loop: receive datagram, decode with `decode_mdp3_message()`
4. On `DecodedInstrumentDef54`:
   - Extract symbol (trim trailing spaces from 20-char field)
   - Convert `min_price_increment` (PRICE9) back to engine fixed-point
   - Convert `max_trade_vol` back to engine quantity
   - Extract `match_algorithm`, `currency`, `display_factor`
   - Build `InstrumentInfo`, insert into map keyed by symbol
5. After receiving `tot_num_reports` definitions or timeout: leave group, return map

### 7. IceSecdefConsumer

**File:** `tools/ice_secdef.h`

```cpp
class IceSecdefConsumer : public SecdefConsumer {
    std::string impact_group_;
    uint16_t    impact_port_;

public:
    IceSecdefConsumer(std::string group, uint16_t port);

    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) override;
};
```

**Algorithm:**
1. Create `UdpMulticastReceiver`, join `impact_group_:impact_port_`
2. Set receive timeout to `timeout_secs`
3. Loop: receive datagram, decode with `decode_messages()`
4. On `InstrumentDefinition`:
   - Extract symbol (trim trailing nulls from 8-char field)
   - Pass-through tick_size, lot_size, max_order_size (already engine fixed-point)
   - Map `match_algo` byte to char ('F' or 'P')
   - Build `InstrumentInfo`, insert into map
5. After timeout or no new instruments for 5s: leave group, return map

Note: ICE secdef shares the same multicast channel as incremental data. The consumer
filters by `MessageType::InstrumentDefinition` and ignores all other message types.

### 8. CME Sim Config Changes

**File:** `cme/sim_config.h` (edit)

Add fields:
```cpp
std::string secdef_group{"239.0.31.3"};  // secdef multicast group
uint16_t    secdef_port{14312};          // secdef multicast port
```

Add CLI flags: `--secdef-group`, `--secdef-port`.

### 9. ICE Sim Config Changes

**File:** `ice/ice_sim_config.h` (edit)

No new fields needed -- ICE secdef uses the same iMpact multicast channel
(`impact_group_:impact_port_`). No config changes required.

### 10. CME Sim Runner -- Secdef Channel

**File:** `cme/cme_sim_runner.cc` (edit)

At startup, before `open_market()`:
1. Create second `UdpMulticastPublisher` for secdef channel
2. For each loaded product: `encode_instrument_definition()`, publish

In event loop (after existing flush logic):
3. Every 30 seconds: re-publish all instrument definitions

### 11. ICE Sim Runner -- Secdef Publishing

**File:** `ice/ice_sim_runner.cc` (edit)

At startup, before `open_market()`:
1. For each loaded product: `encode_instrument_definition()`, publish on existing
   iMpact multicast channel

In event loop (after existing flush logic):
2. Every 30 seconds: re-publish all instrument definitions on same channel

### 12. Observer Integration

**File:** `tools/exchange_observer.cc` (edit)

New CLI flags:
- `--secdef-group` (string, default `"239.0.31.3"`) -- CME secdef multicast group
- `--secdef-port` (uint16, default `14312`) -- CME secdef port
- `--auto-discover` (bool flag) -- enable secdef-based instrument discovery

When `--auto-discover` is set:
1. Create appropriate `SecdefConsumer` (CmeSecdef or IceSecdef based on `--exchange`)
2. Call `discover()` -- blocks until instruments are found or timeout
3. Print discovered instruments to stderr
4. If `--instrument` is also set: filter to that symbol from the discovered map
5. If `--instrument` is not set: observe all discovered instruments (or first one)
6. Use `InstrumentInfo.security_id` for message filtering (replaces hardcoded lookup)
7. Use `InstrumentInfo.tick_size` for price display formatting

When `--auto-discover` is NOT set: existing behavior (hardcoded product lookup).

### 13. iMpact Decoder Update

**File:** `ice/impact/impact_decoder.h` (edit)

Add `InstrumentDefinition` to the decode dispatch switch:

```cpp
case MessageType::InstrumentDefinition:
    detail::try_decode_and_dispatch<InstrumentDefinition>(
        msg_buf, remaining,
        &VisitorT::on_instrument_def, visitor);
    break;
```

Add callback to visitor concept: `void on_instrument_def(const InstrumentDefinition&);`

## Task Breakdown

All tasks target < 200 lines (excluding tests). Dependencies shown with arrows.

```
  T1: InstrumentInfo + SecdefConsumer interface
       |
       v
  T2: ICE InstrumentDefinition message type + decoder update
       |         |
       v         v
  T3a: CME      T3b: ICE
  encode_       encode_
  instr_def     instr_def
       |              |
       v              v
  T4a: CmeSecdef     T4b: IceSecdef
  Consumer           Consumer
       |              |
       +------+-------+
              v
  T5: Observer CLI + secdef integration
              |
              v
  T6a: CME sim    T6b: ICE sim
  secdef channel  secdef publish
       |              |
       +------+-------+
              v
  T7: E2E secdef test
```

### T1: InstrumentInfo + SecdefConsumer Interface (~50 lines)

**Assignee:** Dev 1
**Files:** `tools/instrument_info.h` (new), `tools/secdef_consumer.h` (new)

1. Define `InstrumentInfo` struct with all fields listed in component 1
2. Define `SecdefConsumer` abstract base class with `discover()` method
3. Unit test: verify `InstrumentInfo` default construction, field assignment

### T2: ICE InstrumentDefinition Message Type (~80 lines)

**Assignee:** Dev 2
**Files:** `ice/impact/impact_messages.h` (edit), `ice/impact/impact_decoder.h` (edit)

1. Add `MessageType::InstrumentDefinition = 'I'` to enum
2. Add `InstrumentDefinition` struct (89 bytes, packed)
3. Add `static_assert` for size
4. Update `decode_messages()` dispatch to handle `InstrumentDefinition`
5. Add `on_instrument_def` to visitor concept
6. Unit test: encode an `InstrumentDefinition`, decode it, verify round-trip
7. Unit test: decoder skips `InstrumentDefinition` gracefully when visitor has no-op handler

### T3a: CME encode_instrument_definition (~150 lines)

**Assignee:** Dev 1 (after T1)
**Files:** `cme/codec/mdp3_encoder.h` (edit)

1. Add `encode_instrument_definition()` function
2. Populate 224-byte root block from `CmeProductConfig` fields:
   - `symbol` (space-padded 20-char)
   - `security_id` = `instrument_id`
   - `min_price_increment` via `engine_price_to_price9(tick_size)`
   - `max_trade_vol` via `engine_qty_to_wire(max_order_size)`
   - `security_group` (space-padded 6-char from `product_group`)
   - `asset` (space-padded 6-char from `symbol`)
   - `match_algorithm` = 'F' (all CME products use FIFO in our sim)
   - `currency` = "USD"
   - `display_factor` = PRICE9 from `1.0 / PRICE_SCALE` = 0.0001
3. Write 4 repeating groups (minimal: 1 event, 1 feed type, 0 attribs, 1 lot type)
4. Update `MAX_MDP3_ENCODED_SIZE` to account for secdef (224 + 8 + 4*3 + ~20 = ~264)
5. Unit test: encode a secdef for ES, decode with existing `decode_instrument_def_54()`,
   verify all extracted fields match input
6. Failure test: verify encoding handles symbol longer than 20 chars (truncation)

### T3b: ICE encode_instrument_definition (~80 lines)

**Assignee:** Dev 2 (after T2)
**Files:** `ice/impact/impact_encoder.h` (edit)

1. Add `encode_instrument_definition()` function
2. Populate `InstrumentDefinition` from `IceProductConfig`:
   - Copy `instrument_id`, `symbol`, `description`, `product_group`
   - Pass-through `tick_size`, `lot_size`, `max_order_size`
   - Map `IceMatchAlgo::FIFO` -> 0, `IceMatchAlgo::GTBPR` -> 1
   - Set `currency` based on product_group (Energy/Equity="USD", Softs="GBP")
3. Wrap in BundleStart/BundleEnd
4. Unit test: encode a secdef for Brent, decode with updated decoder, verify round-trip
5. Failure test: verify encoding handles symbol longer than 8 chars (truncation)

### T4a: CmeSecdefConsumer (~120 lines)

**Assignee:** Dev 1 (after T3a)
**Files:** `tools/cme_secdef.h` (new), `tools/cme_secdef.cc` (new)

1. Implement `CmeSecdefConsumer::discover()`:
   - Join secdef multicast group
   - Receive datagrams, decode `MDInstrumentDefinitionFuture54`
   - Convert PRICE9 fields back to engine fixed-point
   - Build `InstrumentInfo` map
   - Return after `tot_num_reports` received or timeout
2. Unit test: publish canned secdef messages on loopback, verify discovery produces
   correct `InstrumentInfo` entries
3. Failure test: timeout when no messages arrive (returns empty map, logs warning)
4. Failure test: duplicate secdef messages (same instrument published twice) are
   deduplicated

### T4b: IceSecdefConsumer (~100 lines)

**Assignee:** Dev 2 (after T3b)
**Files:** `tools/ice_secdef.h` (new), `tools/ice_secdef.cc` (new)

1. Implement `IceSecdefConsumer::discover()`:
   - Join iMpact multicast group
   - Receive datagrams, decode with `decode_messages()`
   - Filter for `InstrumentDefinition` messages only
   - Build `InstrumentInfo` map from decoded fields
   - Return after no new instruments for 5s or global timeout
2. Unit test: publish canned secdef bundles on loopback, verify discovery
3. Failure test: timeout with no secdef messages (returns empty map)
4. Failure test: interleaved secdef + market data messages (secdef extracted correctly,
   market data ignored during discovery phase)

### T5: Observer CLI + Secdef Integration (~100 lines)

**Assignee:** Dev 3
**Files:** `tools/exchange_observer.cc` (edit)

1. Add `--secdef-group`, `--secdef-port`, `--auto-discover` flags to `ObserverConfig`
   and `parse_args()`
2. Add secdef factory: create `CmeSecdefConsumer` or `IceSecdefConsumer` based on
   `--exchange` flag
3. In `main()`, when `--auto-discover`:
   - Call `consumer->discover()`
   - Log discovered instruments to stderr
   - Resolve `security_id` from discovered map (replaces hardcoded product lookup)
   - If `--instrument` is set: filter discovered map to that symbol
   - If `--instrument` is not set AND exactly one instrument discovered: use it
   - If `--instrument` is not set AND multiple instruments: error + exit (require filter)
4. When `--auto-discover` is not set: preserve existing behavior
5. Unit test: verify flag parsing, verify factory creates correct consumer type

### T6a: CME Sim Secdef Channel (~100 lines)

**Assignee:** Dev 3 (after T5)
**Files:** `cme/sim_config.h` (edit), `cme/cme_sim_runner.cc` (edit)

1. Add `secdef_group` and `secdef_port` to `SimConfig`
2. Add `--secdef-group` and `--secdef-port` CLI flags
3. Create `UdpMulticastPublisher` for secdef channel
4. At startup (before `open_market()`): publish secdef for all loaded products
5. In event loop: every 30 seconds, re-publish all secdefs
6. Unit test: start sim with 2 products, join secdef multicast, verify both
   instrument definitions arrive with correct fields

### T6b: ICE Sim Secdef Publishing (~80 lines)

**Assignee:** Dev 4
**Files:** `ice/ice_sim_runner.cc` (edit)

1. At startup (before `open_market()`): for each product, encode and publish
   `InstrumentDefinition` on existing iMpact multicast channel
2. In event loop: every 30 seconds, re-publish all secdefs on same channel
3. Unit test: start sim with 2 products, join iMpact multicast, verify both
   instrument definitions arrive alongside normal market data

### T7: E2E Secdef Test (~120 lines)

**Assignee:** Dev 3 (after T6a, T6b)
**Files:** `tools/secdef_test.cc` (new)

1. **CME E2E:** Start CME sim, create `CmeSecdefConsumer`, call `discover()`,
   verify all loaded products appear in result map with correct metadata
2. **ICE E2E:** Start ICE sim, create `IceSecdefConsumer`, call `discover()`,
   verify all loaded products appear in result map with correct metadata
3. **Observer E2E:** Start CME sim, run observer with `--auto-discover` (no
   `--instrument`), verify observer discovers instruments and proceeds to
   incremental processing
4. **Failure E2E:** Start observer with `--auto-discover` pointing to wrong
   multicast group, verify timeout and graceful error message

## Dependency DAG

```
                    T1 (interface)
                   / \
                  v   v
   T3a (CME enc)     T2 (ICE msg type)
        |                  |
        v                  v
   T4a (CME consume)  T3b (ICE enc)
        |                  |
        v                  v
        |             T4b (ICE consume)
        |                  |
        +--------+---------+
                 v
            T5 (observer CLI)
                 |
            +----+----+
            v         v
      T6a (CME sim)  T6b (ICE sim)
            |         |
            +----+----+
                 v
            T7 (E2E test)
```

### Parallelism

- **Wave 1 (immediate):** T1, T2 can start in parallel
- **Wave 2:** T3a (needs T1), T3b (needs T2) -- parallel
- **Wave 3:** T4a (needs T3a), T4b (needs T3b) -- parallel
- **Wave 4:** T5 (needs T4a, T4b)
- **Wave 5:** T6a, T6b (need T5) -- parallel
- **Wave 6:** T7 (needs T6a, T6b)

## Dev Dispatch Strategy

| Dev | Tasks | Sequential Dependencies | Estimated Lines |
|-----|-------|-------------------------|-----------------|
| Dev 1 | T1, T3a, T4a | T1 -> T3a -> T4a | ~320 |
| Dev 2 | T2, T3b, T4b | T2 -> T3b -> T4b | ~260 |
| Dev 3 | T5, T6a, T7 | T5 -> T6a -> T7 | ~320 |
| Dev 4 | T6b | T6b (blocked on T5) | ~80 |

Dev 1 and Dev 2 are fully independent and can work in parallel from the start.
Dev 3 is blocked until T4a and T4b land. Dev 4 is blocked until T5 lands.

### Critical Path

```
T1 -> T3a -> T4a -> T5 -> T6a -> T7
```

Total critical path: 6 tasks. T2/T3b/T4b run in parallel with T1/T3a/T4a and
feed into T5 as a merge point.

## Wire Format Summary

### CME Secdef (Multicast)

```
[McastSeqHeader(4)] [MessageHeader(8)] [MDInstrumentDefinitionFuture54(224)]
  [GroupHeader(3)] [InstrDefEventEntry(9)] * num_events
  [GroupHeader(3)] [InstrDefFeedTypeEntry(4)] * num_feed_types
  [GroupHeader(3)] [InstrDefAttribEntry(4)] * num_attribs
  [GroupHeader(3)] [InstrDefLotTypeEntry(5)] * num_lot_types
```

- Multicast group: `--secdef-group` (default `239.0.31.3`)
- Port: `--secdef-port` (default `14312`)
- Published at startup + every 30s

### ICE Secdef (Inline on iMpact Channel)

```
[ImpactMessageHeader(3)] [BundleStart(14)]
[ImpactMessageHeader(3)] [InstrumentDefinition(89)]
[ImpactMessageHeader(3)] [BundleEnd(4)]
```

Total per instrument: 116 bytes.

- Same multicast group as incremental data (`--impact-group`)
- Published at startup + every 30s

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|-----------|
| 30s wait for late joiners on CME | Slow startup | Acceptable for sim; production uses persistent TCP channel for secdef. Could add `--secdef-timeout` to override default |
| ICE secdef interleaved with market data | Observer may process md before secdef | Observer ignores md messages during discovery phase; only processes `InstrumentDefinition` |
| New iMpact message type breaks existing decoders | Observer/trader fail to parse | Unknown message types are already skipped by `decode_messages()` (line 107 in impact_decoder.h) |
| Secdef fields don't round-trip perfectly (PRICE9 conversion) | Tick size mismatch | Unit tests verify exact round-trip for all supported products |
| `tot_num_reports` field in CME secdef may be wrong | Consumer waits for timeout instead of early exit | Fallback to timeout is acceptable; log a warning if count mismatches |
