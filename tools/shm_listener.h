#pragma once
#include "exchange-core/listeners.h"
#include "tools/shm_transport.h"

namespace exchange {

class SharedMemoryOrderListener : public OrderListenerBase {
    ShmProducer& producer_;
public:
    explicit SharedMemoryOrderListener(ShmProducer& producer) : producer_(producer) {}

    void on_order_accepted(const OrderAccepted& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_rejected(const OrderRejected& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_filled(const OrderFilled& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_partially_filled(const OrderPartiallyFilled& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_cancelled(const OrderCancelled& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_cancel_rejected(const OrderCancelRejected& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_modified(const OrderModified& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_modify_rejected(const OrderModifyRejected& e) { producer_.publish(RecordedEvent{e}); }
};

class SharedMemoryMdListener : public MarketDataListenerBase {
    ShmProducer& producer_;
public:
    explicit SharedMemoryMdListener(ShmProducer& producer) : producer_(producer) {}

    void on_top_of_book(const TopOfBook& e) { producer_.publish(RecordedEvent{e}); }
    void on_depth_update(const DepthUpdate& e) { producer_.publish(RecordedEvent{e}); }
    void on_order_book_action(const OrderBookAction& e) { producer_.publish(RecordedEvent{e}); }
    void on_trade(const Trade& e) { producer_.publish(RecordedEvent{e}); }
};

}  // namespace exchange
