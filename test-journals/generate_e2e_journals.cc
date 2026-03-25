// generate_e2e_journals.cc
//
// Generates E2E .journal files by running scenarios through the full CME
// pipeline and capturing the actual SBE-encoded output. EXPECT lines are
// derived from decoded output, guaranteeing correctness.
//
// Usage:
//   bazel run //test-journals:generate_e2e_journals
//
// Writes files to $BUILD_WORKSPACE_DIRECTORY/test-journals/{e2e,cme}/.

#include "cme/cme_simulator.h"
#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/ilink3_messages.h"
#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"
#include "cme/ilink3_gateway.h"
#include "cme/ilink3_report_publisher.h"
#include "cme/mdp3_feed_publisher.h"
#include "exchange-core/composite_listener.h"
#include "test-harness/recording_listener.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace exchange;
using namespace exchange::cme;

// ---------------------------------------------------------------------------
// E2E pipeline context — sets up the full CmeSimulator with publishers.
// ---------------------------------------------------------------------------

struct E2EPipeline {
    sbe::ilink3::EncodeContext enc_ctx{};
    ILink3ReportPublisher report_pub{enc_ctx};
    RecordingOrderListener recording_ol;
    CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener> composite_ol{
        &report_pub, &recording_ol};

    Mdp3FeedPublisher md_pub;
    RecordingMdListener recording_ml;
    CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener> composite_ml{
        &md_pub, &recording_ml};

    using SimType = CmeSimulator<
        CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>,
        CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>>;
    SimType sim{composite_ol, composite_ml};
    ILink3Gateway<SimType> gateway{sim};

    std::unordered_map<std::string, InstrumentId> symbol_map;
    std::unordered_map<uint64_t, OrderId> cl_to_exch;
    std::unordered_map<uint64_t, Side> cl_to_side;
    std::unordered_map<uint64_t, OrderType> cl_to_type;
    std::unordered_map<uint64_t, TimeInForce> cl_to_tif;

    char buf[1024]{};

    E2EPipeline() {
        auto products = get_cme_products();
        sim.load_products(products);
        for (const auto& p : products) symbol_map[p.symbol] = p.instrument_id;
    }

    InstrumentId resolve(const std::string& sym) const {
        auto it = symbol_map.find(sym);
        return (it != symbol_map.end()) ? it->second : 0;
    }

    void start_continuous(Timestamp ts) {
        sim.start_trading_day(ts);
        sim.open_market(ts);
        md_pub.clear();
        report_pub.clear_reports();
    }

    void start_preopen(Timestamp ts) {
        sim.start_trading_day(ts);
        md_pub.clear();
        report_pub.clear_reports();
    }

    void open_market(Timestamp ts) {
        report_pub.clear_reports();
        md_pub.clear();
        sim.open_market(ts);
    }

    void close_market(Timestamp ts) {
        report_pub.clear_reports();
        md_pub.clear();
        sim.close_market(ts);
    }

    void clear_output() {
        report_pub.clear_reports();
        md_pub.clear();
        recording_ol.clear();
        recording_ml.clear();
    }

    void switch_instrument(const std::string& sym) {
        auto iid = resolve(sym);
        md_pub = Mdp3FeedPublisher(static_cast<int32_t>(iid));
        composite_ml = CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>(
            &md_pub, &recording_ml);
        report_pub.context().security_id = static_cast<int32_t>(iid);
    }

    void new_order(const std::string& instrument, Timestamp ts,
                   uint64_t cl_ord_id, const std::string& account,
                   Side side, Price price, Quantity qty,
                   OrderType type = OrderType::Limit,
                   TimeInForce tif = TimeInForce::DAY,
                   Quantity display_qty = 0,
                   Price stop_price = 0) {
        clear_output();
        switch_instrument(instrument);

        OrderRequest req{};
        req.client_order_id = cl_ord_id;
        req.account_id = std::hash<std::string>{}(account);
        req.side = side;
        req.price = price;
        req.quantity = qty;
        req.type = type;
        req.tif = tif;
        req.timestamp = ts;
        req.display_qty = display_qty;
        req.stop_price = stop_price;
        report_pub.register_order(req);

        sbe::ilink3::EncodeContext ctx{};
        ctx.security_id = static_cast<int32_t>(resolve(instrument));
        ctx.party_details_list_req_id = req.account_id;
        size_t len = sbe::ilink3::encode_new_order(buf, req, ctx);
        gateway.process(buf, len);

        for (const auto& evt : recording_ol.events()) {
            auto* accepted = std::get_if<OrderAccepted>(&evt);
            if (accepted) cl_to_exch[accepted->client_order_id] = accepted->id;
        }
        cl_to_side[cl_ord_id] = side;
        cl_to_type[cl_ord_id] = type;
        cl_to_tif[cl_ord_id] = tif;
        recording_ol.clear();
        recording_ml.clear();
    }

