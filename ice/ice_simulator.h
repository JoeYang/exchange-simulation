#pragma once

#include "exchange-sim/exchange_simulator.h"
#include "ice/gtbpr_match.h"
#include "ice/ice_exchange.h"
#include "ice/ice_products.h"

#include <vector>

namespace exchange {
namespace ice {

// Convenience aliases for the two ICE engine variants.
template <typename OrderListenerT, typename MdListenerT>
using IceFifoEngine = IceExchange<OrderListenerT, MdListenerT, FifoMatch>;

template <typename OrderListenerT, typename MdListenerT>
using IceGtbprEngine = IceExchange<OrderListenerT, MdListenerT,
                                    GtbprMatch>;

// IceSimulator -- multi-instrument exchange simulator for ICE Futures.
//
// Manages two underlying ExchangeSimulators:
//   - FIFO engines for energy, softs, equity index products
//   - GTBPR engines for STIR products (Euribor, SONIA, SOFR)
//
// Routes orders to the correct engine based on the product's match_algo
// setting. Session state transitions and lifecycle operations are broadcast
// to both engine pools.
template <typename OrderListenerT, typename MdListenerT>
class IceSimulator {
    using FifoSim = ExchangeSimulator<
        IceFifoEngine<OrderListenerT, MdListenerT>,
        OrderListenerT, MdListenerT>;
    using GtbprSim = ExchangeSimulator<
        IceGtbprEngine<OrderListenerT, MdListenerT>,
        OrderListenerT, MdListenerT>;

    FifoSim fifo_sim_;
    GtbprSim gtbpr_sim_;

    // Maps instrument_id → true if GTBPR, false if FIFO.
    std::unordered_map<uint32_t, bool> is_gtbpr_;

public:
    IceSimulator(OrderListenerT& ol, MdListenerT& ml)
        : fifo_sim_(ol, ml), gtbpr_sim_(ol, ml) {}

    // Non-copyable, non-movable.
    IceSimulator(const IceSimulator&) = delete;
    IceSimulator& operator=(const IceSimulator&) = delete;
    IceSimulator(IceSimulator&&) = delete;
    IceSimulator& operator=(IceSimulator&&) = delete;

    // load_products -- create one engine per IceProductConfig entry.
    //
    // Routes each product to the FIFO or GTBPR simulator based on
    // match_algo. Configures per-product IPL width and SMP action.
    void load_products(const std::vector<IceProductConfig>& products) {
        for (const auto& p : products) {
            InstrumentConfig cfg{
                .id = p.instrument_id,
                .symbol = p.symbol,
                .engine_config = EngineConfig{
                    .tick_size       = p.tick_size,
                    .lot_size        = p.lot_size,
                    .price_band_low  = 0,
                    .price_band_high = 0,
                    .max_order_size  = p.max_order_size,
                },
            };

            bool gtbpr = (p.match_algo == IceMatchAlgo::GTBPR);
            is_gtbpr_[p.instrument_id] = gtbpr;

            if (gtbpr) {
                gtbpr_sim_.add_instrument(cfg);
                auto* engine = gtbpr_sim_.get_engine(p.instrument_id);
                if (engine) {
                    engine->set_ipl_width(p.ipl_width);
                    engine->set_smp_action(p.smp_action);
                }
            } else {
                fifo_sim_.add_instrument(cfg);
                auto* engine = fifo_sim_.get_engine(p.instrument_id);
                if (engine) {
                    engine->set_ipl_width(p.ipl_width);
                    engine->set_smp_action(p.smp_action);
                }
            }
        }
    }

    // --- Order Routing ---

    void new_order(uint32_t instrument, const OrderRequest& req) {
        auto it = is_gtbpr_.find(instrument);
        if (it == is_gtbpr_.end()) {
            throw std::runtime_error("Unknown instrument");
        }
        if (it->second) {
            gtbpr_sim_.new_order(instrument, req);
        } else {
            fifo_sim_.new_order(instrument, req);
        }
    }

    void cancel_order(uint32_t instrument, OrderId id, Timestamp ts) {
        auto it = is_gtbpr_.find(instrument);
        if (it == is_gtbpr_.end()) {
            throw std::runtime_error("Unknown instrument");
        }
        if (it->second) {
            gtbpr_sim_.cancel_order(instrument, id, ts);
        } else {
            fifo_sim_.cancel_order(instrument, id, ts);
        }
    }

    void modify_order(uint32_t instrument, const ModifyRequest& req) {
        auto it = is_gtbpr_.find(instrument);
        if (it == is_gtbpr_.end()) {
            throw std::runtime_error("Unknown instrument");
        }
        if (it->second) {
            gtbpr_sim_.modify_order(instrument, req);
        } else {
            fifo_sim_.modify_order(instrument, req);
        }
    }

    // --- Instrument Access ---

    size_t instrument_count() const {
        return fifo_sim_.instrument_count() + gtbpr_sim_.instrument_count();
    }

    bool is_gtbpr_instrument(uint32_t id) const {
        auto it = is_gtbpr_.find(id);
        return it != is_gtbpr_.end() && it->second;
    }

    // Access engines by type. Returns nullptr if instrument not found or
    // wrong algo type.
    auto* get_fifo_engine(uint32_t id) { return fifo_sim_.get_engine(id); }
    auto* get_gtbpr_engine(uint32_t id) { return gtbpr_sim_.get_engine(id); }

    // --- Session Lifecycle ---
    //
    // ICE daily cycle:
    //   Closed → PreOpen → Continuous → settlement (VWAP) → Closed
    // ICE has no closing auction — settlement is VWAP-based.

    void start_trading_day(Timestamp ts) {
        fifo_sim_.set_session_state(SessionState::PreOpen, ts);
        gtbpr_sim_.set_session_state(SessionState::PreOpen, ts);
    }

    void open_market(Timestamp ts) {
        fifo_sim_.execute_all_auctions(ts);
        fifo_sim_.set_session_state(SessionState::Continuous, ts);
        gtbpr_sim_.execute_all_auctions(ts);
        gtbpr_sim_.set_session_state(SessionState::Continuous, ts);
    }

    // close_market -- transition directly to Closed.
    // ICE uses VWAP settlement (not a closing auction), so no uncross.
    void close_market(Timestamp ts) {
        fifo_sim_.set_session_state(SessionState::Closed, ts);
        gtbpr_sim_.set_session_state(SessionState::Closed, ts);
    }

    void end_of_day(Timestamp ts) {
        fifo_sim_.trigger_expiry(ts, TimeInForce::DAY);
        gtbpr_sim_.trigger_expiry(ts, TimeInForce::DAY);
    }

    void set_session_state(SessionState state, Timestamp ts) {
        fifo_sim_.set_session_state(state, ts);
        gtbpr_sim_.set_session_state(state, ts);
    }

    SessionState session_state() const { return fifo_sim_.session_state(); }
};

}  // namespace ice
}  // namespace exchange
