#include "cme/e2e/e2e_test_runner.h"

#include "cme/codec/ilink3_messages.h"
#include "cme/codec/mdp3_messages.h"
#include "cme/codec/sbe_header.h"

#include <cstring>
#include <stdexcept>
#include <variant>

namespace exchange::cme {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

Side E2ETestRunner::parse_side(const std::string& s) {
    if (s == "BUY") return Side::Buy;
    if (s == "SELL") return Side::Sell;
    throw std::runtime_error("Unknown side: " + s);
}

OrderType E2ETestRunner::parse_order_type(const std::string& s) {
    if (s == "LIMIT") return OrderType::Limit;
    if (s == "MARKET") return OrderType::Market;
    if (s == "STOP") return OrderType::Stop;
    if (s == "STOP_LIMIT") return OrderType::StopLimit;
    throw std::runtime_error("Unknown order type: " + s);
}

TimeInForce E2ETestRunner::parse_tif(const std::string& s) {
    if (s == "DAY") return TimeInForce::DAY;
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    throw std::runtime_error("Unknown TIF: " + s);
}

InstrumentId E2ETestRunner::resolve_instrument(const std::string& symbol) const {
    auto it = symbol_map_.find(symbol);
    return (it != symbol_map_.end()) ? it->second : 0;
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

size_t E2ETestRunner::encode_new_order(const ParsedAction& action, char* buf) {
    auto instrument_id = resolve_instrument(action.get_str("instrument"));

    sbe::ilink3::EncodeContext ctx{};
    ctx.security_id = static_cast<int32_t>(instrument_id);

    OrderRequest req{};
    req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
    auto it_account = action.fields.find("account");
    if (it_account != action.fields.end()) {
        req.account_id = std::hash<std::string>{}(it_account->second);
        // Set party_details_list_req_id so the gateway can propagate account_id.
        ctx.party_details_list_req_id = req.account_id;
    }
    req.side = parse_side(action.get_str("side"));
    req.price = action.get_int("price");
    req.quantity = action.get_int("qty");
    req.type = parse_order_type(action.get_str("type"));
    req.tif = parse_tif(action.get_str("tif"));
    req.timestamp = action.get_int("ts");

    auto it_disp = action.fields.find("display_qty");
    if (it_disp != action.fields.end())
        req.display_qty = std::stoll(it_disp->second);
    auto it_stop = action.fields.find("stop_price");
    if (it_stop != action.fields.end())
        req.stop_price = std::stoll(it_stop->second);

    OrderMeta meta{};
    meta.side = req.side;
    meta.type = req.type;
    meta.tif = req.tif;
    order_meta_[req.client_order_id] = meta;

    return sbe::ilink3::encode_new_order(buf, req, ctx);
}

size_t E2ETestRunner::encode_cancel(const ParsedAction& action, char* buf) {
    auto instrument_id = resolve_instrument(action.get_str("instrument"));

    sbe::ilink3::EncodeContext ctx{};
    ctx.security_id = static_cast<int32_t>(instrument_id);

    auto cl_ord_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
    auto orig_cl_ord_id = static_cast<uint64_t>(action.get_int("orig_cl_ord_id"));
    auto ts = static_cast<Timestamp>(action.get_int("ts"));

    OrderId exchange_id = 0;
    Side side = Side::Buy;
    auto it = cl_ord_to_exchange_id_.find(orig_cl_ord_id);
    if (it != cl_ord_to_exchange_id_.end())
        exchange_id = it->second;
    auto mit = order_meta_.find(orig_cl_ord_id);
    if (mit != order_meta_.end())
        side = mit->second.side;

    return sbe::ilink3::encode_cancel_order(
        buf, exchange_id, cl_ord_id, side, ts, ctx);
}

size_t E2ETestRunner::encode_replace(const ParsedAction& action, char* buf) {
    auto instrument_id = resolve_instrument(action.get_str("instrument"));

    sbe::ilink3::EncodeContext ctx{};
    ctx.security_id = static_cast<int32_t>(instrument_id);

    auto cl_ord_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
    auto orig_cl_ord_id = static_cast<uint64_t>(action.get_int("orig_cl_ord_id"));
    auto ts = static_cast<Timestamp>(action.get_int("ts"));

    ModifyRequest req{};
    req.client_order_id = cl_ord_id;
    req.new_price = action.get_int("price");
    req.new_quantity = action.get_int("qty");
    req.timestamp = ts;

    OrderId exchange_id = 0;
    Side side = Side::Buy;
    OrderType ord_type = OrderType::Limit;
    TimeInForce tif = TimeInForce::DAY;

    auto it = cl_ord_to_exchange_id_.find(orig_cl_ord_id);
    if (it != cl_ord_to_exchange_id_.end()) {
        exchange_id = it->second;
        req.order_id = exchange_id;
    }
    auto mit = order_meta_.find(orig_cl_ord_id);
    if (mit != order_meta_.end()) {
        side = mit->second.side;
        ord_type = mit->second.type;
        tif = mit->second.tif;
    }

    return sbe::ilink3::encode_modify_order(
        buf, req, exchange_id, side, ord_type, tif, ctx);
}

size_t E2ETestRunner::encode_mass_cancel(const ParsedAction& action, char* buf) {
    auto instrument_id = resolve_instrument(action.get_str("instrument"));

    sbe::ilink3::EncodeContext ctx{};
    ctx.security_id = static_cast<int32_t>(instrument_id);
    auto ts = static_cast<Timestamp>(action.get_int("ts"));

    return sbe::ilink3::encode_mass_cancel(
        buf, sbe::ilink3::MassActionScope::Instrument, ts, ctx);
}

// ---------------------------------------------------------------------------
// Verification helpers (defined before verify_exec_report/verify_market_data)
// ---------------------------------------------------------------------------

namespace {

bool check_field_int(const ParsedExpectation& expect, const std::string& key,
                     int64_t actual_value, E2EResult& result) {
    auto it = expect.fields.find(key);
    if (it == expect.fields.end()) return true;
    int64_t expected_value = std::stoll(it->second);
    if (expected_value != actual_value) {
        result.passed = false;
        result.expected = key + "=" + it->second;
        result.actual = key + "=" + std::to_string(actual_value);
        return false;
    }
    return true;
}

bool check_field_str(const ParsedExpectation& expect, const std::string& key,
                     const std::string& actual_value, E2EResult& result) {
    auto it = expect.fields.find(key);
    if (it == expect.fields.end()) return true;
    if (it->second != actual_value) {
        result.passed = false;
        result.expected = key + "=" + it->second;
        result.actual = key + "=" + actual_value;
        return false;
    }
    return true;
}

std::string status_str(uint8_t ord_status) {
    switch (static_cast<sbe::ilink3::OrdStatus>(ord_status)) {
        case sbe::ilink3::OrdStatus::New: return "NEW";
        case sbe::ilink3::OrdStatus::PartiallyFilled: return "PARTIAL";
        case sbe::ilink3::OrdStatus::Filled: return "FILLED";
        case sbe::ilink3::OrdStatus::Canceled: return "CANCELLED";
        case sbe::ilink3::OrdStatus::Rejected: return "REJECTED";
        case sbe::ilink3::OrdStatus::Expired: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

std::string side_str(char md_entry_type) {
    if (md_entry_type == static_cast<char>(sbe::mdp3::MDEntryTypeBook::Bid))
        return "BUY";
    if (md_entry_type == static_cast<char>(sbe::mdp3::MDEntryTypeBook::Offer))
        return "SELL";
    return "UNKNOWN";
}

std::string aggressor_side_str(uint8_t agg) {
    if (agg == static_cast<uint8_t>(sbe::mdp3::AggressorSide::Buy)) return "BUY";
    if (agg == static_cast<uint8_t>(sbe::mdp3::AggressorSide::Sell)) return "SELL";
    return "NONE";
}

std::string md_action_str(uint8_t action) {
    switch (static_cast<sbe::mdp3::MDUpdateAction>(action)) {
        case sbe::mdp3::MDUpdateAction::New: return "ADD";
        case sbe::mdp3::MDUpdateAction::Change: return "UPDATE";
        case sbe::mdp3::MDUpdateAction::Delete: return "DELETE";
        default: return "OTHER";
    }
}

// Visitor for iLink3 exec report verification.
struct ExecVerifier {
    const ParsedExpectation& expect;
    E2EResult& result;

    void operator()(const sbe::ilink3::DecodedExecNew522& d) {
        result.passed = true;
        if (expect.event_type != "EXEC_NEW" && expect.event_type != "EXEC_REPLACED") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "EXEC_NEW";
            return;
        }
        const auto& msg = d.root;
        check_field_int(expect, "ord_id", static_cast<int64_t>(msg.order_id), result);
        check_field_int(expect, "cl_ord_id",
            static_cast<int64_t>(sbe::ilink3::decode_cl_ord_id(msg.cl_ord_id)), result);
        check_field_str(expect, "status", "NEW", result);
    }

    void operator()(const sbe::ilink3::DecodedExecReject523&) {
        result.passed = true;
        if (expect.event_type != "EXEC_REJECTED") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "EXEC_REJECTED";
            return;
        }
    }

    void operator()(const sbe::ilink3::DecodedExecTrade525& d) {
        result.passed = true;
        if (expect.event_type != "EXEC_FILL" && expect.event_type != "EXEC_PARTIAL") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "EXEC_FILL";
            return;
        }
        const auto& msg = d.root;
        check_field_int(expect, "ord_id", static_cast<int64_t>(msg.order_id), result);
        check_field_int(expect, "cl_ord_id",
            static_cast<int64_t>(sbe::ilink3::decode_cl_ord_id(msg.cl_ord_id)), result);
        check_field_int(expect, "fill_price", msg.last_px.mantissa, result);
        check_field_int(expect, "fill_qty", static_cast<int64_t>(msg.last_qty), result);
        check_field_int(expect, "leaves_qty", static_cast<int64_t>(msg.leaves_qty), result);
        check_field_str(expect, "status", status_str(msg.ord_status), result);
    }

    void operator()(const sbe::ilink3::DecodedExecCancel534& d) {
        result.passed = true;
        if (expect.event_type != "EXEC_CANCELLED") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "EXEC_CANCELLED";
            return;
        }
        const auto& msg = d.root;
        check_field_int(expect, "ord_id", static_cast<int64_t>(msg.order_id), result);
        check_field_int(expect, "cl_ord_id",
            static_cast<int64_t>(sbe::ilink3::decode_cl_ord_id(msg.cl_ord_id)), result);
    }

    void operator()(const sbe::ilink3::DecodedCancelReject535&) {
        result.passed = true;
        if (expect.event_type != "EXEC_CANCEL_REJECTED") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "EXEC_CANCEL_REJECTED";
        }
    }

