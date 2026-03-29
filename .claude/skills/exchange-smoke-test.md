---
name: exchange-smoke-test
description: Systematic smoke test for any exchange (CME, ICE, KRX). Starts sim, traders, observer, verifies end-to-end data flow. MANDATORY before declaring any exchange implementation complete.
---

# Exchange Smoke Test

Systematic end-to-end verification for exchange simulators. Tests the full pipeline: order entry -> matching -> market data -> observer display.

## Mandatory Gate

**Every exchange implementation MUST pass this smoke test before it is considered done.**

When building a new exchange or fixing an existing one, run:
```bash
./smoke_test_all.sh <exchange>   # e.g., ./smoke_test_all.sh krx
```
The exchange is not complete until all checks pass.

## When to Use

- After building a new exchange implementation
- After fixing a bug in the data pipeline (encoder, decoder, publisher, observer)
- After adding a new protocol feature (secdef, recovery, transitions)
- Before declaring an exchange implementation "done"
- As part of any PR review for exchange-related changes

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
1. Start the exchange simulator (use DEFAULT ports — no overrides)
2. Start the observer with --transitions + --journal
3. Send orders: 2 traders with opposing strategies for 3-4 seconds
4. Verify observer shows: STATUS, book adds, trades, book removes
5. Verify observer journal has lines written
6. Stop everything, check logs
```

### Exchange Configs (VERIFIED against sim_config defaults)

#### CME (iLink3 SBE order entry, MDP3 SBE market data)
```bash
# Sim (defaults: ilink3=9100, mdp3=239.0.31.1:14310)
bazel-bin/cme/cme-sim

# Observer
bazel-bin/tools/exchange-observer --exchange cme \
    --group 239.0.31.1 --port 14310 --instrument ES --transitions

# Orders (CME uses ilink3-send-order, NOT exchange-trader)
bazel-bin/tools/ilink3-send-order --port 9100 --instrument ES \
    --side BUY --price 5000.00 --qty 10 --type LIMIT --tif DAY --account 1
bazel-bin/tools/ilink3-send-order --port 9100 --instrument ES \
    --side SELL --price 5000.00 --qty 10 --type LIMIT --tif DAY --account 2

# Ref prices: ES ~5000, NQ ~18000, CL ~80, GC ~2000
# Tick: ES=0.25 (2500), NQ=0.25 (2500), CL=0.01 (100)
```

#### ICE (FIX 4.2 order entry, iMpact binary market data)
```bash
# Sim (defaults: fix=9200, impact=239.0.32.1:14400)
bazel-bin/ice/ice-sim

# Observer
bazel-bin/tools/exchange-observer --exchange ice \
    --group 239.0.32.1 --port 14400 --instrument B --transitions

# Orders (ICE uses exchange-trader with --exchange ice)
bazel-bin/tools/exchange-trader --exchange ice --port 9200 \
    --account FIRM_A --instrument B \
    --strategy random-walk --ref-price 82.00 --spread 0.50 --rate 5

# Second trader (opposing flow for fills)
bazel-bin/tools/exchange-trader --exchange ice --port 9200 \
    --account FIRM_B --instrument B \
    --strategy random-walk --ref-price 82.00 --spread 0.50 --rate 5

# Ref prices: B (Brent) ~82, G (Gasoil) ~700, M (NatGas) ~100
# Tick: B=0.01 (100), G=0.25 (2500), I (Euribor)=0.005 (50)
```

#### KRX (FIX 4.2 order entry, FAST 1.1 market data)
```bash
# Sim (defaults: fix=9300, fast=224.0.33.1:16000)
bazel-bin/krx/krx-sim

# Observer
bazel-bin/tools/exchange-observer --exchange krx \
    --group 224.0.33.1 --port 16000 --instrument KS --transitions

# Orders (KRX uses exchange-trader with --exchange krx)
bazel-bin/tools/exchange-trader --exchange krx --port 9300 \
    --account FIRM_A --instrument KS \
    --strategy random-walk --ref-price 350.00 --spread 1.00 --rate 5

# Second trader
bazel-bin/tools/exchange-trader --exchange krx --port 9300 \
    --account FIRM_B --instrument KS \
    --strategy random-walk --ref-price 350.00 --spread 1.00 --rate 5

# Ref prices: KS (KOSPI200) ~350, KTB ~110, USD ~1300
# Tick: KS=0.05 (500), MKS=0.02 (200), KTB=0.01 (100)
```

### Step 3: Verify expected output

The observer with --transitions should show lines like:
```
[HH:MM:SS.nnnnnnnnn] STATUS   CONTINUOUS
[HH:MM:SS.nnnnnnnnn] BID ADD  350.0500 x 10  (levels: 1)
[HH:MM:SS.nnnnnnnnn] ASK ADD  351.0000 x 10  (levels: 1)
[HH:MM:SS.nnnnnnnnn] TRADE    350.0500 x 10   aggressor=SELL
[HH:MM:SS.nnnnnnnnn] BID DEL  350.0500        (levels: 0)
```

**Pass criteria:**
1. Observer receives STATUS event (sim is publishing market data)
2. Observer receives at least one BID/ASK ADD (orders are resting)
3. Observer receives at least one TRADE (orders are crossing)
4. Observer journal file has > 0 lines

If nothing appears, see Step 4.

### Step 4: Debug if failing

**Symptom → Root Cause → Fix:**

| Symptom | Likely Cause | Debug |
|---------|-------------|-------|
| Observer shows nothing at all | Wrong multicast group/port | Check sim defaults vs observer args |
| Observer shows STATUS but no book | security_id mismatch (encoder=0, observer filters by real ID) | Check publisher initializes security_id from product config |
| Observer shows STATUS but no book | Market data callbacks not wired to publisher | Check sim runner's CompositeListener wiring |
| Trader hangs on connect | Wrong TCP port | Check sim default port vs trader --port |
| Trader connects but no fills | Orders not crossing (price too far apart) | Use same --ref-price on both traders |
| Trader connects but observer empty | Publisher not flushing to multicast | Check sim event loop flush logic |
| Prices look wrong | PRICE_SCALE mismatch | Engine uses 10000, protocols use different scales (CME PRICE9=1e9, ICE=10000, KRX=10000) |

**Debug steps:**
1. Check sim stderr: should show "Simulator ready" + "client connected" when trader starts
2. Add `--journal /tmp/observer.journal` to observer — check if file has content
3. Run E2E unit tests: `bazel test //EXCHANGE/e2e:...` to verify pipeline works in isolation
4. Check multicast loopback: `ip maddr show` to verify multicast group is joined

## Automated Script

```bash
# Test one exchange
./smoke_test_all.sh cme
./smoke_test_all.sh ice
./smoke_test_all.sh krx

# Test all three
./smoke_test_all.sh all
```

The script checks: sim startup, observer receives data, trades are visible.

## Port Reference (from sim_config defaults)

| Exchange | Order Entry | Protocol | MD Group | MD Port | Secdef Group | Secdef Port | Snapshot |
|----------|------------|----------|----------|---------|-------------|-------------|----------|
| CME | 9100 | iLink3 SBE | 239.0.31.1 | 14310 | 239.0.31.3 | 14312 | 239.0.31.2:14311 |
| ICE | 9200 | FIX 4.2 | 239.0.32.1 | 14400 | (inline) | — | TCP 14401 |
| KRX | 9300 | FIX 4.2 | 224.0.33.1 | 16000 | 224.0.33.2 | 16001 | 224.0.33.3:16003 |
