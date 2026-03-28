#!/usr/bin/env bash
# recovery_integration_test.sh -- E2E test for observer snapshot recovery.
#
# Tests:
#   1. CME: start sim with orders, start observer with --recovery snapshot,
#      verify observer sees the recovered book via lifecycle logs.
#   2. ICE: start sim with orders, start observer with --recovery tcp,
#      verify observer sees the recovered book via lifecycle logs.
#
# Exit 0 = PASS, non-zero = FAIL.

set -euo pipefail

DURATION=8  # total test duration per exchange
INSTRUMENT_CME="ES"
INSTRUMENT_ICE="B"

# Ports -- use random high ports to avoid conflicts.
ILINK3_PORT=$((10000 + RANDOM % 50000))
MDP3_GROUP="239.0.31.1"
MDP3_PORT="14310"
SNAPSHOT_GROUP="239.0.31.2"
SNAPSHOT_PORT="14311"
SECDEF_GROUP="239.0.31.3"
SECDEF_PORT="14312"

ICE_FIX_PORT=$((10000 + RANDOM % 50000))
ICE_MD_GROUP="239.1.31.1"
ICE_MD_PORT="15310"
ICE_SNAPSHOT_PORT=$((10000 + RANDOM % 50000))

# --- Build ---
echo "=== Building binaries ==="
bazel build //cme:cme-sim //ice:ice-sim //tools:exchange-observer //tools:exchange-trader 2>&1

CME_SIM="$(bazel cquery --output=files //cme:cme-sim 2>/dev/null)"
ICE_SIM="$(bazel cquery --output=files //ice:ice-sim 2>/dev/null)"
OBSERVER="$(bazel cquery --output=files //tools:exchange-observer 2>/dev/null)"
TRADER="$(bazel cquery --output=files //tools:exchange-trader 2>/dev/null)"

for bin in "$CME_SIM" "$ICE_SIM" "$OBSERVER" "$TRADER"; do
    if [[ ! -x "$bin" ]]; then
        echo "FAIL: binary not found: $bin"
        exit 2
    fi
done

TMPDIR="$(mktemp -d /tmp/recovery_test_XXXXXX)"
trap 'cleanup' EXIT

PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    echo "Temp dir: $TMPDIR"
}

PASS=0
FAIL=0

check() {
    local desc="$1"
    local file="$2"
    local pattern="$3"
    if grep -q "$pattern" "$file"; then
        echo "  PASS: $desc"
        ((PASS++))
    else
        echo "  FAIL: $desc (pattern '$pattern' not found)"
        ((FAIL++))
    fi
}

# ====================================================================
# TEST 1: CME snapshot recovery
# ====================================================================
echo ""
echo "=== TEST 1: CME snapshot recovery ==="

# Start CME sim.
"$CME_SIM" \
    --ilink3-port "$ILINK3_PORT" \
    --mdp3-group "$MDP3_GROUP" \
    --mdp3-port "$MDP3_PORT" \
    --snapshot-group "$SNAPSHOT_GROUP" \
    --snapshot-port "$SNAPSHOT_PORT" \
    --secdef-group "$SECDEF_GROUP" \
    --secdef-port "$SECDEF_PORT" \
    --products "$INSTRUMENT_CME" \
    >"$TMPDIR/cme_sim.log" 2>&1 &
PIDS+=($!)
CME_SIM_PID=$!
sleep 1

# Submit some orders to build a book.
"$TRADER" \
    --exchange cme \
    --host 127.0.0.1 \
    --port "$ILINK3_PORT" \
    --md-group "$MDP3_GROUP" \
    --md-port "$MDP3_PORT" \
    --instrument "$INSTRUMENT_CME" \
    --strategy market-maker \
    --ref-price 5000.00 \
    --spread 2.50 \
    --rate 10 \
    --duration 3 \
    >"$TMPDIR/cme_trader.log" 2>&1 &
PIDS+=($!)

# Wait for orders to build a book + snapshot to publish (>5s).
echo "  Waiting for book to build and snapshot to publish..."
sleep 6

