#include "krx/e2e/krx_e2e_test_runner.h"

#include "ice/fix/fix_encoder.h"
#include "krx/fast/fast_types.h"

#include <stdexcept>
#include <variant>

namespace exchange::krx {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

Side KrxE2ETestRunner::parse_side(const std::string& s) {
    if (s == "BUY") return Side::Buy;
    if (s == "SELL") return Side::Sell;
    throw std::runtime_error("Unknown side: " + s);
}

OrderType KrxE2ETestRunner::parse_order_type(const std::string& s) {
    if (s == "LIMIT") return OrderType::Limit;
    if (s == "MARKET") return OrderType::Market;
    if (s == "STOP") return OrderType::Stop;
    if (s == "STOP_LIMIT") return OrderType::StopLimit;
    throw std::runtime_error("Unknown order type: " + s);
}

TimeInForce KrxE2ETestRunner::parse_tif(const std::string& s) {
    if (s == "DAY") return TimeInForce::DAY;
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    throw std::runtime_error("Unknown TIF: " + s);
}

uint32_t KrxE2ETestRunner::resolve_instrument(const std::string& symbol) const {
    auto it = symbol_map_.find(symbol);
    return (it != symbol_map_.end()) ? it->second : 0;
}

// ---------------------------------------------------------------------------
// FIX message builders (reuse ice::fix encoder helpers)
// ---------------------------------------------------------------------------

std::string KrxE2ETestRunner::build_fix_new_order(const ParsedAction& action) {
    using namespace ::exchange::ice::fix::detail;
    using ::exchange::ice::fix::encode_side;
    using ::exchange::ice::fix::price_to_fix_str;
    using ::exchange::ice::fix::qty_to_fix_str;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("D"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("KRX"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));

    auto it_acct = action.fields.find("account");
    if (it_acct != action.fields.end())
        append_tag(body, "1", it_acct->second);

    auto side = parse_side(action.get_str("side"));
    append_tag(body, "54", encode_side(side));

    auto type = parse_order_type(action.get_str("type"));
    char ord_type_char = '2';
    switch (type) {
        case OrderType::Market:    ord_type_char = '1'; break;
        case OrderType::Limit:     ord_type_char = '2'; break;
        case OrderType::Stop:      ord_type_char = '3'; break;
        case OrderType::StopLimit: ord_type_char = '4'; break;
    }
    append_tag(body, "40", ord_type_char);

    auto tif = parse_tif(action.get_str("tif"));
    char tif_char = '0';
    switch (tif) {
        case TimeInForce::DAY: tif_char = '0'; break;
        case TimeInForce::GTC: tif_char = '1'; break;
        case TimeInForce::IOC: tif_char = '3'; break;
        case TimeInForce::FOK: tif_char = '4'; break;
        case TimeInForce::GTD: tif_char = '6'; break;
    }
    append_tag(body, "59", tif_char);

    append_tag(body, "44", price_to_fix_str(action.get_int("price")));
    append_tag(body, "38", qty_to_fix_str(action.get_int("qty")));
    append_tag(body, "55", action.get_str("instrument"));

    // KRX custom tags
    auto it_prog = action.fields.find("program_trading");
    if (it_prog != action.fields.end())
        append_tag(body, "5001", it_prog->second);

    auto it_inv = action.fields.find("investor_type");
    if (it_inv != action.fields.end())
        append_tag(body, "5002", it_inv->second);

    auto it_board = action.fields.find("board_id");
    if (it_board != action.fields.end())
        append_tag(body, "5003", it_board->second);

    return assemble_message(body);
}

std::string KrxE2ETestRunner::build_fix_cancel(const ParsedAction& action) {
    using namespace ::exchange::ice::fix::detail;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("F"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("KRX"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));
    append_tag(body, "41", action.get_str("orig_cl_ord_id"));
    append_tag(body, "55", action.get_str("instrument"));

    return assemble_message(body);
}

std::string KrxE2ETestRunner::build_fix_replace(const ParsedAction& action) {
    using namespace ::exchange::ice::fix::detail;
    using ::exchange::ice::fix::price_to_fix_str;
    using ::exchange::ice::fix::qty_to_fix_str;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("G"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("KRX"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));
    append_tag(body, "41", action.get_str("orig_cl_ord_id"));
    append_tag(body, "44", price_to_fix_str(action.get_int("price")));
    append_tag(body, "38", qty_to_fix_str(action.get_int("qty")));
    append_tag(body, "55", action.get_str("instrument"));

    return assemble_message(body);
}

// ---------------------------------------------------------------------------
// Verification: FIX exec reports
// ---------------------------------------------------------------------------

namespace {

bool check_fix_field(const ::ice::fix::FixMessage& msg, int tag,
                     const ParsedExpectation& expect, const std::string& key,
                     KrxE2EResult& result) {
    auto it = expect.fields.find(key);
    if (it == expect.fields.end()) return true;

    std::string actual = msg.get_string(tag);
    if (actual != it->second) {
        result.passed = false;
        result.expected = key + "=" + it->second;
        result.actual = key + "=" + actual;
        return false;
    }
    return true;
}

// Map KRX_EXEC_* event types to FIX ExecType (tag 150) values.
std::string expected_exec_type(const std::string& event_type) {
    if (event_type == "KRX_EXEC_NEW") return "0";
    if (event_type == "KRX_EXEC_FILL") return "2";
    if (event_type == "KRX_EXEC_PARTIAL") return "1";
    if (event_type == "KRX_EXEC_CANCELLED") return "4";
    if (event_type == "KRX_EXEC_REJECTED") return "8";
    if (event_type == "KRX_EXEC_REPLACED") return "5";
    return "";
}

}  // anonymous namespace

KrxE2EResult KrxE2ETestRunner::verify_exec(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<std::string>& reports, size_t& cursor)
{
    KrxE2EResult result{};
    result.action_index = action_idx;
    result.category = "KRX_EXEC";
    result.passed = true;

    if (cursor >= reports.size()) {
        result.passed = false;
        result.expected = expect.event_type;
        result.actual = "<no more exec reports>";
        return result;
    }

    const auto& report = reports[cursor++];
    auto parsed = ::ice::fix::parse_fix_message(report.data(), report.size());
    if (!parsed.has_value()) {
        result.passed = false;
        result.actual = "<parse error: " + parsed.error() + ">";
        return result;
    }

    const auto& msg = parsed.value();

    // Check ExecType matches event type.
    std::string exp_exec = expected_exec_type(expect.event_type);
    if (!exp_exec.empty()) {
        std::string actual_exec = msg.get_string(150);
        if (actual_exec != exp_exec) {
            result.passed = false;
            result.expected = "ExecType=" + exp_exec;
            result.actual = "ExecType=" + actual_exec;
            return result;
        }
    }

    // Check specific fields requested by the expectation.
    check_fix_field(msg, 37, expect, "ord_id", result);
    check_fix_field(msg, 11, expect, "cl_ord_id", result);
    check_fix_field(msg, 44, expect, "fill_price", result);
    check_fix_field(msg, 32, expect, "fill_qty", result);
    check_fix_field(msg, 39, expect, "status", result);

    return result;
}

// ---------------------------------------------------------------------------
// Verification: FAST market data
// ---------------------------------------------------------------------------

KrxE2EResult KrxE2ETestRunner::verify_md(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<fast::FastPacket>& packets, size_t& cursor)
{
    KrxE2EResult result{};
    result.action_index = action_idx;
    result.category = "KRX_MD";
    result.passed = true;

    if (cursor >= packets.size()) {
        result.passed = false;
        result.expected = expect.event_type;
        result.actual = "<no more FAST packets>";
        return result;
    }

    const auto& pkt = packets[cursor++];

    struct MdChecker : fast::FastDecoderVisitorBase {
        const ParsedExpectation& expect;
        KrxE2EResult& result;
        bool visited{false};

        MdChecker(const ParsedExpectation& e, KrxE2EResult& r)
            : expect(e), result(r) {}

        void on_quote(const fast::FastQuote& q) {
            visited = true;
            if (expect.event_type != "KRX_MD_QUOTE") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "KRX_MD_QUOTE";
                return;
            }
            auto it_bp = expect.fields.find("bid_price");
            if (it_bp != expect.fields.end()) {
                int64_t exp_p = std::stoll(it_bp->second);
                if (q.bid_price != exp_p) {
                    result.passed = false;
                    result.expected = "bid_price=" + it_bp->second;
                    result.actual = "bid_price=" + std::to_string(q.bid_price);
                }
            }
            auto it_ap = expect.fields.find("ask_price");
            if (it_ap != expect.fields.end()) {
                int64_t exp_p = std::stoll(it_ap->second);
                if (q.ask_price != exp_p) {
                    result.passed = false;
                    result.expected = "ask_price=" + it_ap->second;
                    result.actual = "ask_price=" + std::to_string(q.ask_price);
                }
            }
        }

        void on_trade(const fast::FastTrade& t) {
            visited = true;
            if (expect.event_type != "KRX_MD_TRADE") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "KRX_MD_TRADE";
                return;
            }
            auto it_price = expect.fields.find("price");
            if (it_price != expect.fields.end()) {
                int64_t exp_p = std::stoll(it_price->second);
                if (t.price != exp_p) {
                    result.passed = false;
                    result.expected = "price=" + it_price->second;
                    result.actual = "price=" + std::to_string(t.price);
                }
            }
            auto it_qty = expect.fields.find("qty");
            if (it_qty != expect.fields.end()) {
                int64_t exp_q = std::stoll(it_qty->second);
                if (t.quantity != exp_q) {
                    result.passed = false;
                    result.expected = "qty=" + it_qty->second;
                    result.actual = "qty=" + std::to_string(t.quantity);
                }
            }
        }

