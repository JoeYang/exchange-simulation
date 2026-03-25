#pragma once
#include "exchange-core/types.h"

#include <string>
#include <vector>

namespace exchange {
namespace cme {

// CmeProductConfig describes a single CME Globex product.
//
// Price and quantity fields use the fixed-point conventions from types.h:
//   Price    = int64_t, 4 decimal places  (e.g. 0.25 → 2500)
//   Quantity = int64_t, 4 decimal places  (1 contract → 10000)
struct CmeProductConfig {
    uint32_t    instrument_id;
    std::string symbol;
    std::string description;
    std::string product_group;
    Price       tick_size;        // minimum price increment in fixed-point
    Quantity    lot_size;         // minimum quantity increment (always 10000 = 1 contract)
    Quantity    max_order_size;   // maximum contracts per order in fixed-point
    int64_t     band_pct;         // price band percentage (e.g. 5 = ±5%)
};

// get_cme_products -- canonical CME Globex product table.
//
// Tick size derivation (4 decimal places → multiply by 10000):
//   ES/NQ/MES: $0.25 tick  → 0.25 * 10000 = 2500
//   CL:        $0.01 tick  → 0.01 * 10000 = 100
//   GC:        $0.10 tick  → 0.10 * 10000 = 1000
//   ZN:        1/64 point  ≈ $0.015625 → 156 (truncated from 156.25)
//   ZB:        1/32 point  ≈ $0.03125  → 313 (truncated from 312.5)
//   6E:        $0.00005 tick → 0.00005 * 10000 = 0.5; use 5 (half-pip = $0.0005)
//
// max_order_size is expressed as contracts * lot_size (10000):
//   2000 contracts = 2000 * 10000 = 20000000
inline std::vector<CmeProductConfig> get_cme_products() {
    return {
        // E-mini S&P 500 Futures
        // Tick: $12.50 per tick = $0.25 index point → 2500 fixed-point
        // Band: ±5% (CME standard equity index band)
        {
            1, "ES", "E-mini S&P 500", "Equity Index",
            /*tick_size=*/  2500,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 2000,
            /*band_pct=*/   5
        },

        // E-mini Nasdaq-100 Futures
        // Tick: $5.00 per tick = $0.25 index point → 2500 fixed-point
        // Band: ±5%
        {
            2, "NQ", "E-mini Nasdaq-100", "Equity Index",
            /*tick_size=*/  2500,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 2000,
            /*band_pct=*/   5
        },

        // Crude Oil (WTI) Futures
        // Tick: $10.00 per tick = $0.01/bbl → 100 fixed-point
        // Band: ±7% (energy products have wider bands)
        {
            3, "CL", "Crude Oil WTI", "Energy",
            /*tick_size=*/  100,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 1000,
            /*band_pct=*/   7
        },

        // Gold Futures (100 troy oz)
        // Tick: $10.00 per tick = $0.10/oz → 1000 fixed-point
        // Band: ±5%
        {
            4, "GC", "Gold 100 oz", "Metals",
            /*tick_size=*/  1000,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 500,
            /*band_pct=*/   5
        },

        // 10-Year U.S. Treasury Note Futures
        // Tick: 1/2 of 1/32 of a point = 1/64 ≈ 0.015625 → 156 fixed-point
        // Band: ±3% (rates products have tighter bands)
        {
            5, "ZN", "10-Year T-Note", "Interest Rate",
            /*tick_size=*/  156,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 10000,
            /*band_pct=*/   3
        },

        // 30-Year U.S. Treasury Bond Futures
        // Tick: 1/32 of a point = 0.03125 → 313 fixed-point (rounded from 312.5)
        // Band: ±3%
        {
            6, "ZB", "30-Year T-Bond", "Interest Rate",
            /*tick_size=*/  313,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 10000,
            /*band_pct=*/   3
        },

        // Micro E-mini S&P 500 Futures (1/10th of ES)
        // Tick: $1.25 per tick = $0.25 index point → 2500 fixed-point (same tick)
        // Band: ±5%
        {
            7, "MES", "Micro E-mini S&P 500", "Equity Index",
            /*tick_size=*/  2500,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 5000,
            /*band_pct=*/   5
        },

        // Euro FX Futures (125,000 EUR notional)
        // Tick: $0.0001/EUR per half-pip tick; CME uses 0.00005 increments
        // 0.0005 (half-pip) * 10000 = 5 fixed-point
        // Band: ±3% (FX products have narrow bands)
        {
            8, "6E", "Euro FX", "FX",
            /*tick_size=*/  5,
            /*lot_size=*/   10000,
            /*max_order=*/  10000 * 2000,
            /*band_pct=*/   3
        },
    };
}

}  // namespace cme
}  // namespace exchange
