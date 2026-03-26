#include "ice/ice_products.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace exchange {
namespace ice {
namespace {

TEST(IceProductsTest, ReturnsAllTenProducts) {
    auto products = get_ice_products();
    EXPECT_EQ(products.size(), 10u);
}

TEST(IceProductsTest, UniqueInstrumentIds) {
    auto products = get_ice_products();
    std::set<uint32_t> ids;
    for (const auto& p : products) {
        EXPECT_TRUE(ids.insert(p.instrument_id).second)
            << "Duplicate instrument_id: " << p.instrument_id
            << " (" << p.symbol << ")";
    }
}

TEST(IceProductsTest, UniqueSymbols) {
    auto products = get_ice_products();
    std::set<std::string> symbols;
    for (const auto& p : products) {
        EXPECT_TRUE(symbols.insert(p.symbol).second)
            << "Duplicate symbol: " << p.symbol;
    }
}

TEST(IceProductsTest, TickSizesPositive) {
    for (const auto& p : get_ice_products()) {
        EXPECT_GT(p.tick_size, 0) << p.symbol << " tick_size must be > 0";
    }
}

TEST(IceProductsTest, LotSizesAreOneContract) {
    for (const auto& p : get_ice_products()) {
        EXPECT_EQ(p.lot_size, 10000)
            << p.symbol << " lot_size should be 10000 (1 contract)";
    }
}

TEST(IceProductsTest, MaxOrderSizePositive) {
    for (const auto& p : get_ice_products()) {
        EXPECT_GT(p.max_order_size, 0) << p.symbol;
    }
}

TEST(IceProductsTest, IplWidthsPositive) {
    for (const auto& p : get_ice_products()) {
        EXPECT_GT(p.ipl_width, 0) << p.symbol << " ipl_width must be > 0";
    }
}

TEST(IceProductsTest, IplHoldMsPositive) {
    for (const auto& p : get_ice_products()) {
        EXPECT_GT(p.ipl_hold_ms, 0) << p.symbol;
    }
}

TEST(IceProductsTest, SettlementWindowPositive) {
    for (const auto& p : get_ice_products()) {
        EXPECT_GT(p.settlement_window_secs, 0) << p.symbol;
    }
}

// Verify STIR products use GTBPR, all others use FIFO.
TEST(IceProductsTest, StirProductsUseGtbpr) {
    for (const auto& p : get_ice_products()) {
        if (p.product_group == "STIR") {
            EXPECT_EQ(p.match_algo, IceMatchAlgo::GTBPR) << p.symbol;
        } else {
            EXPECT_EQ(p.match_algo, IceMatchAlgo::FIFO) << p.symbol;
        }
    }
}

// Verify specific well-known products exist and have expected values.
TEST(IceProductsTest, BrentConfig) {
    auto products = get_ice_products();
    const IceProductConfig* brent = nullptr;
    for (const auto& p : products) {
        if (p.symbol == "B") { brent = &p; break; }
    }
    ASSERT_NE(brent, nullptr);
    EXPECT_EQ(brent->tick_size, 100);       // $0.01/bbl
    EXPECT_EQ(brent->match_algo, IceMatchAlgo::FIFO);
    EXPECT_EQ(brent->ipl_hold_ms, 5000);
    EXPECT_EQ(brent->product_group, "Energy");
}

TEST(IceProductsTest, EuriborConfig) {
    auto products = get_ice_products();
    const IceProductConfig* euribor = nullptr;
    for (const auto& p : products) {
        if (p.symbol == "I") { euribor = &p; break; }
    }
    ASSERT_NE(euribor, nullptr);
    EXPECT_EQ(euribor->tick_size, 50);      // 0.005 half-tick
    EXPECT_EQ(euribor->match_algo, IceMatchAlgo::GTBPR);
    EXPECT_EQ(euribor->ipl_hold_ms, 5000);
    EXPECT_EQ(euribor->ipl_recalc_ms, 5000);
    EXPECT_EQ(euribor->product_group, "STIR");
}

TEST(IceProductsTest, CocoaConfig) {
    auto products = get_ice_products();
    const IceProductConfig* cocoa = nullptr;
    for (const auto& p : products) {
        if (p.symbol == "C") { cocoa = &p; break; }
    }
    ASSERT_NE(cocoa, nullptr);
    EXPECT_EQ(cocoa->tick_size, 10000);     // £1/tonne
    EXPECT_EQ(cocoa->match_algo, IceMatchAlgo::FIFO);
    EXPECT_EQ(cocoa->ipl_hold_ms, 15000);   // softs use 15-sec hold
    EXPECT_EQ(cocoa->product_group, "Softs");
}

TEST(IceProductsTest, Ftse100Config) {
    auto products = get_ice_products();
    const IceProductConfig* ftse = nullptr;
    for (const auto& p : products) {
        if (p.symbol == "Z") { ftse = &p; break; }
    }
    ASSERT_NE(ftse, nullptr);
    EXPECT_EQ(ftse->tick_size, 5000);       // 0.50 index pts
    EXPECT_EQ(ftse->match_algo, IceMatchAlgo::FIFO);
    EXPECT_EQ(ftse->ipl_recalc_ms, 5000);   // index uses 5-sec recalc
    EXPECT_EQ(ftse->product_group, "Equity Index");
}

// Softs products should have longer hold periods (15s vs 5s for energy).
TEST(IceProductsTest, SoftsHaveLongerHoldPeriod) {
    for (const auto& p : get_ice_products()) {
        if (p.product_group == "Softs") {
            EXPECT_EQ(p.ipl_hold_ms, 15000)
                << p.symbol << " softs should have 15-sec IPL hold";
        }
    }
}

// Index products should have faster recalc (5s vs 15s for non-index).
TEST(IceProductsTest, IndexProductsFasterRecalc) {
    for (const auto& p : get_ice_products()) {
        if (p.product_group == "Equity Index") {
            EXPECT_EQ(p.ipl_recalc_ms, 5000)
                << p.symbol << " index products should have 5-sec recalc";
        }
    }
}

}  // namespace
}  // namespace ice
}  // namespace exchange
