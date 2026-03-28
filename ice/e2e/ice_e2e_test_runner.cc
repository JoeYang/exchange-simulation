#include "ice/e2e/ice_e2e_test_runner.h"

#include "ice/fix/fix_encoder.h"
#include "ice/impact/impact_messages.h"

#include <stdexcept>
#include <variant>

namespace exchange::ice {

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

Side IceE2ETestRunner::parse_side(const std::string& s) {
    if (s == "BUY") return Side::Buy;
    if (s == "SELL") return Side::Sell;
    throw std::runtime_error("Unknown side: " + s);
}

OrderType IceE2ETestRunner::parse_order_type(const std::string& s) {
    if (s == "LIMIT") return OrderType::Limit;
    if (s == "MARKET") return OrderType::Market;
    if (s == "STOP") return OrderType::Stop;
    if (s == "STOP_LIMIT") return OrderType::StopLimit;
    throw std::runtime_error("Unknown order type: " + s);
}

TimeInForce IceE2ETestRunner::parse_tif(const std::string& s) {
    if (s == "DAY") return TimeInForce::DAY;
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    throw std::runtime_error("Unknown TIF: " + s);
}

uint32_t IceE2ETestRunner::resolve_instrument(const std::string& symbol) const {
    auto it = symbol_map_.find(symbol);
    return (it != symbol_map_.end()) ? it->second : 0;
}

// ---------------------------------------------------------------------------
// FIX message builders
// ---------------------------------------------------------------------------

std::string IceE2ETestRunner::build_fix_new_order(const ParsedAction& action) {
    using namespace fix::detail;
    constexpr char SOH = fix::SOH;
    (void)SOH;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("D"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("ICE"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));

    auto it_acct = action.fields.find("account");
    if (it_acct != action.fields.end())
        append_tag(body, "1", it_acct->second);

    auto side = parse_side(action.get_str("side"));
    append_tag(body, "54", fix::encode_side(side));

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

    append_tag(body, "44", fix::price_to_fix_str(action.get_int("price")));
    append_tag(body, "38", fix::qty_to_fix_str(action.get_int("qty")));
    append_tag(body, "55", action.get_str("instrument"));

    return assemble_message(body);
}

std::string IceE2ETestRunner::build_fix_cancel(const ParsedAction& action) {
    using namespace fix::detail;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("F"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("ICE"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));
    append_tag(body, "41", action.get_str("orig_cl_ord_id"));
    append_tag(body, "55", action.get_str("instrument"));

    return assemble_message(body);
}

std::string IceE2ETestRunner::build_fix_replace(const ParsedAction& action) {
    using namespace fix::detail;

    std::string body;
    body.reserve(256);
    append_tag(body, "35", std::string("G"));
    append_tag(body, "49", std::string("CLIENT"));
    append_tag(body, "56", std::string("ICE"));
    append_tag(body, "34", static_cast<uint64_t>(1));
    append_tag(body, "52", std::string("20260101-00:00:00"));

    append_tag(body, "11", action.get_str("cl_ord_id"));
    append_tag(body, "41", action.get_str("orig_cl_ord_id"));
    append_tag(body, "44", fix::price_to_fix_str(action.get_int("price")));
    append_tag(body, "38", fix::qty_to_fix_str(action.get_int("qty")));
    append_tag(body, "55", action.get_str("instrument"));

    return assemble_message(body);
}

// ---------------------------------------------------------------------------
// Verification: FIX exec reports
// ---------------------------------------------------------------------------

namespace {

// Parse a FIX exec report and check fields against expectation.
bool check_fix_field(const ::ice::fix::FixMessage& msg, int tag,
                     const ParsedExpectation& expect, const std::string& key,
                     IceE2EResult& result) {
    auto it = expect.fields.find(key);
    if (it == expect.fields.end()) return true;  // not checking this field

    std::string actual = msg.get_string(tag);
    if (actual != it->second) {
        result.passed = false;
        result.expected = key + "=" + it->second;
        result.actual = key + "=" + actual;
        return false;
    }
    return true;
}

// Map ICE_EXEC_* event types to FIX ExecType (tag 150) values.
std::string expected_exec_type(const std::string& event_type) {
    if (event_type == "ICE_EXEC_NEW") return "0";
    if (event_type == "ICE_EXEC_FILL") return "2";       // FIX 4.2: Fill
    if (event_type == "ICE_EXEC_PARTIAL") return "1";   // FIX 4.2: PartialFill
    if (event_type == "ICE_EXEC_CANCELLED") return "4";
    if (event_type == "ICE_EXEC_REJECTED") return "8";
    if (event_type == "ICE_EXEC_REPLACED") return "5";
    return "";
}

}  // anonymous namespace

IceE2EResult IceE2ETestRunner::verify_exec(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<std::string>& reports, size_t& cursor)
{
    IceE2EResult result{};
    result.action_index = action_idx;
    result.category = "ICE_EXEC";
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
    check_fix_field(msg, 37, expect, "ord_id", result);     // OrderID
    check_fix_field(msg, 11, expect, "cl_ord_id", result);  // ClOrdID
    check_fix_field(msg, 44, expect, "fill_price", result);  // Price (LastPx for fills)
    check_fix_field(msg, 32, expect, "fill_qty", result);    // LastQty
    check_fix_field(msg, 39, expect, "status", result);      // OrdStatus

    return result;
}

// ---------------------------------------------------------------------------
// Verification: iMpact market data
// ---------------------------------------------------------------------------

IceE2EResult IceE2ETestRunner::verify_md(
    const ParsedExpectation& expect, size_t action_idx,
    const std::vector<impact::ImpactPacket>& packets, size_t& cursor)
{
    IceE2EResult result{};
    result.action_index = action_idx;
    result.category = "ICE_MD";
    result.passed = true;

    if (cursor >= packets.size()) {
        result.passed = false;
        result.expected = expect.event_type;
        result.actual = "<no more MD packets>";
        return result;
    }

    const auto& pkt = packets[cursor++];

    // Decode and verify using named-member visitor (iMpact decoder API).
    struct MdChecker {
        const ParsedExpectation& expect;
        IceE2EResult& result;
        bool visited{false};

        void on_bundle_start(const impact::BundleStart&) {}
        void on_bundle_end(const impact::BundleEnd&) {}
        void on_snapshot_order(const impact::SnapshotOrder&) {}
        void on_price_level(const impact::PriceLevel&) {}
        void on_instrument_def(const impact::InstrumentDefinition&) {}

        void on_add_modify_order(const impact::AddModifyOrder& m) {
            visited = true;
            if (expect.event_type != "ICE_MD_ADD" &&
                expect.event_type != "ICE_MD_UPDATE") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "ICE_MD_ADD/UPDATE";
                return;
            }
            auto it_price = expect.fields.find("price");
            if (it_price != expect.fields.end()) {
                int64_t exp_p = std::stoll(it_price->second);
                if (m.price != exp_p) {
                    result.passed = false;
                    result.expected = "price=" + it_price->second;
                    result.actual = "price=" + std::to_string(m.price);
                }
            }
            auto it_qty = expect.fields.find("qty");
            if (it_qty != expect.fields.end()) {
                int64_t exp_q = std::stoll(it_qty->second);
                if (m.quantity != static_cast<uint32_t>(exp_q)) {
                    result.passed = false;
                    result.expected = "qty=" + it_qty->second;
                    result.actual = "qty=" + std::to_string(m.quantity);
                }
            }
        }

        void on_order_withdrawal(const impact::OrderWithdrawal&) {
            visited = true;
            if (expect.event_type != "ICE_MD_REMOVE") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "ICE_MD_REMOVE";
            }
        }

