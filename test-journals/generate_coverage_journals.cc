// generate_coverage_journals.cc
//
// Generates 7 critical coverage-gap .journal files by running each scenario
// through the real matching engine, capturing events via recording listeners,
// and serialising everything with JournalWriter.  Every EXPECT line is exactly
// what the engine produces — no hand-written expectations.
//
// Journals generated:
//   1. mass_cancel_account.journal
//   2. mass_cancel_all.journal
//   3. session_closed_rejects.journal
//   4. iceberg_basic.journal
//   5. iceberg_full_fill.journal
//   6. iceberg_validation.journal
//   7. smp_cancel_newest.journal
//
// Usage (via Bazel):
//   bazel run //test-journals:generate_coverage_journals

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
// Concrete engine types used by the generator
// ---------------------------------------------------------------------------

// Standard FIFO engine — matches FifoExchange in the test runner.
class CoverageExchange
    : public MatchingEngine<CoverageExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<CoverageExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;
};

// SMP FIFO engine — matches SmpFifoExchange in the test runner.
// is_self_match returns true when both orders share the same account_id.
// get_smp_action returns CancelNewest (cancel the aggressor).
class CoverageSmpExchange
    : public MatchingEngine<CoverageSmpExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<CoverageSmpExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;

    bool is_self_match(const Order& a, const Order& b) {
        return a.account_id == b.account_id;
    }
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }
};

// ---------------------------------------------------------------------------
// Helper infrastructure — mirrors generate_stress_journals.cc
// ---------------------------------------------------------------------------

struct CaptureContext {
    RecordingOrderListener& order_listener;
    RecordingMdListener&    md_listener;
};

// Build a JournalEntry from a ParsedAction plus whatever the engine fired
// into the two listeners.  Order events come first, then md events — same
// merge order as test_runner.cc run_impl().
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
                            const char* tif  = "GTC",
                            Quantity display_qty = 0) {
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
    if (display_qty > 0) {
        a.fields["display_qty"] = std::to_string(display_qty);
    }
    return a;
}

ParsedAction make_cancel_order(Timestamp ts, OrderId ord_id) {
    ParsedAction a;
    a.type = ParsedAction::Cancel;
    a.fields["ts"]     = std::to_string(ts);
    a.fields["ord_id"] = std::to_string(ord_id);
    return a;
}

