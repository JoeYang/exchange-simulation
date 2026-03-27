#pragma once

#include "exchange-core/events.h"

namespace exchange {

// Default no-op listener base classes.
//
// Derived classes override individual methods by name hiding — not virtual
// dispatch. The matching engine calls methods on the concrete listener type
// directly via its template parameter, so there is zero virtual overhead.
//
// Usage:
//   class MyListener : public OrderListenerBase {
//   public:
//       void on_order_accepted(const OrderAccepted& e) { /* handle */ }
//   };

class OrderListenerBase {
public:
    void on_order_accepted(const OrderAccepted&) {}
    void on_order_rejected(const OrderRejected&) {}
    void on_order_filled(const OrderFilled&) {}
    void on_order_partially_filled(const OrderPartiallyFilled&) {}
    void on_order_cancelled(const OrderCancelled&) {}
    void on_order_cancel_rejected(const OrderCancelRejected&) {}
    void on_order_modified(const OrderModified&) {}
    void on_order_modify_rejected(const OrderModifyRejected&) {}
    void on_trade_busted(const TradeBusted&) {}
};

class MarketDataListenerBase {
public:
    void on_top_of_book(const TopOfBook&) {}
    void on_depth_update(const DepthUpdate&) {}
    void on_order_book_action(const OrderBookAction&) {}
    void on_trade(const Trade&) {}
    void on_market_status(const MarketStatus&) {}
    void on_indicative_price(const IndicativePrice&) {}
    void on_lock_limit_triggered(const LockLimitTriggered&) {}
};

}  // namespace exchange
