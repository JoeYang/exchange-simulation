#pragma once

#include "exchange-sim/exchange_simulator.h"
#include "krx/krx_exchange.h"
#include "krx/krx_products.h"

#include <vector>

namespace exchange {
namespace krx {

// KrxEngine -- convenience alias for FIFO-only KRX exchange engine.
template <typename OrderListenerT, typename MdListenerT>
using KrxEngine = KrxExchange<OrderListenerT, MdListenerT>;

// KrxSimulator -- multi-instrument exchange simulator for KRX derivatives.
//
// Manages two engine pools for dual-session support:
//   - Regular session: 09:00 ~ 15:45 (PreOpen → Continuous → PreClose → Closed)
//   - After-hours session: 16:00 ~ 18:00 (single-price auction or continuous)
//
// Orders in the regular session are fully isolated from after-hours.
// The after-hours session uses separate engine instances.
//
// KRX-specific behaviour configured via load_products():
//   - Tiered daily limits (±8% → ±15% → ±20%) via set_reference_price
//   - Dynamic VI (3%) and static VI (10%) bands
//   - Dynamic order price bands (5%)
//   - Sidecar (programme trading halt) via activate/deactivate
//
// Match algorithm: FIFO only (all KRX derivatives).
template <typename OrderListenerT, typename MdListenerT>
class KrxSimulator {
    using Sim = ExchangeSimulator<KrxEngine<OrderListenerT, MdListenerT>,
                                  OrderListenerT, MdListenerT>;

    Sim regular_sim_;
    Sim after_hours_sim_;

    bool after_hours_active_{false};

    // Product configs retained for after-hours engine setup.
    std::vector<KrxProductConfig> products_;

public:
    KrxSimulator(OrderListenerT& ol, MdListenerT& ml)
        : regular_sim_(ol, ml), after_hours_sim_(ol, ml) {}

    // Non-copyable, non-movable.
    KrxSimulator(const KrxSimulator&) = delete;
    KrxSimulator& operator=(const KrxSimulator&) = delete;
    KrxSimulator(KrxSimulator&&) = delete;
    KrxSimulator& operator=(KrxSimulator&&) = delete;

    // load_products -- create engines for each KrxProductConfig.
    //
    // Sets up both regular and after-hours engine pools. Per-engine
    // configuration: band_pct, vi_dynamic_pct, static_band_pct,
    // reference_price, prior_close_price.
    void load_products(const std::vector<KrxProductConfig>& products) {
        products_ = products;
        for (const auto& p : products) {
            add_product_to_sim(regular_sim_, p);
            add_product_to_sim(after_hours_sim_, p);
        }
    }

    // Set reference and prior close prices for a specific instrument.
    // Must be called before trading starts to enable tiered limits and VI.
    void set_prices(uint32_t instrument_id, Price reference, Price prior_close) {
        configure_engine(regular_sim_.get_engine(instrument_id),
                         reference, prior_close);
        configure_engine(after_hours_sim_.get_engine(instrument_id),
                         reference, prior_close);
    }

    // Set reference/prior close for all instruments at once.
    void set_all_prices(Price reference, Price prior_close) {
        regular_sim_.for_each_instrument(
            [&](InstrumentId, const InstrumentConfig&, auto& engine, auto&) {
                configure_engine(&engine, reference, prior_close);
            });
        after_hours_sim_.for_each_instrument(
            [&](InstrumentId, const InstrumentConfig&, auto& engine, auto&) {
                configure_engine(&engine, reference, prior_close);
            });
    }

    // --- Sidecar control ---

    void activate_sidecar() {
        regular_sim_.for_each_instrument(
            [](InstrumentId, const InstrumentConfig&, auto& engine, auto&) {
                engine.activate_sidecar();
            });
    }

    void deactivate_sidecar() {
        regular_sim_.for_each_instrument(
            [](InstrumentId, const InstrumentConfig&, auto& engine, auto&) {
                engine.deactivate_sidecar();
            });
    }

