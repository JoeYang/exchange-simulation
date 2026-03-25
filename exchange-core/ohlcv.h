#pragma once
#include "exchange-core/types.h"

namespace exchange {

struct OhlcvStats {
    Price open{0};
    Price high{0};
    Price low{0};
    Price close{0};
    Quantity volume{0};
    Quantity turnover{0};   // sum of (price * qty) for VWAP, stored as int64_t
    uint32_t trade_count{0};
    bool has_traded{false};

    void on_trade(Price price, Quantity qty) {
        if (!has_traded) {
            open = price;
            high = price;
            low = price;
            has_traded = true;
        }
        if (price > high) high = price;
        if (price < low) low = price;
        close = price;
        volume += qty;
        turnover += price * qty / PRICE_SCALE;  // normalize to avoid overflow
        ++trade_count;
    }

    void reset() {
        *this = OhlcvStats{};
    }

    // VWAP = total turnover / total volume
    Price vwap() const {
        if (volume == 0) return 0;
        return turnover * PRICE_SCALE / volume;
    }
};

}  // namespace exchange
