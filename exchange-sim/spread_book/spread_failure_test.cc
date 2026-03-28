// spread_failure_test.cc
//
// Failure injection tests for spread trading.
// Verifies graceful degradation under fault conditions:
//   - Partial fill rollback (leg applier fails mid-batch)
//   - Stale BBO (outright book changes between check and execution)
//   - TIF edge cases (FOK with exact-match, IOC with zero liquidity)
//   - Pool exhaustion (spread order/level pools full)
//   - Invalid inputs (bad tick, lot, instrument IDs)
//   - Concurrent cancel during implied match attempt

#include "exchange-sim/exchange_simulator.h"
#include "exchange-sim/spread_book/multi_leg_coordinator.h"
#include "exchange-sim/spread_book/spread_book.h"
#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "exchange-sim/spread_book/spread_simulator.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Minimal engine for failure tests.
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

InstrumentConfig make_instrument(InstrumentId id, const std::string& sym,
                                 Price tick = 2500, Quantity lot = 10000) {
    return InstrumentConfig{
        .id = id, .symbol = sym,
        .engine_config = EngineConfig{.tick_size = tick, .lot_size = lot,
                                      .price_band_low = 0,
                                      .price_band_high = 0}};
}

OrderRequest make_limit(uint64_t cl, Side side, Price price,
                        Quantity qty, Timestamp ts) {
    return OrderRequest{.client_order_id = cl, .account_id = 1,
                        .side = side, .type = OrderType::Limit,
                        .tif = TimeInForce::GTC, .price = price,
                        .quantity = qty, .stop_price = 0,
                        .timestamp = ts, .gtd_expiry = 0};
}

template <typename EventT>
size_t count_events(const RecordingOrderListener& l) {
    size_t n = 0;
    for (const auto& ev : l.events()) { if (std::get_if<EventT>(&ev)) ++n; }
    return n;
}

// ---------------------------------------------------------------------------
// Failure fixture
// ---------------------------------------------------------------------------

class SpreadFailureTest : public ::testing::Test {
protected:
    RecordingOrderListener ol_;
    RecordingMdListener ml_;
    TestSimulator sim_{ol_, ml_};
    std::unique_ptr<TestSpreadSim> ss_;

    void SetUp() override {
        sim_.set_session_state(SessionState::Continuous, 0);
        sim_.add_instrument(make_instrument(1, "ESU5"));
        sim_.add_instrument(make_instrument(2, "ESZ5"));

        ss_ = std::make_unique<TestSpreadSim>(
            [this](uint32_t id) -> TestEngine* { return sim_.get_engine(id); },
            ol_);
        ss_->add_spread_instrument(SpreadInstrumentConfig{
            .id = 100, .symbol = "CAL",
            .strategy_type = StrategyType::CalendarSpread,
            .legs = {
                {.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
                {.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                 .tick_size = 2500, .lot_size = 10000},
            },
        });
    }
};

// ---------------------------------------------------------------------------
// Partial fill rollback: apply_implied_fills rejects insufficient qty
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, ImpliedOutFailsWhenOutrightQtyInsufficient) {
    // Resting spread sell at -5000.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 20000,
        .timestamp = 1000});

    // Outright: only 10000 on each side -- insufficient for 20000 spread.
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 10000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 10000, 2000));

    ol_.clear();
    int fills = ss_->on_outright_bbo_change(1, 3000);

    // Should partially fill 10000 (limited by outright qty).
    EXPECT_GE(fills, 1);

    // Spread order should still have 10000 remaining.
    auto* order = ss_->get_spread_book(100)->get_order(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->remaining_quantity, 10000);
}

// ---------------------------------------------------------------------------
// Stale BBO: outright book empty when implied matching runs
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, ImpliedOutEmptyOutrightBook) {
    // Resting spread sell.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});

    // No outright orders at all.
    int fills = ss_->on_outright_bbo_change(1, 2000);
    EXPECT_EQ(fills, 0);

    // Spread order untouched.
    EXPECT_NE(ss_->get_spread_book(100)->get_order(1), nullptr);
}

TEST_F(SpreadFailureTest, ImpliedInEmptyOutrightBook) {
    // No outright liquidity.
    ol_.clear();
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});

    // Order should rest (implied-in fails gracefully with no liquidity).
    EXPECT_NE(ss_->get_spread_book(100)->get_order(1), nullptr);
}