    // Client->exchange messages should not appear in output.
    void operator()(const sbe::ilink3::DecodedNewOrder514&) {
        result.passed = false;
        result.actual = "<unexpected: NewOrder in output>";
    }
    void operator()(const sbe::ilink3::DecodedCancelRequest516&) {
        result.passed = false;
        result.actual = "<unexpected: CancelRequest in output>";
    }
    void operator()(const sbe::ilink3::DecodedReplaceRequest515&) {
        result.passed = false;
        result.actual = "<unexpected: ReplaceRequest in output>";
    }
    void operator()(const sbe::ilink3::DecodedMassAction529&) {
        result.passed = false;
        result.actual = "<unexpected: MassAction in output>";
    }
};

// Visitor for MDP3 market data verification.
struct MdVerifier {
    const ParsedExpectation& expect;
    E2EResult& result;

    void operator()(const sbe::mdp3::DecodedRefreshBook46& d) {
        if (d.num_md_entries == 0) {
            result.passed = false;
            result.actual = "<empty book refresh>";
            return;
        }
        const auto& entry = d.md_entries[0];
        std::string action = md_action_str(entry.md_update_action);

        std::string expected_action;
        if (expect.event_type == "MD_BOOK_ADD") expected_action = "ADD";
        else if (expect.event_type == "MD_BOOK_UPDATE") expected_action = "UPDATE";
        else if (expect.event_type == "MD_BOOK_DELETE") expected_action = "DELETE";
        else {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "MD_BOOK_*";
            return;
        }

        if (action != expected_action) {
            result.passed = false;
            result.expected = "action=" + expected_action;
            result.actual = "action=" + action;
            return;
        }

        check_field_str(expect, "side", side_str(entry.md_entry_type), result);
        check_field_int(expect, "price", entry.md_entry_px.mantissa, result);
        check_field_int(expect, "qty", static_cast<int64_t>(entry.md_entry_size), result);
        check_field_int(expect, "num_orders",
                        static_cast<int64_t>(entry.number_of_orders), result);
    }

