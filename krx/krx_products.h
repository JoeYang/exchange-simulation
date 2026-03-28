#pragma once

#include "exchange-core/types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace exchange {
namespace krx {

// KRX product group classification.
enum class KrxProductGroup : uint8_t {
    Futures,
    Options,
    FX,
    Bond,
};

// KrxProductConfig describes a single KRX (Korea Exchange) derivatives product.
//
// Price and quantity fields use the fixed-point conventions from types.h:
//   Price    = int64_t, 4 decimal places  (e.g. 0.05 index pts -> 500)
//   Quantity = int64_t, 4 decimal places  (1 contract -> 10000)
//
// KRX-specific fields beyond EngineConfig:
//   - tiered_limit_pcts: daily price limit tiers (KRX uses 3 tiers: 8%, 15%, 20%)
//   - dynamic_band_pct: dynamic price band for order validation (% of last trade)
//   - vi_dynamic_pct: VI dynamic threshold (% deviation from last trade)
//   - vi_static_pct: VI static threshold (% deviation from reference price)
//   - sidecar_threshold_pct: program trading halt threshold (0 = no sidecar)
//   - product_group: classification for routing and session management
struct KrxProductConfig {
    uint32_t    instrument_id;
    std::string symbol;
    std::string description;
    KrxProductGroup product_group;
    Price       tick_size;             // minimum price increment (fixed-point)
    Quantity    lot_size;              // minimum quantity increment (10000 = 1 contract)
    Quantity    max_order_size;        // max contracts per order (fixed-point)

    // Tiered daily price limits: KRX widens bands on successive breaches.
    // tier 1 -> tier 2 -> tier 3 (e.g. 8% -> 15% -> 20%).
    // Values are percentages (e.g. 8 = +/-8%).
    static constexpr int kNumTiers = 3;
    std::array<int, kNumTiers> tiered_limit_pcts;

    // Dynamic price band: orders outside this % of last trade are rejected.
    int dynamic_band_pct;

    // Volatility Interruption (VI) thresholds.
    // Dynamic VI: triggered when price moves > vi_dynamic_pct from last trade.
    // Static VI: triggered when price moves > vi_static_pct from reference price.
    int vi_dynamic_pct;
    int vi_static_pct;

    // Sidecar (program trading halt) threshold percentage.
    // When KOSPI200 futures move > this % in 1 minute, program trading is halted
    // for 5 minutes. 0 = sidecar not applicable for this product.
    int sidecar_threshold_pct;
};

// get_krx_products -- canonical KRX derivatives product table.
//
// Tick size derivation (4 decimal places, multiply by PRICE_SCALE=10000):
//   KOSPI200 (KS):       0.05 index pts  -> 500
//   Mini-KOSPI200 (MKS): 0.02 index pts  -> 200
//   KOSDAQ150 (KSQ):     0.10 index pts  -> 1000
//   Options (KSO/MKSO/KSOW/KSQO): 0.01 pts -> 100
//   USD/KRW (USD):       0.10 KRW        -> 1000
//   KTB 3Y (KTB):        0.01 pts        -> 100
//   KTB 10Y (LKTB):      0.01 pts        -> 100
//
// max_order_size: contracts * lot_size (10000)
//   KOSPI200: 3000 contracts = 30000000
//   Mini:     5000 contracts = 50000000
//   Others:   varies by product
inline std::vector<KrxProductConfig> get_krx_products() {
    return {
        // -----------------------------------------------------------------
        // KOSPI200 Futures (flagship product)
        // Tick: 0.05 index points -> 500 fixed-point
        // Sidecar: 5% threshold (only product with sidecar)
        // -----------------------------------------------------------------
        {
            1, "KS", "KOSPI200 Futures", KrxProductGroup::Futures,
            /*tick_size=*/           500,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 3000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   5,
        },

        // Mini-KOSPI200 Futures (1/5th of KOSPI200)
        // Tick: 0.02 index points -> 200 fixed-point
        {
            2, "MKS", "Mini-KOSPI200 Futures", KrxProductGroup::Futures,
            /*tick_size=*/           200,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 5000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // KOSDAQ150 Futures
        // Tick: 0.10 index points -> 1000 fixed-point
        {
            3, "KSQ", "KOSDAQ150 Futures", KrxProductGroup::Futures,
            /*tick_size=*/           1000,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 3000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // -----------------------------------------------------------------
        // Options
        // Tick: 0.01 points -> 100 fixed-point for all option products
        // -----------------------------------------------------------------

        // KOSPI200 Options
        {
            4, "KSO", "KOSPI200 Options", KrxProductGroup::Options,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 5000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // Mini-KOSPI200 Options
        {
            5, "MKSO", "Mini-KOSPI200 Options", KrxProductGroup::Options,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 5000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // KOSPI200 Weekly Options
        {
            6, "KSOW", "KOSPI200 Weekly Options", KrxProductGroup::Options,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 5000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // KOSDAQ150 Options
        {
            7, "KSQO", "KOSDAQ150 Options", KrxProductGroup::Options,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 5000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    5,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // -----------------------------------------------------------------
        // FX Futures
        // -----------------------------------------------------------------

        // USD/KRW Futures
        // Tick: 0.10 KRW -> 1000 fixed-point
        {
            8, "USD", "USD/KRW Futures", KrxProductGroup::FX,
            /*tick_size=*/           1000,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 2000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    3,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // -----------------------------------------------------------------
        // Bond Futures
        // -----------------------------------------------------------------

        // KTB 3-Year Bond Futures
        // Tick: 0.01 points -> 100 fixed-point
        {
            9, "KTB", "KTB 3-Year Futures", KrxProductGroup::Bond,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 3000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    3,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },

        // LKTB 10-Year Bond Futures
        // Tick: 0.01 points -> 100 fixed-point
        {
            10, "LKTB", "LKTB 10-Year Futures", KrxProductGroup::Bond,
            /*tick_size=*/           100,
            /*lot_size=*/            10000,
            /*max_order_size=*/      10000 * 3000,
            /*tiered_limit_pcts=*/   {8, 15, 20},
            /*dynamic_band_pct=*/    3,
            /*vi_dynamic_pct=*/      3,
            /*vi_static_pct=*/       10,
            /*sidecar_threshold=*/   0,
        },
    };
}

}  // namespace krx
}  // namespace exchange
