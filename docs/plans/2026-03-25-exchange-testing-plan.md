# Exchange End-to-End Testing Plan

**Date:** 2026-03-25
**Goal:** Verify that order actions (add/replace/cancel) sent via order entry protocol produce correct, observable market data via multicast feed.

---

## 1. Architecture

```
                    Test Journal (.journal)
                    ┌─────────────────────┐
                    │ ACTION NEW_ORDER    │
                    │ EXPECT MD_REFRESH   │  ← verify market data output
                    │ EXPECT EXEC_REPORT  │  ← verify execution report
                    └─────────┬───────────┘
                              │
                    ┌─────────v───────────┐
                    │  E2E Test Runner    │
                    │                     │
                    │  1. Parse journal   │
                    │  2. Encode action   │───→ iLink3 SBE message
                    │     as iLink3 msg   │
                    │                     │
                    │  3. Send to engine  │───→ CmeSimulator
                    │                     │
                    │  4. Capture output: │
                    │     - Exec Reports  │←─── iLink3 ExecutionReport
                    │     - Market Data   │←─── MDP3 incremental refresh
                    │                     │
                    │  5. Decode output   │
                    │  6. Compare vs      │
                    │     EXPECT lines    │
                    └─────────────────────┘
```

### Key principle
The test sends orders as a **client** would (encoded SBE), and verifies the response as a **market data consumer** would (decoded MDP3). This tests the full stack:
- iLink3 decode → OrderRequest
- MatchingEngine processing
- Callbacks → MDP3 encode
- MDP3 decode → verify correctness

---

## 2. Extended Journal Format

### 2.1 New ACTION types (client-side, iLink3)

```
# Order entry actions (encoded as iLink3 SBE)
ACTION ILINK3_NEW_ORDER ts=1000 instrument=ES cl_ord_id=1 side=BUY price=50000000 qty=10000 type=LIMIT tif=DAY account=FIRM_A

ACTION ILINK3_CANCEL ts=2000 instrument=ES cl_ord_id=1 orig_cl_ord_id=1

ACTION ILINK3_REPLACE ts=3000 instrument=ES cl_ord_id=2 orig_cl_ord_id=1 price=50010000 qty=10000

ACTION ILINK3_MASS_CANCEL ts=4000 instrument=ES account=FIRM_A
```

### 2.2 New EXPECT types (two categories)

**Execution Reports (iLink3 response to client):**
```
# Client receives back execution reports
EXPECT EXEC_NEW ord_id=1 cl_ord_id=1 status=NEW instrument=ES
EXPECT EXEC_FILL ord_id=1 cl_ord_id=1 fill_price=50000000 fill_qty=10000 status=FILLED
EXPECT EXEC_CANCELLED ord_id=1 cl_ord_id=1 status=CANCELLED
EXPECT EXEC_REPLACED ord_id=1 cl_ord_id=2 new_price=50010000 status=REPLACED
EXPECT EXEC_REJECTED cl_ord_id=1 reason=INVALID_PRICE
```

**Market Data (MDP3 multicast, observable by all):**
```
# Market data consumers see book changes
EXPECT MD_BOOK_ADD instrument=ES side=BUY price=50000000 qty=10000 num_orders=1
EXPECT MD_BOOK_UPDATE instrument=ES side=BUY price=50000000 qty=20000 num_orders=2
EXPECT MD_BOOK_DELETE instrument=ES side=BUY price=50000000
EXPECT MD_TRADE instrument=ES price=50000000 qty=10000 aggressor_side=SELL
EXPECT MD_TOP_OF_BOOK instrument=ES bid=50000000 bid_qty=10000 ask=50020000 ask_qty=5000
EXPECT MD_STATUS instrument=ES state=PRE_OPEN
```

### 2.3 Session actions

```
ACTION SESSION_START ts=0 state=PRE_OPEN
ACTION SESSION_OPEN ts=1000
ACTION SESSION_CLOSE ts=86400000
```

---

## 3. Tooling Components

### 3.1 SBE Codec (encode/decode layer)

Both iLink3 and MDP3 need encode/decode. Rather than a full codegen from XML, start with hand-written codecs for the **key message types only**:

**iLink3 (order entry) — 6 messages:**
```
cme/codec/
├── sbe_header.h              # SBE message header (common)
├── ilink3_encoder.h/.cc      # Encode OrderRequest → iLink3 SBE bytes
│   - encode_new_order_single()   → NewOrderSingle514
│   - encode_cancel_request()     → OrderCancelRequest516
│   - encode_replace_request()    → OrderCancelReplaceRequest515
│   - encode_mass_cancel()        → OrderMassActionRequest529
│
├── ilink3_decoder.h/.cc      # Decode iLink3 SBE bytes → structs
│   - decode_execution_report()   → ExecutionReport (various)
│   - decode_cancel_reject()      → OrderCancelReject
│
└── ilink3_messages.h         # Message structs (matching schema)
    - ILink3NewOrder
    - ILink3CancelRequest
    - ILink3ReplaceRequest
    - ILink3ExecutionReport
    - ILink3CancelReject
```

