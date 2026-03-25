// exchange_dashboard.cc -- FTXUI multi-instrument exchange dashboard
//
// Connects to a POSIX shared memory ring buffer, polls for RecordedEvents,
// maintains per-instrument state, and renders an interactive TUI showing:
//   - Instrument summary table (symbol, session state, OHLCV, VWAP)
//   - Orderbook depth for the selected instrument
//   - Recent trades tape
//   - Throughput statistics (messages/sec, peak)
//
// Usage: exchange-dashboard <shm-name>
//   e.g.  exchange-dashboard /exchange-events

#include "tools/orderbook_state.h"
#include "tools/shm_transport.h"
#include "exchange-core/ohlcv.h"
#include "exchange-core/types.h"
#include "test-harness/recorded_event.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace ftxui;

namespace {

volatile std::sig_atomic_t g_running = 1;

void signal_handler(int /*sig*/) {
    g_running = 0;
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string fmt_price(exchange::Price p) {
    if (p == 0) return "     -    ";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4)
       << (static_cast<double>(p) / 10000.0);
    return ss.str();
}

std::string fmt_qty(exchange::Quantity q) {
    if (q == 0) return "    -   ";
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4)
       << (static_cast<double>(q) / 10000.0);
    return ss.str();
}

std::string fmt_int(int64_t v) {
    return std::to_string(v);
}

const char* session_state_str(exchange::SessionState s) {
    switch (s) {
        case exchange::SessionState::Closed:            return "CLOSED";
        case exchange::SessionState::PreOpen:           return "PRE_OPEN";
        case exchange::SessionState::OpeningAuction:    return "OPENING";
        case exchange::SessionState::Continuous:        return "CONTINUOUS";
        case exchange::SessionState::PreClose:          return "PRE_CLOSE";
        case exchange::SessionState::ClosingAuction:    return "CLOSING";
        case exchange::SessionState::Halt:              return "HALT";
        case exchange::SessionState::VolatilityAuction: return "VOL_AUCTION";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Per-instrument state maintained by the dashboard
// ---------------------------------------------------------------------------

struct InstrumentView {
    std::string symbol;
    exchange::SessionState state{exchange::SessionState::Closed};
    exchange::OhlcvStats ohlcv;
    exchange::OrderbookState book;
};

// ---------------------------------------------------------------------------
// Dashboard state
// ---------------------------------------------------------------------------

struct DashboardState {
    // Instruments indexed by a stable sorted order for display.
    // In a real system these would be keyed by InstrumentId and populated
    // from configuration. For this dashboard we use a single default instrument
    // and update it from the SHM event stream.
    std::vector<InstrumentView> instruments;
    int selected_instrument{0};

    // Throughput tracking
    uint64_t total_messages{0};
    uint64_t messages_this_sec{0};
    uint64_t messages_per_sec{0};
    uint64_t peak_msg_per_sec{0};
    std::chrono::steady_clock::time_point last_sec_tick;

    DashboardState() : last_sec_tick(std::chrono::steady_clock::now()) {
        // Start with one default instrument.
        InstrumentView v;
        v.symbol = "DEFAULT";
        instruments.push_back(std::move(v));
    }

    void tick_throughput() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_sec_tick);
        if (elapsed.count() >= 1) {
            messages_per_sec = messages_this_sec;
            if (messages_per_sec > peak_msg_per_sec) {
                peak_msg_per_sec = messages_per_sec;
            }
            messages_this_sec = 0;
            last_sec_tick = now;
        }
    }

    void on_event(const exchange::RecordedEvent& event) {
        ++total_messages;
        ++messages_this_sec;

        if (instruments.empty()) return;
        // Route all events to the currently selected instrument for now.
        // In a multi-instrument setup, the event would carry an instrument ID.
        auto& inst = instruments[0];
        inst.book.apply(event);

        // Update OHLCV from trade events.
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, exchange::Trade>) {
                inst.ohlcv.on_trade(e.price, e.quantity);
            } else if constexpr (std::is_same_v<T, exchange::MarketStatus>) {
                inst.state = e.state;
            }
        }, event);
    }
};

// ---------------------------------------------------------------------------
// Display constants
// ---------------------------------------------------------------------------

static constexpr size_t kMaxDepthRows  = 8;
static constexpr size_t kMaxTradeRows  = 8;

// ---------------------------------------------------------------------------
// Render: Instrument summary table
// ---------------------------------------------------------------------------

