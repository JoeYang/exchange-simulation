#include "krx/krx_spreads.h"
#include "krx/krx_products.h"
#include "exchange-sim/spread_book/spread_strategy_registry.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <unordered_map>

namespace exchange {
namespace krx {
namespace {

std::unordered_map<uint32_t, KrxProductConfig> outright_map() {
    std::unordered_map<uint32_t, KrxProductConfig> m;
    for (auto& p : get_krx_products()) m.emplace(p.instrument_id, p);
    return m;
}

const SpreadInstrumentConfig* find_spread(
    const std::vector<SpreadInstrumentConfig>& spreads,
    const std::string& symbol) {
    for (const auto& s : spreads) if (s.symbol == symbol) return &s;
    return nullptr;
}

// --- Basic structure ---

TEST(KrxSpreadsTest, ReturnsFourSpreads) {
    EXPECT_EQ(get_krx_spread_products().size(), 4u);
}

TEST(KrxSpreadsTest, UniqueSpreadIds) {
    std::set<uint32_t> ids;
    for (const auto& s : get_krx_spread_products())
        EXPECT_TRUE(ids.insert(s.id).second) << "Dup id: " << s.id;
}

TEST(KrxSpreadsTest, UniqueSymbols) {
    std::set<std::string> symbols;
    for (const auto& s : get_krx_spread_products())
        EXPECT_TRUE(symbols.insert(s.symbol).second) << "Dup: " << s.symbol;
}

TEST(KrxSpreadsTest, SpreadIdsDoNotOverlapOutrights) {
    auto outrights = outright_map();
    for (const auto& s : get_krx_spread_products())
        EXPECT_EQ(outrights.count(s.id), 0u) << "Collision: " << s.id;
}

TEST(KrxSpreadsTest, AllLegsHavePositiveTickAndLot) {
    for (const auto& s : get_krx_spread_products()) {
        for (size_t i = 0; i < s.legs.size(); ++i) {
            EXPECT_GT(s.legs[i].tick_size, 0) << s.symbol << " leg " << i;
            EXPECT_GT(s.legs[i].lot_size, 0) << s.symbol << " leg " << i;
        }
    }
}

TEST(KrxSpreadsTest, FirstLegRatioPositive) {
    for (const auto& s : get_krx_spread_products()) {
        ASSERT_GE(s.legs.size(), 2u) << s.symbol;
        EXPECT_GT(s.legs[0].ratio, 0) << s.symbol;
    }
}

// --- Leg IDs match outright products with correct tick/lot ---

TEST(KrxSpreadsTest, LegInstrumentIdsMatchOutrightProducts) {
    auto outrights = outright_map();
    for (const auto& s : get_krx_spread_products()) {
        for (size_t i = 0; i < s.legs.size(); ++i) {
            uint32_t leg_id = s.legs[i].instrument_id;
            ASSERT_NE(outrights.count(leg_id), 0u)
                << s.symbol << " leg " << i << " unknown id " << leg_id;
            EXPECT_EQ(s.legs[i].tick_size, outrights[leg_id].tick_size)
                << s.symbol << " leg " << i << " tick mismatch";
            EXPECT_EQ(s.legs[i].lot_size, outrights[leg_id].lot_size)
                << s.symbol << " leg " << i << " lot mismatch";
        }
    }
}

// --- Strategy construction and tick normalization ---

TEST(KrxSpreadsTest, BuildStrategySucceeds) {
    for (const auto& s : get_krx_spread_products()) {
        auto strat = s.build_strategy();
        EXPECT_GT(strat.tick_size(), 0) << s.symbol;
        EXPECT_GT(strat.lot_size(), 0) << s.symbol;
        EXPECT_EQ(strat.leg_count(), s.legs.size()) << s.symbol;
    }
}

TEST(KrxSpreadsTest, RegistrationSucceeds) {
    SpreadStrategyRegistry registry;
    auto outrights = outright_map();
    registry.set_instrument_validator(
        [&](uint32_t id) { return outrights.count(id) != 0; });
    for (const auto& s : get_krx_spread_products()) {
        auto err = s.register_in(registry);
        EXPECT_TRUE(err.empty()) << s.symbol << ": " << err;
    }
    EXPECT_EQ(registry.size(), 4u);
}

// --- KS-CAL: KOSPI200 Calendar Spread ---

TEST(KrxSpreadsTest, KsCalendarSpread) {
    auto spreads = get_krx_spread_products();
    const auto* cfg = find_spread(spreads, "KS-CAL");
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->strategy_type, StrategyType::CalendarSpread);
    ASSERT_EQ(cfg->legs.size(), 2u);
    EXPECT_EQ(cfg->legs[0].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);
    // GCD(500*1, 500*1) = 500
    EXPECT_EQ(cfg->build_strategy().tick_size(), 500);
}

// --- KTB-CAL: KTB 3-Year Calendar Spread ---

TEST(KrxSpreadsTest, KtbCalendarSpread) {
    auto spreads = get_krx_spread_products();
    const auto* cfg = find_spread(spreads, "KTB-CAL");
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->strategy_type, StrategyType::CalendarSpread);
    ASSERT_EQ(cfg->legs.size(), 2u);
    EXPECT_EQ(cfg->legs[0].instrument_id, 9u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 9u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);
    EXPECT_EQ(cfg->build_strategy().tick_size(), 100);
}

// --- KS-BF: KOSPI200 Butterfly (+1 near, -2 mid, +1 far) ---

TEST(KrxSpreadsTest, KsButterflySpread) {
    auto spreads = get_krx_spread_products();
    const auto* cfg = find_spread(spreads, "KS-BF");
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->strategy_type, StrategyType::Butterfly);
    ASSERT_EQ(cfg->legs.size(), 3u);
    EXPECT_EQ(cfg->legs[0].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[2].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -2);
    EXPECT_EQ(cfg->legs[2].ratio, 1);
    // GCD(500, 1000, 500) = 500
    EXPECT_EQ(cfg->build_strategy().tick_size(), 500);
}

