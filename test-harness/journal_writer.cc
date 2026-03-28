#include "test-harness/journal_writer.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

const char* side_to_journal(Side s) {
    return s == Side::Buy ? "BUY" : "SELL";
}

const char* depth_action_to_journal(DepthUpdate::Action a) {
    switch (a) {
        case DepthUpdate::Add:    return "ADD";
        case DepthUpdate::Update: return "UPDATE";
        case DepthUpdate::Remove: return "REMOVE";
    }
    return "UNKNOWN";
}

const char* book_action_to_journal(OrderBookAction::Action a) {
    switch (a) {
        case OrderBookAction::Add:    return "ADD";
        case OrderBookAction::Modify: return "MODIFY";
        case OrderBookAction::Cancel: return "CANCEL";
        case OrderBookAction::Fill:   return "FILL";
    }
    return "UNKNOWN";
}

const char* reject_reason_to_journal(RejectReason r) {
    switch (r) {
        case RejectReason::PoolExhausted:      return "POOL_EXHAUSTED";
        case RejectReason::InvalidPrice:       return "INVALID_PRICE";
        case RejectReason::InvalidQuantity:    return "INVALID_QUANTITY";
        case RejectReason::InvalidTif:         return "INVALID_TIF";
        case RejectReason::InvalidSide:        return "INVALID_SIDE";
        case RejectReason::UnknownOrder:       return "UNKNOWN_ORDER";
        case RejectReason::PriceBandViolation: return "PRICE_BAND_VIOLATION";
        case RejectReason::LevelPoolExhausted:   return "LEVEL_POOL_EXHAUSTED";
        case RejectReason::MaxOrderSizeExceeded: return "MAX_ORDER_SIZE_EXCEEDED";
        case RejectReason::RateThrottled:        return "RATE_THROTTLED";
        case RejectReason::LockLimitUp:          return "LOCK_LIMIT_UP";
        case RejectReason::LockLimitDown:          return "LOCK_LIMIT_DOWN";
        case RejectReason::PositionLimitExceeded: return "POSITION_LIMIT_EXCEEDED";
        case RejectReason::ExchangeSpecific:      return "EXCHANGE_SPECIFIC";
    }
    return "UNKNOWN";
}

const char* session_state_to_journal(SessionState s) {
    switch (s) {
        case SessionState::Closed:            return "CLOSED";
        case SessionState::PreOpen:           return "PRE_OPEN";
        case SessionState::OpeningAuction:    return "OPENING_AUCTION";
        case SessionState::Continuous:        return "CONTINUOUS";
        case SessionState::PreClose:          return "PRE_CLOSE";
        case SessionState::ClosingAuction:    return "CLOSING_AUCTION";
        case SessionState::Halt:              return "HALT";
        case SessionState::VolatilityAuction: return "VOLATILITY_AUCTION";
        case SessionState::LockLimit:         return "LOCK_LIMIT";
    }
    return "UNKNOWN";
}

const char* cancel_reason_to_journal(CancelReason r) {
    switch (r) {
        case CancelReason::UserRequested:       return "USER_REQUESTED";
        case CancelReason::IOCRemainder:        return "IOC_REMAINDER";
        case CancelReason::FOKFailed:           return "FOK_FAILED";
        case CancelReason::Expired:             return "EXPIRED";
        case CancelReason::SelfMatchPrevention: return "SELF_MATCH_PREVENTION";
        case CancelReason::LevelPoolExhausted:  return "LEVEL_POOL_EXHAUSTED";
        case CancelReason::MassCancelled:       return "MASS_CANCELLED";
    }
    return "UNKNOWN";
}

