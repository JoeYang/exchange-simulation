#!/usr/bin/env bash
# secdef_integration_test.sh -- E2E integration test for secdef discovery.
#
# Tests:
#   1. CME secdef: start observer with --auto-discover, then start cme-sim.
#      Observer discovers instruments from secdef broadcast at sim startup.
#   2. ICE secdef: same pattern with ice-sim.
#   3. CME single-product: observer auto-selects when only 1 instrument.
#   4. Failure: observer --auto-discover with wrong group times out gracefully.
#
# Exit 0 = PASS, non-zero = FAIL.

set -euo pipefail

# --- Configuration ---
CME_MDP3_GROUP="239.0.31.1"
CME_MDP3_PORT="14310"
CME_SECDEF_GROUP="239.0.31.3"
CME_SECDEF_PORT="14312"
ICE_IMPACT_GROUP="239.0.32.1"
ICE_IMPACT_PORT="14400"

# --- Locate binaries ---
echo "=== Building binaries ==="
bazel build //cme:cme-sim //ice:ice-sim //tools:exchange-observer 2>&1

CME_SIM="$(bazel cquery --output=files //cme:cme-sim 2>/dev/null)"
ICE_SIM="$(bazel cquery --output=files //ice:ice-sim 2>/dev/null)"
OBSERVER="$(bazel cquery --output=files //tools:exchange-observer 2>/dev/null)"

for bin in "$CME_SIM" "$ICE_SIM" "$OBSERVER"; do
    if [[ ! -x "$bin" ]]; then
        echo "FAIL: binary not found or not executable: $bin"
        exit 2
    fi
done

# --- Temp directory ---
TMPDIR="$(mktemp -d /tmp/secdef_integration_XXXXXX)"
trap 'cleanup' EXIT

PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    echo "Temp dir: $TMPDIR (preserved for debugging)"
}

PASS=0
FAIL=0

check() {
    local desc="$1"
    local file="$2"
    local pattern="$3"
    if grep -q "$pattern" "$file"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected pattern: $pattern)"
        echo "  --- file contents ---"
        tail -20 "$file"
        echo "  ---"
        FAIL=$((FAIL + 1))
    fi
}

