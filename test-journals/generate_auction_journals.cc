// generate_auction_journals.cc
//
// Generates auction scenario .journal files by running each scenario through
// the real matching engine, capturing events via RecordingListeners, and
// serialising everything with JournalWriter.  This guarantees every EXPECT
// line is exactly what the engine produces — no hand-written expectations.
//
// Usage (via Bazel):
//   bazel run //test-journals:generate_auction_journals
//
// The generator writes files relative to the directory given as the first
// command-line argument, or $BUILD_WORKSPACE_DIRECTORY (set by `bazel run`),
// or the current working directory as a fallback.

#include "exchange-core/matching_engine.h"
#include "exchange-core/match_algo.h"
#include "test-harness/recording_listener.h"
#include "test-harness/journal_parser.h"
#include "test-harness/journal_writer.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// AuctionExchange
//
// Concrete engine used by all auction generator scenarios.  Uses the same
// pool sizes as FifoExchange in the test runner so generated journals can be
// replayed without hitting limits.
// ---------------------------------------------------------------------------
class AuctionExchange
    : public MatchingEngine<AuctionExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<AuctionExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Helper infrastructure — mirrors generate_stress_journals.cc
// ---------------------------------------------------------------------------

struct CaptureContext {
    RecordingOrderListener& order_listener;
    RecordingMdListener&    md_listener;
};

// Build a JournalEntry from a ParsedAction plus events fired into the two
// listeners.  Order events come first, then md events — same merge order used
// by run_impl in test_runner.cc.
JournalEntry build_entry(ParsedAction action, CaptureContext& ctx) {
    JournalEntry entry;
    entry.action = std::move(action);

    std::vector<RecordedEvent> all_events;
    all_events.reserve(ctx.order_listener.size() + ctx.md_listener.size());
    for (const RecordedEvent& ev : ctx.order_listener.events()) {
        all_events.push_back(ev);
    }
    for (const RecordedEvent& ev : ctx.md_listener.events()) {
        all_events.push_back(ev);
    }

    // Round-trip each RecordedEvent through writer + parser to produce
    // ParsedExpectation values (same strategy as generate_stress_journals.cc).
    for (const RecordedEvent& ev : all_events) {
        std::string line = JournalWriter::event_to_expect_line(ev);
        std::string mini =
            "CONFIG match_algo=FIFO tick_size=100 lot_size=10000\n"
            "ACTION NEW_ORDER ts=1 cl_ord_id=999 account_id=0 "
            "side=BUY price=1000000 qty=10000 type=LIMIT tif=GTC\n" +
            line + "\n";
        Journal j = JournalParser::parse_string(mini);
        entry.expectations.push_back(j.entries[0].expectations[0]);
    }

    ctx.order_listener.clear();
    ctx.md_listener.clear();
    return entry;
}

// ---------------------------------------------------------------------------
// Action factory helpers
// ---------------------------------------------------------------------------

ParsedAction make_new_order(Timestamp ts, uint64_t cl_ord_id,
                            uint64_t account_id, const char* side,
                            Price price, Quantity qty,
                            const char* type = "LIMIT",
                            const char* tif  = "GTC") {
    ParsedAction a;
    a.type = ParsedAction::NewOrder;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["cl_ord_id"]  = std::to_string(cl_ord_id);
    a.fields["account_id"] = std::to_string(account_id);
    a.fields["side"]       = side;
    a.fields["price"]      = std::to_string(price);
    a.fields["qty"]        = std::to_string(qty);
    a.fields["type"]       = type;
    a.fields["tif"]        = tif;
    return a;
}

ParsedAction make_set_session_state(Timestamp ts, const char* state) {
    ParsedAction a;
    a.type = ParsedAction::SetSessionState;
    a.fields["ts"]    = std::to_string(ts);
    a.fields["state"] = state;
    return a;
}

ParsedAction make_execute_auction(Timestamp ts, Price reference_price) {
    ParsedAction a;
    a.type = ParsedAction::ExecuteAuction;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["reference_price"] = std::to_string(reference_price);
    return a;
}