    void operator()(const sbe::mdp3::DecodedTradeSummary48& d) {
        if (expect.event_type != "MD_TRADE") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "MD_TRADE";
            return;
        }
        if (d.num_md_entries == 0) {
            result.passed = false;
            result.actual = "<empty trade summary>";
            return;
        }
        const auto& entry = d.md_entries[0];
        check_field_int(expect, "price", entry.md_entry_px.mantissa, result);
        check_field_int(expect, "qty", static_cast<int64_t>(entry.md_entry_size), result);
        check_field_str(expect, "aggressor_side",
                        aggressor_side_str(entry.aggressor_side), result);
    }

    void operator()(const sbe::mdp3::DecodedSecurityStatus30&) {
        if (expect.event_type != "MD_STATUS") {
            result.passed = false;
            result.expected = expect.event_type;
            result.actual = "MD_STATUS";
        }
    }

    void operator()(const sbe::mdp3::DecodedSnapshot53&) {
        result.passed = false;
        result.actual = "<unexpected snapshot in E2E>";
    }

    void operator()(const sbe::mdp3::DecodedInstrumentDef54&) {
        result.passed = false;
        result.actual = "<unexpected instrument def in E2E>";
    }
};

}  // anonymous namespace

// ---------------------------------------------------------------------------
// verify_exec_report / verify_market_data
// ---------------------------------------------------------------------------

