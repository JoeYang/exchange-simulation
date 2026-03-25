// generate_economics_journals.cc
//
// Generates journal files that rigorously verify the ECONOMICS of
// transactions in the exchange-core matching engine: fill prices, fill
// quantities, remaining quantities, pro-rata allocations, auction
// equilibrium prices, and quantity conservation (no quantity created or
// destroyed).
//
// Each scenario runs through the real engine, captures events via recording
// listeners, validates economics invariants with assertions, and serialises
// everything with JournalWriter.
//
// Usage (via Bazel):
//   bazel run //test-journals:generate_economics_journals
//
// The generator writes files relative to the directory given as the first
// command-line argument, or $BUILD_WORKSPACE_DIRECTORY (set by `bazel run`),
// or the current working directory as a fallback.

#include "exchange-core/matching_engine.h"
#include "exchange-core/match_algo.h"
#include "test-harness/recording_listener.h"
#include "test-harness/journal_parser.h"
#include "test-harness/journal_writer.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Engine types for generation
// ---------------------------------------------------------------------------

class EconFifoExchange
    : public MatchingEngine<EconFifoExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<EconFifoExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;
};

class EconProRataExchange
    : public MatchingEngine<EconProRataExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            ProRataMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<EconProRataExchange,
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

// --- Action factory helpers ---

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

ParsedAction make_publish_indicative(Timestamp ts, Price reference_price) {
    ParsedAction a;
    a.type = ParsedAction::PublishIndicative;
    a.fields["ts"]              = std::to_string(ts);
    a.fields["reference_price"] = std::to_string(reference_price);
    return a;
}

