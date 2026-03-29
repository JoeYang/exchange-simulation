// generate_ice_journals.cc
//
// Generates ICE E2E .journal files by running scenarios through the full
// ICE pipeline and capturing correct EXPECT lines from actual output.
//
// Usage:
//   bazel run //test-journals:generate_ice_journals
//
// Writes files to $BUILD_WORKSPACE_DIRECTORY/test-journals/ice/.

#include "exchange-core/composite_listener.h"
#include "ice/fix/fix_encoder.h"
#include "ice/fix/fix_parser.h"
#include "ice/fix/ice_fix_exec_publisher.h"
#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_publisher.h"
#include "ice/ice_simulator.h"
#include "test-harness/recording_listener.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace exchange;
using namespace exchange::ice;

// ---------------------------------------------------------------------------
// ICE E2E pipeline context
// ---------------------------------------------------------------------------

struct IcePipeline {
    fix::IceFixExecPublisher exec_pub{"ICE", "CLIENT", ""};
    RecordingOrderListener recording_ol;
    CompositeOrderListener<fix::IceFixExecPublisher, RecordingOrderListener>
        composite_ol{&exec_pub, &recording_ol};

    impact::ImpactFeedPublisher md_pub;
    RecordingMdListener recording_ml;
    CompositeMdListener<impact::ImpactFeedPublisher, RecordingMdListener>
        composite_ml{&md_pub, &recording_ml};

    using SimType = IceSimulator<
        CompositeOrderListener<fix::IceFixExecPublisher, RecordingOrderListener>,
        CompositeMdListener<impact::ImpactFeedPublisher, RecordingMdListener>>;
    SimType sim{composite_ol, composite_ml};

    std::unordered_map<std::string, uint32_t> symbol_map;
    std::unordered_map<uint64_t, OrderId> cl_to_exch;

    IcePipeline() {
        auto products = get_ice_products();
        sim.load_products(products);
        for (const auto& p : products) symbol_map[p.symbol] = p.instrument_id;
    }

    uint32_t resolve(const std::string& sym) const {
        auto it = symbol_map.find(sym);
        return (it != symbol_map.end()) ? it->second : 0;
    }

    void start_continuous(Timestamp ts) {
        sim.start_trading_day(ts);
        sim.open_market(ts);
        clear_output();
    }

    void clear_output() {
        exec_pub.clear_messages();
        md_pub.clear();
        recording_ol.clear();
        recording_ml.clear();
    }

    void switch_instrument(const std::string& sym) {
        auto iid = resolve(sym);
        md_pub = impact::ImpactFeedPublisher(static_cast<int32_t>(iid));
        composite_ml = CompositeMdListener<
            impact::ImpactFeedPublisher, RecordingMdListener>(
            &md_pub, &recording_ml);
    }

    void new_order(const std::string& instrument, Timestamp ts,
                   uint64_t cl_ord_id, uint64_t account_id,
                   Side side, Price price, Quantity qty,
                   OrderType type = OrderType::Limit,
                   TimeInForce tif = TimeInForce::GTC) {
        clear_output();
        switch_instrument(instrument);

        auto iid = resolve(instrument);
        exec_pub.register_order(cl_ord_id, price, qty, side);

        OrderRequest req{};
        req.client_order_id = cl_ord_id;
        req.account_id = account_id;
        req.side = side;
        req.type = type;
        req.tif = tif;
        req.price = price;
        req.quantity = qty;
        req.timestamp = ts;

        sim.new_order(iid, req);

        for (const auto& evt : recording_ol.events()) {
            auto* accepted = std::get_if<OrderAccepted>(&evt);
            if (accepted) {
                cl_to_exch[accepted->client_order_id] = accepted->id;
            }
        }
        recording_ol.clear();
        recording_ml.clear();
    }

    void cancel_order(const std::string& instrument, Timestamp ts,
                      uint64_t orig_cl_ord_id) {
        clear_output();
        switch_instrument(instrument);

        auto iid = resolve(instrument);
        OrderId eid = 0;
        auto it = cl_to_exch.find(orig_cl_ord_id);
        if (it != cl_to_exch.end()) eid = it->second;

        sim.cancel_order(iid, eid, ts);
        recording_ol.clear();
        recording_ml.clear();
    }

