#include "ice/ice_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace ice {
namespace {

using TestIceSimulator =
    IceSimulator<RecordingOrderListener, RecordingMdListener>;

class IceSimulatorTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestIceSimulator sim_{order_listener_, md_listener_};

    OrderRequest make_limit(uint64_t cl_id, uint64_t account_id,
                            Side side, Price price, Quantity qty,
                            TimeInForce tif = TimeInForce::GTC,
                            Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id      = account_id,
            .side            = side,
            .type            = OrderType::Limit,
            .tif             = tif,
            .price           = price,
            .quantity        = qty,
            .stop_price      = 0,
            .timestamp       = ts,
            .gtd_expiry      = 0,
        };
    }
};

// ===========================================================================
// Product loading
// ===========================================================================

TEST_F(IceSimulatorTest, LoadAllIceProducts) {
    sim_.load_products(get_ice_products());
    EXPECT_EQ(sim_.instrument_count(), 10u);
}

TEST_F(IceSimulatorTest, StirProductsRouteToGtbpr) {
    sim_.load_products(get_ice_products());

    // Euribor (id=7) and SONIA (id=8) are GTBPR.
    EXPECT_TRUE(sim_.is_gtbpr_instrument(7));
    EXPECT_TRUE(sim_.is_gtbpr_instrument(8));

    // Brent (id=1) is FIFO.
    EXPECT_FALSE(sim_.is_gtbpr_instrument(1));
}

TEST_F(IceSimulatorTest, LoadedProductsHaveCorrectIplWidth) {
    sim_.load_products(get_ice_products());

    // Brent (id=1): ipl_width = 10000
    auto* brent = sim_.get_fifo_engine(1);
    ASSERT_NE(brent, nullptr);
    EXPECT_EQ(brent->ipl_width(), 10000);

    // Euribor (id=7): ipl_width = 500
    auto* euribor = sim_.get_gtbpr_engine(7);
    ASSERT_NE(euribor, nullptr);
    EXPECT_EQ(euribor->ipl_width(), 500);
}

// ===========================================================================
// Order routing
// ===========================================================================

TEST_F(IceSimulatorTest, RouteOrderToFifoProduct) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Brent (id=1): tick=100, lot=10000, price=$80.00 = 800000
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 800000, 10000));

    bool saw_accepted = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(ev)) saw_accepted = true;
    }
    EXPECT_TRUE(saw_accepted);
}

TEST_F(IceSimulatorTest, RouteOrderToGtbprProduct) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Euribor (id=7): tick=50, lot=10000, price=96.000 = 960000
    sim_.new_order(7, make_limit(1, 10, Side::Buy, 960000, 10000));

    bool saw_accepted = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderAccepted>(ev)) saw_accepted = true;
    }
    EXPECT_TRUE(saw_accepted);
}

TEST_F(IceSimulatorTest, RouteToUnknownInstrumentThrows) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    EXPECT_THROW(
        sim_.new_order(999, make_limit(1, 10, Side::Buy, 100000, 10000)),
        std::runtime_error);
}

// ===========================================================================
// Cross-instrument isolation
// ===========================================================================

TEST_F(IceSimulatorTest, CrossInstrumentIsolation) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Place a sell on Brent (id=1).
    sim_.new_order(1, make_limit(1, 10, Side::Sell, 800000, 10000));
    order_listener_.clear();

    // Place a buy on Cocoa (id=4) at a crossing price — should NOT match
    // the Brent sell.
    sim_.new_order(4, make_limit(2, 20, Side::Buy, 800000, 10000));

    // Cocoa order should be accepted and rest (no fill).
    bool saw_fill = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) saw_fill = true;
    }
    EXPECT_FALSE(saw_fill);
}

TEST_F(IceSimulatorTest, CrossAlgoIsolation) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Sell on Brent (FIFO, id=1).
    sim_.new_order(1, make_limit(1, 10, Side::Sell, 800000, 10000));
    order_listener_.clear();

    // Buy on Euribor (GTBPR, id=7) — different algo pool entirely.
    sim_.new_order(7, make_limit(2, 20, Side::Buy, 800000, 10000));

    bool saw_fill = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderFilled>(ev)) saw_fill = true;
    }
    EXPECT_FALSE(saw_fill);
}

// ===========================================================================
// Session lifecycle
// ===========================================================================

TEST_F(IceSimulatorTest, SessionLifecycleFull) {
    sim_.load_products(get_ice_products());

    // Start: Closed (default).
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    // PreOpen.
    sim_.start_trading_day(100);
    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);

    // Continuous.
    sim_.open_market(200);
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);

    // Close.
    sim_.close_market(300);
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);
}

TEST_F(IceSimulatorTest, PreOpenRejectsMarketOrder) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);

    OrderRequest market_req{
        .client_order_id = 1,
        .account_id      = 10,
        .side            = Side::Buy,
        .type            = OrderType::Market,
        .tif             = TimeInForce::GTC,
        .price           = 0,
        .quantity        = 10000,
        .stop_price      = 0,
        .timestamp       = 200,
        .gtd_expiry      = 0,
    };
    sim_.new_order(1, market_req);

    bool rejected = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderRejected>(ev)) rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST_F(IceSimulatorTest, EndOfDayExpiresDayOrders) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    // Place a DAY order on Brent.
    sim_.new_order(1, make_limit(1, 10, Side::Buy, 800000, 10000,
                                 TimeInForce::DAY, 300));
    order_listener_.clear();

    sim_.close_market(400);
    sim_.end_of_day(500);

    bool saw_cancel = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) {
            auto& c = std::get<OrderCancelled>(ev);
            if (c.reason == CancelReason::Expired) saw_cancel = true;
        }
    }
    EXPECT_TRUE(saw_cancel);
}

// ===========================================================================
// Cancel and modify routing
// ===========================================================================

TEST_F(IceSimulatorTest, CancelOrderRoutes) {
    sim_.load_products(get_ice_products());
    sim_.start_trading_day(100);
    sim_.open_market(200);

    sim_.new_order(1, make_limit(1, 10, Side::Buy, 800000, 10000,
                                 TimeInForce::GTC, 300));
    order_listener_.clear();

    // Cancel the order (id=1 on instrument 1).
    sim_.cancel_order(1, 1, 400);

    bool saw_cancel = false;
    for (const auto& ev : order_listener_.events()) {
        if (std::holds_alternative<OrderCancelled>(ev)) saw_cancel = true;
    }
    EXPECT_TRUE(saw_cancel);
}

TEST_F(IceSimulatorTest, CancelOnUnknownInstrumentThrows) {
    sim_.load_products(get_ice_products());
    EXPECT_THROW(sim_.cancel_order(999, 1, 100), std::runtime_error);
}

TEST_F(IceSimulatorTest, ModifyOnUnknownInstrumentThrows) {
    sim_.load_products(get_ice_products());
    ModifyRequest req{.order_id = 1, .client_order_id = 2,
                      .new_price = 810000, .new_quantity = 10000,
                      .timestamp = 100};
    EXPECT_THROW(sim_.modify_order(999, req), std::runtime_error);
}

}  // namespace
}  // namespace ice
}  // namespace exchange
