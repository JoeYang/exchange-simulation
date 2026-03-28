#include "exchange-sim/spread_book/implied_price_engine.h"
#include "exchange-sim/spread_book/spread_strategy.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

constexpr Price kEsTick = 2500;     // 0.25 in fixed-point
constexpr Quantity kEsLot = 10000;  // 1.0 in fixed-point

std::vector<StrategyLeg> make_calendar_legs() {
    return {
        {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
    };
}

std::vector<StrategyLeg> make_butterfly_legs() {
    return {
        {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 2, .ratio = -2, .price_multiplier = 2,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
    };
}

std::vector<StrategyLeg> make_condor_legs() {
    return {
        {.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 3, .ratio = -1, .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
        {.instrument_id = 4, .ratio = 1,  .price_multiplier = 1,
         .tick_size = kEsTick, .lot_size = kEsLot},
    };
}

// Common BBO fixtures: front=100.00/100.25, back=95.00/95.25
const LegBBO kCalendarBBOs[] = {
    {.bid_price = 1000000, .bid_qty = 50000,
     .ask_price = 1002500, .ask_qty = 50000},
    {.bid_price = 950000,  .bid_qty = 30000,
     .ask_price = 952500,  .ask_qty = 30000},
};

// --- 2-leg implied-out ---

TEST(ImpliedOutTest, TwoLegBid) {
    auto legs = make_calendar_legs();
    auto r = ImpliedPriceEngine::compute_implied_out_bid(legs, kCalendarBBOs, kEsTick);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->price, 47500);   // front_bid - back_ask = 1000000 - 952500
    EXPECT_EQ(r->quantity, 30000); // min(50000, 30000)
    EXPECT_EQ(r->side, Side::Buy);
    EXPECT_EQ(r->source, ImpliedSource::ImpliedOut);
}

TEST(ImpliedOutTest, TwoLegAsk) {
    auto legs = make_calendar_legs();
    LegBBO bbos[] = {
        {.bid_price = 1000000, .bid_qty = 50000,
         .ask_price = 1002500, .ask_qty = 40000},
        {.bid_price = 950000,  .bid_qty = 60000,
         .ask_price = 952500,  .ask_qty = 30000},
    };
    auto r = ImpliedPriceEngine::compute_implied_out_ask(legs, bbos, kEsTick);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->price, 52500);   // front_ask - back_bid = 1002500 - 950000
    EXPECT_EQ(r->quantity, 40000); // min(40000, 60000)
    EXPECT_EQ(r->side, Side::Sell);
}

TEST(ImpliedOutTest, MissingBidReturnsNullopt) {
    auto legs = make_calendar_legs();
    LegBBO bbos[] = {
        {0, 0, 1002500, 50000},
        {950000, 30000, 952500, 30000},
    };
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_out_bid(legs, bbos, kEsTick));
}

TEST(ImpliedOutTest, MissingAskReturnsNullopt) {
    auto legs = make_calendar_legs();
    LegBBO bbos[] = {
        {1000000, 50000, 1002500, 50000},
        {950000, 30000, 0, 0},
    };
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_out_bid(legs, bbos, kEsTick));
}

// --- 2-leg implied-in ---

TEST(ImpliedInTest, TargetLeg1BuySide) {
    auto legs = make_calendar_legs();
    // Spread Buy@5.00. Buy leg0 at ask=100.25, implied sell leg1.
    // target = (50000 - 1002500) / (-1) = 952500
    auto r = ImpliedPriceEngine::compute_implied_in(
        legs, kCalendarBBOs, 1, Side::Buy, 50000, kEsTick);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->price, 952500);
    EXPECT_EQ(r->side, Side::Sell);
    EXPECT_EQ(r->quantity, 50000);
    EXPECT_EQ(r->source, ImpliedSource::ImpliedIn);
}

TEST(ImpliedInTest, TargetLeg0BuySide) {
    auto legs = make_calendar_legs();
    // Spread Buy@5.00. Sell leg1 at bid=95.00, implied buy leg0.
    // other = sign(-1)*1*950000 = -950000
    // target = (50000 - (-950000)) / 1 = 1000000
    auto r = ImpliedPriceEngine::compute_implied_in(
        legs, kCalendarBBOs, 0, Side::Buy, 50000, kEsTick);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->price, 1000000);
    EXPECT_EQ(r->side, Side::Buy);
    EXPECT_EQ(r->quantity, 30000);
}

TEST(ImpliedInTest, SellSide) {
    auto legs = make_calendar_legs();
    LegBBO bbos[] = {
        {1000000, 50000, 1002500, 50000},
        {950000, 30000, 952500, 40000},
    };
    // Sell spread: sell leg0, buy leg1.
    // other(leg1 buy) = sign(-1)*1*952500 = -952500
    // target = (50000 - (-952500)) / 1 = 1002500
    auto r = ImpliedPriceEngine::compute_implied_in(
        legs, bbos, 0, Side::Sell, 50000, kEsTick);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->price, 1002500);
    EXPECT_EQ(r->side, Side::Sell);
    EXPECT_EQ(r->quantity, 40000);
}

// --- N-leg butterfly ---

