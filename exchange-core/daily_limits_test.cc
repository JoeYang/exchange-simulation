#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Minimal CRTP exchange for daily limits testing.
class DailyLimitExchange
    : public MatchingEngine<DailyLimitExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<DailyLimitExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

class DailyLimitsTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;

    // tick=100, lot=10000, daily limits: low=900000, high=1100000
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0,
                         .max_order_size = 0,
                         .daily_limit_high = 1100000,
                         .daily_limit_low = 900000};
    DailyLimitExchange engine_{config_, order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts) {
        return OrderRequest{.client_order_id = cl_ord_id,
                            .account_id = 1,
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
// 1. Order at daily limit price is accepted
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, OrderAtLimitPriceAccepted) {
    // Buy at upper limit -- should be accepted
    engine_.new_order(make_limit(1, Side::Buy, 1100000, 10000, 1));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));

    order_listener_.clear();
    md_listener_.clear();

    // Sell at lower limit -- should be accepted
    engine_.new_order(make_limit(2, Side::Sell, 900000, 10000, 2));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 2. Order beyond daily limit is rejected
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, OrderBeyondUpperLimitRejected) {
    engine_.new_order(make_limit(1, Side::Buy, 1100100, 10000, 1));
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::LockLimitUp);
}

TEST_F(DailyLimitsTest, OrderBeyondLowerLimitRejected) {
    engine_.new_order(make_limit(1, Side::Sell, 899900, 10000, 1));
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::LockLimitDown);
}

// ---------------------------------------------------------------------------
// 3. Trade at limit price triggers lock-limit state
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, TradeAtLowerLimitTriggersLock) {
    // Resting sell at lower limit
    engine_.new_order(make_limit(1, Side::Sell, 900000, 10000, 1));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor buy at lower limit -- trade at 900000 triggers lock
    engine_.new_order(make_limit(2, Side::Buy, 900000, 10000, 2));

    // Should see: OrderAccepted, OrderFilled
    ASSERT_GE(order_listener_.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
    EXPECT_TRUE(std::holds_alternative<OrderFilled>(
        order_listener_.events()[1]));

    // MD should contain LockLimitTriggered and MarketStatus(LockLimit)
    bool found_lock_limit = false;
    bool found_market_status = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<LockLimitTriggered>(ev)) {
            auto& llt = std::get<LockLimitTriggered>(ev);
            EXPECT_EQ(llt.side, Side::Sell);
            EXPECT_EQ(llt.limit_price, 900000);
            EXPECT_EQ(llt.last_trade_price, 900000);
            EXPECT_EQ(llt.ts, 2);
            found_lock_limit = true;
        }
        if (std::holds_alternative<MarketStatus>(ev)) {
            auto& ms = std::get<MarketStatus>(ev);
            if (ms.state == SessionState::LockLimit) {
                found_market_status = true;
            }
        }
    }
    EXPECT_TRUE(found_lock_limit) << "LockLimitTriggered event not found";
    EXPECT_TRUE(found_market_status) << "MarketStatus(LockLimit) not found";

    EXPECT_EQ(engine_.session_state(), SessionState::LockLimit);
}

TEST_F(DailyLimitsTest, TradeAtUpperLimitTriggersLock) {
    // Resting buy at upper limit
    engine_.new_order(make_limit(1, Side::Buy, 1100000, 10000, 1));
    order_listener_.clear();
    md_listener_.clear();

    // Aggressor sell at upper limit -- trade at 1100000 triggers lock
    engine_.new_order(make_limit(2, Side::Sell, 1100000, 10000, 2));

    EXPECT_EQ(engine_.session_state(), SessionState::LockLimit);

    bool found_lock_limit = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<LockLimitTriggered>(ev)) {
            auto& llt = std::get<LockLimitTriggered>(ev);
            EXPECT_EQ(llt.side, Side::Buy);
            EXPECT_EQ(llt.limit_price, 1100000);
            found_lock_limit = true;
        }
    }
    EXPECT_TRUE(found_lock_limit);
}

// ---------------------------------------------------------------------------
// 4. In LockLimit: orders accepted but no matching
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, LockLimitPreventsMatching) {
    // Create a trade at lower limit to trigger lock
    engine_.new_order(make_limit(1, Side::Sell, 900000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Buy, 900000, 10000, 2));
    ASSERT_EQ(engine_.session_state(), SessionState::LockLimit);

    order_listener_.clear();
    md_listener_.clear();

    // Submit crossing orders -- both should be accepted but NOT matched
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 10000, 3));
    engine_.new_order(make_limit(4, Side::Sell, 1000000, 10000, 4));

    // Both orders accepted, no fills
    size_t fill_count = 0;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev) ||
            std::holds_alternative<OrderPartiallyFilled>(ev)) {
            ++fill_count;
        }
    }
    EXPECT_EQ(fill_count, 0u) << "No matching should occur in LockLimit";
}

// ---------------------------------------------------------------------------
// 5. unlock_limit resumes matching
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, UnlockLimitResumesMatching) {
    // Trigger lock state
    engine_.new_order(make_limit(1, Side::Sell, 900000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Buy, 900000, 10000, 2));
    ASSERT_EQ(engine_.session_state(), SessionState::LockLimit);

    // Queue crossing orders (no matching yet)
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 10000, 3));
    engine_.new_order(make_limit(4, Side::Sell, 1000000, 10000, 4));

    order_listener_.clear();
    md_listener_.clear();

    // Unlock -- should resume to Continuous
    engine_.unlock_limit(5);
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);

    // MarketStatus event fired
    bool found_continuous = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<MarketStatus>(ev)) {
            auto& ms = std::get<MarketStatus>(ev);
            if (ms.state == SessionState::Continuous) {
                found_continuous = true;
            }
        }
    }
    EXPECT_TRUE(found_continuous);

    // Now submit a new crossing order -- should match against resting
    order_listener_.clear();
    md_listener_.clear();
    engine_.new_order(make_limit(5, Side::Sell, 1000000, 10000, 6));

    // The sell at 1000000 should match the resting buy at 1000000
    bool found_fill = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) {
            found_fill = true;
        }
    }
    EXPECT_TRUE(found_fill) << "Matching should resume after unlock";
}

// ---------------------------------------------------------------------------
// 6. Daily limits of 0 = no limit (backward compatible)
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, ZeroLimitsDisabled) {
    // Create engine with no daily limits
    EngineConfig no_limits{.tick_size = 100,
                           .lot_size = 10000,
                           .price_band_low = 0,
                           .price_band_high = 0};
    RecordingOrderListener ol;
    RecordingMdListener ml;
    DailyLimitExchange engine(no_limits, ol, ml);

    // Any price should be accepted
    engine.new_order(make_limit(1, Side::Buy, 99999900, 10000, 1));
    ASSERT_GE(ol.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ol.events()[0]));

    ol.clear();
    engine.new_order(make_limit(2, Side::Sell, 100, 10000, 2));
    ASSERT_GE(ol.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(ol.events()[0]));

    // Trade should not trigger lock
    EXPECT_EQ(engine.session_state(), SessionState::Continuous);
}

// ---------------------------------------------------------------------------
// 7. unlock_limit is no-op when not in LockLimit
// ---------------------------------------------------------------------------

TEST_F(DailyLimitsTest, UnlockLimitNoOpWhenNotLocked) {
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
    md_listener_.clear();

    engine_.unlock_limit(1);

    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
    // No MarketStatus event should be fired
    EXPECT_EQ(md_listener_.size(), 0u);
}

}  // namespace
}  // namespace exchange
