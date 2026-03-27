#pragma once

#include "exchange-core/listeners.h"
#include "test-harness/recorded_event.h"

#include <vector>

namespace exchange {

// RecordingOrderListener captures every order callback event into an ordered
// vector of RecordedEvent values. Intended solely for use in tests — the
// allocation overhead of std::vector is acceptable in that context.
class RecordingOrderListener : public OrderListenerBase {
    std::vector<RecordedEvent> events_;

public:
    void on_order_accepted(const OrderAccepted& e) { events_.emplace_back(e); }
    void on_order_rejected(const OrderRejected& e) { events_.emplace_back(e); }
    void on_order_filled(const OrderFilled& e) { events_.emplace_back(e); }
    void on_order_partially_filled(const OrderPartiallyFilled& e) { events_.emplace_back(e); }
    void on_order_cancelled(const OrderCancelled& e) { events_.emplace_back(e); }
    void on_order_cancel_rejected(const OrderCancelRejected& e) { events_.emplace_back(e); }
    void on_order_modified(const OrderModified& e) { events_.emplace_back(e); }
    void on_order_modify_rejected(const OrderModifyRejected& e) { events_.emplace_back(e); }
    void on_trade_busted(const TradeBusted& e) { events_.emplace_back(e); }

    const std::vector<RecordedEvent>& events() const { return events_; }
    size_t size() const { return events_.size(); }
    void clear() { events_.clear(); }
};

// RecordingMdListener captures every market data callback event into an ordered
// vector of RecordedEvent values. Intended solely for use in tests.
class RecordingMdListener : public MarketDataListenerBase {
    std::vector<RecordedEvent> events_;

public:
    void on_top_of_book(const TopOfBook& e) { events_.emplace_back(e); }
    void on_depth_update(const DepthUpdate& e) { events_.emplace_back(e); }
    void on_order_book_action(const OrderBookAction& e) { events_.emplace_back(e); }
    void on_trade(const Trade& e) { events_.emplace_back(e); }
    void on_market_status(const MarketStatus& e) { events_.emplace_back(e); }
    void on_indicative_price(const IndicativePrice& e) { events_.emplace_back(e); }
    void on_lock_limit_triggered(const LockLimitTriggered& e) { events_.emplace_back(e); }

    const std::vector<RecordedEvent>& events() const { return events_; }
    size_t size() const { return events_.size(); }
    void clear() { events_.clear(); }
};

}  // namespace exchange