    void replace_order(const std::string& instrument, Timestamp ts,
                       uint64_t cl_ord_id, uint64_t orig_cl_ord_id,
                       Price new_price, Quantity new_qty) {
        clear_output();
        switch_instrument(instrument);

        auto iid = resolve(instrument);
        ModifyRequest req{};
        auto it = cl_to_exch.find(orig_cl_ord_id);
        req.order_id = (it != cl_to_exch.end()) ? it->second : 0;
        req.client_order_id = cl_ord_id;
        req.new_price = new_price;
        req.new_quantity = new_qty;
        req.timestamp = ts;

        sim.modify_order(iid, req);

        for (const auto& evt : recording_ol.events()) {
            auto* accepted = std::get_if<OrderAccepted>(&evt);
            if (accepted) cl_to_exch[cl_ord_id] = accepted->id;
        }
        recording_ol.clear();
        recording_ml.clear();
    }

    // Decode FIX exec reports and produce ICE_EXEC_* EXPECT lines.
    std::string decode_exec_expects() {
        std::ostringstream out;
        for (const auto& msg : exec_pub.messages()) {
            auto parsed = ::ice::fix::parse_fix_message(msg.data(), msg.size());
            if (!parsed.has_value()) continue;
            const auto& m = parsed.value();

            std::string exec_type = m.get_string(150);
            if (exec_type == "0") {
                out << "EXPECT ICE_EXEC_NEW"
                    << " cl_ord_id=" << m.get_string(11) << "\n";
            } else if (exec_type == "F" || exec_type == "1") {
                std::string status = (exec_type == "F") ? "FILLED" : "PARTIAL";
                out << "EXPECT ICE_EXEC_FILL"
                    << " cl_ord_id=" << m.get_string(11)
                    << " fill_price=" << m.get_string(31)
                    << " fill_qty=" << m.get_string(32)
                    << " status=" << status << "\n";
            } else if (exec_type == "4") {
                out << "EXPECT ICE_EXEC_CANCELLED"
                    << " cl_ord_id=" << m.get_string(11) << "\n";
            } else if (exec_type == "8") {
                out << "EXPECT ICE_EXEC_REJECTED"
                    << " cl_ord_id=" << m.get_string(11) << "\n";
            } else if (exec_type == "5") {
                out << "EXPECT ICE_EXEC_REPLACED"
                    << " cl_ord_id=" << m.get_string(11) << "\n";
            }
        }
        return out.str();
    }

    // Decode iMpact packets and produce ICE_MD_* EXPECT lines.
    std::string decode_md_expects() {
        std::ostringstream out;
        struct MdFormatter {
            std::ostringstream& out;
            void on_bundle_start(const impact::BundleStart&) {}
            void on_bundle_end(const impact::BundleEnd&) {}
            void on_snapshot_order(const impact::SnapshotOrder&) {}
            void on_price_level(const impact::PriceLevel&) {}
            void on_instrument_def(const impact::InstrumentDefinition&) {}

            void on_add_modify_order(const impact::AddModifyOrder& m) {
                std::string side = (m.side == 1) ? "BUY" : "SELL";
                out << "EXPECT ICE_MD_ADD"
                    << " side=" << side
                    << " price=" << m.price
                    << " qty=" << m.quantity << "\n";
            }
            void on_order_withdrawal(const impact::OrderWithdrawal& m) {
                std::string side = (m.side == 1) ? "BUY" : "SELL";
                out << "EXPECT ICE_MD_REMOVE"
                    << " side=" << side
                    << " price=" << m.price << "\n";
            }
            void on_deal_trade(const impact::DealTrade& t) {
                std::string agg = (t.aggressor_side == 1) ? "BUY" : "SELL";
                out << "EXPECT ICE_MD_TRADE"
                    << " price=" << t.price
                    << " qty=" << t.quantity
                    << " aggressor=" << agg << "\n";
            }
            void on_market_status(const impact::MarketStatus&) {
                out << "EXPECT ICE_MD_STATUS\n";
            }
        };
        MdFormatter fmt{out};
        for (const auto& pkt : md_pub.packets()) {
            impact::decode_messages(pkt.bytes(), pkt.len, fmt);
        }
        return out.str();
    }

    std::string all_expects() {
        return decode_exec_expects() + decode_md_expects();
    }
};

// ---------------------------------------------------------------------------
// Journal writing helpers
// ---------------------------------------------------------------------------

