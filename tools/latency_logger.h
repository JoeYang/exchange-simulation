#pragma once

#include "exchange-core/events.h"
#include "exchange-core/listeners.h"
#include "exchange-core/matching_engine.h"
#include "exchange-core/types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// LatencyHistogram -- fixed-bucket histogram for nanosecond latencies.
//
// Buckets are power-of-two spaced for compact representation of the
// wide latency range seen in exchange systems (sub-us to ms+).
//
// Bucket boundaries (nanoseconds):
//   [0, 100), [100, 500), [500, 1us), [1us, 5us), [5us, 10us),
//   [10us, 50us), [50us, 100us), [100us, 500us), [500us, 1ms),
//   [1ms, 5ms), [5ms, 10ms), [10ms, 50ms), [50ms+)
class LatencyHistogram {
public:
    static constexpr size_t kNumBuckets = 13;

    void record(int64_t latency_ns) {
        size_t idx = bucket_index(latency_ns);
        ++buckets_[idx];
        ++count_;
        sum_ += latency_ns;
        if (latency_ns < min_) min_ = latency_ns;
        if (latency_ns > max_) max_ = latency_ns;
        samples_.push_back(latency_ns);
    }

    uint64_t count() const { return count_; }
    int64_t  min()   const { return count_ > 0 ? min_ : 0; }
    int64_t  max()   const { return max_; }
    int64_t  sum()   const { return sum_; }

    double mean() const {
        return count_ > 0 ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

    // Percentile from sorted samples. p in [0.0, 1.0].
    int64_t percentile(double p) const {
        if (samples_.empty()) return 0;
        // Lazy sort on first percentile query.
        if (!sorted_) {
            auto& s = const_cast<std::vector<int64_t>&>(samples_);
            std::sort(s.begin(), s.end());
            sorted_ = true;
        }
        size_t idx = static_cast<size_t>(p * static_cast<double>(samples_.size() - 1));
        return samples_[idx];
    }

    uint64_t bucket_count(size_t idx) const {
        return idx < kNumBuckets ? buckets_[idx] : 0;
    }

    static const char* bucket_label(size_t idx) {
        static const char* labels[kNumBuckets] = {
            "    0-100ns", "  100-500ns", " 500ns-1us",
            "    1-5us",   "   5-10us",  "  10-50us",
            " 50-100us",   "100-500us",  "500us-1ms",
            "    1-5ms",   "   5-10ms",  "  10-50ms",
            "     50ms+",
        };
        return idx < kNumBuckets ? labels[idx] : "???";
    }

    void reset() {
        buckets_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = INT64_MAX;
        max_ = 0;
        samples_.clear();
        sorted_ = false;
    }

    // Print a formatted histogram report to the given stream.
    void print(std::ostream& os, const std::string& title) const {
        os << "=== " << title << " ===\n";
        if (count_ == 0) {
            os << "  (no samples)\n\n";
            return;
        }

        os << "  count=" << count_
           << "  min=" << min_ << "ns"
           << "  max=" << max_ << "ns"
           << "  mean=" << static_cast<int64_t>(mean()) << "ns"
           << "  p50=" << percentile(0.50) << "ns"
           << "  p99=" << percentile(0.99) << "ns"
           << "  p99.9=" << percentile(0.999) << "ns"
           << "\n";

        // Find max bucket count for bar scaling.
        uint64_t max_bucket = 0;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            if (buckets_[i] > max_bucket) max_bucket = buckets_[i];
        }

        static constexpr size_t kBarWidth = 40;
        for (size_t i = 0; i < kNumBuckets; ++i) {
            if (buckets_[i] == 0) continue;
            size_t bar_len = max_bucket > 0
                ? static_cast<size_t>(
                    static_cast<double>(buckets_[i]) /
                    static_cast<double>(max_bucket) *
                    static_cast<double>(kBarWidth))
                : 0;
            if (bar_len == 0 && buckets_[i] > 0) bar_len = 1;

            os << "  " << bucket_label(i) << " |";
            for (size_t j = 0; j < bar_len; ++j) os << '#';
            os << " " << buckets_[i] << "\n";
        }
        os << "\n";
    }

private:
    static size_t bucket_index(int64_t ns) {
        if (ns < 100)       return 0;   //     0 - 100ns
        if (ns < 500)       return 1;   //   100 - 500ns
        if (ns < 1000)      return 2;   //   500ns - 1us
        if (ns < 5000)      return 3;   //     1 - 5us
        if (ns < 10000)     return 4;   //     5 - 10us
        if (ns < 50000)     return 5;   //    10 - 50us
        if (ns < 100000)    return 6;   //    50 - 100us
        if (ns < 500000)    return 7;   //   100 - 500us
        if (ns < 1000000)   return 8;   //   500us - 1ms
        if (ns < 5000000)   return 9;   //     1 - 5ms
        if (ns < 10000000)  return 10;  //     5 - 10ms
        if (ns < 50000000)  return 11;  //    10 - 50ms
        return 12;                       //    50ms+
    }

