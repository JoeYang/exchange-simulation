#!/usr/bin/env bash
set -uo pipefail
# NOTE: NOT using set -e — we handle errors explicitly via report()

# Exchange Smoke Test — All Exchanges
# Tests: sim startup, order entry, market data flow, observer receives data
#
# Usage: ./smoke_test_all.sh [cme|ice|krx|all]

EXCHANGE="${1:-all}"
PASS=0
FAIL=0
RESULTS=()

ALL_PIDS=()

cleanup() {
    # Kill all tracked PIDs
    for pid in "${ALL_PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${ALL_PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    ALL_PIDS=()
    rm -f /dev/shm/smoke-* 2>/dev/null || true
}
trap cleanup EXIT

report() {
    local name="$1" result="$2"
    if [ "$result" = "PASS" ]; then
        RESULTS+=("  \033[32m$result\033[0m  $name")
        ((PASS++))
    else
        RESULTS+=("  \033[31m$result\033[0m  $name")
        ((FAIL++))
    fi
}

echo "=== Exchange Smoke Test ==="
echo ""

# Build all binaries
echo "[BUILD] Building all binaries..."
bazel build //cme:cme-sim //ice:ice-sim //krx:krx-sim \
    //tools:exchange-trader //tools:exchange-observer \
    //tools:ilink3-send-order 2>&1 | tail -1
echo ""

# ─────────────────────────────────────────────────────
# CME Smoke Test
# ─────────────────────────────────────────────────────
test_cme() {
    echo "[CME] Starting CME smoke test..."

    # Start sim
    bazel-bin/cme/cme-sim --shm-path /smoke-cme \
        > /tmp/smoke_cme_sim.log 2>&1 &
    local SIM_PID=$!
    ALL_PIDS+=($SIM_PID)
    sleep 2

    if ! kill -0 $SIM_PID 2>/dev/null; then
        echo "  FAIL: cme-sim crashed"
        cat /tmp/smoke_cme_sim.log
        report "CME sim startup" "FAIL"
        return
    fi
    report "CME sim startup" "PASS"

    # Start observer
    bazel-bin/tools/exchange-observer --exchange cme \
        --group 239.0.31.1 --port 14310 --instrument ES \
        --transitions --journal /tmp/smoke_cme_observer.journal \
        > /tmp/smoke_cme_obs.log 2>/tmp/smoke_cme_obs_err.log &
    local OBS_PID=$!
    ALL_PIDS+=($OBS_PID)
    sleep 1

    # Send crossing orders
    timeout 5 bazel-bin/tools/ilink3-send-order --port 9100 \
        --instrument ES --side BUY --price 5000.00 --qty 10 \
        --type LIMIT --tif DAY --account 1 \
        > /tmp/smoke_cme_order1.log 2>&1 || true
    sleep 0.5

    timeout 5 bazel-bin/tools/ilink3-send-order --port 9100 \
        --instrument ES --side SELL --price 5000.00 --qty 10 \
        --type LIMIT --tif DAY --account 2 \
        > /tmp/smoke_cme_order2.log 2>&1 || true
    sleep 1

    # Check observer received data
    if [ -f /tmp/smoke_cme_observer.journal ] && [ -s /tmp/smoke_cme_observer.journal ]; then
        local LINES=$(wc -l < /tmp/smoke_cme_observer.journal)
        report "CME observer received data ($LINES lines)" "PASS"
    else
        report "CME observer received data" "FAIL"
    fi

    # Check for trade in observer (transitions on stderr, journal on disk)
    if grep -q "TRADE" /tmp/smoke_cme_obs_err.log 2>/dev/null; then
        report "CME trade visible in transitions" "PASS"
    elif grep -q "TRADE" /tmp/smoke_cme_observer.journal 2>/dev/null; then
        report "CME trade visible in journal" "PASS"
    elif grep -q "TRADE" /tmp/smoke_cme_obs.log 2>/dev/null; then
        report "CME trade visible in stdout" "PASS"
    else
        report "CME trade visible" "FAIL"
        echo "  Debug: sim log:" && tail -5 /tmp/smoke_cme_sim.log 2>/dev/null || true
        echo "  Debug: observer stderr:" && tail -5 /tmp/smoke_cme_obs_err.log 2>/dev/null || true
        echo "  Debug: order1:" && cat /tmp/smoke_cme_order1.log 2>/dev/null || true
        echo "  Debug: order2:" && cat /tmp/smoke_cme_order2.log 2>/dev/null || true
    fi

    kill $OBS_PID 2>/dev/null || true
    kill $SIM_PID 2>/dev/null || true
    wait $OBS_PID 2>/dev/null || true
    wait $SIM_PID 2>/dev/null || true
    echo ""
}