// Append " key=value" to a string stream.
template <typename T>
void kv(std::ostringstream& os, const char* key, const T& val) {
    os << ' ' << key << '=' << val;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string JournalWriter::config_to_config_lines(const ParsedConfig& config) {
    std::ostringstream os;
    os << "CONFIG";
    kv(os, "match_algo",      config.match_algo);
    kv(os, "tick_size",       config.tick_size);
    kv(os, "lot_size",        config.lot_size);
    kv(os, "max_orders",      config.max_orders);
    kv(os, "max_levels",      config.max_levels);
    kv(os, "max_order_ids",   config.max_order_ids);
    kv(os, "price_band_low",  config.price_band_low);
    kv(os, "price_band_high", config.price_band_high);
    return os.str();
}

std::string JournalWriter::action_to_action_line(const ParsedAction& action) {
    std::ostringstream os;
    os << "ACTION ";

    switch (action.type) {
        case ParsedAction::NewOrder:          os << "NEW_ORDER";          break;
        case ParsedAction::Cancel:            os << "CANCEL";             break;
        case ParsedAction::Modify:            os << "MODIFY";             break;
        case ParsedAction::TriggerExpiry:     os << "TRIGGER_EXPIRY";     break;
        case ParsedAction::SetSessionState:   os << "SET_SESSION_STATE";  break;
        case ParsedAction::ExecuteAuction:    os << "EXECUTE_AUCTION";    break;
        case ParsedAction::PublishIndicative: os << "PUBLISH_INDICATIVE"; break;
        case ParsedAction::MassCancel:        os << "MASS_CANCEL";        break;
        case ParsedAction::MassCancelAll:     os << "MASS_CANCEL_ALL";    break;
        case ParsedAction::RestoreOrder:     os << "RESTORE_ORDER";     break;
        case ParsedAction::BustTrade:        os << "BUST_TRADE";        break;
        case ParsedAction::ILink3NewOrder:    os << "ILINK3_NEW_ORDER";   break;
        case ParsedAction::ILink3Cancel:      os << "ILINK3_CANCEL";     break;
        case ParsedAction::ILink3Replace:     os << "ILINK3_REPLACE";    break;
        case ParsedAction::ILink3MassCancel:  os << "ILINK3_MASS_CANCEL"; break;
        case ParsedAction::IceFixNewOrder:    os << "ICE_FIX_NEW_ORDER";  break;
        case ParsedAction::IceFixCancel:      os << "ICE_FIX_CANCEL";    break;
        case ParsedAction::IceFixReplace:     os << "ICE_FIX_REPLACE";   break;
        case ParsedAction::IceFixMassCancel:  os << "ICE_FIX_MASS_CANCEL"; break;
        case ParsedAction::KrxFixNewOrder:    os << "KRX_FIX_NEW_ORDER";  break;
        case ParsedAction::KrxFixCancel:      os << "KRX_FIX_CANCEL";    break;
        case ParsedAction::KrxFixReplace:     os << "KRX_FIX_REPLACE";   break;
        case ParsedAction::SessionStart:      os << "SESSION_START";      break;
        case ParsedAction::SessionOpen:       os << "SESSION_OPEN";       break;
        case ParsedAction::SessionClose:      os << "SESSION_CLOSE";      break;
    }

    // Emit fields in a deterministic order based on action type, then any
    // remaining fields not covered by the ordered list.
    // This guarantees the parser can always round-trip the output.
    auto emit = [&](const char* key) {
        auto it = action.fields.find(key);
        if (it != action.fields.end()) {
            os << ' ' << key << '=' << it->second;
        }
    };

    switch (action.type) {
        case ParsedAction::NewOrder:
            emit("ts");
            emit("cl_ord_id");
            emit("account_id");
            emit("side");
            emit("price");
            emit("qty");
            emit("type");
            emit("tif");
            emit("stop_price");
            emit("gtd_expiry");
            break;

        case ParsedAction::Cancel:
            emit("ts");
            emit("ord_id");
            break;

        case ParsedAction::Modify:
            emit("ts");
            emit("ord_id");
            emit("cl_ord_id");
            emit("new_price");
            emit("new_qty");
            break;

        case ParsedAction::TriggerExpiry:
            emit("ts");
            emit("tif");
            break;

        case ParsedAction::SetSessionState:
            emit("ts");
            emit("state");
            break;

        case ParsedAction::ExecuteAuction:
            emit("ts");
            emit("reference_price");
            break;

        case ParsedAction::PublishIndicative:
            emit("ts");
            emit("reference_price");
            break;

        case ParsedAction::MassCancel:
            emit("ts");
            emit("account_id");
            break;

        case ParsedAction::MassCancelAll:
            emit("ts");
            break;

        case ParsedAction::RestoreOrder:
            emit("ts");
            emit("ord_id");
            emit("cl_ord_id");
            emit("account_id");
            emit("side");
            emit("price");
            emit("qty");
            emit("filled_qty");
            emit("remaining_qty");
            emit("type");
            emit("tif");
            emit("display_qty");
            emit("total_qty");
            break;

        case ParsedAction::BustTrade:
            emit("ts");
            emit("trade_id");
            break;

        case ParsedAction::ILink3NewOrder:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("account");
            emit("side");
            emit("price");
            emit("qty");
            emit("type");
            emit("tif");
            emit("display_qty");
            emit("stop_price");
            break;

        case ParsedAction::ILink3Cancel:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            break;

        case ParsedAction::ILink3Replace:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            emit("price");
            emit("qty");
            break;

        case ParsedAction::ILink3MassCancel:
            emit("ts");
            emit("instrument");
            emit("account");
            break;

        case ParsedAction::IceFixNewOrder:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("account");
            emit("side");
            emit("price");
            emit("qty");
            emit("type");
            emit("tif");
            emit("display_qty");
            emit("originator_user_id");
            emit("account_code");
            emit("memo");
            break;

        case ParsedAction::IceFixCancel:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            emit("side");
            break;

        case ParsedAction::IceFixReplace:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            emit("price");
            emit("qty");
            emit("side");
            break;

        case ParsedAction::IceFixMassCancel:
            emit("ts");
            emit("instrument");
            emit("account");
            break;

        case ParsedAction::KrxFixNewOrder:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("account");
            emit("side");
            emit("price");
            emit("qty");
            emit("type");
            emit("tif");
            emit("display_qty");
            emit("program_trading");
            emit("investor_type");
            emit("board_id");
            break;

        case ParsedAction::KrxFixCancel:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            emit("side");
            break;

        case ParsedAction::KrxFixReplace:
            emit("ts");
            emit("instrument");
            emit("cl_ord_id");
            emit("orig_cl_ord_id");
            emit("price");
            emit("qty");
            emit("side");
            break;

        case ParsedAction::SessionStart:
            emit("ts");
            emit("state");
            break;

        case ParsedAction::SessionOpen:
            emit("ts");
            break;

        case ParsedAction::SessionClose:
            emit("ts");
            break;
    }

    // Emit any extra fields not in the canonical order (preserves unknown fields).
    static const std::vector<std::string> known_new_order =
        {"ts", "cl_ord_id", "account_id", "side", "price", "qty",
         "type", "tif", "stop_price", "gtd_expiry"};
    static const std::vector<std::string> known_cancel =
        {"ts", "ord_id"};
    static const std::vector<std::string> known_modify =
        {"ts", "ord_id", "cl_ord_id", "new_price", "new_qty"};
    static const std::vector<std::string> known_trigger_expiry =
        {"ts", "tif"};
    static const std::vector<std::string> known_set_session_state =
        {"ts", "state"};
    static const std::vector<std::string> known_execute_auction =
        {"ts", "reference_price"};
    static const std::vector<std::string> known_publish_indicative =
        {"ts", "reference_price"};
    static const std::vector<std::string> known_mass_cancel =
        {"ts", "account_id"};
    static const std::vector<std::string> known_mass_cancel_all =
        {"ts"};
    static const std::vector<std::string> known_restore_order =
        {"ts", "ord_id", "cl_ord_id", "account_id", "side", "price", "qty",
         "filled_qty", "remaining_qty", "type", "tif", "display_qty", "total_qty"};
    static const std::vector<std::string> known_bust_trade =
        {"ts", "trade_id"};
    static const std::vector<std::string> known_ilink3_new_order =
        {"ts", "instrument", "cl_ord_id", "account", "side", "price", "qty",
         "type", "tif", "display_qty", "stop_price"};
    static const std::vector<std::string> known_ilink3_cancel =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id"};
    static const std::vector<std::string> known_ilink3_replace =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id", "price", "qty"};
    static const std::vector<std::string> known_ilink3_mass_cancel =
        {"ts", "instrument", "account"};
    static const std::vector<std::string> known_ice_fix_new_order =
        {"ts", "instrument", "cl_ord_id", "account", "side", "price", "qty",
         "type", "tif", "display_qty", "originator_user_id", "account_code", "memo"};
    static const std::vector<std::string> known_ice_fix_cancel =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id", "side"};
    static const std::vector<std::string> known_ice_fix_replace =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id", "price", "qty", "side"};
    static const std::vector<std::string> known_ice_fix_mass_cancel =
        {"ts", "instrument", "account"};
    static const std::vector<std::string> known_krx_fix_new_order =
        {"ts", "instrument", "cl_ord_id", "account", "side", "price", "qty",
         "type", "tif", "display_qty", "program_trading", "investor_type", "board_id"};
    static const std::vector<std::string> known_krx_fix_cancel =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id", "side"};
    static const std::vector<std::string> known_krx_fix_replace =
        {"ts", "instrument", "cl_ord_id", "orig_cl_ord_id", "price", "qty", "side"};
    static const std::vector<std::string> known_session_start =
        {"ts", "state"};
    static const std::vector<std::string> known_session_open =
        {"ts"};
    static const std::vector<std::string> known_session_close =
        {"ts"};

    const std::vector<std::string>* known = nullptr;
    switch (action.type) {
        case ParsedAction::NewOrder:          known = &known_new_order;          break;
        case ParsedAction::Cancel:            known = &known_cancel;             break;
        case ParsedAction::Modify:            known = &known_modify;             break;
        case ParsedAction::TriggerExpiry:     known = &known_trigger_expiry;     break;
        case ParsedAction::SetSessionState:   known = &known_set_session_state;  break;
        case ParsedAction::ExecuteAuction:    known = &known_execute_auction;    break;
        case ParsedAction::PublishIndicative: known = &known_publish_indicative; break;
        case ParsedAction::MassCancel:        known = &known_mass_cancel;        break;
        case ParsedAction::MassCancelAll:     known = &known_mass_cancel_all;    break;
        case ParsedAction::RestoreOrder:     known = &known_restore_order;      break;
        case ParsedAction::BustTrade:        known = &known_bust_trade;         break;
        case ParsedAction::ILink3NewOrder:    known = &known_ilink3_new_order;   break;
        case ParsedAction::ILink3Cancel:      known = &known_ilink3_cancel;      break;
        case ParsedAction::ILink3Replace:     known = &known_ilink3_replace;     break;
        case ParsedAction::ILink3MassCancel:  known = &known_ilink3_mass_cancel; break;
        case ParsedAction::IceFixNewOrder:    known = &known_ice_fix_new_order;  break;
        case ParsedAction::IceFixCancel:      known = &known_ice_fix_cancel;     break;
        case ParsedAction::IceFixReplace:     known = &known_ice_fix_replace;    break;
        case ParsedAction::IceFixMassCancel:  known = &known_ice_fix_mass_cancel; break;
        case ParsedAction::KrxFixNewOrder:    known = &known_krx_fix_new_order;  break;
        case ParsedAction::KrxFixCancel:      known = &known_krx_fix_cancel;     break;
        case ParsedAction::KrxFixReplace:     known = &known_krx_fix_replace;    break;
        case ParsedAction::SessionStart:      known = &known_session_start;      break;
        case ParsedAction::SessionOpen:       known = &known_session_open;       break;
        case ParsedAction::SessionClose:      known = &known_session_close;      break;
    }

    for (const auto& kv_pair : action.fields) {
        bool is_known = false;
        for (const auto& k : *known) {
            if (k == kv_pair.first) { is_known = true; break; }
        }
        if (!is_known) {
            os << ' ' << kv_pair.first << '=' << kv_pair.second;
        }
    }

    return os.str();
}

std::string JournalWriter::event_to_expect_line(const RecordedEvent& event) {
    return std::visit([](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        std::ostringstream os;

        if constexpr (std::is_same_v<T, OrderAccepted>) {
            os << "EXPECT ORDER_ACCEPTED";
            kv(os, "ord_id",    e.id);
            kv(os, "cl_ord_id", e.client_order_id);
            kv(os, "ts",        e.ts);

        } else if constexpr (std::is_same_v<T, OrderRejected>) {
            os << "EXPECT ORDER_REJECTED";
            kv(os, "cl_ord_id", e.client_order_id);
            kv(os, "ts",        e.ts);
            kv(os, "reason",    reject_reason_to_journal(e.reason));

        } else if constexpr (std::is_same_v<T, OrderFilled>) {
            os << "EXPECT ORDER_FILLED";
            kv(os, "aggressor", e.aggressor_id);
            kv(os, "resting",   e.resting_id);
            kv(os, "price",     e.price);
            kv(os, "qty",       e.quantity);
            kv(os, "ts",        e.ts);

        } else if constexpr (std::is_same_v<T, OrderPartiallyFilled>) {
            os << "EXPECT ORDER_PARTIALLY_FILLED";
            kv(os, "aggressor",     e.aggressor_id);
            kv(os, "resting",       e.resting_id);
            kv(os, "price",         e.price);
            kv(os, "qty",           e.quantity);
            kv(os, "aggressor_rem", e.aggressor_remaining);
            kv(os, "resting_rem",   e.resting_remaining);
            kv(os, "ts",            e.ts);

        } else if constexpr (std::is_same_v<T, OrderCancelled>) {
            os << "EXPECT ORDER_CANCELLED";
            kv(os, "ord_id", e.id);
            kv(os, "ts",     e.ts);
            kv(os, "reason", cancel_reason_to_journal(e.reason));

        } else if constexpr (std::is_same_v<T, OrderCancelRejected>) {
            os << "EXPECT ORDER_CANCEL_REJECTED";
            kv(os, "ord_id",    e.id);
            kv(os, "cl_ord_id", e.client_order_id);
            kv(os, "ts",        e.ts);
            kv(os, "reason",    reject_reason_to_journal(e.reason));

        } else if constexpr (std::is_same_v<T, OrderModified>) {
            os << "EXPECT ORDER_MODIFIED";
            kv(os, "ord_id",    e.id);
            kv(os, "cl_ord_id", e.client_order_id);
            kv(os, "new_price", e.new_price);
            kv(os, "new_qty",   e.new_qty);
            kv(os, "ts",        e.ts);

        } else if constexpr (std::is_same_v<T, OrderModifyRejected>) {
            os << "EXPECT ORDER_MODIFY_REJECTED";
            kv(os, "ord_id",    e.id);
            kv(os, "cl_ord_id", e.client_order_id);
            kv(os, "ts",        e.ts);
            kv(os, "reason",    reject_reason_to_journal(e.reason));

        } else if constexpr (std::is_same_v<T, TopOfBook>) {
            os << "EXPECT TOP_OF_BOOK";
            kv(os, "bid",     e.best_bid);
            kv(os, "bid_qty", e.bid_qty);
            kv(os, "ask",     e.best_ask);
            kv(os, "ask_qty", e.ask_qty);
            kv(os, "ts",      e.ts);

        } else if constexpr (std::is_same_v<T, DepthUpdate>) {
            os << "EXPECT DEPTH_UPDATE";
            kv(os, "side",   side_to_journal(e.side));
            kv(os, "price",  e.price);
            kv(os, "qty",    e.total_qty);
            kv(os, "count",  e.order_count);
            kv(os, "action", depth_action_to_journal(e.action));
            kv(os, "ts",     e.ts);

        } else if constexpr (std::is_same_v<T, OrderBookAction>) {
            os << "EXPECT ORDER_BOOK_ACTION";
            kv(os, "ord_id", e.id);
            kv(os, "side",   side_to_journal(e.side));
            kv(os, "price",  e.price);
            kv(os, "qty",    e.qty);
            kv(os, "action", book_action_to_journal(e.action));
            kv(os, "ts",     e.ts);

        } else if constexpr (std::is_same_v<T, Trade>) {
            os << "EXPECT TRADE";
            kv(os, "price",         e.price);
            kv(os, "qty",           e.quantity);
            kv(os, "aggressor",     e.aggressor_id);
            kv(os, "resting",       e.resting_id);
            kv(os, "aggressor_side", side_to_journal(e.aggressor_side));
            kv(os, "ts",            e.ts);

        } else if constexpr (std::is_same_v<T, MarketStatus>) {
            os << "EXPECT MARKET_STATUS";
            kv(os, "state", session_state_to_journal(e.state));
            kv(os, "ts",    e.ts);

        } else if constexpr (std::is_same_v<T, IndicativePrice>) {
            os << "EXPECT INDICATIVE_PRICE";
            kv(os, "price",       e.price);
            kv(os, "matched_vol", e.matched_volume);
            kv(os, "buy_surplus", e.buy_surplus);
            kv(os, "sell_surplus", e.sell_surplus);
            kv(os, "ts",          e.ts);
        }

        return os.str();
    }, event);
}

std::string JournalWriter::to_string(const ParsedConfig& config,
                                     const std::vector<JournalEntry>& entries) {
    std::ostringstream os;

    os << config_to_config_lines(config) << '\n';

    for (const JournalEntry& entry : entries) {
        os << '\n';
        os << action_to_action_line(entry.action) << '\n';
        for (const ParsedExpectation& exp : entry.expectations) {
            // Re-serialize a ParsedExpectation back to an EXPECT line.
            os << "EXPECT " << exp.event_type;
            for (const auto& kv_pair : exp.fields) {
                os << ' ' << kv_pair.first << '=' << kv_pair.second;
            }
            os << '\n';
        }
    }

    return os.str();
}

void JournalWriter::write(const std::string& path,
                          const ParsedConfig& config,
                          const std::vector<JournalEntry>& entries) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "JournalWriter: cannot open file for writing '" + path + "'");
    }
    file << to_string(config, entries);
    if (!file) {
        throw std::runtime_error(
            "JournalWriter: write error for file '" + path + "'");
    }
}

}  // namespace exchange
