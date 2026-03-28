#include "krx/krx_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace krx {
namespace {

using TestKrxSimulator =
    KrxSimulator<RecordingOrderListener, RecordingMdListener>;

class KrxSimulatorTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestKrxSimulator sim_{order_listener_, md_listener_};

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

    bool has_accept() const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderAccepted>(ev)) return true;
        }
        return false;
    }

    bool has_any_reject() const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<OrderRejected>(ev)) return true;
        }
        return false;
    }

    bool has_trade() const {
        for (const auto& ev : md_listener_.events()) {
            if (std::holds_alternative<Trade>(ev)) return true;
        }
        return false;
    }
};

// ===========================================================================
// Product loading
// ===========================================================================

TEST_F(KrxSimulatorTest, LoadAllKrxProducts) {
    auto products = get_krx_products();
    sim_.load_products(products);

    EXPECT_EQ(sim_.instrument_count(), products.size());
}

TEST_F(KrxSimulatorTest, LoadedProductsHaveCorrectConfig) {
    sim_.load_products(get_krx_products());

    // KS (id=1) should have 5% order band, 3% VI dynamic.
    auto* ks = sim_.get_regular_engine(1);
    ASSERT_NE(ks, nullptr);
    EXPECT_EQ(ks->band_percentage(), 5);
    EXPECT_EQ(ks->vi_dynamic_percentage(), 3);
    EXPECT_EQ(ks->static_band_percentage(), 10);

    // USD (id=8) should have 3% order band.
    auto* usd = sim_.get_regular_engine(8);
    ASSERT_NE(usd, nullptr);
    EXPECT_EQ(usd->band_percentage(), 3);
}

// ===========================================================================
// Order routing
// ===========================================================================

TEST_F(KrxSimulatorTest, RouteOrderToKospi200) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);

    // KS tick = 500, lot = 10000. Price 350.0000 = 3500000.
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 3500000, 10000, TimeInForce::GTC, 300));

    EXPECT_TRUE(has_accept()) << "KS order must be accepted";
}

TEST_F(KrxSimulatorTest, RouteOrderRejectsWrongTick) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);

    // KS tick = 500. Price 3500100 not aligned to 500.
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 3500100, 10000, TimeInForce::GTC, 300));

    EXPECT_TRUE(has_any_reject()) << "Misaligned tick must be rejected";
}

// ===========================================================================
// Cross-instrument isolation
// ===========================================================================

TEST_F(KrxSimulatorTest, FillOnKsDoesNotAffectKtb) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);

    // Resting on KTB (id=9).
    sim_.new_order(9, make_limit(100, 1, Side::Sell, 1000000, 10000, TimeInForce::GTC, 300));

    // Cross on KS (id=1).
    sim_.new_order(1, make_limit(200, 1, Side::Sell, 3500000, 10000, TimeInForce::GTC, 301));
    sim_.new_order(1, make_limit(201, 2, Side::Buy, 3500000, 10000, TimeInForce::GTC, 302));

    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 0u)
        << "KS orders should have filled";
    EXPECT_EQ(sim_.get_regular_engine(9)->active_order_count(), 1u)
        << "KTB order must be unaffected";
}

// ===========================================================================
// Regular session lifecycle
// ===========================================================================

TEST_F(KrxSimulatorTest, RegularSessionPreOpen) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);

    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);
    EXPECT_FALSE(sim_.is_after_hours());
}

TEST_F(KrxSimulatorTest, RegularSessionOpenContinuous) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);

    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);
}

TEST_F(KrxSimulatorTest, RegularSessionPreClose) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);
    sim_.pre_close_regular(300);

    EXPECT_EQ(sim_.session_state(), SessionState::PreClose);
}

TEST_F(KrxSimulatorTest, RegularSessionClose) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);
    sim_.pre_close_regular(300);
    sim_.close_regular_session(400);

    EXPECT_EQ(sim_.session_state(), SessionState::Closed);
}

TEST_F(KrxSimulatorTest, OpeningAuctionUncrosses) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);

    // Place crossing orders during PreOpen on KS.
    sim_.new_order(1, make_limit(1, 1, Side::Buy, 3500500, 10000, TimeInForce::GTC, 101));
    sim_.new_order(1, make_limit(2, 2, Side::Sell, 3500000, 10000, TimeInForce::GTC, 102));

    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 2u);

    sim_.open_regular_market(200);

    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 0u)
        << "Crossing orders should fill in opening auction";
}

// ===========================================================================
// Dual session — after-hours
// ===========================================================================

TEST_F(KrxSimulatorTest, AfterHoursSessionLifecycle) {
    sim_.load_products(get_krx_products());

    // Complete regular session.
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);
    sim_.pre_close_regular(300);
    sim_.close_regular_session(400);

    // Start after-hours.
    sim_.start_after_hours(500);
    EXPECT_TRUE(sim_.is_after_hours());
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);

    // Close after-hours.
    sim_.close_after_hours(600);
    EXPECT_FALSE(sim_.is_after_hours());
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);
}

