#include "exchange-sim/spread_book/multi_leg_coordinator.h"
#include "exchange-sim/spread_book/spread_book.h"
#include "exchange-sim/spread_book/spread_events.h"
#include "exchange-sim/spread_book/spread_strategy.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Small pool sizes for tests.
using TestSpreadBook = SpreadBook<100, 50, 1000>;

// Calendar spread: ES Jun (+1) vs ES Sep (-1), tick=2500, lot=10000.
SpreadStrategy make_test_calendar() {
    return SpreadStrategy(
        StrategyType::CalendarSpread,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        });
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class SpreadBookTest : public ::testing::Test {
protected:
    SpreadStrategy strategy_ = make_test_calendar();
    RecordingOrderListener listener_;
    TestSpreadBook book_{strategy_, 100};

    SpreadOrderRequest make_spread_order(
        uint64_t cl_ord_id, Side side, Price price, Quantity qty,
        TimeInForce tif = TimeInForce::GTC, Timestamp ts = 1000) {
        return SpreadOrderRequest{
            .client_order_id = cl_ord_id,
            .account_id = 1,
            .side = side,
            .tif = tif,
            .price = price,
            .quantity = qty,
            .timestamp = ts,
        };
    }
};

// ---------------------------------------------------------------------------
// Order entry tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, AcceptValidOrder) {
    auto req = make_spread_order(1, Side::Buy, 5000, 10000);
    OrderId id = book_.new_order(req, listener_);
    EXPECT_NE(id, 0u);

    ASSERT_GE(listener_.size(), 1u);
    auto* accepted = std::get_if<OrderAccepted>(&listener_.events()[0]);
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->id, id);
}

TEST_F(SpreadBookTest, RejectZeroQuantity) {
    auto req = make_spread_order(1, Side::Buy, 5000, 0);
    EXPECT_EQ(book_.new_order(req, listener_), 0u);

    ASSERT_GE(listener_.size(), 1u);
    auto* rejected = std::get_if<OrderRejected>(&listener_.events()[0]);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->reason, RejectReason::InvalidQuantity);
}

TEST_F(SpreadBookTest, RejectBadLotSize) {
    // 15000 is not a multiple of lot_size=10000
    auto req = make_spread_order(1, Side::Buy, 5000, 15000);
    EXPECT_EQ(book_.new_order(req, listener_), 0u);
}

TEST_F(SpreadBookTest, RejectBadTickSize) {
    // 1000 is not a multiple of tick_size=2500
    auto req = make_spread_order(1, Side::Buy, 1000, 10000);
    EXPECT_EQ(book_.new_order(req, listener_), 0u);
}

TEST_F(SpreadBookTest, AcceptNegativePrice) {
    // Calendar spreads can have negative prices.
    auto req = make_spread_order(1, Side::Buy, -5000, 10000);
    EXPECT_NE(book_.new_order(req, listener_), 0u);
}

TEST_F(SpreadBookTest, AcceptZeroPrice) {
    auto req = make_spread_order(1, Side::Buy, 0, 10000);
    EXPECT_NE(book_.new_order(req, listener_), 0u);
}

TEST_F(SpreadBookTest, RejectInvalidTif) {
    auto req = make_spread_order(1, Side::Buy, 5000, 10000, TimeInForce::GTD);
    EXPECT_EQ(book_.new_order(req, listener_), 0u);
}

// ---------------------------------------------------------------------------
// Cancel tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, CancelRestingOrder) {
    auto req = make_spread_order(1, Side::Buy, 5000, 10000);
    OrderId id = book_.new_order(req, listener_);
    ASSERT_NE(id, 0u);

    listener_.clear();
    EXPECT_TRUE(book_.cancel_order(
        SpreadCancelRequest{.order_id = id, .timestamp = 2000}, listener_));

    ASSERT_GE(listener_.size(), 1u);
    auto* cancelled = std::get_if<OrderCancelled>(&listener_.events()[0]);
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->id, id);
    EXPECT_EQ(cancelled->reason, CancelReason::UserRequested);
}

TEST_F(SpreadBookTest, CancelUnknownOrder) {
    EXPECT_FALSE(book_.cancel_order(
        SpreadCancelRequest{.order_id = 999, .timestamp = 1000}, listener_));
}