static std::string side_str(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
static std::string type_str(OrderType t) {
    switch (t) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::Stop: return "STOP";
        case OrderType::StopLimit: return "STOP_LIMIT";
    }
    return "LIMIT";
}
static std::string tif_str(TimeInForce t) {
    switch (t) {
        case TimeInForce::DAY: return "DAY";
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTD: return "GTD";
    }
    return "GTC";
}

struct IceJournalBuilder {
    std::ostringstream out;

    void comment(const std::string& text) { out << "# " << text << "\n"; }
    void blank() { out << "\n"; }
    void session_start(Timestamp ts, const std::string& state) {
        out << "ACTION SESSION_START ts=" << ts << " state=" << state << "\n";
    }
    void new_order(const std::string& instr, Timestamp ts, uint64_t cl_ord_id,
                   uint64_t account, Side side, Price price, Quantity qty,
                   OrderType type = OrderType::Limit,
                   TimeInForce tif = TimeInForce::GTC) {
        out << "ACTION ICE_FIX_NEW_ORDER ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " account=" << account
            << " side=" << side_str(side)
            << " price=" << price
            << " qty=" << qty
            << " type=" << type_str(type)
            << " tif=" << tif_str(tif) << "\n";
    }
    void cancel(const std::string& instr, Timestamp ts,
                uint64_t cl_ord_id, uint64_t orig_cl_ord_id) {
        out << "ACTION ICE_FIX_CANCEL ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " orig_cl_ord_id=" << orig_cl_ord_id << "\n";
    }
    void replace(const std::string& instr, Timestamp ts,
                 uint64_t cl_ord_id, uint64_t orig_cl_ord_id,
                 Price price, Quantity qty) {
        out << "ACTION ICE_FIX_REPLACE ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " orig_cl_ord_id=" << orig_cl_ord_id
            << " price=" << price
            << " qty=" << qty << "\n";
    }
    void expects(const std::string& e) { out << e; }
    std::string str() const { return out.str(); }
};

void write_journal(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "ERROR: cannot write " << path << "\n";
        return;
    }
    f << content;
    std::cout << "  wrote " << path << "\n";
}

// ---------------------------------------------------------------------------
// ICE E2E scenarios
// ---------------------------------------------------------------------------

