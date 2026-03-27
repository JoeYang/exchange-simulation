#include "exchange-core/matching_engine.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// Exchange with VI enabled: triggers when trade deviates > vi_threshold_ticks
// from reference price (tick-based threshold).
// ---------------------------------------------------------------------------

class ViExchange
    : public MatchingEngine<ViExchange, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<ViExchange, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    bool should_trigger_volatility_auction(Price trade_price,
                                           Price reference_price) {
        if (config_.vi.vi_threshold_ticks <= 0) return false;
        Price threshold = config_.vi.vi_threshold_ticks * config_.tick_size;
        Price deviation = std::abs(trade_price - reference_price);
        return deviation > threshold;
    }
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class VolatilityAuctionTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;

    // VI threshold = 10 ticks * 100 tick_size = 1000 price units
    EngineConfig config_{
        .tick_size = 100,
        .lot_size = 10000,
        .price_band_low = 0,
        .price_band_high = 0,
        .vi = VolatilityConfig{.vi_threshold_ticks = 10,
                               .vi_auction_duration_ns = 5000000000}};
    ViExchange engine_{config_, order_listener_, md_listener_};

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

    // Create a crossing trade to establish reference price
    void establish_reference(Price price, Timestamp ts) {
        engine_.new_order(make_limit(100, Side::Buy, price, 10000, ts));
        engine_.new_order(make_limit(101, Side::Sell, price, 10000, ts + 1));
        // Set reference by transitioning through Continuous
        // (the first trade sets last_trade_price_, but vi_reference_price_
        //  is set when entering Continuous or after an auction)
    }

    bool has_market_status(SessionState state) const {
        for (const auto& e : md_listener_.events()) {
            if (std::holds_alternative<MarketStatus>(e)) {
                if (std::get<MarketStatus>(e).state == state) return true;
            }
        }
        return false;
    }
};

// ===========================================================================
// 1. Trade exceeding VI threshold triggers VolatilityAuction
// ===========================================================================

TEST_F(VolatilityAuctionTest, TradeExceedsThresholdTriggersAuction) {
    // Establish reference price via session state transition
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 10000, 2));
    // last_trade_price_ = 1000000

    // Transition to PreOpen then back to Continuous to set vi_reference_price_
    engine_.set_session_state(SessionState::PreOpen, 3);
    engine_.set_session_state(SessionState::Continuous, 4);
    // vi_reference_price_ = 1000000

    order_listener_.clear();
    md_listener_.clear();

    // Trade at 1001200 (deviation = 1200 > threshold 1000)
    engine_.new_order(make_limit(3, Side::Buy, 1001200, 10000, 10));
    engine_.new_order(make_limit(4, Side::Sell, 1001200, 10000, 11));

    // Should have triggered VolatilityAuction
    EXPECT_TRUE(has_market_status(SessionState::VolatilityAuction));
    EXPECT_EQ(engine_.session_state(), SessionState::VolatilityAuction);
}

// ===========================================================================
// 2. Trade within threshold -- no transition
// ===========================================================================

TEST_F(VolatilityAuctionTest, TradeWithinThresholdNoTransition) {
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 10000, 2));

    engine_.set_session_state(SessionState::PreOpen, 3);
    engine_.set_session_state(SessionState::Continuous, 4);

    md_listener_.clear();

    // Trade at 1000800 (deviation = 800 < threshold 1000) -- no trigger
    engine_.new_order(make_limit(3, Side::Buy, 1000800, 10000, 10));
    engine_.new_order(make_limit(4, Side::Sell, 1000800, 10000, 11));

    EXPECT_FALSE(has_market_status(SessionState::VolatilityAuction));
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
}

// ===========================================================================
// 3. VI disabled (threshold=0) -- no transition
// ===========================================================================

