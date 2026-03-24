#include "exchange-core/orderbook.h"

namespace exchange {

// ---------------------------------------------------------------------------
// insert_order
// ---------------------------------------------------------------------------

PriceLevel* OrderBook::insert_order(Order* order, PriceLevel* new_level_if_needed) {
    PriceLevel* lv = find_level(order->side, order->price);

    if (lv == nullptr) {
        // Initialise the caller-supplied level slot.
        lv = new_level_if_needed;
        lv->price          = order->price;
        lv->total_quantity = 0;
        lv->order_count    = 0;
        lv->head           = nullptr;
        lv->tail           = nullptr;
        lv->prev           = nullptr;
        lv->next           = nullptr;
        link_level(order->side, lv);
    }

    // Append order at the back of the level's order list (FIFO time-priority).
    list_push_back(lv->head, lv->tail, order);
    order->level = lv;

    lv->total_quantity += order->remaining_quantity;
    ++lv->order_count;

    return lv;
}

// ---------------------------------------------------------------------------
// remove_order
// ---------------------------------------------------------------------------

PriceLevel* OrderBook::remove_order(Order* order) {
    PriceLevel* lv = order->level;

    lv->total_quantity -= order->remaining_quantity;
    --lv->order_count;

    list_remove(lv->head, lv->tail, order);

    if (lv->order_count == 0) {
        // Determine the side by scanning the bid list for lv.
        // If found → bid; otherwise it must be on the ask side.
        // Price levels are typically O(10s), so this scan is acceptable.
        Side side = Side::Sell;
        for (const PriceLevel* p = best_bid_; p != nullptr; p = p->next) {
            if (p == lv) { side = Side::Buy; break; }
        }
        unlink_level(side, lv);
        return lv;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// find_level
// ---------------------------------------------------------------------------

PriceLevel* OrderBook::find_level(Side side, Price price) const noexcept {
    const PriceLevel* head = (side == Side::Buy) ? best_bid_ : best_ask_;
    for (const PriceLevel* lv = head; lv != nullptr; lv = lv->next) {
        if (lv->price == price) return const_cast<PriceLevel*>(lv);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// link_level
//
// Bids (descending): insert before the first node whose price < lv->price,
//   or push to back when lv is the worst (lowest) bid.
//
// Asks (ascending): insert before the first node whose price > lv->price,
//   or push to back when lv is the worst (highest) ask.
// ---------------------------------------------------------------------------

void OrderBook::link_level(Side side, PriceLevel* lv) noexcept {
    if (side == Side::Buy) {
        // Walk bids from best (highest) to find insertion point.
        for (PriceLevel* cur = best_bid_; cur != nullptr; cur = cur->next) {
            if (lv->price > cur->price) {
                list_insert_before(best_bid_, bid_tail_, cur, lv);
                return;
            }
        }
        // lv is the new worst (lowest) bid — append to tail.
        list_push_back(best_bid_, bid_tail_, lv);
    } else {
        // Walk asks from best (lowest) to find insertion point.
        for (PriceLevel* cur = best_ask_; cur != nullptr; cur = cur->next) {
            if (lv->price < cur->price) {
                list_insert_before(best_ask_, ask_tail_, cur, lv);
                return;
            }
        }
        // lv is the new worst (highest) ask — append to tail.
        list_push_back(best_ask_, ask_tail_, lv);
    }
}

// ---------------------------------------------------------------------------
// unlink_level
// ---------------------------------------------------------------------------

void OrderBook::unlink_level(Side side, PriceLevel* lv) noexcept {
    if (side == Side::Buy) {
        list_remove(best_bid_, bid_tail_, lv);
    } else {
        list_remove(best_ask_, ask_tail_, lv);
    }
}

}  // namespace exchange
