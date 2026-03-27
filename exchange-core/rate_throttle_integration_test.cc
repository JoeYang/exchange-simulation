#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

constexpr int64_t ONE_SECOND_NS = 1'000'000'000;

// ---------------------------------------------------------------------------
// CRTP exchange with rate checking ENABLED
// ---------------------------------------------------------------------------

class ThrottledExchange
    : public MatchingEngine<ThrottledExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch,
                            100, 50, 1000, 16> {
public:
    using Base = MatchingEngine<ThrottledExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch,
                                100, 50, 1000, 16>;
    using Base::Base;

    // Enable rate checking
    bool is_rate_check_enabled() { return true; }
};

// ---------------------------------------------------------------------------
// CRTP exchange with rate checking DISABLED (default)
// ---------------------------------------------------------------------------

class UnthrottledExchange
    : public MatchingEngine<UnthrottledExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch,
                            100, 50, 1000, 16> {
public:
    using Base = MatchingEngine<UnthrottledExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch,
                                100, 50, 1000, 16>;
    using Base::Base;
    // is_rate_check_enabled() defaults to false
};

// ---------------------------------------------------------------------------
// Test fixture for throttled engine
// ---------------------------------------------------------------------------

class RateThrottleIntegrationTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;

    EngineConfig make_config(int64_t rate_limit, int64_t interval_ns) {
        return EngineConfig{
            .tick_size = 100,
            .lot_size = 10000,
            .price_band_low = 0,
            .price_band_high = 0,
            .max_order_size = 0,
            .daily_limit_high = 0,
            .daily_limit_low = 0,
            .throttle = ThrottleConfig{.max_messages_per_interval = rate_limit,
                                       .interval_ns = interval_ns}};
    }

    OrderRequest make_limit(uint64_t cl_ord_id, uint64_t account_id,
                            Side side, Price price, Quantity qty,
                            Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = account_id,
                            .side = side,
                            .type = OrderType::Limit,
                            .tif = TimeInForce::GTC,
                            .price = price,
                            .quantity = qty,
                            .stop_price = 0,
                            .timestamp = ts,
                            .gtd_expiry = 0};
    }
};

// ---------------------------------------------------------------------------
// Orders within rate limit are accepted
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, OrdersWithinLimitAccepted) {
    auto config = make_config(3, ONE_SECOND_NS);
    ThrottledExchange engine(config, order_listener_, md_listener_);

    engine.new_order(make_limit(1, 1, Side::Buy, 1000000, 10000, 100));
    engine.new_order(make_limit(2, 1, Side::Buy, 990000, 10000, 200));
    engine.new_order(make_limit(3, 1, Side::Buy, 980000, 10000, 300));

    // All 3 should be accepted
    ASSERT_EQ(order_listener_.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
            order_listener_.events()[i]))
            << "event " << i << " should be OrderAccepted";
    }
}

// ---------------------------------------------------------------------------
// Order exceeding rate limit is rejected with RateThrottled
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, OrderExceedingLimitRejected) {
    auto config = make_config(3, ONE_SECOND_NS);
    ThrottledExchange engine(config, order_listener_, md_listener_);

    engine.new_order(make_limit(1, 1, Side::Buy, 1000000, 10000, 100));
    engine.new_order(make_limit(2, 1, Side::Buy, 990000, 10000, 200));
    engine.new_order(make_limit(3, 1, Side::Buy, 980000, 10000, 300));
    // 4th within same window: rejected
    engine.new_order(make_limit(4, 1, Side::Buy, 970000, 10000, 400));

    ASSERT_EQ(order_listener_.size(), 4u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[3]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[3]);
    EXPECT_EQ(rej.client_order_id, 4u);
    EXPECT_EQ(rej.reason, RejectReason::RateThrottled);
}

