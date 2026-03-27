#pragma once

#include "test-harness/journal_parser.h"
#include "test-harness/recorded_event.h"
#include "test-harness/recording_listener.h"
#include "exchange-core/matching_engine.h"
#include "exchange-core/match_algo.h"
#include "exchange-core/types.h"

#include <string>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// TestResult -- outcome of running a journal through the test runner.
// ---------------------------------------------------------------------------
struct TestResult {
    bool        passed;
    size_t      action_index;   // 0-based index of the action that failed
    size_t      event_index;    // 0-based index of the expected event that mismatched
    std::string expected;       // to_string() of the expected RecordedEvent
    std::string actual;         // to_string() of the actual RecordedEvent (or "<missing>")
    std::string diff;           // human-readable summary of the mismatch
};

// ---------------------------------------------------------------------------
// Concrete engine types used by the runner.
//
// Template parameters mirror the ParsedConfig defaults:
//   MaxOrders     = 1000
//   MaxPriceLevels= 100
//   MaxOrderIds   = 10000
//
// These are fixed at compile-time.  The runtime ParsedConfig fields
// (tick_size, lot_size, price_band_*) are forwarded through EngineConfig.
// ---------------------------------------------------------------------------

class FifoExchange
    : public MatchingEngine<FifoExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
    using Base = MatchingEngine<FifoExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
public:
    using Base::Base;
};

class ProRataExchange
    : public MatchingEngine<ProRataExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            ProRataMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
    using Base = MatchingEngine<ProRataExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                ProRataMatch,
                                1000, 100, 10000>;
public:
    using Base::Base;
};

// RateThrottledFifoExchange: FIFO matching with per-account rate throttling.
class RateThrottledFifoExchange
    : public MatchingEngine<RateThrottledFifoExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<RateThrottledFifoExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;

    bool is_rate_check_enabled() { return true; }
};

// SmpFifoExchange: FIFO matching with self-match prevention enabled.
// is_self_match() returns true when both orders belong to the same account.
// get_smp_action() returns CancelNewest (cancel the aggressor on self-match).
class SmpFifoExchange
    : public MatchingEngine<SmpFifoExchange,
                            RecordingOrderListener,
                            RecordingMdListener,
                            FifoMatch,
                            /*MaxOrders=*/1000,
                            /*MaxPriceLevels=*/100,
                            /*MaxOrderIds=*/10000> {
public:
    using Base = MatchingEngine<SmpFifoExchange,
                                RecordingOrderListener,
                                RecordingMdListener,
                                FifoMatch,
                                1000, 100, 10000>;
    using Base::Base;

    bool is_self_match(const Order& a, const Order& b) {
        return a.account_id == b.account_id;
    }
    SmpAction get_smp_action() { return SmpAction::CancelNewest; }
};

// ---------------------------------------------------------------------------
// JournalTestRunner
// ---------------------------------------------------------------------------
class JournalTestRunner {
public:
    // Run a journal against a FIFO-priority matching engine.
    TestResult run_fifo(const Journal& journal);

    // Run a journal against a Pro-Rata matching engine.
    TestResult run_pro_rata(const Journal& journal);

    // Run a journal against a FIFO engine with self-match prevention enabled
    // (CancelNewest: the aggressor is cancelled on a self-match).
    TestResult run_smp_fifo(const Journal& journal);

    // Run a journal against a FIFO engine with per-account rate throttling.
    TestResult run_rate_throttled(const Journal& journal);

private:
    // Shared replay + compare implementation.
    template <typename EngineT>
    TestResult run_impl(EngineT& engine,
                        RecordingOrderListener& order_listener,
                        RecordingMdListener& md_listener,
                        const Journal& journal);

    // Dispatch an action to the engine.
    template <typename EngineT>
    void execute_action(EngineT& engine, const ParsedAction& action);

    // Convert a ParsedExpectation to the corresponding RecordedEvent.
    // Throws std::runtime_error for unknown event_type strings.
    RecordedEvent expectation_to_event(const ParsedExpectation& expect);

    // Element-by-element comparison of recorded vs expected event vectors.
    // Returns a passing TestResult if all match; otherwise the first mismatch.
    TestResult compare(const std::vector<RecordedEvent>& recorded,
                       const std::vector<RecordedEvent>& expected,
                       size_t action_index);
};

}  // namespace exchange
