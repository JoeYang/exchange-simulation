// generate_important_journals.cc
//
// Generates the Priority-2 "important" coverage-gap journal files by running
// each scenario through the real matching engine, capturing events via
// RecordingListeners, and serialising everything with JournalWriter.
// Every EXPECT line is exactly what the engine produces — no hand-written
// expectations.
//
// Journals generated:
//   #8  cancel_partial_fill.journal         -- partial fill then user cancel
//   #9  session_preopen_market_reject.journal -- market order in PreOpen
//   #11 iceberg_priority_loss.journal        -- iceberg tranche lose priority
//   #13 pro_rata_multi_level.journal         -- ProRata sweep across 2 levels
//   #14 auction_iceberg.journal              -- iceberg in auction collection
//   #17 modify_triggers_stop.journal         -- modify crosses spread, triggers stop
//   #18 session_preclose_collect.journal     -- orders rest in PreClose, no match
//
// Usage (via Bazel):
//   bazel run //test-journals:generate_important_journals
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
// Concrete engine types
//
// ImpFifoExchange   -- FIFO matching, pool sizes matching the test runner so
//                      generated journals replay without hitting limits.
// ImpProRataExchange -- ProRata matching, same pool sizes.
// ---------------------------------------------------------------------------

class ImpFifoExchange
    : public MatchingEngine<ImpFifoExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<ImpFifoExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;
};

class ImpProRataExchange
    : public MatchingEngine<ImpProRataExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            ProRataMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<ImpProRataExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                ProRataMatch,
                                1000, 100, 10000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Helper infrastructure (same pattern as other generators)
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
    // ParsedExpectation values.
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

ParsedAction make_new_order_stop(Timestamp ts, uint64_t cl_ord_id,
                                 uint64_t account_id, const char* side,
                                 Price stop_price, Quantity qty) {
    ParsedAction a;
    a.type = ParsedAction::NewOrder;
    a.fields["ts"]          = std::to_string(ts);
    a.fields["cl_ord_id"]   = std::to_string(cl_ord_id);
    a.fields["account_id"]  = std::to_string(account_id);
    a.fields["side"]        = side;
    a.fields["price"]       = "0";
    a.fields["qty"]         = std::to_string(qty);
    a.fields["type"]        = "STOP";
    a.fields["tif"]         = "GTC";
    a.fields["stop_price"]  = std::to_string(stop_price);
    return a;
}

ParsedAction make_cancel(Timestamp ts, OrderId ord_id) {
    ParsedAction a;
    a.type = ParsedAction::Cancel;
    a.fields["ts"]     = std::to_string(ts);
    a.fields["ord_id"] = std::to_string(ord_id);
    return a;
}