// ---------------------------------------------------------------------------
// Modify counts toward rate limit
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, ModifyCountsTowardRate) {
    auto config = make_config(2, ONE_SECOND_NS);
    ThrottledExchange engine(config, order_listener_, md_listener_);

    // 1st message: new order
    engine.new_order(make_limit(1, 1, Side::Buy, 1000000, 10000, 100));
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));

    // 2nd message: modify (counts toward rate)
    engine.modify_order(ModifyRequest{.order_id = 1,
                                       .client_order_id = 10,
                                       .new_price = 990000,
                                       .new_quantity = 10000,
                                       .timestamp = 200});
    // Modify should succeed (might produce Modified + other events)
    bool has_modified = false;
    for (auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderModified>(ev)) {
            has_modified = true;
            break;
        }
    }
    EXPECT_TRUE(has_modified);

    // 3rd message: another new order -- should be throttled
    order_listener_.clear();
    engine.new_order(make_limit(2, 1, Side::Buy, 980000, 10000, 300));
    ASSERT_EQ(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(order_listener_.events()[0]).reason,
              RejectReason::RateThrottled);
}

// ---------------------------------------------------------------------------
// Cancel counts toward rate limit
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, CancelCountsTowardRate) {
    auto config = make_config(2, ONE_SECOND_NS);
    ThrottledExchange engine(config, order_listener_, md_listener_);

    // 1st message: new order
    engine.new_order(make_limit(1, 1, Side::Buy, 1000000, 10000, 100));
    ASSERT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));

    // 2nd message: cancel (counts toward rate)
    engine.cancel_order(1, 200);
    bool has_cancel = false;
    for (auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) {
            has_cancel = true;
            break;
        }
    }
    EXPECT_TRUE(has_cancel);

    // 3rd message: new order -- should be throttled
    order_listener_.clear();
    engine.new_order(make_limit(2, 1, Side::Buy, 980000, 10000, 300));
    ASSERT_EQ(order_listener_.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    EXPECT_EQ(std::get<OrderRejected>(order_listener_.events()[0]).reason,
              RejectReason::RateThrottled);
}

// ---------------------------------------------------------------------------
// Rate resets after window expires
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, RateResetsAfterWindow) {
    auto config = make_config(2, ONE_SECOND_NS);
    ThrottledExchange engine(config, order_listener_, md_listener_);

    engine.new_order(make_limit(1, 1, Side::Buy, 1000000, 10000, 100));
    engine.new_order(make_limit(2, 1, Side::Buy, 990000, 10000, 200));
    // At limit
    engine.new_order(make_limit(3, 1, Side::Buy, 980000, 10000, 300));
    ASSERT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[2]));

    // After window expires
    order_listener_.clear();
    Timestamp after_window = 100 + ONE_SECOND_NS;
    engine.new_order(make_limit(4, 1, Side::Buy, 970000, 10000, after_window));
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// Rate disabled (default config): unlimited orders accepted
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, DisabledRateUnlimited) {
    EngineConfig config{.tick_size = 100,
                        .lot_size = 10000,
                        .price_band_low = 0,
                        .price_band_high = 0};
    // ThrottleConfig defaults to {0, 0} = disabled
    UnthrottledExchange engine(config, order_listener_, md_listener_);

    // Send many orders -- all accepted because is_rate_check_enabled=false
    for (uint64_t i = 1; i <= 50; ++i) {
        engine.new_order(make_limit(i, 1, Side::Buy,
                                    1000000 - i * 100, 10000, i));
    }
    ASSERT_EQ(order_listener_.size(), 50u);
    for (size_t i = 0; i < 50; ++i) {
        EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
            order_listener_.events()[i]))
            << "order " << i << " should be accepted";
    }
}

// ---------------------------------------------------------------------------
// ThrottledExchange with disabled throttle config: unlimited
// Even if is_rate_check_enabled=true, max_messages=0 means disabled
// ---------------------------------------------------------------------------

TEST_F(RateThrottleIntegrationTest, ThrottledExchangeWithZeroConfig) {
    auto config = make_config(0, ONE_SECOND_NS);  // 0 = disabled
    ThrottledExchange engine(config, order_listener_, md_listener_);

    for (uint64_t i = 1; i <= 20; ++i) {
        engine.new_order(make_limit(i, 1, Side::Buy,
                                    1000000 - i * 100, 10000, i));
    }
    ASSERT_EQ(order_listener_.size(), 20u);
    for (size_t i = 0; i < 20; ++i) {
        EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
            order_listener_.events()[i]));
    }
}

}  // namespace
}  // namespace exchange
