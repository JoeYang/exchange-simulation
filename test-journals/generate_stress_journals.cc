// generate_stress_journals.cc
//
// Generates large stress-test .journal files by running scenarios through
// the actual matching engine, capturing events via recording listeners, and
// serialising everything with JournalWriter.  This ensures every EXPECT line
// is exactly what the engine produces — no hand-written expectations.
//
// Usage (via Bazel):
//   bazel run //test-journals:generate_stress_journals
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
// StressExchange
//
// Larger pool sizes than the test-runner's FifoExchange so the generator can
// hold many resting orders during generation.  The *generated* journal files
// are subsequently replayed by the test runner's FifoExchange (MaxOrders=1000,
// MaxPriceLevels=100).  Every scenario is designed to stay within those limits
// so the replay succeeds.
// ---------------------------------------------------------------------------
class StressExchange
    : public MatchingEngine<StressExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/2000,
                            /*MaxPriceLevels=*/500,
                            /*MaxOrderIds=*/100000> {
public:
    using Base = MatchingEngine<StressExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                2000, 500, 100000>;
    using Base::Base;
};

// ---------------------------------------------------------------------------
// Helper: harvest events and build a JournalEntry for one action
// ---------------------------------------------------------------------------

struct CaptureContext {
    RecordingOrderListener& order_listener;
    RecordingMdListener&    md_listener;
};

