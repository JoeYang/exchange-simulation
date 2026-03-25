#pragma once

#include "cme/cme_exchange.h"
#include "cme/cme_products.h"
#include "exchange-sim/exchange_simulator.h"

#include <vector>

namespace exchange {
namespace cme {

// CmeEngine -- convenience alias for the CME exchange engine with default
// matching algorithm and pool sizes.  Used as the EngineT parameter for
// ExchangeSimulator.
template <typename OrderListenerT, typename MdListenerT>
using CmeEngine = CmeExchange<OrderListenerT, MdListenerT>;

// CmeSimulator -- a multi-instrument exchange simulator pre-configured for
// CME Globex.
//
// Wraps ExchangeSimulator with CmeExchange engines and adds:
//   - load_products(): bulk-load CmeProductConfig into instruments
//   - Session lifecycle helpers: start_trading_day, open_market, close_market,
//     end_of_day — matching the simplified CME daily cycle:
//       Closed -> PreOpen -> Continuous -> PreClose -> Closed
template <typename OrderListenerT, typename MdListenerT>
class CmeSimulator
    : public ExchangeSimulator<CmeEngine<OrderListenerT, MdListenerT>,
                               OrderListenerT, MdListenerT> {
    using Base = ExchangeSimulator<CmeEngine<OrderListenerT, MdListenerT>,
                                   OrderListenerT, MdListenerT>;

public:
    using Base::Base;

    // load_products -- create one instrument per CmeProductConfig entry.
    //
    // Converts each CmeProductConfig into an InstrumentConfig and adds it to
    // the simulator.  After adding, configures the per-engine band percentage
    // from the product config.
    void load_products(const std::vector<CmeProductConfig>& products) {
        for (const auto& p : products) {
            InstrumentConfig cfg{
                .id = p.instrument_id,
                .symbol = p.symbol,
                .engine_config =
                    EngineConfig{
                        .tick_size = p.tick_size,
                        .lot_size = p.lot_size,
                        .price_band_low = 0,
                        .price_band_high = 0,
                        .max_order_size = p.max_order_size,
                    },
            };
            this->add_instrument(cfg);

            // Configure per-product dynamic band percentage on the engine.
            auto* engine = this->get_engine(p.instrument_id);
            if (engine) {
                engine->set_band_percentage(p.band_pct);
            }
        }
    }

    // --- Session lifecycle helpers ---
    //
    // These model the simplified CME daily cycle:
    //   start_trading_day (PreOpen) -> open_market (Continuous)
    //   -> close_market (PreClose -> Closed) -> end_of_day (expire DAY orders)

    // start_trading_day -- transition to PreOpen.
    // Orders may be entered but no matching occurs.
    void start_trading_day(Timestamp ts) {
        this->set_session_state(SessionState::PreOpen, ts);
    }

    // open_market -- execute opening auction, then transition to Continuous.
    // All crossing orders collected during PreOpen are uncrossed at the
    // equilibrium price, then continuous matching begins.
    void open_market(Timestamp ts) {
        this->execute_all_auctions(ts);
        this->set_session_state(SessionState::Continuous, ts);
    }

    // close_market -- transition to PreClose, execute closing auction, then
    // transition to Closed.
    //
    // Timestamps are offset by +1 and +2 nanoseconds for the auction and
    // close transitions respectively, so every event has a distinct timestamp.
    void close_market(Timestamp ts) {
        this->set_session_state(SessionState::PreClose, ts);
        this->execute_all_auctions(ts + 1);
        this->set_session_state(SessionState::Closed, ts + 2);
    }

    // end_of_day -- expire all DAY orders and ensure the session is Closed.
    // Should be called after close_market.  GTC orders survive.
    void end_of_day(Timestamp ts) {
        this->trigger_expiry(ts, TimeInForce::DAY);
    }
};

}  // namespace cme
}  // namespace exchange
