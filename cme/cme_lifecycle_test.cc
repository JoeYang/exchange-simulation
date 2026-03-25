#include "cme/cme_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace cme {
namespace {

using TestCmeSimulator =
    CmeSimulator<RecordingOrderListener, RecordingMdListener>;

// ---------------------------------------------------------------------------
// Test fixture -- single ES instrument
// ---------------------------------------------------------------------------

class CmeLifecycleTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestCmeSimulator sim_{order_listener_, md_listener_};

    static constexpr InstrumentId ES_ID = 1;

    void SetUp() override {
        sim_.load_products({CmeProductConfig{
            ES_ID, "ES", "E-mini S&P 500", "Equity Index",
            /*tick_size=*/2500, /*lot_size=*/10000,
            /*max_order=*/10000 * 2000, /*band_pct=*/5}});
    }

    OrderRequest make_limit(uint64_t cl_id, uint64_t account_id, Side side,
                            Price price, Quantity qty,
                            TimeInForce tif = TimeInForce::GTC,
                            Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id, .account_id = account_id,
            .side = side, .type = OrderType::Limit, .tif = tif,
            .price = price, .quantity = qty, .stop_price = 0,
            .timestamp = ts, .gtd_expiry = 0,
        };
    }

    OrderRequest make_stop(uint64_t cl_id, uint64_t account_id, Side side,
                           Price stop_price, Quantity qty,
                           Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id, .account_id = account_id,
            .side = side, .type = OrderType::Stop,
            .tif = TimeInForce::GTC, .price = 0, .quantity = qty,
            .stop_price = stop_price, .timestamp = ts, .gtd_expiry = 0,
        };
    }

    template <typename T>
    size_t count_events() const {
        size_t n = 0;
        for (const auto& ev : order_listener_.events())
            if (std::holds_alternative<T>(ev)) ++n;
        return n;
    }

    template <typename T>
    bool has_event_with(
        std::function<bool(const T&)> pred = nullptr) const {
        for (const auto& ev : order_listener_.events()) {
            if (std::holds_alternative<T>(ev)) {
                if (!pred || pred(std::get<T>(ev))) return true;
            }
        }
        return false;
    }
};

// ===========================================================================
// Full trading day lifecycle for ES
// ===========================================================================

TEST_F(CmeLifecycleTest, FullTradingDay) {
    // 1. PreOpen -- order collection, no matching
    sim_.start_trading_day(1000);
    EXPECT_EQ(sim_.session_state(), SessionState::PreOpen);

    // Crossing limit orders: Buy@5001, Sell@5000 -- no match in PreOpen.
    sim_.new_order(ES_ID, make_limit(1, 10, Side::Buy,  50010000, 10000, TimeInForce::GTC, 1001));
    sim_.new_order(ES_ID, make_limit(2, 11, Side::Sell, 50000000, 10000, TimeInForce::GTC, 1002));
    EXPECT_EQ(sim_.get_engine(ES_ID)->active_order_count(), 2u);
    EXPECT_EQ(count_events<OrderFilled>(), 0u) << "No fills during PreOpen";

    // 2. IOC rejected in PreOpen
    order_listener_.clear();
    sim_.new_order(ES_ID, make_limit(3, 12, Side::Buy, 50000000, 10000, TimeInForce::IOC, 1003));
    EXPECT_TRUE(has_event_with<OrderRejected>()) << "IOC must be rejected during PreOpen";

    // 3. Opening auction -- uncross, transition to Continuous
    order_listener_.clear();
    md_listener_.clear();
    sim_.open_market(2000);
    EXPECT_EQ(sim_.session_state(), SessionState::Continuous);
    EXPECT_EQ(sim_.get_engine(ES_ID)->active_order_count(), 0u)
        << "Crossing orders must be filled by opening auction";

    bool saw_auction_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            EXPECT_EQ(std::get<Trade>(ev).quantity, 10000);
            saw_auction_trade = true;
        }
    }
    EXPECT_TRUE(saw_auction_trade) << "Auction must produce a trade";

    // 4. Continuous trading -- normal matching
    order_listener_.clear();
    md_listener_.clear();
    sim_.new_order(ES_ID, make_limit(10, 20, Side::Sell, 50050000, 20000, TimeInForce::GTC, 3000));
    sim_.new_order(ES_ID, make_limit(11, 21, Side::Buy,  50050000, 10000, TimeInForce::GTC, 3001));
    EXPECT_EQ(sim_.get_engine(ES_ID)->active_order_count(), 1u);

    bool saw_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev)) {
            auto& t = std::get<Trade>(ev);
            EXPECT_EQ(t.price, 50050000);
            EXPECT_EQ(t.quantity, 10000);
            saw_trade = true;
        }
    }
    EXPECT_TRUE(saw_trade) << "Continuous matching must produce a trade";

    // 5. SMP -- same account crossing cancelled
    order_listener_.clear();
    md_listener_.clear();
    sim_.new_order(ES_ID, make_limit(12, 20, Side::Buy, 50050000, 10000, TimeInForce::GTC, 3100));

    EXPECT_TRUE(has_event_with<OrderCancelled>(
        [](const OrderCancelled& c) {
            return c.reason == CancelReason::SelfMatchPrevention;
        })) << "Same-account crossing must trigger SMP cancel";
    for (const auto& ev : md_listener_.events())
        EXPECT_FALSE(std::holds_alternative<Trade>(ev)) << "SMP must prevent trade";

    // 6. Stop order trigger
    order_listener_.clear();
    md_listener_.clear();
    sim_.get_engine(ES_ID)->mass_cancel_all(3200);
    order_listener_.clear();
    md_listener_.clear();

    sim_.new_order(ES_ID, make_limit(20, 30, Side::Sell, 50100000, 10000, TimeInForce::GTC, 3300));
    sim_.new_order(ES_ID, make_stop(21, 31, Side::Buy, 50100000, 10000, 3301));
    sim_.new_order(ES_ID, make_limit(22, 32, Side::Buy, 50100000, 10000, TimeInForce::GTC, 3302));

    bool saw_trigger_trade = false;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<Trade>(ev) &&
            std::get<Trade>(ev).price == 50100000)
            saw_trigger_trade = true;
    }
    EXPECT_TRUE(saw_trigger_trade) << "Trade at stop price must trigger the stop";

    // 7. Close market
    order_listener_.clear();
    md_listener_.clear();
    sim_.close_market(5000);
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    int preclose_count = 0, closed_count = 0;
    for (const auto& ev : md_listener_.events()) {
        if (std::holds_alternative<MarketStatus>(ev)) {
            auto& ms = std::get<MarketStatus>(ev);
            if (ms.state == SessionState::PreClose) ++preclose_count;
            if (ms.state == SessionState::Closed) ++closed_count;
        }
    }
    EXPECT_GE(preclose_count, 1) << "PreClose status must be published";
    EXPECT_GE(closed_count, 1) << "Closed status must be published";
}