// Build a JournalEntry from a ParsedAction plus whatever the engine fired
// into the two listeners.  The entry's expectations mirror the combined
// (order-then-md) ordering that JournalTestRunner uses when replaying.
JournalEntry build_entry(ParsedAction action,
                         CaptureContext& ctx) {
    JournalEntry entry;
    entry.action = std::move(action);

    // Merge order events then md events (same order as test runner's run_impl)
    std::vector<RecordedEvent> all_events;
    all_events.reserve(ctx.order_listener.size() + ctx.md_listener.size());
    for (const RecordedEvent& ev : ctx.order_listener.events()) {
        all_events.push_back(ev);
    }
    for (const RecordedEvent& ev : ctx.md_listener.events()) {
        all_events.push_back(ev);
    }

    // Convert each RecordedEvent to a ParsedExpectation via the round-trip
    // through JournalWriter::event_to_expect_line / JournalParser.
    for (const RecordedEvent& ev : all_events) {
        std::string line = JournalWriter::event_to_expect_line(ev);
        // Parse the EXPECT line to extract event_type and fields.
        // The line format is: EXPECT <TYPE> key=value ...
        // Re-use the parser by building a minimal journal string.
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

// Convenience: build a ParsedAction for NEW_ORDER
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

// Convenience: build a ParsedAction for CANCEL
ParsedAction make_cancel(Timestamp ts, OrderId ord_id) {
    ParsedAction a;
    a.type = ParsedAction::Cancel;
    a.fields["ts"]     = std::to_string(ts);
    a.fields["ord_id"] = std::to_string(ord_id);
    return a;
}

// Convenience: build a ParsedAction for MODIFY
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

// ---------------------------------------------------------------------------
// Scenario 1: stress_500_resting_orders
//
// 500 limit buy orders spread across 50 price levels (10 orders per level),
// then 1 large market sell order that sweeps them all.
//
// Designed to stay within FifoExchange constraints:
//   - MaxOrders=1000 : peak usage = 500 resting + 1 aggressor = 501
//   - MaxPriceLevels=100: peak usage = 50 bid levels
//   - MaxOrderIds=10000: 501 IDs assigned
// ---------------------------------------------------------------------------
void generate_500_resting_orders(const std::string& out_dir) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    ParsedConfig config;
    config.match_algo   = "FIFO";
    config.tick_size    = 100;
    config.lot_size     = 10000;
    config.max_orders   = 1000;
    config.max_levels   = 100;
    config.max_order_ids = 10000;

    EngineConfig ecfg{config.tick_size, config.lot_size, 0, 0};
    StressExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;

    // 500 buy limits: 50 price levels × 10 orders each.
    // Price levels: 1000000, 1000100, 1000200, ..., 1004900 (in ticks of 100).
    const int kNumLevels      = 50;
    const int kOrdersPerLevel = 10;
    const Quantity kOrderQty  = 10000;   // 1.0000 lots

    Timestamp ts      = 1000;
    uint64_t  cl_id   = 1;

    for (int lvl = 0; lvl < kNumLevels; ++lvl) {
        Price price = 1000000 + static_cast<Price>(lvl) * 100;
        for (int n = 0; n < kOrdersPerLevel; ++n) {
            OrderRequest req{};
            req.client_order_id = cl_id;
            req.account_id      = 1;
            req.side            = Side::Buy;
            req.type            = OrderType::Limit;
            req.tif             = TimeInForce::GTC;
            req.price           = price;
            req.quantity        = kOrderQty;
            req.timestamp       = ts;

            order_listener.clear();
            md_listener.clear();
            engine.new_order(req);

            entries.push_back(build_entry(
                make_new_order(ts, cl_id, 1, "BUY", price, kOrderQty),
                ctx));

            ++cl_id;
            ts += 1000;
        }
    }

    // 1 large market sell to sweep all 500 buy orders.
    // Total quantity = 500 orders × 10000 = 5000000.
    const Quantity kSweepQty = static_cast<Quantity>(kNumLevels) *
                               kOrdersPerLevel * kOrderQty;

    {
        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Market;
        req.tif             = TimeInForce::GTC;
        req.price           = 0;
        req.quantity        = kSweepQty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.new_order(req);

        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "SELL", 0, kSweepQty, "MARKET"),
            ctx));
    }

    std::string path = out_dir + "/test-journals/stress_500_resting_orders.journal";
    // Write header comment manually by using a custom write.
    {
        std::string body = JournalWriter::to_string(config, entries);
        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + path);
        }
        f << "# stress_500_resting_orders.journal\n"
          << "# 500 limit buy orders across 50 price levels (10 per level),\n"
          << "# then 1 large market sell that sweeps them all.\n"
          << "# Tests orderbook depth and massive fill cascade.\n"
          << body;
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// ---------------------------------------------------------------------------
// Scenario 2: stress_buy_sell_alternating
//
// 200 sell limit orders resting at increasing ask prices, then 200 buy limit
// orders at prices that match (cross the spread), producing continuous fills.
// Net simultaneous resting orders stays low because each buy fills a sell.
//
// Sell side: prices 1010000, 1010100, ..., 1029900 (200 levels × 1 order)
// Buy  side: prices match each sell level exactly → immediate fill.
//
// Constraints:
//   - MaxOrders: peak = 200 resting sells before any buys → fine
//   - MaxPriceLevels: 200 sell levels (1 per level) → exceeds MaxPriceLevels=100
//
// Revised: 100 sell orders at 100 distinct levels, then 100 crossing buys.
// ---------------------------------------------------------------------------
void generate_buy_sell_alternating(const std::string& out_dir) {
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
    StressExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;

    // Phase 1: place 100 resting sell limit orders at 100 distinct price levels.
    // Prices: 1010000, 1010100, ..., 1019900.
    const int kNumSells  = 100;
    const Quantity kQty  = 10000;

    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    for (int i = 0; i < kNumSells; ++i) {
        Price price = 1010000 + static_cast<Price>(i) * 100;

        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = kQty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.new_order(req);

        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "SELL", price, kQty),
            ctx));

        ++cl_id;
        ts += 1000;
    }

    // Phase 2: place 100 buy limit orders that cross each sell.
    // Buy at the *best ask* each time (which is the lowest resting sell).
    // After each fill both orders are gone, keeping the book shallow.
    for (int i = 0; i < kNumSells; ++i) {
        // Best ask is currently at 1010000 + i*100 (lowest remaining sell).
        Price price = 1010000 + static_cast<Price>(i) * 100;

        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = kQty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.new_order(req);

        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", price, kQty),
            ctx));

        ++cl_id;
        ts += 1000;
    }

    std::string path = out_dir + "/test-journals/stress_buy_sell_alternating.journal";
    {
        std::string body = JournalWriter::to_string(config, entries);
        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + path);
        }
        f << "# stress_buy_sell_alternating.journal\n"
          << "# 100 sell limit orders at 100 distinct price levels, then\n"
          << "# 100 buy limit orders that each cross the best ask, producing\n"
          << "# 100 immediate fills. Tests sustained fill throughput.\n"
          << body;
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// ---------------------------------------------------------------------------
// Scenario 3: stress_cancel_storm
//
// 500 resting limit buy orders, then cancel all 500.
// Tests bulk cancellation: ORDER_CANCELLED + L3 CANCEL + L2 REMOVE/UPDATE
// + TOP_OF_BOOK fire per cancel.
//
// Spread across 50 price levels (10 per level) so L2 events vary
// (first 9 cancels at a level → UPDATE, last one → REMOVE).
//
// Constraints:
//   - MaxOrders=1000: peak = 500
//   - MaxPriceLevels=100: peak = 50
//   - MaxOrderIds=10000: 1000 IDs (500 place + 500 cancel = 500 IDs total)
// ---------------------------------------------------------------------------
void generate_cancel_storm(const std::string& out_dir) {
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
    StressExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;

    const int kNumLevels      = 50;
    const int kOrdersPerLevel = 10;
    const Quantity kQty       = 10000;

    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Track the engine-assigned order IDs (sequential from 1).
    // next_order_id_ starts at 1 and increments per accepted order.
    std::vector<OrderId> placed_ids;
    placed_ids.reserve(kNumLevels * kOrdersPerLevel);

    // Phase 1: place 500 resting buy orders.
    OrderId next_oid = 1;
    for (int lvl = 0; lvl < kNumLevels; ++lvl) {
        Price price = 1000000 + static_cast<Price>(lvl) * 100;
        for (int n = 0; n < kOrdersPerLevel; ++n) {
            OrderRequest req{};
            req.client_order_id = cl_id;
            req.account_id      = 1;
            req.side            = Side::Buy;
            req.type            = OrderType::Limit;
            req.tif             = TimeInForce::GTC;
            req.price           = price;
            req.quantity        = kQty;
            req.timestamp       = ts;

            order_listener.clear();
            md_listener.clear();
            engine.new_order(req);

            entries.push_back(build_entry(
                make_new_order(ts, cl_id, 1, "BUY", price, kQty),
                ctx));

            placed_ids.push_back(next_oid++);
            ++cl_id;
            ts += 1000;
        }
    }

    // Phase 2: cancel all 500, FIFO order (order IDs 1..500).
    for (OrderId oid : placed_ids) {
        order_listener.clear();
        md_listener.clear();
        engine.cancel_order(oid, ts);

        entries.push_back(build_entry(make_cancel(ts, oid), ctx));
        ts += 1000;
    }

    std::string path = out_dir + "/test-journals/stress_cancel_storm.journal";
    {
        std::string body = JournalWriter::to_string(config, entries);
        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + path);
        }
        f << "# stress_cancel_storm.journal\n"
          << "# 500 resting limit buy orders across 50 price levels, then\n"
          << "# cancel all 500 in sequence. Tests bulk cancellation.\n"
          << body;
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// ---------------------------------------------------------------------------
// Scenario 4: stress_modify_storm
//
// 200 resting limit buy orders, then modify each one (price change via
// cancel-replace).  Each modify changes price by +100 ticks, moving the
// order to a new level or updating an existing one.
//
// To avoid exceeding MaxPriceLevels=100 during replay:
//   Original levels: 1000000..1001900 in steps of 100 → 20 levels, 10 orders each
//   Modified levels: 1002000..1003900 in steps of 100 → 20 new levels
//   Peak simultaneous levels = 20 original + 20 new = 40 (well within 100).
//
// Constraints:
//   - MaxOrders=1000: peak = 200
//   - MaxPriceLevels=100: peak = 40
//   - MaxOrderIds=10000: 200 IDs (modify reuses the same ID)
// ---------------------------------------------------------------------------
void generate_modify_storm(const std::string& out_dir) {
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
    StressExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;

    const int kNumLevels      = 20;
    const int kOrdersPerLevel = 10;
    const Quantity kQty       = 10000;
    const Price    kPriceDelta = 2000;   // shift each order up by 20 ticks * 100

    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    struct PlacedOrder {
        OrderId  oid;
        Price    price;
        Quantity qty;
    };
    std::vector<PlacedOrder> placed;
    placed.reserve(kNumLevels * kOrdersPerLevel);

    OrderId next_oid = 1;

    // Phase 1: place 200 resting buy orders.
    for (int lvl = 0; lvl < kNumLevels; ++lvl) {
        Price price = 1000000 + static_cast<Price>(lvl) * 100;
        for (int n = 0; n < kOrdersPerLevel; ++n) {
            OrderRequest req{};
            req.client_order_id = cl_id;
            req.account_id      = 1;
            req.side            = Side::Buy;
            req.type            = OrderType::Limit;
            req.tif             = TimeInForce::GTC;
            req.price           = price;
            req.quantity        = kQty;
            req.timestamp       = ts;

            order_listener.clear();
            md_listener.clear();
            engine.new_order(req);

            entries.push_back(build_entry(
                make_new_order(ts, cl_id, 1, "BUY", price, kQty),
                ctx));

            placed.push_back(PlacedOrder{next_oid++, price, kQty});
            ++cl_id;
            ts += 1000;
        }
    }

    // Phase 2: modify each order — move price up by kPriceDelta.
    for (const PlacedOrder& po : placed) {
        Price new_price = po.price + kPriceDelta;

        ModifyRequest req{};
        req.order_id        = po.oid;
        req.client_order_id = cl_id;
        req.new_price       = new_price;
        req.new_quantity    = po.qty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.modify_order(req);

        entries.push_back(build_entry(
            make_modify(ts, po.oid, cl_id, new_price, po.qty),
            ctx));

        ++cl_id;
        ts += 1000;
    }

    std::string path = out_dir + "/test-journals/stress_modify_storm.journal";
    {
        std::string body = JournalWriter::to_string(config, entries);
        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + path);
        }
        f << "# stress_modify_storm.journal\n"
          << "# 200 resting limit buy orders across 20 price levels, then\n"
          << "# modify (cancel-replace) each one to a new price. Tests bulk\n"
          << "# cancel-replace behaviour.\n"
          << body;
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