    void cancel_order(const std::string& instrument, Timestamp ts,
                      uint64_t cl_ord_id, uint64_t orig_cl_ord_id) {
        clear_output();
        switch_instrument(instrument);

        OrderId exch_id = 0;
        Side side = Side::Buy;
        auto it = cl_to_exch.find(orig_cl_ord_id);
        if (it != cl_to_exch.end()) exch_id = it->second;
        auto sit = cl_to_side.find(orig_cl_ord_id);
        if (sit != cl_to_side.end()) side = sit->second;

        sbe::ilink3::EncodeContext ctx{};
        ctx.security_id = static_cast<int32_t>(resolve(instrument));
        size_t len = sbe::ilink3::encode_cancel_order(
            buf, exch_id, cl_ord_id, side, ts, ctx);
        gateway.process(buf, len);
        recording_ol.clear();
        recording_ml.clear();
    }

    void replace_order(const std::string& instrument, Timestamp ts,
                       uint64_t cl_ord_id, uint64_t orig_cl_ord_id,
                       Price new_price, Quantity new_qty) {
        clear_output();
        switch_instrument(instrument);

        ModifyRequest req{};
        req.client_order_id = cl_ord_id;
        req.new_price = new_price;
        req.new_quantity = new_qty;
        req.timestamp = ts;

        OrderId exch_id = 0;
        Side side = Side::Buy;
        OrderType otype = OrderType::Limit;
        TimeInForce tif = TimeInForce::DAY;

        auto it = cl_to_exch.find(orig_cl_ord_id);
        if (it != cl_to_exch.end()) { exch_id = it->second; req.order_id = exch_id; }
        auto sit = cl_to_side.find(orig_cl_ord_id);
        if (sit != cl_to_side.end()) side = sit->second;
        auto tit = cl_to_type.find(orig_cl_ord_id);
        if (tit != cl_to_type.end()) otype = tit->second;
        auto fit = cl_to_tif.find(orig_cl_ord_id);
        if (fit != cl_to_tif.end()) tif = fit->second;

        sbe::ilink3::EncodeContext ctx{};
        ctx.security_id = static_cast<int32_t>(resolve(instrument));
        size_t len = sbe::ilink3::encode_modify_order(
            buf, req, exch_id, side, otype, tif, ctx);
        gateway.process(buf, len);

        // Track new order ID for replaced order.
        for (const auto& evt : recording_ol.events()) {
            auto* accepted = std::get_if<OrderAccepted>(&evt);
            if (accepted) cl_to_exch[cl_ord_id] = accepted->id;
        }
        recording_ol.clear();
        recording_ml.clear();
    }

    void mass_cancel(const std::string& instrument, Timestamp ts) {
        clear_output();
        switch_instrument(instrument);

        sbe::ilink3::EncodeContext ctx{};
        ctx.security_id = static_cast<int32_t>(resolve(instrument));
        size_t len = sbe::ilink3::encode_mass_cancel(
            buf, sbe::ilink3::MassActionScope::Instrument, ts, ctx);
        gateway.process(buf, len);
        recording_ol.clear();
        recording_ml.clear();
    }

    // Decode exec reports and produce EXPECT lines.
    std::string decode_exec_expects() {
        std::ostringstream out;
        for (const auto& report : report_pub.reports()) {
            sbe::ilink3::decode_ilink3_message(
                report.data.data(), report.length,
                [&](const auto& decoded) { format_exec(decoded, out); });
        }
        return out.str();
    }

    // Decode MDP3 packets and produce EXPECT lines.
    std::string decode_md_expects() {
        std::ostringstream out;
        for (const auto& pkt : md_pub.packets()) {
            sbe::mdp3::decode_mdp3_message(
                pkt.bytes(), pkt.len,
                [&](const auto& decoded) { format_md(decoded, out); });
        }
        return out.str();
    }

