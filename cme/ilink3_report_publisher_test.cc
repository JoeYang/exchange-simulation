#include "cme/ilink3_report_publisher.h"
#include "cme/cme_simulator.h"
#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/composite_listener.h"
#include "test-harness/recording_listener.h"

#include <cstring>
#include <gtest/gtest.h>

namespace exchange::cme {
namespace {

using namespace sbe;
// Avoid pulling ilink3::TimeInForce into scope — it conflicts with exchange::TimeInForce.
using sbe::ilink3::EncodeContext;
using sbe::ilink3::EXEC_REPORT_NEW_ID;
using sbe::ilink3::EXEC_REPORT_REJECT_ID;
using sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID;
using sbe::ilink3::EXEC_REPORT_CANCEL_ID;
using sbe::ilink3::ORDER_CANCEL_REJECT_ID;
using sbe::ilink3::ExecutionReportNew522;
using sbe::ilink3::ExecutionReportTradeOutright525;
using sbe::ilink3::ExecutionReportCancel534;

EncodeContext make_ctx() {
    EncodeContext ctx{};
    ctx.uuid = 0x1234;
    ctx.party_details_list_req_id = 1;
    ctx.security_id = 1;  // ES
    std::strncpy(ctx.sender_id, "SIM", sizeof(ctx.sender_id));
    std::strncpy(ctx.location, "TEST", sizeof(ctx.location));
    return ctx;
}

// Decode the SBE header from an EncodedReport.
MessageHeader decode_header(const EncodedReport& r) {
    MessageHeader hdr{};
    MessageHeader::decode_from(r.data.data(), hdr);
    return hdr;
}

// Test fixture that wires publisher + recording listener to CmeSimulator.
class ReportPublisherTest : public ::testing::Test {
protected:
    ILink3ReportPublisher publisher_{make_ctx()};
    RecordingOrderListener recorder_;
    CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>
        composite_{&publisher_, &recorder_};
    RecordingMdListener md_listener_;
    CmeSimulator<
        CompositeOrderListener<ILink3ReportPublisher, RecordingOrderListener>,
        RecordingMdListener>
        sim_{composite_, md_listener_};

    void SetUp() override {
        InstrumentConfig cfg{};
        cfg.id = 1;
        cfg.symbol = "ES";
        cfg.engine_config.tick_size = 250;   // 0.25 tick
        cfg.engine_config.lot_size = 10000;  // 1 contract
        sim_.add_instrument(cfg);
        sim_.set_session_state(SessionState::Continuous, 100);
    }

    OrderRequest make_limit(uint64_t cl_id, Side side, Price price,
                             Quantity qty, Timestamp ts = 1000) {
        return OrderRequest{
            .client_order_id = cl_id,
            .account_id = 100,
            .side = side,
            .type = OrderType::Limit,
            .tif = TimeInForce::GTC,
            .price = price,
            .quantity = qty,
            .stop_price = 0,
            .timestamp = ts,
            .gtd_expiry = 0,
        };
    }

    void submit(uint64_t instrument_id, const OrderRequest& req) {
        publisher_.register_order(req);
        sim_.new_order(instrument_id, req);
    }
};

// ---------------------------------------------------------------------------
// OrderAccepted -> ExecutionReportNew522
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, AcceptedProducesExecNew) {
    auto req = make_limit(1, Side::Buy, 1000000, 10000);
    submit(1, req);

    ASSERT_EQ(publisher_.report_count(), 1u);
    auto hdr = decode_header(publisher_.reports()[0]);
    EXPECT_EQ(hdr.template_id, EXEC_REPORT_NEW_ID);
    EXPECT_EQ(hdr.schema_id, ILINK3_SCHEMA_ID);
    EXPECT_EQ(hdr.version, ILINK3_VERSION);
    EXPECT_EQ(hdr.block_length, ExecutionReportNew522::BLOCK_LENGTH);

    // Decode body and verify key fields.
    ExecutionReportNew522 msg{};
    std::memcpy(&msg, publisher_.reports()[0].data.data() + sizeof(MessageHeader),
                sizeof(msg));
    EXPECT_NEAR(msg.price.to_double(), 100.0, 1e-6);
    EXPECT_EQ(msg.order_qty, 1u);  // 10000 engine qty = 1 contract
    EXPECT_EQ(msg.side, 1);  // Buy
}

// ---------------------------------------------------------------------------
// OrderRejected -> ExecutionReportReject523
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, RejectedProducesExecReject) {
    // Submit with invalid price (0 for limit order triggers reject).
    auto req = make_limit(2, Side::Buy, 0, 10000);
    submit(1, req);

    ASSERT_GE(publisher_.report_count(), 1u);
    auto& last = publisher_.reports().back();
    auto hdr = decode_header(last);
    EXPECT_EQ(hdr.template_id, EXEC_REPORT_REJECT_ID);
}