Element render_instrument_table(const DashboardState& state) {
    Elements rows;

    // Header
    rows.push_back(
        hbox({
            text("  Symbol   ") | bold | size(WIDTH, EQUAL, 12),
            text(" State      ") | bold | size(WIDTH, EQUAL, 13),
            text("     Open    ") | bold | size(WIDTH, EQUAL, 12),
            text("     High    ") | bold | size(WIDTH, EQUAL, 12),
            text("      Low    ") | bold | size(WIDTH, EQUAL, 12),
            text("    Close    ") | bold | size(WIDTH, EQUAL, 12),
            text("    Vol    ") | bold | size(WIDTH, EQUAL, 10),
            text("  Trades") | bold | size(WIDTH, EQUAL, 8),
            text("     VWAP    ") | bold | size(WIDTH, EQUAL, 12),
        })
    );
    rows.push_back(separatorLight());

    for (size_t i = 0; i < state.instruments.size(); ++i) {
        const auto& inst = state.instruments[i];
        bool is_selected = (static_cast<int>(i) == state.selected_instrument);

        auto state_color = Color::White;
        if (inst.state == exchange::SessionState::Continuous)
            state_color = Color::Green;
        else if (inst.state == exchange::SessionState::Halt)
            state_color = Color::Red;
        else if (inst.state == exchange::SessionState::PreOpen ||
                 inst.state == exchange::SessionState::OpeningAuction)
            state_color = Color::Yellow;

        Element row = hbox({
            text("  " + inst.symbol) | size(WIDTH, EQUAL, 12),
            text(" " + std::string(session_state_str(inst.state)))
                | color(state_color) | size(WIDTH, EQUAL, 13),
            text(fmt_price(inst.ohlcv.open))  | size(WIDTH, EQUAL, 12),
            text(fmt_price(inst.ohlcv.high))  | color(Color::Green) | size(WIDTH, EQUAL, 12),
            text(fmt_price(inst.ohlcv.low))   | color(Color::Red)   | size(WIDTH, EQUAL, 12),
            text(fmt_price(inst.ohlcv.close)) | size(WIDTH, EQUAL, 12),
            text(fmt_qty(inst.ohlcv.volume))  | size(WIDTH, EQUAL, 10),
            text(fmt_int(inst.ohlcv.trade_count)) | size(WIDTH, EQUAL, 8),
            text(fmt_price(inst.ohlcv.vwap())) | size(WIDTH, EQUAL, 12),
        });

        if (is_selected) {
            row = row | inverted;
        }
        rows.push_back(std::move(row));
    }

    return window(text(" Instruments "), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// Render: Orderbook depth for selected instrument
// ---------------------------------------------------------------------------

Element render_orderbook(const DashboardState& state) {
    if (state.instruments.empty()) {
        return window(text(" Orderbook "), text("  (no instruments)") | dim);
    }
    const auto& inst = state.instruments[state.selected_instrument];
    const auto& book = inst.book;

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
                text("    Qty    ")    | size(WIDTH, EQUAL, 11),
                text(" Cnt")           | size(WIDTH, EQUAL, 7),
            }) | size(WIDTH, EQUAL, 32),
            separator(),
            hbox({
                text("  Price       ") | size(WIDTH, EQUAL, 14),
                text("    Qty    ")    | size(WIDTH, EQUAL, 11),
                text(" Cnt")           | size(WIDTH, EQUAL, 7),
            }) | size(WIDTH, EQUAL, 32),
        })
    );
    rows.push_back(separatorLight());

    std::vector<std::pair<exchange::Price, exchange::PriceLevelView>> bid_levels;
    std::vector<std::pair<exchange::Price, exchange::PriceLevelView>> ask_levels;

    for (const auto& [p, lv] : book.bids()) {
        bid_levels.emplace_back(p, lv);
        if (bid_levels.size() >= kMaxDepthRows) break;
    }
    for (const auto& [p, lv] : book.asks()) {
        ask_levels.emplace_back(p, lv);
        if (ask_levels.size() >= kMaxDepthRows) break;
    }

    size_t n_rows = std::max(bid_levels.size(), ask_levels.size());
    if (n_rows == 0) n_rows = 1;

    for (size_t i = 0; i < n_rows; ++i) {
        Element bid_cell;
        if (i < bid_levels.size()) {
            const auto& [p, lv] = bid_levels[i];
            bid_cell = hbox({
                text("  " + fmt_price(p)) | color(Color::Green) | size(WIDTH, EQUAL, 14),
                text("  " + fmt_qty(lv.total_qty)) | size(WIDTH, EQUAL, 11),
                text(" (" + fmt_int(lv.order_count) + ")") | dim | size(WIDTH, EQUAL, 7),
            });
        } else {
            bid_cell = text("") | size(WIDTH, EQUAL, 32);
        }

        Element ask_cell;
        if (i < ask_levels.size()) {
            const auto& [p, lv] = ask_levels[i];
            ask_cell = hbox({
                text("  " + fmt_price(p)) | color(Color::Red) | size(WIDTH, EQUAL, 14),
                text("  " + fmt_qty(lv.total_qty)) | size(WIDTH, EQUAL, 11),
                text(" (" + fmt_int(lv.order_count) + ")") | dim | size(WIDTH, EQUAL, 7),
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

    std::string title = " Orderbook: " + inst.symbol + " ";
    return window(text(title), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// Render: Recent trades for selected instrument
// ---------------------------------------------------------------------------

Element render_trades(const DashboardState& state) {
    if (state.instruments.empty()) {
        return window(text(" Trades "), text("  (no instruments)") | dim);
    }
    const auto& inst = state.instruments[state.selected_instrument];
    const auto& trades = inst.book.recent_trades();

    Elements rows;
    rows.push_back(
        hbox({
            text("  Price       ") | bold | size(WIDTH, EQUAL, 14),
            text("    Qty     ")   | bold | size(WIDTH, EQUAL, 12),
            text(" IDs          ") | bold | dim,
        })
    );
    rows.push_back(separatorLight());

    if (trades.empty()) {
        rows.push_back(text("  (no trades yet)") | dim);
    }

    size_t start = (trades.size() > kMaxTradeRows)
                       ? (trades.size() - kMaxTradeRows)
                       : 0;

    for (size_t i = trades.size(); i-- > start; ) {
        const auto& t = trades[i];
        std::string ids = "[" + std::to_string(t.aggressor_id) +
                          "x" + std::to_string(t.resting_id) + "]";
        rows.push_back(hbox({
            text("  " + fmt_price(t.price)) | color(Color::Yellow) | size(WIDTH, EQUAL, 14),
            text("  " + fmt_qty(t.quantity)) | size(WIDTH, EQUAL, 12),
            text(" " + ids) | dim,
        }));
    }

    return window(text(" Recent Trades "), vbox(std::move(rows)));
}

// ---------------------------------------------------------------------------
// Render: Throughput statistics panel
// ---------------------------------------------------------------------------

Element render_throughput(const DashboardState& state) {
    auto msg_s = std::to_string(state.messages_per_sec);
    auto peak  = std::to_string(state.peak_msg_per_sec);
    auto total = std::to_string(state.total_messages);

    return window(text(" Throughput "),
        vbox({
            hbox({
                text("  Msgs/sec: ") | bold,
                text(msg_s) | color(Color::Cyan),
            }),
            hbox({
                text("  Peak:     ") | bold,
                text(peak) | color(Color::Yellow),
            }),
            hbox({
                text("  Total:    ") | bold,
                text(total),
            }),
        })
    );
}

// ---------------------------------------------------------------------------
// Render: Status bar
// ---------------------------------------------------------------------------

Element render_status_bar(const DashboardState& state) {
    const auto& inst = state.instruments.empty()
        ? "N/A" : state.instruments[state.selected_instrument].symbol;
    std::string sel = "Instrument: " + std::string(inst);

    return hbox({
        text(" " + sel + " ") | bold,
        separatorEmpty(),
        text("[Up/Down] select ") | color(Color::Cyan),
        text("[q] quit ") | color(Color::Cyan),
    }) | borderLight;
}

// ---------------------------------------------------------------------------
// Composite render
// ---------------------------------------------------------------------------

Element render_dashboard(const DashboardState& state) {
    Element instrument_panel = render_instrument_table(state);
    Element book_panel       = render_orderbook(state);
    Element trades_panel     = render_trades(state);
    Element throughput_panel = render_throughput(state);
    Element status_bar       = render_status_bar(state);

    return vbox({
        instrument_panel,
        hbox({
            book_panel | flex,
            vbox({
                trades_panel | flex,
                throughput_panel,
            }) | size(WIDTH, EQUAL, 40),
        }) | flex,
        status_bar,
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <shm-name>\n"
            "  Example: %s /exchange-events\n",
            argv[0], argv[0]);
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        exchange::ShmConsumer consumer(argv[1]);
        DashboardState state;
        // Use the SHM name (sans /) as the instrument symbol.
        state.instruments[0].symbol = (argv[1][0] == '/')
            ? std::string(argv[1] + 1)
            : std::string(argv[1]);

        auto screen = ScreenInteractive::Fullscreen();

        auto component = Renderer([&] {
            return render_dashboard(state);
        });

        // Handle keyboard navigation.
        component = CatchEvent(component, [&](Event event) -> bool {
            if (event == Event::Character('q') || event == Event::Escape) {
                screen.Exit();
                return true;
            }
            int n = static_cast<int>(state.instruments.size());
            if (event == Event::ArrowUp) {
                state.selected_instrument =
                    (state.selected_instrument - 1 + n) % n;
                return true;
            }
            if (event == Event::ArrowDown) {
                state.selected_instrument =
                    (state.selected_instrument + 1) % n;
                return true;
            }
            return false;
        });

        // Run the FTXUI event loop with periodic SHM polling.
        auto loop = Loop(&screen, component);

        while (!loop.HasQuitted() && g_running) {
            // Drain available events from shared memory.
            exchange::RecordedEvent event;
            bool got_event = false;
            while (consumer.poll(event)) {
                state.on_event(event);
                got_event = true;
            }
            state.tick_throughput();

            // Run one iteration of the FTXUI event loop (renders if needed).
            loop.RunOnce();

            if (!got_event) {
                // No SHM events — brief sleep to avoid busy-spinning.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Post a custom event to trigger a redraw after new data.
            screen.PostEvent(Event::Custom);
        }

    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    return 0;
}
