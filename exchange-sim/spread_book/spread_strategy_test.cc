#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "exchange-sim/spread_book/spread_strategy.h"
#include "exchange-sim/spread_book/spread_strategy_registry.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// ---------------------------------------------------------------------------
// SpreadStrategy tests
// ---------------------------------------------------------------------------

// Helper: build a 2-leg calendar spread (ES Jun vs Sep).
// Both legs have identical tick/lot (ES: tick=2500, lot=10000).
SpreadStrategy make_calendar_spread() {
    return SpreadStrategy(
        StrategyType::CalendarSpread,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        });
}

// Helper: build a 3-leg butterfly (+1 near, -2 mid, +1 far).
// All legs same tick/lot.
SpreadStrategy make_butterfly() {
    return SpreadStrategy(
        StrategyType::Butterfly,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -2, .price_multiplier = 2,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        });
}

// Helper: build inter-commodity spread with different tick sizes.
// CL: tick=100 (0.01/bbl), RB: tick=42 (fictional for testing GCD).
SpreadStrategy make_inter_commodity() {
    return SpreadStrategy(
        StrategyType::InterCommodity,
        {
            StrategyLeg{.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 10000},
            StrategyLeg{.instrument_id = 4, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 42,  .lot_size = 10000},
        });
}

TEST(SpreadStrategyTest, CalendarSpreadBasics) {
    auto s = make_calendar_spread();
    EXPECT_EQ(s.type(), StrategyType::CalendarSpread);
    EXPECT_EQ(s.leg_count(), 2u);
    EXPECT_EQ(s.legs()[0].instrument_id, 1u);
    EXPECT_EQ(s.legs()[1].instrument_id, 2u);
    EXPECT_EQ(s.legs()[0].ratio, 1);
    EXPECT_EQ(s.legs()[1].ratio, -1);
}

TEST(SpreadStrategyTest, CalendarSpreadTickSize) {
    auto s = make_calendar_spread();
    // Both legs: tick=2500, multiplier=1 => GCD(2500, 2500) = 2500
    EXPECT_EQ(s.tick_size(), 2500);
}

TEST(SpreadStrategyTest, CalendarSpreadLotSize) {
    auto s = make_calendar_spread();
    // Both legs: lot=10000, abs(ratio)=1
    // factor = lot / gcd(lot, 1) = 10000 / 1 = 10000
    // LCM(10000, 10000) = 10000
    EXPECT_EQ(s.lot_size(), 10000);
}

TEST(SpreadStrategyTest, CalendarSpreadPrice) {
    auto s = make_calendar_spread();
    // Front at 1000000 (100.0000), back at 1005000 (100.5000)
    // Spread = front * 1 * (+1) + back * 1 * (-1) = -5000 (-0.5000)
    Price legs[] = {1000000, 1005000};
    EXPECT_EQ(s.compute_spread_price(legs, 2), -5000);
}

TEST(SpreadStrategyTest, CalendarSpreadLegQuantity) {
    auto s = make_calendar_spread();
    // 1 spread-lot = 10000 fixed-point
    EXPECT_EQ(s.leg_quantity(0, 10000), 10000);  // abs(1) * 10000
    EXPECT_EQ(s.leg_quantity(1, 10000), 10000);  // abs(-1) * 10000
    // 5 spread-lots
    EXPECT_EQ(s.leg_quantity(0, 50000), 50000);
}

TEST(SpreadStrategyTest, CalendarSpreadLegSide) {
    auto s = make_calendar_spread();
    // Buy spread: leg0 (ratio +1) = Buy, leg1 (ratio -1) = Sell
    EXPECT_EQ(s.leg_side(0, Side::Buy), Side::Buy);
    EXPECT_EQ(s.leg_side(1, Side::Buy), Side::Sell);
    // Sell spread: inverted
    EXPECT_EQ(s.leg_side(0, Side::Sell), Side::Sell);
    EXPECT_EQ(s.leg_side(1, Side::Sell), Side::Buy);
}

