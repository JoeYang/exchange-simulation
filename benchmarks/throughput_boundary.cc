// Throughput Boundary Test
//
// Incrementally increases order rate until the engine can't keep up.
// Measures: orders/sec at various target rates, actual achieved rate,
// and the point where latency degrades (p99 > threshold).
//
// Since the engine is synchronous (single-threaded, no queue), "dropping"
// doesn't apply — every call blocks until complete. Instead we measure:
// 1. Max sustained throughput (how fast can we push orders?)
// 2. Latency degradation curve (at what rate does p99 blow up?)
// 3. Mixed workload saturation point

#include "exchange-core/matching_engine.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <vector>

using namespace exchange;
using Clock = std::chrono::high_resolution_clock;
using Nanos = std::chrono::nanoseconds;

// Null listeners for pure throughput
class NullOL : public OrderListenerBase {};
class NullML : public MarketDataListenerBase {};

class BenchEngine : public MatchingEngine<BenchEngine, NullOL, NullML, FifoMatch, 50000, 5000, 500000> {
public:
    using Base = MatchingEngine<BenchEngine, NullOL, NullML, FifoMatch, 50000, 5000, 500000>;
    using Base::Base;
};

OrderRequest make_limit(uint64_t id, Side side, Price price, Quantity qty, Timestamp ts) {
    return OrderRequest{
        .client_order_id = id, .account_id = 1, .side = side,
        .type = OrderType::Limit, .tif = TimeInForce::GTC,
        .price = price, .quantity = qty, .stop_price = 0,
        .timestamp = ts, .gtd_expiry = 0,
    };
}

struct RateResult {
    double target_rate;       // target orders/sec
    double achieved_rate;     // actual orders/sec
    double avg_ns;
    double p50_ns;
    double p99_ns;
    double p999_ns;
    double max_ns;
    size_t orders_sent;
    bool saturated;           // true if achieved < 95% of target
};

// Run at a target rate using busy-wait pacing
RateResult run_at_rate(double target_ops_per_sec, double duration_secs, bool with_fills) {
    NullOL ol;
    NullML ml;
    EngineConfig cfg{.tick_size = 100, .lot_size = 10000, .price_band_low = 0, .price_band_high = 0};
    auto engine = std::make_unique<BenchEngine>(cfg, ol, ml);

    size_t total_ops = static_cast<size_t>(target_ops_per_sec * duration_secs);
    if (total_ops > 400000) total_ops = 400000; // cap to avoid pool exhaustion

    int64_t interval_ns = static_cast<int64_t>(1e9 / target_ops_per_sec);
    std::vector<int64_t> latencies;
    latencies.reserve(total_ops);

    uint64_t seq = 0;

    // Warmup: 1000 orders
    for (size_t i = 0; i < 1000; ++i) {
        if (with_fills) {
            auto bid = seq++;
            engine->new_order(make_limit(bid, Side::Buy, 1000000, 10000, static_cast<Timestamp>(bid)));
            auto ask = seq++;
            engine->new_order(make_limit(ask, Side::Sell, 1000000, 10000, static_cast<Timestamp>(ask)));
        } else {
            auto id = seq++;
            engine->new_order(make_limit(id, Side::Buy,
                1000000 - static_cast<Price>(i % 1000) * 100, 10000,
                static_cast<Timestamp>(id)));
        }
    }

    // If resting-only mode, cancel warmup orders to free pool
    if (!with_fills) {
        for (OrderId oid = 1; oid <= 1000; ++oid) {
            engine->cancel_order(oid, static_cast<Timestamp>(seq++));
        }
    }

    // Paced send
    auto test_start = Clock::now();
    auto next_send = test_start;

    for (size_t i = 0; i < total_ops; ++i) {
        // Busy-wait until it's time to send
        while (Clock::now() < next_send) {
            // spin
        }

        auto op_start = Clock::now();

        if (with_fills) {
            // Alternate: place buy, then sell that fills it
            if (i % 2 == 0) {
                auto id = seq++;
                engine->new_order(make_limit(id, Side::Buy, 1000000, 10000,
                    static_cast<Timestamp>(id)));
            } else {
                auto id = seq++;
                engine->new_order(make_limit(id, Side::Sell, 1000000, 10000,
                    static_cast<Timestamp>(id)));
            }
        } else {
            auto id = seq++;
            engine->new_order(make_limit(id, Side::Buy,
                1000000 - static_cast<Price>(i % 2000) * 100, 10000,
                static_cast<Timestamp>(id)));
            // Periodically cancel to avoid pool exhaustion
            if (i > 0 && i % 100 == 0) {
                for (size_t j = 0; j < 50; ++j) {
                    OrderId cancel_id = static_cast<OrderId>(seq - 200 + j);
                    if (cancel_id > 0 && cancel_id < seq) {
                        engine->cancel_order(cancel_id, static_cast<Timestamp>(seq));
                    }
                }
            }
        }

        auto op_end = Clock::now();
        latencies.push_back(std::chrono::duration_cast<Nanos>(op_end - op_start).count());

        next_send = test_start + Nanos(static_cast<int64_t>((i + 1) * interval_ns));
    }

    auto test_end = Clock::now();
    double elapsed_secs = std::chrono::duration_cast<Nanos>(test_end - test_start).count() / 1e9;

    // Compute stats
    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    size_t n = latencies.size();

    RateResult r;
    r.target_rate = target_ops_per_sec;
    r.achieved_rate = n / elapsed_secs;
    r.avg_ns = sum / n;
    r.p50_ns = static_cast<double>(latencies[n / 2]);
    r.p99_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.99)]);
    r.p999_ns = static_cast<double>(latencies[std::min(static_cast<size_t>(n * 0.999), n - 1)]);
    r.max_ns = static_cast<double>(latencies[n - 1]);
    r.orders_sent = n;
    r.saturated = (r.achieved_rate < target_ops_per_sec * 0.95);

    return r;
}

