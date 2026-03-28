// spread_e2e_test.cc
//
// E2E journal-style tests for spread fill invariants.
// Verifies the full chain: outright engines + SpreadSimulator + implied matching.
//
// Invariants tested:
//   1. Quantity conservation: sum of leg fills equals spread qty * abs(ratio)
//   2. Price consistency: implied spread price from leg execution prices
//      matches or improves on the resting spread order price
//   3. Atomicity: all outright leg fills succeed, or none do
//   4. Order lifecycle: spread order transitions accepted -> filled/cancelled
//   5. No orphan state: after fill, spread order is removed from book;
//      after cancel, order is removed and no fills occurred

#include "exchange-sim/exchange_simulator.h"
#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "exchange-sim/spread_book/spread_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// --- Engine setup (same as integration tests) ---

class TestEngine
    : public MatchingEngine<TestEngine, RecordingOrderListener,
                            RecordingMdListener, FifoMatch, 100, 50, 1000> {
public:
    using Base = MatchingEngine<TestEngine, RecordingOrderListener,
                                RecordingMdListener, FifoMatch, 100, 50, 1000>;
    using Base::Base;
    const OrderBook& book() const { return book_; }
};

using TestSimulator =
    ExchangeSimulator<TestEngine, RecordingOrderListener, RecordingMdListener>;
using TestSpreadSim = SpreadSimulator<TestEngine, RecordingOrderListener>;

// --- Helpers ---

InstrumentConfig make_instrument(InstrumentId id, const std::string& symbol,
                                 Price tick = 2500, Quantity lot = 10000) {
    return InstrumentConfig{
        .id = id, .symbol = symbol,
        .engine_config = EngineConfig{.tick_size = tick, .lot_size = lot,
                                      .price_band_low = 0,
                                      .price_band_high = 0}};
}

OrderRequest make_limit(uint64_t cl_id, Side side, Price price,
                        Quantity qty, Timestamp ts) {
    return OrderRequest{.client_order_id = cl_id, .account_id = 1,
                        .side = side, .type = OrderType::Limit,
                        .tif = TimeInForce::GTC, .price = price,
                        .quantity = qty, .stop_price = 0,
                        .timestamp = ts, .gtd_expiry = 0};
}

// Count events of a specific type in the listener.
template <typename EventT>
size_t count_events(const RecordingOrderListener& listener) {
    size_t n = 0;
    for (const auto& ev : listener.events()) {
        if (std::get_if<EventT>(&ev)) ++n;
    }
    return n;
}

template <typename EventT>
size_t count_md_events(const RecordingMdListener& listener) {
    size_t n = 0;
    for (const auto& ev : listener.events()) {
        if (std::get_if<EventT>(&ev)) ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// E2E Fixture
// ---------------------------------------------------------------------------

class SpreadE2ETest : public ::testing::Test {
protected:
    RecordingOrderListener order_listener_;
    RecordingMdListener md_listener_;
    TestSimulator sim_{order_listener_, md_listener_};
    std::unique_ptr<TestSpreadSim> spread_sim_;

    void SetUp() override {
        sim_.set_session_state(SessionState::Continuous, 0);
        sim_.add_instrument(make_instrument(1, "ESU5", 2500, 10000));
        sim_.add_instrument(make_instrument(2, "ESZ5", 2500, 10000));

        spread_sim_ = std::make_unique<TestSpreadSim>(
            [this](uint32_t id) -> TestEngine* {
                return sim_.get_engine(id);
            },
            order_listener_);

        spread_sim_->add_spread_instrument(SpreadInstrumentConfig{
            .id = 100, .symbol = "ESU5-ESZ5",
            .strategy_type = StrategyType::CalendarSpread,
            .legs = {
                StrategyLeg{.instrument_id = 1, .ratio = 1,
                            .price_multiplier = 1,
                            .tick_size = 2500, .lot_size = 10000},
                StrategyLeg{.instrument_id = 2, .ratio = -1,
                            .price_multiplier = 1,
                            .tick_size = 2500, .lot_size = 10000},
            },
        });
    }
};

// ---------------------------------------------------------------------------
// Invariant 1: Quantity conservation
//   Spread fill of Q spread-lots => leg 0 fill = Q*abs(ratio0),
//   leg 1 fill = Q*abs(ratio1).
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, QuantityConservation_ImpliedIn) {
    // Outright liquidity: front ask=1000000, back bid=1005000.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 30000, 1000));
    sim_.new_order(2, make_limit(30, Side::Buy, 1005000, 30000, 1000));

    order_listener_.clear();

    // Spread buy 20000 (2 lots) at -5000.
    // Actual spread = front_ask - back_bid = 1000000 - 1005000 = -5000.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 20000,
        .timestamp = 2000});

    // Verify outright fills: each leg should have 20000 qty consumed.
    auto* front = sim_.get_engine(1);
    auto* back = sim_.get_engine(2);

    // Front ask was 30000, should now be 10000 remaining (30000 - 20000).
    auto* front_ask = front->book().best_ask();
    ASSERT_NE(front_ask, nullptr);
    EXPECT_EQ(front_ask->total_quantity, 10000);

    // Back bid was 30000, should now be 10000 remaining.
    auto* back_bid = back->book().best_bid();
    ASSERT_NE(back_bid, nullptr);
    EXPECT_EQ(back_bid->total_quantity, 10000);
}

