#include "krx/krx_products.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace exchange {
namespace krx {
namespace {

// ===========================================================================
// Product table structure
// ===========================================================================

TEST(KrxProductsTest, TenProductsPresent) {
    auto products = get_krx_products();
    EXPECT_EQ(products.size(), 10u);
}

TEST(KrxProductsTest, UniqueInstrumentIds) {
    auto products = get_krx_products();
    std::set<uint32_t> ids;
    for (const auto& p : products) {
        EXPECT_TRUE(ids.insert(p.instrument_id).second)
            << "Duplicate instrument_id: " << p.instrument_id
            << " (" << p.symbol << ")";
    }
}

TEST(KrxProductsTest, UniqueSymbols) {
    auto products = get_krx_products();
    std::set<std::string> symbols;
    for (const auto& p : products) {
        EXPECT_TRUE(symbols.insert(p.symbol).second)
            << "Duplicate symbol: " << p.symbol;
    }
}

// ===========================================================================
// Field validation -- all products
// ===========================================================================

TEST(KrxProductsTest, TickSizesPositive) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.tick_size, 0) << p.symbol << " tick_size must be > 0";
    }
}

TEST(KrxProductsTest, LotSizesAreOneContract) {
    for (const auto& p : get_krx_products()) {
        EXPECT_EQ(p.lot_size, 10000)
            << p.symbol << " lot_size should be 10000 (1 contract)";
    }
}

TEST(KrxProductsTest, MaxOrderSizePositive) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.max_order_size, 0) << p.symbol;
    }
}

TEST(KrxProductsTest, DescriptionsNonEmpty) {
    for (const auto& p : get_krx_products()) {
        EXPECT_FALSE(p.symbol.empty()) << "symbol must not be empty";
        EXPECT_FALSE(p.description.empty()) << "description must not be empty";
    }
}

// ===========================================================================
// Tiered daily price limits -- all products use 8/15/20
// ===========================================================================

TEST(KrxProductsTest, AllProductsHaveThreeTieredLimits) {
    for (const auto& p : get_krx_products()) {
        EXPECT_EQ(p.tiered_limit_pcts[0], 8)
            << p.symbol << " tier 1 must be 8%";
        EXPECT_EQ(p.tiered_limit_pcts[1], 15)
            << p.symbol << " tier 2 must be 15%";
        EXPECT_EQ(p.tiered_limit_pcts[2], 20)
            << p.symbol << " tier 3 must be 20%";
    }
}

TEST(KrxProductsTest, TieredLimitsAreStrictlyIncreasing) {
    for (const auto& p : get_krx_products()) {
        for (int i = 1; i < KrxProductConfig::kNumTiers; ++i) {
            EXPECT_GT(p.tiered_limit_pcts[i], p.tiered_limit_pcts[i - 1])
                << p.symbol << " tier " << i << " must be > tier " << (i - 1);
        }
    }
}

// ===========================================================================
// VI thresholds
// ===========================================================================

TEST(KrxProductsTest, ViDynamicPctPositive) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.vi_dynamic_pct, 0) << p.symbol;
    }
}

TEST(KrxProductsTest, ViStaticPctPositive) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.vi_static_pct, 0) << p.symbol;
    }
}

TEST(KrxProductsTest, ViStaticWiderThanDynamic) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.vi_static_pct, p.vi_dynamic_pct)
            << p.symbol << " static VI must be wider than dynamic VI";
    }
}

// ===========================================================================
// Dynamic band
// ===========================================================================

TEST(KrxProductsTest, DynamicBandPctPositive) {
    for (const auto& p : get_krx_products()) {
        EXPECT_GT(p.dynamic_band_pct, 0) << p.symbol;
    }
}

// ===========================================================================
// Sidecar -- only KOSPI200 futures (KS)
// ===========================================================================

TEST(KrxProductsTest, OnlyKospi200FuturesHasSidecar) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "KS") {
            EXPECT_EQ(p.sidecar_threshold_pct, 5)
                << "KS must have 5% sidecar threshold";
        } else {
            EXPECT_EQ(p.sidecar_threshold_pct, 0)
                << p.symbol << " must not have sidecar (threshold = 0)";
        }
    }
}

// ===========================================================================
// Product group classification
// ===========================================================================

TEST(KrxProductsTest, FuturesClassification) {
    std::set<std::string> futures_symbols = {"KS", "MKS", "KSQ"};
    for (const auto& p : get_krx_products()) {
        if (futures_symbols.count(p.symbol)) {
            EXPECT_EQ(p.product_group, KrxProductGroup::Futures) << p.symbol;
        }
    }
}