// ---------------------------------------------------------------------------
// Direct matching tests (spread-vs-spread)
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, DirectMatchFullFill) {
    // Resting sell at 5000.
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Incoming buy at 5000 -- should cross.
    auto buy = make_spread_order(2, Side::Buy, 5000, 10000, TimeInForce::GTC, 2000);
    OrderId buy_id = book_.new_order(buy, listener_);
    ASSERT_NE(buy_id, 0u);

    // Should have: Accepted + Filled
    bool found_fill = false;
    for (const auto& ev : listener_.events()) {
        if (auto* fill = std::get_if<OrderFilled>(&ev)) {
            found_fill = true;
            EXPECT_EQ(fill->price, 5000);
            EXPECT_EQ(fill->quantity, 10000);
        }
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(SpreadBookTest, DirectMatchPartialFill) {
    // Resting sell 10000 at 5000.
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Incoming buy 20000 at 5000 -- partial fill (10000 matches, 10000 rests).
    auto buy = make_spread_order(2, Side::Buy, 5000, 20000, TimeInForce::GTC, 2000);
    book_.new_order(buy, listener_);

    bool found_partial = false;
    for (const auto& ev : listener_.events()) {
        if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            found_partial = true;
            EXPECT_EQ(pf->quantity, 10000);
        }
    }
    EXPECT_TRUE(found_partial);
}

TEST_F(SpreadBookTest, DirectMatchPriceImprovement) {
    // Resting sell at 2500 (low ask).
    auto sell = make_spread_order(1, Side::Sell, 2500, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Incoming buy at 5000 (higher bid) -- matches at resting price 2500.
    auto buy = make_spread_order(2, Side::Buy, 5000, 10000, TimeInForce::GTC, 2000);
    book_.new_order(buy, listener_);

    bool found_fill = false;
    for (const auto& ev : listener_.events()) {
        if (auto* fill = std::get_if<OrderFilled>(&ev)) {
            found_fill = true;
            EXPECT_EQ(fill->price, 2500);  // resting price, not aggressive
        }
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(SpreadBookTest, NoCrossNoBuySell) {
    // Resting sell at 7500.
    auto sell = make_spread_order(1, Side::Sell, 7500, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Incoming buy at 5000 -- does NOT cross (5000 < 7500).
    auto buy = make_spread_order(2, Side::Buy, 5000, 10000, TimeInForce::GTC, 2000);
    book_.new_order(buy, listener_);

    for (const auto& ev : listener_.events()) {
        EXPECT_EQ(std::get_if<OrderFilled>(&ev), nullptr);
        EXPECT_EQ(std::get_if<OrderPartiallyFilled>(&ev), nullptr);
    }
}

// ---------------------------------------------------------------------------
// IOC tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, IOCFullMatch) {
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);
    listener_.clear();

    auto ioc = make_spread_order(2, Side::Buy, 5000, 10000, TimeInForce::IOC, 2000);
    book_.new_order(ioc, listener_);

    bool found_fill = false;
    for (const auto& ev : listener_.events()) {
        if (std::get_if<OrderFilled>(&ev)) found_fill = true;
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(SpreadBookTest, IOCPartialMatchCancelsRemainder) {
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);
    listener_.clear();

    // IOC buy for 20000, only 10000 available.
    auto ioc = make_spread_order(2, Side::Buy, 5000, 20000, TimeInForce::IOC, 2000);
    OrderId id = book_.new_order(ioc, listener_);

    bool found_cancel = false;
    for (const auto& ev : listener_.events()) {
        if (auto* c = std::get_if<OrderCancelled>(&ev)) {
            if (c->id == id) {
                EXPECT_EQ(c->reason, CancelReason::IOCRemainder);
                found_cancel = true;
            }
        }
    }
    EXPECT_TRUE(found_cancel);
}

TEST_F(SpreadBookTest, IOCNoMatchCancelled) {
    // No resting orders.
    auto ioc = make_spread_order(1, Side::Buy, 5000, 10000, TimeInForce::IOC);
    OrderId id = book_.new_order(ioc, listener_);

    bool found_cancel = false;
    for (const auto& ev : listener_.events()) {
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
// FOK tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, FOKFullMatchExecutes) {
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);
    listener_.clear();

    auto fok = make_spread_order(2, Side::Buy, 5000, 10000, TimeInForce::FOK, 2000);
    book_.new_order(fok, listener_);

    bool found_fill = false;
    for (const auto& ev : listener_.events()) {
        if (std::get_if<OrderFilled>(&ev)) found_fill = true;
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(SpreadBookTest, FOKInsufficientLiquidityCancelled) {
    auto sell = make_spread_order(1, Side::Sell, 5000, 10000);
    book_.new_order(sell, listener_);
    listener_.clear();

    // FOK buy for 20000, only 10000 available -> rejected.
    auto fok = make_spread_order(2, Side::Buy, 5000, 20000, TimeInForce::FOK, 2000);
    OrderId id = book_.new_order(fok, listener_);

    bool found_cancel = false;
    for (const auto& ev : listener_.events()) {
        if (auto* c = std::get_if<OrderCancelled>(&ev)) {
            if (c->id == id) {
                EXPECT_EQ(c->reason, CancelReason::FOKFailed);
                found_cancel = true;
            }
        }
    }
    EXPECT_TRUE(found_cancel);
}

// ---------------------------------------------------------------------------
// Modify (cancel-replace) tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, ModifyPriceAndQuantity) {
    auto req = make_spread_order(1, Side::Buy, 5000, 10000);
    OrderId id = book_.new_order(req, listener_);
    listener_.clear();

    SpreadModifyRequest mod{
        .order_id = id,
        .client_order_id = 2,
        .new_price = 7500,
        .new_quantity = 20000,
        .timestamp = 2000,
    };
    EXPECT_TRUE(book_.modify_order(mod, listener_));

    bool found_modified = false;
    for (const auto& ev : listener_.events()) {
        if (auto* m = std::get_if<OrderModified>(&ev)) {
            found_modified = true;
            EXPECT_EQ(m->id, id);
            EXPECT_EQ(m->new_price, 7500);
            EXPECT_EQ(m->new_qty, 20000);
        }
    }
    EXPECT_TRUE(found_modified);
}

TEST_F(SpreadBookTest, ModifyInvalidPrice) {
    auto req = make_spread_order(1, Side::Buy, 5000, 10000);
    OrderId id = book_.new_order(req, listener_);
    listener_.clear();

    SpreadModifyRequest mod{
        .order_id = id,
        .client_order_id = 2,
        .new_price = 1000,  // not aligned to tick=2500
        .new_quantity = 10000,
        .timestamp = 2000,
    };
    EXPECT_FALSE(book_.modify_order(mod, listener_));
}

TEST_F(SpreadBookTest, ModifyUnknownOrder) {
    SpreadModifyRequest mod{
        .order_id = 999,
        .client_order_id = 2,
        .new_price = 5000,
        .new_quantity = 10000,
        .timestamp = 2000,
    };
    EXPECT_FALSE(book_.modify_order(mod, listener_));
}

// ---------------------------------------------------------------------------
// Implied-out matching tests
// ---------------------------------------------------------------------------

class SpreadBookImpliedTest : public ::testing::Test {
protected:
    SpreadStrategy strategy_ = make_test_calendar();
    RecordingOrderListener listener_;
    TestSpreadBook book_{strategy_, 100};

    // Simulated outright BBOs.
    LegBBO leg0_bbo_;  // front month
    LegBBO leg1_bbo_;  // back month

    // Simulated best order IDs.
    std::optional<OrderId> leg0_best_bid_id_;
    std::optional<OrderId> leg0_best_ask_id_;
    std::optional<OrderId> leg1_best_bid_id_;
    std::optional<OrderId> leg1_best_ask_id_;

    // Track applied fills.
    struct AppliedFill {
        uint32_t instrument_id;
        LegFill fill;
    };
    std::vector<AppliedFill> applied_fills_;
    bool fill_applier_should_fail_{false};

    void SetUp() override {
        book_.set_bbo_provider([this](uint32_t id) -> LegBBO {
            return (id == 1) ? leg0_bbo_ : leg1_bbo_;
        });

        book_.set_best_order_provider(
            [this](uint32_t id, Side side) -> std::optional<OrderId> {
                if (id == 1) return (side == Side::Buy) ? leg0_best_bid_id_
                                                         : leg0_best_ask_id_;
                return (side == Side::Buy) ? leg1_best_bid_id_
                                            : leg1_best_ask_id_;
            });

        book_.set_fill_applier(
            [this](uint32_t id, std::span<const LegFill> fills,
                   Timestamp) -> bool {
                if (fill_applier_should_fail_) return false;
                for (const auto& f : fills) {
                    applied_fills_.push_back({id, f});
                }
                return true;
            });
    }
};

TEST_F(SpreadBookImpliedTest, ImpliedOutBidMatchesRestingAsk) {
    // Resting spread sell at -5000 (front - back = -0.5000).
    book_.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000}, listener_);

    listener_.clear();

    // Outright BBOs: front bid=1000000, back ask=1005000
    // Implied spread bid = front_bid - back_ask = 1000000 - 1005000 = -5000
    // This crosses the resting ask at -5000.
    leg0_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1010000, .ask_qty = 10000};
    leg1_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1005000, .ask_qty = 10000};

    // For implied-out bid (synthetic buy spread): we buy front (need resting
    // ask on front) and sell back (need resting bid on back).
    leg0_best_ask_id_ = 101;  // front month resting ask (we buy from it)
    leg1_best_bid_id_ = 202;  // back month resting bid (we sell to it)

    int fills = book_.on_outright_bbo_change(listener_, 2000);
    EXPECT_EQ(fills, 1);

    // Check outright fills were applied.
    EXPECT_EQ(applied_fills_.size(), 2u);
}

TEST_F(SpreadBookImpliedTest, ImpliedOutNoCross) {
    // Resting spread sell at -2500.
    book_.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -2500, .quantity = 10000,
        .timestamp = 1000}, listener_);

    listener_.clear();

    // Implied spread bid = 1000000 - 1005000 = -5000
    // -5000 < -2500, so does NOT cross resting ask at -2500.
    leg0_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1010000, .ask_qty = 10000};
    leg1_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1005000, .ask_qty = 10000};

    leg0_best_ask_id_ = 101;
    leg1_best_bid_id_ = 202;

    int fills = book_.on_outright_bbo_change(listener_, 2000);
    EXPECT_EQ(fills, 0);
}