TEST(SpreadStrategyTest, ButterflyTickSize) {
    auto s = make_butterfly();
    // Leg contributions: 2500*1=2500, 2500*2=5000, 2500*1=2500
    // GCD(2500, 5000, 2500) = 2500
    EXPECT_EQ(s.tick_size(), 2500);
}

TEST(SpreadStrategyTest, ButterflyLotSize) {
    auto s = make_butterfly();
    // Leg 0: lot=10000, abs(ratio)=1 => factor = 10000/gcd(10000,1) = 10000
    // Leg 1: lot=10000, abs(ratio)=2 => factor = 10000/gcd(10000,2) = 5000
    // Leg 2: same as leg 0 => 10000
    // LCM(10000, 5000, 10000) = 10000
    EXPECT_EQ(s.lot_size(), 10000);
}

TEST(SpreadStrategyTest, ButterflyPrice) {
    auto s = make_butterfly();
    // Near=1000000, Mid=1002500, Far=1005000
    // Spread = 1000000*1*(+1) + 1002500*2*(-1) + 1005000*1*(+1)
    //        = 1000000 - 2005000 + 1005000 = 0
    Price legs[] = {1000000, 1002500, 1005000};
    EXPECT_EQ(s.compute_spread_price(legs, 3), 0);

    // Non-zero butterfly value
    // Near=1000000, Mid=1005000, Far=1007500
    // = 1000000 - 2*1005000 + 1007500 = 1000000 - 2010000 + 1007500 = -2500
    Price legs2[] = {1000000, 1005000, 1007500};
    EXPECT_EQ(s.compute_spread_price(legs2, 3), -2500);
}

TEST(SpreadStrategyTest, ButterflyLegQuantity) {
    auto s = make_butterfly();
    Quantity spread_qty = 10000;  // 1 spread-lot
    EXPECT_EQ(s.leg_quantity(0, spread_qty), 10000);   // abs(+1) * 10000
    EXPECT_EQ(s.leg_quantity(1, spread_qty), 20000);   // abs(-2) * 10000
    EXPECT_EQ(s.leg_quantity(2, spread_qty), 10000);   // abs(+1) * 10000
}

TEST(SpreadStrategyTest, ButterflyLegSide) {
    auto s = make_butterfly();
    // Buy butterfly: leg0 (+1)=Buy, leg1 (-2)=Sell, leg2 (+1)=Buy
    EXPECT_EQ(s.leg_side(0, Side::Buy), Side::Buy);
    EXPECT_EQ(s.leg_side(1, Side::Buy), Side::Sell);
    EXPECT_EQ(s.leg_side(2, Side::Buy), Side::Buy);
}

TEST(SpreadStrategyTest, InterCommodityDifferentTicks) {
    auto s = make_inter_commodity();
    // Leg contributions: 100*1=100, 42*1=42
    // GCD(100, 42) = 2
    EXPECT_EQ(s.tick_size(), 2);
}

TEST(SpreadStrategyTest, InterCommodityLotSize) {
    auto s = make_inter_commodity();
    // Both lots = 10000, both abs(ratio) = 1
    // factor = 10000/1 = 10000 each
    // LCM(10000, 10000) = 10000
    EXPECT_EQ(s.lot_size(), 10000);
}

TEST(SpreadStrategyTest, ComputeSpreadPriceWrongCount) {
    auto s = make_calendar_spread();
    Price legs[] = {1000000};
    // Wrong count => returns 0
    EXPECT_EQ(s.compute_spread_price(legs, 1), 0);
}

TEST(SpreadStrategyTest, LegQuantityOutOfBounds) {
    auto s = make_calendar_spread();
    EXPECT_EQ(s.leg_quantity(5, 10000), 0);
}

TEST(SpreadStrategyTest, TickSizeOverride) {
    auto s = SpreadStrategy(
        StrategyType::CalendarSpread,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        },
        /*tick_size_override=*/5000);
    EXPECT_EQ(s.tick_size(), 5000);
    EXPECT_EQ(s.lot_size(), 10000);  // auto-computed
}