TEST_F(KrxSimulatorTest, AfterHoursOrdersIsolatedFromRegular) {
    sim_.load_products(get_krx_products());

    // Regular session: place a resting order.
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);
    sim_.new_order(1, make_limit(1, 1, Side::Buy, 3500000, 10000, TimeInForce::GTC, 300));
    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 1u);

    sim_.pre_close_regular(400);
    sim_.close_regular_session(500);

    // After-hours: place a different resting order.
    sim_.start_after_hours(600);
    sim_.new_order(1, make_limit(2, 2, Side::Sell, 3500000, 10000, TimeInForce::GTC, 700));

    // After-hours engine should have 1 order.
    EXPECT_EQ(sim_.get_after_hours_engine(1)->active_order_count(), 1u);
    // Regular engine still has its 1 order (isolated).
    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 1u);
}

TEST_F(KrxSimulatorTest, AfterHoursTradeDoesNotAffectRegular) {
    sim_.load_products(get_krx_products());

    sim_.start_regular_session(100);
    sim_.open_regular_market(200);
    sim_.pre_close_regular(300);
    sim_.close_regular_session(400);

    sim_.start_after_hours(500);

    // Trade in after-hours.
    sim_.new_order(1, make_limit(1, 1, Side::Sell, 3500000, 10000, TimeInForce::GTC, 600));
    sim_.new_order(1, make_limit(2, 2, Side::Buy, 3500000, 10000, TimeInForce::GTC, 601));

    // After-hours orders should have matched.
    EXPECT_EQ(sim_.get_after_hours_engine(1)->active_order_count(), 0u);
    // Regular engine unaffected.
    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 0u);
}

// ===========================================================================
// Price configuration
// ===========================================================================

TEST_F(KrxSimulatorTest, SetPricesConfiguresBothSessions) {
    sim_.load_products(get_krx_products());
    sim_.set_prices(1, 3500000, 3480000);

    auto* reg = sim_.get_regular_engine(1);
    ASSERT_NE(reg, nullptr);
    EXPECT_EQ(reg->reference_price(), 3500000);
    EXPECT_EQ(reg->prior_close_price(), 3480000);
    EXPECT_EQ(reg->current_limit_tier(), 1);

    auto* ah = sim_.get_after_hours_engine(1);
    ASSERT_NE(ah, nullptr);
    EXPECT_EQ(ah->reference_price(), 3500000);
    EXPECT_EQ(ah->prior_close_price(), 3480000);
}

TEST_F(KrxSimulatorTest, SetAllPricesConfiguresEveryInstrument) {
    sim_.load_products(get_krx_products());
    sim_.set_all_prices(5000000, 4990000);

    auto* ks = sim_.get_regular_engine(1);
    ASSERT_NE(ks, nullptr);
    EXPECT_EQ(ks->reference_price(), 5000000);

    auto* ktb = sim_.get_regular_engine(9);
    ASSERT_NE(ktb, nullptr);
    EXPECT_EQ(ktb->reference_price(), 5000000);
}

// ===========================================================================
// Sidecar
// ===========================================================================

TEST_F(KrxSimulatorTest, SidecarAffectsAllRegularEngines) {
    sim_.load_products(get_krx_products());
    sim_.activate_sidecar();

    EXPECT_TRUE(sim_.get_regular_engine(1)->sidecar_active());
    EXPECT_TRUE(sim_.get_regular_engine(9)->sidecar_active());

    sim_.deactivate_sidecar();
    EXPECT_FALSE(sim_.get_regular_engine(1)->sidecar_active());
}

// ===========================================================================
// End of day
// ===========================================================================

TEST_F(KrxSimulatorTest, EndOfDayExpiresDayOrders) {
    sim_.load_products(get_krx_products());
    sim_.start_regular_session(100);
    sim_.open_regular_market(200);

    sim_.new_order(1, make_limit(1, 10, Side::Buy, 3500000, 10000, TimeInForce::DAY, 300));
    sim_.new_order(1, make_limit(2, 11, Side::Buy, 3500000, 10000, TimeInForce::GTC, 301));
    EXPECT_EQ(sim_.get_regular_engine(1)->active_order_count(), 2u);

    sim_.pre_close_regular(400);
    sim_.close_regular_session(500);
    sim_.end_of_day(600);

    bool saw_expired = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) {
            if (std::get<OrderCancelled>(ev).reason == CancelReason::Expired)
                saw_expired = true;
        }
    }
    EXPECT_TRUE(saw_expired) << "DAY order must be expired";
}

}  // namespace
}  // namespace krx
}  // namespace exchange