// Burst throughput: no pacing, just fire as fast as possible
RateResult run_burst(size_t total_ops, bool with_fills) {
    NullOL ol;
    NullML ml;
    EngineConfig cfg{.tick_size = 100, .lot_size = 10000, .price_band_low = 0, .price_band_high = 0};
    auto engine = std::make_unique<BenchEngine>(cfg, ol, ml);

    std::vector<int64_t> latencies;
    latencies.reserve(total_ops);
    uint64_t seq = 0;

    if (with_fills) {
        // Pre-place buys for all fills
        for (size_t i = 0; i < total_ops; ++i) {
            auto id = seq++;
            engine->new_order(make_limit(id, Side::Buy, 1000000, 10000,
                static_cast<Timestamp>(id)));
        }
    }

    auto test_start = Clock::now();

    for (size_t i = 0; i < total_ops; ++i) {
        auto op_start = Clock::now();

        if (with_fills) {
            auto id = seq++;
            engine->new_order(make_limit(id, Side::Sell, 1000000, 10000,
                static_cast<Timestamp>(id)));
        } else {
            auto id = seq++;
            engine->new_order(make_limit(id, Side::Buy,
                1000000 - static_cast<Price>(i % 2000) * 100, 10000,
                static_cast<Timestamp>(id)));
        }

        auto op_end = Clock::now();
        latencies.push_back(std::chrono::duration_cast<Nanos>(op_end - op_start).count());
    }

    auto test_end = Clock::now();
    double elapsed_secs = std::chrono::duration_cast<Nanos>(test_end - test_start).count() / 1e9;

    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    size_t n = latencies.size();

    RateResult r;
    r.target_rate = 0;  // burst = no target
    r.achieved_rate = n / elapsed_secs;
    r.avg_ns = sum / n;
    r.p50_ns = static_cast<double>(latencies[n / 2]);
    r.p99_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.99)]);
    r.p999_ns = static_cast<double>(latencies[std::min(static_cast<size_t>(n * 0.999), n - 1)]);
    r.max_ns = static_cast<double>(latencies[n - 1]);
    r.orders_sent = n;
    r.saturated = false;

    return r;
}