// ---------------------------------------------------------------------------
// Scenario 5: stress_deep_book
//
// Build maximum book depth within FifoExchange limits:
//   50 bid levels × 1 order each + 50 ask levels × 1 order each = 100 levels.
//
// Bid levels:  1000000..1004900 (step 100), 1 buy per level
// Ask levels:  1005000..1009900 (step 100), 1 sell per level
//
// No crossing → all orders rest.  Final book has 50 bid + 50 ask levels.
//
// Constraints:
//   - MaxOrders=1000: 100 orders peak
//   - MaxPriceLevels=100: 100 levels peak (exactly at limit)
//   - MaxOrderIds=10000: 100 IDs
// ---------------------------------------------------------------------------
void generate_deep_book(const std::string& out_dir) {
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
    StressExchange engine(ecfg, order_listener, md_listener);
    CaptureContext ctx{order_listener, md_listener};

    std::vector<JournalEntry> entries;

    const int kLevelsPerSide = 50;
    const Quantity kQty      = 10000;

    Timestamp ts    = 1000;
    uint64_t  cl_id = 1;

    // Place 50 buy orders at 50 distinct bid levels (descending prices
    // so the best bid is highest: 1004900 down to 1000000).
    for (int i = kLevelsPerSide - 1; i >= 0; --i) {
        Price price = 1000000 + static_cast<Price>(i) * 100;

        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Buy;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = kQty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.new_order(req);

        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "BUY", price, kQty),
            ctx));

        ++cl_id;
        ts += 1000;
    }

    // Place 50 sell orders at 50 distinct ask levels (ascending prices
    // so the best ask is lowest: 1005000 up to 1009900).
    for (int i = 0; i < kLevelsPerSide; ++i) {
        Price price = 1005000 + static_cast<Price>(i) * 100;

        OrderRequest req{};
        req.client_order_id = cl_id;
        req.account_id      = 1;
        req.side            = Side::Sell;
        req.type            = OrderType::Limit;
        req.tif             = TimeInForce::GTC;
        req.price           = price;
        req.quantity        = kQty;
        req.timestamp       = ts;

        order_listener.clear();
        md_listener.clear();
        engine.new_order(req);

        entries.push_back(build_entry(
            make_new_order(ts, cl_id, 1, "SELL", price, kQty),
            ctx));

        ++cl_id;
        ts += 1000;
    }

    std::string path = out_dir + "/test-journals/stress_deep_book.journal";
    {
        std::string body = JournalWriter::to_string(config, entries);
        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + path);
        }
        f << "# stress_deep_book.journal\n"
          << "# 50 buy orders across 50 bid levels and 50 sell orders across\n"
          << "# 50 ask levels (no crossing). Creates a 100-level-deep book at\n"
          << "# the FifoExchange limit. Tests maximum book depth.\n"
          << body;
    }
    std::cout << "Wrote " << path << " (" << entries.size() << " actions)\n";
}

}  // namespace exchange

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    using namespace exchange;

    // Determine the output root directory.
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

    std::cout << "Writing stress journals to: " << out_dir << "/test-journals/\n";

    try {
        generate_500_resting_orders(out_dir);
        generate_buy_sell_alternating(out_dir);
        generate_cancel_storm(out_dir);
        generate_modify_storm(out_dir);
        generate_deep_book(out_dir);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "Done.\n";
    return 0;
}