**MDP3 (market data) — 5 messages:**
```
cme/codec/
├── mdp3_encoder.h/.cc        # Encode engine callbacks → MDP3 SBE bytes
│   - encode_book_refresh()       → MDIncrementalRefreshBook46
│   - encode_trade_summary()      → MDIncrementalRefreshTradeSummary48
│   - encode_security_status()    → SecurityStatus30
│
├── mdp3_decoder.h/.cc        # Decode MDP3 SBE bytes → structs
│   - decode_book_refresh()
│   - decode_trade_summary()
│   - decode_security_status()
│
└── mdp3_messages.h           # Message structs
    - MDP3BookEntry
    - MDP3TradeSummary
    - MDP3SecurityStatus
```

### 3.2 Protocol Listeners (engine callback → encoded messages)

```cpp
// cme/codec/ilink3_report_publisher.h
// Listens to engine order events, encodes as iLink3 ExecutionReports
class ILink3ReportPublisher : public OrderListenerBase {
    std::vector<std::vector<char>> encoded_reports_;
public:
    void on_order_accepted(const OrderAccepted& e) override;
    void on_order_filled(const OrderFilled& e) override;
    void on_order_cancelled(const OrderCancelled& e) override;
    // ...
    const std::vector<std::vector<char>>& reports() const;
    void clear();
};

// cme/codec/mdp3_feed_publisher.h
// Listens to engine market data events, encodes as MDP3 messages
class MDP3FeedPublisher : public MarketDataListenerBase {
    std::vector<std::vector<char>> messages_;
public:
    void on_depth_update(const DepthUpdate& e) override;
    void on_trade(const Trade& e) override;
    void on_top_of_book(const TopOfBook& e) override;
    void on_market_status(const MarketStatus& e) override;
    // ...
    const std::vector<std::vector<char>>& messages() const;
    void clear();
};
```

### 3.3 E2E Test Runner

```cpp
// cme/e2e/e2e_test_runner.h
class E2ETestRunner {
    CmeSimulator<CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>,
                  CompositeOrderListener<MDP3FeedPublisher, RecordingMdListener>> simulator_;

    ILink3Encoder encoder_;
    ILink3Decoder ilink3_decoder_;
    MDP3Decoder mdp3_decoder_;

public:
    struct E2EResult {
        bool passed;
        size_t action_index;
        std::string expected;
        std::string actual;
        std::string category;  // "EXEC_REPORT" or "MARKET_DATA"
    };

    // Run a journal through the full encode→engine→decode→verify pipeline
    E2EResult run(const Journal& journal);

private:
    // Encode journal action as iLink3 SBE, send to engine
    void execute_action(const ParsedAction& action);

    // After each action, collect and decode:
    // - ILink3 execution reports
    // - MDP3 market data messages
    // Compare decoded output against EXPECT lines
    E2EResult verify_expectations(const std::vector<ParsedExpectation>& expects);
};
```

---

## 4. Exchange-Specific Test Journals

### 4.1 Generic (all exchanges)

```
test-journals/e2e/
├── e2e_basic_limit_order.journal        # Add limit, verify MD_BOOK_ADD + EXEC_NEW
├── e2e_limit_fill.journal               # Two limits cross, verify MD_TRADE + EXEC_FILL
├── e2e_cancel_order.journal             # Add then cancel, verify MD_BOOK_DELETE + EXEC_CANCELLED
├── e2e_replace_order.journal            # Add then replace, verify MD update + EXEC_REPLACED
├── e2e_partial_fill.journal             # Partial fill, verify MD_BOOK_UPDATE + EXEC_PARTIAL
├── e2e_reject_invalid.journal           # Bad order, verify EXEC_REJECTED (no MD events)
├── e2e_mass_cancel.journal              # Multiple orders, mass cancel, verify all MD_BOOK_DELETE
├── e2e_auction_lifecycle.journal        # PreOpen → collect → uncross → continuous
└── e2e_iceberg.journal                  # Iceberg order, verify only display qty in MD
```

### 4.2 CME-Specific

```
test-journals/cme/
├── cme_e2e_es_trading_day.journal       # Full ES trading day lifecycle
├── cme_e2e_smp_cancel_newest.journal    # CME SMP behavior via protocol
├── cme_e2e_dynamic_bands.journal        # Order outside dynamic band → EXEC_REJECTED
├── cme_e2e_multi_product.journal        # ES + NQ + CL simultaneous
├── cme_e2e_tick_alignment.journal       # ES 0.25 tick enforcement via protocol
├── cme_e2e_ioc_in_auction.journal       # IOC rejected during PreOpen
├── cme_e2e_day_expiry.journal           # DAY orders expire at session close
├── cme_e2e_stop_trigger.journal         # Stop order trigger via protocol
└── cme_e2e_market_sweep.journal         # Market order sweeps multiple levels, verify trades
```

### 4.3 Future Exchange-Specific (structure only)

```
test-journals/eurex/
├── eurex_e2e_volatility_interruption.journal
├── eurex_e2e_gfa_tif.journal
└── ...

test-journals/sgx/
├── sgx_e2e_ato_atc.journal
└── ...
```