TEST(SpreadStrategyTest, LotSizeOverride) {
    auto s = SpreadStrategy(
        StrategyType::CalendarSpread,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        },
        /*tick_size_override=*/0,
        /*lot_size_override=*/20000);
    EXPECT_EQ(s.tick_size(), 2500);
    EXPECT_EQ(s.lot_size(), 20000);
}

// Different lot sizes: leg0 lot=10000, leg1 lot=20000
// With ratio +1, -1: factor0 = 10000, factor1 = 20000
// LCM(10000, 20000) = 20000
TEST(SpreadStrategyTest, DifferentLotSizes) {
    auto s = SpreadStrategy(
        StrategyType::InterCommodity,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 20000},
        });
    EXPECT_EQ(s.lot_size(), 20000);
}

// Ratio=2 with lot_size=10000:
// factor = 10000 / gcd(10000, 2) = 10000 / 2 = 5000
TEST(SpreadStrategyTest, NonUnityRatioLotSize) {
    auto s = SpreadStrategy(
        StrategyType::Butterfly,
        {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -2, .price_multiplier = 2,
                        .tick_size = 100, .lot_size = 10000},
            StrategyLeg{.instrument_id = 3, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 10000},
        });
    // leg0: 10000/gcd(10000,1)=10000
    // leg1: 10000/gcd(10000,2)=5000
    // leg2: 10000/gcd(10000,1)=10000
    // LCM(10000,5000,10000) = 10000
    EXPECT_EQ(s.lot_size(), 10000);
}

// ---------------------------------------------------------------------------
// SpreadStrategyRegistry tests
// ---------------------------------------------------------------------------

class SpreadStrategyRegistryTest : public ::testing::Test {
protected:
    SpreadStrategyRegistry registry_;

    void SetUp() override {
        // Instrument IDs 1-10 are valid outright instruments.
        registry_.set_instrument_validator([](uint32_t id) {
            return id >= 1 && id <= 10;
        });
    }
};

TEST_F(SpreadStrategyRegistryTest, RegisterAndLookup) {
    auto err = registry_.register_strategy(100, make_calendar_spread());
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(registry_.size(), 1u);

    const auto* s = registry_.lookup(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type(), StrategyType::CalendarSpread);
    EXPECT_EQ(s->leg_count(), 2u);
}

TEST_F(SpreadStrategyRegistryTest, IsSpread) {
    registry_.register_strategy(100, make_calendar_spread());
    EXPECT_TRUE(registry_.is_spread(100));
    EXPECT_FALSE(registry_.is_spread(1));   // outright
    EXPECT_FALSE(registry_.is_spread(999)); // unknown
}

TEST_F(SpreadStrategyRegistryTest, LookupUnknown) {
    EXPECT_EQ(registry_.lookup(999), nullptr);
}

