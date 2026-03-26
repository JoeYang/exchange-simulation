#pragma once

#include "exchange-core/matching_engine.h"

namespace exchange {
namespace ice {

// IceExchange -- CRTP-derived matching engine for ICE (Intercontinental
// Exchange) products. Overrides the MatchingEngine CRTP hooks with
// ICE-specific behaviour:
//
//   - SMP: tag-based (uses account_id as SMP ID in simulator), configurable
//     action: RTO (cancel resting), ATO (cancel aggressor), CABO (cancel both)
//   - Price bands: IPL (Intra-day Price Limits) -- fixed-point width around
//     last trade price, not percentage-based like CME
//   - Settlement: VWAP over a configurable window (accumulated via on_trade)
//   - Phase validation: reject Market/IOC/FOK in PreOpen (same as CME)
//   - Circuit breaker: transition to Halt, resume to Continuous after hold
//
// Two template specializations cover all ICE products:
//   IceExchange<OL, ML, FifoMatch>    -- energy, softs, equity index
//   IceExchange<OL, ML, GtbprMatch>   -- STIR (Euribor, SONIA, SOFR)
template <
    typename OrderListenerT,
    typename MdListenerT,
    typename MatchAlgoT = FifoMatch,
    size_t MaxOrders = 10000,
    size_t MaxPriceLevels = 1000,
    size_t MaxOrderIds = 100000
>
class IceExchange : public MatchingEngine<
    IceExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                MaxOrders, MaxPriceLevels, MaxOrderIds>,
    OrderListenerT, MdListenerT, MatchAlgoT,
    MaxOrders, MaxPriceLevels, MaxOrderIds> {

    using Base = MatchingEngine<
        IceExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                    MaxOrders, MaxPriceLevels, MaxOrderIds>,
        OrderListenerT, MdListenerT, MatchAlgoT,
        MaxOrders, MaxPriceLevels, MaxOrderIds>;

    // ICE IPL width in fixed-point price units (not percentage).
    Price ipl_width_{0};

    // ICE SMP action (configurable per product).
    SmpAction smp_action_{SmpAction::CancelNewest};

    // VWAP settlement accumulator.
    // Quantities are stored in contract units (divided by PRICE_SCALE) to
    // avoid int64 overflow when accumulating price * quantity products.
    // At PRICE_SCALE=10000, raw price*qty for Brent at $80 * 5000 lots
    // = 800000 * 50000000 = 4e13, which overflows int64 range (~9.2e18)
    // after only ~230k trades. Dividing qty by PRICE_SCALE keeps values
    // in a safe range for realistic session volumes.
    int64_t vwap_volume_{0};       // sum of (qty / PRICE_SCALE)
    int64_t vwap_price_volume_{0}; // sum of (price * qty / PRICE_SCALE)

    // Circuit breaker state.
    bool circuit_breaker_active_{false};
    Timestamp circuit_breaker_resume_ts_{0};

public:
    using Base::Base;

    // --- Configuration setters (called per-product at setup) ---

    void set_ipl_width(Price width) { ipl_width_ = width; }
    Price ipl_width() const { return ipl_width_; }

    void set_smp_action(SmpAction action) { smp_action_ = action; }

    void reset_vwap() {
        vwap_volume_ = 0;
        vwap_price_volume_ = 0;
    }

    // Record a trade into the VWAP accumulator.
    // Called externally (e.g. by an MdListener wrapper) on each Trade event.
    // Divides qty by PRICE_SCALE before accumulating to prevent overflow.
    void record_trade_for_vwap(Price price, Quantity qty) {
        Quantity contracts = qty / PRICE_SCALE;
        vwap_price_volume_ += price * contracts;
        vwap_volume_ += contracts;
    }

    // Calculate VWAP settlement price from accumulated trades.
    // Returns 0 if no trades have been recorded.
    Price calculate_settlement_price() const {
        if (vwap_volume_ == 0) return 0;
        return vwap_price_volume_ / vwap_volume_;
    }

    // Circuit breaker: enter Halt state with a scheduled resume time.
    void trigger_circuit_breaker(Timestamp hold_until, Timestamp ts) {
        circuit_breaker_active_ = true;
        circuit_breaker_resume_ts_ = hold_until;
        this->set_session_state(SessionState::Halt, ts);
    }

    // Resume from circuit breaker hold (called when hold period expires).
    void resume_from_circuit_breaker(Timestamp ts) {
        if (!circuit_breaker_active_) return;
        circuit_breaker_active_ = false;
        circuit_breaker_resume_ts_ = 0;
        this->set_session_state(SessionState::Continuous, ts);
    }

    bool circuit_breaker_active() const { return circuit_breaker_active_; }
    Timestamp circuit_breaker_resume_ts() const {
        return circuit_breaker_resume_ts_;
    }

    // -------------------------------------------------------------------
    // CRTP hook overrides
    // -------------------------------------------------------------------

    // ICE SMP: same account_id = self-match (account_id used as SMP ID
    // in simulator; full ICE uses FIX tag 9821).
    bool is_self_match(const Order& aggressor, const Order& resting) {
        return aggressor.account_id != 0 &&
               aggressor.account_id == resting.account_id;
    }

    // ICE SMP action: RTO (CancelOldest), ATO (CancelNewest), CABO (CancelBoth).
    SmpAction get_smp_action() { return smp_action_; }

    // ICE modify policy: cancel-replace (same as CME).
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }

    // ICE phase validation:
    //   PreOpen / PreClose: reject Market, IOC, FOK
    //   Closed: reject all
    //   Continuous / Halt / others: allow all
    bool is_order_allowed_in_phase(const OrderRequest& req, SessionState state) {
        switch (state) {
            case SessionState::Closed:
                return false;
            case SessionState::PreOpen:
            case SessionState::PreClose:
                if (req.tif == TimeInForce::IOC || req.tif == TimeInForce::FOK)
                    return false;
                if (req.type == OrderType::Market)
                    return false;
                return true;
            default:
                return true;
        }
    }

    // ICE IPL dynamic price bands: ±ipl_width_ (fixed-point) around reference.
    // Returns {0, 0} when bands are disabled or reference is not established.
    std::pair<Price, Price> calculate_dynamic_bands(Price reference) {
        if (reference <= 0 || ipl_width_ <= 0) return {0, 0};
        return {reference - ipl_width_, reference + ipl_width_};
    }

    // ICE session transition: allow all standard transitions.
    // Circuit breaker transitions are managed via trigger/resume methods.
    bool on_session_transition(SessionState /*old_state*/,
                               SessionState /*new_state*/,
                               Timestamp /*ts*/) {
        return true;
    }
};

}  // namespace ice
}  // namespace exchange
