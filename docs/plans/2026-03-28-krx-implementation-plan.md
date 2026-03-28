# KRX Exchange Simulator — Implementation Plan

**Date:** 2026-03-28
**Status:** Plan
**Prerequisite:** ICE Phase 5 complete (FIX gateway reuse, CRTP pattern established)
**Phases:** 6A–6G (Tasks 51–72, 22 tasks total)

---

## 1. Overview

The KRX (Korea Exchange) simulator adds support for KOSPI derivatives matching,
including sidecar (program trading halt), tiered daily price limits, VI
(Volatility Interruption), and dual trading sessions (regular + after-hours).

**Key constraint:** ZERO changes to exchange-core, CME, or ICE code.

---

## 2. Architecture

```
         Client (test runner)                  Market Data Consumer
               |                                        ^
               | FIX 4.4 over TCP                      | FAST 1.1 UDP multicast
               v                                        |
       +------------------+                  +-------------------+
       | KRX FIX Gateway  |  decode          | FAST Publisher    | encode
       | (FIX 4.4 decoder)|----+             | (multicast feed)  |-------> FAST bytes
       +------------------+    |             +-------------------+
                               |                      ^
                               v                      |
                    +---------------------+    engine callbacks
                    |   KrxSimulator      |    (OrderListener +
                    |                     |     MdListener)
                    |  KOSPI200 ──┐       |
                    |  KTB        │engine |
                    |  USD/KRW    │       |
                    |  MiniKOSPI  │       |
                    └─────────────┴───────+
                               |
                               | ExecutionReport (FIX 4.4)
                               v
                    +------------------+
                    | FIX Exec Report  |
                    | Publisher        |
                    +------------------+
                               |
                               v FIX 4.4 text
                    Client receives ack/fill/cancel
```

---

## 3. CRTP Design

```
MatchingEngine<KrxExchange<...>, ...>    [exchange-core — generic]
        ^
        |  inherits via CRTP
        |
KrxExchange<OL, ML, MatchAlgoT, ...>    [krx/ — KRX-specific policy]
        ^
        |  product configuration
        |
KrxProducts (configs for KOSPI200, KTB, USD/KRW, etc.)
```

### 3.1 CRTP Hook Overrides

| Hook | KRX Behavior | Core Change? |
|---|---|---|
| `on_validate_order` | Sidecar halt: reject orders during program trading halt | No — existing hook |
| `on_daily_limit_hit` | Tiered limits: widen band on breach (±8% → ±15% → ±20%) | No — existing hook |
| `is_order_allowed_in_phase` | VI: block non-limit orders during volatility interruption | No — existing hook |
| `on_session_transition` | Dual session: regular → after-hours transition logic | No — existing hook |
| `calculate_dynamic_bands` | VI bands: ±N% around last trade (dynamic or static) | No — existing hook |
| `on_circuit_breaker` | VI trigger: enter call auction for 2 min then resume | No — existing hook |
| `is_self_match` | Account-based SMP (same as CME) | No |
| `get_smp_action` | Cancel newest (same as CME) | No |

### 3.2 Key Design Decisions

1. **Sidecar** — handled via `on_validate_order` CRTP hook. When sidecar is
   active, the hook returns false for program-trading-flagged orders. No new
   `SessionState` enum value needed.

2. **Tiered daily limits** — handled via `on_daily_limit_hit` override. The
   KRX exchange tracks the current tier (1/2/3) and widens bands on breach.
   No changes to core price-band logic.

3. **Dual session** — managed at `KrxSimulator` level. The simulator holds
   two engine instances per product (regular + after-hours) or re-configures
   a single engine on session transition. Managed externally to core.

