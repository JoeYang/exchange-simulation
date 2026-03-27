#!/usr/bin/env bash
# sim_integration_test.sh -- Integration test for the exchange simulation.
#
# Starts cme-sim, launches 5 exchange-trader instances + 1 observer,
# runs for a configurable duration (default 5s), stops all gracefully,
# then runs journal-reconciler and asserts all 8 invariants pass.
#
# Exit 0 = PASS, non-zero = FAIL.

set -euo pipefail

# --- Configuration ---
DURATION="${SIM_DURATION:-5}"  # seconds to run the simulation
ILINK3_PORT="${SIM_PORT:-0}"   # 0 = let OS pick a free port
MDP3_GROUP="239.0.31.1"
MDP3_PORT="14310"
INSTRUMENT="ES"
REF_PRICE="5000.00"
SPREAD="2.50"
RATE="5"
MAX_POSITION="10"
NUM_TRADERS=5

# --- Locate binaries (via Bazel runfiles or direct path) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Build all required targets first.
echo "=== Building binaries ==="
bazel build //cme:cme-sim //tools:exchange-trader //tools:exchange-observer //tools:journal-reconciler 2>&1

CME_SIM="$(bazel cquery --output=files //cme:cme-sim 2>/dev/null)"
TRADER="$(bazel cquery --output=files //tools:exchange-trader 2>/dev/null)"
OBSERVER="$(bazel cquery --output=files //tools:exchange-observer 2>/dev/null)"
RECONCILER="$(bazel cquery --output=files //tools:journal-reconciler 2>/dev/null)"

# Verify binaries exist.
for bin in "$CME_SIM" "$TRADER" "$OBSERVER" "$RECONCILER"; do
    if [[ ! -x "$bin" ]]; then
        echo "FAIL: binary not found or not executable: $bin"
        exit 2
    fi
done

# --- Temp directory for journals ---
TMPDIR="$(mktemp -d /tmp/sim_integration_XXXXXX)"
trap 'cleanup' EXIT

PIDS=()

cleanup() {
    echo "=== Cleanup ==="
    # Kill all background processes.
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    # Wait for them to exit.
    for pid in "${PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    echo "Temp dir: $TMPDIR (preserved for debugging)"
}

# --- Start cme-sim ---
echo "=== Starting cme-sim ==="

# Use a known port or let the server pick one. For the integration test
# we use a fixed port to connect traders.
if [[ "$ILINK3_PORT" == "0" ]]; then
    # Pick a random high port.
    ILINK3_PORT=$((10000 + RANDOM % 50000))
fi

"$CME_SIM" \
    --ilink3-port "$ILINK3_PORT" \
    --mdp3-group "$MDP3_GROUP" \
    --mdp3-port "$MDP3_PORT" \
    --products "$INSTRUMENT" \
    > "$TMPDIR/cme_sim_stdout.log" 2> "$TMPDIR/cme_sim_stderr.log" &
PIDS+=($!)
SIM_PID=$!
echo "cme-sim PID=$SIM_PID, iLink3 port=$ILINK3_PORT"

# Wait for cme-sim to be ready (accept TCP connections).
echo "Waiting for cme-sim to accept connections..."
MAX_WAIT=10
for i in $(seq 1 $MAX_WAIT); do
    if bash -c "echo > /dev/tcp/127.0.0.1/$ILINK3_PORT" 2>/dev/null; then
        echo "cme-sim ready after ${i}s"
        break
    fi
    if [[ $i -eq $MAX_WAIT ]]; then
        echo "FAIL: cme-sim did not start within ${MAX_WAIT}s"
        cat "$TMPDIR/cme_sim_stderr.log"
        exit 1
    fi
    sleep 1
done

# --- Start observer ---
echo "=== Starting observer ==="
"$OBSERVER" \
    --exchange cme \
    --group "$MDP3_GROUP" \
    --port "$MDP3_PORT" \
    --instrument "$INSTRUMENT" \
    --journal "$TMPDIR/observer.journal" \
    > /dev/null 2> "$TMPDIR/observer_stderr.log" &
