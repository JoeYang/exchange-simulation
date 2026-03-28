#pragma once

#include <cstdint>
#include <string>

namespace exchange {

// InstrumentInfo -- exchange-agnostic instrument metadata.
//
// Produced by SecdefConsumer implementations (CmeSecdef, IceSecdef).
// The observer works exclusively with this type -- it never sees
// CmeProductConfig or IceProductConfig directly.
//
// Price/quantity fields use engine fixed-point (PRICE_SCALE = 10000).
struct InstrumentInfo {
    uint32_t    security_id{0};       // CME security_id or ICE instrument_id
    std::string symbol;               // "ES", "B", etc.
    std::string description;          // "E-mini S&P 500", "Brent Crude Futures"
    std::string product_group;        // "Equity Index", "Energy"
    int64_t     tick_size{0};         // min price increment (engine fixed-point)
    int64_t     lot_size{0};          // min qty increment (engine fixed-point)
    int64_t     max_order_size{0};    // max qty per order (engine fixed-point)
    char        match_algorithm{'F'}; // 'F' (FIFO), 'P' (ProRata), etc.
    std::string currency;             // "USD", "GBP", etc.
    double      display_factor{0.0};  // price -> display conversion
};

}  // namespace exchange
