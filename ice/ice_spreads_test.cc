#include "ice/ice_spreads.h"

#include "ice/ice_products.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <unordered_map>

namespace exchange {
namespace ice {
namespace {

// Helper: build a map of outright instrument_id -> IceProductConfig.
std::unordered_map<uint32_t, IceProductConfig> outright_map() {
    std::unordered_map<uint32_t, IceProductConfig> m;
    for (auto& p : get_ice_products()) {
        m.emplace(p.instrument_id, p);
    }
    return m;
}

// Helper: find a spread by symbol.  Returns nullptr if not found.
// Caller must keep `spreads` alive for the pointer to remain valid.
const SpreadInstrumentConfig* find_spread(
    const std::vector<SpreadInstrumentConfig>& spreads,
    const std::string& symbol) {
    for (const auto& s : spreads) {
        if (s.symbol == symbol) return &s;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Basic structure
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, ReturnsFourSpreads) {
    auto spreads = get_ice_spread_products();
    EXPECT_EQ(spreads.size(), 4u);
}

TEST(IceSpreadsTest, UniqueSpreadIds) {
    auto spreads = get_ice_spread_products();
    std::set<uint32_t> ids;
    for (const auto& s : spreads) {
        EXPECT_TRUE(ids.insert(s.id).second)
            << "Duplicate spread id: " << s.id << " (" << s.symbol << ")";
    }
}

TEST(IceSpreadsTest, UniqueSymbols) {
    auto spreads = get_ice_spread_products();
    std::set<std::string> symbols;
    for (const auto& s : spreads) {
        EXPECT_TRUE(symbols.insert(s.symbol).second)
            << "Duplicate symbol: " << s.symbol;
    }
}

TEST(IceSpreadsTest, SpreadIdsDoNotOverlapOutrights) {
    auto outrights = outright_map();
    for (const auto& s : get_ice_spread_products()) {
        EXPECT_EQ(outrights.count(s.id), 0u)
            << "Spread id " << s.id << " collides with outright";
    }
}

TEST(IceSpreadsTest, AllLegsHavePositiveTickAndLot) {
    for (const auto& s : get_ice_spread_products()) {
        for (size_t i = 0; i < s.legs.size(); ++i) {
            EXPECT_GT(s.legs[i].tick_size, 0)
                << s.symbol << " leg " << i << " tick_size must be > 0";
            EXPECT_GT(s.legs[i].lot_size, 0)
                << s.symbol << " leg " << i << " lot_size must be > 0";
        }
    }
}

TEST(IceSpreadsTest, FirstLegRatioPositive) {
    for (const auto& s : get_ice_spread_products()) {
        ASSERT_GE(s.legs.size(), 2u) << s.symbol;
        EXPECT_GT(s.legs[0].ratio, 0)
            << s.symbol << " first leg ratio must be positive (convention)";
    }
}

// ---------------------------------------------------------------------------
// Each leg instrument_id references a real ICE outright product, and the
// leg tick_size/lot_size match that outright.
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, LegInstrumentIdsMatchOutrightProducts) {
    auto outrights = outright_map();
    for (const auto& s : get_ice_spread_products()) {
        for (size_t i = 0; i < s.legs.size(); ++i) {
            uint32_t leg_id = s.legs[i].instrument_id;
            ASSERT_NE(outrights.count(leg_id), 0u)
                << s.symbol << " leg " << i
                << " references unknown outright instrument_id " << leg_id;
            EXPECT_EQ(s.legs[i].tick_size, outrights[leg_id].tick_size)
                << s.symbol << " leg " << i << " tick_size mismatch";
            EXPECT_EQ(s.legs[i].lot_size, outrights[leg_id].lot_size)
                << s.symbol << " leg " << i << " lot_size mismatch";
        }
    }
}

// ---------------------------------------------------------------------------
// Strategy construction and tick normalization
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, BuildStrategySucceeds) {
    for (const auto& s : get_ice_spread_products()) {
        auto strategy = s.build_strategy();
        EXPECT_GT(strategy.tick_size(), 0) << s.symbol;
        EXPECT_GT(strategy.lot_size(), 0) << s.symbol;
        EXPECT_EQ(strategy.leg_count(), s.legs.size()) << s.symbol;
    }
}

TEST(IceSpreadsTest, RegistrationSucceeds) {
    SpreadStrategyRegistry registry;
    auto outrights = outright_map();
    registry.set_instrument_validator(
        [&](uint32_t id) { return outrights.count(id) != 0; });

    for (const auto& s : get_ice_spread_products()) {
        std::string err = s.register_in(registry);
        EXPECT_TRUE(err.empty())
            << s.symbol << " registration failed: " << err;
    }
    EXPECT_EQ(registry.size(), 4u);
}

// ---------------------------------------------------------------------------
// Brent calendar spread: Buy B front, Sell B back
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, BrentCalendarSpread) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "B-CAL");
    ASSERT_NE(cfg, nullptr) << "B-CAL not found";

    EXPECT_EQ(cfg->strategy_type, StrategyType::CalendarSpread);
    ASSERT_EQ(cfg->legs.size(), 2u);

    // Both legs reference Brent (instrument_id = 1)
    EXPECT_EQ(cfg->legs[0].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 1u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);

    // Tick: $0.01/bbl -> 100 both legs, GCD(100*1, 100*1) = 100
    auto strategy = cfg->build_strategy();
    EXPECT_EQ(strategy.tick_size(), 100);
}