---

## 5. Implementation Tasks

### Group A — SBE Codec (no dependencies beyond exchange-core)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| A1 | SBE header + common types | `cme/codec/sbe_header.h` | ~80 | SBE message header struct, endian helpers, field types |
| A2 | iLink3 message structs | `cme/codec/ilink3_messages.h` | ~150 | NewOrder, Cancel, Replace, ExecutionReport, CancelReject structs |
| A3 | MDP3 message structs | `cme/codec/mdp3_messages.h` | ~120 | BookEntry, TradeSummary, SecurityStatus structs |
| A4 | iLink3 encoder | `cme/codec/ilink3_encoder.h/.cc`, test | ~200 | Encode OrderRequest → SBE bytes for each message type |
| A5 | iLink3 decoder | `cme/codec/ilink3_decoder.h/.cc`, test | ~200 | Decode SBE bytes → message structs |
| A6 | MDP3 encoder | `cme/codec/mdp3_encoder.h/.cc`, test | ~200 | Encode engine callbacks → MDP3 SBE bytes |
| A7 | MDP3 decoder | `cme/codec/mdp3_decoder.h/.cc`, test | ~200 | Decode MDP3 SBE bytes → message structs |
| A8 | Round-trip tests | `cme/codec/codec_roundtrip_test.cc` | ~150 | Encode → decode → verify for all message types |

### Group B — Protocol Publishers (depends on A)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| B1 | iLink3 report publisher | `cme/codec/ilink3_report_publisher.h/.cc`, test | ~180 | OrderListener → encoded ExecutionReports |
| B2 | MDP3 feed publisher | `cme/codec/mdp3_feed_publisher.h/.cc`, test | ~180 | MdListener → encoded MDP3 messages |

### Group C — E2E Test Framework (depends on B)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| C1 | E2E journal format extension | Parser/writer/runner updates | ~200 | Add ILINK3_*, EXEC_*, MD_* action/expect types |
| C2 | E2E test runner | `cme/e2e/e2e_test_runner.h/.cc`, test | ~300 | Full pipeline: encode→engine→decode→verify |
| C3 | E2E journal test harness | `cme/e2e/e2e_journal_test.cc` | ~80 | Bazel test that discovers and runs e2e/*.journal files |

### Group D — Test Journals (depends on C)

| # | Task | Files | Est. Lines | Description |
|---|------|-------|-----------|-------------|
| D1 | Generic e2e journals | `test-journals/e2e/*.journal` | ~300 | 9 generic end-to-end scenarios |
| D2 | CME-specific e2e journals | `test-journals/cme/*.journal` | ~400 | 9 CME-specific scenarios |
| D3 | E2E journal generator | `test-journals/generate_e2e_journals.cc` | ~400 | Generate journals with correct EXPECT lines via real engine |

### Dependency Graph

```
Group A: [A1] → [A2] [A3]         (header → message structs)
               |     |
         [A4] [A5] [A6] [A7]      (encoders + decoders, parallel)
               |
         [A8]                       (round-trip tests)

Group B: [B1] [B2]                 (parallel, depend on A4-A7)

Group C: [C1] → [C2] → [C3]       (sequential)

Group D: [D3] → [D1] [D2]         (generator first, then journals)
```

---

## 6. Verification Matrix

What each e2e test verifies across the full stack:

| Scenario | iLink3 Decode | Engine Processing | MDP3 Encode | EXEC Report |
|----------|:---:|:---:|:---:|:---:|
| Add limit order | NewOrder514 → OrderRequest | Rests on book | BookAdd | EXEC_NEW |
| Fill order | NewOrder514 → OrderRequest | Match + fill | BookDelete + Trade | EXEC_FILL |
| Cancel order | CancelReq516 → cancel_order() | Remove from book | BookDelete | EXEC_CANCELLED |
| Replace order | ReplaceReq515 → modify_order() | Cancel-replace | BookDelete + BookAdd | EXEC_REPLACED |
| Reject order | NewOrder514 → validation fail | Reject | (none) | EXEC_REJECTED |
| Mass cancel | MassAction529 → mass_cancel() | Remove all | BookDelete × N | EXEC_CANCELLED × N |
| Auction | SessionAction → execute_auction() | Uncross | Trade × N | EXEC_FILL × N |
| Iceberg | NewOrder514 with display_qty | Rests display only | BookAdd(display_qty) | EXEC_NEW |

---

## 7. Success Criteria

1. **Protocol correctness:** Every iLink3 message encodes/decodes to the exact SBE byte layout per schema
2. **Market data accuracy:** Every order action produces the correct MDP3 message with correct fields
3. **Execution report accuracy:** Every order action produces the correct ExecutionReport
4. **No phantom events:** Actions that fail validation produce EXEC_REJECTED and NO market data
5. **Iceberg visibility:** Market data shows only display_qty, not total hidden quantity
6. **Auction correctness:** Auction fills produce trades at single equilibrium price in market data
7. **Cross-instrument isolation:** Actions on ES don't produce market data for NQ