TEST(ImpliedOutTest, ButterflyBid) {
    auto legs = make_butterfly_legs();
    LegBBO bbos[] = {
        {1000000, 50000, 1002500, 50000},
        {995000, 40000, 997500, 40000},
        {990000, 30000, 992500, 30000},
    };
    auto r = ImpliedPriceEngine::compute_implied_out_bid(legs, bbos, kEsTick);
    ASSERT_TRUE(r.has_value());
    // 1*bid0 - 2*ask1 + 1*bid2 = 1000000 - 1995000 + 990000 = -5000
    EXPECT_EQ(r->price, -5000);
    // min(50000/1, 40000/2, 30000/1) = 20000
    EXPECT_EQ(r->quantity, 20000);
}

TEST(ImpliedOutTest, ButterflyAsk) {
    auto legs = make_butterfly_legs();
    LegBBO bbos[] = {
        {1000000, 50000, 1002500, 50000},
        {995000, 40000, 997500, 40000},
        {990000, 30000, 992500, 30000},
    };
    auto r = ImpliedPriceEngine::compute_implied_out_ask(legs, bbos, kEsTick);
    ASSERT_TRUE(r.has_value());
    // 1*ask0 - 2*bid1 + 1*ask2 = 1002500 - 1990000 + 992500 = 5000
    EXPECT_EQ(r->price, 5000);
    EXPECT_EQ(r->quantity, 20000);
}

// --- N-leg condor ---

TEST(ImpliedOutTest, CondorBid) {
    auto legs = make_condor_legs();
    LegBBO bbos[] = {
        {1000000, 50000, 1002500, 50000},
        {995000, 40000, 997500, 40000},
        {990000, 30000, 992500, 30000},
        {985000, 60000, 987500, 60000},
    };
    auto r = ImpliedPriceEngine::compute_implied_out_bid(legs, bbos, kEsTick);
    ASSERT_TRUE(r.has_value());
    // bid0 - ask1 - ask2 + bid3 = 1000000-997500-992500+985000 = -5000
    EXPECT_EQ(r->price, -5000);
    EXPECT_EQ(r->quantity, 30000);
}

// --- Tick normalization ---

TEST(NormalizeTickTest, Positive) {
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(47500, 2500), 47500);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(47600, 2500), 47500);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(49999, 2500), 47500);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(50000, 2500), 50000);
}

TEST(NormalizeTickTest, Negative) {
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(-5000, 2500), -5000);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(-5100, 2500), -5000);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(-7499, 2500), -5000);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(-7500, 2500), -7500);
}

TEST(NormalizeTickTest, ZeroTickDisabled) {
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(12345, 0), 12345);
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(-12345, 0), -12345);
}

TEST(NormalizeTickTest, ZeroPrice) {
    EXPECT_EQ(ImpliedPriceEngine::normalize_tick(0, 2500), 0);
}

// --- Lot GCD / min spread qty ---

TEST(LotGcdTest, Calendar) {
    EXPECT_EQ(ImpliedPriceEngine::compute_min_spread_qty(make_calendar_legs()), 10000);
}

TEST(LotGcdTest, Butterfly) {
    EXPECT_EQ(ImpliedPriceEngine::compute_min_spread_qty(make_butterfly_legs()), 10000);
}

TEST(LotGcdTest, DifferentLotSizes) {
    std::vector<StrategyLeg> legs = {
        {1, 3, 3, 100, 30000}, {2, -2, 2, 100, 20000}, {3, -1, 1, 100, 10000},
    };
    EXPECT_EQ(ImpliedPriceEngine::compute_min_spread_qty(legs), 10000);
}

TEST(LotGcdTest, ZeroRatio) {
    std::vector<StrategyLeg> legs = {{1, 0, 1, 100, 10000}};
    EXPECT_EQ(ImpliedPriceEngine::compute_min_spread_qty(legs), 0);
}

TEST(LotGcdTest, RoundQtyDown) {
    EXPECT_EQ(ImpliedPriceEngine::round_qty_down(25000, 10000), 20000);
    EXPECT_EQ(ImpliedPriceEngine::round_qty_down(10000, 10000), 10000);
    EXPECT_EQ(ImpliedPriceEngine::round_qty_down(9999, 10000), 0);
    EXPECT_EQ(ImpliedPriceEngine::round_qty_down(0, 10000), 0);
}

// --- Edge cases ---

TEST(ImpliedEdgeCaseTest, EmptyLegs) {
    std::vector<StrategyLeg> e;
    std::vector<LegBBO> eb;
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_out_bid(e, eb, kEsTick));
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_out_ask(e, eb, kEsTick));
}

TEST(ImpliedEdgeCaseTest, SizeMismatch) {
    auto legs = make_calendar_legs();
    LegBBO one[] = {{1000000, 50000, 1002500, 50000}};
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_out_bid(legs, one, kEsTick));
}

TEST(ImpliedEdgeCaseTest, InvalidTargetIndex) {
    auto legs = make_calendar_legs();
    EXPECT_FALSE(ImpliedPriceEngine::compute_implied_in(
        legs, kCalendarBBOs, 5, Side::Buy, 50000, kEsTick));
}

}  // namespace
}  // namespace exchange
