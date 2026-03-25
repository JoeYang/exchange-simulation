#pragma once
#include "exchange-core/listeners.h"
#include <tuple>

namespace exchange {

// CompositeOrderListener fans out every order event to all wrapped listeners.
//
// Listeners are stored as pointers (not values) to avoid object slicing and to
// allow the composite to reference externally-owned objects. Listener set is
// fixed at compile time; dispatch uses C++20 fold expressions — zero virtual
// overhead.
//
// Usage:
//   MyListenerA a;
//   MyListenerB b;
//   CompositeOrderListener<MyListenerA, MyListenerB> composite(&a, &b);
//   composite.on_order_filled(event);  // dispatched to both a and b

template <typename... Listeners>
class CompositeOrderListener : public OrderListenerBase {
    std::tuple<Listeners*...> listeners_;

public:
    explicit CompositeOrderListener(Listeners*... listeners)
        : listeners_(listeners...) {}

    void on_order_accepted(const OrderAccepted& e) {
        std::apply([&](auto*... l) { (l->on_order_accepted(e), ...); }, listeners_);
    }
    void on_order_rejected(const OrderRejected& e) {
        std::apply([&](auto*... l) { (l->on_order_rejected(e), ...); }, listeners_);
    }
    void on_order_filled(const OrderFilled& e) {
        std::apply([&](auto*... l) { (l->on_order_filled(e), ...); }, listeners_);
    }
    void on_order_partially_filled(const OrderPartiallyFilled& e) {
        std::apply([&](auto*... l) { (l->on_order_partially_filled(e), ...); }, listeners_);
    }
    void on_order_cancelled(const OrderCancelled& e) {
        std::apply([&](auto*... l) { (l->on_order_cancelled(e), ...); }, listeners_);
    }
    void on_order_cancel_rejected(const OrderCancelRejected& e) {
        std::apply([&](auto*... l) { (l->on_order_cancel_rejected(e), ...); }, listeners_);
    }
    void on_order_modified(const OrderModified& e) {
        std::apply([&](auto*... l) { (l->on_order_modified(e), ...); }, listeners_);
    }
    void on_order_modify_rejected(const OrderModifyRejected& e) {
        std::apply([&](auto*... l) { (l->on_order_modify_rejected(e), ...); }, listeners_);
    }
};

// CompositeMdListener fans out every market-data event to all wrapped listeners.
//
// Same design as CompositeOrderListener — pointer storage, fold-expression
// dispatch, listener set fixed at compile time.

template <typename... Listeners>
class CompositeMdListener : public MarketDataListenerBase {
    std::tuple<Listeners*...> listeners_;

public:
    explicit CompositeMdListener(Listeners*... listeners)
        : listeners_(listeners...) {}

    void on_top_of_book(const TopOfBook& e) {
        std::apply([&](auto*... l) { (l->on_top_of_book(e), ...); }, listeners_);
    }
    void on_depth_update(const DepthUpdate& e) {
        std::apply([&](auto*... l) { (l->on_depth_update(e), ...); }, listeners_);
    }
    void on_order_book_action(const OrderBookAction& e) {
        std::apply([&](auto*... l) { (l->on_order_book_action(e), ...); }, listeners_);
    }
    void on_trade(const Trade& e) {
        std::apply([&](auto*... l) { (l->on_trade(e), ...); }, listeners_);
    }
    void on_market_status(const MarketStatus& e) {
        std::apply([&](auto*... l) { (l->on_market_status(e), ...); }, listeners_);
    }
    void on_indicative_price(const IndicativePrice& e) {
        std::apply([&](auto*... l) { (l->on_indicative_price(e), ...); }, listeners_);
    }
};

}  // namespace exchange
