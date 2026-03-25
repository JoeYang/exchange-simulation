#include "test-harness/test_runner.h"

#include "test-harness/recorded_event.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Parse "BUY" / "SELL" from a journal field value.
Side parse_side(const std::string& s) {
    if (s == "BUY")  return Side::Buy;
    if (s == "SELL") return Side::Sell;
    throw std::runtime_error("test_runner: unknown side '" + s + "'");
}

// Parse order type string from journal.
OrderType parse_order_type(const std::string& s) {
    if (s == "LIMIT")      return OrderType::Limit;
    if (s == "MARKET")     return OrderType::Market;
    if (s == "STOP")       return OrderType::Stop;
    if (s == "STOP_LIMIT") return OrderType::StopLimit;
    throw std::runtime_error("test_runner: unknown order type '" + s + "'");
}

// Parse TIF string from journal.
TimeInForce parse_tif(const std::string& s) {
    if (s == "DAY") return TimeInForce::DAY;
    if (s == "GTC") return TimeInForce::GTC;
    if (s == "IOC") return TimeInForce::IOC;
    if (s == "FOK") return TimeInForce::FOK;
    if (s == "GTD") return TimeInForce::GTD;
    throw std::runtime_error("test_runner: unknown TIF '" + s + "'");
}

// Parse RejectReason string (uppercase with underscores, as written by
// JournalWriter::reject_reason_to_journal).
RejectReason parse_reject_reason(const std::string& s) {
    if (s == "POOL_EXHAUSTED")       return RejectReason::PoolExhausted;
    if (s == "INVALID_PRICE")        return RejectReason::InvalidPrice;
    if (s == "INVALID_QUANTITY")     return RejectReason::InvalidQuantity;
    if (s == "INVALID_TIF")          return RejectReason::InvalidTif;
    if (s == "INVALID_SIDE")         return RejectReason::InvalidSide;
    if (s == "UNKNOWN_ORDER")        return RejectReason::UnknownOrder;
    if (s == "PRICE_BAND_VIOLATION") return RejectReason::PriceBandViolation;
    if (s == "LEVEL_POOL_EXHAUSTED") return RejectReason::LevelPoolExhausted;
    if (s == "EXCHANGE_SPECIFIC")    return RejectReason::ExchangeSpecific;
    throw std::runtime_error("test_runner: unknown reject reason '" + s + "'");
}

// Parse CancelReason string.
CancelReason parse_cancel_reason(const std::string& s) {
    if (s == "USER_REQUESTED")       return CancelReason::UserRequested;
    if (s == "IOC_REMAINDER")        return CancelReason::IOCRemainder;
    if (s == "FOK_FAILED")           return CancelReason::FOKFailed;
    if (s == "EXPIRED")              return CancelReason::Expired;
    if (s == "SELF_MATCH_PREVENTION") return CancelReason::SelfMatchPrevention;
    if (s == "LEVEL_POOL_EXHAUSTED") return CancelReason::LevelPoolExhausted;
    throw std::runtime_error("test_runner: unknown cancel reason '" + s + "'");
}

// Parse SessionState string from journal field value.
SessionState parse_session_state(const std::string& s) {
    if (s == "CLOSED")             return SessionState::Closed;
    if (s == "PRE_OPEN")           return SessionState::PreOpen;
    if (s == "OPENING_AUCTION")    return SessionState::OpeningAuction;
    if (s == "CONTINUOUS")         return SessionState::Continuous;
    if (s == "PRE_CLOSE")          return SessionState::PreClose;
    if (s == "CLOSING_AUCTION")    return SessionState::ClosingAuction;
    if (s == "HALT")               return SessionState::Halt;
    if (s == "VOLATILITY_AUCTION") return SessionState::VolatilityAuction;
    throw std::runtime_error("test_runner: unknown session state '" + s + "'");
}

// Parse DepthUpdate::Action string.
DepthUpdate::Action parse_depth_action(const std::string& s) {
    if (s == "ADD")    return DepthUpdate::Add;
    if (s == "UPDATE") return DepthUpdate::Update;
    if (s == "REMOVE") return DepthUpdate::Remove;
    throw std::runtime_error("test_runner: unknown depth action '" + s + "'");
}

// Parse OrderBookAction::Action string.
OrderBookAction::Action parse_book_action(const std::string& s) {
    if (s == "ADD")    return OrderBookAction::Add;
    if (s == "MODIFY") return OrderBookAction::Modify;
    if (s == "CANCEL") return OrderBookAction::Cancel;
    if (s == "FILL")   return OrderBookAction::Fill;
    throw std::runtime_error("test_runner: unknown book action '" + s + "'");
}

