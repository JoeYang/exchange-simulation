#pragma once

#include "exchange-core/matching_engine.h"

namespace exchange {
namespace krx {

// KrxExchange — KRX (Korea Exchange) matching engine with CRTP overrides.
//
// KRX-specific behaviour:
//   - SMP: account-based, CancelNewest (same convention as CME)
//   - Phase validation: reject Market/IOC/FOK in PreOpen, PreClose,
//     and VolatilityAuction; reject all in Closed
//   - Dynamic price bands: ±band_pct_% around last trade (order validation)
//   - VI (Volatility Interruption): vi_dynamic_pct_% (3%) around VI
//     reference, static_band_pct_% (10%) around prior close
//   - Tiered daily limits: ±8% → ±15% → ±20%, widened on each breach
//     via on_daily_limit_hit
//   - Sidecar: on_validate_order rejects programme orders
//     (account_id >= 10000) when sidecar is active
//   - Modify policy: cancel-replace (standard)
//   - Match algorithm: FIFO only
template <
    typename OrderListenerT,
    typename MdListenerT,
    typename MatchAlgoT = FifoMatch,
    size_t MaxOrders = 10000,
    size_t MaxPriceLevels = 1000,
    size_t MaxOrderIds = 100000
>
class KrxExchange : public MatchingEngine<
    KrxExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                MaxOrders, MaxPriceLevels, MaxOrderIds>,
    OrderListenerT, MdListenerT, MatchAlgoT,
    MaxOrders, MaxPriceLevels, MaxOrderIds> {

    using Base = MatchingEngine<
        KrxExchange<OrderListenerT, MdListenerT, MatchAlgoT,
                    MaxOrders, MaxPriceLevels, MaxOrderIds>,
        OrderListenerT, MdListenerT, MatchAlgoT,
        MaxOrders, MaxPriceLevels, MaxOrderIds>;

    // Dynamic price band for order validation: ±band_pct_% around last trade.
    // This is the order-entry band — orders outside this range are rejected.
    // 0 = disabled. Default 5% (wider than VI trigger to allow VI to fire).
    int64_t band_pct_{5};

    // VI dynamic trigger threshold: ±vi_dynamic_pct_% from reference price.
    // Narrower than the order band — trades that breach this trigger VI.
    // Default 3%.
    int64_t vi_dynamic_pct_{3};

    // VI static band: ±static_band_pct_% around prior_close_price_.
    // Triggers VolatilityAuction when breached. Default 10%.
    int64_t static_band_pct_{10};

    // Prior settlement/close price — used as reference for static VI band.
    // Must be set externally before trading begins.
    Price prior_close_price_{0};

    // Sidecar (programme trading halt) state.
    bool sidecar_active_{false};

    // Programme order threshold: account_id >= this value is a programme order.
    static constexpr uint64_t kProgrammeAccountThreshold = 10000;

    // Tiered daily price limits.
    // KRX widens limits on successive breaches: tier 1 (8%) → 2 (15%) → 3 (20%).
    // The reference_price_ is the prior close used to compute absolute limits.
    int current_limit_tier_{0};  // 0 = not yet set, 1/2/3 = active tier

    // Tier percentages (index 0 = tier 1).
    static constexpr int64_t kLimitTierPct[3] = {8, 15, 20};
    static constexpr int kMaxLimitTier = 3;

    Price reference_price_{0};  // prior close for daily limit computation

public:
    using Base::Base;

    // --- Configuration ---

    void set_band_percentage(int64_t pct) { band_pct_ = pct; }
    int64_t band_percentage() const { return band_pct_; }

    void set_vi_dynamic_percentage(int64_t pct) { vi_dynamic_pct_ = pct; }
    int64_t vi_dynamic_percentage() const { return vi_dynamic_pct_; }

    void set_static_band_percentage(int64_t pct) { static_band_pct_ = pct; }
    int64_t static_band_percentage() const { return static_band_pct_; }

    void set_prior_close_price(Price price) { prior_close_price_ = price; }
    Price prior_close_price() const { return prior_close_price_; }

    // Set the reference price used for tiered daily limit computation.
    // Typically the prior day's close. Also initializes tier 1 limits.
    void set_reference_price(Price price) {
        reference_price_ = price;
        if (reference_price_ > 0 && current_limit_tier_ == 0) {
            apply_limit_tier(1);
        }
    }
    Price reference_price() const { return reference_price_; }
    int current_limit_tier() const { return current_limit_tier_; }

    // --- Sidecar control ---

    void activate_sidecar() { sidecar_active_ = true; }
    void deactivate_sidecar() { sidecar_active_ = false; }
    bool sidecar_active() const { return sidecar_active_; }

    // --- CRTP hook overrides ---

    // KRX SMP: same account_id = self-match.
    bool is_self_match(const Order& aggressor, const Order& resting) {
        return aggressor.account_id == resting.account_id;
    }

    // KRX SMP action: cancel newest (aggressor).
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }

