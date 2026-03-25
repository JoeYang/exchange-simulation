#pragma once

#include "exchange-core/types.h"

namespace exchange {

// --- Order events (Section 6.1) ---

struct OrderAccepted {
    OrderId  id;
    uint64_t client_order_id;
    Timestamp ts;
};

struct OrderRejected {
    uint64_t  client_order_id;
    Timestamp ts;
    RejectReason reason;
};

struct OrderFilled {
    OrderId   aggressor_id;
    OrderId   resting_id;
    Price     price;
    Quantity  quantity;
    Timestamp ts;
};

struct OrderPartiallyFilled {
    OrderId   aggressor_id;
    OrderId   resting_id;
    Price     price;
    Quantity  quantity;
    Quantity  aggressor_remaining;
    Quantity  resting_remaining;
    Timestamp ts;
};

struct OrderCancelled {
    OrderId      id;
    Timestamp    ts;
    CancelReason reason;
};

struct OrderCancelRejected {
    OrderId      id;
    uint64_t     client_order_id;
    Timestamp    ts;
    RejectReason reason;
};

struct OrderModified {
    OrderId   id;
    uint64_t  client_order_id;
    Price     new_price;
    Quantity  new_qty;
    Timestamp ts;
};

struct OrderModifyRejected {
    OrderId      id;
    uint64_t     client_order_id;
    Timestamp    ts;
    RejectReason reason;
};

// --- Market data events (Section 6.2) ---

struct TopOfBook {
    Price     best_bid;
    Quantity  bid_qty;
    Price     best_ask;
    Quantity  ask_qty;
    Timestamp ts;
};

struct DepthUpdate {
    Side      side;
    Price     price;
    Quantity  total_qty;
    uint32_t  order_count;
    enum Action : uint8_t { Add, Update, Remove } action;
    Timestamp ts;
};

struct OrderBookAction {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;
    enum Action : uint8_t { Add, Modify, Cancel, Fill } action;
    Timestamp ts;
};

struct Trade {
    Price     price;
    Quantity  quantity;
    OrderId   aggressor_id;
    OrderId   resting_id;
    Side      aggressor_side;
    Timestamp ts;
};

// --- Session state events (Phase 2, Section 1.4 / 2.4) ---

struct MarketStatus {
    SessionState state{};
    Timestamp    ts{};
};

struct IndicativePrice {
    Price    price{};
    Quantity matched_volume{};
    Quantity buy_surplus{};
    Quantity sell_surplus{};
    Timestamp ts{};
};

}  // namespace exchange