# ─────────────────────────────────────────────────────
# ICE Smoke Test
# ─────────────────────────────────────────────────────
test_ice() {
    echo "[ICE] Starting ICE smoke test..."

    # Start sim
    bazel-bin/ice/ice-sim --shm-path /smoke-ice \
        > /tmp/smoke_ice_sim.log 2>&1 &
    local SIM_PID=$!
    ALL_PIDS+=($SIM_PID)
    sleep 2

    if ! kill -0 $SIM_PID 2>/dev/null; then
        echo "  FAIL: ice-sim crashed"
        cat /tmp/smoke_ice_sim.log
        report "ICE sim startup" "FAIL"
        return
    fi
    report "ICE sim startup" "PASS"

    # Start observer
    bazel-bin/tools/exchange-observer --exchange ice \
        --group 239.0.32.1 --port 14400 --instrument B \
        --transitions --journal /tmp/smoke_ice_observer.journal \
        > /tmp/smoke_ice_obs.log 2>/tmp/smoke_ice_obs_err.log &
    local OBS_PID=$!
    ALL_PIDS+=($OBS_PID)
    sleep 1

    # Send orders via trader (3 seconds of random-walk)
    timeout 7 bazel-bin/tools/exchange-trader --exchange ice \
        --host 127.0.0.1 --port 9200 \
        --account 1 --instrument B \
        --strategy market-maker --ref-price 81.95 --spread 0.10 --rate 10 \
        > /tmp/smoke_ice_trader1.log 2>&1 &
    local T1_PID=$!

    timeout 7 bazel-bin/tools/exchange-trader --exchange ice \
        --host 127.0.0.1 --port 9200 \
        --account 2 --instrument B \
        --strategy market-maker --ref-price 82.05 --spread 0.10 --rate 10 \
        > /tmp/smoke_ice_trader2.log 2>&1 &
    local T2_PID=$!

    sleep 6

    # Check observer
    if [ -f /tmp/smoke_ice_observer.journal ] && [ -s /tmp/smoke_ice_observer.journal ]; then
        local LINES=$(wc -l < /tmp/smoke_ice_observer.journal)
        report "ICE observer received data ($LINES lines)" "PASS"
    else
        report "ICE observer received data" "FAIL"
    fi

    if grep -q "TRADE" /tmp/smoke_ice_obs_err.log 2>/dev/null || \
       grep -q "TRADE" /tmp/smoke_ice_observer.journal 2>/dev/null || \
       grep -q "TRADE" /tmp/smoke_ice_obs.log 2>/dev/null; then
        report "ICE trade visible" "PASS"
    else
        report "ICE trade visible" "FAIL"
        echo "  Debug: sim log:" && tail -5 /tmp/smoke_ice_sim.log 2>/dev/null || true
        echo "  Debug: observer stderr:" && tail -5 /tmp/smoke_ice_obs_err.log 2>/dev/null || true
    fi

    kill $OBS_PID $SIM_PID $T1_PID $T2_PID 2>/dev/null || true
    wait $OBS_PID $SIM_PID $T1_PID $T2_PID 2>/dev/null || true
    echo ""
}