// ---------------------------------------------------------------------------
// Natural Gas (NBP) calendar spread
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, NatGasCalendarSpread) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "M-CAL");
    ASSERT_NE(cfg, nullptr) << "M-CAL not found";

    EXPECT_EQ(cfg->strategy_type, StrategyType::CalendarSpread);
    ASSERT_EQ(cfg->legs.size(), 2u);

    EXPECT_EQ(cfg->legs[0].instrument_id, 3u);
    EXPECT_EQ(cfg->legs[1].instrument_id, 3u);
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);

    auto strategy = cfg->build_strategy();
    EXPECT_EQ(strategy.tick_size(), 100);
}

// ---------------------------------------------------------------------------
// Gasoil/Brent crack spread: Buy Gasoil (G), Sell Brent (B)
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, GasoilCrackSpread) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "G-B-CRK");
    ASSERT_NE(cfg, nullptr) << "G-B-CRK not found";

    EXPECT_EQ(cfg->strategy_type, StrategyType::InterCommodity);
    ASSERT_EQ(cfg->legs.size(), 2u);

    EXPECT_EQ(cfg->legs[0].instrument_id, 2u);  // Gasoil
    EXPECT_EQ(cfg->legs[1].instrument_id, 1u);  // Brent
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);

    // Tick normalization: GCD(2500*1, 100*1) = 100
    auto strategy = cfg->build_strategy();
    EXPECT_EQ(strategy.tick_size(), 100);
}

// ---------------------------------------------------------------------------
// Brent/Gasoil crack: Buy Brent (B), Sell Gasoil (G)
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, BrentGasoilCrackSpread) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "B-G-CRK");
    ASSERT_NE(cfg, nullptr) << "B-G-CRK not found";

    EXPECT_EQ(cfg->strategy_type, StrategyType::InterCommodity);
    ASSERT_EQ(cfg->legs.size(), 2u);

    EXPECT_EQ(cfg->legs[0].instrument_id, 1u);  // Brent
    EXPECT_EQ(cfg->legs[1].instrument_id, 2u);  // Gasoil
    EXPECT_EQ(cfg->legs[0].ratio, 1);
    EXPECT_EQ(cfg->legs[1].ratio, -1);

    // GCD(100*1, 2500*1) = 100
    auto strategy = cfg->build_strategy();
    EXPECT_EQ(strategy.tick_size(), 100);
}

// ---------------------------------------------------------------------------
// Spread price computation sanity
// ---------------------------------------------------------------------------

TEST(IceSpreadsTest, BrentCalendarSpreadPriceComputation) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "B-CAL");
    ASSERT_NE(cfg, nullptr);
    auto strategy = cfg->build_strategy();

    // Front at $70.50, Back at $71.20 (fixed-point * 10000)
    // Spread = front - back = 70.50 - 71.20 = -$0.70 = -7000
    Price legs[] = {705000, 712000};
    EXPECT_EQ(strategy.compute_spread_price(legs, 2), -7000);
}

TEST(IceSpreadsTest, GasoilCrackSpreadPriceComputation) {
    auto spreads = get_ice_spread_products();
    const auto* cfg = find_spread(spreads, "G-B-CRK");
    ASSERT_NE(cfg, nullptr);
    auto strategy = cfg->build_strategy();

    // Gasoil at $650.00, Brent at $70.50
    // Spread = Gasoil - Brent = 650.00 - 70.50 = $579.50 = 5795000
    Price legs[] = {6500000, 705000};
    EXPECT_EQ(strategy.compute_spread_price(legs, 2), 5795000);
}

}  // namespace
}  // namespace ice
}  // namespace exchange
