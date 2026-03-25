#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <vector>
#include <memory>

using namespace exchange;
using Clock = std::chrono::high_resolution_clock;
using Nanos = std::chrono::nanoseconds;

// ---------------------------------------------------------------------------
// Null listeners -- discard all callbacks for pure engine throughput
// ---------------------------------------------------------------------------

class NullOrderListener : public OrderListenerBase {};
class NullMdListener : public MarketDataListenerBase {};

// ---------------------------------------------------------------------------
// Engine types -- one with null listeners, one with recording listeners
// ---------------------------------------------------------------------------

// Note: these are heap-allocated via unique_ptr in each benchmark
// to avoid stack overflow from large pool arrays
class NullExchange
    : public MatchingEngine<NullExchange, NullOrderListener, NullMdListener,
                            FifoMatch, 50000, 5000, 200000> {
public:
    using Base = MatchingEngine<NullExchange, NullOrderListener, NullMdListener,
                                FifoMatch, 50000, 5000, 200000>;
    using Base::Base;
};

class RecordExchange
    : public MatchingEngine<RecordExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 50000, 5000,
                            200000> {
public:
    using Base =
        MatchingEngine<RecordExchange, RecordingOrderListener,
                       RecordingMdListener, FifoMatch, 50000, 5000, 200000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// BenchResult -- holds statistics for a single benchmark run
// ---------------------------------------------------------------------------

struct BenchResult {
    std::string name;
    size_t iterations;
    double avg_ns;
    double p50_ns;
    double p99_ns;
    double p999_ns;
    double min_ns;
    double max_ns;
    double ops_per_sec;
};

// ---------------------------------------------------------------------------
// run_benchmark -- measure per-operation latencies with warmup
// ---------------------------------------------------------------------------

template <typename SetupFn, typename Fn>
BenchResult run_benchmark(const std::string& name, size_t iterations,
                          size_t warmup, SetupFn&& setup, Fn&& fn) {
    setup();

    // Warmup
    for (size_t i = 0; i < warmup; ++i) {
        fn(i);
    }

    // Measure each operation individually
    std::vector<int64_t> latencies(iterations);
    for (size_t i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        fn(warmup + i);
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    // Compute statistics
    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = name;
    result.iterations = iterations;
    result.avg_ns = sum / static_cast<double>(iterations);
    result.p50_ns =
        static_cast<double>(latencies[iterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(iterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(iterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[iterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;

    return result;
}

// Simpler overload: no separate setup phase
template <typename Fn>
BenchResult run_benchmark(const std::string& name, size_t iterations,
                          size_t warmup, Fn&& fn) {
    return run_benchmark(name, iterations, warmup, [] {}, std::forward<Fn>(fn));
}

// ---------------------------------------------------------------------------
// print helpers
// ---------------------------------------------------------------------------

void print_header() {
    std::printf(
        "%-38s | %10s | %10s | %10s | %12s | %10s | %12s | %14s\n",
        "Scenario", "Avg", "P50", "P99", "P99.9", "Min", "Max", "Ops/sec");
    std::printf("%s\n", std::string(142, '-').c_str());
}

void print_result(const BenchResult& r) {
    std::printf(
        "%-38s | %8.0f ns | %8.0f ns | %8.0f ns | %10.0f ns | %8.0f ns | "
        "%10.0f ns | %12.0f\n",
        r.name.c_str(), r.avg_ns, r.p50_ns, r.p99_ns, r.p999_ns, r.min_ns,
        r.max_ns, r.ops_per_sec);
}

// ---------------------------------------------------------------------------
// Helper: make a GTC limit order request
// ---------------------------------------------------------------------------

OrderRequest make_limit(uint64_t client_order_id, Side side, Price price,
                        Quantity qty, Timestamp ts) {
    return OrderRequest{
        .client_order_id = client_order_id,
        .account_id = 1,
        .side = side,
        .type = OrderType::Limit,
        .tif = TimeInForce::GTC,
        .price = price,
        .quantity = qty,
        .stop_price = 0,
        .timestamp = ts,
        .gtd_expiry = 0,
    };
}

// ---------------------------------------------------------------------------
// Benchmark 1: Resting orders (no match)
// Place limit buy orders at spread-out prices so nothing matches.
// ---------------------------------------------------------------------------

BenchResult bench_resting_orders() {
    static constexpr size_t kIterations = 100000;
    static constexpr size_t kWarmup = 1000;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    return run_benchmark(
        "new_order (resting, no match)", kIterations, kWarmup,
        [&](size_t i) {
            engine.new_order(make_limit(
                i, Side::Buy,
                1000000 - static_cast<Price>(i % 1000) * 100, 10000,
                static_cast<Timestamp>(i)));
        });
}

// ---------------------------------------------------------------------------
// Benchmark 2: Immediate fill
// Alternating buy/sell at the same price. Every other order matches.
// We measure the sell (matching) operations only.
// ---------------------------------------------------------------------------

BenchResult bench_immediate_fill() {
    static constexpr size_t kIterations = 50000;
    static constexpr size_t kWarmup = 500;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    // Pre-place buy orders for warmup iterations
    for (size_t i = 0; i < kWarmup; ++i) {
        engine.new_order(make_limit(i * 2, Side::Buy, 1000000, 10000,
                                    static_cast<Timestamp>(i * 2)));
    }

    size_t seq = kWarmup * 2;

    // Warmup: alternate buy then sell
    for (size_t i = 0; i < kWarmup; ++i) {
        // Place buy
        auto _seq_0 = seq++;
        engine.new_order(make_limit(_seq_0, Side::Buy, 1000000, 10000,
                                    static_cast<Timestamp>(seq)));
        // Sell matches it (this is what warmup exercises)
        auto _seq_1 = seq++;
        engine.new_order(make_limit(_seq_1, Side::Sell, 1000000, 10000,
                                    static_cast<Timestamp>(seq)));
    }

    // Now pre-place all the buy orders we will measure sells against
    for (size_t i = 0; i < kIterations; ++i) {
        auto _seq_2 = seq++;
        engine.new_order(make_limit(_seq_2, Side::Buy, 1000000, 10000,
                                    static_cast<Timestamp>(seq)));
    }

    // Measure: each iteration sends a sell that matches the waiting buy
    std::vector<int64_t> latencies(kIterations);
    for (size_t i = 0; i < kIterations; ++i) {
        auto start = Clock::now();
        auto _seq_3 = seq++;
        engine.new_order(make_limit(_seq_3, Side::Sell, 1000000, 10000,
                                    static_cast<Timestamp>(seq)));
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "new_order (immediate fill)";
    result.iterations = kIterations;
    result.avg_ns = sum / static_cast<double>(kIterations);
    result.p50_ns = static_cast<double>(latencies[kIterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kIterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kIterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kIterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// Benchmark 3: Cancel order
// Fill the book with resting orders, then cancel them all.
// ---------------------------------------------------------------------------

BenchResult bench_cancel_order() {
    static constexpr size_t kIterations = 100000;
    static constexpr size_t kWarmup = 1000;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    // Pre-fill: place orders that will be cancelled.
    // Order IDs are assigned sequentially starting at 1.
    // We need warmup + iterations orders.
    size_t total_orders = kWarmup + kIterations;
    for (size_t i = 0; i < total_orders; ++i) {
        engine.new_order(make_limit(
            i, Side::Buy,
            1000000 - static_cast<Price>(i % 1000) * 100, 10000,
            static_cast<Timestamp>(i)));
    }

    // Warmup: cancel the first kWarmup orders (IDs 1..kWarmup)
    for (size_t i = 0; i < kWarmup; ++i) {
        engine.cancel_order(static_cast<OrderId>(i + 1),
                            static_cast<Timestamp>(total_orders + i));
    }

    // Measure: cancel the remaining orders
    std::vector<int64_t> latencies(kIterations);
    for (size_t i = 0; i < kIterations; ++i) {
        OrderId oid = static_cast<OrderId>(kWarmup + i + 1);
        auto start = Clock::now();
        engine.cancel_order(oid,
                            static_cast<Timestamp>(total_orders + kWarmup + i));
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "cancel_order";
    result.iterations = kIterations;
    result.avg_ns = sum / static_cast<double>(kIterations);
    result.p50_ns = static_cast<double>(latencies[kIterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kIterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kIterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kIterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// Benchmark 4: Modify order (cancel-replace)
// Resting orders, modify each to a new price.
// ---------------------------------------------------------------------------

BenchResult bench_modify_order() {
    static constexpr size_t kIterations = 100000;
    static constexpr size_t kWarmup = 1000;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    // Pre-fill: place resting buy orders.
    size_t total_orders = kWarmup + kIterations;
    for (size_t i = 0; i < total_orders; ++i) {
        engine.new_order(make_limit(
            i, Side::Buy,
            500000 - static_cast<Price>(i % 500) * 100, 10000,
            static_cast<Timestamp>(i)));
    }

    // Warmup: modify first kWarmup orders
    for (size_t i = 0; i < kWarmup; ++i) {
        OrderId oid = static_cast<OrderId>(i + 1);
        engine.modify_order(ModifyRequest{
            .order_id = oid,
            .client_order_id = i + total_orders,
            .new_price = 400000 - static_cast<Price>(i % 400) * 100,
            .new_quantity = 10000,
            .timestamp = static_cast<Timestamp>(total_orders + i),
        });
    }

    // Measure: modify remaining orders to a new price
    std::vector<int64_t> latencies(kIterations);
    for (size_t i = 0; i < kIterations; ++i) {
        OrderId oid = static_cast<OrderId>(kWarmup + i + 1);
        auto start = Clock::now();
        engine.modify_order(ModifyRequest{
            .order_id = oid,
            .client_order_id = i + total_orders + kWarmup,
            .new_price = 400000 - static_cast<Price>(i % 400) * 100,
            .new_quantity = 10000,
            .timestamp =
                static_cast<Timestamp>(total_orders + kWarmup + i),
        });
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "modify_order (cancel-replace)";
    result.iterations = kIterations;
    result.avg_ns = sum / static_cast<double>(kIterations);
    result.p50_ns = static_cast<double>(latencies[kIterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kIterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kIterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kIterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// Benchmark 5: Mixed workload
// 40% new resting, 30% new matching, 20% cancel, 10% modify
// ---------------------------------------------------------------------------

BenchResult bench_mixed_workload() {
    static constexpr size_t kIterations = 100000;
    static constexpr size_t kWarmup = 2000;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    // Pre-seed the book with resting orders on both sides for cancels/modifies
    // to operate on. Place buys below 1000000, sells above 1000000.
    OrderId next_cancel_id = 0;
    OrderId next_modify_id = 0;
    std::vector<OrderId> cancel_pool;
    std::vector<OrderId> modify_pool;

    // Seed: place 20000 buy orders and 20000 sell orders
    uint64_t seq = 0;
    for (size_t i = 0; i < 20000; ++i) {
        auto id = seq++;
        engine.new_order(make_limit(
            id, Side::Buy,
            900000 - static_cast<Price>(i % 500) * 100, 10000,
            static_cast<Timestamp>(id)));
    }
    for (size_t i = 0; i < 20000; ++i) {
        auto id = seq++;
        engine.new_order(make_limit(
            id, Side::Sell,
            1100000 + static_cast<Price>(i % 500) * 100, 10000,
            static_cast<Timestamp>(id)));
    }

    // Build pools for cancel/modify. Order IDs are 1..40000 from seeding.
    // First 20k are buys, next 20k are sells.
    for (OrderId id = 1; id <= 20000; ++id) {
        cancel_pool.push_back(id);
    }
    for (OrderId id = 20001; id <= 40000; ++id) {
        modify_pool.push_back(id);
    }

    // We need a deterministic sequence for reproducibility.
    // Use a simple LCG to pick operations.
    uint64_t rng_state = 12345;
    auto next_rng = [&]() -> uint32_t {
        rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(rng_state >> 33);
    };

    // Lambda that performs one mixed operation
    auto do_op = [&](size_t i) {
        uint32_t r = next_rng() % 100;
        Timestamp ts = static_cast<Timestamp>(50000 + i);

        if (r < 40) {
            auto id = seq++;
            engine.new_order(make_limit(
                id, Side::Buy,
                800000 - static_cast<Price>(i % 500) * 100, 10000, ts));
        } else if (r < 70) {
            Price match_price = 1500000 + static_cast<Price>(i % 100) * 100;
            auto id1 = seq++;
            engine.new_order(make_limit(id1, Side::Buy, match_price, 10000, ts));
            auto id2 = seq++;
            engine.new_order(make_limit(id2, Side::Sell, match_price, 10000, ts));
        } else if (r < 90) {
            if (next_cancel_id < cancel_pool.size()) {
                engine.cancel_order(cancel_pool[next_cancel_id++], ts);
            } else {
                auto id = seq++;
                engine.new_order(make_limit(
                    id, Side::Buy,
                    700000 - static_cast<Price>(i % 300) * 100, 10000, ts));
            }
        } else {
            if (next_modify_id < modify_pool.size()) {
                OrderId oid = modify_pool[next_modify_id++];
                auto cid = seq++;
                engine.modify_order(ModifyRequest{
                    .order_id = oid,
                    .client_order_id = cid,
                    .new_price = 1200000 + static_cast<Price>(i % 400) * 100,
                    .new_quantity = 10000,
                    .timestamp = ts,
                });
            } else {
                auto id = seq++;
                engine.new_order(make_limit(
                    id, Side::Sell,
                    1300000 + static_cast<Price>(i % 300) * 100, 10000, ts));
            }
        }
    };

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        do_op(i);
    }

    // Measure
    std::vector<int64_t> latencies(kIterations);
    for (size_t i = 0; i < kIterations; ++i) {
        auto start = Clock::now();
        do_op(kWarmup + i);
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "mixed workload (40/30/20/10)";
    result.iterations = kIterations;
    result.avg_ns = sum / static_cast<double>(kIterations);
    result.p50_ns = static_cast<double>(latencies[kIterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kIterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kIterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kIterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// Benchmark 6: Deep book stress
// Place orders across many price levels, then sweep with aggressive orders.
// ---------------------------------------------------------------------------

BenchResult bench_deep_book_sweep() {
    static constexpr size_t kLevels = 500;
    static constexpr size_t kOrdersPerLevel = 5;
    static constexpr size_t kSweeps = 1000;
    static constexpr size_t kWarmupSweeps = 100;

    NullOrderListener ol;
    NullMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<NullExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    uint64_t seq = 0;

    // Helper to rebuild the book for a sweep round
    auto fill_book = [&]() {
        for (size_t lvl = 0; lvl < kLevels; ++lvl) {
            Price price = 1000000 + static_cast<Price>(lvl) * 100;
            for (size_t j = 0; j < kOrdersPerLevel; ++j) {
                auto id = seq++;
                engine.new_order(make_limit(
                    id, Side::Sell, price, 10000,
                    static_cast<Timestamp>(id)));
            }
        }
    };

    // Initial fill
    fill_book();

    // Warmup sweeps: aggressive buy that sweeps through many levels
    for (size_t i = 0; i < kWarmupSweeps; ++i) {
        // Sweep 50 levels worth of quantity
        auto sid = seq++;
        engine.new_order(make_limit(
            sid, Side::Buy,
            1000000 + static_cast<Price>(49) * 100,
            10000 * static_cast<Quantity>(kOrdersPerLevel) * 50,
            static_cast<Timestamp>(sid)));
        // Refill swept levels
        for (size_t lvl = 0; lvl < 50; ++lvl) {
            Price price = 1000000 + static_cast<Price>(lvl) * 100;
            for (size_t j = 0; j < kOrdersPerLevel; ++j) {
                auto rid = seq++;
                engine.new_order(make_limit(
                    rid, Side::Sell, price, 10000,
                    static_cast<Timestamp>(rid)));
            }
        }
    }

    // Measure: each sweep clears some levels, then we refill
    std::vector<int64_t> latencies(kSweeps);
    for (size_t i = 0; i < kSweeps; ++i) {
        // Sweep 50 levels
        Quantity sweep_qty =
            10000 * static_cast<Quantity>(kOrdersPerLevel) * 50;
        Price sweep_price = 1000000 + static_cast<Price>(49) * 100;

        auto start = Clock::now();
        auto _seq_7 = seq++;
        engine.new_order(make_limit(_seq_7, Side::Buy, sweep_price,
                                    sweep_qty,
                                    static_cast<Timestamp>(seq)));
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();

        // Refill for next sweep
        for (size_t lvl = 0; lvl < 50; ++lvl) {
            Price price = 1000000 + static_cast<Price>(lvl) * 100;
            for (size_t j = 0; j < kOrdersPerLevel; ++j) {
                auto fid = seq++;
                engine.new_order(make_limit(
                    fid, Side::Sell, price, 10000,
                    static_cast<Timestamp>(fid)));
            }
        }
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "deep book sweep (50 levels)";
    result.iterations = kSweeps;
    result.avg_ns = sum / static_cast<double>(kSweeps);
    result.p50_ns = static_cast<double>(latencies[kSweeps / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kSweeps * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kSweeps * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kSweeps - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// Benchmark 7: Recording listener overhead comparison
// Same as bench_resting_orders but with RecordingListeners.
// ---------------------------------------------------------------------------

BenchResult bench_resting_with_recording_listener() {
    static constexpr size_t kIterations = 100000;
    static constexpr size_t kWarmup = 1000;

    RecordingOrderListener ol;
    RecordingMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<RecordExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    return run_benchmark(
        "new_order (resting, RecordingListener)", kIterations, kWarmup,
        [&](size_t i) {
            engine.new_order(make_limit(
                i, Side::Buy,
                1000000 - static_cast<Price>(i % 1000) * 100, 10000,
                static_cast<Timestamp>(i)));
        });
}

BenchResult bench_fill_with_recording_listener() {
    static constexpr size_t kIterations = 50000;

    RecordingOrderListener ol;
    RecordingMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0};
    auto engine_ptr = std::make_unique<RecordExchange>(cfg, ol, ml); auto& engine = *engine_ptr;

    uint64_t seq = 0;

    // Pre-place buy orders for all iterations
    for (size_t i = 0; i < kIterations; ++i) {
        auto _seq_8 = seq++;
        engine.new_order(make_limit(_seq_8, Side::Buy, 1000000, 10000,
                       static_cast<Timestamp>(seq)));
    }

    ol.clear();
    ml.clear();

    // Measure: each sell matches a waiting buy
    std::vector<int64_t> latencies(kIterations);
    for (size_t i = 0; i < kIterations; ++i) {
        auto start = Clock::now();
        auto _seq_9 = seq++;
        engine.new_order(make_limit(_seq_9, Side::Sell, 1000000, 10000,
                       static_cast<Timestamp>(seq)));
        auto end = Clock::now();
        latencies[i] =
            std::chrono::duration_cast<Nanos>(end - start).count();
    }

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    BenchResult result;
    result.name = "new_order (fill, RecordingListener)";
    result.iterations = kIterations;
    result.avg_ns = sum / static_cast<double>(kIterations);
    result.p50_ns = static_cast<double>(latencies[kIterations / 2]);
    result.p99_ns =
        static_cast<double>(latencies[static_cast<size_t>(kIterations * 0.99)]);
    result.p999_ns = static_cast<double>(
        latencies[static_cast<size_t>(kIterations * 0.999)]);
    result.min_ns = static_cast<double>(latencies[0]);
    result.max_ns = static_cast<double>(latencies[kIterations - 1]);
    result.ops_per_sec = 1e9 / result.avg_ns;
    return result;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::printf("Exchange Core Performance Benchmark\n");
    std::printf("===================================\n\n");

    // --- Null listener benchmarks ---
    std::printf("--- Null Listeners (pure engine throughput) ---\n\n");
    print_header();

    auto r1 = bench_resting_orders();
    print_result(r1);

    auto r2 = bench_immediate_fill();
    print_result(r2);

    auto r3 = bench_cancel_order();
    print_result(r3);

    auto r4 = bench_modify_order();
    print_result(r4);

    auto r5 = bench_mixed_workload();
    print_result(r5);

    auto r6 = bench_deep_book_sweep();
    print_result(r6);

    // --- Recording listener benchmarks ---
    std::printf("\n--- Recording Listeners (callback overhead) ---\n\n");
    print_header();

    auto r7 = bench_resting_with_recording_listener();
    print_result(r7);

    auto r8 = bench_fill_with_recording_listener();
    print_result(r8);

    // --- Overhead summary ---
    std::printf("\n--- Listener Overhead ---\n\n");
    std::printf("Resting: NullListener %.0f ns vs RecordingListener %.0f ns "
                "(+%.1f%%)\n",
                r1.avg_ns, r7.avg_ns,
                ((r7.avg_ns - r1.avg_ns) / r1.avg_ns) * 100.0);
    std::printf("Fill:    NullListener %.0f ns vs RecordingListener %.0f ns "
                "(+%.1f%%)\n",
                r2.avg_ns, r8.avg_ns,
                ((r8.avg_ns - r2.avg_ns) / r2.avg_ns) * 100.0);

    return 0;
}
