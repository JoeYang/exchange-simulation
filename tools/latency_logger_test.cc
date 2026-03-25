#include "tools/latency_logger.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace exchange {

// ---------------------------------------------------------------------------
// LatencyHistogram tests
// ---------------------------------------------------------------------------

TEST(LatencyHistogramTest, EmptyHistogram) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 0);
    EXPECT_DOUBLE_EQ(h.mean(), 0.0);
    EXPECT_EQ(h.percentile(0.5), 0);
}

TEST(LatencyHistogramTest, SingleSample) {
    LatencyHistogram h;
    h.record(500);  // 500ns -> bucket [500, 1000) = bucket 2
    EXPECT_EQ(h.count(), 1u);
    EXPECT_EQ(h.min(), 500);
    EXPECT_EQ(h.max(), 500);
    EXPECT_DOUBLE_EQ(h.mean(), 500.0);
    EXPECT_EQ(h.percentile(0.5), 500);
    EXPECT_EQ(h.bucket_count(2), 1u);  // 500ns-1us bucket
}

TEST(LatencyHistogramTest, BucketBoundaries) {
    LatencyHistogram h;

    // Each sample lands in a distinct bucket.
    h.record(50);        // bucket 0: 0-100ns
    h.record(200);       // bucket 1: 100-500ns
    h.record(750);       // bucket 2: 500ns-1us
    h.record(3000);      // bucket 3: 1-5us
    h.record(7000);      // bucket 4: 5-10us
    h.record(25000);     // bucket 5: 10-50us
    h.record(75000);     // bucket 6: 50-100us
    h.record(250000);    // bucket 7: 100-500us
    h.record(750000);    // bucket 8: 500us-1ms
    h.record(2500000);   // bucket 9: 1-5ms
    h.record(7500000);   // bucket 10: 5-10ms
    h.record(25000000);  // bucket 11: 10-50ms
    h.record(75000000);  // bucket 12: 50ms+

    EXPECT_EQ(h.count(), 13u);
    for (size_t i = 0; i < LatencyHistogram::kNumBuckets; ++i) {
        EXPECT_EQ(h.bucket_count(i), 1u) << "bucket " << i;
    }
    EXPECT_EQ(h.min(), 50);
    EXPECT_EQ(h.max(), 75000000);
}

TEST(LatencyHistogramTest, Percentiles) {
    LatencyHistogram h;
    // 100 samples: 0, 100, 200, ..., 9900
    for (int i = 0; i < 100; ++i) {
        h.record(i * 100);
    }
    EXPECT_EQ(h.count(), 100u);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 9900);

    // p50 should be near 4950
    int64_t p50 = h.percentile(0.50);
    EXPECT_GE(p50, 4800);
    EXPECT_LE(p50, 5100);

    // p99 should be near 9800
    int64_t p99 = h.percentile(0.99);
    EXPECT_GE(p99, 9700);
    EXPECT_LE(p99, 9900);
}

TEST(LatencyHistogramTest, Reset) {
    LatencyHistogram h;
    h.record(1000);
    h.record(2000);
    EXPECT_EQ(h.count(), 2u);

    h.reset();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 0);
}

TEST(LatencyHistogramTest, PrintOutput) {
    LatencyHistogram h;
    h.record(500);
    h.record(1500);

    std::ostringstream os;
    h.print(os, "Test Histogram");
    std::string output = os.str();

    EXPECT_NE(output.find("Test Histogram"), std::string::npos);
    EXPECT_NE(output.find("count=2"), std::string::npos);
    EXPECT_NE(output.find("min=500ns"), std::string::npos);
    EXPECT_NE(output.find("max=1500ns"), std::string::npos);
}

TEST(LatencyHistogramTest, PrintEmptyHistogram) {
    LatencyHistogram h;
    std::ostringstream os;
    h.print(os, "Empty");
    std::string output = os.str();
    EXPECT_NE(output.find("no samples"), std::string::npos);
}

// ---------------------------------------------------------------------------
// LatencyLogger tests
// ---------------------------------------------------------------------------

