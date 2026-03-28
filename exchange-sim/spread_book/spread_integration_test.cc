#include "exchange-sim/exchange_simulator.h"
#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "exchange-sim/spread_book/spread_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Minimal CRTP engine for testing (same as exchange_simulator_test.cc).
class TestEngine
    : public MatchingEngine<TestEngine, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestEngine, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;

    // Expose book for SpreadSimulator BBO access.
    const OrderBook& book() const { return book_; }
};

using TestSimulator =
    ExchangeSimulator<TestEngine, RecordingOrderListener, RecordingMdListener>;
using TestSpreadSim = SpreadSimulator<TestEngine, RecordingOrderListener>;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

InstrumentConfig make_instrument(InstrumentId id, const std::string& symbol,
                                 Price tick = 2500, Quantity lot = 10000) {
    return InstrumentConfig{
        .id = id,
        .symbol = symbol,
        .engine_config = EngineConfig{.tick_size = tick,
                                      .lot_size = lot,
                                      .price_band_low = 0,
                                      .price_band_high = 0}};
}

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

SpreadInstrumentConfig make_calendar_config(
    uint32_t spread_id, uint32_t front_id, uint32_t back_id,
    Price tick = 2500, Quantity lot = 10000) {
    return SpreadInstrumentConfig{
        .id = spread_id,
        .symbol = "CAL",
        .strategy_type = StrategyType::CalendarSpread,
        .legs = {
            StrategyLeg{.instrument_id = front_id, .ratio = 1,
                        .price_multiplier = 1,
                        .tick_size = tick, .lot_size = lot},
            StrategyLeg{.instrument_id = back_id, .ratio = -1,
                        .price_multiplier = 1,
                        .tick_size = tick, .lot_size = lot},
        },
    };
}

// ---------------------------------------------------------------------------
// Integration fixture
// ---------------------------------------------------------------------------

class SpreadIntegrationTest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestSimulator sim_{order_listener_, md_listener_};
    std::unique_ptr<TestSpreadSim> spread_sim_;

    void SetUp() override {
        sim_.set_session_state(SessionState::Continuous, 0);
        // Add two outright instruments: front month (1) and back month (2).
        sim_.add_instrument(make_instrument(1, "ESU5", 2500, 10000));
        sim_.add_instrument(make_instrument(2, "ESZ5", 2500, 10000));

        // Create SpreadSimulator with engine lookup.
        spread_sim_ = std::make_unique<TestSpreadSim>(
            [this](uint32_t id) -> TestEngine* {
                return sim_.get_engine(id);
            },
            order_listener_);

        // Register calendar spread (ID=100): ESU5 (+1) - ESZ5 (-1).
        spread_sim_->add_spread_instrument(
            make_calendar_config(100, 1, 2, 2500, 10000));
    }
};

// ---------------------------------------------------------------------------
// SpreadSimulator lifecycle tests
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, SpreadInstrumentRegistered) {
    EXPECT_EQ(spread_sim_->spread_count(), 1u);
    EXPECT_TRUE(spread_sim_->is_spread(100));
    EXPECT_FALSE(spread_sim_->is_spread(1));
}

TEST_F(SpreadIntegrationTest, SpreadBookCreated) {
    auto* book = spread_sim_->get_spread_book(100);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->spread_instrument_id(), 100u);
}

// ---------------------------------------------------------------------------
// Direct spread order routing
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, SpreadOrderAccepted) {
    order_listener_.clear();

    SpreadOrderRequest req{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000};
    OrderId id = spread_sim_->new_spread_order(100, req);
    EXPECT_NE(id, 0u);
}

