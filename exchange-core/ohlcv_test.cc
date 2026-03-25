#include "exchange-core/ohlcv.h"
#include <gtest/gtest.h>

namespace exchange {

// Helper: price in fixed-point (e.g. 100.0000 = 1000000)
static constexpr Price p(int64_t units) { return units * PRICE_SCALE; }
// Helper: quantity in fixed-point (e.g. 5.0000 = 50000)
static constexpr Quantity q(int64_t units) { return units * PRICE_SCALE; }

// --- Single trade ---

TEST(OhlcvStats, SingleTradeSetsSameOhlc) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(5));

    EXPECT_EQ(stats.open,  p(100));
    EXPECT_EQ(stats.high,  p(100));
    EXPECT_EQ(stats.low,   p(100));
    EXPECT_EQ(stats.close, p(100));
}

TEST(OhlcvStats, SingleTradeHasTradedIsTrue) {
    OhlcvStats stats;
    EXPECT_FALSE(stats.has_traded);
    stats.on_trade(p(50), q(1));
    EXPECT_TRUE(stats.has_traded);
}

TEST(OhlcvStats, SingleTradeCountIsOne) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(1));
    EXPECT_EQ(stats.trade_count, 1u);
}

// --- Multiple trades: high = max, low = min, close = last ---

TEST(OhlcvStats, MultipleTradesHighIsMax) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(1));
    stats.on_trade(p(120), q(1));
    stats.on_trade(p(110), q(1));
    EXPECT_EQ(stats.high, p(120));
}

TEST(OhlcvStats, MultipleTradesLowIsMin) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(1));
    stats.on_trade(p(80),  q(1));
    stats.on_trade(p(90),  q(1));
    EXPECT_EQ(stats.low, p(80));
}

TEST(OhlcvStats, MultipleTradesCloseIsLast) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(1));
    stats.on_trade(p(120), q(1));
    stats.on_trade(p(95),  q(1));
    EXPECT_EQ(stats.close, p(95));
}

TEST(OhlcvStats, MultipleTradesOpenIsFirst) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(1));
    stats.on_trade(p(120), q(1));
    stats.on_trade(p(95),  q(1));
    EXPECT_EQ(stats.open, p(100));
}

// --- Volume accumulation ---

TEST(OhlcvStats, VolumeAccumulates) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(3));
    stats.on_trade(p(110), q(7));
    EXPECT_EQ(stats.volume, q(10));
}

TEST(OhlcvStats, VolumeZeroBeforeAnyTrade) {
    OhlcvStats stats;
    EXPECT_EQ(stats.volume, 0);
}

// --- Trade count ---

TEST(OhlcvStats, TradeCountTracksCorrectly) {
    OhlcvStats stats;
    EXPECT_EQ(stats.trade_count, 0u);
    stats.on_trade(p(100), q(1));
    EXPECT_EQ(stats.trade_count, 1u);
    stats.on_trade(p(101), q(2));
    EXPECT_EQ(stats.trade_count, 2u);
    stats.on_trade(p(99),  q(3));
    EXPECT_EQ(stats.trade_count, 3u);
}

// --- VWAP ---

// VWAP = (sum of price_i * qty_i) / total_qty
// Trade 1: price=100, qty=4  → contribution = 400
// Trade 2: price=200, qty=1  → contribution = 200
// Total qty = 5, total turnover = 600
// Expected VWAP = 600/5 = 120
TEST(OhlcvStats, VwapCalculationCorrect) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(4));
    stats.on_trade(p(200), q(1));
    // vwap = (100*4 + 200*1) / 5 = 120
    EXPECT_EQ(stats.vwap(), p(120));
}

TEST(OhlcvStats, VwapEqualPriceWhenAllTradesSamePrice) {
    OhlcvStats stats;
    stats.on_trade(p(150), q(3));
    stats.on_trade(p(150), q(7));
    EXPECT_EQ(stats.vwap(), p(150));
}

// VWAP with a single trade equals that trade's price
TEST(OhlcvStats, VwapSingleTradeEqualsThatPrice) {
    OhlcvStats stats;
    stats.on_trade(p(75), q(10));
    EXPECT_EQ(stats.vwap(), p(75));
}

// --- No trades: vwap returns 0 ---

TEST(OhlcvStats, VwapReturnsZeroWithNoTrades) {
    OhlcvStats stats;
    EXPECT_EQ(stats.vwap(), 0);
}

// --- Reset clears all fields ---

TEST(OhlcvStats, ResetClearsAllFields) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(5));
    stats.on_trade(p(200), q(3));
    stats.reset();

    EXPECT_EQ(stats.open,        0);
    EXPECT_EQ(stats.high,        0);
    EXPECT_EQ(stats.low,         0);
    EXPECT_EQ(stats.close,       0);
    EXPECT_EQ(stats.volume,      0);
    EXPECT_EQ(stats.turnover,    0);
    EXPECT_EQ(stats.trade_count, 0u);
    EXPECT_FALSE(stats.has_traded);
}

TEST(OhlcvStats, ResetThenNewTradeStartsFresh) {
    OhlcvStats stats;
    stats.on_trade(p(100), q(5));
    stats.reset();
    stats.on_trade(p(50), q(2));

    EXPECT_EQ(stats.open,  p(50));
    EXPECT_EQ(stats.high,  p(50));
    EXPECT_EQ(stats.low,   p(50));
    EXPECT_EQ(stats.close, p(50));
    EXPECT_EQ(stats.volume, q(2));
    EXPECT_EQ(stats.trade_count, 1u);
    EXPECT_TRUE(stats.has_traded);
}

}  // namespace exchange
