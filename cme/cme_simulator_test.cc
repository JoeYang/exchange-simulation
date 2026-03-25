#include "cme/cme_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace cme {
namespace {

// ---------------------------------------------------------------------------
// Convenience alias with small pool sizes for fast tests
// ---------------------------------------------------------------------------

using TestCmeSimulator =
    CmeSimulator<RecordingOrderListener, RecordingMdListener>;

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class CmeSimulatorTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestCmeSimulator sim_{order_listener_, md_listener_};

    // Helper: build a limit order request.
    OrderRequest make_limit(uint64_t cl_id, uint64_t account_id, Side side,
                            Price price, Quantity qty,
                            TimeInForce tif = TimeInForce::GTC,
                            Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id = account_id,
            .side = side,
            .type = OrderType::Limit,
            .tif = tif,
            .price = price,
            .quantity = qty,
            .stop_price = 0,
            .timestamp = ts,
            .gtd_expiry = 0,
        };
    }
};

// ===========================================================================
// Product loading
// ===========================================================================

TEST_F(CmeSimulatorTest, LoadAllCmeProducts) {
    auto products = get_cme_products();
    sim_.load_products(products);

    EXPECT_EQ(sim_.instrument_count(), products.size());
}

TEST_F(CmeSimulatorTest, LoadedProductsHaveCorrectBandPct) {
    sim_.load_products(get_cme_products());

    // ES (id=1) should have 5% band
    auto* es_engine = sim_.get_engine(1);
    ASSERT_NE(es_engine, nullptr);
    EXPECT_EQ(es_engine->band_percentage(), 5);

    // CL (id=3) should have 7% band
    auto* cl_engine = sim_.get_engine(3);
    ASSERT_NE(cl_engine, nullptr);
    EXPECT_EQ(cl_engine->band_percentage(), 7);

    // ZN (id=5) should have 3% band
    auto* zn_engine = sim_.get_engine(5);
    ASSERT_NE(zn_engine, nullptr);
    EXPECT_EQ(zn_engine->band_percentage(), 3);
}

// ===========================================================================
// Order routing to specific instruments
// ===========================================================================

TEST_F(CmeSimulatorTest, RouteOrderToEs) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // ES tick = 2500, lot = 10000.  Price 5000.0000 = 50000000 fixed-point.
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 50000000, 10000));

    bool saw_accepted = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(ev)) {
            saw_accepted = true;
        }
    }
    EXPECT_TRUE(saw_accepted) << "ES order must be accepted";
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 1u);
}

TEST_F(CmeSimulatorTest, RouteOrderToClWithDifferentTickSize) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // CL tick = 100 (=$0.01).  Price 75.00 = 750000 fixed-point.
    sim_.new_order(3, make_limit(1, 10, Side::Buy, 750000, 10000));

    bool saw_accepted = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(ev)) {
            saw_accepted = true;
        }
    }
    EXPECT_TRUE(saw_accepted) << "CL order must be accepted";
    EXPECT_EQ(sim_.get_engine(3)->active_order_count(), 1u);
}

TEST_F(CmeSimulatorTest, ClRejectsBadTickAlignment) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // CL tick = 100.  Price 750050 is not aligned to 100.
    sim_.new_order(3, make_limit(1, 10, Side::Buy, 750050, 10000));

    bool saw_reject = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) {
            saw_reject = true;
        }
    }
    EXPECT_TRUE(saw_reject)
        << "CL order with misaligned tick must be rejected";
}

// ===========================================================================
// Cross-instrument isolation
// ===========================================================================

TEST_F(CmeSimulatorTest, FillOnEsDoesNotAffectNq) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Place resting sell on NQ (id=2)
    sim_.new_order(2, make_limit(100, 1, Side::Sell, 180000000, 10000, TimeInForce::GTC, 300));

    // Place crossing orders on ES (id=1) -- these should match
    sim_.new_order(1, make_limit(200, 1, Side::Sell, 50000000, 10000, TimeInForce::GTC, 301));
    sim_.new_order(1, make_limit(201, 2, Side::Buy, 50000000, 10000, TimeInForce::GTC, 302));

    // ES orders should have filled (0 active), but NQ should still have 1.
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 0u)
        << "ES orders should have crossed and filled";
    EXPECT_EQ(sim_.get_engine(2)->active_order_count(), 1u)
        << "NQ order must be unaffected by ES fill";
}

// ===========================================================================
// Session lifecycle
// ===========================================================================

TEST_F(CmeSimulatorTest, StartTradingDaySetsPreOpen) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);

    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);
}

TEST_F(CmeSimulatorTest, OpenMarketSetsContinuous) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);
}

TEST_F(CmeSimulatorTest, CloseMarketSetsClosed) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);
    sim_.close_market(300);

    EXPECT_EQ(sim_.session_state(), SessionState::Closed);
}

TEST_F(CmeSimulatorTest, FullLifecycleTransitions) {
    sim_.load_products(get_cme_products());

    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    sim_.start_trading_day(100);
    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);

    sim_.open_market(200);
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);

    sim_.close_market(300);
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);
}

TEST_F(CmeSimulatorTest, OpeningAuctionUncrossesPreOpenOrders) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);

    // Place crossing orders during PreOpen on ES (id=1)
    sim_.new_order(1, make_limit(1, 1, Side::Buy, 50005000, 10000, TimeInForce::GTC, 101));
    sim_.new_order(1, make_limit(2, 2, Side::Sell, 50000000, 10000, TimeInForce::GTC, 102));

    // Both should be resting (no matching in PreOpen)
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 2u);

    sim_.open_market(200);

    // After auction, crossing orders should have filled.
    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 0u);
}

TEST_F(CmeSimulatorTest, EndOfDayExpiresDayOrders) {
    sim_.load_products(get_cme_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Place a DAY order and a GTC order on ES
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 49000000, 10000, TimeInForce::DAY, 300));
    sim_.new_order(1, make_limit(2, 11, Side::Buy, 49000000, 10000, TimeInForce::GTC, 301));

    EXPECT_EQ(sim_.get_engine(1)->active_order_count(), 2u);

    sim_.close_market(400);
    sim_.end_of_day(500);

    // DAY order expired, GTC survives -- but Closed state rejects cancels,
    // so trigger_expiry must have processed DAY orders.
    // After close_market the engine is in Closed state. trigger_expiry
    // performs cancel_active_order internally which bypasses the state check.
    // Let's verify the order listener saw the Expired cancel.
    bool saw_day_expired = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) {
            auto& c = std::get<OrderCancelled>(ev);
            if (c.reason == CancelReason::Expired) {
                saw_day_expired = true;
            }
        }
    }
    EXPECT_TRUE(saw_day_expired) << "DAY order must be expired at end of day";
}

}  // namespace
}  // namespace cme
}  // namespace exchange