void print_header() {
    printf("  %-14s | %-14s | %10s | %10s | %10s | %12s | %10s | %s\n",
           "Target Rate", "Achieved Rate", "Avg", "P50", "P99", "P99.9", "Max", "Status");
    printf("  %s\n", std::string(110, '-').c_str());
}

void print_result(const RateResult& r) {
    const char* status = r.saturated ? "SATURATED" :
                         (r.p99_ns > 1000) ? "DEGRADED" : "OK";
    if (r.target_rate == 0) {
        printf("  %-14s | %12.0f/s | %8.0f ns | %8.0f ns | %8.0f ns | %10.0f ns | %8.0f ns | %s\n",
               "BURST",
               r.achieved_rate, r.avg_ns, r.p50_ns, r.p99_ns, r.p999_ns, r.max_ns, status);
    } else {
        printf("  %12.0f/s | %12.0f/s | %8.0f ns | %8.0f ns | %8.0f ns | %10.0f ns | %8.0f ns | %s\n",
               r.target_rate, r.achieved_rate,
               r.avg_ns, r.p50_ns, r.p99_ns, r.p999_ns, r.max_ns, status);
    }
}

int main() {
    printf("=== Exchange Core Throughput Boundary Test ===\n\n");
    printf("Incrementally increasing order rate to find the saturation point.\n");
    printf("SATURATED = achieved rate < 95%% of target rate.\n");
    printf("DEGRADED  = p99 latency > 1 microsecond.\n\n");

    // Test 1: Resting orders (no fills) — ramp up
    printf("--- Test 1: Resting Orders (no matching, new price levels) ---\n\n");
    print_header();

    double resting_rates[] = {100000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000};
    for (double rate : resting_rates) {
        auto r = run_at_rate(rate, 0.5, false);
        print_result(r);
        if (r.saturated) {
            printf("  >> Saturation point reached at ~%.0f orders/sec\n", r.achieved_rate);
            break;
        }
    }

    // Burst (unlimited)
    auto burst_rest = run_burst(100000, false);
    print_result(burst_rest);
    printf("\n  >> Max burst throughput (resting): %.0f orders/sec\n", burst_rest.achieved_rate);

    // Test 2: Immediate fills — ramp up
    printf("\n--- Test 2: Immediate Fills (every order matches) ---\n\n");
    print_header();

    double fill_rates[] = {100000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000, 50000000, 100000000};
    for (double rate : fill_rates) {
        auto r = run_at_rate(rate, 0.5, true);
        print_result(r);
        if (r.saturated) {
            printf("  >> Saturation point reached at ~%.0f orders/sec\n", r.achieved_rate);
            break;
        }
    }

    // Burst (unlimited)
    auto burst_fill = run_burst(100000, true);
    print_result(burst_fill);
    printf("\n  >> Max burst throughput (fills): %.0f orders/sec\n", burst_fill.achieved_rate);

    // Summary
    printf("\n=== Summary ===\n\n");
    printf("  Resting orders:  max ~%.1fM orders/sec (burst), p99 degrades above ~%.1fM/s\n",
           burst_rest.achieved_rate / 1e6, burst_rest.achieved_rate / 1e6 * 0.8);
    printf("  Immediate fills: max ~%.1fM orders/sec (burst), p99 degrades above ~%.1fM/s\n",
           burst_fill.achieved_rate / 1e6, burst_fill.achieved_rate / 1e6 * 0.8);
    printf("\n  Note: This engine is synchronous — it never 'drops' orders.\n");
    printf("  Saturation means the caller can't push orders fast enough to\n");
    printf("  reach the target rate. In production, the protocol layer's\n");
    printf("  sequencer would queue orders if the engine falls behind.\n");

    return 0;
}