# Start observer with snapshot recovery (late joiner).
"$OBSERVER" \
    --exchange cme \
    --group "$MDP3_GROUP" \
    --port "$MDP3_PORT" \
    --instrument "$INSTRUMENT_CME" \
    --auto-discover \
    --secdef-group "$SECDEF_GROUP" \
    --secdef-port "$SECDEF_PORT" \
    --recovery snapshot \
    --snapshot-group "$SNAPSHOT_GROUP" \
    --snapshot-port "$SNAPSHOT_PORT" \
    --transitions \
    >"$TMPDIR/cme_observer.log" 2>&1 &
PIDS+=($!)
CME_OBS_PID=$!

sleep 3

# Stop observer.
kill -TERM "$CME_OBS_PID" 2>/dev/null || true
wait "$CME_OBS_PID" 2>/dev/null || true

# Check lifecycle logs.
echo "  Checking CME observer logs..."
check "Observer logged recovery start" \
    "$TMPDIR/cme_observer.log" "Starting snapshot recovery"
check "Observer logged recovery complete" \
    "$TMPDIR/cme_observer.log" "Recovery complete"
check "Observer logged incremental switch" \
    "$TMPDIR/cme_observer.log" "Switching to incremental feed"

# Stop sim.
kill -TERM "$CME_SIM_PID" 2>/dev/null || true
wait "$CME_SIM_PID" 2>/dev/null || true

# ====================================================================
# TEST 2: ICE TCP snapshot recovery
# ====================================================================
echo ""
echo "=== TEST 2: ICE TCP snapshot recovery ==="

# Start ICE sim.
"$ICE_SIM" \
    --fix-port "$ICE_FIX_PORT" \
    --impact-group "$ICE_MD_GROUP" \
    --impact-port "$ICE_MD_PORT" \
    --snapshot-port "$ICE_SNAPSHOT_PORT" \
    --products "$INSTRUMENT_ICE" \
    >"$TMPDIR/ice_sim.log" 2>&1 &
PIDS+=($!)
ICE_SIM_PID=$!
sleep 1

# Submit orders.
"$TRADER" \
    --exchange ice \
    --host 127.0.0.1 \
    --port "$ICE_FIX_PORT" \
    --md-group "$ICE_MD_GROUP" \
    --md-port "$ICE_MD_PORT" \
    --instrument "$INSTRUMENT_ICE" \
    --strategy market-maker \
    --ref-price 100.00 \
    --spread 0.50 \
    --rate 10 \
    --duration 3 \
    >"$TMPDIR/ice_trader.log" 2>&1 &
PIDS+=($!)

echo "  Waiting for book to build..."
sleep 4

# Start observer with TCP recovery.
"$OBSERVER" \
    --exchange ice \
    --group "$ICE_MD_GROUP" \
    --port "$ICE_MD_PORT" \
    --instrument "$INSTRUMENT_ICE" \
    --recovery tcp \
    --snapshot-host 127.0.0.1 \
    --snapshot-port "$ICE_SNAPSHOT_PORT" \
    --transitions \
    >"$TMPDIR/ice_observer.log" 2>&1 &
PIDS+=($!)
ICE_OBS_PID=$!

sleep 3

# Stop observer.
kill -TERM "$ICE_OBS_PID" 2>/dev/null || true
wait "$ICE_OBS_PID" 2>/dev/null || true

echo "  Checking ICE observer logs..."
check "Observer logged recovery start" \
    "$TMPDIR/ice_observer.log" "Starting tcp recovery"
check "Observer logged recovery complete" \
    "$TMPDIR/ice_observer.log" "Recovery complete"
check "Observer logged incremental switch" \
    "$TMPDIR/ice_observer.log" "Switching to incremental feed"

# Stop sim.
kill -TERM "$ICE_SIM_PID" 2>/dev/null || true
wait "$ICE_SIM_PID" 2>/dev/null || true

# ====================================================================
# Summary
# ====================================================================
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [[ "$FAIL" -gt 0 ]]; then
    echo "FAIL"
    exit 1
fi

echo "PASS"
exit 0
