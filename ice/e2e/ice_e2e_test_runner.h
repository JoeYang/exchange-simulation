#pragma once

#include "exchange-core/composite_listener.h"
#include "ice/fix/ice_fix_exec_publisher.h"
#include "ice/fix/ice_fix_gateway.h"
#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_publisher.h"
#include "ice/ice_simulator.h"
#include "test-harness/journal_parser.h"
#include "test-harness/recording_listener.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::ice {

// Result of verifying one EXPECT line against pipeline output.
struct IceE2EResult {
    bool passed{false};
    size_t action_index{0};
    std::string expected;
    std::string actual;
    std::string category;  // "ICE_EXEC" or "ICE_MD"
};

// IceE2ETestRunner wires up the full ICE E2E pipeline:
//   journal actions → FIX 4.2 text → IceFixGateway → IceSimulator
//   → IceFixExecPublisher (FIX exec reports) + ImpactFeedPublisher (iMpact)
//   → parse/decode → verify against EXPECT lines.
class IceE2ETestRunner {
public:
    std::vector<IceE2EResult> run(const Journal& journal);

private:
    // Resolve symbol (e.g. "BRENT") to instrument_id.
    uint32_t resolve_instrument(const std::string& symbol) const;

    // Build a FIX NewOrderSingle (35=D) text message from journal fields.
    std::string build_fix_new_order(const ParsedAction& action);

    // Build a FIX OrderCancelRequest (35=F) from journal fields.
    std::string build_fix_cancel(const ParsedAction& action);

    // Build a FIX OrderCancelReplaceRequest (35=G) from journal fields.
    std::string build_fix_replace(const ParsedAction& action);

    // Verify an ICE_EXEC_* expectation against collected FIX exec reports.
    IceE2EResult verify_exec(const ParsedExpectation& expect, size_t action_idx,
                             const std::vector<std::string>& reports,
                             size_t& cursor);

    // Verify an ICE_MD_* expectation against collected iMpact packets.
    IceE2EResult verify_md(const ParsedExpectation& expect, size_t action_idx,
                           const std::vector<impact::ImpactPacket>& packets,
                           size_t& cursor);

    // Static helpers.
    static Side parse_side(const std::string& s);
    static OrderType parse_order_type(const std::string& s);
    static TimeInForce parse_tif(const std::string& s);

    // Symbol -> instrument_id mapping.
    std::unordered_map<std::string, uint32_t> symbol_map_;

    // Track exchange-assigned order IDs by client_order_id.
    std::unordered_map<uint64_t, OrderId> cl_to_exchange_id_;
};

}  // namespace exchange::ice