4. **FIX gateway** — reuses `ice/fix/` parser/encoder infrastructure. KRX
   uses FIX 4.4 (minor version bump from ICE's 4.2), so we extend the
   existing parser with 4.4 tags.

5. **FAST codec** — new component. Uses hardcoded templates (not XML-driven),
   encoding only the message types needed for KRX market data simulation.

6. **ZERO core changes** — all KRX behavior is expressed through CRTP hooks
   and the simulator wrapper layer.

---

## 4. Task Breakdown

### Phase 6A — KRX Core (Tasks 51–53)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 51 | KrxExchange CRTP class | `krx/krx_exchange.h` + test | ~250 | — | CRTP overrides: sidecar via on_validate_order, tiered daily limits via on_daily_limit_hit, VI bands, session validation. Unit tests for all hooks |
| 52 | KRX product configs | `krx/krx_products.h` | ~150 | 51 | EngineConfig for KOSPI200, Mini-KOSPI200, KTB 3Y/10Y, USD/KRW, KOSDAQ150. Tick sizes, lot sizes, VI thresholds, daily limit tiers |
| 53 | KrxSimulator wrapper | `krx/krx_simulator.h` + test | ~200 | 51, 52 | Multi-instrument wrapper; dual session management (regular + after-hours); routes orders by instrument; session scheduling |

### Phase 6B — FIX Gateway (Tasks 54–56)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 54 | KRX FIX messages | `krx/fix/krx_fix_messages.h` | ~100 | — | KRX-specific FIX 4.4 message structs; extends ice/fix/ types with KRX tags (program trading flag, investor type) |
| 55 | KRX FIX gateway | `krx/fix/krx_fix_gateway.h` + test | ~200 | 54 | Decodes FIX 4.4 → OrderRequest; reuses ice/fix/fix_parser for core parsing; adds KRX-specific field extraction |
| 56 | KRX FIX exec publisher | `krx/fix/krx_fix_exec_publisher.h` + test | ~150 | 54 | OrderListenerBase that encodes engine callbacks as FIX 4.4 ExecutionReports with KRX-specific fields |

### Phase 6C — FAST Codec (Tasks 57–61)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 57 | FAST types and primitives | `krx/fast/fast_types.h` | ~120 | — | FAST 1.1 wire types: unsigned/signed integers, ASCII strings, decimal, byte vector. Presence map bits. No XML — hardcoded templates |
| 58 | FAST encoder | `krx/fast/fast_encoder.h/.cc` + test | ~250 | 57 | Encodes engine events → FAST 1.1 binary messages. Stop-bit encoding for integers, presence map generation, template ID dispatch |
| 59 | FAST decoder | `krx/fast/fast_decoder.h/.cc` + test | ~250 | 57 | FAST bytes → decoded structs. Stop-bit decoding, presence map parsing, nullable field handling. For test verification |
| 60 | FAST publisher | `krx/fast/fast_publisher.h` + test | ~150 | 58 | MdListenerBase: encodes market data callbacks into FAST messages, publishes via UDP multicast |
| 61 | FAST codec round-trip | `krx/fast/fast_codec_test.cc` | ~150 | 58, 59 | Encode → decode → verify field equality for every KRX message type |

### Phase 6D — E2E Framework (Tasks 62–64)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 62 | KRX E2E journal format | `test-harness/` updates | ~100 | — | Add KRX_FIX_* action types, KRX_EXEC_* and KRX_MD_* expect types |
| 63 | KRX E2E test runner | `krx/e2e/krx_e2e_test_runner.h/.cc` + test | ~300 | 53, 55, 56, 60, 62 | Full pipeline: parse journal → FIX 4.4 → gateway → KrxSimulator → publishers → decode → verify |
| 64 | KRX E2E test harness | `krx/e2e/krx_e2e_journal_test.cc` | ~80 | 63 | Discovers and runs all test-journals/krx/*.journal via Bazel |

### Phase 6E — Test Journals (Tasks 65–68)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 65 | Basic KOSPI200 journal | `test-journals/krx/krx_e2e_kospi200_basic.journal` | ~120 | 63 | Limit order lifecycle: add → partial fill → cancel. Verifies FIX acks + FAST market data |
| 66 | Sidecar journal | `test-journals/krx/krx_e2e_sidecar.journal` | ~120 | 63 | Program trading halt: orders accepted → sidecar triggered → program orders rejected → sidecar lifted → orders accepted again |
| 67 | Tiered daily limits journal | `test-journals/krx/krx_e2e_tiered_limits.journal` | ~150 | 63 | Hit ±8% → band widens to ±15% → hit again → widens to ±20%. Verifies on_daily_limit_hit callback chain |
| 68 | VI + dual session journal | `test-journals/krx/krx_e2e_vi_dual_session.journal` | ~150 | 63 | VI trigger → 2min call auction → resume. Regular session close → after-hours open. Verifies session transitions |

### Phase 6F — Live Simulator (Tasks 69–70)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 69 | KRX sim config | `krx/krx_sim_config.h` | ~80 | 53 | Runtime config: FIX port, FAST multicast group, enabled products, session schedule |
| 70 | KRX live sim binary | `krx/krx_sim_runner.cc` | ~250 | 53, 55, 60, 69 | `krx-sim --fix-port 9300 --fast-group 224.0.33.1:16000` |

### Phase 6G — Verification (Tasks 71–72)

| # | Task | Files | Est. Lines | Depends On | Description |
|---|------|-------|-----------|-----------|-------------|
| 71 | KRX integration tests | `krx/krx_integration_test.cc` + BUILD | ~300 | 52 | Engine-level tests through real KrxExchange: sidecar reject, tiered limit widening, VI trigger + resume, dual session isolation. 10 scenarios |
| 72 | KRX test runner script | `test-journals/run_all_krx_tests.sh` | ~120 | 64 | Runs ALL KRX tests (unit + E2E journals) with color-coded summary. Discovers bazel targets under //krx/... and journals under test-journals/krx/ |

---

## 5. Dependency DAG

```
Phase 6A (Core):
  [51] KrxExchange CRTP          (independent)
   ├→ [52] Product Configs
   └→ [53] KrxSimulator           (depends: 51, 52)

Phase 6B (FIX Gateway):
  [54] KRX FIX Messages          (independent)
   ├→ [55] KRX FIX Gateway        (depends: 54)
   └→ [56] KRX FIX Exec Publisher (depends: 54)

Phase 6C (FAST Codec):                          ◄── CRITICAL PATH
  [57] FAST Types                (independent)
   ├→ [58] FAST Encoder           (depends: 57)  ◄──
   ├→ [59] FAST Decoder           (depends: 57)
   ├→ [60] FAST Publisher          (depends: 58)  ◄──
   └→ [61] FAST Round-trip         (depends: 58, 59)

Phase 6D (E2E Framework):
  [62] Journal Format            (independent)
  [63] E2E Test Runner           (depends: 53, 55, 56, 60, 62)  ◄──
  [64] E2E Test Harness          (depends: 63)

Phase 6E (Journals):                             ◄── CRITICAL PATH
  [65] Basic KOSPI200            (depends: 63)   ◄──
  [66] Sidecar                   (depends: 63)   ◄──
  [67] Tiered Limits             (depends: 63)   ◄──
  [68] VI + Dual Session         (depends: 63)   ◄──

Phase 6F (Live Sim):
  [69] Config                    (depends: 53)
  [70] Binary                    (depends: 53, 55, 60, 69)

Phase 6G (Verification):
  [71] Integration Tests         (depends: 52)
  [72] Test Runner Script        (depends: 64)

Critical path: 57 → 58 → 60 → 63 → 65-68
```

---

## 6. Dev Dispatch (4 Devs)

| Wave | Dev 1 | Dev 2 | Dev 3 | Dev 4 |
|------|-------|-------|-------|-------|
| **Wave 1** | 51 KrxExchange CRTP | 54 KRX FIX messages | 57 FAST types | 62 Journal format |
| **Wave 2** | 52 Product configs | 55 FIX gateway | 58 FAST encoder | 59 FAST decoder |
| **Wave 3** | 53 KrxSimulator | 56 FIX exec pub | 60 FAST publisher | 61 FAST round-trip |
| **Wave 4** | 71 Integration tests | 63 E2E test runner | 69 Sim config | — |
| **Wave 5** | 65 Basic journal | 66 Sidecar journal | 67 Tiered limits journal | 68 VI+dual session journal |
| **Wave 6** | 64 E2E harness | 72 Test runner script | 70 Live sim binary | — |

**Critical path:** 57 → 58 → 60 → 63 → 65–68 (5 sequential steps)
**Bottleneck:** Dev 3 owns the FAST chain (Waves 1–3). Dev 2 picks up E2E runner in Wave 4.

---

## 7. File Structure

```
krx/
├── BUILD.bazel
├── krx_exchange.h              # KrxExchange CRTP class (Task 51)
├── krx_exchange_test.cc
├── krx_products.h              # Product configs (Task 52)
├── krx_simulator.h             # Multi-instrument + dual session (Task 53)
├── krx_simulator_test.cc
├── krx_integration_test.cc     # Engine-level integration tests (Task 71)
├── krx_sim_config.h            # Runtime config (Task 69)
├── krx_sim_runner.cc           # Live simulator binary (Task 70)
├── fix/
│   ├── BUILD.bazel
│   ├── krx_fix_messages.h      # KRX FIX 4.4 message structs (Task 54)
│   ├── krx_fix_gateway.h       # FIX → OrderRequest (Task 55)
│   ├── krx_fix_gateway_test.cc
│   ├── krx_fix_exec_publisher.h  # Events → FIX (Task 56)
│   └── krx_fix_exec_publisher_test.cc
├── fast/
│   ├── BUILD.bazel
│   ├── fast_types.h            # FAST 1.1 primitives (Task 57)
│   ├── fast_encoder.h          # Events → FAST binary (Task 58)
│   ├── fast_encoder.cc
│   ├── fast_encoder_test.cc
│   ├── fast_decoder.h          # FAST → structs (Task 59)
│   ├── fast_decoder.cc
│   ├── fast_decoder_test.cc
│   ├── fast_publisher.h        # Multicast publisher (Task 60)
│   ├── fast_publisher_test.cc
│   └── fast_codec_test.cc      # Round-trip tests (Task 61)
└── e2e/
    ├── BUILD.bazel
    ├── krx_e2e_test_runner.h   # KRX E2E runner (Task 63)
    ├── krx_e2e_test_runner.cc
    ├── krx_e2e_test_runner_test.cc
    └── krx_e2e_journal_test.cc # Bazel harness (Task 64)

test-harness/
└── (updates for KRX_FIX_*, KRX_EXEC_*, KRX_MD_* types)  # Task 62

test-journals/
├── run_all_krx_tests.sh        # KRX test runner (Task 72)
└── krx/
    ├── krx_e2e_kospi200_basic.journal       # Task 65
    ├── krx_e2e_sidecar.journal              # Task 66
    ├── krx_e2e_tiered_limits.journal        # Task 67
    └── krx_e2e_vi_dual_session.journal      # Task 68
```

---

## 8. Success Criteria

| Criterion | Verified By |
|---|---|
| Sidecar: program orders rejected during halt, accepted after lift | Task 66 journal |
| Tiered limits: band widens ±8% → ±15% → ±20% on successive breaches | Task 67 journal |
| VI: call auction triggered, 2 min hold, then resume continuous | Task 68 journal |
| Dual session: regular orders isolated from after-hours orders | Task 68 journal |
| FAST round-trip: encode → decode = original for all message types | Task 61 tests |
| FIX 4.4: NewOrderSingle → ExecReport correct with KRX tags | Task 65 journal |
| KOSPI200 basic: limit order add → fill → cancel lifecycle | Task 65 journal |
| Multi-product: KOSPI200 and KTB fully isolated | Task 71 integration tests |
| KrxExchange: all CRTP hooks exercised without core changes | Task 51 unit tests |
| Daily limit tiers: exact boundary hit at ±8% triggers widening | Task 71 integration tests |
| VI bands: order outside dynamic band triggers interruption | Task 71 integration tests |
| FAST publisher: every order add produces KRX_MD_ADD | Task 65–68 journals |
| FAST publisher: every fill produces KRX_MD_TRADE + KRX_EXEC_FILL | Task 65–68 journals |
| Test runner: single script runs all KRX tests with summary | Task 72 script |
| ZERO exchange-core / CME / ICE files modified | Code review |