    std::array<uint64_t, kNumBuckets> buckets_{};
    uint64_t count_{0};
    int64_t  sum_{0};
    int64_t  min_{INT64_MAX};
    int64_t  max_{0};
    std::vector<int64_t> samples_;
    mutable bool sorted_{false};
};

// LatencyLogger -- per-order latency tracker.
//
// Tracks entry-to-ack and entry-to-fill latencies by recording the
// submission timestamp for each client_order_id, then computing the
// delta when the corresponding accept/fill callback arrives.
//
// Usage:
//   LatencyLogger logger;
//   logger.on_order_submitted(req);  // call before engine.new_order(req)
//   // ... engine calls back on_order_accepted, on_order_filled, etc.
//   logger.report(std::cout);
//
// Inherits from OrderListenerBase so it can be composed via
// CompositeOrderListener alongside other listeners.
class LatencyLogger : public OrderListenerBase {
public:
    // Call this when an order is submitted to the engine.
    // Records the entry timestamp for latency measurement.
    void on_order_submitted(const OrderRequest& req) {
        pending_by_cl_ord_id_[req.client_order_id] = req.timestamp;
    }

    // --- OrderListenerBase callbacks ---

    void on_order_accepted(const OrderAccepted& e) {
        auto it = pending_by_cl_ord_id_.find(e.client_order_id);
        if (it != pending_by_cl_ord_id_.end()) {
            int64_t latency = e.ts - it->second;
            ack_histogram_.record(latency);
            // Map engine-assigned order ID to entry timestamp for fill tracking.
            pending_by_ord_id_[e.id] = it->second;
            pending_by_cl_ord_id_.erase(it);
        }
    }

    void on_order_rejected(const OrderRejected& e) {
        auto it = pending_by_cl_ord_id_.find(e.client_order_id);
        if (it != pending_by_cl_ord_id_.end()) {
            int64_t latency = e.ts - it->second;
            reject_histogram_.record(latency);
            pending_by_cl_ord_id_.erase(it);
        }
    }

    void on_order_filled(const OrderFilled& e) {
        record_fill(e.resting_id, e.ts);
        record_fill(e.aggressor_id, e.ts);
    }

    void on_order_partially_filled(const OrderPartiallyFilled& e) {
        record_fill(e.resting_id, e.ts);
        record_fill(e.aggressor_id, e.ts);
    }

    void on_order_cancelled(const OrderCancelled& e) {
        // Clean up tracking state.
        pending_by_ord_id_.erase(e.id);
    }

    // --- Reporting ---

    const LatencyHistogram& ack_histogram()    const { return ack_histogram_; }
    const LatencyHistogram& fill_histogram()   const { return fill_histogram_; }
    const LatencyHistogram& reject_histogram() const { return reject_histogram_; }

    void report(std::ostream& os) const {
        ack_histogram_.print(os, "Entry-to-Ack Latency");
        fill_histogram_.print(os, "Entry-to-Fill Latency");
        reject_histogram_.print(os, "Entry-to-Reject Latency");
    }

    void reset() {
        ack_histogram_.reset();
        fill_histogram_.reset();
        reject_histogram_.reset();
        pending_by_cl_ord_id_.clear();
        pending_by_ord_id_.clear();
    }

private:
    void record_fill(OrderId id, Timestamp fill_ts) {
        auto it = pending_by_ord_id_.find(id);
        if (it != pending_by_ord_id_.end()) {
            int64_t latency = fill_ts - it->second;
            fill_histogram_.record(latency);
            // Don't erase — the order may get multiple partial fills.
        }
    }

    LatencyHistogram ack_histogram_;
    LatencyHistogram fill_histogram_;
    LatencyHistogram reject_histogram_;

    // Pending orders: client_order_id → entry timestamp (before ack).
    std::unordered_map<uint64_t, Timestamp> pending_by_cl_ord_id_;
    // Accepted orders: engine order_id → entry timestamp (for fill tracking).
    std::unordered_map<OrderId, Timestamp> pending_by_ord_id_;
};

}  // namespace exchange