// --- KS-MKS: KOSPI200/Mini inter-product spread ---

TEST(KrxSpreadsTest, KsMksInterProductSpread) {
    auto spreads = get_krx_spread_products();
    const auto* cfg = find_spread(spreads, "KS-MKS");
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->strategy_type, StrategyType::InterCommodity);
    ASSERT_EQ(cfg->legs.size(), 2u);
    EXPECT_EQ(cfg->legs[0].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 2u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -5);
    EXPECT_EQ(cfg->legs[1].price_multiplier, 5);
    // Tick override to 200 for Mini-KOSPI200 granularity
    EXPECT_EQ(cfg->build_strategy().tick_size(), 200);
}

// --- Spread price computation ---

TEST(KrxSpreadsTest, KsCalendarSpreadPriceComputation) {
    auto spreads = get_krx_spread_products();
    auto strategy = find_spread(spreads, "KS-CAL")->build_strategy();
    // Front=350.05 (3500500), Back=349.55 (3495500), spread=+0.50 (5000)
    Price legs[] = {3500500, 3495500};
    EXPECT_EQ(strategy.compute_spread_price(legs, 2), 5000);
}

TEST(KrxSpreadsTest, KsMksSpreadPriceComputation) {
    auto spreads = get_krx_spread_products();
    auto strategy = find_spread(spreads, "KS-MKS")->build_strategy();
    // KS=350.00, MKS=70.00 => spread = 3500000 - 700000*5 = 0
    Price legs[] = {3500000, 700000};
    EXPECT_EQ(strategy.compute_spread_price(legs, 2), 0);
    // MKS=69.98 => spread = 3500000 - 699800*5 = 1000
    Price legs2[] = {3500000, 699800};
    EXPECT_EQ(strategy.compute_spread_price(legs2, 2), 1000);
}

}  // namespace
}  // namespace krx
}  // namespace exchange