TEST_F(SpreadBookImpliedTest, ImpliedOutFailedFillApplier) {
    book_.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Sell,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000}, listener_);

    leg0_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1010000, .ask_qty = 10000};
    leg1_bbo_ = {.bid_price = 1000000, .bid_qty = 10000,
                  .ask_price = 1005000, .ask_qty = 10000};
    leg0_best_ask_id_ = 101;
    leg1_best_bid_id_ = 202;

    fill_applier_should_fail_ = true;

    int fills = book_.on_outright_bbo_change(listener_, 2000);
    EXPECT_EQ(fills, 0);
}

// ---------------------------------------------------------------------------
// Implied-in matching tests
// ---------------------------------------------------------------------------

TEST_F(SpreadBookImpliedTest, ImpliedInFillsRestingBid) {
    // Resting spread buy at -5000.
    book_.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -5000, .quantity = 10000,
        .timestamp = 1000}, listener_);

    listener_.clear();

    // Buy spread: buy front, sell back.
    // Front ask = 1000000, back bid = 1005000.
    // Actual spread price = front_ask - back_bid = 1000000 - 1005000 = -5000.
    // Resting buy at -5000 should fill since actual <= resting.
    leg0_bbo_ = {.bid_price = 990000,  .bid_qty = 10000,
                  .ask_price = 1000000, .ask_qty = 10000};
    leg1_bbo_ = {.bid_price = 1005000, .bid_qty = 10000,
                  .ask_price = 1010000, .ask_qty = 10000};

    leg0_best_ask_id_ = 101;  // sell side for buying front
    leg1_best_bid_id_ = 202;  // buy side for selling back

    int fills = book_.try_implied_in(listener_, 2000);
    EXPECT_EQ(fills, 1);
    EXPECT_EQ(applied_fills_.size(), 2u);
}