ParsedAction make_modify(Timestamp ts, OrderId ord_id, uint64_t cl_ord_id,
                         Price new_price, Quantity new_qty) {
    ParsedAction a;
    a.type = ParsedAction::Modify;
    a.fields["ts"]        = std::to_string(ts);
    a.fields["ord_id"]    = std::to_string(ord_id);
    a.fields["cl_ord_id"] = std::to_string(cl_ord_id);
    a.fields["new_price"] = std::to_string(new_price);
    a.fields["new_qty"]   = std::to_string(new_qty);
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

// ---------------------------------------------------------------------------
// Run helpers (templated to support both FIFO and ProRata engines)
// ---------------------------------------------------------------------------

template <typename EngineT>
void run_new_order(EngineT& engine, CaptureContext& ctx,
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

template <typename EngineT>
void run_new_order_stop(EngineT& engine, CaptureContext& ctx,
                        std::vector<JournalEntry>& entries,
                        Timestamp ts, uint64_t cl_ord_id, uint64_t account_id,
                        const char* side, Price stop_price, Quantity qty) {
    OrderRequest req{};
    req.timestamp       = ts;
    req.client_order_id = cl_ord_id;
    req.account_id      = account_id;
    req.side            = (std::string(side) == "BUY") ? Side::Buy : Side::Sell;
    req.type            = OrderType::Stop;
    req.tif             = TimeInForce::GTC;
    req.price           = 0;
    req.quantity        = qty;
    req.stop_price      = stop_price;

    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.new_order(req);
    entries.push_back(build_entry(
        make_new_order_stop(ts, cl_ord_id, account_id, side, stop_price, qty),
        ctx));
}

template <typename EngineT>
void run_new_order_iceberg(EngineT& engine, CaptureContext& ctx,
                           std::vector<JournalEntry>& entries,
                           Timestamp ts, uint64_t cl_ord_id,
                           uint64_t account_id, const char* side,
                           Price price, Quantity qty, Quantity display_qty) {
    OrderRequest req{};
    req.timestamp       = ts;
    req.client_order_id = cl_ord_id;
    req.account_id      = account_id;
    req.side            = (std::string(side) == "BUY") ? Side::Buy : Side::Sell;
    req.type            = OrderType::Limit;
    req.tif             = TimeInForce::GTC;
    req.price           = price;
    req.quantity        = qty;
    req.display_qty     = display_qty;

    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.new_order(req);

    ParsedAction a = make_new_order(ts, cl_ord_id, account_id, side,
                                    price, qty);
    a.fields["display_qty"] = std::to_string(display_qty);
    entries.push_back(build_entry(std::move(a), ctx));
}

template <typename EngineT>
void run_cancel(EngineT& engine, CaptureContext& ctx,
                std::vector<JournalEntry>& entries,
                Timestamp ts, OrderId ord_id) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.cancel_order(ord_id, ts);
    entries.push_back(build_entry(make_cancel(ts, ord_id), ctx));
}

template <typename EngineT>
void run_modify(EngineT& engine, CaptureContext& ctx,
                std::vector<JournalEntry>& entries,
                Timestamp ts, OrderId ord_id, uint64_t cl_ord_id,
                Price new_price, Quantity new_qty) {
    ModifyRequest req{};
    req.order_id        = ord_id;
    req.client_order_id = cl_ord_id;
    req.new_price       = new_price;
    req.new_quantity    = new_qty;
    req.timestamp       = ts;

    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.modify_order(req);
    entries.push_back(build_entry(
        make_modify(ts, ord_id, cl_ord_id, new_price, new_qty), ctx));
}

template <typename EngineT>
void run_set_session_state(EngineT& engine, CaptureContext& ctx,
                           std::vector<JournalEntry>& entries,
                           Timestamp ts, SessionState state,
                           const char* state_str) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.set_session_state(state, ts);
    entries.push_back(build_entry(
        make_set_session_state(ts, state_str), ctx));
}

template <typename EngineT>
void run_execute_auction(EngineT& engine, CaptureContext& ctx,
                         std::vector<JournalEntry>& entries,
                         Timestamp ts, Price reference_price) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.execute_auction(reference_price, ts);
    entries.push_back(build_entry(
        make_execute_auction(ts, reference_price), ctx));
}

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

ParsedConfig make_fifo_config() {
    ParsedConfig cfg;
    cfg.match_algo    = "FIFO";
    cfg.tick_size     = 100;
    cfg.lot_size      = 10000;
    cfg.max_orders    = 1000;
    cfg.max_levels    = 100;
    cfg.max_order_ids = 10000;
    return cfg;
}

ParsedConfig make_pro_rata_config() {
    ParsedConfig cfg;
    cfg.match_algo    = "PRO_RATA";
    cfg.tick_size     = 100;
    cfg.lot_size      = 10000;
    cfg.max_orders    = 1000;
    cfg.max_levels    = 100;
    cfg.max_order_ids = 10000;
    return cfg;
}

