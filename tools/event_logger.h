#pragma once

#include "exchange-core/events.h"
#include "exchange-core/listeners.h"
#include "exchange-core/types.h"

#include <cstdint>
#include <ostream>

namespace exchange {

// EventLogger -- structured event log for post-trade analysis.
//
// Logs every engine callback as a single line in either JSON or CSV format.
// Designed for offline replay, audit trails, and analytics pipelines.
//
// Usage:
//   std::ofstream file("events.jsonl");
//   EventLogger logger(file, EventLogger::Format::JSON);
//   // ... wire as listener to the matching engine ...
//   // Each callback writes one line to the stream.
class EventLogger : public OrderListenerBase, public MarketDataListenerBase {
public:
    enum class Format { JSON, CSV };

    explicit EventLogger(std::ostream& out, Format fmt = Format::JSON)
        : out_(out), fmt_(fmt) {}

    // --- OrderListenerBase overrides ---

    void on_order_accepted(const OrderAccepted& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderAccepted","order_id":)" << e.id
                 << R"(,"client_order_id":)" << e.client_order_id
                 << "}\n";
        } else {
            out_ << e.ts << ",OrderAccepted," << e.id << ',' << e.client_order_id << '\n';
        }
    }

    void on_order_rejected(const OrderRejected& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderRejected","client_order_id":)" << e.client_order_id
                 << R"(,"reason":")" << reject_reason_str(e.reason)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",OrderRejected," << e.client_order_id
                 << ',' << reject_reason_str(e.reason) << '\n';
        }
    }

    void on_order_filled(const OrderFilled& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderFilled","aggressor_id":)" << e.aggressor_id
                 << R"(,"resting_id":)" << e.resting_id
                 << R"(,"price":)" << e.price
                 << R"(,"qty":)" << e.quantity
                 << "}\n";
        } else {
            out_ << e.ts << ",OrderFilled," << e.aggressor_id << ',' << e.resting_id
                 << ',' << e.price << ',' << e.quantity << '\n';
        }
    }

    void on_order_partially_filled(const OrderPartiallyFilled& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderPartiallyFilled","aggressor_id":)" << e.aggressor_id
                 << R"(,"resting_id":)" << e.resting_id
                 << R"(,"price":)" << e.price
                 << R"(,"qty":)" << e.quantity
                 << R"(,"aggressor_remaining":)" << e.aggressor_remaining
                 << R"(,"resting_remaining":)" << e.resting_remaining
                 << "}\n";
        } else {
            out_ << e.ts << ",OrderPartiallyFilled," << e.aggressor_id << ',' << e.resting_id
                 << ',' << e.price << ',' << e.quantity
                 << ',' << e.aggressor_remaining << ',' << e.resting_remaining << '\n';
        }
    }

    void on_order_cancelled(const OrderCancelled& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderCancelled","order_id":)" << e.id
                 << R"(,"reason":")" << cancel_reason_str(e.reason)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",OrderCancelled," << e.id
                 << ',' << cancel_reason_str(e.reason) << '\n';
        }
    }

    void on_order_cancel_rejected(const OrderCancelRejected& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderCancelRejected","order_id":)" << e.id
                 << R"(,"client_order_id":)" << e.client_order_id
                 << R"(,"reason":")" << reject_reason_str(e.reason)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",OrderCancelRejected," << e.id << ',' << e.client_order_id
                 << ',' << reject_reason_str(e.reason) << '\n';
        }
    }

    void on_order_modified(const OrderModified& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderModified","order_id":)" << e.id
                 << R"(,"client_order_id":)" << e.client_order_id
                 << R"(,"new_price":)" << e.new_price
                 << R"(,"new_qty":)" << e.new_qty
                 << "}\n";
        } else {
            out_ << e.ts << ",OrderModified," << e.id << ',' << e.client_order_id
                 << ',' << e.new_price << ',' << e.new_qty << '\n';
        }
    }

    void on_order_modify_rejected(const OrderModifyRejected& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"OrderModifyRejected","order_id":)" << e.id
                 << R"(,"client_order_id":)" << e.client_order_id
                 << R"(,"reason":")" << reject_reason_str(e.reason)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",OrderModifyRejected," << e.id << ',' << e.client_order_id
                 << ',' << reject_reason_str(e.reason) << '\n';
        }
    }

    // --- MarketDataListenerBase overrides ---

    void on_trade(const Trade& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"Trade","price":)" << e.price
                 << R"(,"qty":)" << e.quantity
                 << R"(,"aggressor_id":)" << e.aggressor_id
                 << R"(,"resting_id":)" << e.resting_id
                 << R"(,"aggressor_side":")" << side_str(e.aggressor_side)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",Trade," << e.price << ',' << e.quantity
                 << ',' << e.aggressor_id << ',' << e.resting_id
                 << ',' << side_str(e.aggressor_side) << '\n';
        }
    }

    void on_top_of_book(const TopOfBook& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"TopOfBook","best_bid":)" << e.best_bid
                 << R"(,"bid_qty":)" << e.bid_qty
                 << R"(,"best_ask":)" << e.best_ask
                 << R"(,"ask_qty":)" << e.ask_qty
                 << "}\n";
        } else {
            out_ << e.ts << ",TopOfBook," << e.best_bid << ',' << e.bid_qty
                 << ',' << e.best_ask << ',' << e.ask_qty << '\n';
        }
    }

    void on_depth_update(const DepthUpdate& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"DepthUpdate","side":")" << side_str(e.side)
                 << R"(","price":)" << e.price
                 << R"(,"total_qty":)" << e.total_qty
                 << R"(,"order_count":)" << e.order_count
                 << R"(,"action":")" << depth_action_str(e.action)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",DepthUpdate," << side_str(e.side)
                 << ',' << e.price << ',' << e.total_qty << ',' << e.order_count
                 << ',' << depth_action_str(e.action) << '\n';
        }
    }

    void on_market_status(const MarketStatus& e) {
        ++count_;
        if (fmt_ == Format::JSON) {
            out_ << R"({"ts":)" << e.ts
                 << R"(,"type":"MarketStatus","state":")" << session_state_str(e.state)
                 << "\"}\n";
        } else {
            out_ << e.ts << ",MarketStatus," << session_state_str(e.state) << '\n';
        }
    }

    uint64_t event_count() const { return count_; }

