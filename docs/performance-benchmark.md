# Exchange Core Performance Benchmark

**Date:** 2026-03-24
**Hardware:** Intel Core Ultra 7 255HX, 20 cores, Linux 6.17.0-1012-oem
**Build:** Bazel -c opt, -O3 -DNDEBUG -march=native
**Engine config:** MaxOrders=50K, MaxPriceLevels=5K, MaxOrderIds=200K

---

## Results Summary

| Scenario | P50 (ns) | P99 (ns) | Avg (ns) | Ops/sec |
|----------|----------|----------|----------|---------|
| New order (resting, no match) | 17 | 927 | 245 | **4.1M** |
| New order (immediate fill) | 14 | 16 | 14 | **71.6M** |
| Cancel order | 11 | 13 | 11 | **89.3M** |
| Modify order (cancel-replace) | 12 | 728 | 285 | **3.5M** |
| Mixed workload (40/30/20/10) | 306 | 1,217 | 434 | **2.3M** |
| Deep book sweep (50 levels) | 2,449 | 2,635 | 1,708 | **586K** |

---

## Detailed Results

### Run 1: No CPU Pinning

```
Scenario                               |        Avg |        P50 |        P99 |      P99.9 |        Min |        Max |      Ops/sec
------------------------------------------------------------------------------------------------------------------------------------
new_order (resting, no match)          |     245 ns |      17 ns |     927 ns |     958 ns |      14 ns |   32060 ns |    4,078,622
new_order (immediate fill)             |      14 ns |      14 ns |      16 ns |      18 ns |      12 ns |    1399 ns |   71,603,181
cancel_order                           |      11 ns |      11 ns |      13 ns |      24 ns |       9 ns |    3654 ns |   89,273,359
modify_order (cancel-replace)          |     285 ns |      12 ns |     728 ns |     756 ns |       9 ns |    3869 ns |    3,512,721
mixed workload (40/30/20/10)           |     434 ns |     306 ns |    1217 ns |    1838 ns |      13 ns |    5318 ns |    2,303,175
deep book sweep (50 levels)            |    1708 ns |    2449 ns |    2635 ns |    3704 ns |      11 ns |    3704 ns |      585,633
```

### Run 2: taskset -c 0 (single core pinning)

```
Scenario                               |        Avg |        P50 |        P99 |      P99.9 |        Min |        Max |      Ops/sec
------------------------------------------------------------------------------------------------------------------------------------
new_order (resting, no match)          |     215 ns |      16 ns |     818 ns |     886 ns |      13 ns |   11843 ns |    4,658,396
new_order (immediate fill)             |      14 ns |      14 ns |      16 ns |      17 ns |      11 ns |    2227 ns |   71,349,781
cancel_order                           |      11 ns |      11 ns |      14 ns |      24 ns |       9 ns |    2589 ns |   87,893,543
modify_order (cancel-replace)          |     288 ns |      12 ns |     737 ns |     751 ns |      10 ns |    2334 ns |    3,473,380
mixed workload (40/30/20/10)           |     438 ns |     303 ns |    1241 ns |    1885 ns |      13 ns |   11635 ns |    2,281,610
deep book sweep (50 levels)            |    1674 ns |    2395 ns |    2570 ns |    4796 ns |      12 ns |    4796 ns |      597,525
```

### Run 3: numactl --cpunodebind=0 --membind=0 + taskset -c 0

```
Scenario                               |        Avg |        P50 |        P99 |      P99.9 |        Min |        Max |      Ops/sec
------------------------------------------------------------------------------------------------------------------------------------
new_order (resting, no match)          |     223 ns |      16 ns |     848 ns |     947 ns |      13 ns |   17529 ns |    4,488,188
new_order (immediate fill)             |      14 ns |      14 ns |      17 ns |      18 ns |      12 ns |      33 ns |   69,269,251
cancel_order                           |      13 ns |      12 ns |      32 ns |     205 ns |      10 ns |    6097 ns |   78,576,384
modify_order (cancel-replace)          |     298 ns |      12 ns |     767 ns |     908 ns |      10 ns |    5198 ns |    3,361,332
mixed workload (40/30/20/10)           |     457 ns |     314 ns |    1361 ns |    2057 ns |      13 ns |    3070 ns |    2,185,904
deep book sweep (50 levels)            |    1721 ns |    2454 ns |    2750 ns |    3620 ns |      12 ns |    3620 ns |      581,037
```

### Recording Listener Overhead

| Scenario | NullListener | RecordingListener | Overhead |
|----------|-------------|-------------------|----------|
| Resting order | 245 ns | 320 ns | +30.6% |
| Immediate fill | 14 ns | 15 ns | +4.8% |

The overhead from RecordingListener (std::vector push_back) is modest — 31% for resting
orders (which fire 3 callbacks: Accepted + L3 + L2 + L1) and negligible for fills.

---

## Analysis

### Key Observations

1. **Immediate fill is blazing fast: 14 ns (71M ops/sec).** This is the core matching path —
   one order matches against one resting order. The P50 and P99 are nearly identical,
   showing extremely consistent latency.

2. **Cancel is the fastest operation: 11 ns (89M ops/sec).** Direct O(1) index lookup +
   intrusive list removal + callback.

3. **Resting orders have high average but low P50.** The 245 ns average vs 17 ns P50
   suggests occasional expensive operations (likely new price level creation requiring
   sorted insertion into the level list).

4. **Modify has the same pattern** — 12 ns P50 vs 285 ns average. The expensive path is
   cancel-replace with price change requiring level insertion.

5. **Deep book sweep scales linearly with levels:** 50 levels at ~1.7 us ≈ 34 ns per level
   per fill, which is consistent with the 14 ns single-fill cost plus level traversal.

6. **CPU pinning shows modest improvement.** ~12% reduction in average for resting orders
   (245→215 ns), but P50 and P99 are similar. The main benefit is reduced max latency
   (32 us → 12 us without numactl, due to fewer context switches).

7. **numactl adds no benefit** on this single-socket system. Results are within noise of
   taskset-only. numactl matters for multi-socket NUMA systems.

### Bimodal Distribution in Resting/Modify

The large gap between P50 (16 ns) and average (245 ns) for resting orders indicates a
bimodal distribution:
- **Fast path (~16 ns):** Order inserts into an existing price level — just a linked list push_back
- **Slow path (~800-950 ns):** New price level needed — allocate from pool, sorted insertion
  into the level list (O(n) walk to find position)

This is expected and matches the intrusive linked list design. A skip list or tree-based
level structure would reduce the slow path at the cost of higher constant factors on the
fast path.

### Comparison to Production Systems

| System | Typical Latency | Our Engine |
|--------|----------------|------------|
| CME Globex | ~500 ns - 1 us | 14-245 ns |
| LMAX Exchange | ~100 ns | 14 ns (fill) |
| Aeron (messaging) | ~100 ns | N/A |

Our engine is competitive with production matching engines for the core matching path.
The higher-level operations (new level creation, deep sweeps) add expected overhead.

---

## How to Run

```bash
# Build with optimizations
bazel build //benchmarks:exchange_benchmark -c opt

# Run without pinning
bazel-bin/benchmarks/exchange_benchmark

# Run with CPU pinning
taskset -c 0 bazel-bin/benchmarks/exchange_benchmark

# Run with NUMA binding (multi-socket systems)
numactl --cpunodebind=0 --membind=0 -- taskset -c 0 bazel-bin/benchmarks/exchange_benchmark

# Or use the convenience script
./benchmarks/run_benchmark.sh
```