// Helper: get field or return a default int64 value.
int64_t get_int_or(const ParsedExpectation& e, const std::string& key,
                   int64_t def) {
    auto it = e.fields.find(key);
    if (it == e.fields.end()) return def;
    return std::stoll(it->second);
}

// Build a passing TestResult.
TestResult make_pass() {
    return TestResult{true, 0, 0, "", "", ""};
}

// Build a failing TestResult for a per-event mismatch.
TestResult make_event_mismatch(size_t action_index, size_t event_index,
                               const std::string& exp_str,
                               const std::string& act_str) {
    std::string diff = "action[" + std::to_string(action_index) +
                       "] event[" + std::to_string(event_index) +
                       "]: mismatch\n  expected: " + exp_str +
                       "\n  actual:   " + act_str;
    return TestResult{false, action_index, event_index, exp_str, act_str, diff};
}

}  // namespace

// ---------------------------------------------------------------------------
// execute_action
// ---------------------------------------------------------------------------

template <typename EngineT>
void JournalTestRunner::execute_action(EngineT& engine,
                                       const ParsedAction& action) {
    switch (action.type) {

    case ParsedAction::NewOrder: {
        OrderRequest req{};
        req.timestamp       = static_cast<Timestamp>(action.get_int("ts"));
        req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));

        // account_id is optional (defaults to 0).
        {
            auto it = action.fields.find("account_id");
            req.account_id = (it != action.fields.end())
                                 ? static_cast<uint64_t>(std::stoll(it->second))
                                 : 0;
        }

        req.side     = parse_side(action.get_str("side"));
        req.type     = parse_order_type(action.get_str("type"));
        req.quantity = static_cast<Quantity>(action.get_int("qty"));

        // price is optional for Market orders (default 0).
        {
            auto it = action.fields.find("price");
            req.price = (it != action.fields.end())
                            ? static_cast<Price>(std::stoll(it->second))
                            : 0;
        }

        // tif is optional (defaults to GTC).
        {
            auto it = action.fields.find("tif");
            req.tif = (it != action.fields.end())
                          ? parse_tif(it->second)
                          : TimeInForce::GTC;
        }

        // stop_price is optional (default 0).
        {
            auto it = action.fields.find("stop_price");
            req.stop_price = (it != action.fields.end())
                                 ? static_cast<Price>(std::stoll(it->second))
                                 : 0;
        }

        // gtd_expiry is optional (default 0).
        {
            auto it = action.fields.find("gtd_expiry");
            req.gtd_expiry = (it != action.fields.end())
                                 ? static_cast<Timestamp>(std::stoll(it->second))
                                 : 0;
        }

        // display_qty is optional (default 0 = fully visible, no iceberg).
        {
            auto it = action.fields.find("display_qty");
            req.display_qty = (it != action.fields.end())
                                  ? static_cast<Quantity>(std::stoll(it->second))
                                  : 0;
        }

        engine.new_order(req);
        break;
    }

    case ParsedAction::Cancel: {
        OrderId   id = static_cast<OrderId>(action.get_int("ord_id"));
        Timestamp ts = static_cast<Timestamp>(action.get_int("ts"));
        engine.cancel_order(id, ts);
        break;
    }

    case ParsedAction::Modify: {
        ModifyRequest req{};
        req.order_id        = static_cast<OrderId>(action.get_int("ord_id"));
        req.client_order_id = static_cast<uint64_t>(action.get_int("cl_ord_id"));
        req.new_price       = static_cast<Price>(action.get_int("new_price"));
        req.new_quantity    = static_cast<Quantity>(action.get_int("new_qty"));
        req.timestamp       = static_cast<Timestamp>(action.get_int("ts"));
        engine.modify_order(req);
        break;
    }

    case ParsedAction::TriggerExpiry: {
        Timestamp   ts  = static_cast<Timestamp>(action.get_int("ts"));
        TimeInForce tif = parse_tif(action.get_str("tif"));
        engine.trigger_expiry(ts, tif);
        break;
    }

    case ParsedAction::SetSessionState: {
        Timestamp    ts    = static_cast<Timestamp>(action.get_int("ts"));
        SessionState state = parse_session_state(action.get_str("state"));
        engine.set_session_state(state, ts);
        break;
    }

    case ParsedAction::ExecuteAuction: {
        Timestamp ts  = static_cast<Timestamp>(action.get_int("ts"));
        Price     ref = static_cast<Price>(action.get_int("reference_price"));
        engine.execute_auction(ref, ts);
        break;
    }

    case ParsedAction::PublishIndicative: {
        Timestamp ts  = static_cast<Timestamp>(action.get_int("ts"));
        Price     ref = static_cast<Price>(action.get_int("reference_price"));
        engine.publish_indicative_price(ref, ts);
        break;
    }

    }  // switch
}