// ---------------------------------------------------------------------------
// One-sided outright: only one leg has liquidity
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, OneLegLiquidityOnlyNoFill) {
    // Only front month ask, no back month bid.
    sim_.new_order(1, make_limit(20, Side::Sell, 1000000, 10000, 1000));

    ol_.clear();
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 2000});

    // Spread rests, front ask untouched.
    EXPECT_NE(ss_->get_spread_book(100)->get_order(1), nullptr);
    EXPECT_EQ(sim_.get_engine(1)->book().best_ask()->total_quantity, 10000);
}

// ---------------------------------------------------------------------------
// TIF edge cases
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, FOKExactQuantityMatches) {
    // Resting sell exactly 10000.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = 5000, .quantity = 10000,
        .timestamp = 1000});

    ol_.clear();

    // FOK buy for exactly 10000 at 5000 -- should fill.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 2, .account_id = 2, .side = Side::Buy,
        .tif = TimeInForce::FOK, .price = 5000, .quantity = 10000,
        .timestamp = 2000});

    EXPECT_GE(count_events<OrderFilled>(ol_), 1u);
}

TEST_F(SpreadFailureTest, FOKOffByOneLotRejected) {
    // Resting sell 10000.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = 5000, .quantity = 10000,
        .timestamp = 1000});

    ol_.clear();

    // FOK buy for 20000 -- insufficient (only 10000 available).
    OrderId id = ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 2, .account_id = 2, .side = Side::Buy,
        .tif = TimeInForce::FOK, .price = 5000, .quantity = 20000,
        .timestamp = 2000});

    bool found_cancel = false;
    for (const auto& ev : ol_.events()) {
        if (auto* c = std::get_if<OrderCancelled>(&ev)) {
            if (c->id == id) {
                EXPECT_EQ(c->reason, CancelReason::FOKFailed);
                found_cancel = true;
            }
        }
    }
    EXPECT_TRUE(found_cancel);
}

TEST_F(SpreadFailureTest, IOCPartialFillCancelsRemainder) {
    // Resting sell 10000.
    ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = 5000, .quantity = 10000,
        .timestamp = 1000});

    ol_.clear();

    // IOC buy for 30000 -- only 10000 available.
    OrderId id = ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 2, .account_id = 2, .side = Side::Buy,
        .tif = TimeInForce::IOC, .price = 5000, .quantity = 30000,
        .timestamp = 2000});

    bool found_cancel = false;
    for (const auto& ev : ol_.events()) {
        if (auto* c = std::get_if<OrderCancelled>(&ev)) {
            if (c->id == id) {
                EXPECT_EQ(c->reason, CancelReason::IOCRemainder);
                found_cancel = true;
            }
        }
    }
    EXPECT_TRUE(found_cancel);
}

// ---------------------------------------------------------------------------
// Pool exhaustion: small SpreadBook pool
// ---------------------------------------------------------------------------

TEST(SpreadBookPoolTest, OrderPoolExhaustion) {
    SpreadStrategy strategy(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                    .tick_size = 2500, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 2500, .lot_size = 10000},
    });

    // Tiny pool: only 3 orders.
    SpreadBook<3, 10, 100> book(strategy, 100);
    RecordingOrderListener listener;

    // Fill all 3 slots.
    for (int i = 1; i <= 3; ++i) {
        OrderId id = book.new_order(SpreadOrderRequest{
            .client_order_id = static_cast<uint64_t>(i),
            .account_id = 1, .side = Side::Buy,
            .tif = TimeInForce::GTC,
            .price = static_cast<Price>(i * 2500),
            .quantity = 10000,
            .timestamp = static_cast<Timestamp>(i * 1000)},
            listener);
        EXPECT_NE(id, 0u) << "Order " << i << " should succeed";
    }

    // 4th order should be rejected (pool exhausted).
    OrderId id = book.new_order(SpreadOrderRequest{
        .client_order_id = 4, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = 10000, .quantity = 10000,
        .timestamp = 4000}, listener);
    EXPECT_EQ(id, 0u);

    bool found_reject = false;
    for (const auto& ev : listener.events()) {
        if (auto* r = std::get_if<OrderRejected>(&ev)) {
            if (r->client_order_id == 4) {
                EXPECT_EQ(r->reason, RejectReason::PoolExhausted);
                found_reject = true;
            }
        }
    }
    EXPECT_TRUE(found_reject);
}