E2EResult E2ETestRunner::verify_exec_report(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<EncodedReport>& reports, size_t& report_cursor)
{
    E2EResult result{};
    result.action_index = action_idx;
    result.category = "EXEC_REPORT";
    result.passed = true;

    if (report_cursor >= reports.size()) {
        result.passed = false;
        result.expected = expect.event_type;
        result.actual = "<no more exec reports>";
        return result;
    }

    const auto& report = reports[report_cursor++];

    ExecVerifier verifier{expect, result};
    auto decode_rc = sbe::ilink3::decode_ilink3_message(
        report.data.data(), report.length, verifier);

    if (decode_rc != sbe::ilink3::DecodeResult::kOk) {
        result.passed = false;
        result.actual = "<decode error>";
    }

    return result;
}

E2EResult E2ETestRunner::verify_market_data(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<Mdp3Packet>& packets, size_t& packet_cursor)
{
    E2EResult result{};
    result.action_index = action_idx;
    result.category = "MARKET_DATA";
    result.passed = true;

    if (packet_cursor >= packets.size()) {
        result.passed = false;
        result.expected = expect.event_type;
        result.actual = "<no more MD packets>";
        return result;
    }

    const auto& pkt = packets[packet_cursor++];

    MdVerifier verifier{expect, result};
    auto decode_rc = sbe::mdp3::decode_mdp3_message(
        pkt.bytes(), pkt.len, verifier);

    if (decode_rc != sbe::mdp3::DecodeResult::kOk) {
        result.passed = false;
        result.actual = "<MDP3 decode error>";
    }

    return result;
}

// ---------------------------------------------------------------------------
// Main run() implementation
// ---------------------------------------------------------------------------