// Explicit instantiations so the linker can find these from test_runner.cc.
template void JournalTestRunner::execute_action<FifoExchange>(
    FifoExchange&, const ParsedAction&);
template void JournalTestRunner::execute_action<ProRataExchange>(
    ProRataExchange&, const ParsedAction&);

// ---------------------------------------------------------------------------
// expectation_to_event
// ---------------------------------------------------------------------------

RecordedEvent JournalTestRunner::expectation_to_event(
        const ParsedExpectation& e) {
    const std::string& t = e.event_type;

    if (t == "ORDER_ACCEPTED") {
        return OrderAccepted{
            static_cast<OrderId>(e.get_int("ord_id")),
            static_cast<uint64_t>(e.get_int("cl_ord_id")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "ORDER_REJECTED") {
        return OrderRejected{
            static_cast<uint64_t>(e.get_int("cl_ord_id")),
            static_cast<Timestamp>(e.get_int("ts")),
            parse_reject_reason(e.get_str("reason"))
        };
    }
    if (t == "ORDER_FILLED") {
        return OrderFilled{
            static_cast<OrderId>(e.get_int("aggressor")),
            static_cast<OrderId>(e.get_int("resting")),
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("qty")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "ORDER_PARTIALLY_FILLED") {
        return OrderPartiallyFilled{
            static_cast<OrderId>(e.get_int("aggressor")),
            static_cast<OrderId>(e.get_int("resting")),
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("qty")),
            static_cast<Quantity>(e.get_int("aggressor_rem")),
            static_cast<Quantity>(e.get_int("resting_rem")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "ORDER_CANCELLED") {
        return OrderCancelled{
            static_cast<OrderId>(e.get_int("ord_id")),
            static_cast<Timestamp>(e.get_int("ts")),
            parse_cancel_reason(e.get_str("reason"))
        };
    }
    if (t == "ORDER_CANCEL_REJECTED") {
        return OrderCancelRejected{
            static_cast<OrderId>(e.get_int("ord_id")),
            static_cast<uint64_t>(get_int_or(e, "cl_ord_id", 0)),
            static_cast<Timestamp>(e.get_int("ts")),
            parse_reject_reason(e.get_str("reason"))
        };
    }
    if (t == "ORDER_MODIFIED") {
        return OrderModified{
            static_cast<OrderId>(e.get_int("ord_id")),
            static_cast<uint64_t>(e.get_int("cl_ord_id")),
            static_cast<Price>(e.get_int("new_price")),
            static_cast<Quantity>(e.get_int("new_qty")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "ORDER_MODIFY_REJECTED") {
        return OrderModifyRejected{
            static_cast<OrderId>(e.get_int("ord_id")),
            static_cast<uint64_t>(e.get_int("cl_ord_id")),
            static_cast<Timestamp>(e.get_int("ts")),
            parse_reject_reason(e.get_str("reason"))
        };
    }
    if (t == "TOP_OF_BOOK") {
        return TopOfBook{
            static_cast<Price>(e.get_int("bid")),
            static_cast<Quantity>(e.get_int("bid_qty")),
            static_cast<Price>(e.get_int("ask")),
            static_cast<Quantity>(e.get_int("ask_qty")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "DEPTH_UPDATE") {
        return DepthUpdate{
            parse_side(e.get_str("side")),
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("qty")),
            static_cast<uint32_t>(e.get_int("count")),
            parse_depth_action(e.get_str("action")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "ORDER_BOOK_ACTION") {
        return OrderBookAction{
            static_cast<OrderId>(e.get_int("ord_id")),
            parse_side(e.get_str("side")),
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("qty")),
            parse_book_action(e.get_str("action")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "TRADE") {
        return Trade{
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("qty")),
            static_cast<OrderId>(e.get_int("aggressor")),
            static_cast<OrderId>(e.get_int("resting")),
            parse_side(e.get_str("aggressor_side")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "MARKET_STATUS") {
        return MarketStatus{
            parse_session_state(e.get_str("state")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }
    if (t == "INDICATIVE_PRICE") {
        return IndicativePrice{
            static_cast<Price>(e.get_int("price")),
            static_cast<Quantity>(e.get_int("matched_vol")),
            static_cast<Quantity>(e.get_int("buy_surplus")),
            static_cast<Quantity>(e.get_int("sell_surplus")),
            static_cast<Timestamp>(e.get_int("ts"))
        };
    }

    throw std::runtime_error(
        "test_runner: unknown EXPECT event type '" + t + "'");
}

// ---------------------------------------------------------------------------
// compare
// ---------------------------------------------------------------------------

TestResult JournalTestRunner::compare(
        const std::vector<RecordedEvent>& recorded,
        const std::vector<RecordedEvent>& expected,
        size_t action_index) {

    if (recorded.size() != expected.size()) {
        // Report size mismatch.
        // Show the first missing or extra event as the actual/expected strings.
        size_t min_size = std::min(recorded.size(), expected.size());
        if (min_size < expected.size()) {
            // More expected than recorded.
            std::string exp_str = to_string(expected[min_size]);
            std::string diff =
                "action[" + std::to_string(action_index) +
                "]: expected " + std::to_string(expected.size()) +
                " event(s), got " + std::to_string(recorded.size()) +
                "; first missing: " + exp_str;
            return TestResult{false, action_index, min_size,
                              exp_str, "<missing>", diff};
        } else {
            // More recorded than expected.
            std::string act_str = to_string(recorded[min_size]);
            std::string diff =
                "action[" + std::to_string(action_index) +
                "]: expected " + std::to_string(expected.size()) +
                " event(s), got " + std::to_string(recorded.size()) +
                "; first extra: " + act_str;
            return TestResult{false, action_index, min_size,
                              "<none>", act_str, diff};
        }
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        if (!(recorded[i] == expected[i])) {
            return make_event_mismatch(action_index, i,
                                       to_string(expected[i]),
                                       to_string(recorded[i]));
        }
    }

    return make_pass();
}

// ---------------------------------------------------------------------------
// run_impl
// ---------------------------------------------------------------------------

template <typename EngineT>
TestResult JournalTestRunner::run_impl(
        EngineT& engine,
        RecordingOrderListener& order_listener,
        RecordingMdListener& md_listener,
        const Journal& journal) {

    for (size_t i = 0; i < journal.entries.size(); ++i) {
        const JournalEntry& entry = journal.entries[i];

        // Clear listener state before executing this action so we only
        // capture the events from this one action.
        order_listener.clear();
        md_listener.clear();

        // Execute the action.
        execute_action(engine, entry.action);

        // Collect all recorded events in order: order events first,
        // then market data events.  The writer emits them interleaved
        // (per the engine's actual callback sequence), but the journal
        // format records them per-action, so we merge both listeners
        // into the combined ordering that the engine produced.
        //
        // The engine fires callbacks synchronously, but the two listeners
        // record them independently.  To recover the original interleaved
        // sequence we rely on the fact that the journal writer (which
        // produced the EXPECT lines) wrote them in the same merged order.
        //
        // We use the same merge strategy as the writer: collect all events
        // from both listeners into a single vector by replaying the action
        // against a merged listener.  Because we cannot replay without
        // resetting the engine, we instead match the EXPECT lines against
        // the combined (order + md) event vectors concatenated — which is
        // the convention the journal writer follows when no interleaving
        // information is preserved.
        //
        // Specifically, the journal writer calls event_to_expect_line() for
        // each RecordedEvent in a single pass over the combined vector.
        // The combined vector is produced by appending md_listener.events()
        // after order_listener.events().  We do the same here.

        std::vector<RecordedEvent> recorded;
        recorded.reserve(order_listener.size() + md_listener.size());
        for (const RecordedEvent& ev : order_listener.events()) {
            recorded.push_back(ev);
        }
        for (const RecordedEvent& ev : md_listener.events()) {
            recorded.push_back(ev);
        }

        // Convert expectations to RecordedEvent values.
        std::vector<RecordedEvent> expected;
        expected.reserve(entry.expectations.size());
        for (const ParsedExpectation& exp : entry.expectations) {
            expected.push_back(expectation_to_event(exp));
        }

        // Compare.
        TestResult result = compare(recorded, expected, i);
        if (!result.passed) {
            return result;
        }
    }

    return make_pass();
}

// Explicit instantiations.
template TestResult JournalTestRunner::run_impl<FifoExchange>(
    FifoExchange&, RecordingOrderListener&, RecordingMdListener&,
    const Journal&);
template TestResult JournalTestRunner::run_impl<ProRataExchange>(
    ProRataExchange&, RecordingOrderListener&, RecordingMdListener&,
    const Journal&);

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

TestResult JournalTestRunner::run_fifo(const Journal& journal) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    EngineConfig cfg{
        journal.config.tick_size,
        journal.config.lot_size,
        journal.config.price_band_low,
        journal.config.price_band_high
    };

    FifoExchange engine(cfg, order_listener, md_listener);
    return run_impl(engine, order_listener, md_listener, journal);
}

TestResult JournalTestRunner::run_pro_rata(const Journal& journal) {
    RecordingOrderListener order_listener;
    RecordingMdListener    md_listener;

    EngineConfig cfg{
        journal.config.tick_size,
        journal.config.lot_size,
        journal.config.price_band_low,
        journal.config.price_band_high
    };

    ProRataExchange engine(cfg, order_listener, md_listener);
    return run_impl(engine, order_listener, md_listener, journal);
}

}  // namespace exchange