        void on_status(const fast::FastStatus& s) {
            visited = true;
            if (expect.event_type != "KRX_MD_STATUS") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "KRX_MD_STATUS";
                return;
            }
            auto it_state = expect.fields.find("state");
            if (it_state != expect.fields.end()) {
                uint8_t exp_s = static_cast<uint8_t>(std::stoi(it_state->second));
                if (s.session_state != exp_s) {
                    result.passed = false;
                    result.expected = "state=" + it_state->second;
                    result.actual = "state=" + std::to_string(s.session_state);
                }
            }
        }
    };

    MdChecker checker{expect, result};
    fast::decode_message(pkt.bytes(), pkt.len, checker);

    if (!checker.visited && result.passed) {
        result.passed = false;
        result.actual = "<no decodable FAST message>";
    }

    return result;
}

// ---------------------------------------------------------------------------
// Main run()
// ---------------------------------------------------------------------------

std::vector<KrxE2EResult> KrxE2ETestRunner::run(const Journal& journal) {
    std::vector<KrxE2EResult> results;

    symbol_map_.clear();
    cl_to_exchange_id_.clear();

    // Build symbol map from KRX products.
    auto products = get_krx_products();
    for (const auto& p : products) {
        symbol_map_[p.symbol] = p.instrument_id;
    }

    // Create publishers.
    fix::KrxFixExecPublisher exec_pub("KRX", "CLIENT", "");
    RecordingOrderListener recording_ol;
    CompositeOrderListener<fix::KrxFixExecPublisher, RecordingOrderListener>
        composite_ol(&exec_pub, &recording_ol);

    fast::FastFeedPublisher md_pub;
    RecordingMdListener recording_ml;
    CompositeMdListener<fast::FastFeedPublisher, RecordingMdListener>
        composite_ml(&md_pub, &recording_ml);

    // Create simulator with composite listeners.
    using SimType = KrxSimulator<
        CompositeOrderListener<fix::KrxFixExecPublisher, RecordingOrderListener>,
        CompositeMdListener<fast::FastFeedPublisher, RecordingMdListener>>;
    SimType sim(composite_ol, composite_ml);
    sim.load_products(products);

    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const auto& entry = journal.entries[i];
        const auto& action = entry.action;

        exec_pub.clear_messages();
        md_pub.clear();

        switch (action.type) {
            case ParsedAction::SessionStart: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                auto state_str = action.get_str("state");
                if (state_str == "CONTINUOUS") {
                    sim.start_regular_session(ts);
                    sim.open_regular_market(ts + 1);
                } else if (state_str == "PRE_OPEN") {
                    sim.start_regular_session(ts);
                } else if (state_str == "AFTER_HOURS") {
                    sim.start_after_hours(ts);
                }
                exec_pub.clear_messages();
                md_pub.clear();
                break;
            }
            case ParsedAction::SessionOpen: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                sim.open_regular_market(ts);
                break;
            }
            case ParsedAction::SessionClose: {
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                sim.close_regular_session(ts);
                break;
            }
            case ParsedAction::KrxFixNewOrder: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);

                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                auto cl_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
                auto side = parse_side(action.get_str("side"));
                auto price = action.get_int("price");
                auto qty = action.get_int("qty");

                OrderRequest req{};
                req.client_order_id = cl_id;
                auto it_acct = action.fields.find("account");
                if (it_acct != action.fields.end())
                    req.account_id = static_cast<uint64_t>(
                        std::strtoll(it_acct->second.c_str(), nullptr, 10));
                req.side = side;
                req.type = parse_order_type(action.get_str("type"));
                req.tif = parse_tif(action.get_str("tif"));
                req.price = price;
                req.quantity = qty;
                req.timestamp = ts;

                // Register with exec publisher before engine fires callbacks.
                exec_pub.register_order(cl_id, cl_id, price, qty, side);

                sim.new_order(iid, req);

                // Track exchange-assigned IDs.
                for (const auto& evt : recording_ol.events()) {
                    auto* accepted = std::get_if<OrderAccepted>(&evt);
                    if (accepted) {
                        cl_to_exchange_id_[accepted->client_order_id] =
                            accepted->id;
                        exec_pub.register_order(
                            accepted->id, accepted->client_order_id,
                            price, qty, side);
                    }
                }
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::KrxFixCancel: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);

                auto orig = static_cast<uint64_t>(
                    action.get_int("orig_cl_ord_id"));
                auto ts = static_cast<Timestamp>(action.get_int("ts"));

                OrderId eid = 0;
                auto it = cl_to_exchange_id_.find(orig);
                if (it != cl_to_exchange_id_.end()) eid = it->second;

                sim.cancel_order(iid, eid, ts);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::KrxFixReplace: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);

                auto orig = static_cast<uint64_t>(
                    action.get_int("orig_cl_ord_id"));
                auto ts = static_cast<Timestamp>(action.get_int("ts"));

                ModifyRequest req{};
                auto it = cl_to_exchange_id_.find(orig);
                req.order_id = (it != cl_to_exchange_id_.end()) ? it->second : 0;
                req.client_order_id = static_cast<uint64_t>(
                    action.get_int("cl_ord_id"));
                req.new_price = action.get_int("price");
                req.new_quantity = action.get_int("qty");
                req.timestamp = ts;

                sim.modify_order(iid, req);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            default:
                continue;
        }

        // Verify expectations.
        size_t exec_cursor = 0;
        size_t md_cursor = 0;

        for (const auto& expect : entry.expectations) {
            bool is_exec = (expect.event_type.size() >= 8 &&
                            expect.event_type.substr(0, 8) == "KRX_EXEC");
            bool is_md = (expect.event_type.size() >= 6 &&
                          expect.event_type.substr(0, 6) == "KRX_MD");

            KrxE2EResult r{};
            if (is_exec) {
                r = verify_exec(expect, i, exec_pub.messages(), exec_cursor);
            } else if (is_md) {
                r = verify_md(expect, i, md_pub.packets(), md_cursor);
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

}  // namespace exchange::krx