ParsedAction make_publish_indicative(Timestamp ts, Price reference_price) {
    ParsedAction a;
    a.type = ParsedAction::PublishIndicative;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["reference_price"] = std::to_string(reference_price);
    return a;
}

// ---------------------------------------------------------------------------
// Helper: run an action against the engine and append the resulting entry.
// ---------------------------------------------------------------------------

void run_new_order(AuctionExchange& engine, CaptureContext& ctx,
                   std::vector<JournalEntry>& entries,
                   Timestamp ts, uint64_t cl_ord_id, uint64_t account_id,
                   const char* side, Price price, Quantity qty,
                   const char* type = "LIMIT", const char* tif = "GTC") {
    OrderRequest req{};
    req.timestamp       = ts;
    req.client_order_id = cl_ord_id;
    req.account_id      = account_id;
    req.side            = (std::string(side) == "BUY") ? Side::Buy : Side::Sell;
    req.type            = (std::string(type) == "MARKET") ? OrderType::Market
                                                           : OrderType::Limit;
    req.tif             = TimeInForce::GTC;
    req.price           = price;
    req.quantity        = qty;

    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.new_order(req);
    entries.push_back(build_entry(
        make_new_order(ts, cl_ord_id, account_id, side, price, qty, type, tif),
        ctx));
}

void run_set_session_state(AuctionExchange& engine, CaptureContext& ctx,
                           std::vector<JournalEntry>& entries,
                           Timestamp ts, SessionState state,
                           const char* state_str) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.set_session_state(state, ts);
    entries.push_back(build_entry(make_set_session_state(ts, state_str), ctx));
}

void run_execute_auction(AuctionExchange& engine, CaptureContext& ctx,
                         std::vector<JournalEntry>& entries,
                         Timestamp ts, Price reference_price) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.execute_auction(reference_price, ts);
    entries.push_back(build_entry(make_execute_auction(ts, reference_price), ctx));
}

void run_publish_indicative(AuctionExchange& engine, CaptureContext& ctx,
                            std::vector<JournalEntry>& entries,
                            Timestamp ts, Price reference_price) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.publish_indicative_price(reference_price, ts);
    entries.push_back(build_entry(make_publish_indicative(ts, reference_price), ctx));
}

// Write journal file with header comment.
void write_journal(const std::string& path,
                   const std::string& header_comment,
                   const ParsedConfig& config,
                   const std::vector<JournalEntry>& entries) {
    std::string body = JournalWriter::to_string(config, entries);
    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open: " + path);
    }
    f << header_comment << body;
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// Standard config used by all auction scenarios.
ParsedConfig make_config() {
    ParsedConfig cfg;
    cfg.match_algo    = "FIFO";
    cfg.tick_size     = 100;
    cfg.lot_size      = 10000;
    cfg.max_orders    = 1000;
    cfg.max_levels    = 100;
    cfg.max_order_ids = 10000;
    return cfg;
}

// ---------------------------------------------------------------------------
// Scenario 1: auction_simple_cross
//
// 1 bid at 101.0000 + 1 ask at 99.0000, both GTC.  Orders rest without
// matching in PreOpen.  execute_auction at reference_price=100.0000 fills
// both orders completely at price 100.0000.
// ---------------------------------------------------------------------------
void generate_simple_cross(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Transition to PreOpen
    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Place crossing orders — should rest, not fill
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1010000, 10000);   // bid 101.0000
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL", 990000, 10000);    // ask  99.0000

    // Execute auction: both fill at auction price 100.0000
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_simple_cross.journal",
        "# auction_simple_cross.journal\n"
        "# 1 bid at 101.0000 + 1 ask at 99.0000 placed in PreOpen.\n"
        "# execute_auction fills both at equilibrium price 100.0000.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 2: auction_multiple_levels