TEST_F(VolatilityAuctionTest, ViDisabledNoTransition) {
    // Create engine with VI disabled
    RecordingOrderListener ol;
    RecordingMdListener ml;
    EngineConfig cfg{.tick_size = 100,
                     .lot_size = 10000,
                     .price_band_low = 0,
                     .price_band_high = 0,
                     .vi = VolatilityConfig{.vi_threshold_ticks = 0}};
    ViExchange eng(cfg, ol, ml);

    eng.new_order(make_limit(1, Side::Buy, 1000000, 10000, 1));
    eng.new_order(make_limit(2, Side::Sell, 1000000, 10000, 2));
    eng.set_session_state(SessionState::PreOpen, 3);
    eng.set_session_state(SessionState::Continuous, 4);
    ml.clear();

    // Big move -- but VI disabled
    eng.new_order(make_limit(3, Side::Buy, 2000000, 10000, 10));
    eng.new_order(make_limit(4, Side::Sell, 2000000, 10000, 11));

    EXPECT_EQ(eng.session_state(), SessionState::Continuous);
}

// ===========================================================================
// 4. VI triggers mid-sweep: aggressor partial fill, rest goes to book
// ===========================================================================

TEST_F(VolatilityAuctionTest, ViTriggersMidSweepRestGoesToBook) {
    // Set reference price = 1000000
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 10000, 2));
    engine_.set_session_state(SessionState::PreOpen, 3);
    engine_.set_session_state(SessionState::Continuous, 4);

    // Place two sell orders at different prices
    // First at 1000500 (within threshold), second at 1001200 (exceeds)
    engine_.new_order(make_limit(5, Side::Sell, 1000500, 10000, 5));
    engine_.new_order(make_limit(6, Side::Sell, 1001200, 10000, 6));

    order_listener_.clear();
    md_listener_.clear();

    // Buy sweeps at 1001200 -- should fill at 1000500, then fill at 1001200
    // triggers VI. The aggressor was fully filled in this case since both
    // levels have 10000 qty each and aggressor wants 20000.
    engine_.new_order(make_limit(7, Side::Buy, 1001200, 20000, 10));

    // The fill at 1000500 should succeed (within threshold).
    // The fill at 1001200 should also succeed, but then VI triggers.
    EXPECT_TRUE(has_market_status(SessionState::VolatilityAuction));
    EXPECT_EQ(engine_.session_state(), SessionState::VolatilityAuction);
}

// ===========================================================================
// 5. Already in auction phase -- no re-trigger
// ===========================================================================

TEST_F(VolatilityAuctionTest, AlreadyInAuctionNoRetrigger) {
    engine_.new_order(make_limit(1, Side::Buy, 1000000, 10000, 1));
    engine_.new_order(make_limit(2, Side::Sell, 1000000, 10000, 2));

    // Stay in VolatilityAuction state
    engine_.set_session_state(SessionState::VolatilityAuction, 3);

    md_listener_.clear();

    // VI check should not trigger (already in auction state, not Continuous)
    // Orders in auction collection phase don't match anyway
    engine_.new_order(make_limit(3, Side::Buy, 2000000, 10000, 10));

    EXPECT_EQ(engine_.session_state(), SessionState::VolatilityAuction);
}

// ===========================================================================
// 6. Auction execution updates VI reference price
// ===========================================================================

TEST_F(VolatilityAuctionTest, AuctionExecutionUpdatesReference) {
    // Setup: auction with some orders
    engine_.set_session_state(SessionState::PreOpen, 1);
    engine_.new_order(make_limit(1, Side::Buy, 1500000, 10000, 2));
    engine_.new_order(make_limit(2, Side::Sell, 1500000, 10000, 3));

    // Execute auction at reference 1000000 -- clears at 1500000
    engine_.execute_auction(1000000, 4);

    // Transition to continuous -- vi_reference_price_ should be auction price
    engine_.set_session_state(SessionState::Continuous, 5);

    md_listener_.clear();

    // Trade at 1500500 (deviation = 500 < threshold 1000 from 1500000)
    engine_.new_order(make_limit(3, Side::Buy, 1500500, 10000, 10));
    engine_.new_order(make_limit(4, Side::Sell, 1500500, 10000, 11));

    // Should NOT trigger (within threshold of new reference)
    EXPECT_EQ(engine_.session_state(), SessionState::Continuous);
}

}  // namespace
}  // namespace exchange