TEST_F(SpreadBookImpliedTest, ImpliedInPriceNotFavorable) {
    // Resting spread buy at -7500 (wants a more negative spread).
    book_.new_order(SpreadOrderRequest{
        .client_order_id = 1, .account_id = 1, .side = Side::Buy,
        .tif = TimeInForce::GTC, .price = -7500, .quantity = 10000,
        .timestamp = 1000}, listener_);

    listener_.clear();

    // Actual spread = 1000000 - 1005000 = -5000.
    // -5000 > -7500 (not favorable for buyer). Should NOT fill.
    leg0_bbo_ = {.bid_price = 990000,  .bid_qty = 10000,
                  .ask_price = 1000000, .ask_qty = 10000};
    leg1_bbo_ = {.bid_price = 1005000, .bid_qty = 10000,
                  .ask_price = 1010000, .ask_qty = 10000};

    leg0_best_ask_id_ = 101;
    leg1_best_bid_id_ = 202;

    int fills = book_.try_implied_in(listener_, 2000);
    EXPECT_EQ(fills, 0);
}

// ---------------------------------------------------------------------------
// MultiLegCoordinator tests
// ---------------------------------------------------------------------------

TEST(MultiLegCoordinatorTest, ValidateThenApply) {
    MultiLegCoordinator coord;

    bool validated = false;
    bool applied = false;

    coord.set_validator([&](uint32_t, std::span<const LegFill>) {
        validated = true;
        return true;
    });
    coord.set_applier([&](uint32_t, std::span<const LegFill>, Timestamp) {
        applied = true;
        return true;
    });

    coord.add_leg_fill(1, LegFill{101, 1000000, 10000});
    coord.add_leg_fill(2, LegFill{202, 1005000, 10000});

    EXPECT_EQ(coord.instrument_count(), 2u);
    EXPECT_EQ(coord.total_fill_count(), 2u);

    EXPECT_TRUE(coord.execute(1000));
    EXPECT_TRUE(validated);
    EXPECT_TRUE(applied);
}