TEST(KrxProductsTest, OptionsClassification) {
    std::set<std::string> options_symbols = {"KSO", "MKSO", "KSOW", "KSQO"};
    for (const auto& p : get_krx_products()) {
        if (options_symbols.count(p.symbol)) {
            EXPECT_EQ(p.product_group, KrxProductGroup::Options) << p.symbol;
        }
    }
}

TEST(KrxProductsTest, FxClassification) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "USD") {
            EXPECT_EQ(p.product_group, KrxProductGroup::FX);
        }
    }
}

TEST(KrxProductsTest, BondClassification) {
    std::set<std::string> bond_symbols = {"KTB", "LKTB"};
    for (const auto& p : get_krx_products()) {
        if (bond_symbols.count(p.symbol)) {
            EXPECT_EQ(p.product_group, KrxProductGroup::Bond) << p.symbol;
        }
    }
}

// ===========================================================================
// Specific product configs
// ===========================================================================

TEST(KrxProductsTest, Kospi200FuturesConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "KS") {
            EXPECT_EQ(p.tick_size, 500);         // 0.05 * 10000
            EXPECT_EQ(p.lot_size, 10000);
            EXPECT_EQ(p.sidecar_threshold_pct, 5);
            EXPECT_EQ(p.vi_dynamic_pct, 3);
            EXPECT_EQ(p.vi_static_pct, 10);
            EXPECT_EQ(p.product_group, KrxProductGroup::Futures);
            return;
        }
    }
    FAIL() << "KS product not found";
}

TEST(KrxProductsTest, MiniKospi200FuturesConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "MKS") {
            EXPECT_EQ(p.tick_size, 200);         // 0.02 * 10000
            EXPECT_EQ(p.sidecar_threshold_pct, 0);
            EXPECT_EQ(p.product_group, KrxProductGroup::Futures);
            return;
        }
    }
    FAIL() << "MKS product not found";
}

TEST(KrxProductsTest, Kosdaq150FuturesConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "KSQ") {
            EXPECT_EQ(p.tick_size, 1000);        // 0.10 * 10000
            EXPECT_EQ(p.product_group, KrxProductGroup::Futures);
            return;
        }
    }
    FAIL() << "KSQ product not found";
}

TEST(KrxProductsTest, OptionsTickSize) {
    std::set<std::string> options_symbols = {"KSO", "MKSO", "KSOW", "KSQO"};
    int found = 0;
    for (const auto& p : get_krx_products()) {
        if (options_symbols.count(p.symbol)) {
            EXPECT_EQ(p.tick_size, 100)          // 0.01 * 10000
                << p.symbol << " options tick must be 0.01 (100 fixed-point)";
            ++found;
        }
    }
    EXPECT_EQ(found, 4) << "Expected 4 option products";
}

TEST(KrxProductsTest, UsdKrwConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "USD") {
            EXPECT_EQ(p.tick_size, 1000);        // 0.10 * 10000
            EXPECT_EQ(p.dynamic_band_pct, 3);    // FX uses tighter band
            EXPECT_EQ(p.product_group, KrxProductGroup::FX);
            return;
        }
    }
    FAIL() << "USD product not found";
}

TEST(KrxProductsTest, Ktb3YearConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "KTB") {
            EXPECT_EQ(p.tick_size, 100);         // 0.01 * 10000
            EXPECT_EQ(p.dynamic_band_pct, 3);    // bonds use tighter band
            EXPECT_EQ(p.product_group, KrxProductGroup::Bond);
            return;
        }
    }
    FAIL() << "KTB product not found";
}

TEST(KrxProductsTest, Lktb10YearConfig) {
    for (const auto& p : get_krx_products()) {
        if (p.symbol == "LKTB") {
            EXPECT_EQ(p.tick_size, 100);         // 0.01 * 10000
            EXPECT_EQ(p.dynamic_band_pct, 3);
            EXPECT_EQ(p.product_group, KrxProductGroup::Bond);
            return;
        }
    }
    FAIL() << "LKTB product not found";
}

// FX and bond products should have tighter dynamic bands than equity products.
TEST(KrxProductsTest, FxAndBondHaveTighterDynamicBand) {
    for (const auto& p : get_krx_products()) {
        if (p.product_group == KrxProductGroup::FX ||
            p.product_group == KrxProductGroup::Bond) {
            EXPECT_LE(p.dynamic_band_pct, 3)
                << p.symbol << " FX/bond dynamic band should be <= 3%";
        }
    }
}

}  // namespace
}  // namespace krx
}  // namespace exchange