ParsedAction make_modify_order(Timestamp ts, OrderId ord_id,
                               uint64_t cl_ord_id,
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

ParsedAction make_mass_cancel(Timestamp ts, uint64_t account_id) {
    ParsedAction a;
    a.type = ParsedAction::MassCancel;
    a.fields["ts"]         = std::to_string(ts);
    a.fields["account_id"] = std::to_string(account_id);
    return a;
}

ParsedAction make_mass_cancel_all(Timestamp ts) {
    ParsedAction a;
    a.type = ParsedAction::MassCancelAll;
    a.fields["ts"] = std::to_string(ts);
    return a;
}

// ---------------------------------------------------------------------------
// Write helper
// ---------------------------------------------------------------------------

void write_journal(const std::string& path, const std::string& comment,
                   const ParsedConfig& config,
                   const std::vector<JournalEntry>& entries) {
    std::string body = JournalWriter::to_string(config, entries);
    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open: " + path);
    }
    f << comment << body;
    if (!f) {
        throw std::runtime_error("Write error for: " + path);
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// ---------------------------------------------------------------------------
// 1. mass_cancel_account
//
// Place 2 orders from account 1 and 1 order from account 2.
// Issue MASS_CANCEL for account 1.
// Expected: account-1 orders cancelled (MassCancelled reason),
//           account-2 order survives.
// ---------------------------------------------------------------------------

void generate_mass_cancel_account(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Order 1: account 1, buy at 1000000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", 1000000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // Order 2: account 1, buy at 990000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 990000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", 990000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // Order 3: account 2, buy at 980000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 2;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 980000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 2, "BUY", 980000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // MASS_CANCEL account 1 — cancels orders 1 and 2, order 3 survives
    {
        order_listener.clear(); md_listener.clear();
        engine.mass_cancel(1, ts);
        entries.push_back(build_entry(make_mass_cancel(ts, 1), ctx));
        ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/mass_cancel_account.journal",
        "# mass_cancel_account.journal\n"
        "# Two orders from account 1, one from account 2.\n"
        "# MASS_CANCEL account 1 cancels exactly the two account-1 orders.\n"
        "# The account-2 order survives.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 2. mass_cancel_all
//
// Place 5 resting orders (mix of buy and sell at non-crossing prices).
// Issue MASS_CANCEL_ALL.
// Expected: all 5 cancelled, book empty.
// ---------------------------------------------------------------------------

void generate_mass_cancel_all(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // 3 buy orders
    for (int i = 0; i < 3; ++i) {
        Price price = static_cast<Price>(990000 + i * 10000);
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = static_cast<uint64_t>(i + 1);
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, req.account_id, "BUY",
                           price, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // 2 sell orders (above buys — no crossing)
    for (int i = 0; i < 2; ++i) {
        Price price = static_cast<Price>(1050000 + i * 10000);
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = static_cast<uint64_t>(i + 10);
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, req.account_id, "SELL",
                           price, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // MASS_CANCEL_ALL — cancels all 5
    {
        order_listener.clear(); md_listener.clear();
        engine.mass_cancel_all(ts);
        entries.push_back(build_entry(make_mass_cancel_all(ts), ctx));
        ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/mass_cancel_all.journal",
        "# mass_cancel_all.journal\n"
        "# Five resting orders (3 buys, 2 sells) from different accounts.\n"
        "# MASS_CANCEL_ALL cancels every active order; book becomes empty.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 3. session_closed_rejects
//
// Transition engine from Continuous to Closed.
// Verify that new_order, cancel_order, and modify_order are all rejected
// with ExchangeSpecific reason while the session is Closed.
// ---------------------------------------------------------------------------

void generate_session_closed_rejects(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Transition to Continuous (engine already starts there, but be explicit).
    {
        order_listener.clear(); md_listener.clear();
        engine.set_session_state(SessionState::Continuous, ts);
        entries.push_back(build_entry(
            make_set_session_state(ts, "CONTINUOUS"), ctx));
        ts += 1000;
    }

    // Transition to Closed.
    {
        order_listener.clear(); md_listener.clear();
        engine.set_session_state(SessionState::Closed, ts);
        entries.push_back(build_entry(
            make_set_session_state(ts, "CLOSED"), ctx));
        ts += 1000;
    }

    // New order → rejected (ExchangeSpecific).
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", 1000000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // Cancel unknown order (ord_id=999) → rejected (ExchangeSpecific).
    {
        order_listener.clear(); md_listener.clear();
        engine.cancel_order(999, ts);
        entries.push_back(build_entry(make_cancel_order(ts, 999), ctx));
        ts += 1000;
    }

    // Modify unknown order → rejected (ExchangeSpecific).
    {
        ModifyRequest req{};
        req.order_id        = 999;
        req.client_order_id = cl_id;
        req.new_price       = 1010000;
        req.new_quantity    = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.modify_order(req);
        entries.push_back(build_entry(
            make_modify_order(ts, 999, cl_id, 1010000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/session_closed_rejects.journal",
        "# session_closed_rejects.journal\n"
        "# Transition Continuous -> Closed, then verify that new_order,\n"
        "# cancel_order, and modify_order are all rejected with\n"
        "# EXCHANGE_SPECIFIC reason while session is Closed.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 4. iceberg_basic
//
// Engine starts in Continuous.
// Place an iceberg sell (total=30000, display=10000) at price 1000000.
// Then buy 10000 — fills the first tranche.
// Verify: tranche fill + reveal + priority loss callbacks fire.
// ---------------------------------------------------------------------------

void generate_iceberg_basic(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Iceberg sell: total=30000, display=10000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 30000;
        req.display_qty     = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        // Use display_qty param in make_new_order
        ParsedAction a = make_new_order(ts, cl_id, 1, "SELL", 1000000, 30000,
                                        "LIMIT", "GTC", 10000);
        entries.push_back(build_entry(std::move(a), ctx));
        ++cl_id; ts += 1000;
    }

    // Buy 10000 — fills the displayed tranche exactly, triggers reveal
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 2;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 2, "BUY", 1000000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/iceberg_basic.journal",
        "# iceberg_basic.journal\n"
        "# Iceberg sell (total=30000, display=10000) at price 1000000.\n"
        "# A buy of 10000 fills the first displayed tranche.\n"
        "# Engine reveals next tranche; order loses time-priority.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 5. iceberg_full_fill
//
// Iceberg sell (total=20000, display=10000).
// Buy 20000 — consumes both tranches entirely, order removed from book.
// ---------------------------------------------------------------------------

void generate_iceberg_full_fill(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Iceberg sell: total=20000, display=10000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 20000;
        req.display_qty     = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        ParsedAction a = make_new_order(ts, cl_id, 1, "SELL", 1000000, 20000,
                                        "LIMIT", "GTC", 10000);
        entries.push_back(build_entry(std::move(a), ctx));
        ++cl_id; ts += 1000;
    }

    // Buy 20000 — exhausts both tranches (10000 displayed + 10000 hidden)
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 2;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 20000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 2, "BUY", 1000000, 20000), ctx));
        ++cl_id; ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/iceberg_full_fill.journal",
        "# iceberg_full_fill.journal\n"
        "# Iceberg sell (total=20000, display=10000) at price 1000000.\n"
        "# A buy of 20000 consumes both tranches entirely.\n"
        "# Order is removed from book after full consumption.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 6. iceberg_validation
//
// Three iceberg orders testing validation edge cases:
//   a) display_qty > total_qty  -> rejected (InvalidQuantity)
//   b) display_qty not lot-aligned  -> rejected (InvalidQuantity)
//   c) display_qty == total_qty  -> accepted (degenerate iceberg)
// ---------------------------------------------------------------------------

void generate_iceberg_validation(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO";
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Case a: display_qty (20000) > total_qty (10000) -> InvalidQuantity
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.display_qty     = 20000;  // display > total -> invalid
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        ParsedAction a = make_new_order(ts, cl_id, 1, "SELL", 1000000, 10000,
                                        "LIMIT", "GTC", 20000);
        entries.push_back(build_entry(std::move(a), ctx));
        ++cl_id; ts += 1000;
    }

    // Case b: display_qty (5000) not lot-aligned (lot_size=10000) -> InvalidQuantity
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 30000;
        req.display_qty     = 5000;  // not a multiple of lot_size=10000 -> invalid
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        ParsedAction a = make_new_order(ts, cl_id, 1, "SELL", 1000000, 30000,
                                        "LIMIT", "GTC", 5000);
        entries.push_back(build_entry(std::move(a), ctx));
        ++cl_id; ts += 1000;
    }

    // Case c: display_qty == total_qty (degenerate iceberg) -> accepted
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.display_qty     = 10000;  // display == total -> valid
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        ParsedAction a = make_new_order(ts, cl_id, 1, "SELL", 1000000, 10000,
                                        "LIMIT", "GTC", 10000);
        entries.push_back(build_entry(std::move(a), ctx));
        ++cl_id; ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/iceberg_validation.journal",
        "# iceberg_validation.journal\n"
        "# Tests three iceberg validation edge cases:\n"
        "#   1. display_qty > total_qty          -> INVALID_QUANTITY reject\n"
        "#   2. display_qty not lot-aligned       -> INVALID_QUANTITY reject\n"
        "#   3. display_qty == total_qty (degen.) -> accepted\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// 7. smp_cancel_newest
//
// CONFIG match_algo=FIFO_SMP.
// Place a buy from account 1 (resting).
// Place a crossing sell from account 1 (same account = self-match).
// Expected: sell accepted then immediately cancelled (SelfMatchPrevention).
// No trade occurs; the resting buy survives.
// ---------------------------------------------------------------------------

void generate_smp_cancel_newest(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo    = "FIFO_SMP";  // selects SmpFifoExchange at replay
    config.tick_size     = 100;
    config.lot_size      = 10000;
    config.max_orders    = 1000;
    config.max_levels    = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    CoverageSmpExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;
    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Resting buy: account 1, limit at 1000000
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", 1000000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    // Crossing sell from same account 1 — triggers SMP CancelNewest
    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = 1000000;
        req.quantity        = 10000;
        req.timestamp       = ts;
        order_listener.clear(); md_listener.clear();
        engine.new_order(req);
        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "SELL", 1000000, 10000), ctx));
        ++cl_id; ts += 1000;
    }

    write_journal(
        out_dir + "/test-journals/smp_cancel_newest.journal",
        "# smp_cancel_newest.journal\n"
        "# CONFIG match_algo=FIFO_SMP (SmpFifoExchange with CancelNewest).\n"
        "# Resting buy from account 1, then crossing sell from same account.\n"
        "# SMP fires: sell (aggressor) is accepted then immediately cancelled\n"
        "# with reason SELF_MATCH_PREVENTION. No trade occurs; buy survives.\n",
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

    std::cout << "Writing coverage journals to: " << out_dir
              << "/test-journals/\n";

    try {
        generate_mass_cancel_account(out_dir);
        generate_mass_cancel_all(out_dir);
        generate_session_closed_rejects(out_dir);
        generate_iceberg_basic(out_dir);
        generate_iceberg_full_fill(out_dir);
        generate_iceberg_validation(out_dir);
        generate_smp_cancel_newest(out_dir);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Done.\n";
    return 0;
}
