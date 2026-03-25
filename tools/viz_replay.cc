// exchange-viz-replay: offline journal visualizer
//
// Usage:
//   exchange-viz-replay <journal-file>      # display final state
//   exchange-viz-replay <journal-file> -i   # interactive keyboard step

#include "tools/tui_renderer.h"
#include "tools/orderbook_state.h"
#include "test-harness/journal_parser.h"
#include "test-harness/recording_listener.h"
#include "test-harness/test_runner.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ftxui;

namespace exchange {

// ---------------------------------------------------------------------------
// Local action-dispatch helpers (mirrors test_runner.cc private methods).
// ---------------------------------------------------------------------------

namespace {

Side parse_side(const std::string& s) {
    if (s == "BUY")  return Side::Buy;
    if (s == "SELL") return Side::Sell;
    throw std::runtime_error("viz_replay: unknown side '" + s + "'");
}

OrderType parse_order_type(const std::string& s) {
    if (s == "LIMIT")      return OrderType::Limit;
    if (s == "MARKET")     return OrderType::Market;
    if (s == "STOP")       return OrderType::Stop;
    if (s == "STOP_LIMIT") return OrderType::StopLimit;
    throw std::runtime_error("viz_replay: unknown order type '" + s + "'");
}

TimeInForce parse_tif(const std::string& s) {
    if (s == "DAY") return TimeInForce::DAY;
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    throw std::runtime_error("viz_replay: unknown TIF '" + s + "'");
}

SessionState parse_session_state(const std::string& s) {
    if (s == "CLOSED")             return SessionState::Closed;
    if (s == "PRE_OPEN")           return SessionState::PreOpen;
    if (s == "OPENING_AUCTION")    return SessionState::OpeningAuction;
    if (s == "CONTINUOUS")         return SessionState::Continuous;
    if (s == "PRE_CLOSE")          return SessionState::PreClose;
    if (s == "CLOSING_AUCTION")    return SessionState::ClosingAuction;
    if (s == "HALT")               return SessionState::Halt;
    if (s == "VOLATILITY_AUCTION") return SessionState::VolatilityAuction;
    throw std::runtime_error("viz_replay: unknown session state '" + s + "'");
}

template <typename EngineT>
void dispatch_action(EngineT& engine, const ParsedAction& action) {
    switch (action.type) {

    case ParsedAction::NewOrder: {
        OrderRequest req{};
        req.timestamp       = static_cast<Timestamp>(action.get_int("ts"));
        req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));

        {
            auto it = action.fields.find("account_id");
            req.account_id = (it != action.fields.end())
                                 ? static_cast<uint64_t>(std::stoll(it->second))
                                 : 0;
        }

        req.side     = parse_side(action.get_str("side"));
        req.type     = parse_order_type(action.get_str("type"));
        req.quantity = static_cast<Quantity>(action.get_int("qty"));

        {
            auto it = action.fields.find("price");
            req.price = (it != action.fields.end())
                            ? static_cast<Price>(std::stoll(it->second))
                            : 0;
        }

        {
            auto it = action.fields.find("tif");
            req.tif = (it != action.fields.end())
                          ? parse_tif(it->second)
                          : TimeInForce::GTC;
        }

        {
            auto it = action.fields.find("stop_price");
            req.stop_price = (it != action.fields.end())
                                 ? static_cast<Price>(std::stoll(it->second))
                                 : 0;
        }

        {
            auto it = action.fields.find("gtd_expiry");
            req.gtd_expiry = (it != action.fields.end())
                                 ? static_cast<Timestamp>(std::stoll(it->second))
                                 : 0;
        }

        engine.new_order(req);
        break;
    }

    case ParsedAction::Cancel: {
        OrderId   id = static_cast<OrderId>(action.get_int("ord_id"));
        Timestamp ts = static_cast<Timestamp>(action.get_int("ts"));
        engine.cancel_order(id, ts);
        break;
    }

    case ParsedAction::Modify: {
        ModifyRequest req{};
        req.order_id        = static_cast<OrderId>(action.get_int("ord_id"));
        req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
        req.new_price       = static_cast<Price>(action.get_int("new_price"));
        req.new_quantity    = static_cast<Quantity>(action.get_int("new_qty"));
        req.timestamp       = static_cast<Timestamp>(action.get_int("ts"));
        engine.modify_order(req);
        break;
    }

    case ParsedAction::TriggerExpiry: {
        Timestamp   ts  = static_cast<Timestamp>(action.get_int("ts"));
        TimeInForce tif = parse_tif(action.get_str("tif"));
        engine.trigger_expiry(ts, tif);
        break;
    }

    case ParsedAction::SetSessionState: {
        Timestamp    ts    = static_cast<Timestamp>(action.get_int("ts"));
        SessionState state = parse_session_state(action.get_str("state"));
        engine.set_session_state(state, ts);
        break;
    }

    case ParsedAction::ExecuteAuction: {
        Timestamp ts  = static_cast<Timestamp>(action.get_int("ts"));
        Price     ref = static_cast<Price>(action.get_int("reference_price"));
        engine.execute_auction(ref, ts);
        break;
    }

    case ParsedAction::PublishIndicative: {
        Timestamp ts  = static_cast<Timestamp>(action.get_int("ts"));
        Price     ref = static_cast<Price>(action.get_int("reference_price"));
        engine.publish_indicative_price(ref, ts);
        break;
    }

