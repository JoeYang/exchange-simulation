#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Exchange with position limits enabled. Uniform limit for all accounts.
class PosLimitExchange
    : public MatchingEngine<PosLimitExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<PosLimitExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    Quantity position_limit{0};  // set by test, 0 = no limit

    bool is_position_check_enabled() { return position_limit > 0; }
    Quantity get_position_limit(uint64_t /*account_id*/) {
        return position_limit;
    }
};

class PositionLimitEngineTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    PosLimitExchange engine_{config_, order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_ord_id, Side side, Price price,
                            Quantity qty, Timestamp ts,
                            uint64_t account_id = 1) {
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
// 1. Order that would exceed position limit is rejected
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, OrderExceedingLimitRejected) {
    engine_.position_limit = 100000;

    // First: create a fill to build up position
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 100000, 2, 2));

    // Account 1 is now long 100000 (at limit)
    order_listener_.clear();

    // New buy for account 1: would push to 110000 > 100000
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 10000, 3, 1));

    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::PositionLimitExceeded);
}

// ---------------------------------------------------------------------------
// 2. Order within limit is accepted and fills update position
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, OrderWithinLimitAccepted) {
    engine_.position_limit = 100000;

    // Buy 50000 -- within limit of 100000
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 50000, 1, 1));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));

    // Sell against it to fill
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 50000, 2, 2));

    // Account 1: net long 50000. Account 2: net short 50000.
    // Another buy of 50000 should be accepted (total = 100000 = at limit)
    order_listener_.clear();
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 50000, 3, 1));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 3. Position limit disabled (default) -- all orders accepted
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, DisabledLimitAcceptsAll) {
    // position_limit remains 0 (disabled)

    // Large buy -- accepted even though it would be huge position
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 9990000, 1, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 9990000, 2, 2));

    // Another massive buy -- still accepted
    order_listener_.clear();
    engine_.new_order(make_limit(3, Side::Buy, 1000100, 9990000, 3, 1));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 4. Sell order exceeding short limit is rejected
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, SellExceedingShortLimitRejected) {
    engine_.position_limit = 100000;

    // Build short position: account 2 sells
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 100000, 2, 2));

    // Account 2 is now short 100000 (at limit). Another sell rejected.
    order_listener_.clear();
    engine_.new_order(make_limit(3, Side::Sell, 999900, 10000, 3, 2));
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));
    auto& rej = std::get<OrderRejected>(order_listener_.events()[0]);
    EXPECT_EQ(rej.reason, RejectReason::PositionLimitExceeded);
}

// ---------------------------------------------------------------------------
// 5. Bust reverses position -- allows new orders
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, BustReversesPosition) {
    engine_.position_limit = 100000;

    // Fill to build up position to limit
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 100000, 2, 2));

    // Account 1 at limit -- buy rejected
    order_listener_.clear();
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 10000, 3, 1));
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));

    // Bust trade 1 (the only trade)
    engine_.bust_trade(1, BustReason::ErroneousTrade, 4);

    // Now account 1 position should be 0 -- buy accepted
    order_listener_.clear();
    engine_.new_order(make_limit(4, Side::Buy, 1000000, 50000, 5, 1));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

// ---------------------------------------------------------------------------
// 6. Different accounts have independent limits
// ---------------------------------------------------------------------------

TEST_F(PositionLimitEngineTest, IndependentAccountLimits) {
    engine_.position_limit = 100000;

    // Account 1 buys 100000
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 100000, 2, 2));

    // Account 1 at limit -- buy rejected
    order_listener_.clear();
    engine_.new_order(make_limit(3, Side::Buy, 1000000, 10000, 3, 1));
    EXPECT_TRUE(std::holds_alternative<OrderRejected>(
        order_listener_.events()[0]));

    // Account 3 has no position -- buy accepted
    order_listener_.clear();
    engine_.new_order(make_limit(4, Side::Buy, 1000100, 100000, 4, 3));
    ASSERT_GE(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<OrderAccepted>(
        order_listener_.events()[0]));
}

}  // namespace
}  // namespace exchange