PIDS+=($!)
OBSERVER_PID=$!
echo "Observer PID=$OBSERVER_PID, journal=$TMPDIR/observer.journal"

# Brief pause for observer to join multicast group.
sleep 1

# --- Start traders ---
echo "=== Starting $NUM_TRADERS traders ==="
TRADER_PIDS=()
JOURNAL_PATHS=""

for i in $(seq 1 "$NUM_TRADERS"); do
    JOURNAL="$TMPDIR/client_${i}.journal"
    SEED=$((42 + i))  # deterministic seeds for reproducibility

    # Alternate strategies: odd = random-walk, even = market-maker.
    if (( i % 2 == 0 )); then
        STRATEGY="market-maker"
    else
        STRATEGY="random-walk"
    fi

    "$TRADER" \
        --exchange cme \
        --host 127.0.0.1 \
        --port "$ILINK3_PORT" \
        --instrument "$INSTRUMENT" \
        --strategy "$STRATEGY" \
        --ref-price "$REF_PRICE" \
        --spread "$SPREAD" \
        --rate "$RATE" \
        --max-position "$MAX_POSITION" \
        --journal "$JOURNAL" \
        --seed "$SEED" \
        > /dev/null 2> "$TMPDIR/trader_${i}_stderr.log" &
    PIDS+=($!)
    TRADER_PIDS+=($!)
    echo "  Trader $i: PID=$!, strategy=$STRATEGY, seed=$SEED"

    if [[ -z "$JOURNAL_PATHS" ]]; then
        JOURNAL_PATHS="$JOURNAL"
    else
        JOURNAL_PATHS="$JOURNAL_PATHS,$JOURNAL"
    fi

    # Stagger connections slightly to avoid overwhelming the sim.
    sleep 0.2
done

# --- Run simulation for DURATION seconds ---
echo "=== Running simulation for ${DURATION}s ==="
sleep "$DURATION"

# --- Graceful shutdown ---
echo "=== Stopping traders ==="
for pid in "${TRADER_PIDS[@]}"; do
    kill -INT "$pid" 2>/dev/null || true
done
# Wait for traders to finish (cancel open orders, drain responses).
for pid in "${TRADER_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done
echo "All traders stopped."

# Give observer a moment to receive final market data.
sleep 2

echo "=== Stopping observer ==="
kill -INT "$OBSERVER_PID" 2>/dev/null || true
wait "$OBSERVER_PID" 2>/dev/null || true
echo "Observer stopped."

echo "=== Stopping cme-sim ==="
kill -INT "$SIM_PID" 2>/dev/null || true
wait "$SIM_PID" 2>/dev/null || true
echo "cme-sim stopped."

# Clear PIDS since everything is already stopped.
PIDS=()

# --- Report journal stats ---
echo ""
echo "=== Journal files ==="
for f in "$TMPDIR"/*.journal; do
    lines=$(wc -l < "$f")
    echo "  $(basename "$f"): $lines lines"
done

# --- Run reconciler ---
echo ""
echo "=== Running journal-reconciler ==="
RECONCILER_OUTPUT="$TMPDIR/reconciler_output.txt"

set +e
"$RECONCILER" \
    --observer-journal "$TMPDIR/observer.journal" \
    --client-journals "$JOURNAL_PATHS" \
    | tee "$RECONCILER_OUTPUT"
RECONCILER_EXIT=$?
set -e

echo ""
if [[ $RECONCILER_EXIT -eq 0 ]]; then
    echo "=== RESULT: PASS (all 8 invariants satisfied) ==="
    exit 0
else
    echo "=== RESULT: FAIL (invariant violations detected) ==="
    echo ""
    echo "--- cme-sim stderr ---"
    tail -20 "$TMPDIR/cme_sim_stderr.log"
    echo ""
    echo "--- Trader 1 stderr ---"
    tail -10 "$TMPDIR/trader_1_stderr.log"
    echo ""
    echo "--- Observer stderr ---"
    tail -10 "$TMPDIR/observer_stderr.log"
    exit 1
fi