void gen_ice_brent_basic(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Brent basic limit order lifecycle");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("B", 1000, 1, 1, Side::Buy, 800000, 10000);
    p.new_order("B", 1000, 1, 1, Side::Buy, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("B", 2000, 2, 2, Side::Sell, 800000, 10000);
    p.new_order("B", 2000, 2, 2, Side::Sell, 800000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_brent_basic.journal", j.str());
}

void gen_ice_cancel(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Place and cancel order on Brent");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("B", 1000, 1, 1, Side::Buy, 800000, 10000);
    p.new_order("B", 1000, 1, 1, Side::Buy, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.cancel("B", 2000, 2, 1);
    p.cancel_order("B", 2000, 1);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_cancel.journal", j.str());
}

void gen_ice_replace(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Place and modify (cancel-replace) order on Cocoa");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Cocoa tick = 10000, lot = 10000
    j.new_order("C", 1000, 1, 1, Side::Buy, 50000000, 10000);
    p.new_order("C", 1000, 1, 1, Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.replace("C", 2000, 2, 1, 51000000, 10000);
    p.replace_order("C", 2000, 2, 1, 51000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_replace.journal", j.str());
}

void gen_ice_smp(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Self-match prevention — same account crosses");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("B", 1000, 1, 42, Side::Sell, 800000, 10000);
    p.new_order("B", 1000, 1, 42, Side::Sell, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    // Same account — SMP should prevent fill.
    j.new_order("B", 2000, 2, 42, Side::Buy, 800000, 10000);
    p.new_order("B", 2000, 2, 42, Side::Buy, 800000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_smp.journal", j.str());
}

void gen_ice_multi_product(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Multi-product — Brent + Euribor + Cocoa isolation");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Brent sell
    j.new_order("B", 1000, 1, 1, Side::Sell, 800000, 10000);
    p.new_order("B", 1000, 1, 1, Side::Sell, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    // Euribor buy (GTBPR engine) — should NOT match Brent
    j.new_order("I", 2000, 2, 2, Side::Buy, 960000, 10000);
    p.new_order("I", 2000, 2, 2, Side::Buy, 960000, 10000);
    j.expects(p.all_expects()); j.blank();

    // Cocoa buy — should NOT match Brent
    j.new_order("C", 3000, 3, 3, Side::Buy, 50000000, 10000);
    p.new_order("C", 3000, 3, 3, Side::Buy, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_multi_product.journal", j.str());
}

void gen_ice_session_lifecycle(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Session lifecycle — preopen collects, then continuous matches");
    j.blank();
    j.session_start(0, "PRE_OPEN"); j.blank();

    // Place orders during preopen — they should be accepted but not match.
    p.sim.start_trading_day(0);
    p.clear_output();
    p.switch_instrument("B");

    OrderRequest buy_req{};
    buy_req.client_order_id = 1;
    buy_req.account_id = 1;
    buy_req.side = Side::Buy;
    buy_req.type = OrderType::Limit;
    buy_req.tif = TimeInForce::GTC;
    buy_req.price = 800000;
    buy_req.quantity = 10000;
    buy_req.timestamp = 1000;
    p.exec_pub.register_order(1, 800000, 10000, Side::Buy);
    p.sim.new_order(1, buy_req);
    for (const auto& evt : p.recording_ol.events()) {
        auto* acc = std::get_if<OrderAccepted>(&evt);
        if (acc) p.cl_to_exch[acc->client_order_id] = acc->id;
    }
    p.recording_ol.clear();
    p.recording_ml.clear();

    j.new_order("B", 1000, 1, 1, Side::Buy, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    p.clear_output();
    OrderRequest sell_req = buy_req;
    sell_req.client_order_id = 2;
    sell_req.account_id = 2;
    sell_req.side = Side::Sell;
    sell_req.timestamp = 2000;
    p.exec_pub.register_order(2, 800000, 10000, Side::Sell);
    p.sim.new_order(1, sell_req);
    for (const auto& evt : p.recording_ol.events()) {
        auto* acc = std::get_if<OrderAccepted>(&evt);
        if (acc) p.cl_to_exch[acc->client_order_id] = acc->id;
    }
    p.recording_ol.clear();
    p.recording_ml.clear();

    j.new_order("B", 2000, 2, 2, Side::Sell, 800000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_session_lifecycle.journal", j.str());
}

void gen_ice_iceberg(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Iceberg order — display qty partially visible");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Iceberg sell: total 30000, display 10000
    p.clear_output();
    p.switch_instrument("B");
    OrderRequest iceberg{};
    iceberg.client_order_id = 1;
    iceberg.account_id = 1;
    iceberg.side = Side::Sell;
    iceberg.type = OrderType::Limit;
    iceberg.tif = TimeInForce::GTC;
    iceberg.price = 800000;
    iceberg.quantity = 30000;
    iceberg.display_qty = 10000;
    iceberg.timestamp = 1000;
    p.exec_pub.register_order(1, 800000, 30000, Side::Sell);
    p.sim.new_order(1, iceberg);
    for (const auto& evt : p.recording_ol.events()) {
        auto* acc = std::get_if<OrderAccepted>(&evt);
        if (acc) p.cl_to_exch[acc->client_order_id] = acc->id;
    }
    p.recording_ol.clear(); p.recording_ml.clear();

    j.out << "ACTION ICE_FIX_NEW_ORDER ts=1000 instrument=B cl_ord_id=1"
          << " account=1 side=SELL price=800000 qty=30000 type=LIMIT"
          << " tif=GTC display_qty=10000\n";
    j.expects(p.all_expects()); j.blank();

    // Buy 10000 — fills first tranche
    j.new_order("B", 2000, 2, 2, Side::Buy, 800000, 10000);
    p.new_order("B", 2000, 2, 2, Side::Buy, 800000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_iceberg.journal", j.str());
}

void gen_ice_mass_cancel(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Mass cancel by account across multiple orders");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Place 3 orders from account 42
    j.new_order("B", 1000, 1, 42, Side::Buy, 790000, 10000);
    p.new_order("B", 1000, 1, 42, Side::Buy, 790000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("B", 2000, 2, 42, Side::Buy, 800000, 10000);
    p.new_order("B", 2000, 2, 42, Side::Buy, 800000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("B", 3000, 3, 42, Side::Buy, 810000, 10000);
    p.new_order("B", 3000, 3, 42, Side::Buy, 810000, 10000);
    j.expects(p.all_expects()); j.blank();

    // Mass cancel account 42
    p.clear_output();
    p.switch_instrument("B");
    auto* engine = p.sim.get_fifo_engine(1);
    if (engine) engine->mass_cancel(42, 4000);
    p.recording_ol.clear(); p.recording_ml.clear();

    j.out << "ACTION ICE_FIX_MASS_CANCEL ts=4000 instrument=B account=42\n";
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_mass_cancel.journal", j.str());
}

// ---------------------------------------------------------------------------
// GTBPR-specific scenarios (Euribor — STIR products)
// ---------------------------------------------------------------------------

void gen_ice_euribor_gtbpr(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: Euribor GTBPR — priority order + pro-rata allocation");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Euribor (I): tick=50, lot=10000
    // Place two sells: one large (qualifies for priority), one small
    j.new_order("I", 1000, 1, 1, Side::Sell, 960000, 100000);
    p.new_order("I", 1000, 1, 1, Side::Sell, 960000, 100000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("I", 2000, 2, 2, Side::Sell, 960000, 50000);
    p.new_order("I", 2000, 2, 2, Side::Sell, 960000, 50000);
    j.expects(p.all_expects()); j.blank();

    // Aggressor buy — triggers GTBPR matching
    j.new_order("I", 3000, 3, 3, Side::Buy, 960000, 100000);
    p.new_order("I", 3000, 3, 3, Side::Buy, 960000, 100000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_euribor_gtbpr.journal", j.str());
}

void gen_ice_gtbpr_no_priority(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: GTBPR no priority — all orders below collar, pure pro-rata");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // Both sells below collar (50000 default) → no priority, pure pro-rata
    j.new_order("I", 1000, 1, 1, Side::Sell, 960000, 30000);
    p.new_order("I", 1000, 1, 1, Side::Sell, 960000, 30000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("I", 2000, 2, 2, Side::Sell, 960000, 30000);
    p.new_order("I", 2000, 2, 2, Side::Sell, 960000, 30000);
    j.expects(p.all_expects()); j.blank();

    // Partial aggressor
    j.new_order("I", 3000, 3, 3, Side::Buy, 960000, 30000);
    p.new_order("I", 3000, 3, 3, Side::Buy, 960000, 30000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_gtbpr_no_priority.journal", j.str());
}

void gen_ice_gtbpr_single_order(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: GTBPR single resting order — full fill");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("I", 1000, 1, 1, Side::Sell, 960000, 50000);
    p.new_order("I", 1000, 1, 1, Side::Sell, 960000, 50000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("I", 2000, 2, 2, Side::Buy, 960000, 50000);
    p.new_order("I", 2000, 2, 2, Side::Buy, 960000, 50000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_gtbpr_single_order.journal", j.str());
}

void gen_ice_gtbpr_sonia(const std::string& dir) {
    IcePipeline p; IceJournalBuilder j;
    j.comment("ICE E2E: SONIA (GTBPR) — basic limit order on second STIR product");
    j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    // SONIA (SO): tick=100, lot=10000
    j.new_order("SO", 1000, 1, 1, Side::Sell, 950000, 50000);
    p.new_order("SO", 1000, 1, 1, Side::Sell, 950000, 50000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("SO", 2000, 2, 2, Side::Buy, 950000, 50000);
    p.new_order("SO", 2000, 2, 2, Side::Buy, 950000, 50000);
    j.expects(p.all_expects());

    write_journal(dir + "/ice_e2e_gtbpr_sonia.journal", j.str());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    const char* ws = std::getenv("BUILD_WORKSPACE_DIRECTORY");
    std::string dir = ws ? std::string(ws) + "/test-journals/ice"
                         : "test-journals/ice";

    std::cout << "Generating ICE E2E journals in " << dir << "\n";

    // Base ICE scenarios (8)
    gen_ice_brent_basic(dir);
    gen_ice_cancel(dir);
    gen_ice_replace(dir);
    gen_ice_smp(dir);
    gen_ice_multi_product(dir);
    gen_ice_session_lifecycle(dir);
    gen_ice_iceberg(dir);
    gen_ice_mass_cancel(dir);

    // GTBPR scenarios (4)
    gen_ice_euribor_gtbpr(dir);
    gen_ice_gtbpr_no_priority(dir);
    gen_ice_gtbpr_single_order(dir);
    gen_ice_gtbpr_sonia(dir);

    std::cout << "Done — 12 ICE E2E journals generated"
              << " (8 base + 4 GTBPR).\n";
    return 0;
}
