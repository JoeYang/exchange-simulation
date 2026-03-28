#pragma once

#include "exchange-core/composite_listener.h"
#include "krx/fix/krx_fix_exec_publisher.h"
#include "krx/fix/krx_fix_gateway.h"
#include "krx/fast/fast_decoder.h"
#include "krx/fast/fast_publisher.h"
#include "krx/krx_simulator.h"
#include "test-harness/journal_parser.h"
#include "test-harness/recording_listener.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::krx {

// Result of verifying one EXPECT line against pipeline output.
struct KrxE2EResult {
    bool passed{false};
    size_t action_index{0};
    std::string expected;
    std::string actual;
    std::string category;  // "KRX_EXEC" or "KRX_MD"
};

// KrxE2ETestRunner wires up the full KRX E2E pipeline:
//   journal actions -> FIX 4.2 text -> KrxFixGateway -> KrxSimulator
//   -> KrxFixExecPublisher (FIX exec reports) + FastFeedPublisher (FAST)
//   -> parse/decode -> verify against EXPECT lines.
class KrxE2ETestRunner {
public:
    std::vector<KrxE2EResult> run(const Journal& journal);

private:
    // Resolve symbol (e.g. "KS") to instrument_id.
    uint32_t resolve_instrument(const std::string& symbol) const;

    // Build a FIX NewOrderSingle (35=D) text message from journal fields.
    std::string build_fix_new_order(const ParsedAction& action);

    // Build a FIX OrderCancelRequest (35=F) from journal fields.
    std::string build_fix_cancel(const ParsedAction& action);

    // Build a FIX OrderCancelReplaceRequest (35=G) from journal fields.
    std::string build_fix_replace(const ParsedAction& action);

    // Verify a KRX_EXEC_* expectation against collected FIX exec reports.
    KrxE2EResult verify_exec(const ParsedExpectation& expect, size_t action_idx,
                             const std::vector<std::string>& reports,
                             size_t& cursor);

    // Verify a KRX_MD_* expectation against collected FAST packets.
    KrxE2EResult verify_md(const ParsedExpectation& expect, size_t action_idx,
                           const std::vector<fast::FastPacket>& packets,
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

}  // namespace exchange::krx