    // Concatenate exec + md expects for a single action.
    std::string all_expects() {
        return decode_exec_expects() + decode_md_expects();
    }

private:
    // --- Exec report formatters ---

    void format_exec(const sbe::ilink3::DecodedExecNew522& d, std::ostringstream& out) {
        const auto& m = d.root;
        out << "EXPECT EXEC_NEW"
            << " cl_ord_id=" << sbe::ilink3::decode_cl_ord_id(m.cl_ord_id)
            << " status=NEW\n";
    }

    void format_exec(const sbe::ilink3::DecodedExecReject523& d, std::ostringstream& out) {
        const auto& m = d.root;
        out << "EXPECT EXEC_REJECTED"
            << " cl_ord_id=" << sbe::ilink3::decode_cl_ord_id(m.cl_ord_id) << "\n";
    }

    void format_exec(const sbe::ilink3::DecodedExecTrade525& d, std::ostringstream& out) {
        const auto& m = d.root;
        std::string status;
        switch (static_cast<sbe::ilink3::OrdStatus>(m.ord_status)) {
            case sbe::ilink3::OrdStatus::Filled: status = "FILLED"; break;
            case sbe::ilink3::OrdStatus::PartiallyFilled: status = "PARTIAL"; break;
            default: status = "FILLED"; break;
        }
        out << "EXPECT EXEC_FILL"
            << " cl_ord_id=" << sbe::ilink3::decode_cl_ord_id(m.cl_ord_id)
            << " fill_price=" << m.last_px.mantissa
            << " fill_qty=" << m.last_qty
            << " leaves_qty=" << m.leaves_qty
            << " status=" << status << "\n";
    }

    void format_exec(const sbe::ilink3::DecodedExecCancel534& d, std::ostringstream& out) {
        const auto& m = d.root;
        out << "EXPECT EXEC_CANCELLED"
            << " cl_ord_id=" << sbe::ilink3::decode_cl_ord_id(m.cl_ord_id) << "\n";
    }

    void format_exec(const sbe::ilink3::DecodedCancelReject535&, std::ostringstream& out) {
        out << "EXPECT EXEC_CANCEL_REJECTED\n";
    }

    // Client-side messages — should never appear.
    void format_exec(const sbe::ilink3::DecodedNewOrder514&, std::ostringstream&) {}
    void format_exec(const sbe::ilink3::DecodedCancelRequest516&, std::ostringstream&) {}
    void format_exec(const sbe::ilink3::DecodedReplaceRequest515&, std::ostringstream&) {}
    void format_exec(const sbe::ilink3::DecodedMassAction529&, std::ostringstream&) {}

    // --- MDP3 formatters ---

    void format_md(const sbe::mdp3::DecodedRefreshBook46& d, std::ostringstream& out) {
        // The test runner verifies one packet per EXPECT line, using only
        // the first entry. Output one EXPECT per packet (not per entry).
        if (d.num_md_entries == 0) return;
        const auto& e = d.md_entries[0];
        std::string action;
        switch (static_cast<sbe::mdp3::MDUpdateAction>(e.md_update_action)) {
            case sbe::mdp3::MDUpdateAction::New: action = "MD_BOOK_ADD"; break;
            case sbe::mdp3::MDUpdateAction::Change: action = "MD_BOOK_UPDATE"; break;
            case sbe::mdp3::MDUpdateAction::Delete: action = "MD_BOOK_DELETE"; break;
            default: action = "MD_BOOK_UNKNOWN"; break;
        }
        std::string side =
            (e.md_entry_type == static_cast<char>(sbe::mdp3::MDEntryTypeBook::Bid))
            ? "BUY" : "SELL";
        out << "EXPECT " << action
            << " side=" << side
            << " price=" << e.md_entry_px.mantissa
            << " qty=" << e.md_entry_size
            << " num_orders=" << e.number_of_orders << "\n";
    }

    void format_md(const sbe::mdp3::DecodedTradeSummary48& d, std::ostringstream& out) {
        for (uint8_t i = 0; i < d.num_md_entries; ++i) {
            const auto& e = d.md_entries[i];
            std::string agg =
                (e.aggressor_side == static_cast<uint8_t>(sbe::mdp3::AggressorSide::Buy))
                ? "BUY" : "SELL";
            out << "EXPECT MD_TRADE"
                << " price=" << e.md_entry_px.mantissa
                << " qty=" << e.md_entry_size
                << " aggressor_side=" << agg << "\n";
        }
    }

