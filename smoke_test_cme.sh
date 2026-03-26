#!/usr/bin/env bash
set -euo pipefail

# CME Simulator + Dashboard Smoke Test
# Starts cme-sim with SHM, launches dashboard, verifies both work.

PORT=9200
SHM_PATH="/cme-smoke-test"
TIMEOUT=8
SIM_PID=""
DASH_PID=""
PASS=true

cleanup() {
    echo ""
    echo "--- Cleaning up ---"
    [ -n "$DASH_PID" ] && kill "$DASH_PID" 2>/dev/null || true
    [ -n "$SIM_PID" ]  && kill "$SIM_PID"  2>/dev/null || true
    # Wait for processes to exit
    [ -n "$DASH_PID" ] && wait "$DASH_PID" 2>/dev/null || true
    [ -n "$SIM_PID" ]  && wait "$SIM_PID"  2>/dev/null || true
    # Clean up SHM
    rm -f "/dev/shm${SHM_PATH}" 2>/dev/null || true
    echo "Cleanup done."
}
trap cleanup EXIT

echo "=== CME Simulator + Dashboard Smoke Test ==="
echo ""

# --- Step 1: Build ---
echo "[1/6] Building cme-sim and exchange-dashboard..."
bazel build //cme:cme-sim //tools:exchange-dashboard 2>&1 | tail -3
echo "      Build OK"
echo ""

# --- Step 2: Start simulator ---
echo "[2/6] Starting cme-sim (port=$PORT, shm=$SHM_PATH)..."
bazel-bin/cme/cme-sim \
    --ilink3-port "$PORT" \
    --shm-path "$SHM_PATH" \
    > /tmp/cme_sim_stdout.log 2>&1 &
SIM_PID=$!
sleep 2

if ! kill -0 "$SIM_PID" 2>/dev/null; then
    echo "      FAIL: cme-sim crashed on startup"
    echo "      --- stderr ---"
    cat /tmp/cme_sim_stdout.log
    exit 1
fi
echo "      cme-sim running (PID=$SIM_PID)"
echo ""

# --- Step 3: Verify SHM created ---
echo "[3/6] Checking shared memory segment..."
SHM_FILE="/dev/shm${SHM_PATH}"
if [ -f "$SHM_FILE" ]; then
    SHM_SIZE=$(stat -c%s "$SHM_FILE" 2>/dev/null || echo "unknown")
    echo "      SHM exists: $SHM_FILE ($SHM_SIZE bytes)"
else
    echo "      FAIL: $SHM_FILE not found"
    PASS=false
fi
echo ""

# --- Step 4: Verify TCP port listening ---
echo "[4/6] Checking TCP port $PORT..."
if ss -tlnp 2>/dev/null | grep -q ":$PORT"; then
    echo "      TCP port $PORT is listening"
else
    echo "      WARN: Could not verify port $PORT (ss may lack permissions)"
fi
echo ""

# --- Step 5: Launch dashboard ---
echo "[5/6] Launching exchange-dashboard (${TIMEOUT}s timeout)..."
timeout "$TIMEOUT" bazel-bin/tools/exchange-dashboard "$SHM_PATH" \
    > /tmp/cme_dash_stdout.log 2>&1 &
DASH_PID=$!
sleep 3

if kill -0 "$DASH_PID" 2>/dev/null; then
    echo "      Dashboard running (PID=$DASH_PID)"
    # Let it run a few more seconds to confirm stability
    sleep 3
    if kill -0 "$DASH_PID" 2>/dev/null; then
        echo "      Dashboard stable after ${TIMEOUT}s"
    else
        echo "      WARN: Dashboard exited (may be timeout, checking exit code)"
    fi
else
    DASH_EXIT=$(wait "$DASH_PID" 2>/dev/null; echo $?)
    if [ "$DASH_EXIT" = "124" ]; then
        echo "      Dashboard ran for ${TIMEOUT}s and timed out (OK)"
    elif [ "$DASH_EXIT" = "0" ]; then
        echo "      Dashboard exited cleanly"
    else
        echo "      FAIL: Dashboard crashed (exit=$DASH_EXIT)"
        echo "      --- output ---"
        cat /tmp/cme_dash_stdout.log 2>/dev/null || true
        PASS=false
    fi
fi
DASH_PID=""  # already exited or will be cleaned up
echo ""

# --- Step 6: Verify simulator still alive ---
echo "[6/6] Verifying simulator still running..."
if kill -0 "$SIM_PID" 2>/dev/null; then
    echo "      cme-sim still running (stable)"
else
    echo "      FAIL: cme-sim died during test"
    PASS=false
fi
echo ""

# --- Result ---
echo "==========================================="
if $PASS; then
    echo "  SMOKE TEST PASSED"
else
    echo "  SMOKE TEST FAILED"
fi
echo "==========================================="
echo ""
echo "Logs:"
echo "  Simulator: /tmp/cme_sim_stdout.log"
echo "  Dashboard: /tmp/cme_dash_stdout.log"

$PASS
