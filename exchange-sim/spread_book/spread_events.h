#pragma once

#include "exchange-core/types.h"

#include <cstdint>
#include <vector>

namespace exchange {

// --- Spread fill attribution ---

// SpreadFill records a single spread trade with per-leg fill details.
// Published when an implied-out or implied-in match executes.
struct SpreadFill {
    uint32_t  spread_instrument_id{0};
    OrderId   spread_order_id{0};       // resting spread order
    Side      spread_side{Side::Buy};
    Price     spread_price{0};          // execution price of the spread
    Quantity  spread_qty{0};            // spread-lots filled
    Timestamp ts{0};

    // Per-leg fill details (parallel with strategy legs).
    struct LegDetail {
        uint32_t instrument_id{0};
        OrderId  outright_order_id{0};  // resting outright order
        Side     side{Side::Buy};
        Price    price{0};
        Quantity qty{0};
    };
    std::vector<LegDetail> leg_details;

    // Source of the match.
    enum class Source : uint8_t {
        Direct,      // spread-vs-spread match
        ImpliedOut,  // outright BBO change triggered spread match
        ImpliedIn,   // spread order triggered outright fills
    };
    Source source{Source::Direct};
};

// --- Implied top-of-book for spread instrument ---

// Published when the implied spread BBO changes.
struct ImpliedTopOfBook {
    uint32_t  spread_instrument_id{0};
    Price     implied_bid{0};      // best synthetic spread bid (0 = no bid)
    Quantity  implied_bid_qty{0};
    Price     implied_ask{0};      // best synthetic spread ask (0 = no ask)
    Quantity  implied_ask_qty{0};
    Timestamp ts{0};
};

// --- Implied depth update for spread instrument ---

// Published when implied depth changes at a specific price level.
struct ImpliedDepthUpdate {
    uint32_t  spread_instrument_id{0};
    Side      side{Side::Buy};
    Price     price{0};
    Quantity  total_qty{0};       // 0 = level removed
    Timestamp ts{0};
    enum Action : uint8_t { Add, Update, Remove } action{Update};
};

// --- Listener interface for spread events ---

class SpreadEventListenerBase {
public:
    void on_spread_fill(const SpreadFill&) {}
    void on_implied_top_of_book(const ImpliedTopOfBook&) {}
    void on_implied_depth_update(const ImpliedDepthUpdate&) {}
};

}  // namespace exchange