// ===========================================================================
// DAY vs GTC order expiry at end of day
// ===========================================================================

TEST_F(CmeLifecycleTest, DayOrdersExpiredGtcSurvives) {
    sim_.start_trading_day(1000);
    sim_.open_market(2000);

    sim_.new_order(ES_ID, make_limit(1, 10, Side::Buy, 49000000, 10000, TimeInForce::DAY, 3000));
    sim_.new_order(ES_ID, make_limit(2, 11, Side::Buy, 49000000, 10000, TimeInForce::GTC, 3001));
    EXPECT_EQ(sim_.get_engine(ES_ID)->active_order_count(), 2u);

    sim_.close_market(4000);
    order_listener_.clear();
    sim_.end_of_day(5000);

    EXPECT_TRUE(has_event_with<OrderCancelled>(
        [](const OrderCancelled& c) { return c.reason == CancelReason::Expired; }))
        << "DAY order must expire at end of day";
}

// ===========================================================================
// OHLCV stats across a session
// ===========================================================================

TEST_F(CmeLifecycleTest, OhlcvTracksSessionTrades) {
    sim_.start_trading_day(1000);
    sim_.open_market(2000);

    OhlcvStats* ohlcv = sim_.get_ohlcv(ES_ID);
    ASSERT_NE(ohlcv, nullptr);

    // Execute trades and manually update OHLCV.
    // (ExchangeSimulator provides storage; the app feeds Trade events.)
    auto trade_at = [&](Price price, uint64_t base_id, Timestamp ts) {
        md_listener_.clear();
        sim_.new_order(ES_ID, make_limit(base_id,   base_id,   Side::Sell, price, 10000, TimeInForce::GTC, ts));
        sim_.new_order(ES_ID, make_limit(base_id+1, base_id+1, Side::Buy,  price, 10000, TimeInForce::GTC, ts+1));
        for (const auto& ev : md_listener_.events())
            if (std::holds_alternative<Trade>(ev)) {
                auto& t = std::get<Trade>(ev);
                ohlcv->on_trade(t.price, t.quantity);
            }
    };

    trade_at(50000000, 100, 3000);  // open
    trade_at(50050000, 200, 3010);  // high
    trade_at(49975000, 300, 3020);  // low + close

    EXPECT_TRUE(ohlcv->has_traded);
    EXPECT_EQ(ohlcv->open,  50000000);
    EXPECT_EQ(ohlcv->high,  50050000);
    EXPECT_EQ(ohlcv->low,   49975000);
    EXPECT_EQ(ohlcv->close, 49975000);
    EXPECT_EQ(ohlcv->volume, 30000);
    EXPECT_EQ(ohlcv->trade_count, 3u);
}

// ===========================================================================
// Orders rejected when market is Closed
// ===========================================================================

TEST_F(CmeLifecycleTest, OrdersRejectedWhenClosed) {
    // Cycle through so the engine reaches Closed state.
    sim_.start_trading_day(100);
    sim_.open_market(200);
    sim_.close_market(300);
    EXPECT_EQ(sim_.session_state(), SessionState::Closed);

    order_listener_.clear();
    sim_.new_order(ES_ID, make_limit(1, 10, Side::Buy, 50000000, 10000, TimeInForce::GTC, 400));
    EXPECT_TRUE(has_event_with<OrderRejected>())
        << "Orders must be rejected when session is Closed";
}

// ===========================================================================
// PreOpen allows GTC but prevents matching
// ===========================================================================

TEST_F(CmeLifecycleTest, PreOpenAllowsGtcNoMatching) {
    sim_.start_trading_day(1000);

    sim_.new_order(ES_ID, make_limit(1, 10, Side::Buy,  50000000, 10000, TimeInForce::GTC, 1001));
    sim_.new_order(ES_ID, make_limit(2, 11, Side::Sell, 50000000, 10000, TimeInForce::GTC, 1002));

    EXPECT_EQ(sim_.get_engine(ES_ID)->active_order_count(), 2u);
    bool saw_trade = false;
    for (const auto& ev : md_listener_.events())
        if (std::holds_alternative<Trade>(ev)) saw_trade = true;
    EXPECT_FALSE(saw_trade) << "No trades during PreOpen";
}

}  // namespace
}  // namespace cme
}  // namespace exchange
