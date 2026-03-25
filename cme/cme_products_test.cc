#include "cme/cme_exchange.h"
#include "cme/cme_products.h"
#include "test-harness/recording_listener.h"

#include <algorithm>
#include <gtest/gtest.h>

namespace exchange {
namespace cme {
namespace {

// ---------------------------------------------------------------------------
// Convenience alias: same small pool as the engine test
// ---------------------------------------------------------------------------

using TestCmeExchange = CmeExchange<
    RecordingOrderListener,
    RecordingMdListener,
    FifoMatch,
    /*MaxOrders=*/     200,
    /*MaxPriceLevels=*/100,
    /*MaxOrderIds=*/   2000>;

// ===========================================================================
// CME product configurations
// ===========================================================================

// All eight standard CME products must be present in the product table.
TEST(CmeProductsTest, EightProductsPresent) {
    auto products = get_cme_products();
    EXPECT_EQ(products.size(), 8u);
}

// Every product must have a positive tick size, lot size, max order size,
// and band percentage.
TEST(CmeProductsTest, AllProductsHavePositiveSizes) {
    for (const auto& p : get_cme_products()) {
        EXPECT_GT(p.tick_size, 0)
            << p.symbol << " must have positive tick_size";
        EXPECT_GT(p.lot_size, 0)
            << p.symbol << " must have positive lot_size";
        EXPECT_GT(p.max_order_size, 0)
            << p.symbol << " must have positive max_order_size";
        EXPECT_GT(p.band_pct, 0)
            << p.symbol << " must have positive band_pct";
    }
}

// Verify the ES product (most-traded CME equity index future).
TEST(CmeProductsTest, EsConfig) {
    for (const auto& p : get_cme_products()) {
        if (p.symbol == "ES") {
            EXPECT_EQ(p.tick_size, 2500)     // 0.25 * 10000
                << "ES tick must be 0.25 (2500 fixed-point)";
            EXPECT_EQ(p.lot_size, 10000);
            EXPECT_EQ(p.band_pct, 5);
            EXPECT_EQ(p.product_group, "Equity Index");
            return;
        }
    }
    FAIL() << "ES product not found";
}

// Verify the NQ product.
TEST(CmeProductsTest, NqConfig) {
    for (const auto& p : get_cme_products()) {
        if (p.symbol == "NQ") {
            EXPECT_EQ(p.tick_size, 2500);
            EXPECT_EQ(p.band_pct, 5);
            return;
        }
    }
    FAIL() << "NQ product not found";
}

// Verify the CL (Crude Oil) product — energy products use a wider band.
TEST(CmeProductsTest, ClConfig) {
    for (const auto& p : get_cme_products()) {
        if (p.symbol == "CL") {
            EXPECT_EQ(p.tick_size, 100)   // 0.01 * 10000
                << "CL tick must be 0.01 (100 fixed-point)";
            EXPECT_EQ(p.band_pct, 7)
                << "CL must have 7% energy band";
            EXPECT_EQ(p.product_group, "Energy");
            return;
        }
    }
    FAIL() << "CL product not found";
}

// Verify the GC (Gold) product.
TEST(CmeProductsTest, GcConfig) {
    for (const auto& p : get_cme_products()) {
        if (p.symbol == "GC") {
            EXPECT_EQ(p.tick_size, 1000)  // 0.10 * 10000
                << "GC tick must be 0.10 (1000 fixed-point)";
            EXPECT_EQ(p.product_group, "Metals");
            return;
        }
    }
    FAIL() << "GC product not found";
}

// Rates products (ZN, ZB) must have ±3% bands (tighter than equities).
TEST(CmeProductsTest, RatesProductsBandPct) {
    bool found_zn = false, found_zb = false;
    for (const auto& p : get_cme_products()) {
        if (p.symbol == "ZN") {
            EXPECT_EQ(p.band_pct, 3);
            EXPECT_EQ(p.product_group, "Interest Rate");
            found_zn = true;
        }
        if (p.symbol == "ZB") {
            EXPECT_EQ(p.band_pct, 3);
            EXPECT_EQ(p.product_group, "Interest Rate");
            found_zb = true;
        }
    }
    EXPECT_TRUE(found_zn) << "ZN product not found";
    EXPECT_TRUE(found_zb) << "ZB product not found";
}

// Verify product symbols are non-empty and instrument IDs are unique.
TEST(CmeProductsTest, UniqueInstrumentIdsAndNonEmptyFields) {
    auto products = get_cme_products();
    std::vector<uint32_t> ids;
    for (const auto& p : products) {
        EXPECT_FALSE(p.symbol.empty())
            << "symbol must not be empty";
        EXPECT_FALSE(p.description.empty())
            << "description must not be empty";
        EXPECT_FALSE(p.product_group.empty())
            << "product_group must not be empty";
        ids.push_back(p.instrument_id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end())
        << "instrument_id values must be unique";
}

// lot_size must be 10000 (1 contract) for every product.
TEST(CmeProductsTest, AllProductsHaveStandardLotSize) {
    for (const auto& p : get_cme_products()) {
        EXPECT_EQ(p.lot_size, 10000)
            << p.symbol << " lot_size must be 10000 (1 contract)";
    }
}

// ===========================================================================
// CRTP policy accessors
// ===========================================================================

// The default CRTP policies must match CME Globex specification.
TEST(CmeExchangePoliciesTest, DefaultPolicies) {
    RecordingOrderListener ol;
    RecordingMdListener    ml;
    EngineConfig cfg{.tick_size = 2500, .lot_size = 10000,
                     .price_band_low = 0, .price_band_high = 0};
    TestCmeExchange engine{cfg, ol, ml};

    EXPECT_EQ(engine.get_smp_action(),    SmpAction::CancelNewest);
    EXPECT_EQ(engine.get_modify_policy(), ModifyPolicy::CancelReplace);
    EXPECT_EQ(engine.band_percentage(),   5);
}

// set_band_percentage must update band_percentage() immediately.
TEST(CmeExchangePoliciesTest, SetBandPercentage) {
    RecordingOrderListener ol;
    RecordingMdListener    ml;
    EngineConfig cfg{.tick_size = 2500, .lot_size = 10000,
                     .price_band_low = 0, .price_band_high = 0};
    TestCmeExchange engine{cfg, ol, ml};

    engine.set_band_percentage(3);
    EXPECT_EQ(engine.band_percentage(), 3);

    engine.set_band_percentage(0);
    EXPECT_EQ(engine.band_percentage(), 0);
}

}  // namespace
}  // namespace cme
}  // namespace exchange
