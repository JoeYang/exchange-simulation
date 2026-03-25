#pragma once

#include "exchange-core/matching_engine.h"
#include "exchange-core/ohlcv.h"
#include "exchange-sim/instrument_config.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// ExchangeSimulator -- manages multiple MatchingEngine instances (one per
// instrument) and provides exchange-wide operations.
//
// EngineT must be a concrete CRTP class that inherits from MatchingEngine
// and is constructible from (EngineConfig, OrderListenerT&, MdListenerT&).
//
// OrderListenerT and MdListenerT are deduced from EngineT's constructor.
// To keep it simple, they are passed explicitly as template parameters.

template <typename EngineT, typename OrderListenerT, typename MdListenerT>
class ExchangeSimulator {
public:
    ExchangeSimulator(OrderListenerT& ol, MdListenerT& ml)
        : order_listener_(ol), md_listener_(ml) {}

    // Non-copyable, non-movable (owns unique_ptrs).
    ExchangeSimulator(const ExchangeSimulator&) = delete;
    ExchangeSimulator& operator=(const ExchangeSimulator&) = delete;
    ExchangeSimulator(ExchangeSimulator&&) = delete;
    ExchangeSimulator& operator=(ExchangeSimulator&&) = delete;

    // --- Instrument Management ---

    void add_instrument(const InstrumentConfig& cfg) {
        if (instruments_.count(cfg.id)) {
            throw std::runtime_error(
                "Instrument already exists: " + cfg.symbol);
        }
        auto engine = std::make_unique<EngineT>(
            cfg.engine_config, order_listener_, md_listener_);
        // Bring new engine to the current session state.
        if (current_state_ != SessionState::Closed) {
            engine->set_session_state(current_state_, 0);
        }
        instruments_[cfg.id] = InstrumentState{cfg, std::move(engine), {}};
    }

    void remove_instrument(InstrumentId id) {
        auto it = instruments_.find(id);
        if (it == instruments_.end()) {
            throw std::runtime_error("Unknown instrument");
        }
        instruments_.erase(it);
    }

    EngineT* get_engine(InstrumentId id) {
        auto it = instruments_.find(id);
        return it != instruments_.end() ? it->second.engine.get() : nullptr;
    }

    const EngineT* get_engine(InstrumentId id) const {
        auto it = instruments_.find(id);
        return it != instruments_.end() ? it->second.engine.get() : nullptr;
    }

    const InstrumentConfig* get_config(InstrumentId id) const {
        auto it = instruments_.find(id);
        return it != instruments_.end() ? &it->second.config : nullptr;
    }

    OhlcvStats* get_ohlcv(InstrumentId id) {
        auto it = instruments_.find(id);
        return it != instruments_.end() ? &it->second.ohlcv : nullptr;
    }

    const OhlcvStats* get_ohlcv(InstrumentId id) const {
        auto it = instruments_.find(id);
        return it != instruments_.end() ? &it->second.ohlcv : nullptr;
    }

    size_t instrument_count() const { return instruments_.size(); }

    // --- Order Routing ---

    void new_order(InstrumentId instrument, const OrderRequest& req) {
        auto* engine = get_engine(instrument);
        if (!engine) {
            throw std::runtime_error("Unknown instrument");
        }
        engine->new_order(req);
    }

    void cancel_order(InstrumentId instrument, OrderId id, Timestamp ts) {
        auto* engine = get_engine(instrument);
        if (!engine) {
            throw std::runtime_error("Unknown instrument");
        }
        engine->cancel_order(id, ts);
    }

    void modify_order(InstrumentId instrument, const ModifyRequest& req) {
        auto* engine = get_engine(instrument);
        if (!engine) {
            throw std::runtime_error("Unknown instrument");
        }
        engine->modify_order(req);
    }

    // --- Exchange-Wide Operations ---

    void set_session_state(SessionState state, Timestamp ts) {
        current_state_ = state;
        for (auto& [id, inst] : instruments_) {
            inst.engine->set_session_state(state, ts);
            if (state == SessionState::PreOpen ||
                state == SessionState::Closed) {
                inst.ohlcv.reset();
            }
        }
    }

    void execute_all_auctions(Timestamp ts) {
        for (auto& [id, inst] : instruments_) {
            // Use the previous close as reference price, or 0 if none.
            Price ref = inst.ohlcv.close;
            inst.engine->execute_auction(ref, ts);
        }
    }

    void mass_cancel_all(Timestamp ts) {
        for (auto& [id, inst] : instruments_) {
            inst.engine->mass_cancel_all(ts);
        }
    }

    void trigger_expiry(Timestamp now, TimeInForce tif) {
        for (auto& [id, inst] : instruments_) {
            inst.engine->trigger_expiry(now, tif);
        }
    }

    SessionState session_state() const { return current_state_; }

    // --- Iteration ---

    template <typename Fn>
    void for_each_instrument(Fn&& fn) {
        for (auto& [id, inst] : instruments_) {
            fn(id, inst.config, *inst.engine, inst.ohlcv);
        }
    }

    template <typename Fn>
    void for_each_instrument(Fn&& fn) const {
        for (const auto& [id, inst] : instruments_) {
            fn(id, inst.config, *inst.engine, inst.ohlcv);
        }
    }

private:
    struct InstrumentState {
        InstrumentConfig config;
        std::unique_ptr<EngineT> engine;
        OhlcvStats ohlcv;
    };

    std::unordered_map<InstrumentId, InstrumentState> instruments_;
    OrderListenerT& order_listener_;
    MdListenerT& md_listener_;
    SessionState current_state_{SessionState::Closed};
};

}  // namespace exchange
