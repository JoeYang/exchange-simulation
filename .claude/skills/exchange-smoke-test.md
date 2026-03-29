---
name: exchange-smoke-test
description: Systematic smoke test for any exchange (CME, ICE, KRX). Starts sim, traders, observer, verifies end-to-end data flow.
---

# Exchange Smoke Test

Systematic end-to-end verification for exchange simulators. Tests the full pipeline: order entry -> matching -> market data -> observer display.

## When to Use

Run this skill after:
- Building a new exchange implementation
- Fixing a bug in the data pipeline (encoder, decoder, publisher, observer)
- Adding a new protocol feature (secdef, recovery, transitions)
- Before declaring an exchange implementation "done"

## Process

### Step 1: Build all binaries

```bash
bazel build //cme:cme-sim //ice:ice-sim //krx:krx-sim \
    //tools:exchange-trader //tools:exchange-observer \
    //tools:ilink3-send-order
```

### Step 2: Pick the exchange and run the test

For each exchange, the test follows the same pattern:

```
1. Start the exchange simulator
2. Start the observer with --transitions
3. Send a known order sequence (2 crossing orders = guaranteed fill)
4. Verify observer shows: book add, trade, book remove
5. Stop everything
```

### Exchange Configs

#### CME
```bash
# Sim
bazel-bin/cme/cme-sim --ilink3-port 9200 --shm-path /cme-test

# Observer
bazel-bin/tools/exchange-observer --exchange cme \
    --group 239.0.31.1 --port 14310 --instrument ES --transitions

# Orders (use ilink3-send-order for CME)
bazel-bin/tools/ilink3-send-order --port 9200 --instrument ES \
    --side BUY --price 5000.00 --qty 10 --type LIMIT --tif DAY --account 1
bazel-bin/tools/ilink3-send-order --port 9200 --instrument ES \
    --side SELL --price 5000.00 --qty 10 --type LIMIT --tif DAY --account 2
```

#### ICE
```bash
# Sim
bazel-bin/ice/ice-sim --fix-port 9300 --shm-path /ice-test

# Observer
bazel-bin/tools/exchange-observer --exchange ice \
    --group 239.0.32.1 --port 15000 --instrument B --transitions

# Orders (use exchange-trader with --exchange ice)
bazel-bin/tools/exchange-trader --exchange ice --port 9300 \
    --client-id 1 --account 1 --instrument B \
    --strategy random-walk --ref-price 82.00 --spread 0.50 --rate 5
```

#### KRX
```bash
# Sim
bazel-bin/krx/krx-sim --fix-port 9400 --shm-path /krx-test

# Observer
bazel-bin/tools/exchange-observer --exchange krx \
    --group 239.1.1.1 --port 16000 --instrument KS --transitions

# Orders (use exchange-trader with --exchange krx)
bazel-bin/tools/exchange-trader --exchange krx --port 9400 \
    --client-id 1 --account 1 --instrument KS \
    --strategy random-walk --ref-price 350.00 --spread 1.00 --rate 5
```

### Step 3: Verify expected output

The observer with --transitions should show:
```
[HH:MM:SS.nnnnnnnnn] BID ADD  <price> x <qty>  (levels: 1)
[HH:MM:SS.nnnnnnnnn] ASK ADD  <price> x <qty>  (levels: 1)
[HH:MM:SS.nnnnnnnnn] TRADE    <price> x <qty>   aggressor=SELL
[HH:MM:SS.nnnnnnnnn] BID DEL  <price>           (levels: 0)
[HH:MM:SS.nnnnnnnnn] STATUS   CONTINUOUS
```

If nothing appears:
1. Check sim is running (stderr should show "Simulator ready")
2. Check observer joined correct multicast group/port
3. Check trader connected to correct TCP port
4. Check instrument symbol matches between trader and observer
5. Run with 2 separate manual orders (buy then sell) to isolate whether the issue is order entry or market data

### Step 4: Debug if failing

Common issues:
- **Observer shows nothing**: security_id mismatch (encoder uses ID 0, observer filters by real ID)
- **Observer shows status but no book**: market data events not flowing through publisher
- **Trader hangs**: TCP connection refused or exec report not being sent back
- **Wrong prices**: price scale mismatch (PRICE_SCALE=10000 vs protocol-specific scaling)

Debug steps:
1. Check sim stderr for "iLink3/FIX client connected" when trader starts
2. Check sim stderr for multicast send errors
3. Add `--journal /tmp/observer.journal` to observer, check if any lines written
4. Run `bazel test //EXCHANGE/e2e:...` to verify the E2E pipeline works in isolation

## Automated Script

For CI or quick verification, use the per-exchange test scripts:
```bash
./smoke_test_cme.sh          # CME smoke test
./test-journals/run_all_ice_tests.sh   # ICE tests
./test-journals/run_all_krx_tests.sh   # KRX tests
```

Or run the full simulation exercise:
```bash
bash tools/sim_integration_test.sh
```