TEST(MultiLegCoordinatorTest, ValidationFailureNoApply) {
    MultiLegCoordinator coord;

    bool applied = false;

    coord.set_validator([](uint32_t id, std::span<const LegFill>) {
        return id != 2;  // fail for instrument 2
    });
    coord.set_applier([&](uint32_t, std::span<const LegFill>, Timestamp) {
        applied = true;
        return true;
    });

    coord.add_leg_fill(1, LegFill{101, 1000000, 10000});
    coord.add_leg_fill(2, LegFill{202, 1005000, 10000});

    EXPECT_FALSE(coord.execute(1000));
    EXPECT_FALSE(applied);
}

TEST(MultiLegCoordinatorTest, EmptyExecuteSucceeds) {
    MultiLegCoordinator coord;
    EXPECT_TRUE(coord.execute(1000));
}

TEST(MultiLegCoordinatorTest, ClearResetsState) {
    MultiLegCoordinator coord;
    coord.add_leg_fill(1, LegFill{101, 1000000, 10000});
    EXPECT_EQ(coord.total_fill_count(), 1u);

    coord.clear();
    EXPECT_EQ(coord.total_fill_count(), 0u);
    EXPECT_EQ(coord.instrument_count(), 0u);
}

// ---------------------------------------------------------------------------
// SpreadEvents type tests
// ---------------------------------------------------------------------------

TEST(SpreadEventsTest, SpreadFillConstruction) {
    SpreadFill fill{
        .spread_instrument_id = 100,
        .spread_order_id = 1,
        .spread_side = Side::Buy,
        .spread_price = -5000,
        .spread_qty = 10000,
        .ts = 2000,
        .leg_details = {
            {.instrument_id = 1, .outright_order_id = 101,
             .side = Side::Buy, .price = 1000000, .qty = 10000},
            {.instrument_id = 2, .outright_order_id = 202,
             .side = Side::Sell, .price = 1005000, .qty = 10000},
        },
        .source = SpreadFill::Source::ImpliedOut,
    };

    EXPECT_EQ(fill.spread_instrument_id, 100u);
    EXPECT_EQ(fill.spread_price, -5000);
    EXPECT_EQ(fill.leg_details.size(), 2u);
    EXPECT_EQ(fill.source, SpreadFill::Source::ImpliedOut);
}

TEST(SpreadEventsTest, ImpliedTopOfBookConstruction) {
    ImpliedTopOfBook tob{
        .spread_instrument_id = 100,
        .implied_bid = -5000,
        .implied_bid_qty = 10000,
        .implied_ask = -2500,
        .implied_ask_qty = 20000,
        .ts = 2000,
    };

    EXPECT_EQ(tob.implied_bid, -5000);
    EXPECT_EQ(tob.implied_ask, -2500);
}

// ---------------------------------------------------------------------------
// Negative spread price matching (calendar spread specific)
// ---------------------------------------------------------------------------

TEST_F(SpreadBookTest, NegativeSpreadPriceCrossing) {
    // Sell at -7500 (willing to sell at -7500 or higher).
    auto sell = make_spread_order(1, Side::Sell, -7500, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Buy at -5000 (-5000 > -7500, so crosses).
    auto buy = make_spread_order(2, Side::Buy, -5000, 10000, TimeInForce::GTC, 2000);
    book_.new_order(buy, listener_);

    bool found_fill = false;
    for (const auto& ev : listener_.events()) {
        if (auto* fill = std::get_if<OrderFilled>(&ev)) {
            found_fill = true;
            EXPECT_EQ(fill->price, -7500);  // fills at resting price
        }
    }
    EXPECT_TRUE(found_fill);
}

TEST_F(SpreadBookTest, NegativeSpreadPriceNoCross) {
    // Sell at -2500.
    auto sell = make_spread_order(1, Side::Sell, -2500, 10000);
    book_.new_order(sell, listener_);

    listener_.clear();

    // Buy at -5000 (-5000 < -2500, so does NOT cross).
    auto buy = make_spread_order(2, Side::Buy, -5000, 10000, TimeInForce::GTC, 2000);
    book_.new_order(buy, listener_);

    for (const auto& ev : listener_.events()) {
        EXPECT_EQ(std::get_if<OrderFilled>(&ev), nullptr);
    }
}

}  // namespace
}  // namespace exchange