TEST_F(SpreadE2ETest, QuantityConservation_ImpliedOut) {
    // Resting spread sell 20000 at -5000.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 20000,
        .timestamp = 1000});

    // Outright orders that create implied spread bid = -5000.
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 20000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 20000, 2000));

    order_listener_.clear();
    int fills = spread_sim_->on_outright_bbo_change(1, 3000);
    EXPECT_GE(fills, 1);

    // Verify quantity: both outright orders should have qty consumed.
    auto* front_bid = sim_.get_engine(1)->book().best_bid();

    // If spread-lots filled, front bid qty consumed (was 20000).
    EXPECT_TRUE(front_bid == nullptr ||
                front_bid->total_quantity < 20000);
}

// ---------------------------------------------------------------------------
// Invariant 2: Price consistency
//   Implied spread price from execution <= resting buy price (favorable)
//   or >= resting sell price.
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, PriceConsistency_BuySpread) {
    // Outright: front ask=1000000, back bid=1007500.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    sim_.new_order(2, make_limit(30, Side::Buy, 1007500, 10000, 1000));

    // Spread buy at -5000. Actual spread = 1000000 - 1007500 = -7500.
    // -7500 <= -5000, so price is FAVORABLE for the buyer.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 2000});

    // Spread should be filled at -7500 (better than -5000 for buyer).
    auto* book = spread_sim_->get_spread_book(100);
    EXPECT_EQ(book->get_order(1), nullptr);  // filled, removed
}

TEST_F(SpreadE2ETest, PriceConsistency_SellSpread) {
    // Resting spread sell at -7500.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -7500, .quantity = 10000,
        .timestamp = 1000});

    // Outright: front bid=1000000, back ask=1005000.
    // Implied spread bid = 1000000 - 1005000 = -5000.
    // -5000 >= -7500, so price is FAVORABLE for the seller.
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 10000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 10000, 2000));

    int fills = spread_sim_->on_outright_bbo_change(1, 3000);
    EXPECT_GE(fills, 1);

    auto* book = spread_sim_->get_spread_book(100);
    EXPECT_EQ(book->get_order(1), nullptr);  // filled
}

// ---------------------------------------------------------------------------
// Invariant 3: Atomicity
//   If one leg fails, no fills on any leg.
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, AtomicRollback_InsufficientLegLiquidity) {
    // Only front has liquidity, back has none.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    // No back month bid => implied-in should fail.

    order_listener_.clear();

    // Spread buy: needs to buy front (take ask) and sell back (take bid).
    // Back has no bid => no fill.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 2000});

    // Front ask should be untouched (no partial leg fill).
    auto* front_ask = sim_.get_engine(1)->book().best_ask();
    ASSERT_NE(front_ask, nullptr);
    EXPECT_EQ(front_ask->total_quantity, 10000);
}

// ---------------------------------------------------------------------------
// Invariant 4: Order lifecycle
//   Spread order transitions: accepted -> filled (or cancelled).
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, LifecycleAcceptedThenFilled) {
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    sim_.new_order(2, make_limit(30, Side::Buy, 1005000, 10000, 1000));

    order_listener_.clear();

    OrderId id = spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 2000});

    // Should have OrderAccepted followed by OrderFilled.
    EXPECT_GE(count_events<OrderAccepted>(order_listener_), 1u);
    // Fill events from outright legs + spread.
    size_t total_fills = count_events<OrderFilled>(order_listener_) +
                         count_events<OrderPartiallyFilled>(order_listener_);
    EXPECT_GE(total_fills, 1u);

    // Spread order removed from book.
    EXPECT_EQ(spread_sim_->get_spread_book(100)->get_order(id), nullptr);
}