// ---------------------------------------------------------------------------
// Scenario #8: cancel_partial_fill
//
// Buy 30000. Sell 10000 partially fills it (resting_rem=20000). User then
// cancels the remainder. Verifies:
//   - ORDER_PARTIALLY_FILLED fires with correct remaining quantities
//   - ORDER_CANCELLED fires with remaining qty = 20000
//   - L3 Cancel and L2 Update/Remove fire with correct quantities
// ---------------------------------------------------------------------------
void generate_cancel_partial_fill(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: place resting buy 30000 at 1005000
    // ord_id = 1
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1005000, 30000);

    // Action 1: aggressor sell 10000 — partial fill (10000 of 30000)
    // ord_id = 2; ORDER_PARTIALLY_FILLED(agg=2, rest=1, qty=10000,
    //                                    agg_rem=0, rest_rem=20000)
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "SELL", 1005000, 10000);

    // Action 2: cancel the remainder of the buy order (ord_id=1, rem=20000)
    run_cancel(engine, ctx, entries, 3000, 1);

    write_journal(out_dir + "/test-journals/cancel_partial_fill.journal",
        "# cancel_partial_fill.journal\n"
        "# Buy 30000. Sell 10000 partially fills it (remaining=20000).\n"
        "# User cancels the remainder.\n"
        "# Verifies ORDER_CANCELLED fires with the correct remaining qty,\n"
        "# and that the L3 Cancel and L2 Remove events are correct.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #9: session_preopen_market_reject
//
// Transition to PreOpen. Submit a market order. The engine accepts it then
// immediately cancels it with IOCRemainder — market orders cannot rest in
// collection phases.
// ---------------------------------------------------------------------------
void generate_session_preopen_market_reject(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: transition to PreOpen
    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Action 1: market order — accepted then cancelled IOC_REMAINDER
    // price=0 for market order; no MD events (nothing inserted in book)
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 0, 10000, "MARKET", "GTC");

    write_journal(
        out_dir + "/test-journals/session_preopen_market_reject.journal",
        "# session_preopen_market_reject.journal\n"
        "# In PreOpen, the engine is in collection phase (no matching).\n"
        "# A market order is accepted and then immediately cancelled with\n"
        "# IOCRemainder because market orders cannot rest.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #11: iceberg_priority_loss
//
// Iceberg sell (total=20000, display=10000) placed first at price P.
// Plain sell 10000 placed second at same price P.
// Book at P: [iceberg(disp=10000), plain(10000)] — iceberg has time priority.
//
// Buy 10000 fills the iceberg's first tranche.
// Iceberg reveals second tranche (10000) but is moved to back of queue.
// Book at P: [plain(10000), iceberg(10000)]
//
// Buy another 10000: fills the plain order (not the iceberg's second tranche).
// This confirms priority loss: the plain order goes ahead of the iceberg.
// ---------------------------------------------------------------------------
void generate_iceberg_priority_loss(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: iceberg sell at 1005000, total=20000, display=10000
    // ord_id = 1
    run_new_order_iceberg(engine, ctx, entries,
                          1000, 1, 1, "SELL", 1005000, 20000, 10000);

    // Action 1: plain sell 10000 at same price 1005000
    // ord_id = 2; goes behind iceberg in queue
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "SELL", 1005000, 10000);

    // Action 2: aggressor buy 10000 fills iceberg first tranche
    // ord_id = 3; ORDER_FILLED(agg=3, rest=1, qty=10000)
    // Iceberg reveals second tranche (10000) — moves to back of queue
    // Queue at 1005000: [plain(ord2, 10000), iceberg_tranche2(ord1, 10000)]
    run_new_order(engine, ctx, entries,
                  3000, 3, 3, "BUY", 1005000, 10000);

    // Action 3: aggressor buy 10000 fills the PLAIN order (ord2), not iceberg
    // This proves priority loss: ord2 is now ahead of iceberg's 2nd tranche
    // ord_id = 4; ORDER_FILLED(agg=4, rest=2, qty=10000)
    run_new_order(engine, ctx, entries,
                  4000, 4, 4, "BUY", 1005000, 10000);

    write_journal(out_dir + "/test-journals/iceberg_priority_loss.journal",
        "# iceberg_priority_loss.journal\n"
        "# Iceberg sell (total=20000, display=10000) placed first at 1005000.\n"
        "# Plain sell 10000 placed second at same price.\n"
        "# Buy 10000 fills iceberg first tranche; iceberg reveals next tranche\n"
        "# but is moved to back of queue (priority loss).\n"
        "# Buy another 10000 fills the PLAIN order, not the iceberg's 2nd tranche.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #13: pro_rata_multi_level
//
// ProRata engine. Orders at two price levels. Aggressor sweeps both.
//
// Setup (sells resting):
//   Level 1005000: sell 30000 (ord1) + sell 20000 (ord2) = 50000 total
//   Level 1006000: sell 40000 (ord3)
//
// Aggressor buy 90000 at 1006000: sweeps level 1005000 first (better price),
// then level 1006000.
//
// At level 1005000 (50000 total, 50000 remaining aggressor needs 90000):
//   ProRata: ord1 gets floor(30000*50000/50000)=30000, ord2 gets 20000
//   (whole level consumed)
// At level 1006000 (40000 available, 40000 still needed):
//   Single order ord3: full fill 40000
// ---------------------------------------------------------------------------
void generate_pro_rata_multi_level(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_pro_rata_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpProRataExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: sell 30000 at 1005000 (ord_id=1)
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "SELL", 1005000, 30000);

    // Action 1: sell 20000 at 1005000 (ord_id=2)
    run_new_order(engine, ctx, entries,
                  2000, 2, 1, "SELL", 1005000, 20000);

    // Action 2: sell 40000 at 1006000 (ord_id=3)
    run_new_order(engine, ctx, entries,
                  3000, 3, 1, "SELL", 1006000, 40000);

    // Action 3: aggressor buy 90000 at 1006000 sweeps both levels
    // Level 1005000 fills first (best ask): ProRata fills ord1 and ord2
    // Level 1006000 fills next: ord3 fully filled
    run_new_order(engine, ctx, entries,
                  4000, 4, 2, "BUY", 1006000, 90000);

    write_journal(out_dir + "/test-journals/pro_rata_multi_level.journal",
        "# pro_rata_multi_level.journal\n"
        "# ProRata engine. Resting sells at two price levels.\n"
        "# Level 1005000: ord1(30000) + ord2(20000) = 50000 total.\n"
        "# Level 1006000: ord3(40000).\n"
        "# Aggressor buy 90000 sweeps both levels.\n"
        "# Level 1005000 consumed entirely (ProRata allocation),\n"
        "# then level 1006000 consumed fully.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #14: auction_iceberg
//
// PreOpen collection phase: place an iceberg bid and a plain ask.
// Execute auction. Verify the auction only sees the display qty from the
// iceberg (not the full total_qty), and fills correctly.
//
// Setup:
//   Iceberg buy: total=20000, display=10000 at price 1010000
//   Plain ask:   10000 at price 990000
//
// Auction at ref=1000000: best bid=1010000 (display=10000), best ask=990000
// (10000). Both sides have 10000 visible -> matched = 10000.
// Iceberg's first tranche (10000) fills. Second tranche reveals.
// ---------------------------------------------------------------------------
void generate_auction_iceberg(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: transition to PreOpen
    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Action 1: iceberg buy, total=20000, display=10000 at 1010000 (ord_id=1)
    run_new_order_iceberg(engine, ctx, entries,
                          2000, 1, 1, "BUY", 1010000, 20000, 10000);

    // Action 2: plain ask 10000 at 990000 (ord_id=2)
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL", 990000, 10000);

    // Action 3: execute auction at reference_price=1000000
    // Only iceberg display qty (10000) is visible on the buy side.
    // Auction fills 10000: first iceberg tranche consumed, second revealed.
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    write_journal(out_dir + "/test-journals/auction_iceberg.journal",
        "# auction_iceberg.journal\n"
        "# PreOpen: place iceberg bid (total=20000, display=10000) and plain ask 10000.\n"
        "# Execute auction: only display qty visible on buy side.\n"
        "# Auction fills 10000 (first iceberg tranche).\n"
        "# Second tranche reveals after fill.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #17: modify_triggers_stop
//
// Place a resting sell, place a buy stop order, then place a buy order
// below the ask (no fill). Modify the buy to cross the spread ->
// fill -> triggers the stop.
//
// Setup:
//   ord1: limit sell 10000 at 1006000 (ask)
//   ord2: buy stop at stop_price=1006000 (triggers when last_trade >= 1006000)
//   ord3: limit buy  10000 at 1004000 (below ask, no fill)
//
// Modify ord3: new_price=1006000, new_qty=10000
//   -> cancel-replace fires: removes ord3 at 1004000, re-enters at 1006000
//   -> crosses resting sell ord1 -> ORDER_FILLED
//   -> last_trade = 1006000 >= stop_price(ord2) = 1006000 -> stop triggers
//   -> ord2 becomes market buy, no liquidity left -> IOC_REMAINDER cancel
// ---------------------------------------------------------------------------
void generate_modify_triggers_stop(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: limit sell rests at 1006000 (ord_id=1)
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "SELL", 1006000, 10000);

    // Action 1: buy stop at stop_price=1006000 (ord_id=2)
    // Stop orders go to stop book — only ORDER_ACCEPTED fires, no MD events.
    run_new_order_stop(engine, ctx, entries,
                       2000, 2, 2, "BUY", 1006000, 10000);

    // Action 2: limit buy at 1004000 — below ask, does not cross (ord_id=3)
    run_new_order(engine, ctx, entries,
                  3000, 3, 3, "BUY", 1004000, 10000);

    // Action 3: modify ord3 price to 1006000 — crosses resting sell ord1
    // Order events: ORDER_MODIFIED(ord3), ORDER_FILLED(agg=3, rest=1)
    //               then stop cascade: ORDER_CANCELLED(ord2, IOC_REMAINDER)
    // MD events: OBA(CANCEL ord3 at 1004000), DU(REMOVE 1004000),
    //            TRADE(1006000), OBA(FILL ord1), DU(REMOVE ask 1006000),
    //            TOP_OF_BOOK(bid=0, ask=0)
    run_modify(engine, ctx, entries, 4000, 3, 4, 1006000, 10000);

    write_journal(out_dir + "/test-journals/modify_triggers_stop.journal",
        "# modify_triggers_stop.journal\n"
        "# Resting sell at 1006000. Buy stop at stop_price=1006000.\n"
        "# Buy at 1004000 (below ask, no fill).\n"
        "# Modify buy to 1006000 -> crosses sell -> fill -> triggers stop.\n"
        "# Stop becomes market buy, no liquidity -> IOC_REMAINDER cancel.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario #18: session_preclose_collect
//
// Transition to PreClose. Place crossing orders (bid > ask). Verify they
// rest without matching — PreClose is a collection phase like PreOpen.
// ---------------------------------------------------------------------------
void generate_session_preclose_collect(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    ImpFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Action 0: transition to PreClose
    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreClose, "PRE_CLOSE");

    // Action 1: bid at 1010000 (ord_id=1) — should rest, not match
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1010000, 10000);

    // Action 2: ask at 990000 (ord_id=2) — crosses spread but should NOT fill
    // In PreClose (collection phase) matching is suppressed
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL", 990000, 10000);

    write_journal(
        out_dir + "/test-journals/session_preclose_collect.journal",
        "# session_preclose_collect.journal\n"
        "# PreClose is a collection phase: orders rest without matching.\n"
        "# Crossing orders (bid 1010000 > ask 990000) are placed but do NOT fill.\n"
        "# Verifies that the engine suppresses matching during PreClose.\n",
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

    std::cout << "Writing important journals to: " << out_dir
              << "/test-journals/\n";

    try {
        generate_cancel_partial_fill(out_dir);
        generate_session_preopen_market_reject(out_dir);
        generate_iceberg_priority_loss(out_dir);
        generate_pro_rata_multi_level(out_dir);
        generate_auction_iceberg(out_dir);
        generate_modify_triggers_stop(out_dir);
        generate_session_preclose_collect(out_dir);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Done.\n";
    return 0;
}