// ---------------------------------------------------------------------------
// OrderFilled -> ExecutionReportTradeOutright525
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, FillProducesExecTrade) {
    auto buy = make_limit(10, Side::Buy, 1000000, 10000, 1000);
    buy.account_id = 100;
    submit(1, buy);
    auto sell = make_limit(11, Side::Sell, 1000000, 10000, 2000);
    sell.account_id = 200;  // different account to avoid SMP
    submit(1, sell);

    // The matching engine fires on_order_filled for fully-filled orders.
    // Look for trade reports among all generated reports.
    bool found_trade = false;
    for (const auto& r : publisher_.reports()) {
        if (r.template_id == sbe::ilink3::EXEC_REPORT_TRADE_OUTRIGHT_ID) {
            found_trade = true;
            auto hdr = decode_header(r);
            EXPECT_EQ(hdr.block_length, ExecutionReportTradeOutright525::BLOCK_LENGTH);

            ExecutionReportTradeOutright525 msg{};
            std::memcpy(&msg, r.data.data() + sizeof(MessageHeader), sizeof(msg));
            EXPECT_NEAR(msg.last_px.to_double(), 100.0, 1e-6);
            EXPECT_EQ(msg.last_qty, 1u);
        }
    }
    EXPECT_TRUE(found_trade);
}

// ---------------------------------------------------------------------------
// OrderCancelled -> ExecutionReportCancel534
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, CancelProducesExecCancel) {
    auto req = make_limit(20, Side::Buy, 1000000, 10000, 1000);
    submit(1, req);
    ASSERT_EQ(publisher_.report_count(), 1u);

    // Cancel by order id (from the accepted event).
    auto accept_hdr = decode_header(publisher_.reports()[0]);
    ASSERT_EQ(accept_hdr.template_id, EXEC_REPORT_NEW_ID);
    ExecutionReportNew522 new_msg{};
    std::memcpy(&new_msg,
                publisher_.reports()[0].data.data() + sizeof(MessageHeader),
                sizeof(new_msg));
    OrderId oid = new_msg.order_id;

    sim_.cancel_order(1, oid, 2000);

    bool found_cancel = false;
    for (const auto& r : publisher_.reports()) {
        auto hdr = decode_header(r);
        if (hdr.template_id == EXEC_REPORT_CANCEL_ID) {
            found_cancel = true;
            ExecutionReportCancel534 msg{};
            std::memcpy(&msg, r.data.data() + sizeof(MessageHeader), sizeof(msg));
            EXPECT_EQ(msg.order_id, oid);
        }
    }
    EXPECT_TRUE(found_cancel);
}

// ---------------------------------------------------------------------------
// OrderCancelRejected -> OrderCancelReject535
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, CancelRejectProducesCancelReject) {
    // Cancel a non-existent order.
    sim_.cancel_order(1, 99999, 1000);

    ASSERT_GE(publisher_.report_count(), 1u);
    auto& last = publisher_.reports().back();
    auto hdr = decode_header(last);
    EXPECT_EQ(hdr.template_id, ORDER_CANCEL_REJECT_ID);
}

// ---------------------------------------------------------------------------
// Sequence numbers increment
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, SeqNumIncrements) {
    auto req1 = make_limit(30, Side::Buy, 1000000, 10000, 1000);
    submit(1, req1);
    auto req2 = make_limit(31, Side::Buy, 990000, 10000, 2000);
    submit(1, req2);

    ASSERT_GE(publisher_.report_count(), 2u);

    ExecutionReportNew522 msg1{}, msg2{};
    std::memcpy(&msg1,
                publisher_.reports()[0].data.data() + sizeof(MessageHeader),
                sizeof(msg1));
    std::memcpy(&msg2,
                publisher_.reports()[1].data.data() + sizeof(MessageHeader),
                sizeof(msg2));

    EXPECT_EQ(msg1.seq_num, 1u);
    EXPECT_EQ(msg2.seq_num, 2u);
}

// ---------------------------------------------------------------------------
// Clear reports
// ---------------------------------------------------------------------------

TEST_F(ReportPublisherTest, ClearReports) {
    auto req = make_limit(40, Side::Buy, 1000000, 10000);
    submit(1, req);
    EXPECT_GE(publisher_.report_count(), 1u);

    publisher_.clear_reports();
    EXPECT_EQ(publisher_.report_count(), 0u);
}

}  // namespace
}  // namespace exchange::cme