static OrderRequest make_request(uint64_t cl_ord_id, Timestamp ts) {
    OrderRequest req{};
    req.client_order_id = cl_ord_id;
    req.account_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::GTC;
    req.price = 1000000;
    req.quantity = 10000;
    req.stop_price = 0;
    req.timestamp = ts;
    req.gtd_expiry = 0;
    return req;
}

TEST(LatencyLoggerTest, AckLatency) {
    LatencyLogger logger;

    // Submit at ts=1000, ack at ts=1200 -> 200ns ack latency
    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1200;
    logger.on_order_accepted(ack);

    EXPECT_EQ(logger.ack_histogram().count(), 1u);
    EXPECT_EQ(logger.ack_histogram().min(), 200);
    EXPECT_EQ(logger.ack_histogram().max(), 200);
}

TEST(LatencyLoggerTest, FillLatency) {
    LatencyLogger logger;

    // Submit at ts=1000, ack at ts=1100, fill at ts=2000
    // Entry-to-fill = 2000 - 1000 = 1000ns
    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1100;
    logger.on_order_accepted(ack);

    OrderFilled fill{};
    fill.aggressor_id = 2;
    fill.resting_id = 1;
    fill.price = 1000000;
    fill.quantity = 10000;
    fill.ts = 2000;
    logger.on_order_filled(fill);

    EXPECT_EQ(logger.ack_histogram().count(), 1u);
    EXPECT_EQ(logger.fill_histogram().count(), 1u);
    EXPECT_EQ(logger.fill_histogram().min(), 1000);  // 2000 - 1000
}

TEST(LatencyLoggerTest, RejectLatency) {
    LatencyLogger logger;

    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderRejected rej{};
    rej.client_order_id = 1;
    rej.ts = 1050;
    rej.reason = RejectReason::InvalidPrice;
    logger.on_order_rejected(rej);

    EXPECT_EQ(logger.reject_histogram().count(), 1u);
    EXPECT_EQ(logger.reject_histogram().min(), 50);
    EXPECT_EQ(logger.ack_histogram().count(), 0u);
}

TEST(LatencyLoggerTest, PartialFillTracksMultipleFills) {
    LatencyLogger logger;

    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1100;
    logger.on_order_accepted(ack);

    // First partial fill at ts=2000
    OrderPartiallyFilled pf1{};
    pf1.aggressor_id = 2;
    pf1.resting_id = 1;
    pf1.price = 1000000;
    pf1.quantity = 5000;
    pf1.aggressor_remaining = 5000;
    pf1.resting_remaining = 5000;
    pf1.ts = 2000;
    logger.on_order_partially_filled(pf1);

    // Second partial fill at ts=3000
    OrderPartiallyFilled pf2{};
    pf2.aggressor_id = 3;
    pf2.resting_id = 1;
    pf2.price = 1000000;
    pf2.quantity = 5000;
    pf2.aggressor_remaining = 5000;
    pf2.resting_remaining = 0;
    pf2.ts = 3000;
    logger.on_order_partially_filled(pf2);

    // Both fills should be recorded (resting order 1 was filled twice).
    EXPECT_EQ(logger.fill_histogram().count(), 2u);
    EXPECT_EQ(logger.fill_histogram().min(), 1000);   // 2000 - 1000
    EXPECT_EQ(logger.fill_histogram().max(), 2000);   // 3000 - 1000
}

TEST(LatencyLoggerTest, AggressorFillLatency) {
    LatencyLogger logger;

    // Resting order: submitted at ts=1000, acked at 1100
    auto req1 = make_request(1, 1000);
    logger.on_order_submitted(req1);
    OrderAccepted ack1{};
    ack1.id = 1;
    ack1.client_order_id = 1;
    ack1.ts = 1100;
    logger.on_order_accepted(ack1);

    // Aggressor order: submitted at ts=5000, acked at 5050
    auto req2 = make_request(2, 5000);
    logger.on_order_submitted(req2);
    OrderAccepted ack2{};
    ack2.id = 2;
    ack2.client_order_id = 2;
    ack2.ts = 5050;
    logger.on_order_accepted(ack2);

    // Fill at ts=5100 — both resting and aggressor get fill latency recorded.
    OrderFilled fill{};
    fill.aggressor_id = 2;
    fill.resting_id = 1;
    fill.price = 1000000;
    fill.quantity = 10000;
    fill.ts = 5100;
    logger.on_order_filled(fill);

    // Two fill latency samples:
    //   resting:   5100 - 1000 = 4100
    //   aggressor: 5100 - 5000 = 100
    EXPECT_EQ(logger.fill_histogram().count(), 2u);
    EXPECT_EQ(logger.fill_histogram().min(), 100);
    EXPECT_EQ(logger.fill_histogram().max(), 4100);
}