    void format_md(const sbe::mdp3::DecodedSecurityStatus30&, std::ostringstream& out) {
        out << "EXPECT MD_STATUS\n";
    }

    void format_md(const sbe::mdp3::DecodedSnapshot53&, std::ostringstream&) {}
    void format_md(const sbe::mdp3::DecodedInstrumentDef54&, std::ostringstream&) {}
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
    return "DAY";
}

struct JournalBuilder {
    std::ostringstream out;

    void comment(const std::string& text) { out << "# " << text << "\n"; }
    void blank() { out << "\n"; }
    void config(const std::string& instrument) {
        out << "CONFIG match_algo=FIFO instrument=" << instrument
            << " tick_size=2500 lot_size=10000\n";
    }
    void session_start(Timestamp ts, const std::string& state) {
        out << "ACTION SESSION_START ts=" << ts << " state=" << state << "\n";
    }
    void session_open(Timestamp ts) {
        out << "ACTION SESSION_OPEN ts=" << ts << "\n";
    }
    void session_close(Timestamp ts) {
        out << "ACTION SESSION_CLOSE ts=" << ts << "\n";
    }
    void new_order(const std::string& instr, Timestamp ts, uint64_t cl_ord_id,
                   const std::string& account, Side side, Price price, Quantity qty,
                   OrderType type = OrderType::Limit, TimeInForce tif = TimeInForce::DAY,
                   Quantity display_qty = 0, Price stop_price = 0) {
        out << "ACTION ILINK3_NEW_ORDER ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " account=" << account
            << " side=" << side_str(side)
            << " price=" << price
            << " qty=" << qty
            << " type=" << type_str(type)
            << " tif=" << tif_str(tif);
        if (display_qty > 0) out << " display_qty=" << display_qty;
        if (stop_price > 0) out << " stop_price=" << stop_price;
        out << "\n";
    }
    void cancel(const std::string& instr, Timestamp ts,
                uint64_t cl_ord_id, uint64_t orig_cl_ord_id) {
        out << "ACTION ILINK3_CANCEL ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " orig_cl_ord_id=" << orig_cl_ord_id << "\n";
    }
    void replace(const std::string& instr, Timestamp ts,
                 uint64_t cl_ord_id, uint64_t orig_cl_ord_id,
                 Price price, Quantity qty) {
        out << "ACTION ILINK3_REPLACE ts=" << ts
            << " instrument=" << instr
            << " cl_ord_id=" << cl_ord_id
            << " orig_cl_ord_id=" << orig_cl_ord_id
            << " price=" << price
            << " qty=" << qty << "\n";
    }
    void mass_cancel(const std::string& instr, Timestamp ts,
                     const std::string& account) {
        out << "ACTION ILINK3_MASS_CANCEL ts=" << ts
            << " instrument=" << instr
            << " account=" << account << "\n";
    }
    void expects(const std::string& e) { out << e; }

    std::string str() const { return out.str(); }
};

// Write a journal file to disk.
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
// Generic E2E scenarios (9 journals)
// ---------------------------------------------------------------------------

void gen_e2e_basic_limit(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Add limit order, verify EXEC_NEW + MD_BOOK_ADD");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.start_continuous(0);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_basic_limit.journal", j.str());
}

void gen_e2e_fill(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Two crossing limits produce fill");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 2000, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 2000, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_fill.journal", j.str());
}

void gen_e2e_cancel(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Add then cancel order");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.cancel("ES", 2000, 2, 1);
    p.cancel_order("ES", 2000, 2, 1);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_cancel.journal", j.str());
}

void gen_e2e_replace(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Add then replace (cancel-replace) order");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.replace("ES", 2000, 2, 1, 50010000, 10000);
    p.replace_order("ES", 2000, 2, 1, 50010000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_replace.journal", j.str());
}

void gen_e2e_partial_fill(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Partial fill — aggressor qty < resting qty");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 20000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 20000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 2000, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 2000, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_partial_fill.journal", j.str());
}