std::vector<E2EResult> E2ETestRunner::run(const Journal& journal) {
    std::vector<E2EResult> results;

    // Clear state from any prior run.
    symbol_map_.clear();
    cl_ord_to_exchange_id_.clear();
    order_meta_.clear();

    // Build symbol map from the CME product table.
    auto products = cme::get_cme_products();
    for (const auto& p : products) {
        symbol_map_[p.symbol] = p.instrument_id;
    }

    // Create composite listeners: SBE publishers + recording listeners.
    sbe::ilink3::EncodeContext enc_ctx{};
    ILink3ReportPublisher report_pub(enc_ctx);
    RecordingOrderListener recording_ol;
    CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>
        composite_ol(&report_pub, &recording_ol);

    Mdp3FeedPublisher md_pub;
    RecordingMdListener recording_ml;
    CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>
        composite_ml(&md_pub, &recording_ml);

    // Create the simulator with composite listeners.
    using SimType = CmeSimulator<
        CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>,
        CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>>;
    SimType sim(composite_ol, composite_ml);
    sim.load_products(products);

    // Create the gateway.
    ILink3Gateway<SimType> gateway(sim);

    char encode_buf[1024]{};

    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const auto& entry = journal.entries[i];
        const auto& action = entry.action;

        // Clear per-action output collectors.
        report_pub.clear_reports();
        md_pub.clear();

        switch (action.type) {
            case ParsedAction::SessionStart: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                auto state_str = action.get_str("state");
                if (state_str == "CONTINUOUS") {
                    sim.start_trading_day(ts);
                    sim.open_market(ts);
                } else if (state_str == "PRE_OPEN") {
                    sim.start_trading_day(ts);
                }
                // Clear session-transition events before checking expects.
                md_pub.clear();
                report_pub.clear_reports();
                break;
            }
            case ParsedAction::SessionOpen: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                sim.open_market(ts);
                break;
            }
            case ParsedAction::SessionClose: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                sim.close_market(ts);
                break;
            }
            case ParsedAction::ILink3NewOrder: {
                auto instr = action.get_str("instrument");
                auto iid = resolve_instrument(instr);
                // Update MDP3 publisher context for this instrument.
                md_pub = Mdp3FeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);
                report_pub.context().security_id = static_cast<int32_t>(iid);

                // Register order with the report publisher (for price/qty/side context).
                OrderRequest req{};
                req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
                req.side = parse_side(action.get_str("side"));
                req.price = action.get_int("price");
                req.quantity = action.get_int("qty");
                req.type = parse_order_type(action.get_str("type"));
                req.tif = parse_tif(action.get_str("tif"));
                auto it_disp = action.fields.find("display_qty");
                if (it_disp != action.fields.end())
                    req.display_qty = std::stoll(it_disp->second);
                report_pub.register_order(req);

                size_t len = encode_new_order(action, encode_buf);
                gateway.process(encode_buf, len);

                // Track exchange-assigned order_id from the recording listener.
                for (const auto& evt : recording_ol.events()) {
                    auto* accepted = std::get_if<OrderAccepted>(&evt);
                    if (accepted) {
                        cl_ord_to_exchange_id_[accepted->client_order_id] = accepted->id;
                        auto mit = order_meta_.find(accepted->client_order_id);
                        if (mit != order_meta_.end())
                            mit->second.exchange_id = accepted->id;
                    }
                }
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::ILink3Cancel: {
                auto iid = resolve_instrument(action.get_str("instrument"));
                md_pub = Mdp3FeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);
                report_pub.context().security_id = static_cast<int32_t>(iid);

                size_t len = encode_cancel(action, encode_buf);
                gateway.process(encode_buf, len);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::ILink3Replace: {
                auto iid = resolve_instrument(action.get_str("instrument"));
                md_pub = Mdp3FeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);
                report_pub.context().security_id = static_cast<int32_t>(iid);

                size_t len = encode_replace(action, encode_buf);
                gateway.process(encode_buf, len);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::ILink3MassCancel: {
                auto iid = resolve_instrument(action.get_str("instrument"));
                md_pub = Mdp3FeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<Mdp3FeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);
                report_pub.context().security_id = static_cast<int32_t>(iid);

                size_t len = encode_mass_cancel(action, encode_buf);
                gateway.process(encode_buf, len);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            default:
                continue;
        }

        // Verify expectations against collected SBE output.
        size_t report_cursor = 0;
        size_t md_cursor = 0;

        for (const auto& expect : entry.expectations) {
            bool is_exec = (expect.event_type.substr(0, 4) == "EXEC");
            bool is_md = (expect.event_type.substr(0, 2) == "MD");

            E2EResult r{};
            if (is_exec) {
                r = verify_exec_report(expect, i, report_pub.reports(), report_cursor);
            } else if (is_md) {
                r = verify_market_data(expect, i, md_pub.packets(), md_cursor);
            } else {
                r.passed = false;
                r.action_index = i;
                r.expected = expect.event_type;
                r.actual = "<unknown expect type>";
                r.category = "UNKNOWN";
            }
            results.push_back(r);
        }
    }

    return results;
}

}  // namespace exchange::cme
