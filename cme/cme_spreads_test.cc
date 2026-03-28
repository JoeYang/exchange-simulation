#include "cme/cme_spreads.h"
#include "exchange-sim/spread_book/spread_strategy_registry.h"

#include <gtest/gtest.h>

namespace exchange {
namespace cme {
namespace {

// Validate all outright instrument IDs used by spreads exist in the product
// table.
std::unordered_set<uint32_t> outright_ids() {
    std::unordered_set<uint32_t> ids;
    for (const auto& p : get_cme_products()) {
        ids.insert(p.instrument_id);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Calendar spreads
// ---------------------------------------------------------------------------

TEST(CmeSpreadsTest, CalendarSpreadCount) {
    auto cals = get_cme_calendar_spreads();
    EXPECT_GE(cals.size(), 3u);
}

TEST(CmeSpreadsTest, CalendarSpreadLegsAreValid) {
    auto known = outright_ids();
    for (const auto& cfg : get_cme_calendar_spreads()) {
        EXPECT_EQ(cfg.legs.size(), 2u) << cfg.symbol;
        EXPECT_EQ(cfg.legs[0].ratio, 1) << cfg.symbol;
        EXPECT_EQ(cfg.legs[1].ratio, -1) << cfg.symbol;
        EXPECT_EQ(cfg.strategy_type, StrategyType::CalendarSpread) << cfg.symbol;
        for (const auto& leg : cfg.legs) {
            EXPECT_TRUE(known.count(leg.instrument_id))
                << cfg.symbol << " leg " << leg.instrument_id << " not in products";
            EXPECT_GT(leg.tick_size, 0) << cfg.symbol;
            EXPECT_GT(leg.lot_size, 0) << cfg.symbol;
        }
    }
}

TEST(CmeSpreadsTest, CalendarSpreadBuildStrategy) {
    auto cals = get_cme_calendar_spreads();
    auto strategy = cals[0].build_strategy();
    EXPECT_EQ(strategy.type(), StrategyType::CalendarSpread);
    EXPECT_EQ(strategy.leg_count(), 2u);
    EXPECT_GT(strategy.tick_size(), 0);
    EXPECT_GT(strategy.lot_size(), 0);
}

// ---------------------------------------------------------------------------
// Butterfly spreads
// ---------------------------------------------------------------------------

TEST(CmeSpreadsTest, ButterflySpreadLegs) {
    auto bfs = get_cme_butterfly_spreads();
    EXPECT_GE(bfs.size(), 1u);

    for (const auto& cfg : bfs) {
        EXPECT_EQ(cfg.legs.size(), 3u) << cfg.symbol;
        EXPECT_EQ(cfg.legs[0].ratio, 1) << cfg.symbol;
        EXPECT_EQ(cfg.legs[1].ratio, -2) << cfg.symbol;
        EXPECT_EQ(cfg.legs[2].ratio, 1) << cfg.symbol;
        EXPECT_EQ(cfg.strategy_type, StrategyType::Butterfly) << cfg.symbol;
    }
}

TEST(CmeSpreadsTest, ButterflyTickSize) {
    auto strategy = get_cme_butterfly_spreads()[0].build_strategy();
    // All ES-like legs: tick=2500, multipliers 1,2,1
    // GCD(2500*1, 2500*2, 2500*1) = 2500
    EXPECT_EQ(strategy.tick_size(), 2500);
}

// ---------------------------------------------------------------------------
// Condor spreads
// ---------------------------------------------------------------------------

TEST(CmeSpreadsTest, CondorSpreadLegs) {
    auto cds = get_cme_condor_spreads();
    EXPECT_GE(cds.size(), 1u);

    for (const auto& cfg : cds) {
        EXPECT_EQ(cfg.legs.size(), 4u) << cfg.symbol;
        EXPECT_EQ(cfg.legs[0].ratio, 1) << cfg.symbol;
        EXPECT_EQ(cfg.legs[1].ratio, -1) << cfg.symbol;
        EXPECT_EQ(cfg.legs[2].ratio, -1) << cfg.symbol;
        EXPECT_EQ(cfg.legs[3].ratio, 1) << cfg.symbol;
        EXPECT_EQ(cfg.strategy_type, StrategyType::Condor) << cfg.symbol;
    }
}

TEST(CmeSpreadsTest, CondorTickSize) {
    auto strategy = get_cme_condor_spreads()[0].build_strategy();
    // Leg ticks: 2500, 2500, 2500, 5  (all multiplier=1)
    // GCD(2500, 2500, 2500, 5) = 5
    EXPECT_EQ(strategy.tick_size(), 5);
}

// ---------------------------------------------------------------------------
// Crack (inter-commodity) spreads
// ---------------------------------------------------------------------------

TEST(CmeSpreadsTest, CrackSpreadLegs) {
    auto crs = get_cme_crack_spreads();
    EXPECT_GE(crs.size(), 1u);

    for (const auto& cfg : crs) {
        EXPECT_EQ(cfg.legs.size(), 2u) << cfg.symbol;
        EXPECT_EQ(cfg.strategy_type, StrategyType::InterCommodity) << cfg.symbol;
    }
}

TEST(CmeSpreadsTest, CrackSpreadTickSize) {
    auto strategy = get_cme_crack_spreads()[0].build_strategy();
    // CL tick=100, GC tick=1000 => GCD(100,1000) = 100
    EXPECT_EQ(strategy.tick_size(), 100);
}

// ---------------------------------------------------------------------------
// Combined: all spreads
// ---------------------------------------------------------------------------

TEST(CmeSpreadsTest, AllSpreadsNoDuplicateIds) {
    auto all = get_all_cme_spreads();
    std::unordered_set<uint32_t> ids;
    for (const auto& cfg : all) {
        EXPECT_TRUE(ids.insert(cfg.id).second)
            << "Duplicate spread ID: " << cfg.id;
    }
    // At least 6 total (3 cal + 1 bf + 1 cd + 1 crack)
    EXPECT_GE(all.size(), 6u);
}

TEST(CmeSpreadsTest, AllSpreadsRegisterInRegistry) {
    SpreadStrategyRegistry registry;
    auto known = outright_ids();
    registry.set_instrument_validator([&](uint32_t id) {
        return known.count(id) != 0;
    });

    for (const auto& cfg : get_all_cme_spreads()) {
        auto err = cfg.register_in(registry);
        EXPECT_TRUE(err.empty()) << cfg.symbol << ": " << err;
    }

    EXPECT_GE(registry.size(), 6u);
}

TEST(CmeSpreadsTest, SpreadIdsDoNotOverlapOutrights) {
    auto known = outright_ids();
    for (const auto& cfg : get_all_cme_spreads()) {
        EXPECT_FALSE(known.count(cfg.id))
            << "Spread ID " << cfg.id << " collides with outright";
    }
}

}  // namespace
}  // namespace cme
}  // namespace exchange