TEST(LatencyLoggerTest, CancelCleansUpTracking) {
    LatencyLogger logger;

    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1100;
    logger.on_order_accepted(ack);

    OrderCancelled cancel{};
    cancel.id = 1;
    cancel.ts = 2000;
    cancel.reason = CancelReason::UserRequested;
    logger.on_order_cancelled(cancel);

    // Fill after cancel should not record (order removed from tracking).
    OrderFilled fill{};
    fill.aggressor_id = 99;
    fill.resting_id = 1;
    fill.price = 1000000;
    fill.quantity = 10000;
    fill.ts = 3000;
    logger.on_order_filled(fill);

    EXPECT_EQ(logger.fill_histogram().count(), 0u);
}

TEST(LatencyLoggerTest, MultipleOrdersIndependent) {
    LatencyLogger logger;

    auto req1 = make_request(1, 1000);
    auto req2 = make_request(2, 2000);
    logger.on_order_submitted(req1);
    logger.on_order_submitted(req2);

    OrderAccepted ack1{};
    ack1.id = 1;
    ack1.client_order_id = 1;
    ack1.ts = 1500;  // 500ns latency
    logger.on_order_accepted(ack1);

    OrderAccepted ack2{};
    ack2.id = 2;
    ack2.client_order_id = 2;
    ack2.ts = 2300;  // 300ns latency
    logger.on_order_accepted(ack2);

    EXPECT_EQ(logger.ack_histogram().count(), 2u);
    EXPECT_EQ(logger.ack_histogram().min(), 300);
    EXPECT_EQ(logger.ack_histogram().max(), 500);
}

TEST(LatencyLoggerTest, ResetClearsEverything) {
    LatencyLogger logger;

    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);

    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1200;
    logger.on_order_accepted(ack);

    EXPECT_EQ(logger.ack_histogram().count(), 1u);

    logger.reset();
    EXPECT_EQ(logger.ack_histogram().count(), 0u);
    EXPECT_EQ(logger.fill_histogram().count(), 0u);
    EXPECT_EQ(logger.reject_histogram().count(), 0u);
}

TEST(LatencyLoggerTest, ReportProducesOutput) {
    LatencyLogger logger;

    auto req = make_request(1, 1000);
    logger.on_order_submitted(req);
    OrderAccepted ack{};
    ack.id = 1;
    ack.client_order_id = 1;
    ack.ts = 1500;
    logger.on_order_accepted(ack);

    std::ostringstream os;
    logger.report(os);
    std::string output = os.str();

    EXPECT_NE(output.find("Entry-to-Ack"), std::string::npos);
    EXPECT_NE(output.find("Entry-to-Fill"), std::string::npos);
    EXPECT_NE(output.find("Entry-to-Reject"), std::string::npos);
}

TEST(LatencyLoggerTest, UnknownOrderEventsIgnored) {
    LatencyLogger logger;

    // Accept without prior submission — should not crash or record.
    OrderAccepted ack{};
    ack.id = 99;
    ack.client_order_id = 99;
    ack.ts = 1000;
    logger.on_order_accepted(ack);
    EXPECT_EQ(logger.ack_histogram().count(), 0u);

    // Fill for unknown order — should not crash or record.
    OrderFilled fill{};
    fill.aggressor_id = 99;
    fill.resting_id = 88;
    fill.price = 1000000;
    fill.quantity = 10000;
    fill.ts = 2000;
    logger.on_order_filled(fill);
    EXPECT_EQ(logger.fill_histogram().count(), 0u);
}

}  // namespace exchange
