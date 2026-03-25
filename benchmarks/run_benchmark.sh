#!/usr/bin/env bash
set -euo pipefail

echo "=== Exchange Core Performance Benchmark ==="
echo "Date: $(date)"
echo "CPU: $(lscpu | grep 'Model name' | sed 's/.*: *//')"
echo "Cores: $(nproc)"
echo "Kernel: $(uname -r)"
echo ""

BENCH_BIN="bazel-bin/benchmarks/exchange_benchmark"

echo "Building with -O3 -march=native..."
bazel build //benchmarks:exchange_benchmark -c opt 2>/dev/null
echo ""

echo "=========================================="
echo "=== Run 1: Without CPU pinning        ==="
echo "=========================================="
echo ""
$BENCH_BIN

echo ""
echo "=========================================="
echo "=== Run 2: With taskset (core 0)      ==="
echo "=========================================="
echo ""
taskset -c 0 $BENCH_BIN

echo ""
echo "=========================================="
echo "=== Run 3: With numactl + taskset     ==="
echo "=========================================="
echo ""
if command -v numactl &>/dev/null; then
    numactl --cpunodebind=0 --membind=0 -- taskset -c 0 $BENCH_BIN
else
    echo "numactl not installed, skipping. Install with: sudo apt install numactl"
fi