        void on_deal_trade(const impact::DealTrade& t) {
            visited = true;
            if (expect.event_type != "ICE_MD_TRADE") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "ICE_MD_TRADE";
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
                if (t.quantity != static_cast<uint32_t>(exp_q)) {
                    result.passed = false;
                    result.expected = "qty=" + it_qty->second;
                    result.actual = "qty=" + std::to_string(t.quantity);
                }
            }
        }

        void on_market_status(const impact::MarketStatus&) {
            visited = true;
            if (expect.event_type != "ICE_MD_STATUS") {
                result.passed = false;
                result.expected = expect.event_type;
                result.actual = "ICE_MD_STATUS";
            }
        }
    };

    MdChecker checker{expect, result};
    impact::decode_messages(pkt.bytes(), pkt.len, checker);

    if (!checker.visited && result.passed) {
        result.passed = false;
        result.actual = "<no decodable iMpact message>";
    }

    return result;
}

// ---------------------------------------------------------------------------
// Main run()
// ---------------------------------------------------------------------------

std::vector<IceE2EResult> IceE2ETestRunner::run(const Journal& journal) {
    std::vector<IceE2EResult> results;

    symbol_map_.clear();
    cl_to_exchange_id_.clear();

    // Build symbol map from ICE products.
    auto products = get_ice_products();
    for (const auto& p : products) {
        symbol_map_[p.symbol] = p.instrument_id;
    }

    // Create publishers.
    fix::IceFixExecPublisher exec_pub("ICE", "CLIENT", "");
    RecordingOrderListener recording_ol;
    CompositeOrderListener<fix::IceFixExecPublisher, RecordingOrderListener>
        composite_ol(&exec_pub, &recording_ol);

    impact::ImpactFeedPublisher md_pub;
    RecordingMdListener recording_ml;
    CompositeMdListener<impact::ImpactFeedPublisher, RecordingMdListener>
        composite_ml(&md_pub, &recording_ml);

    // Create simulator with composite listeners.
    using SimType = IceSimulator<
        CompositeOrderListener<fix::IceFixExecPublisher, RecordingOrderListener>,
        CompositeMdListener<impact::ImpactFeedPublisher, RecordingMdListener>>;
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
                    sim.start_trading_day(ts);
                    sim.open_market(ts);
                } else if (state_str == "PRE_OPEN") {
                    sim.start_trading_day(ts);
                }
                exec_pub.clear_messages();
                md_pub.clear();
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
            case ParsedAction::IceFixNewOrder: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);
                md_pub = impact::ImpactFeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<
                    impact::ImpactFeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);

                // Build and send FIX message through gateway to simulator.
                auto fix_msg = build_fix_new_order(action);
                auto ts = static_cast<Timestamp>(action.get_int("ts"));

                // Register order with exec publisher for context.
                auto cl_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
                auto side = parse_side(action.get_str("side"));
                auto price = action.get_int("price");
                auto qty = action.get_int("qty");

                // Use gateway to parse FIX and dispatch to engine.
                fix::IceFixGateway<SimType> gateway(sim);
                // The gateway takes a single engine, but IceSimulator routes
                // by instrument. We need to call the simulator directly instead.
                // Build OrderRequest from action fields.
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
                // We don't know the exchange-assigned ID yet; use cl_id as
                // placeholder. The accepted callback will give us the real ID.
                exec_pub.register_order(cl_id, cl_id, price, qty, side);

                sim.new_order(iid, req);

                // Track exchange-assigned IDs.
                for (const auto& evt : recording_ol.events()) {
                    auto* accepted = std::get_if<OrderAccepted>(&evt);
                    if (accepted) {
                        cl_to_exchange_id_[accepted->client_order_id] =
                            accepted->id;
                        // Re-register with correct exchange ID.
                        exec_pub.register_order(
                            accepted->id, accepted->client_order_id,
                            price, qty, side);
                    }
                }
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::IceFixCancel: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);
                md_pub = impact::ImpactFeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<
                    impact::ImpactFeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);

                auto orig = static_cast<uint64_t>(
                    action.get_int("orig_cl_ord_id"));
                auto ts = static_cast<Timestamp>(action.get_int("ts"));

                // Resolve to exchange ID.
                OrderId eid = 0;
                auto it = cl_to_exchange_id_.find(orig);
                if (it != cl_to_exchange_id_.end()) eid = it->second;

                sim.cancel_order(iid, eid, ts);
                recording_ol.clear();
                recording_ml.clear();
                break;
            }
            case ParsedAction::IceFixReplace: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);
                md_pub = impact::ImpactFeedPublisher(static_cast<int32_t>(iid));
                composite_ml = CompositeMdListener<
                    impact::ImpactFeedPublisher, RecordingMdListener>(
                    &md_pub, &recording_ml);

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
            case ParsedAction::IceFixMassCancel: {
                auto instr_sym = action.get_str("instrument");
                auto iid = resolve_instrument(instr_sym);
                auto ts = static_cast<Timestamp>(action.get_int("ts"));
                auto acct = static_cast<uint64_t>(action.get_int("account"));

                // Mass cancel via the simulator's FIFO or GTBPR engine.
                if (sim.is_gtbpr_instrument(iid)) {
                    auto* engine = sim.get_gtbpr_engine(iid);
                    if (engine) engine->mass_cancel(acct, ts);
                } else {
                    auto* engine = sim.get_fifo_engine(iid);
                    if (engine) engine->mass_cancel(acct, ts);
                }
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
            bool is_exec = (expect.event_type.substr(0, 8) == "ICE_EXEC");
            bool is_md = (expect.event_type.substr(0, 6) == "ICE_MD");

            IceE2EResult r{};
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

}  // namespace exchange::ice