TEST_F(SpreadIntegrationTest, SpreadOrderCancelled) {
    SpreadOrderRequest req{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000};
    OrderId id = spread_sim_->new_spread_order(100, req);

    bool ok = spread_sim_->cancel_spread_order(100,
        SpreadCancelRequest{.order_id = id, .timestamp = 2000});
    EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// Implied-out: outright BBO change triggers spread fill
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, ImpliedOutCalendarSpread) {
    // 1. Place resting spread sell at -5000 (sell front-back spread at -0.50).
    order_listener_.clear();
    SpreadOrderRequest spread_sell{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000};
    OrderId spread_id = spread_sim_->new_spread_order(100, spread_sell);
    ASSERT_NE(spread_id, 0u);

    // 2. Place outright orders that create an implied spread bid of -5000.
    // Front month: bid at 1000000 (100.0000).
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 10000, 2000));
    // Back month: ask at 1005000 (100.5000).
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 10000, 2000));

    order_listener_.clear();

    // 3. Trigger implied matching.
    // Implied spread bid = front_bid - back_ask = 1000000 - 1005000 = -5000.
    // This crosses the resting spread sell at -5000.
    int fills = spread_sim_->on_outright_bbo_change(1, 3000);
    EXPECT_GE(fills, 1);

    // 4. Verify the spread order was filled.
    auto* book = spread_sim_->get_spread_book(100);
    ASSERT_NE(book, nullptr);
    // Spread order should be fully filled (removed from book).
    EXPECT_EQ(book->get_order(spread_id), nullptr);
}

// ---------------------------------------------------------------------------
// Implied-in: spread order triggers outright fills
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, ImpliedInCalendarSpread) {
    // 1. Place outright liquidity that a spread order can take.
    // Front month: ask at 1000000 (we can buy front at this price).
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    // Back month: bid at 1005000 (we can sell back at this price).
    sim_.new_order(2, make_limit(30, Side::Buy, 1005000, 10000, 1000));

    order_listener_.clear();

    // 2. Place spread buy at -5000.
    // To buy spread: buy front, sell back.
    // Actual spread = front_ask(1000000) - back_bid(1005000) = -5000.
    // Resting buy at -5000: actual(-5000) <= resting(-5000), so should fill.
    SpreadOrderRequest spread_buy{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 2000};
    OrderId spread_id = spread_sim_->new_spread_order(100, spread_buy);
    ASSERT_NE(spread_id, 0u);

    // 3. Verify spread order was filled via implied-in.
    auto* book = spread_sim_->get_spread_book(100);
    ASSERT_NE(book, nullptr);
    EXPECT_EQ(book->get_order(spread_id), nullptr);

    // 4. Verify outright orders were consumed.
    auto* front_engine = sim_.get_engine(1);
    auto* back_engine = sim_.get_engine(2);
    // Both outright orders should be fully filled (no resting liquidity).
    EXPECT_EQ(front_engine->best_order_id(Side::Sell), std::nullopt);
    EXPECT_EQ(back_engine->best_order_id(Side::Buy), std::nullopt);
}

// ---------------------------------------------------------------------------
// No crossing: spread order rests without implied fill
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, NoCrossingSpreadRests) {
    // Outright liquidity: spread would be -5000 but order wants -7500.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    sim_.new_order(2, make_limit(30, Side::Buy, 1005000, 10000, 1000));

    // Spread buy at -7500 (wants more negative = cheaper).
    // Actual spread = -5000, which is > -7500 (not favorable for buyer).
    SpreadOrderRequest req{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -7500, .quantity = 10000,
        .timestamp = 2000};
    OrderId id = spread_sim_->new_spread_order(100, req);

    auto* book = spread_sim_->get_spread_book(100);
    // Order should still be resting (not filled).
    EXPECT_NE(book->get_order(id), nullptr);
}

// ---------------------------------------------------------------------------
// Multi-spread scenario: two spreads share a leg
// ---------------------------------------------------------------------------

TEST_F(SpreadIntegrationTest, MultiSpreadSharedLeg) {
    // Add a third outright: ESH6 (ID=3).
    sim_.add_instrument(make_instrument(3, "ESH6", 2500, 10000));

    // Add second spread: ESU5-ESH6 (ID=101).
    spread_sim_->add_spread_instrument(
        make_calendar_config(101, 1, 3, 2500, 10000));

    EXPECT_EQ(spread_sim_->spread_count(), 2u);
    EXPECT_TRUE(spread_sim_->is_spread(101));

    // Place spread orders on both books.
    SpreadOrderRequest req1{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000};
    EXPECT_NE(spread_sim_->new_spread_order(100, req1), 0u);

    SpreadOrderRequest req2{
        .client_order_id = 11, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -2500, .quantity = 10000,
        .timestamp = 1000};
    EXPECT_NE(spread_sim_->new_spread_order(101, req2), 0u);
}

}  // namespace
}  // namespace exchange
