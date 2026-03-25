#pragma once
#include "exchange-core/matching_engine.h"

namespace exchange {
namespace cme {

// CmeExchange — CME Globex matching engine with CME-specific CRTP overrides.
//
// Parameterized on listener types so it can be used with RecordingListeners
// in tests or production listeners in a live system.
//
// CME-specific behaviour overridden here:
//   - SMP: same account_id = self-match, cancel newest (aggressor)
//   - ModifyPolicy: cancel-replace (standard CME behaviour)
//   - Phase validation: no IOC/FOK/Market during auction collection phases
//   - Dynamic price bands: ±band_pct_% around last trade price
//   - Session transition: default allow-all (can be overridden further)
template <
    typename OrderListenerT,
    typename MdListenerT,
    typename MatchAlgoT = FifoMatch,
    size_t MaxOrders = 10000,
    size_t MaxPriceLevels = 1000,
    size_t MaxOrderIds = 100000
>
class CmeExchange : public MatchingEngine<
    CmeExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                MaxOrders, MaxPriceLevels, MaxOrderIds>,
    OrderListenerT, MdListenerT, MatchAlgoT,
    MaxOrders, MaxPriceLevels, MaxOrderIds> {

    using Base = MatchingEngine<
        CmeExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                    MaxOrders, MaxPriceLevels, MaxOrderIds>,
        OrderListenerT, MdListenerT, MatchAlgoT,
        MaxOrders, MaxPriceLevels, MaxOrderIds>;

    int64_t band_pct_{5};  // default 5% price band; 0 = disabled

public:
    using Base::Base;  // inherit MatchingEngine(EngineConfig, OL&, ML&) constructor

    // Reconfigure the price band percentage at runtime (per-product).
    void set_band_percentage(int64_t pct) { band_pct_ = pct; }
    int64_t band_percentage() const { return band_pct_; }

    // -----------------------------------------------------------------------
    // CRTP overrides
    // -----------------------------------------------------------------------

    // CME SMP: two orders share the same account → self-match.
    bool is_self_match(const Order& aggressor, const Order& resting) {
        return aggressor.account_id == resting.account_id;
    }

    // CME default SMP action: cancel the newest order (aggressor).
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }

    // CME modify policy: cancel-replace (re-enters order, loses time priority).
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }

    // CME phase validation.
    //
    // During auction collection phases (PreOpen, PreClose, VolatilityAuction):
    //   - IOC and FOK are rejected (they cannot participate in uncrossing).
    //   - Market orders are rejected (price unknown until uncross).
    // While Closed: all orders are rejected.
    // During Continuous / OpeningAuction / ClosingAuction / Halt: all allowed.
    bool is_order_allowed_in_phase(const OrderRequest& req, SessionState state) {
        switch (state) {
            case SessionState::Closed:
                return false;
            case SessionState::PreOpen:
            case SessionState::PreClose:
            case SessionState::VolatilityAuction:
                if (req.tif == TimeInForce::IOC || req.tif == TimeInForce::FOK)
                    return false;
                if (req.type == OrderType::Market)
                    return false;
                return true;
            default:
                return true;
        }
    }

    // CME dynamic price bands: ±band_pct_% around reference price.
    //
    // Returns {0, 0} when bands are disabled (band_pct_ == 0) or reference
    // is not yet established (reference <= 0).  The base engine treats {0, 0}
    // as "no band check", so new orders are freely accepted before the first
    // trade establishes a reference.
    std::pair<Price, Price> calculate_dynamic_bands(Price reference) {
        if (reference <= 0 || band_pct_ <= 0) return {0, 0};
        Price band = reference * band_pct_ / 100;
        return {reference - band, reference + band};
    }

    // CME session transition: allow all transitions by default.
    // Override in a subclass or test fixture to inject blocked transitions.
    bool on_session_transition(SessionState /*old_state*/,
                               SessionState /*new_state*/,
                               Timestamp /*ts*/) {
        return true;
    }
};

}  // namespace cme
}  // namespace exchange