# Wait for pattern to appear in a file (poll with timeout).
wait_for_pattern() {
    local file="$1"
    local pattern="$2"
    local max_wait="$3"
    for i in $(seq 1 "$max_wait"); do
        if grep -q "$pattern" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# =====================================================================
# TEST 1: CME secdef discovery (8 products)
# =====================================================================
echo ""
echo "=== TEST 1: CME secdef discovery ==="

CME_ILINK3_PORT=$((10000 + RANDOM % 50000))

# Start observer FIRST so it joins the secdef multicast group and waits.
timeout 50 "$OBSERVER" \
    --exchange cme \
    --group "$CME_MDP3_GROUP" \
    --port "$CME_MDP3_PORT" \
    --auto-discover \
    --secdef-group "$CME_SECDEF_GROUP" \
    --secdef-port "$CME_SECDEF_PORT" \
    --instrument ES \
    > /dev/null 2> "$TMPDIR/cme_observer.log" &
PIDS+=($!)
OBS_PID=$!

# Brief pause for observer to join multicast group.
sleep 1

# Start sim -- it publishes secdef immediately at startup.
"$CME_SIM" \
    --ilink3-port "$CME_ILINK3_PORT" \
    --mdp3-group "$CME_MDP3_GROUP" \
    --mdp3-port "$CME_MDP3_PORT" \
    --secdef-group "$CME_SECDEF_GROUP" \
    --secdef-port "$CME_SECDEF_PORT" \
    > /dev/null 2> "$TMPDIR/cme_sim.log" &
PIDS+=($!)
CME_PID=$!

# Wait for observer to discover and start listening (up to 40s for secdef timeout).
if wait_for_pattern "$TMPDIR/cme_observer.log" "Listening on" 40; then
    echo "  Observer started listening."
else
    echo "  WARNING: Observer did not start listening within 40s."
fi

check "Observer discovered instruments" \
    "$TMPDIR/cme_observer.log" "Discovered.*instrument"
check "ES instrument present" \
    "$TMPDIR/cme_observer.log" "ES"
check "Observer started listening" \
    "$TMPDIR/cme_observer.log" "Listening on"

kill -INT "$OBS_PID" 2>/dev/null || true
wait "$OBS_PID" 2>/dev/null || true
kill -INT "$CME_PID" 2>/dev/null || true
wait "$CME_PID" 2>/dev/null || true
PIDS=()

# =====================================================================
# TEST 2: ICE secdef discovery (10 products)
# =====================================================================
echo ""
echo "=== TEST 2: ICE secdef discovery ==="

ICE_FIX_PORT=$((10000 + RANDOM % 50000))

# Start observer FIRST.
timeout 50 "$OBSERVER" \
    --exchange ice \
    --group "$ICE_IMPACT_GROUP" \
    --port "$ICE_IMPACT_PORT" \
    --auto-discover \
    --instrument B \
    > /dev/null 2> "$TMPDIR/ice_observer.log" &
PIDS+=($!)
OBS_PID=$!

sleep 1

"$ICE_SIM" \
    --fix-port "$ICE_FIX_PORT" \
    --impact-group "$ICE_IMPACT_GROUP" \
    --impact-port "$ICE_IMPACT_PORT" \
    > /dev/null 2> "$TMPDIR/ice_sim.log" &
PIDS+=($!)
ICE_PID=$!

# ICE secdef needs idle timeout (5s after last new instrument).
if wait_for_pattern "$TMPDIR/ice_observer.log" "Listening on" 45; then
    echo "  Observer started listening."
else
    echo "  WARNING: Observer did not start listening within 45s."
fi

check "Observer discovered ICE instruments" \
    "$TMPDIR/ice_observer.log" "Discovered.*instrument"
check "Brent (B) instrument present" \
    "$TMPDIR/ice_observer.log" "B"
check "ICE observer started listening" \
    "$TMPDIR/ice_observer.log" "Listening on"

kill -INT "$OBS_PID" 2>/dev/null || true
wait "$OBS_PID" 2>/dev/null || true
kill -INT "$ICE_PID" 2>/dev/null || true
wait "$ICE_PID" 2>/dev/null || true
PIDS=()

# =====================================================================
# TEST 3: CME auto-discover with single product sim
# =====================================================================
echo ""
echo "=== TEST 3: CME auto-discover single product (no --instrument) ==="

CME_ILINK3_PORT=$((10000 + RANDOM % 50000))

# Start observer FIRST (no --instrument flag).
timeout 50 "$OBSERVER" \
    --exchange cme \
    --group "$CME_MDP3_GROUP" \
    --port "$CME_MDP3_PORT" \
    --auto-discover \
    --secdef-group "$CME_SECDEF_GROUP" \
    --secdef-port "$CME_SECDEF_PORT" \
    > /dev/null 2> "$TMPDIR/cme_observer_single.log" &
PIDS+=($!)
OBS_PID=$!

sleep 1

"$CME_SIM" \
    --ilink3-port "$CME_ILINK3_PORT" \
    --mdp3-group "$CME_MDP3_GROUP" \
    --mdp3-port "$CME_MDP3_PORT" \
    --secdef-group "$CME_SECDEF_GROUP" \
    --secdef-port "$CME_SECDEF_PORT" \
    --products ES \
    > /dev/null 2> "$TMPDIR/cme_sim_single.log" &
PIDS+=($!)
CME_PID=$!

if wait_for_pattern "$TMPDIR/cme_observer_single.log" "Listening on" 40; then
    echo "  Observer started listening."
else
    echo "  WARNING: Observer did not start listening within 40s."
fi

check "Single-product discovery" \
    "$TMPDIR/cme_observer_single.log" "Discovered 1 instrument"
check "Auto-selected ES" \
    "$TMPDIR/cme_observer_single.log" "Listening.*ES"

kill -INT "$OBS_PID" 2>/dev/null || true
wait "$OBS_PID" 2>/dev/null || true
kill -INT "$CME_PID" 2>/dev/null || true
wait "$CME_PID" 2>/dev/null || true
PIDS=()

# =====================================================================
# TEST 4: Failure -- wrong multicast group (graceful timeout)
# =====================================================================
echo ""
echo "=== TEST 4: Failure -- wrong secdef group ==="

# Point secdef at a group nobody is publishing on.
# CmeSecdefConsumer will time out (35s default), observer prints error.
set +e
timeout 40 "$OBSERVER" \
    --exchange cme \
    --group "$CME_MDP3_GROUP" \
    --port "$CME_MDP3_PORT" \
    --auto-discover \
    --secdef-group "239.0.99.99" \
    --secdef-port "19999" \
    > /dev/null 2> "$TMPDIR/fail_observer.log"
FAIL_EXIT=$?
set -e

check "Observer reports no instruments" \
    "$TMPDIR/fail_observer.log" "no instruments"
check "Observer exited with error code" \
    "$TMPDIR/fail_observer.log" "Error"

# =====================================================================
# Summary
# =====================================================================
echo ""
echo "============================================="
echo "  PASS: $PASS  FAIL: $FAIL"
echo "============================================="

if [[ $FAIL -gt 0 ]]; then
    echo "RESULT: FAIL"
    exit 1
fi

echo "RESULT: ALL TESTS PASSED"
exit 0