private:
    static const char* side_str(Side s) {
        switch (s) {
            case Side::Buy:  return "Buy";
            case Side::Sell: return "Sell";
        }
        return "Unknown";
    }

    static const char* reject_reason_str(RejectReason r) {
        switch (r) {
            case RejectReason::PoolExhausted:       return "PoolExhausted";
            case RejectReason::InvalidPrice:        return "InvalidPrice";
            case RejectReason::InvalidQuantity:     return "InvalidQuantity";
            case RejectReason::InvalidTif:          return "InvalidTif";
            case RejectReason::InvalidSide:         return "InvalidSide";
            case RejectReason::UnknownOrder:        return "UnknownOrder";
            case RejectReason::PriceBandViolation:  return "PriceBandViolation";
            case RejectReason::LevelPoolExhausted:  return "LevelPoolExhausted";
            case RejectReason::MaxOrderSizeExceeded: return "MaxOrderSizeExceeded";
            case RejectReason::ExchangeSpecific:    return "ExchangeSpecific";
        }
        return "Unknown";
    }

    static const char* cancel_reason_str(CancelReason r) {
        switch (r) {
            case CancelReason::UserRequested:         return "UserRequested";
            case CancelReason::IOCRemainder:          return "IOCRemainder";
            case CancelReason::FOKFailed:             return "FOKFailed";
            case CancelReason::Expired:               return "Expired";
            case CancelReason::SelfMatchPrevention:   return "SelfMatchPrevention";
            case CancelReason::LevelPoolExhausted:    return "LevelPoolExhausted";
            case CancelReason::MassCancelled:         return "MassCancelled";
        }
        return "Unknown";
    }

    static const char* session_state_str(SessionState s) {
        switch (s) {
            case SessionState::Closed:            return "Closed";
            case SessionState::PreOpen:           return "PreOpen";
            case SessionState::OpeningAuction:    return "OpeningAuction";
            case SessionState::Continuous:        return "Continuous";
            case SessionState::PreClose:          return "PreClose";
            case SessionState::ClosingAuction:    return "ClosingAuction";
            case SessionState::Halt:              return "Halt";
            case SessionState::VolatilityAuction: return "VolatilityAuction";
        }
        return "Unknown";
    }

    static const char* depth_action_str(DepthUpdate::Action a) {
        switch (a) {
            case DepthUpdate::Add:    return "Add";
            case DepthUpdate::Update: return "Update";
            case DepthUpdate::Remove: return "Remove";
        }
        return "Unknown";
    }

    std::ostream& out_;
    Format fmt_;
    uint64_t count_{0};
};

}  // namespace exchange