    case ParsedAction::MassCancel: {
        auto account_id = static_cast<uint64_t>(action.get_int("account_id"));
        Timestamp ts    = static_cast<Timestamp>(action.get_int("ts"));
        engine.mass_cancel(account_id, ts);
        break;
    }

    case ParsedAction::MassCancelAll: {
        Timestamp ts = static_cast<Timestamp>(action.get_int("ts"));
        engine.mass_cancel_all(ts);
        break;
    }

    // iLink3 E2E actions are not supported by the viz replay tool.
    case ParsedAction::ILink3NewOrder:
    case ParsedAction::ILink3Cancel:
    case ParsedAction::ILink3Replace:
    case ParsedAction::ILink3MassCancel:
        throw std::runtime_error(
            "iLink3 actions require the E2E test runner, "
            "not the viz replay tool");

    case ParsedAction::SessionStart:
    case ParsedAction::SessionOpen:
    case ParsedAction::SessionClose:
        throw std::runtime_error(
            "Session lifecycle actions require the E2E test runner, "
            "not the viz replay tool");

    }  // switch
}

}  // namespace

// ---------------------------------------------------------------------------
// ReplaySnapshot: pre-computed orderbook state after each action.
// ---------------------------------------------------------------------------

struct ReplaySnapshot {
    OrderbookState state;
};

// ---------------------------------------------------------------------------
// replay_journal: run the full journal, capturing one snapshot per action.
// ---------------------------------------------------------------------------

static std::vector<ReplaySnapshot> replay_journal(const Journal& journal) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    EngineConfig cfg{
        journal.config.tick_size,
        journal.config.lot_size,
        journal.config.price_band_low,
        journal.config.price_band_high
    };

    const bool use_fifo = (journal.config.match_algo != "PRO_RATA");

    std::vector<ReplaySnapshot> snapshots;
    snapshots.reserve(journal.entries.size());

    OrderbookState running_state;

    auto run_steps = [&](auto& engine) {
        for (const JournalEntry& entry : journal.entries) {
            order_listener.clear();
            md_listener.clear();

            dispatch_action(engine, entry.action);

            // Apply all recorded events to the running orderbook state.
            for (const RecordedEvent& ev : order_listener.events()) {
                running_state.apply(ev);
            }
            for (const RecordedEvent& ev : md_listener.events()) {
                running_state.apply(ev);
            }

            snapshots.push_back({running_state});
        }
    };

    if (use_fifo) {
        FifoExchange engine(cfg, order_listener, md_listener);
        run_steps(engine);
    } else {
        ProRataExchange engine(cfg, order_listener, md_listener);
        run_steps(engine);
    }

    return snapshots;
}

// ---------------------------------------------------------------------------
// non_interactive_display: render the final state to stdout.
// ---------------------------------------------------------------------------

static void non_interactive_display(const std::vector<ReplaySnapshot>& snaps) {
    if (snaps.empty()) {
        std::cout << "(no actions in journal)\n";
        return;
    }
    const ReplaySnapshot& last = snaps.back();
    Element layout = TuiRenderer::render(last.state,
                                         snaps.size() - 1,
                                         snaps.size());
    auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(layout));
    Render(screen, layout);
    std::cout << screen.ToString() << "\n";
}

// ---------------------------------------------------------------------------
// interactive_display: FTXUI keyboard-driven step viewer.
// ---------------------------------------------------------------------------

static void interactive_display(std::vector<ReplaySnapshot>& snaps) {
    if (snaps.empty()) {
        std::cout << "(no actions in journal)\n";
        return;
    }

    const size_t total = snaps.size();
    size_t current     = 0;

    auto screen_ui = ScreenInteractive::Fullscreen();

    auto renderer = Renderer([&] {
        return TuiRenderer::render(snaps[current].state, current, total);
    });

    auto with_events = CatchEvent(renderer, [&](Event event) -> bool {
        if (event == Event::Character('q') || event == Event::Escape) {
            screen_ui.Exit();
            return true;
        }
        if (event == Event::Character('n') || event == Event::ArrowRight) {
            if (current + 1 < total) ++current;
            return true;
        }
        if (event == Event::Character('p') || event == Event::ArrowLeft) {
            if (current > 0) --current;
            return true;
        }
        if (event == Event::Character('g')) {
            current = total - 1;
            return true;
        }
        return false;
    });

    screen_ui.Loop(with_events);
}

}  // namespace exchange

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: exchange-viz-replay <journal-file> [-i]\n";
        return EXIT_FAILURE;
    }

    const std::string journal_path = argv[1];
    bool interactive = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "-i") {
            interactive = true;
        }
    }

    exchange::Journal journal;
    try {
        journal = exchange::JournalParser::parse(journal_path);
    } catch (const std::exception& ex) {
        std::cerr << "exchange-viz-replay: failed to parse journal: "
                  << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    std::vector<exchange::ReplaySnapshot> snapshots;
    try {
        snapshots = exchange::replay_journal(journal);
    } catch (const std::exception& ex) {
        std::cerr << "exchange-viz-replay: replay failed: "
                  << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    if (interactive) {
        exchange::interactive_display(snapshots);
    } else {
        exchange::non_interactive_display(snapshots);
    }

    return EXIT_SUCCESS;
}