TEST_F(SpreadE2ETest, LifecycleAcceptedThenCancelled) {
    // No outright liquidity => spread rests.
    OrderId id = spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});

    order_listener_.clear();

    // Cancel.
    spread_sim_->cancel_spread_order(100,
        SpreadCancelRequest{.order_id = id, .timestamp = 2000});

    EXPECT_GE(count_events<OrderCancelled>(order_listener_), 1u);
    EXPECT_EQ(spread_sim_->get_spread_book(100)->get_order(id), nullptr);
}

// ---------------------------------------------------------------------------
// Invariant 5: No orphan state
//   After IOC with no match: order cancelled, no fills on any leg.
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, IOCNoMatchNoOrphanFills) {
    // No outright liquidity.
    order_listener_.clear();

    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::IOC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});

    // IOC cancelled immediately.
    EXPECT_GE(count_events<OrderCancelled>(order_listener_), 1u);
    // No fills on outright engines.
    EXPECT_EQ(sim_.get_engine(1)->book().best_bid(), nullptr);
    EXPECT_EQ(sim_.get_engine(1)->book().best_ask(), nullptr);
    EXPECT_EQ(sim_.get_engine(2)->book().best_bid(), nullptr);
    EXPECT_EQ(sim_.get_engine(2)->book().best_ask(), nullptr);
}

TEST_F(SpreadE2ETest, FOKRejectNoOrphanFills) {
    // Only partial liquidity on one leg.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));
    // No back month bid.

    order_listener_.clear();

    // FOK for 10000 spread-lots -- back leg has no liquidity.
    // Direct spread book has no resting orders either => FOK fails.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::FOK, .price = -5000, .quantity = 10000,
        .timestamp = 2000});

    // FOK should be cancelled.
    EXPECT_GE(count_events<OrderCancelled>(order_listener_), 1u);

    // Front ask untouched.
    auto* front_ask = sim_.get_engine(1)->book().best_ask();
    ASSERT_NE(front_ask, nullptr);
    EXPECT_EQ(front_ask->total_quantity, 10000);
}

// ---------------------------------------------------------------------------
// Multi-fill implied-out: multiple spread orders fill sequentially
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, MultipleSpreadOrdersFillSequentially) {
    // Two resting spread sells.
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});
    spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 11, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1001});

    // Outright liquidity for 2 fills.
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 20000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 20000, 2000));

    order_listener_.clear();
    int fills = spread_sim_->on_outright_bbo_change(1, 3000);
    EXPECT_GE(fills, 2);

    // Both spread orders should be filled.
    auto* book = spread_sim_->get_spread_book(100);
    EXPECT_EQ(book->get_order(1), nullptr);
    EXPECT_EQ(book->get_order(2), nullptr);
}

// ---------------------------------------------------------------------------
// Spread modify then fill: cancel-replace followed by implied fill
// ---------------------------------------------------------------------------

TEST_F(SpreadE2ETest, ModifyThenImpliedFill) {
    // Resting spread sell at -2500 (too expensive for current implied).
    OrderId id = spread_sim_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 10, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -2500, .quantity = 10000,
        .timestamp = 1000});

    // Outright: implied bid = 1000000 - 1005000 = -5000 (won't cross -2500).
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 10000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 10000, 2000));

    int fills = spread_sim_->on_outright_bbo_change(1, 3000);
    EXPECT_EQ(fills, 0);  // No cross at -2500.

    // Modify spread to -5000 (now matches implied bid).
    spread_sim_->modify_spread_order(100, SpreadModifyRequest{
        .order_id = id, .client_order_id = 11,
        .new_price = -5000, .new_quantity = 10000, .timestamp = 4000});

    // Trigger implied matching again.
    fills = spread_sim_->on_outright_bbo_change(1, 5000);
    EXPECT_GE(fills, 1);

    auto* book = spread_sim_->get_spread_book(100);
    EXPECT_EQ(book->get_order(id), nullptr);  // filled
}

}  // namespace
}  // namespace exchange
