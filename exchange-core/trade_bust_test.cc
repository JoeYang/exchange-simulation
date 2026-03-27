#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Minimal CRTP exchange for testing -- uses all defaults
// ---------------------------------------------------------------------------

class TestExchange
    : public MatchingEngine<TestExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class TradeBustTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    EngineConfig config_{.tick_size = 100,
                         .lot_size = 10000,
                         .price_band_low = 0,
                         .price_band_high = 0};
    TestExchange engine_{config_, order_listener_, md_listener_};

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

    // Execute a crossing trade: buy rests, sell crosses at same price.
    // Returns after both orders are submitted.
    void create_crossing_trade(Price price, Quantity qty) {
        engine_.new_order(make_limit(1, Side::Buy, price, qty, 1));
        engine_.new_order(make_limit(2, Side::Sell, price, qty, 2));
    }
};

// ===========================================================================
// 1. Bust a trade where both orders were resting -- quantities restored
// ===========================================================================

TEST_F(TradeBustTest, BustTradePartialFillBothAlive) {
    // Buy 100, sell 50 -- partial fill, both orders alive
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 50000, 2));

    // Trade happened: qty=50000, aggressor=2 (sell), resting=1 (buy)
    // Buy order: filled=50000, remaining=50000, still on book
    // Sell order: fully filled, deallocated

    // Clear events to isolate bust events
    order_listener_.clear();
    md_listener_.clear();

    // Bust the trade (trade_id=1, first trade)
    engine_.bust_trade(1, BustReason::ErroneousTrade, 3);

    // Should fire TradeBusted event
    ASSERT_GE(order_listener_.size(), 1u);
    auto& evt = order_listener_.events().back();
    ASSERT_TRUE(std::holds_alternative<TradeBusted>(evt));

    auto& busted = std::get<TradeBusted>(evt);
    EXPECT_EQ(busted.trade_id, 1u);
    EXPECT_EQ(busted.aggressor_id, 2u);
    EXPECT_EQ(busted.resting_id, 1u);
    EXPECT_EQ(busted.price, 1000000);
    EXPECT_EQ(busted.quantity, 50000);
    EXPECT_EQ(busted.reason, BustReason::ErroneousTrade);
    EXPECT_EQ(busted.ts, 3);
}

// ===========================================================================
// 2. Bust a trade where aggressor was fully filled (deallocated)
//    Resting side is restored, event still fires
// ===========================================================================

TEST_F(TradeBustTest, BustTradeAggressorDeallocated) {
    // Buy rests with 100000 qty
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1));
    // Sell crosses with exact qty -- both fully filled
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 100000, 2));

    // Both orders are fully filled and deallocated.
    order_listener_.clear();

    engine_.bust_trade(1, BustReason::ErroneousTrade, 3);

    // Event should still fire even though both orders are deallocated
    ASSERT_GE(order_listener_.size(), 1u);
    auto& evt = order_listener_.events().back();
    ASSERT_TRUE(std::holds_alternative<TradeBusted>(evt));

    auto& busted = std::get<TradeBusted>(evt);
    EXPECT_EQ(busted.trade_id, 1u);
    EXPECT_EQ(busted.quantity, 100000);
}

// ===========================================================================
// 3. Bust restores resting order that is still on the book
// ===========================================================================

TEST_F(TradeBustTest, BustRestoresRestingOrderOnBook) {
    // Buy 100000 rests
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 100000, 1));
    // Sell 50000 crosses -- partial fill of buy
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 50000, 2));

    // Buy order (id=1) should have filled=50000, remaining=50000
    // Verify via trade registry
    ASSERT_EQ(engine_.trade_registry().trade_count(), 1u);

    order_listener_.clear();

    engine_.bust_trade(1, BustReason::ErroneousTrade, 3);

    // After bust: buy order should be restored to filled=0, remaining=100000
    // We can verify by trying to cross with another sell order
    order_listener_.clear();
    md_listener_.clear();

    // Send sell for 100000 -- should fully match the restored buy
    engine_.new_order(make_limit(3, Side::Sell, 1000000, 100000, 4));

    // The sell should match against 100000 of the buy (restored)
    bool found_fill = false;
    for (const auto& e : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(e)) {
            auto& fill = std::get<OrderFilled>(e);
            EXPECT_EQ(fill.quantity, 100000);
            found_fill = true;
        }
    }
    EXPECT_TRUE(found_fill) << "Expected a fill after bust restored the order";
}

// ===========================================================================
// 4. Bust non-existent trade ID -- no event fired
// ===========================================================================

TEST_F(TradeBustTest, BustNonExistentTradeNoEvent) {
    order_listener_.clear();
    engine_.bust_trade(999, BustReason::ErroneousTrade, 1);

    // No TradeBusted should be fired
    for (const auto& e : order_listener_.events()) {
        EXPECT_FALSE(std::holds_alternative<TradeBusted>(e));
    }
}

// ===========================================================================
// 5. Bust same trade twice -- second bust does nothing
// ===========================================================================

TEST_F(TradeBustTest, BustSameTradeTwiceSecondIgnored) {
    create_crossing_trade(1000000, 50000);
    order_listener_.clear();

    engine_.bust_trade(1, BustReason::ErroneousTrade, 3);
    ASSERT_EQ(order_listener_.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<TradeBusted>(
        order_listener_.events()[0]));

    // Second bust -- should produce no additional event
    size_t count_before = order_listener_.size();
    engine_.bust_trade(1, BustReason::ErroneousTrade, 4);
    EXPECT_EQ(order_listener_.size(), count_before);
}

// ===========================================================================
// 6. Bust in Closed session state -- no event fired
// ===========================================================================

TEST_F(TradeBustTest, BustInClosedSessionStateRejected) {
    create_crossing_trade(1000000, 50000);
    order_listener_.clear();

    engine_.set_session_state(SessionState::Closed, 3);
    engine_.bust_trade(1, BustReason::ErroneousTrade, 4);

    // No TradeBusted should be fired
    for (const auto& e : order_listener_.events()) {
        EXPECT_FALSE(std::holds_alternative<TradeBusted>(e));
    }
}

// ===========================================================================
// 7. Trade recording -- trades are registered during matching
// ===========================================================================

TEST_F(TradeBustTest, TradesRecordedDuringMatching) {
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 50000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 50000, 2));

    EXPECT_EQ(engine_.trade_registry().trade_count(), 1u);

    auto record = engine_.trade_registry().lookup(1);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->aggressor_id, 2u);  // sell is aggressor
    EXPECT_EQ(record->resting_id, 1u);    // buy was resting
    EXPECT_EQ(record->price, 1000000);
    EXPECT_EQ(record->quantity, 50000);
    EXPECT_EQ(record->ts, 2);
    EXPECT_FALSE(record->busted);
}

// ===========================================================================
// 8. Multiple trades produce sequential trade IDs
// ===========================================================================

TEST_F(TradeBustTest, MultipleTradesSequentialIds) {
    // First trade
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 50000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 50000, 2));

    // Second trade
    engine_.new_order(make_limit(3, Side::Buy, 2000000, 30000, 3));
    engine_.new_order(make_limit(4, Side::Sell, 2000000, 30000, 4));

    EXPECT_EQ(engine_.trade_registry().trade_count(), 2u);

    auto r1 = engine_.trade_registry().lookup(1);
    auto r2 = engine_.trade_registry().lookup(2);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r1->price, 1000000);
    EXPECT_EQ(r2->price, 2000000);
}

}  // namespace
}  // namespace exchange