# ─────────────────────────────────────────────────────
# KRX Smoke Test
# ─────────────────────────────────────────────────────
test_krx() {
    echo "[KRX] Starting KRX smoke test..."

    # Start sim
    bazel-bin/krx/krx-sim --shm-path /smoke-krx \
        > /tmp/smoke_krx_sim.log 2>&1 &
    local SIM_PID=$!
    ALL_PIDS+=($SIM_PID)
    sleep 2

    if ! kill -0 $SIM_PID 2>/dev/null; then
        echo "  FAIL: krx-sim crashed"
        cat /tmp/smoke_krx_sim.log
        report "KRX sim startup" "FAIL"
        return
    fi
    report "KRX sim startup" "PASS"

    # Start observer
    bazel-bin/tools/exchange-observer --exchange krx \
        --group 224.0.33.1 --port 16000 --instrument KS \
        --transitions --journal /tmp/smoke_krx_observer.journal \
        > /tmp/smoke_krx_obs.log 2>/tmp/smoke_krx_obs_err.log &
    local OBS_PID=$!
    ALL_PIDS+=($OBS_PID)
    sleep 1

    # Send orders via trader
    timeout 7 bazel-bin/tools/exchange-trader --exchange krx \
        --host 127.0.0.1 --port 9300 \
        --account 1 --instrument KS \
        --strategy market-maker --ref-price 349.75 --spread 0.50 --rate 10 \
        > /tmp/smoke_krx_trader1.log 2>&1 &
    local T1_PID=$!

    timeout 7 bazel-bin/tools/exchange-trader --exchange krx \
        --host 127.0.0.1 --port 9300 \
        --account 2 --instrument KS \
        --strategy market-maker --ref-price 350.25 --spread 0.50 --rate 10 \
        > /tmp/smoke_krx_trader2.log 2>&1 &
    local T2_PID=$!

    sleep 6

    # Check observer
    if [ -f /tmp/smoke_krx_observer.journal ] && [ -s /tmp/smoke_krx_observer.journal ]; then
        local LINES=$(wc -l < /tmp/smoke_krx_observer.journal)
        report "KRX observer received data ($LINES lines)" "PASS"
    else
        report "KRX observer received data" "FAIL"
    fi

    if grep -q "TRADE" /tmp/smoke_krx_obs_err.log 2>/dev/null || \
       grep -q "TRADE" /tmp/smoke_krx_observer.journal 2>/dev/null || \
       grep -q "TRADE" /tmp/smoke_krx_obs.log 2>/dev/null; then
        report "KRX trade visible" "PASS"
    else
        report "KRX trade visible" "FAIL"
        echo "  Debug: sim log:" && tail -5 /tmp/smoke_krx_sim.log 2>/dev/null || true
        echo "  Debug: observer stderr:" && tail -5 /tmp/smoke_krx_obs_err.log 2>/dev/null || true
        echo "  Debug: trader1:" && tail -5 /tmp/smoke_krx_trader1.log 2>/dev/null || true
        echo "  Debug: trader2:" && tail -5 /tmp/smoke_krx_trader2.log 2>/dev/null || true
    fi

    kill $OBS_PID $SIM_PID $T1_PID $T2_PID 2>/dev/null || true
    wait $OBS_PID $SIM_PID $T1_PID $T2_PID 2>/dev/null || true
    echo ""
}

# ─────────────────────────────────────────────────────
# Run tests
# ─────────────────────────────────────────────────────

case "$EXCHANGE" in
    cme) test_cme ;;
    ice) test_ice ;;
    krx) test_krx ;;
    all) test_cme; cleanup; sleep 2; test_ice; cleanup; sleep 2; test_krx ;;
    *) echo "Usage: $0 [cme|ice|krx|all]"; exit 1 ;;
esac

# ─────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────
echo "==========================================="
echo "  Smoke Test Results"
echo "==========================================="
for r in "${RESULTS[@]}"; do
    echo -e "$r"
done
echo ""
echo "  Total: $((PASS + FAIL))  Pass: $PASS  Fail: $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo -e "  \033[31mSMOKE TEST FAILED\033[0m"
    echo ""
    echo "  Logs:"
    echo "    /tmp/smoke_*_sim.log    — sim stderr"
    echo "    /tmp/smoke_*_obs.log    — observer output"
    echo "    /tmp/smoke_*_trader*.log — trader output"
    echo "    /tmp/smoke_*_observer.journal — observer journal"
    exit 1
else
    echo -e "  \033[32mALL SMOKE TESTS PASSED\033[0m"
fi
