#pragma once

#include "cme/cme_simulator.h"
#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/mdp3_decoder.h"
#include "cme/ilink3_gateway.h"
#include "cme/ilink3_report_publisher.h"
#include "cme/mdp3_feed_publisher.h"
#include "exchange-core/composite_listener.h"
#include "test-harness/journal_parser.h"
#include "test-harness/recording_listener.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace exchange::cme {

// Result of verifying one expectation line against decoded SBE output.
struct E2EResult {
    bool passed;
    size_t action_index;
    std::string expected;
    std::string actual;
    std::string category;  // "EXEC_REPORT" or "MARKET_DATA"
};

// E2ETestRunner wires up the full CME E2E pipeline:
//   journal actions -> iLink3 SBE encode -> ILink3Gateway decode ->
//   CmeSimulator -> ILink3ReportPublisher + Mdp3FeedPublisher ->
//   iLink3/MDP3 SBE decode -> verify against EXPECT lines.
class E2ETestRunner {
public:
    // Run a parsed journal through the full E2E pipeline.
    // Returns a vector of E2EResult: one per expectation line.
    // On first failure, remaining expectations for that action are still checked.
    std::vector<E2EResult> run(const Journal& journal);

private:
    // Resolve instrument name (e.g. "ES") to InstrumentId.
    // Returns 0 if not found.
    InstrumentId resolve_instrument(const std::string& symbol) const;

    // Execute a session lifecycle action (SESSION_START, SESSION_OPEN, etc).
    void execute_session_action(const ParsedAction& action);

    // Encode an ILINK3_NEW_ORDER journal action into SBE bytes.
    // Returns the encoded byte count.
    size_t encode_new_order(const ParsedAction& action, char* buf);

    // Encode an ILINK3_CANCEL journal action into SBE bytes.
    size_t encode_cancel(const ParsedAction& action, char* buf);

    // Encode an ILINK3_REPLACE journal action into SBE bytes.
    size_t encode_replace(const ParsedAction& action, char* buf);

    // Encode an ILINK3_MASS_CANCEL journal action into SBE bytes.
    size_t encode_mass_cancel(const ParsedAction& action, char* buf);

    // Verify exec report expectations against collected SBE reports.
    E2EResult verify_exec_report(
        const ParsedExpectation& expect, size_t action_idx,
        const std::vector<EncodedReport>& reports, size_t& report_cursor);

    // Verify market data expectations against collected MDP3 packets.
    E2EResult verify_market_data(
        const ParsedExpectation& expect, size_t action_idx,
        const std::vector<Mdp3Packet>& packets, size_t& packet_cursor);

    // Helpers for building OrderRequest from action fields.
    static Side parse_side(const std::string& s);
    static OrderType parse_order_type(const std::string& s);
    static TimeInForce parse_tif(const std::string& s);

    // Instrument symbol -> ID mapping (populated during setup).
    std::unordered_map<std::string, InstrumentId> symbol_map_;

    // Track order IDs assigned by the exchange, keyed by client_order_id.
    // Needed for cancel/replace which require the exchange-assigned order_id.
    std::unordered_map<uint64_t, OrderId> cl_ord_to_exchange_id_;

    // Track order metadata needed for cancel/replace encoding.
    struct OrderMeta {
        OrderId exchange_id{0};
        Side side{Side::Buy};
        OrderType type{OrderType::Limit};
        TimeInForce tif{TimeInForce::DAY};
    };
    std::unordered_map<uint64_t, OrderMeta> order_meta_;
};

}  // namespace exchange::cme
