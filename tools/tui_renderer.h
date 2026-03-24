#pragma once

#include "tools/orderbook_state.h"
#include <ftxui/dom/elements.hpp>
#include <string>

namespace exchange {

// TuiRenderer builds FTXUI Element trees from an OrderbookState snapshot.
// All methods are stateless static functions — callers own the state.
class TuiRenderer {
public:
    // Compose the full TUI layout: orderbook + trades side by side,
    // event log below, and a status bar at the bottom.
    static ftxui::Element render(const OrderbookState& state,
                                 size_t action_index = 0,
                                 size_t total_actions = 0);

    // Two-column depth panel (bids | asks).
    static ftxui::Element render_orderbook(const OrderbookState& state);

    // Recent trade tape panel.
    static ftxui::Element render_trades(const OrderbookState& state);

    // Order / market-data event log panel.
    static ftxui::Element render_events(const OrderbookState& state);

    // Status bar: action counter + key-binding hints.
    static ftxui::Element render_status(size_t action_index,
                                        size_t total_actions);
};

}  // namespace exchange
