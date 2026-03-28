#pragma once

#include "exchange-sim/spread_book/spread_book.h"
#include "exchange-sim/spread_book/spread_instrument_config.h"
#include "exchange-sim/spread_book/spread_strategy_registry.h"

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace exchange {

// SpreadSimulator -- manages SpreadBook instances alongside outright engines.
//
// Template parameters:
//   EngineT:         outright matching engine type (CRTP concrete class)
//   OrderListenerT:  order event listener type
//   MdListenerT:     market data listener type
//
// Usage:
//   1. Create SpreadSimulator with references to outright engine lookup.
//   2. Call add_spread_instrument() for each spread.
//   3. Route spread orders via new_spread_order(), cancel_spread_order(), etc.
//   4. Call on_outright_bbo_change() after outright fills to trigger implied.
template <typename EngineT, typename OrderListenerT>
class SpreadSimulator {
public:
    // engine_lookup: function to get an outright engine by instrument ID.
    using EngineLookup = std::function<EngineT*(uint32_t)>;

    SpreadSimulator(EngineLookup engine_lookup, OrderListenerT& listener)
        : engine_lookup_(std::move(engine_lookup))
        , listener_(listener) {}

    // Non-copyable, non-movable.
    SpreadSimulator(const SpreadSimulator&) = delete;
    SpreadSimulator& operator=(const SpreadSimulator&) = delete;
    SpreadSimulator(SpreadSimulator&&) = delete;
    SpreadSimulator& operator=(SpreadSimulator&&) = delete;

    // --- Spread Instrument Management ---

    // Register a spread instrument. Creates a SpreadBook and wires up
    // callbacks to outright engines.
    void add_spread_instrument(const SpreadInstrumentConfig& cfg) {
        if (spread_books_.count(cfg.id)) {
            throw std::runtime_error(
                "Spread instrument already exists: " + cfg.symbol);
        }

        auto strategy = cfg.build_strategy();

        // Register in strategy registry.
        auto err = registry_.register_strategy(cfg.id, strategy);
        if (!err.empty()) {
            throw std::runtime_error("Strategy registration failed: " + err);
        }

        // Create SpreadBook.
        auto book = std::make_unique<SpreadBook<>>(
            *registry_.lookup(cfg.id), cfg.id);

        // Wire up callbacks.
        book->set_bbo_provider([this](uint32_t instr_id) -> LegBBO {
            auto* engine = engine_lookup_(instr_id);
            if (!engine) return {};
            LegBBO bbo;
            auto* best_bid = engine->book().best_bid();
            auto* best_ask = engine->book().best_ask();
            if (best_bid) {
                bbo.bid_price = best_bid->price;
                bbo.bid_qty = best_bid->total_quantity;
            }
            if (best_ask) {
                bbo.ask_price = best_ask->price;
                bbo.ask_qty = best_ask->total_quantity;
            }
            return bbo;
        });

        book->set_best_order_provider(
            [this](uint32_t instr_id, Side side) -> std::optional<OrderId> {
                auto* engine = engine_lookup_(instr_id);
                if (!engine) return std::nullopt;
                return engine->best_order_id(side);
            });

        book->set_fill_applier(
            [this](uint32_t instr_id, std::span<const LegFill> fills,
                   Timestamp ts) -> bool {
                auto* engine = engine_lookup_(instr_id);
                if (!engine) return false;
                return engine->apply_implied_fills(fills, ts);
            });

        // Track which outright instruments are legs of this spread.
        const auto* strat = registry_.lookup(cfg.id);
        for (const auto& leg : strat->legs()) {
            leg_to_spreads_[leg.instrument_id].insert(cfg.id);
        }

        spread_books_[cfg.id] = std::move(book);
    }

    // --- Order Routing ---

    OrderId new_spread_order(uint32_t spread_id,
                             const SpreadOrderRequest& req) {
        auto* book = get_spread_book(spread_id);
        if (!book) throw std::runtime_error("Unknown spread instrument");
        OrderId id = book->new_order(req, listener_);

        // After order entry, try implied-in matching.
        if (id != 0) {
            book->try_implied_in(listener_, req.timestamp);
        }
        return id;
    }

    bool cancel_spread_order(uint32_t spread_id,
                             const SpreadCancelRequest& req) {
        auto* book = get_spread_book(spread_id);
        if (!book) return false;
        return book->cancel_order(req, listener_);
    }

    bool modify_spread_order(uint32_t spread_id,
                             const SpreadModifyRequest& req) {
        auto* book = get_spread_book(spread_id);
        if (!book) return false;
        return book->modify_order(req, listener_);
    }

    // --- Implied Matching Trigger ---
    //
    // Called after an outright instrument's BBO changes (fill, cancel, etc.)
    // to trigger implied-out matching on all spreads that include this leg.
    int on_outright_bbo_change(uint32_t outright_id, Timestamp ts) {
        auto it = leg_to_spreads_.find(outright_id);
        if (it == leg_to_spreads_.end()) return 0;

        int total_fills = 0;
        for (uint32_t spread_id : it->second) {
            auto* book = get_spread_book(spread_id);
            if (book) {
                total_fills += book->on_outright_bbo_change(listener_, ts);
            }
        }
        return total_fills;
    }

    // --- Accessors ---

    SpreadBook<>* get_spread_book(uint32_t spread_id) {
        auto it = spread_books_.find(spread_id);
        return it != spread_books_.end() ? it->second.get() : nullptr;
    }

    const SpreadBook<>* get_spread_book(uint32_t spread_id) const {
        auto it = spread_books_.find(spread_id);
        return it != spread_books_.end() ? it->second.get() : nullptr;
    }

    const SpreadStrategyRegistry& registry() const { return registry_; }
    size_t spread_count() const { return spread_books_.size(); }

    bool is_spread(uint32_t id) const { return registry_.is_spread(id); }

private:
    EngineLookup engine_lookup_;
    OrderListenerT& listener_;
    SpreadStrategyRegistry registry_;
    std::unordered_map<uint32_t, std::unique_ptr<SpreadBook<>>> spread_books_;
    // Maps outright instrument ID -> set of spread IDs that use it as a leg.
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> leg_to_spreads_;
};

}  // namespace exchange