    // KRX modify policy: cancel-replace.
    ModifyPolicy get_modify_policy() { return ModifyPolicy::CancelReplace; }

    // KRX phase validation.
    //
    // PreOpen / PreClose / VolatilityAuction:
    //   - Reject IOC, FOK (cannot participate in auction uncrossing)
    //   - Reject Market orders (price unknown until uncross)
    // Closed: reject all (handled by base engine, but belt-and-suspenders)
    // Continuous / Halt / others: allow all
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

    // Sidecar: reject programme orders when sidecar is active.
    // Programme orders are identified by account_id >= 10000.
    bool on_validate_order(const OrderRequest& req) {
        if (sidecar_active_ && req.account_id >= kProgrammeAccountThreshold) {
            return false;
        }
        return true;
    }

    // KRX dynamic price bands: ±band_pct_% around reference (last trade).
    // Returns {0, 0} when disabled or reference not established.
    std::pair<Price, Price> calculate_dynamic_bands(Price reference) {
        if (reference <= 0 || band_pct_ <= 0) return {0, 0};
        Price band = reference * band_pct_ / 100;
        return {reference - band, reference + band};
    }

    // KRX VI trigger: percentage-based threshold check.
    //
    // Dynamic VI: trade_price deviates > vi_dynamic_pct_% from VI reference.
    // Static VI: trade_price deviates > static_band_pct_% from prior close.
    // Either breach triggers VolatilityAuction.
    //
    // The base engine calls this after each fill with last_trade_price_ and
    // vi_reference_price_. We also check the static band against prior_close_.
    bool should_trigger_volatility_auction(Price trade_price,
                                           Price reference_price) {
        // Dynamic VI check: trade vs VI reference price
        if (reference_price > 0 && vi_dynamic_pct_ > 0) {
            Price threshold = reference_price * vi_dynamic_pct_ / 100;
            Price deviation = trade_price > reference_price
                ? trade_price - reference_price
                : reference_price - trade_price;
            if (deviation > threshold) return true;
        }

        // Static band check: trade vs prior close
        if (prior_close_price_ > 0 && static_band_pct_ > 0) {
            Price threshold = prior_close_price_ * static_band_pct_ / 100;
            Price deviation = trade_price > prior_close_price_
                ? trade_price - prior_close_price_
                : prior_close_price_ - trade_price;
            if (deviation > threshold) return true;
        }

        return false;
    }

    // KRX tiered daily limits: widen band on successive breaches.
    //
    // Called by the base engine when a trade executes at a daily limit price.
    // Tier 1: ±8% → Tier 2: ±15% → Tier 3: ±20% (max, no further widening).
    //
    // After widening, transitions back to Continuous so trading can resume
    // at the wider limits. At tier 3, remains in LockLimit (no further widening).
    void on_daily_limit_hit(Side /*side*/, Price /*limit_price*/,
                            Timestamp ts) {
        if (reference_price_ <= 0) return;
        if (current_limit_tier_ >= kMaxLimitTier) return;  // already at widest

        // Widen to next tier
        apply_limit_tier(current_limit_tier_ + 1);

        // Resume trading at wider limits (transition back to Continuous).
        // The base engine already set state to LockLimit; we override back.
        this->set_session_state(SessionState::Continuous, ts);
    }

    // KRX session transition: allow all.
    bool on_session_transition(SessionState /*old_state*/,
                               SessionState /*new_state*/,
                               Timestamp /*ts*/) {
        return true;
    }

private:
    // Apply daily limit tier: compute ±pct% of reference_price and update
    // the engine config's daily_limit_high / daily_limit_low.
    void apply_limit_tier(int tier) {
        if (tier < 1 || tier > kMaxLimitTier) return;
        if (reference_price_ <= 0) return;

        current_limit_tier_ = tier;
        int64_t pct = kLimitTierPct[tier - 1];
        Price band = reference_price_ * pct / 100;
        this->config_.daily_limit_high = reference_price_ + band;
        this->config_.daily_limit_low  = reference_price_ - band;
    }
};

}  // namespace krx
}  // namespace exchange
