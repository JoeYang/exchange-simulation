#include "exchange-core/stop_book.h"

namespace exchange {

// ---------------------------------------------------------------------------
// insert_stop
// ---------------------------------------------------------------------------

PriceLevel* StopBook::insert_stop(Order* order, PriceLevel* new_level_if_needed) {
    PriceLevel* lv = find_level(order->side, order->price);

    if (lv == nullptr) {
        // Initialise the caller-supplied level slot.
        lv                 = new_level_if_needed;
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
// remove_stop
// ---------------------------------------------------------------------------

PriceLevel* StopBook::remove_stop(Order* order) {
    PriceLevel* lv = order->level;

    lv->total_quantity -= order->remaining_quantity;
    --lv->order_count;

    list_remove(lv->head, lv->tail, order);

    if (lv->order_count == 0) {
        // Determine the side by scanning the buy_stops list for lv.
        // If found → Buy; otherwise it must be on the Sell side.
        Side side = Side::Sell;
        for (const PriceLevel* p = buy_stops_; p != nullptr; p = p->next) {
            if (p == lv) { side = Side::Buy; break; }
        }
        unlink_level(side, lv);
        return lv;
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// has_triggered_stops
// ---------------------------------------------------------------------------

bool StopBook::has_triggered_stops(Price last_trade_price) const noexcept {
    // Buy stops: ascending list, head has lowest price.
    // Triggers when last_trade_price >= head->price.
    if (buy_stops_ != nullptr && last_trade_price >= buy_stops_->price) {
        return true;
    }
    // Sell stops: descending list, head has highest price.
    // Triggers when last_trade_price <= head->price.
    if (sell_stops_ != nullptr && last_trade_price <= sell_stops_->price) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// next_triggered_stop
// ---------------------------------------------------------------------------

Order* StopBook::next_triggered_stop(Price last_trade_price) const noexcept {
    // Check buy stops first (lowest price = head, triggers on price rise).
    if (buy_stops_ != nullptr && last_trade_price >= buy_stops_->price) {
        return buy_stops_->head;
    }
    // Check sell stops (highest price = head, triggers on price fall).
    if (sell_stops_ != nullptr && last_trade_price <= sell_stops_->price) {
        return sell_stops_->head;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// find_level
// ---------------------------------------------------------------------------

PriceLevel* StopBook::find_level(Side side, Price price) const noexcept {
    const PriceLevel* head = (side == Side::Buy) ? buy_stops_ : sell_stops_;
    for (const PriceLevel* lv = head; lv != nullptr; lv = lv->next) {
        if (lv->price == price) return const_cast<PriceLevel*>(lv);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// link_level
//
// Buy stops (ascending):  insert before the first node whose price > lv->price,
//   or push to back when lv has the highest price (worst/last to trigger).
//
// Sell stops (descending): insert before the first node whose price < lv->price,
//   or push to back when lv has the lowest price (worst/last to trigger).
// ---------------------------------------------------------------------------

void StopBook::link_level(Side side, PriceLevel* lv) noexcept {
    if (side == Side::Buy) {
        // Walk buy stops from lowest to find insertion point.
        for (PriceLevel* cur = buy_stops_; cur != nullptr; cur = cur->next) {
            if (lv->price < cur->price) {
                list_insert_before(buy_stops_, buy_stops_tail_, cur, lv);
                return;
            }
        }
        // lv has the highest stop price — append to tail.
        list_push_back(buy_stops_, buy_stops_tail_, lv);
    } else {
        // Walk sell stops from highest to find insertion point.
        for (PriceLevel* cur = sell_stops_; cur != nullptr; cur = cur->next) {
            if (lv->price > cur->price) {
                list_insert_before(sell_stops_, sell_stops_tail_, cur, lv);
                return;
            }
        }
        // lv has the lowest stop price — append to tail.
        list_push_back(sell_stops_, sell_stops_tail_, lv);
    }
}

// ---------------------------------------------------------------------------
// unlink_level
// ---------------------------------------------------------------------------

void StopBook::unlink_level(Side side, PriceLevel* lv) noexcept {
    if (side == Side::Buy) {
        list_remove(buy_stops_, buy_stops_tail_, lv);
    } else {
        list_remove(sell_stops_, sell_stops_tail_, lv);
    }
}

}  // namespace exchange
