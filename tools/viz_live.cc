#include "tools/shm_transport.h"
#include "tools/orderbook_state.h"
#include "test-harness/recorded_event.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <thread>

// exchange-viz-live <shm-name>
//
// Connects to a POSIX shared memory ring buffer created by ShmProducer,
// polls for RecordedEvents, feeds them into OrderbookState, and renders
// the orderbook depth, recent trades, and event log to the terminal using
// ANSI escape codes.
//
// The SHM name must include the leading '/' (e.g. /exchange-events).
//
// Designed as a placeholder for the full FTXUI viewer (Task 37): once
// tools/tui_renderer.h is available the rendering block below can be
// replaced with TuiRenderer::render(state) calls without changing the
// polling or signal-handling logic.

namespace {
volatile std::sig_atomic_t g_running = 1;

void signal_handler(int /*sig*/) {
    g_running = 0;
}

// Render the current orderbook state to stdout using ANSI escape codes.
// Clears the screen before each redraw so the output appears in-place.
void render(const exchange::OrderbookState& state) {
    // Move cursor to home and clear screen.
    std::fputs("\033[2J\033[H", stdout);

    // ------------------------------------------------------------------ header
    std::puts("=== BIDS =======================  === ASKS =======================");
    std::puts("       Price       Qty  Orders         Price       Qty  Orders");
    std::puts("---------------------------------------------------------------");

    const auto& bids = state.bids();
    const auto& asks = state.asks();
    auto bit = bids.cbegin();
    auto ait = asks.cbegin();

    for (int i = 0; i < 10; ++i) {
        if (bit != bids.cend()) {
            std::printf("  %12.4f %8.4f (%4u)  ",
                        static_cast<double>(bit->second.price) / 10000.0,
                        static_cast<double>(bit->second.total_qty) / 10000.0,
                        bit->second.order_count);
            ++bit;
        } else {
            std::printf("  %30s  ", "");
        }

        if (ait != asks.cend()) {
            std::printf("  %12.4f %8.4f (%4u)",
                        static_cast<double>(ait->second.price) / 10000.0,
                        static_cast<double>(ait->second.total_qty) / 10000.0,
                        ait->second.order_count);
            ++ait;
        }

        std::putchar('\n');
    }

    // ----------------------------------------------------------- recent trades
    std::puts("\n=== RECENT TRADES (last 10) ===");
    std::puts("       Price       Qty   Aggressor      Resting");
    std::puts("-----------------------------------------------");

    const auto& trades = state.recent_trades();
    // Show at most last 10 trades, most recent first.
    const size_t trade_start =
        trades.size() > 10 ? trades.size() - 10 : 0;
    for (size_t i = trade_start; i < trades.size(); ++i) {
        const auto& t = trades[i];
        std::printf("  %12.4f %8.4f   %12lu  %12lu\n",
                    static_cast<double>(t.price) / 10000.0,
                    static_cast<double>(t.quantity) / 10000.0,
                    static_cast<unsigned long>(t.aggressor_id),
                    static_cast<unsigned long>(t.resting_id));
    }
    if (trades.empty()) {
        std::puts("  (none)");
    }

    // -------------------------------------------------------------- event log
    std::puts("\n=== RECENT EVENTS (last 5) ===");
    const auto& log = state.event_log();
    const size_t log_start = log.size() > 5 ? log.size() - 5 : 0;
    for (size_t i = log_start; i < log.size(); ++i) {
        std::printf("  %s\n", log[i].description.c_str());
    }
    if (log.empty()) {
        std::puts("  (none)");
    }

    std::puts("\nPress Ctrl+C to exit.");
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <shm-name>\n", argv[0]);
        std::fprintf(stderr, "  Example: %s /exchange-events\n", argv[0]);
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        exchange::ShmConsumer consumer(argv[1]);
        exchange::OrderbookState state;
        exchange::RecordedEvent event;

        std::fprintf(stdout, "Connected to %s. Waiting for events...\n",
                     argv[1]);
        std::fflush(stdout);

        while (g_running) {
            bool got_event = false;

            // Drain all available events before redrawing.
            while (consumer.poll(event)) {
                state.apply(event);
                got_event = true;
            }

            if (got_event) {
                render(state);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    std::fprintf(stdout, "\nDisconnected from %s.\n", argv[1]);
    return 0;
}
