#pragma once

#include "exchange-core/types.h"

#include <string>
#include <vector>

namespace exchange {
namespace ice {

// ICE matching algorithm selection for product configuration.
// Used to determine which MatchAlgoT template parameter to use when
// instantiating IceExchange for a given product.
enum class IceMatchAlgo : uint8_t { FIFO, GTBPR };

// IceProductConfig describes a single ICE Futures product.
//
// Price and quantity fields use the fixed-point conventions from types.h:
//   Price    = int64_t, 4 decimal places  (e.g. $0.01 → 100)
//   Quantity = int64_t, 4 decimal places  (1 contract → 10000)
//
// ICE-specific fields beyond EngineConfig:
//   - match_algo: FIFO for most products, GTBPR for STIRs
//   - ipl_width: Interval Price Limit band half-width (fixed-point)
//   - ipl_hold_ms: hold period when IPL triggers (milliseconds)
//   - ipl_recalc_ms: IPL recalculation interval (milliseconds)
//   - settlement_window_secs: VWAP settlement window duration (seconds)
//   - smp_action: default SMP action for the product
struct IceProductConfig {
    uint32_t    instrument_id;
    std::string symbol;
    std::string description;
    std::string product_group;
    Price       tick_size;             // minimum price increment (fixed-point)
    Quantity    lot_size;              // minimum quantity increment (10000 = 1 contract)
    Quantity    max_order_size;        // max contracts per order (fixed-point)
    IceMatchAlgo match_algo;           // FIFO or GTBPR
    Price       ipl_width;             // IPL band half-width (fixed-point)
    int         ipl_hold_ms;           // IPL hold period (ms)
    int         ipl_recalc_ms;         // IPL recalculation interval (ms)
    int         settlement_window_secs;// VWAP settlement window (seconds)
    SmpAction   smp_action;            // default SMP action
};

// get_ice_products -- canonical ICE Futures product table.
//
// Tick size derivation (4 decimal places, multiply by PRICE_SCALE=10000):
//   Brent:          $0.01/bbl  → 100
//   Gasoil:         $0.25/tonne → 2500
//   Natural Gas:    0.01 p/therm → 100
//   Cocoa:          £1/tonne   → 10000
//   Robusta Coffee: $1/tonne   → 10000
//   White Sugar:    $0.10/tonne → 1000
//   Euribor:        0.005 (half-tick) → 50
//   SONIA:          0.01       → 100
//   FTSE 100:       0.50 index pts → 5000
//   MSCI World:     0.10 index pts → 1000
//
// IPL widths are product-specific, set by ICE Market Supervision.
// Values here are representative simulator defaults.
inline std::vector<IceProductConfig> get_ice_products() {
    return {
        // -----------------------------------------------------------------
        // Energy (ICE Futures Europe) — FIFO matching
        // -----------------------------------------------------------------

        // Brent Crude Futures (IFEU)
        // Tick: $0.01/bbl → 100 fixed-point
        // IPL: ~$1.00 band → 10000, 5-sec hold, 15-sec recalc
        {
            1, "B", "Brent Crude Futures", "Energy",
            /*tick_size=*/          100,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 5000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          10000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // Gasoil Futures (IFEU)
        // Tick: $0.25/tonne → 2500 fixed-point
        {
            2, "G", "Gasoil Futures", "Energy",
            /*tick_size=*/          2500,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 2000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          50000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // UK Natural Gas (NBP) (IFEU)
        // Tick: 0.01 pence/therm → 100 fixed-point
        {
            3, "M", "UK Natural Gas (NBP)", "Energy",
            /*tick_size=*/          100,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 5000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          5000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // -----------------------------------------------------------------
        // Agricultural / Softs (ICE Futures Europe) — FIFO matching
        // -----------------------------------------------------------------

        // London Cocoa (IFEU)
        // Tick: £1/tonne → 10000 fixed-point
        // IPL: wider hold (15-sec) for softs
        {
            4, "C", "London Cocoa", "Softs",
            /*tick_size=*/          10000,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 1000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          100000,
            /*ipl_hold_ms=*/        15000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // Robusta Coffee (IFEU)
        // Tick: $1/tonne → 10000 fixed-point
        {
            5, "RC", "Robusta Coffee", "Softs",
            /*tick_size=*/          10000,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 1000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          100000,
            /*ipl_hold_ms=*/        15000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // White Sugar No. 5 (IFEU)
        // Tick: $0.10/tonne → 1000 fixed-point
        {
            6, "W", "White Sugar No. 5", "Softs",
            /*tick_size=*/          1000,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 2000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          20000,
            /*ipl_hold_ms=*/        15000,
            /*ipl_recalc_ms=*/      15000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // -----------------------------------------------------------------
        // STIR (Short-Term Interest Rates) — GTBPR matching
        // -----------------------------------------------------------------

        // Three-Month Euribor Futures (IFEU)
        // Tick: 0.005 (half-tick) → 50 fixed-point
        // IPL: tighter bands for rates products; 5-sec hold, 5-sec recalc
        {
            7, "I", "Three-Month Euribor", "STIR",
            /*tick_size=*/          50,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 10000,
            /*match_algo=*/         IceMatchAlgo::GTBPR,
            /*ipl_width=*/          500,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      5000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // Three-Month SONIA Futures (IFEU)
        // Tick: 0.01 → 100 fixed-point
        {
            8, "SO", "Three-Month SONIA", "STIR",
            /*tick_size=*/          100,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 10000,
            /*match_algo=*/         IceMatchAlgo::GTBPR,
            /*ipl_width=*/          1000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      5000,
            /*settlement_window=*/  120,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // -----------------------------------------------------------------
        // Equity Index (ICE Futures Europe) — FIFO matching
        // -----------------------------------------------------------------

        // FTSE 100 Index Future (IFEU)
        // Tick: 0.50 index points → 5000 fixed-point
        // IPL: 5-sec recalc for index products
        {
            9, "Z", "FTSE 100 Index", "Equity Index",
            /*tick_size=*/          5000,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 2000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          500000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      5000,
            /*settlement_window=*/  300,
            /*smp_action=*/         SmpAction::CancelNewest,
        },

        // MSCI World Index Future (IFEU)
        // Tick: 0.10 index points → 1000 fixed-point
        {
            10, "MW", "MSCI World Index", "Equity Index",
            /*tick_size=*/          1000,
            /*lot_size=*/           10000,
            /*max_order_size=*/     10000 * 2000,
            /*match_algo=*/         IceMatchAlgo::FIFO,
            /*ipl_width=*/          100000,
            /*ipl_hold_ms=*/        5000,
            /*ipl_recalc_ms=*/      5000,
            /*settlement_window=*/  300,
            /*smp_action=*/         SmpAction::CancelNewest,
        },
    };
}

}  // namespace ice
}  // namespace exchange
