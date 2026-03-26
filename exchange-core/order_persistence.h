#pragma once

#include "exchange-core/types.h"

#include <cstring>

namespace exchange {

// ---------------------------------------------------------------------------
// SerializedOrder -- flat, POD-friendly representation of an Order.
//
// All intrusive-list pointers and back-pointers are stripped.  This struct
// is safe to memcpy, write to disk, or send over the wire.  Layout is
// fixed and does not depend on pointer size (all fields are fixed-width).
// ---------------------------------------------------------------------------

struct SerializedOrder {
    OrderId     id{0};
    uint64_t    client_order_id{0};
    uint64_t    account_id{0};
    Price       price{0};
    Quantity    quantity{0};
    Quantity    filled_quantity{0};
    Quantity    remaining_quantity{0};
    Side        side{Side::Buy};
    OrderType   type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
    Timestamp   timestamp{0};
    Timestamp   gtd_expiry{0};
    Quantity    display_qty{0};
    Quantity    total_qty{0};
};

// ---------------------------------------------------------------------------
// serialize_order -- extract a flat SerializedOrder from a live Order.
//
// Strips intrusive-list pointers (prev, next, level).  All value fields
// are copied verbatim.  O(1), no allocation.
// ---------------------------------------------------------------------------

inline SerializedOrder serialize_order(const Order& order) {
    SerializedOrder s;
    s.id                 = order.id;
    s.client_order_id    = order.client_order_id;
    s.account_id         = order.account_id;
    s.price              = order.price;
    s.quantity           = order.quantity;
    s.filled_quantity    = order.filled_quantity;
    s.remaining_quantity = order.remaining_quantity;
    s.side               = order.side;
    s.type               = order.type;
    s.tif                = order.tif;
    s.timestamp          = order.timestamp;
    s.gtd_expiry         = order.gtd_expiry;
    s.display_qty        = order.display_qty;
    s.total_qty          = order.total_qty;
    return s;
}

// ---------------------------------------------------------------------------
// deserialize_order -- populate an Order from a SerializedOrder.
//
// Sets all value fields from the serialized representation.  Intrusive-list
// pointers (prev, next, level) are zeroed -- the caller is responsible for
// inserting the order into the appropriate data structure.
// ---------------------------------------------------------------------------

inline void deserialize_order(const SerializedOrder& s, Order* order) {
    order->id                 = s.id;
    order->client_order_id    = s.client_order_id;
    order->account_id         = s.account_id;
    order->price              = s.price;
    order->quantity           = s.quantity;
    order->filled_quantity    = s.filled_quantity;
    order->remaining_quantity = s.remaining_quantity;
    order->side               = s.side;
    order->type               = s.type;
    order->tif                = s.tif;
    order->timestamp          = s.timestamp;
    order->gtd_expiry         = s.gtd_expiry;
    order->display_qty        = s.display_qty;
    order->total_qty          = s.total_qty;
    order->prev               = nullptr;
    order->next               = nullptr;
    order->level              = nullptr;
}

}  // namespace exchange
