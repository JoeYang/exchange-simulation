#include "tools/tui_renderer.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <iomanip>
#include <sstream>
#include <string>

using namespace ftxui;

namespace exchange {

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

namespace {

// Format a fixed-point price (PRICE_SCALE = 10000) as "NNNN.DDDD".
std::string fmt_price(Price p) {
    if (p == 0) return "      -   ";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4)
       << (static_cast<double>(p) / 10000.0);
    return ss.str();
}

// Format a fixed-point quantity (lot scale = 10000) as "N.DDDD".
std::string fmt_qty(Quantity q) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4)
       << (static_cast<double>(q) / 10000.0);
    return ss.str();
}

// Format a compact integer quantity (just the raw value, no decimal).
std::string fmt_raw(int64_t v) {
    return std::to_string(v);
}

// Trim a string to at most max_len characters, appending ".." if truncated.
std::string trim_str(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len - 2) + "..";
}

// Number of depth levels to display per side.
static constexpr size_t kMaxDepthRows = 8;

// Number of trade and event rows to display.
static constexpr size_t kMaxTradeRows  = 8;
static constexpr size_t kMaxEventRows  = 10;

}  // namespace

// ---------------------------------------------------------------------------
// render_orderbook
// ---------------------------------------------------------------------------

ftxui::Element TuiRenderer::render_orderbook(const OrderbookState& state) {
    // Header row
    Elements rows;
    rows.push_back(
        hbox({
            text("  BIDS") | bold | color(Color::Green) | size(WIDTH, EQUAL, 32),
            separator(),
            text("  ASKS") | bold | color(Color::Red)   | size(WIDTH, EQUAL, 32),
        })
    );
    rows.push_back(separatorLight());

    // Column sub-header
    rows.push_back(
        hbox({
            hbox({
                text("  Price       ") | size(WIDTH, EQUAL, 14),
                text("    Qty    ") | size(WIDTH, EQUAL, 11),
                text(" Cnt") | size(WIDTH, EQUAL, 7),
            }) | size(WIDTH, EQUAL, 32),
            separator(),
            hbox({
                text("  Price       ") | size(WIDTH, EQUAL, 14),
                text("    Qty    ") | size(WIDTH, EQUAL, 11),
                text(" Cnt") | size(WIDTH, EQUAL, 7),
            }) | size(WIDTH, EQUAL, 32),
        })
    );
    rows.push_back(separatorLight());

    // Collect up to kMaxDepthRows from each side.
    std::vector<std::pair<Price, PriceLevelView>> bid_levels;
    std::vector<std::pair<Price, PriceLevelView>> ask_levels;

    for (const auto& [p, lv] : state.bids()) {
        bid_levels.emplace_back(p, lv);
        if (bid_levels.size() >= kMaxDepthRows) break;
    }
    for (const auto& [p, lv] : state.asks()) {
        ask_levels.emplace_back(p, lv);
        if (ask_levels.size() >= kMaxDepthRows) break;
    }

    size_t n_rows = std::max(bid_levels.size(), ask_levels.size());
    if (n_rows == 0) n_rows = 1;  // ensure at least one empty row

    for (size_t i = 0; i < n_rows; ++i) {
        // Bid side cell
        Element bid_cell;
        if (i < bid_levels.size()) {
            const auto& [p, lv] = bid_levels[i];
            std::string price_s = fmt_price(p);
            std::string qty_s   = fmt_qty(lv.total_qty);
            std::string cnt_s   = "(" + fmt_raw(lv.order_count) + ")";
            bid_cell = hbox({
                text("  " + price_s) | color(Color::Green) | size(WIDTH, EQUAL, 14),
                text("  " + qty_s)   | size(WIDTH, EQUAL, 11),
                text(" " + cnt_s)    | dim | size(WIDTH, EQUAL, 7),
            });
        } else {
            bid_cell = text("") | size(WIDTH, EQUAL, 32);
        }

        // Ask side cell
        Element ask_cell;
        if (i < ask_levels.size()) {
            const auto& [p, lv] = ask_levels[i];
            std::string price_s = fmt_price(p);
            std::string qty_s   = fmt_qty(lv.total_qty);
            std::string cnt_s   = "(" + fmt_raw(lv.order_count) + ")";
            ask_cell = hbox({
                text("  " + price_s) | color(Color::Red) | size(WIDTH, EQUAL, 14),
                text("  " + qty_s)   | size(WIDTH, EQUAL, 11),
                text(" " + cnt_s)    | dim | size(WIDTH, EQUAL, 7),
            });
        } else {
            ask_cell = text("") | size(WIDTH, EQUAL, 32);
        }

        rows.push_back(hbox({
            bid_cell | size(WIDTH, EQUAL, 32),
            separator(),
            ask_cell | size(WIDTH, EQUAL, 32),
        }));
    }

    return window(text(" Orderbook "), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// render_trades
// ---------------------------------------------------------------------------

ftxui::Element TuiRenderer::render_trades(const OrderbookState& state) {
    Elements rows;
    rows.push_back(
        hbox({
            text("  Price       ") | bold | size(WIDTH, EQUAL, 14),
            text("    Qty     ") | bold | size(WIDTH, EQUAL, 12),
            text(" IDs          ") | bold | dim,
        })
    );
    rows.push_back(separatorLight());

    const auto& trades = state.recent_trades();
    // Show the most-recent kMaxTradeRows, in reverse order (newest first).
    size_t start = (trades.size() > kMaxTradeRows)
                       ? (trades.size() - kMaxTradeRows)
                       : 0;

    if (trades.empty()) {
        rows.push_back(text("  (no trades yet)") | dim);
    }

    for (size_t i = trades.size(); i-- > start; ) {
        const TradeView& t = trades[i];
        std::string price_s = fmt_price(t.price);
        std::string qty_s   = fmt_qty(t.quantity);
        std::string ids_s   = "[" + std::to_string(t.aggressor_id) +
                              "x" + std::to_string(t.resting_id) + "]";
        rows.push_back(hbox({
            text("  " + price_s) | color(Color::Yellow) | size(WIDTH, EQUAL, 14),
            text("  " + qty_s)   | size(WIDTH, EQUAL, 12),
            text(" " + ids_s)    | dim,
        }));
    }

    return window(text(" Recent Trades "), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// render_events
// ---------------------------------------------------------------------------

ftxui::Element TuiRenderer::render_events(const OrderbookState& state) {
    Elements rows;

    const auto& log = state.event_log();
    // Show the most-recent kMaxEventRows, newest first.
    size_t start = (log.size() > kMaxEventRows)
                       ? (log.size() - kMaxEventRows)
                       : 0;

    if (log.empty()) {
        rows.push_back(text("  (no events yet)") | dim);
    }

    for (size_t i = log.size(); i-- > start; ) {
        const EventLogEntry& entry = log[i];
        std::string ts_s  = "ts=" + std::to_string(entry.ts);
        std::string desc  = trim_str(entry.description, 60);
        rows.push_back(hbox({
            text("  " + desc) | flex,
            text("  " + ts_s) | dim,
        }));
    }

    return window(text(" Order Events "), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// render_status
// ---------------------------------------------------------------------------

ftxui::Element TuiRenderer::render_status(size_t action_index,
                                          size_t total_actions) {
    std::string pos;
    if (total_actions > 0) {
        pos = "Action " + std::to_string(action_index + 1) +
              "/" + std::to_string(total_actions);
    } else {
        pos = "No journal loaded";
    }

    return hbox({
        text(" " + pos + " ") | bold,
        separatorEmpty(),
        text("[n]ext ") | color(Color::Cyan),
        text("[p]rev ") | color(Color::Cyan),
        text("[g]oto ") | color(Color::Cyan),
        text("[q]uit ") | color(Color::Cyan),
    }) | borderLight;
}

// ---------------------------------------------------------------------------
// render (composite)
// ---------------------------------------------------------------------------

ftxui::Element TuiRenderer::render(const OrderbookState& state,
                                   size_t action_index,
                                   size_t total_actions) {
    Element book_panel   = render_orderbook(state);
    Element trades_panel = render_trades(state);
    Element events_panel = render_events(state);
    Element status_bar   = render_status(action_index, total_actions);

    return vbox({
        hbox({
            book_panel   | flex,
            trades_panel | size(WIDTH, EQUAL, 38),
        }),
        events_panel | size(HEIGHT, EQUAL, 14),
        status_bar,
    });
}

}  // namespace exchange