    // --- Order Routing ---

    void new_order(uint32_t instrument, const OrderRequest& req) {
        active_sim().new_order(instrument, req);
    }

    void cancel_order(uint32_t instrument, OrderId id, Timestamp ts) {
        active_sim().cancel_order(instrument, id, ts);
    }

    void modify_order(uint32_t instrument, const ModifyRequest& req) {
        active_sim().modify_order(instrument, req);
    }

    // --- Engine Access ---

    auto* get_engine(uint32_t id) { return active_sim().get_engine(id); }
    auto* get_regular_engine(uint32_t id) { return regular_sim_.get_engine(id); }
    auto* get_after_hours_engine(uint32_t id) {
        return after_hours_sim_.get_engine(id);
    }

    size_t instrument_count() const { return regular_sim_.instrument_count(); }
    bool is_after_hours() const { return after_hours_active_; }

    // --- Session Lifecycle ---
    //
    // KRX daily cycle:
    //   Regular: Closed → PreOpen → Opening Auction → Continuous
    //            → PreClose → Closing Auction → Closed
    //   After-hours: Closed → Continuous → Closed
    //
    // Timestamps should be monotonically increasing.

    // Start regular session pre-open.
    void start_regular_session(Timestamp ts) {
        after_hours_active_ = false;
        regular_sim_.set_session_state(SessionState::PreOpen, ts);
    }

    // Execute opening auction and transition to continuous trading.
    void open_regular_market(Timestamp ts) {
        regular_sim_.execute_all_auctions(ts);
        regular_sim_.set_session_state(SessionState::Continuous, ts + 1);
    }

    // Enter pre-close phase (closing auction collection).
    void pre_close_regular(Timestamp ts) {
        regular_sim_.set_session_state(SessionState::PreClose, ts);
    }

    // Execute closing auction and close regular session.
    void close_regular_session(Timestamp ts) {
        regular_sim_.execute_all_auctions(ts);
        regular_sim_.set_session_state(SessionState::Closed, ts + 1);
    }

    // Start after-hours session (continuous, limited order types).
    void start_after_hours(Timestamp ts) {
        after_hours_active_ = true;
        after_hours_sim_.set_session_state(SessionState::Continuous, ts);
    }

    // Close after-hours session.
    void close_after_hours(Timestamp ts) {
        after_hours_sim_.set_session_state(SessionState::Closed, ts);
        after_hours_active_ = false;
    }

    // End of day: expire DAY orders in both sessions.
    void end_of_day(Timestamp ts) {
        regular_sim_.trigger_expiry(ts, TimeInForce::DAY);
        after_hours_sim_.trigger_expiry(ts, TimeInForce::DAY);
    }

    // Session state of the currently active session.
    SessionState session_state() const {
        return after_hours_active_
            ? after_hours_sim_.session_state()
            : regular_sim_.session_state();
    }

    // Direct state transition (for testing and advanced use).
    void set_session_state(SessionState state, Timestamp ts) {
        active_sim().set_session_state(state, ts);
    }

private:
    Sim& active_sim() {
        return after_hours_active_ ? after_hours_sim_ : regular_sim_;
    }

    const Sim& active_sim() const {
        return after_hours_active_ ? after_hours_sim_ : regular_sim_;
    }

    void add_product_to_sim(Sim& sim, const KrxProductConfig& p) {
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
        sim.add_instrument(cfg);

        auto* engine = sim.get_engine(p.instrument_id);
        if (engine) {
            engine->set_band_percentage(p.dynamic_band_pct);
            engine->set_vi_dynamic_percentage(p.vi_dynamic_pct);
            engine->set_static_band_percentage(p.vi_static_pct);
        }
    }

    static void configure_engine(
            KrxEngine<OrderListenerT, MdListenerT>* engine,
            Price reference, Price prior_close) {
        if (!engine) return;
        engine->set_reference_price(reference);
        engine->set_prior_close_price(prior_close);
    }
};

}  // namespace krx
}  // namespace exchange