void gen_e2e_reject(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Reject — IOC during PreOpen");
    j.config("ES"); j.blank();
    j.session_start(0, "PRE_OPEN"); j.blank();
    p.start_preopen(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::IOC);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::IOC);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_reject.journal", j.str());
}

void gen_e2e_mass_cancel(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Mass cancel all orders on an instrument");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1001, 2, "FIRM_A", Side::Buy, 49990000, 10000);
    p.new_order("ES", 1001, 2, "FIRM_A", Side::Buy, 49990000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.mass_cancel("ES", 2000, "FIRM_A");
    p.mass_cancel("ES", 2000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_mass_cancel.journal", j.str());
}

void gen_e2e_auction(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Auction lifecycle — PreOpen -> collect -> open -> uncross");
    j.config("ES"); j.blank();
    j.session_start(0, "PRE_OPEN"); j.blank();
    p.start_preopen(0);

    j.comment("Collect orders during PreOpen");
    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50010000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50010000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Open market — triggers auction uncross");
    j.session_open(2000);
    p.open_market(2000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_auction.journal", j.str());
}

void gen_e2e_iceberg(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("E2E: Iceberg order — only display_qty visible in MD");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 50000,
                OrderType::Limit, TimeInForce::DAY, /*display_qty=*/10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 50000,
                OrderType::Limit, TimeInForce::DAY, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/e2e_iceberg.journal", j.str());
}

// ---------------------------------------------------------------------------
// CME-specific scenarios (9 journals)
// ---------------------------------------------------------------------------

void gen_cme_es_trading_day(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Full ES trading day lifecycle");
    j.config("ES"); j.blank();

    j.session_start(0, "PRE_OPEN"); j.blank();
    p.start_preopen(0);

    j.new_order("ES", 100, 1, "FIRM_A", Side::Buy, 50010000, 10000);
    p.new_order("ES", 100, 1, "FIRM_A", Side::Buy, 50010000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 200, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 200, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Open market — auction uncross");
    j.session_open(1000);
    p.open_market(1000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Continuous trading");
    j.new_order("ES", 2000, 3, "FIRM_A", Side::Buy, 50020000, 10000);
    p.new_order("ES", 2000, 3, "FIRM_A", Side::Buy, 50020000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Close market");
    j.session_close(86400000);
    p.close_market(86400000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_es_trading_day.journal", j.str());
}

void gen_cme_smp(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Self-match prevention — same account, cancel newest");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Same account crosses — SMP cancels aggressor");
    j.new_order("ES", 2000, 2, "FIRM_A", Side::Sell, 50000000, 10000);
    p.new_order("ES", 2000, 2, "FIRM_A", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_smp.journal", j.str());
}

void gen_cme_dynamic_bands(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Dynamic price band rejection");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.comment("Establish reference price via fill");
    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Order far outside band — should be rejected");
    j.new_order("ES", 2000, 3, "FIRM_A", Side::Buy, 60000000, 10000);
    p.new_order("ES", 2000, 3, "FIRM_A", Side::Buy, 60000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_dynamic_bands.journal", j.str());
}

void gen_cme_multi_product(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Multi-product — ES + NQ simultaneous, cross-instrument isolation");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("NQ", 1001, 2, "FIRM_A", Side::Buy, 18000000, 10000);
    p.new_order("NQ", 1001, 2, "FIRM_A", Side::Buy, 18000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Fill on ES only — NQ should be unaffected");
    j.new_order("ES", 2000, 3, "FIRM_B", Side::Sell, 50000000, 10000);
    p.new_order("ES", 2000, 3, "FIRM_B", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_multi_product.journal", j.str());
}

void gen_cme_tick_alignment(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Tick alignment — ES tick = 2500 (0.25 index point)");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.comment("Price aligned to tick — accepted");
    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50002500, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50002500, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Price NOT aligned to tick — rejected (invalid price)");
    j.new_order("ES", 1001, 2, "FIRM_A", Side::Buy, 50001000, 10000);
    p.new_order("ES", 1001, 2, "FIRM_A", Side::Buy, 50001000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_tick_alignment.journal", j.str());
}

void gen_cme_ioc_in_auction(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: IOC rejected during PreOpen auction collection");
    j.config("ES"); j.blank();
    j.session_start(0, "PRE_OPEN"); j.blank();
    p.start_preopen(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::IOC);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::IOC);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_ioc_in_auction.journal", j.str());
}

void gen_cme_day_expiry(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: DAY orders expire at session close");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::DAY);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50000000, 10000,
                OrderType::Limit, TimeInForce::DAY);
    j.expects(p.all_expects()); j.blank();

    j.comment("Close market — DAY orders expire");
    j.session_close(86400000);
    p.close_market(86400000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_day_expiry.journal", j.str());
}

void gen_cme_stop_trigger(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Stop order triggered by trade at stop price");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.comment("Add stop buy at 50010000, triggered when trade >= stop price");
    j.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50010000, 10000,
                OrderType::Stop, TimeInForce::DAY, 0, 50010000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Buy, 50010000, 10000,
                OrderType::Stop, TimeInForce::DAY, 0, 50010000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Create resting bid and offer to produce a trade");
    j.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50010000, 10000);
    p.new_order("ES", 1001, 2, "FIRM_B", Side::Sell, 50010000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1002, 3, "FIRM_C", Side::Buy, 50010000, 10000);
    p.new_order("ES", 1002, 3, "FIRM_C", Side::Buy, 50010000, 10000);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_stop_trigger.journal", j.str());
}

void gen_cme_market_sweep(const std::string& dir) {
    E2EPipeline p; JournalBuilder j;
    j.comment("CME: Market order sweeps multiple resting levels");
    j.config("ES"); j.blank();
    j.session_start(0, "CONTINUOUS"); j.blank();
    p.start_continuous(0);

    j.comment("Build 3 levels of resting asks");
    j.new_order("ES", 1000, 1, "FIRM_A", Side::Sell, 50000000, 10000);
    p.new_order("ES", 1000, 1, "FIRM_A", Side::Sell, 50000000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1001, 2, "FIRM_A", Side::Sell, 50002500, 10000);
    p.new_order("ES", 1001, 2, "FIRM_A", Side::Sell, 50002500, 10000);
    j.expects(p.all_expects()); j.blank();

    j.new_order("ES", 1002, 3, "FIRM_A", Side::Sell, 50005000, 10000);
    p.new_order("ES", 1002, 3, "FIRM_A", Side::Sell, 50005000, 10000);
    j.expects(p.all_expects()); j.blank();

    j.comment("Market buy sweeps all 3 levels");
    j.new_order("ES", 2000, 4, "FIRM_B", Side::Buy, 0, 30000,
                OrderType::Market, TimeInForce::IOC);
    p.new_order("ES", 2000, 4, "FIRM_B", Side::Buy, 0, 30000,
                OrderType::Market, TimeInForce::IOC);
    j.expects(p.all_expects());

    write_journal(dir + "/cme_e2e_market_sweep.journal", j.str());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string base_dir;
    if (argc > 1) {
        base_dir = argv[1];
    } else {
        const char* ws = std::getenv("BUILD_WORKSPACE_DIRECTORY");
        if (ws) base_dir = ws;
        else base_dir = ".";
    }
    base_dir += "/test-journals";

    std::cout << "Generating E2E journals in " << base_dir << "\n";

    std::string e2e_dir = base_dir + "/e2e";
    std::string cme_dir = base_dir + "/cme";

    // Generic E2E journals (9 scenarios)
    std::cout << "\n--- Generic E2E journals ---\n";
    gen_e2e_basic_limit(e2e_dir);
    gen_e2e_fill(e2e_dir);
    gen_e2e_cancel(e2e_dir);
    gen_e2e_replace(e2e_dir);
    gen_e2e_partial_fill(e2e_dir);
    gen_e2e_reject(e2e_dir);
    gen_e2e_mass_cancel(e2e_dir);
    gen_e2e_auction(e2e_dir);
    gen_e2e_iceberg(e2e_dir);

    // CME-specific journals (9 scenarios)
    std::cout << "\n--- CME-specific E2E journals ---\n";
    gen_cme_es_trading_day(cme_dir);
    gen_cme_smp(cme_dir);
    gen_cme_dynamic_bands(cme_dir);
    gen_cme_multi_product(cme_dir);
    gen_cme_tick_alignment(cme_dir);
    gen_cme_ioc_in_auction(cme_dir);
    gen_cme_day_expiry(cme_dir);
    gen_cme_stop_trigger(cme_dir);
    gen_cme_market_sweep(cme_dir);

    std::cout << "\nDone. Generated 18 E2E journal files.\n";
    return 0;
}