//
// Multiple bids and asks at different levels.  The auction price is the one
// that maximises matched volume.
//
// Bids:  200@102.0000, 100@101.0000
// Asks:  100@98.0000,  200@99.0000
//
// At 99.0000: buy_vol = 300, sell_vol = 100 → matched = 100
// At 101.0000: buy_vol = 300, sell_vol = 300 → matched = 300  <-- max
// At 102.0000: buy_vol = 200, sell_vol = 300 → matched = 200
//
// Auction price = 101.0000 (max volume = 300).
// ---------------------------------------------------------------------------
void generate_multiple_levels(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Bids: 200 lots at 102.0000, 100 lots at 101.0000
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1020000, 200000);
    run_new_order(engine, ctx, entries,
                  3000, 2, 1, "BUY", 1010000, 100000);

    // Asks: 100 lots at 98.0000, 200 lots at 99.0000
    run_new_order(engine, ctx, entries,
                  4000, 3, 2, "SELL",  980000, 100000);
    run_new_order(engine, ctx, entries,
                  5000, 4, 2, "SELL",  990000, 200000);

    // Auction: max volume at 101.0000 (300 lots matched)
    run_execute_auction(engine, ctx, entries, 7000, 1000000);

    write_journal(out_dir + "/test-journals/auction_multiple_levels.journal",
        "# auction_multiple_levels.journal\n"
        "# Bids at 102.0000 (200 lots) and 101.0000 (100 lots).\n"
        "# Asks at 98.0000 (100 lots) and 99.0000 (200 lots).\n"
        "# Auction price = 101.0000 maximises matched volume (300 lots).\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 3: auction_partial_fill
//
// Unequal bid and ask sizes.  Smaller side fully fills; larger side has
// surplus that remains in the book.
//
// Bid:  300 lots at 101.0000
// Ask:  100 lots at 99.0000
//
// Auction at ref=100.0000: matched = 100 lots.
// Buy surplus: 200 lots remain in book at 101.0000.
// ---------------------------------------------------------------------------
void generate_partial_fill(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY",  1010000, 300000);  // 300 lots bid
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL",  990000, 100000);   // 100 lots ask

    // execute_auction: 100 lots match, 200 lots bid surplus remains
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_partial_fill.journal",
        "# auction_partial_fill.journal\n"
        "# Bid: 300 lots at 101.0000.  Ask: 100 lots at 99.0000.\n"
        "# Auction fills 100 lots.  200-lot buy surplus stays in book.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 4: auction_no_cross
//
// Best bid < best ask — no fill.  execute_auction is a no-op.
//
// Bid: 100 lots at 99.0000
// Ask: 100 lots at 101.0000
// ---------------------------------------------------------------------------
void generate_no_cross(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY",   990000, 10000);  // bid 99.0000
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL", 1010000, 10000);  // ask 101.0000

    // No crossing: execute_auction produces no fills, no events
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_no_cross.journal",
        "# auction_no_cross.journal\n"
        "# Bid at 99.0000, ask at 101.0000 — no crossing.\n"
        "# execute_auction is a no-op: no fills, no events.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 5: auction_empty_book
//
// No orders in book.  execute_auction is a no-op.
// ---------------------------------------------------------------------------
void generate_empty_book(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // No orders placed — empty book
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_empty_book.journal",
        "# auction_empty_book.journal\n"
        "# Empty book: execute_auction produces no events.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 6: auction_reference_tiebreak
//
// Two candidate prices each give the same maximum matched volume.
// The reference price breaks the tie: the candidate closer to the reference
// price wins.
//
// Setup:
//   Bid:  100 lots @ 102.0000,  100 lots @ 100.0000
//   Ask:  100 lots @ 100.0000,  100 lots @ 102.0000
//
// Candidate 100.0000: buy_vol = 200, sell_vol = 100 → matched = 100
// Candidate 102.0000: buy_vol = 100, sell_vol = 200 → matched = 100
//
// Both give matched = 100.  Imbalance both = 100.
// Reference = 100.0000 → dist(100.0000) = 0, dist(102.0000) = 200.
// Winner = 100.0000.
// ---------------------------------------------------------------------------
void generate_reference_tiebreak(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Bids
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1020000, 100000);  // 100 lots @ 102
    run_new_order(engine, ctx, entries,
                  3000, 2, 1, "BUY", 1000000, 100000);  // 100 lots @ 100

    // Asks
    run_new_order(engine, ctx, entries,
                  4000, 3, 2, "SELL", 1000000, 100000); // 100 lots @ 100
    run_new_order(engine, ctx, entries,
                  5000, 4, 2, "SELL", 1020000, 100000); // 100 lots @ 102

    // Reference = 100.0000: tiebreak selects 100.0000 as auction price
    run_execute_auction(engine, ctx, entries, 7000, 1000000);

    write_journal(out_dir + "/test-journals/auction_reference_tiebreak.journal",
        "# auction_reference_tiebreak.journal\n"
        "# Two prices (100.0000 and 102.0000) give the same matched volume.\n"
        "# Reference price 100.0000 breaks the tie: auction at 100.0000.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 7: auction_indicative_price
//
// Publish indicative price during PreOpen collection phase after adding
// orders.  Verifies the INDICATIVE_PRICE event contains the correct
// equilibrium price, matched volume, and surplus values.
//
// Orders:
//   Bid: 100 lots @ 101.0000
//   Ask: 100 lots @  99.0000
//
// Indicative at ref=100.0000: price=100.0000, matched=100, buy_surplus=0,
//                              sell_surplus=0
// ---------------------------------------------------------------------------
void generate_indicative_price(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY",  1010000, 100000);  // 100 lots bid
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL",  990000, 100000);   // 100 lots ask

    // Publish indicative before executing the auction
    run_publish_indicative(engine, ctx, entries, 4000, 1000000);

    // Execute auction: fills the full 100 lots
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_indicative_price.journal",
        "# auction_indicative_price.journal\n"
        "# Bid: 100 lots @ 101.0000.  Ask: 100 lots @ 99.0000.\n"
        "# Publish indicative at ref=100.0000 during PreOpen, then execute.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 8: auction_full_lifecycle
//
// Full session lifecycle:
//   Closed → PreOpen → collect orders → publish indicative →
//   execute auction → Continuous → normal matching
//
// Collection phase (PreOpen):
//   Bid: 100 lots @ 101.0000,  Ask: 100 lots @  99.0000
//
// After auction: both filled at 100.0000.
// Continuous phase: place a new bid + ask at same price → immediate fill.
// ---------------------------------------------------------------------------
void generate_full_lifecycle(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    AuctionExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Start in Closed — no need to set it, but set PreOpen to make the
    // transition explicit and emit the MARKET_STATUS event.
    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Collect orders in PreOpen — they rest without matching
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY",  1010000, 100000);  // 100 lots bid
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL",  990000, 100000);   // 100 lots ask

    // Publish indicative price
    run_publish_indicative(engine, ctx, entries, 4000, 1000000);

    // Execute auction: clears all crossing orders
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    // Transition to Continuous
    run_set_session_state(engine, ctx, entries, 6000,
                          SessionState::Continuous, "CONTINUOUS");

    // Normal matching in Continuous: immediate fill on crossing orders
    run_new_order(engine, ctx, entries,
                  7000, 3, 1, "BUY",  1010000, 50000);   // 50 lots bid
    run_new_order(engine, ctx, entries,
                  8000, 4, 2, "SELL", 1010000, 50000);   // 50 lots ask @ same price

    write_journal(out_dir + "/test-journals/auction_full_lifecycle.journal",
        "# auction_full_lifecycle.journal\n"
        "# Full session: Closed -> PreOpen -> collect -> indicative ->\n"
        "# execute_auction -> Continuous -> normal matching fill.\n",
        config, entries);
}

}  // namespace exchange

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    using namespace exchange;

    std::string out_dir;
    if (argc >= 2) {
        out_dir = argv[1];
    } else {
        const char* ws = std::getenv("BUILD_WORKSPACE_DIRECTORY");
        if (ws != nullptr && ws[0] != '\0') {
            out_dir = ws;
        } else {
            out_dir = ".";
        }
    }

    std::cout << "Writing auction journals to: " << out_dir << "/test-journals/\n";

    try {
        generate_simple_cross(out_dir);
        generate_multiple_levels(out_dir);
        generate_partial_fill(out_dir);
        generate_no_cross(out_dir);
        generate_empty_book(out_dir);
        generate_reference_tiebreak(out_dir);
        generate_indicative_price(out_dir);
        generate_full_lifecycle(out_dir);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Done.\n";
    return 0;
}