// --- Run helpers (template to support both FIFO and ProRata engines) ---

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
void run_new_order_iceberg(EngineT& engine, CaptureContext& ctx,
                           std::vector<JournalEntry>& entries,
                           Timestamp ts, uint64_t cl_ord_id,
                           uint64_t account_id,
                           const char* side, Price price, Quantity qty,
                           Quantity display_qty) {
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

    // Build action with display_qty field
    ParsedAction a = make_new_order(ts, cl_ord_id, account_id, side,
                                    price, qty);
    a.fields["display_qty"] = std::to_string(display_qty);
    entries.push_back(build_entry(std::move(a), ctx));
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
    entries.push_back(build_entry(make_set_session_state(ts, state_str), ctx));
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

template <typename EngineT>
void run_publish_indicative(EngineT& engine, CaptureContext& ctx,
                            std::vector<JournalEntry>& entries,
                            Timestamp ts, Price reference_price) {
    ctx.order_listener.clear();
    ctx.md_listener.clear();
    engine.publish_indicative_price(reference_price, ts);
    entries.push_back(build_entry(
        make_publish_indicative(ts, reference_price), ctx));
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
// Economics assertion helpers
//
// These verify invariants on the captured events BEFORE writing the journal,
// so the generator itself validates the math.
// ---------------------------------------------------------------------------

struct FillInfo {
    OrderId aggressor_id;
    OrderId resting_id;
    Price   price;
    Quantity quantity;
};

// Extract fill events from a set of recorded events.
std::vector<FillInfo> extract_fills(
        const RecordingOrderListener& order_listener) {
    std::vector<FillInfo> fills;
    for (const auto& ev : order_listener.events()) {
        if (auto* f = std::get_if<OrderFilled>(&ev)) {
            fills.push_back({f->aggressor_id, f->resting_id,
                             f->price, f->quantity});
        } else if (auto* pf = std::get_if<OrderPartiallyFilled>(&ev)) {
            fills.push_back({pf->aggressor_id, pf->resting_id,
                             pf->price, pf->quantity});
        }
    }
    return fills;
}

// Extract Trade events from market data listener.
struct TradeInfo {
    Price    price;
    Quantity quantity;
    OrderId  aggressor_id;
    OrderId  resting_id;
};

std::vector<TradeInfo> extract_trades(
        const RecordingMdListener& md_listener) {
    std::vector<TradeInfo> trades;
    for (const auto& ev : md_listener.events()) {
        if (auto* t = std::get_if<Trade>(&ev)) {
            trades.push_back({t->price, t->quantity,
                              t->aggressor_id, t->resting_id});
        }
    }
    return trades;
}

// Assert that total filled quantity for a given aggressor equals expected.
void assert_aggressor_total(const std::vector<FillInfo>& fills,
                            OrderId aggressor_id,
                            Quantity expected_total) {
    Quantity total = 0;
    for (const auto& f : fills) {
        if (f.aggressor_id == aggressor_id) total += f.quantity;
    }
    assert(total == expected_total);
}

// Assert all fill prices equal expected.
void assert_all_fills_at_price(const std::vector<FillInfo>& fills,
                               Price expected_price) {
    for (const auto& f : fills) {
        assert(f.price == expected_price);
    }
}

// Assert all trades at a single price.
void assert_all_trades_at_price(const std::vector<TradeInfo>& trades,
                                Price expected_price) {
    for (const auto& t : trades) {
        assert(t.price == expected_price);
    }
}

// ---------------------------------------------------------------------------
// Scenario 1: economics_fill_price_accuracy
//
// Verify that fill price is always the resting order's price (continuous):
//   Buy @ 101.0000, Sell @ 100.5000 -> fill at 100.5000 (resting sell price)
//   Sell @ 99.0000, Buy @ 99.5000 -> fill at 99.5000 (resting buy price)
// ---------------------------------------------------------------------------
void generate_fill_price_accuracy(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Subtest 1: Sell rests at 100.5000, Buy crosses at 101.0000.
    // Fill price = resting price = 100.5000 = 1005000.
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "SELL", 1005000, 10000);
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "BUY", 1010000, 10000);

    // Verify: check the fill from action 2
    {
        auto fills = extract_fills(order_listener);
        auto trades = extract_trades(md_listener);
        // Events were cleared by build_entry, so re-run to verify
        // We verify using the just-built entry instead.
    }

    // Subtest 2: Buy rests at 99.5000, Sell crosses at 99.0000.
    // Fill price = resting price = 99.5000 = 995000.
    run_new_order(engine, ctx, entries,
                  3000, 3, 3, "BUY", 995000, 10000);
    run_new_order(engine, ctx, entries,
                  4000, 4, 4, "SELL", 990000, 10000);

    // Post-generation economics assertion: verify fill prices in journal
    // entries by checking the EXPECT lines.
    // Action 1 (index 1) should have a fill at price=1005000
    // Action 3 (index 3) should have a fill at price=995000
    bool found_fill_1 = false;
    bool found_fill_2 = false;
    for (const auto& exp : entries[1].expectations) {
        if (exp.event_type == "ORDER_FILLED" ||
            exp.event_type == "ORDER_PARTIALLY_FILLED") {
            assert(exp.get_int("price") == 1005000);
            found_fill_1 = true;
        }
        if (exp.event_type == "TRADE") {
            assert(exp.get_int("price") == 1005000);
        }
    }
    for (const auto& exp : entries[3].expectations) {
        if (exp.event_type == "ORDER_FILLED" ||
            exp.event_type == "ORDER_PARTIALLY_FILLED") {
            assert(exp.get_int("price") == 995000);
            found_fill_2 = true;
        }
        if (exp.event_type == "TRADE") {
            assert(exp.get_int("price") == 995000);
        }
    }
    assert(found_fill_1);
    assert(found_fill_2);

    write_journal(out_dir + "/test-journals/economics_fill_price_accuracy.journal",
        "# economics_fill_price_accuracy.journal\n"
        "# Verify fill price is always the resting order's price.\n"
        "# Subtest 1: Sell rests @ 100.5000, Buy crosses @ 101.0000\n"
        "#   -> fill at 100.5000 (resting sell price)\n"
        "# Subtest 2: Buy rests @ 99.5000, Sell crosses @ 99.0000\n"
        "#   -> fill at 99.5000 (resting buy price)\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 2: economics_quantity_conservation
//
// Verify quantity is conserved:
//   Buy 10, Sell 10 -> both fully filled, quantities match
//   Buy 10, Sell 3 -> partial: aggressor remaining = 7, resting remaining = 0
//   Buy 10, Sell 3, Sell 4, Sell 3 -> fill quantities sum to 10
// ---------------------------------------------------------------------------
void generate_quantity_conservation(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Subtest 1: Buy 10 lots @ 100.0000, Sell 10 lots @ 100.0000
    // Both fully filled, quantities match.
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1000000, 100000);   // 10 lots
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "SELL", 1000000, 100000);  // 10 lots

    // Verify full fill: action 1 should have ORDER_FILLED qty=100000
    {
        bool found = false;
        for (const auto& exp : entries[1].expectations) {
            if (exp.event_type == "ORDER_FILLED") {
                assert(exp.get_int("qty") == 100000);
                found = true;
            }
        }
        assert(found);
    }

    // Subtest 2: Buy 10 lots, Sell 3 lots -> partial fill.
    run_new_order(engine, ctx, entries,
                  3000, 3, 3, "BUY", 1000000, 100000);   // 10 lots
    run_new_order(engine, ctx, entries,
                  4000, 4, 4, "SELL", 1000000, 30000);   // 3 lots

    // Action 3 (sell 3 lots): ORDER_PARTIALLY_FILLED, aggressor_remaining should
    // reflect remaining sell (0, fully consumed), resting_remaining = 70000.
    // Actually: aggressor is order 4 (sell), remaining = 0 -> ORDER_FILLED.
    // The resting order 3 has remaining 70000.
    {
        bool found = false;
        for (const auto& exp : entries[3].expectations) {
            if (exp.event_type == "ORDER_FILLED") {
                assert(exp.get_int("qty") == 30000);
                found = true;
            }
        }
        assert(found);
    }

    // Subtest 3: Sell 4 lots and Sell 3 lots to fill the remaining 7 lots.
    run_new_order(engine, ctx, entries,
                  5000, 5, 5, "SELL", 1000000, 40000);   // 4 lots
    run_new_order(engine, ctx, entries,
                  6000, 6, 6, "SELL", 1000000, 30000);   // 3 lots

    // Verify total fill on the resting buy (order 3 = 10 lots):
    // Fills should sum to 100000 across actions 3, 4, 5.
    Quantity total_buy_filled = 0;
    for (size_t action_idx = 3; action_idx <= 5; ++action_idx) {
        for (const auto& exp : entries[action_idx].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                total_buy_filled += exp.get_int("qty");
            }
        }
    }
    assert(total_buy_filled == 100000);

    write_journal(out_dir + "/test-journals/economics_quantity_conservation.journal",
        "# economics_quantity_conservation.journal\n"
        "# Verify quantity conservation: what fills on aggressor = resting.\n"
        "# Subtest 1: Buy 10 + Sell 10 -> both fully filled\n"
        "# Subtest 2: Buy 10 + Sell 3 -> partial fill (3 lots)\n"
        "# Subtest 3: Sell 4 + Sell 3 -> completes the remaining 7 lots\n"
        "# Total fills on buy order sum to exactly 10 lots.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 3: economics_partial_fill_remaining
//
// Verify remaining_quantity tracking through multiple partial fills:
//   Place Buy 100 lots
//   Sell 20 lots -> aggressor_remaining=80 (on buy), resting filled=20
//   Sell 30 lots -> aggressor_remaining=50, resting filled=30
//   Sell 50 lots -> aggressor_remaining=0, fully filled
// ---------------------------------------------------------------------------
void generate_partial_fill_remaining(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Place buy 100 lots @ 100.0000 (qty=1000000)
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1000000, 1000000);

    // Sell 20 lots: partial fill. Aggressor (sell) fully consumed.
    // Resting buy remaining = 800000.
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "SELL", 1000000, 200000);

    // Verify: ORDER_FILLED with qty=200000 (sell fully consumed)
    {
        bool found = false;
        for (const auto& exp : entries[1].expectations) {
            if (exp.event_type == "ORDER_FILLED") {
                assert(exp.get_int("qty") == 200000);
                found = true;
            }
        }
        assert(found);
    }

    // Sell 30 lots: partial fill. Buy remaining = 500000.
    run_new_order(engine, ctx, entries,
                  3000, 3, 3, "SELL", 1000000, 300000);

    {
        bool found = false;
        for (const auto& exp : entries[2].expectations) {
            if (exp.event_type == "ORDER_FILLED") {
                assert(exp.get_int("qty") == 300000);
                found = true;
            }
        }
        assert(found);
    }

    // Sell 50 lots: final fill. Buy remaining = 0.
    run_new_order(engine, ctx, entries,
                  4000, 4, 4, "SELL", 1000000, 500000);

    // This should be ORDER_FILLED since both are fully consumed.
    {
        bool found = false;
        for (const auto& exp : entries[3].expectations) {
            if (exp.event_type == "ORDER_FILLED") {
                assert(exp.get_int("qty") == 500000);
                found = true;
            }
        }
        assert(found);
    }

    // Verify total fills sum = 1000000 (100 lots)
    Quantity total = 0;
    for (size_t i = 1; i <= 3; ++i) {
        for (const auto& exp : entries[i].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                total += exp.get_int("qty");
            }
        }
    }
    assert(total == 1000000);

    write_journal(
        out_dir + "/test-journals/economics_partial_fill_remaining.journal",
        "# economics_partial_fill_remaining.journal\n"
        "# Verify remaining_quantity tracking through multiple partial fills.\n"
        "# Buy 100 lots -> Sell 20 -> Sell 30 -> Sell 50.\n"
        "# Fill quantities: 200000 + 300000 + 500000 = 1000000 (100 lots).\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 4: economics_pro_rata_allocation
//
// Verify pro-rata math is exact:
//   3 resting orders: 100, 200, 300 lots (total 600) at same price
//   Aggressor: 300 lots -> expected: 50, 100, 150 (exact proportional)
//   Aggressor: 100 lots -> floor allocations + remainder = 100 lots total
// ---------------------------------------------------------------------------
void generate_pro_rata_allocation(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_pro_rata_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconProRataExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Place 3 resting buy orders at same price 100.0000:
    //   o1: 100 lots (qty=1000000)
    //   o2: 200 lots (qty=2000000)
    //   o3: 300 lots (qty=3000000)
    // Total = 600 lots = 6000000
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1000000, 1000000);
    run_new_order(engine, ctx, entries,
                  2000, 2, 1, "BUY", 1000000, 2000000);
    run_new_order(engine, ctx, entries,
                  3000, 3, 1, "BUY", 1000000, 3000000);

    // Aggressor 1: Sell 300 lots (3000000).
    // Expected allocations: floor(1000000*3000000/6000000)=500000 (50 lots)
    //                       floor(2000000*3000000/6000000)=1000000 (100 lots)
    //                       floor(3000000*3000000/6000000)=1500000 (150 lots)
    //                       Sum = 3000000 (no remainder)
    run_new_order(engine, ctx, entries,
                  4000, 4, 2, "SELL", 1000000, 3000000);

    // Verify fill quantities from action 3 (aggressor sell 300 lots)
    {
        Quantity total_fill = 0;
        std::vector<Quantity> fill_qtys;
        for (const auto& exp : entries[3].expectations) {
            if (exp.event_type == "ORDER_PARTIALLY_FILLED" ||
                exp.event_type == "ORDER_FILLED") {
                Quantity q = exp.get_int("qty");
                fill_qtys.push_back(q);
                total_fill += q;
            }
        }
        // Total must equal aggressor quantity
        assert(total_fill == 3000000);
        // Exact pro-rata: 500000, 1000000, 1500000
        assert(fill_qtys.size() == 3);
        assert(fill_qtys[0] == 500000);
        assert(fill_qtys[1] == 1000000);
        assert(fill_qtys[2] == 1500000);
    }

    // Aggressor 2: Sell 100 lots (1000000) against remaining:
    //   o1: 500000 remaining, o2: 1000000, o3: 1500000. Total = 3000000.
    //   floor(500000*1000000/3000000) = 166666
    //   floor(1000000*1000000/3000000) = 333333
    //   floor(1500000*1000000/3000000) = 500000
    //   Base sum = 999999, remainder = 1. Goes to o1 (FIFO).
    //   Final: 166667, 333333, 500000 = 1000000
    run_new_order(engine, ctx, entries,
                  5000, 5, 2, "SELL", 1000000, 1000000);

    {
        Quantity total_fill = 0;
        std::vector<Quantity> fill_qtys;
        for (const auto& exp : entries[4].expectations) {
            if (exp.event_type == "ORDER_PARTIALLY_FILLED" ||
                exp.event_type == "ORDER_FILLED") {
                Quantity q = exp.get_int("qty");
                fill_qtys.push_back(q);
                total_fill += q;
            }
        }
        // Total must equal aggressor quantity exactly
        assert(total_fill == 1000000);
        // No quantity created or destroyed
        assert(fill_qtys.size() == 3);
        assert(fill_qtys[0] == 166667);  // 166666 + 1 remainder
        assert(fill_qtys[1] == 333333);
        assert(fill_qtys[2] == 500000);
    }

    write_journal(
        out_dir + "/test-journals/economics_pro_rata_allocation.journal",
        "# economics_pro_rata_allocation.journal\n"
        "# Verify pro-rata allocation math is exact.\n"
        "# 3 resting orders: 100, 200, 300 lots at same price (total 600).\n"
        "# Aggressor 1: 300 lots -> 50, 100, 150 (exact, no remainder).\n"
        "# Aggressor 2: 100 lots -> 166667, 333333, 500000 = 1000000.\n"
        "# Remainder (1) goes to first order in FIFO order.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 5: economics_pro_rata_rounding
//
// Verify pro-rata rounding edge cases:
//   3 equal orders of 10 lots, aggressor 10 lots.
//   floor(10*10/30) = 3 each = 9, remainder 1 to first order FIFO.
//   Final: 4, 3, 3 = 10.
// ---------------------------------------------------------------------------
void generate_pro_rata_rounding(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_pro_rata_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconProRataExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // 3 equal resting buy orders: 10 lots each at 100.0000
    // qty = 100000 each, total = 300000
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1000000, 100000);
    run_new_order(engine, ctx, entries,
                  2000, 2, 1, "BUY", 1000000, 100000);
    run_new_order(engine, ctx, entries,
                  3000, 3, 1, "BUY", 1000000, 100000);

    // Aggressor sell 10 lots (100000)
    // floor(100000*100000/300000) = floor(33333.33) = 33333 each
    // Base total = 99999, remainder = 1
    // Remainder goes to o1 (FIFO)
    // Final: 33334, 33333, 33333 = 100000
    run_new_order(engine, ctx, entries,
                  4000, 4, 2, "SELL", 1000000, 100000);

    {
        Quantity total_fill = 0;
        std::vector<Quantity> fill_qtys;
        for (const auto& exp : entries[3].expectations) {
            if (exp.event_type == "ORDER_PARTIALLY_FILLED" ||
                exp.event_type == "ORDER_FILLED") {
                Quantity q = exp.get_int("qty");
                fill_qtys.push_back(q);
                total_fill += q;
            }
        }
        // No quantity created or destroyed
        assert(total_fill == 100000);
        assert(fill_qtys.size() == 3);
        // First order gets the remainder
        assert(fill_qtys[0] == 33334);
        assert(fill_qtys[1] == 33333);
        assert(fill_qtys[2] == 33333);
    }

    write_journal(
        out_dir + "/test-journals/economics_pro_rata_rounding.journal",
        "# economics_pro_rata_rounding.journal\n"
        "# Verify pro-rata rounding edge cases.\n"
        "# 3 equal orders of 10 lots, aggressor 10 lots.\n"
        "# floor(100000*100000/300000) = 33333 each, sum = 99999.\n"
        "# Remainder 1 goes to first order (FIFO).\n"
        "# Final: 33334, 33333, 33333 = 100000. No quantity lost or created.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 6: economics_auction_price_volume
//
// Verify auction economics: equilibrium price maximises volume, all fills
// at the single auction price, total buy fills = total sell fills.
//
// Bids: 10@101 (100000), 20@100.50 (200000), 30@100 (300000)
// Asks: 15@99.50 (150000), 25@100 (250000), 10@100.50 (100000)
//
// Candidate analysis:
//   At 99.5000: buy_vol=600000, sell_vol=150000 -> matched=150000
//   At 100.0000: buy_vol=600000, sell_vol=400000 -> matched=400000
//   At 100.5000: buy_vol=300000, sell_vol=500000 -> matched=300000
//   At 101.0000: buy_vol=100000, sell_vol=500000 -> matched=100000
//
// Auction price = 100.0000 (max matched volume = 400000 = 40 lots).
// ---------------------------------------------------------------------------
void generate_auction_price_volume(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    // Bids
    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1010000, 100000);  // 10 lots @ 101
    run_new_order(engine, ctx, entries,
                  3000, 2, 1, "BUY", 1005000, 200000);  // 20 lots @ 100.50
    run_new_order(engine, ctx, entries,
                  4000, 3, 1, "BUY", 1000000, 300000);  // 30 lots @ 100

    // Asks
    run_new_order(engine, ctx, entries,
                  5000, 4, 2, "SELL",  995000, 150000);  // 15 lots @ 99.50
    run_new_order(engine, ctx, entries,
                  6000, 5, 2, "SELL", 1000000, 250000);  // 25 lots @ 100
    run_new_order(engine, ctx, entries,
                  7000, 6, 2, "SELL", 1005000, 100000);  // 10 lots @ 100.50

    // Publish indicative to verify equilibrium
    run_publish_indicative(engine, ctx, entries, 8000, 1000000);

    // Execute auction: price = 1000000, volume = 400000
    run_execute_auction(engine, ctx, entries, 9000, 1000000);

    // Verify: all fills at auction price 1000000, total buy = total sell
    {
        Quantity total_fill = 0;
        size_t auction_action = entries.size() - 1;
        for (const auto& exp : entries[auction_action].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                assert(exp.get_int("price") == 1000000);
                total_fill += exp.get_int("qty");
            }
            if (exp.event_type == "TRADE") {
                assert(exp.get_int("price") == 1000000);
            }
        }
        // Each fill is reported once (aggressor=buy by convention).
        // Total TRADE quantities should sum to matched volume.
        Quantity trade_total = 0;
        for (const auto& exp : entries[auction_action].expectations) {
            if (exp.event_type == "TRADE") {
                trade_total += exp.get_int("qty");
            }
        }
        assert(trade_total == 400000);
    }

    write_journal(
        out_dir + "/test-journals/economics_auction_price_volume.journal",
        "# economics_auction_price_volume.journal\n"
        "# Verify auction equilibrium price maximises volume.\n"
        "# Bids: 10@101, 20@100.50, 30@100.\n"
        "# Asks: 15@99.50, 25@100, 10@100.50.\n"
        "# Auction price = 100.0000, matched volume = 40 lots.\n"
        "# All fills and trades at the single auction price.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 7: economics_auction_surplus
//
// Verify auction surplus: after auction, the correct quantity remains.
//   Buy 100 lots @ 101.0000, Sell 50 lots @ 99.0000
//   Auction fills 50 lots (min of buy/sell volume).
//   Buy surplus: 50 lots remain in book.
//   Sell surplus: 0.
// ---------------------------------------------------------------------------
void generate_auction_surplus(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    run_set_session_state(engine, ctx, entries, 1000,
                          SessionState::PreOpen, "PRE_OPEN");

    run_new_order(engine, ctx, entries,
                  2000, 1, 1, "BUY", 1010000, 1000000);   // 100 lots bid
    run_new_order(engine, ctx, entries,
                  3000, 2, 2, "SELL", 990000, 500000);     // 50 lots ask

    // Publish indicative: should show matched=500000, buy_surplus=500000
    run_publish_indicative(engine, ctx, entries, 4000, 1000000);

    // Verify indicative
    {
        size_t ind_action = entries.size() - 1;
        for (const auto& exp : entries[ind_action].expectations) {
            if (exp.event_type == "INDICATIVE_PRICE") {
                assert(exp.get_int("matched_vol") == 500000);
                assert(exp.get_int("buy_surplus") == 500000);
                assert(exp.get_int("sell_surplus") == 0);
            }
        }
    }

    // Execute auction: 50 lots fill, 50 lots remain on buy side
    run_execute_auction(engine, ctx, entries, 5000, 1000000);

    // Verify fills sum to 500000
    {
        Quantity trade_total = 0;
        size_t auction_action = entries.size() - 1;
        for (const auto& exp : entries[auction_action].expectations) {
            if (exp.event_type == "TRADE") {
                trade_total += exp.get_int("qty");
            }
        }
        assert(trade_total == 500000);
    }

    // Transition to continuous and verify buy surplus is still in book
    run_set_session_state(engine, ctx, entries, 6000,
                          SessionState::Continuous, "CONTINUOUS");

    // Sell 50 lots to fill the remaining surplus
    run_new_order(engine, ctx, entries,
                  7000, 3, 2, "SELL", 1010000, 500000);

    // Verify: this sell should fill 50 lots against the remaining buy
    {
        Quantity fill_total = 0;
        size_t last_action = entries.size() - 1;
        for (const auto& exp : entries[last_action].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                fill_total += exp.get_int("qty");
            }
        }
        assert(fill_total == 500000);
    }

    write_journal(
        out_dir + "/test-journals/economics_auction_surplus.journal",
        "# economics_auction_surplus.journal\n"
        "# Verify auction surplus is correct.\n"
        "# Buy 100 lots @ 101.0000, Sell 50 lots @ 99.0000.\n"
        "# Auction fills 50 lots (min of buy/sell).\n"
        "# Buy surplus: 50 lots remain in book and are fillable in continuous.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 8: economics_iceberg_total_qty
//
// Verify iceberg economics: total filled across all tranches = total_qty.
//   Place iceberg buy: total=50 lots (500000), display=10 lots (100000)
//   Sell 50 lots -> fills all 5 tranches.
//   Sum of TRADE quantities = 500000.
// ---------------------------------------------------------------------------
void generate_iceberg_total_qty(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Place iceberg buy: total=500000 (50 lots), display=100000 (10 lots)
    run_new_order_iceberg(engine, ctx, entries,
                          1000, 1, 1, "BUY", 1000000, 500000, 100000);

    // Sell 500000 (50 lots) to fill all tranches
    run_new_order(engine, ctx, entries,
                  2000, 2, 2, "SELL", 1000000, 500000);

    // Verify: total TRADE quantities sum to 500000
    {
        Quantity trade_total = 0;
        size_t sell_action = entries.size() - 1;
        for (const auto& exp : entries[sell_action].expectations) {
            if (exp.event_type == "TRADE") {
                trade_total += exp.get_int("qty");
            }
        }
        assert(trade_total == 500000);
    }

    // Verify: total fill quantities also sum to 500000
    {
        Quantity fill_total = 0;
        size_t sell_action = entries.size() - 1;
        for (const auto& exp : entries[sell_action].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                fill_total += exp.get_int("qty");
            }
        }
        assert(fill_total == 500000);
    }

    write_journal(
        out_dir + "/test-journals/economics_iceberg_total_qty.journal",
        "# economics_iceberg_total_qty.journal\n"
        "# Verify iceberg economics: total filled across all tranches = total_qty.\n"
        "# Iceberg buy: total=50 lots, display=10 lots (5 tranches).\n"
        "# Sell 50 lots fills all 5 tranches.\n"
        "# Sum of all TRADE quantities = 500000 = 50 lots.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 9: economics_multi_level_sweep
//
// Verify sweep across multiple price levels: total fill = sum across levels.
//   Ask book: 10@100.00, 20@100.50, 30@101.00
//   Aggressive buy market 60 lots -> fills at each level's price:
//     10@100.0000 + 20@100.5000 + 30@101.0000 = 60 total
//   Each TRADE has the resting level's price (not a single price).
// ---------------------------------------------------------------------------
void generate_multi_level_sweep(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Build ask book at 3 price levels
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "SELL", 1000000, 100000);  // 10 lots @ 100.00
    run_new_order(engine, ctx, entries,
                  2000, 2, 1, "SELL", 1005000, 200000);  // 20 lots @ 100.50
    run_new_order(engine, ctx, entries,
                  3000, 3, 1, "SELL", 1010000, 300000);  // 30 lots @ 101.00

    // Aggressive buy market order: 60 lots
    run_new_order(engine, ctx, entries,
                  4000, 4, 2, "BUY", 0, 600000, "MARKET");

    // Verify: each TRADE has correct price (resting level's price)
    {
        std::vector<TradeInfo> trade_infos;
        size_t buy_action = entries.size() - 1;
        for (const auto& exp : entries[buy_action].expectations) {
            if (exp.event_type == "TRADE") {
                trade_infos.push_back({exp.get_int("price"),
                                       exp.get_int("qty"), 0, 0});
            }
        }
        assert(trade_infos.size() == 3);
        assert(trade_infos[0].price == 1000000);
        assert(trade_infos[0].quantity == 100000);
        assert(trade_infos[1].price == 1005000);
        assert(trade_infos[1].quantity == 200000);
        assert(trade_infos[2].price == 1010000);
        assert(trade_infos[2].quantity == 300000);

        // Total fill = 600000
        Quantity total = 0;
        for (const auto& t : trade_infos) total += t.quantity;
        assert(total == 600000);
    }

    // Verify: fill event quantities also sum to 600000
    {
        Quantity fill_total = 0;
        size_t buy_action = entries.size() - 1;
        for (const auto& exp : entries[buy_action].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                fill_total += exp.get_int("qty");
            }
        }
        assert(fill_total == 600000);
    }

    write_journal(
        out_dir + "/test-journals/economics_multi_level_sweep.journal",
        "# economics_multi_level_sweep.journal\n"
        "# Verify sweep across multiple price levels.\n"
        "# Ask book: 10@100.00, 20@100.50, 30@101.00.\n"
        "# Aggressive buy market 60 lots sweeps all 3 levels.\n"
        "# Each TRADE has the resting level's price.\n"
        "# Total: 100000 + 200000 + 300000 = 600000.\n",
        config, entries);
}

// ---------------------------------------------------------------------------
// Scenario 10: economics_modify_no_double_count
//
// Verify modify (cancel-replace) doesn't double-count or lose quantity:
//   Buy 10 lots @ 100.0000
//   Modify to 15 lots @ 100.0000
//   Sell 15 lots @ 100.0000 -> fills 15 (not 10, not 25)
// ---------------------------------------------------------------------------
void generate_modify_no_double_count(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;
    ParsedConfig config = make_fifo_config();
    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    EconFifoExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};
    std::vector<JournalEntry> entries;

    // Place buy 10 lots @ 100.0000
    run_new_order(engine, ctx, entries,
                  1000, 1, 1, "BUY", 1000000, 100000);   // order id = 1

    // Modify to 15 lots @ 100.0000 (cancel-replace)
    run_modify(engine, ctx, entries,
               2000, 1, 2, 1000000, 150000);

    // Sell 15 lots @ 100.0000 -> should fill exactly 15 lots
    run_new_order(engine, ctx, entries,
                  3000, 3, 2, "SELL", 1000000, 150000);

    // Verify: fill quantity = 150000 (exactly 15 lots)
    {
        Quantity fill_total = 0;
        size_t sell_action = entries.size() - 1;
        for (const auto& exp : entries[sell_action].expectations) {
            if (exp.event_type == "ORDER_FILLED" ||
                exp.event_type == "ORDER_PARTIALLY_FILLED") {
                fill_total += exp.get_int("qty");
            }
        }
        assert(fill_total == 150000);
    }

    // Verify the TRADE quantity is also 150000
    {
        Quantity trade_total = 0;
        size_t sell_action = entries.size() - 1;
        for (const auto& exp : entries[sell_action].expectations) {
            if (exp.event_type == "TRADE") {
                trade_total += exp.get_int("qty");
            }
        }
        assert(trade_total == 150000);
    }

    write_journal(
        out_dir + "/test-journals/economics_modify_no_double_count.journal",
        "# economics_modify_no_double_count.journal\n"
        "# Verify modify (cancel-replace) doesn't double-count or lose quantity.\n"
        "# Buy 10 lots -> modify to 15 lots -> sell 15 lots.\n"
        "# Fill quantity = 150000 (exactly 15 lots, not 10 or 25).\n",
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

    std::cout << "Writing economics journals to: "
              << out_dir << "/test-journals/\n";

    try {
        generate_fill_price_accuracy(out_dir);
        generate_quantity_conservation(out_dir);
        generate_partial_fill_remaining(out_dir);
        generate_pro_rata_allocation(out_dir);
        generate_pro_rata_rounding(out_dir);
        generate_auction_price_volume(out_dir);
        generate_auction_surplus(out_dir);
        generate_iceberg_total_qty(out_dir);
        generate_multi_level_sweep(out_dir);
        generate_modify_no_double_count(out_dir);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Done. All economics assertions passed.\n";
    return 0;
}