TEST_F(SpreadStrategyRegistryTest, RejectDuplicateId) {
    auto err1 = registry_.register_strategy(100, make_calendar_spread());
    EXPECT_TRUE(err1.empty());

    auto err2 = registry_.register_strategy(100, make_butterfly());
    EXPECT_FALSE(err2.empty());
    EXPECT_NE(err2.find("Duplicate"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectTooFewLegs) {
    SpreadStrategy single_leg(StrategyType::Custom,
        {StrategyLeg{.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                     .tick_size = 100, .lot_size = 10000}});
    auto err = registry_.register_strategy(100, std::move(single_leg));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("2 legs"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectNegativeFirstLeg) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = registry_.register_strategy(100, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("positive"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectZeroRatio) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = 0, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = registry_.register_strategy(100, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("zero ratio"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectInvalidTickSize) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 0, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = registry_.register_strategy(100, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("tick_size"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectInvalidLotSize) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 0},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = registry_.register_strategy(100, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("lot_size"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectUnknownLegInstrument) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1,  .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 99, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = registry_.register_strategy(100, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown leg"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RejectLegIdEqualsSpreadId) {
    SpreadStrategy bad(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    // Spread ID = 1, which is also leg0's instrument_id
    auto err = registry_.register_strategy(1, std::move(bad));
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("cannot equal"), std::string::npos);
}

TEST_F(SpreadStrategyRegistryTest, RegisterMultipleStrategies) {
    auto e1 = registry_.register_strategy(100, make_calendar_spread());
    auto e2 = registry_.register_strategy(101, make_butterfly());
    EXPECT_TRUE(e1.empty()) << e1;
    EXPECT_TRUE(e2.empty()) << e2;
    EXPECT_EQ(registry_.size(), 2u);

    EXPECT_NE(registry_.lookup(100), nullptr);
    EXPECT_NE(registry_.lookup(101), nullptr);
    EXPECT_EQ(registry_.lookup(100)->type(), StrategyType::CalendarSpread);
    EXPECT_EQ(registry_.lookup(101)->type(), StrategyType::Butterfly);
}

TEST_F(SpreadStrategyRegistryTest, NoValidatorAllowsAnyLeg) {
    SpreadStrategyRegistry reg_no_validator;
    // No validator set -- should accept any leg instrument ID
    SpreadStrategy s(StrategyType::CalendarSpread, {
        StrategyLeg{.instrument_id = 999, .ratio = 1,  .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
        StrategyLeg{.instrument_id = 888, .ratio = -1, .price_multiplier = 1,
                    .tick_size = 100, .lot_size = 10000},
    });
    auto err = reg_no_validator.register_strategy(100, std::move(s));
    EXPECT_TRUE(err.empty()) << err;
}

// ---------------------------------------------------------------------------
// SpreadInstrumentConfig tests
// ---------------------------------------------------------------------------

TEST(SpreadInstrumentConfigTest, BuildStrategy) {
    SpreadInstrumentConfig cfg{
        .id = 100,
        .symbol = "ESU5-ESZ5",
        .strategy_type = StrategyType::CalendarSpread,
        .legs = {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        },
    };

    auto strategy = cfg.build_strategy();
    EXPECT_EQ(strategy.type(), StrategyType::CalendarSpread);
    EXPECT_EQ(strategy.leg_count(), 2u);
    EXPECT_EQ(strategy.tick_size(), 2500);
    EXPECT_EQ(strategy.lot_size(), 10000);
}

TEST(SpreadInstrumentConfigTest, RegisterIn) {
    SpreadStrategyRegistry registry;
    registry.set_instrument_validator([](uint32_t id) {
        return id >= 1 && id <= 10;
    });

    SpreadInstrumentConfig cfg{
        .id = 100,
        .symbol = "ESU5-ESZ5",
        .strategy_type = StrategyType::CalendarSpread,
        .legs = {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 2500, .lot_size = 10000},
        },
    };

    auto err = cfg.register_in(registry);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NE(registry.lookup(100), nullptr);
}

TEST(SpreadInstrumentConfigTest, RegisterWithOverrides) {
    SpreadStrategyRegistry registry;

    SpreadInstrumentConfig cfg{
        .id = 100,
        .symbol = "CL-RB",
        .strategy_type = StrategyType::InterCommodity,
        .legs = {
            StrategyLeg{.instrument_id = 1, .ratio = 1,  .price_multiplier = 1,
                        .tick_size = 100, .lot_size = 10000},
            StrategyLeg{.instrument_id = 2, .ratio = -1, .price_multiplier = 1,
                        .tick_size = 42,  .lot_size = 10000},
        },
        .tick_size_override = 50,
        .lot_size_override = 10000,
    };

    auto err = cfg.register_in(registry);
    EXPECT_TRUE(err.empty()) << err;
    const auto* s = registry.lookup(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->tick_size(), 50);
    EXPECT_EQ(s->lot_size(), 10000);
}

}  // namespace
}  // namespace exchange