// ---------------------------------------------------------------------------
// Invalid inputs
// ---------------------------------------------------------------------------

TEST(SpreadBookValidationTest, RejectNegativeQuantity) {
    SpreadStrategy strategy(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    SpreadBook<> book(strategy, 100);
    RecordingOrderListener listener;

    EXPECT_EQ(book.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = 100, .quantity = -10000,
        .timestamp = 1000}, listener), 0u);
}

TEST(SpreadBookValidationTest, RejectMisalignedTick) {
    SpreadStrategy strategy(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                    .tick_size = 2500, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 2500, .lot_size = 10000},
    });
    SpreadBook<> book(strategy, 100);
    RecordingOrderListener listener;

    // 1000 is not aligned to tick=2500.
    EXPECT_EQ(book.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = 1000, .quantity = 10000,
        .timestamp = 1000}, listener), 0u);
}

// ---------------------------------------------------------------------------
// MultiLegCoordinator failure: validator rejects one instrument
// ---------------------------------------------------------------------------

TEST(CoordinatorFailureTest, ValidatorRejectsOneInstrument) {
    MultiLegCoordinator coord;
    bool applied = false;

    coord.set_validator([](uint32_t id, std::span<const LegFill>) {
        return id != 2;  // instrument 2 fails
    });
    coord.set_applier([&](uint32_t, std::span<const LegFill>, Timestamp) {
        applied = true;
        return true;
    });

    coord.add_leg_fill(1, LegFill{101, 1000000, 10000});
    coord.add_leg_fill(2, LegFill{202, 1005000, 10000});

    EXPECT_FALSE(coord.execute(1000));
    EXPECT_FALSE(applied);  // Nothing applied.
}

TEST(CoordinatorFailureTest, ApplierFailsMidBatch) {
    MultiLegCoordinator coord;
    int apply_count = 0;

    coord.set_validator([](uint32_t, std::span<const LegFill>) {
        return true;  // all validate
    });
    coord.set_applier([&](uint32_t id, std::span<const LegFill>, Timestamp) {
        ++apply_count;
        return id != 2;  // instrument 2 fails during apply
    });

    coord.add_leg_fill(1, LegFill{101, 1000000, 10000});
    coord.add_leg_fill(2, LegFill{202, 1005000, 10000});

    EXPECT_FALSE(coord.execute(1000));
    // At least one apply was attempted before failure.
    EXPECT_GE(apply_count, 1);
}

// ---------------------------------------------------------------------------
// Cancel after rest, then implied trigger: no stale reference
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, CancelThenImpliedNoStaleFill) {
    // Rest a spread sell.
    OrderId id = ss_->new_spread_order(100, SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000});

    // Cancel it.
    ss_->cancel_spread_order(100,
        SpreadCancelRequest{.order_id = id, .timestamp = 1500});

    // Now add outright liquidity that would have triggered a fill.
    sim_.new_order(1, make_limit(20, Side::Buy, 1000000, 10000, 2000));
    sim_.new_order(2, make_limit(30, Side::Sell, 1005000, 10000, 2000));

    ol_.clear();
    int fills = ss_->on_outright_bbo_change(1, 3000);
    EXPECT_EQ(fills, 0);  // No spread orders to fill.

    // Outright orders untouched.
    EXPECT_EQ(sim_.get_engine(1)->book().best_bid()->total_quantity, 10000);
    EXPECT_EQ(sim_.get_engine(2)->book().best_ask()->total_quantity, 10000);
}

// ---------------------------------------------------------------------------
// Spread order on unknown instrument
// ---------------------------------------------------------------------------

TEST_F(SpreadFailureTest, UnknownSpreadInstrumentThrows) {
    EXPECT_THROW(
        ss_->new_spread_order(999, SpreadOrderRequest{
            .client_order_id = 1, .account_id = 1, .side = Side::Buy,
            .tif = TimeInForce::GTC, .price = 5000, .quantity = 10000,
            .timestamp = 1000}),
        std::runtime_error);
}

}  // namespace
}  // namespace exchange
