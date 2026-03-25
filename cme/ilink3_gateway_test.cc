#include "cme/ilink3_gateway.h"
#include "cme/cme_simulator.h"
#include "cme/cme_products.h"
#include "cme/codec/ilink3_encoder.h"
#include "test-harness/recording_listener.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace exchange::cme {

// ---------------------------------------------------------------------------
// Test fixture: CmeSimulator + ILink3Gateway
// ---------------------------------------------------------------------------

class ILink3GatewayTest : public ::testing::Test {
protected:
    using SimT = CmeSimulator<RecordingOrderListener, RecordingMdListener>;

    void SetUp() override {
        sim_ = std::make_unique<SimT>(ol_, ml_);

        // Create a single product with instrument_id matching our test security_id.
        CmeProductConfig es_cfg{};
        es_cfg.instrument_id = 5678;
        es_cfg.symbol = "ES";
        es_cfg.description = "E-mini S&P 500";
        es_cfg.product_group = "Equity Index";
        es_cfg.tick_size = 2500;
        es_cfg.lot_size = 10000;
        es_cfg.max_order_size = 10000 * 2000;
        es_cfg.band_pct = 5;
        sim_->load_products({es_cfg});
        sim_->start_trading_day(0);
        sim_->open_market(1);

        gw_ = std::make_unique<ILink3Gateway<SimT>>(*sim_);

        // Set up encode context matching the security_id.
        ctx_.seq_num = 1;
        ctx_.uuid = 100;
        ctx_.party_details_list_req_id = 200;
        ctx_.security_id = 5678;
        std::memcpy(ctx_.sender_id, "TESTSENDER", 10);
        std::memcpy(ctx_.location, "US,IL", 5);
    }

    RecordingOrderListener ol_;
    RecordingMdListener ml_;
    std::unique_ptr<SimT> sim_;
    std::unique_ptr<ILink3Gateway<SimT>> gw_;
    sbe::ilink3::EncodeContext ctx_;
    char buf_[2048]{};
};

// ---------------------------------------------------------------------------
// NewOrderSingle -> new_order
// ---------------------------------------------------------------------------

TEST_F(ILink3GatewayTest, NewOrderSingleCreatesOrder) {
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::GTC;
    req.price = 50000000;
    req.quantity = 10000;
    req.timestamp = 1000;
    req.stop_price = 0;
    req.display_qty = 0;

    size_t n = sbe::ilink3::encode_new_order(buf_, req, ctx_);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kOk);

    // Verify the order was accepted.
    ASSERT_GE(ol_.events().size(), 1u);
    auto* accepted = std::get_if<OrderAccepted>(&ol_.events()[0]);
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->client_order_id, 1u);
}

// ---------------------------------------------------------------------------
// OrderCancelRequest -> cancel_order
// ---------------------------------------------------------------------------

TEST_F(ILink3GatewayTest, CancelRequestCancelsOrder) {
    // First place an order.
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::GTC;
    req.price = 50000000;
    req.quantity = 10000;
    req.timestamp = 1000;
    size_t n = sbe::ilink3::encode_new_order(buf_, req, ctx_);
    gw_->process(buf_, n);

    // Get the assigned order ID from the accept event.
    auto* accepted = std::get_if<OrderAccepted>(&ol_.events()[0]);
    ASSERT_NE(accepted, nullptr);
    OrderId order_id = accepted->id;

    // Now cancel it.
    std::memset(buf_, 0, sizeof(buf_));
    ctx_.seq_num = 2;
    n = sbe::ilink3::encode_cancel_order(buf_, order_id, 2, Side::Buy, 2000, ctx_);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kOk);

    // Find the cancel event.
    bool found_cancel = false;
    for (const auto& evt : ol_.events()) {
        if (auto* cancelled = std::get_if<OrderCancelled>(&evt)) {
            EXPECT_EQ(cancelled->id, order_id);
            found_cancel = true;
        }
    }
    EXPECT_TRUE(found_cancel);
}

// ---------------------------------------------------------------------------
// OrderCancelReplaceRequest -> modify_order
// ---------------------------------------------------------------------------

TEST_F(ILink3GatewayTest, ReplaceRequestModifiesOrder) {
    // Place an order.
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::GTC;
    req.price = 50000000;
    req.quantity = 10000;
    req.timestamp = 1000;
    size_t n = sbe::ilink3::encode_new_order(buf_, req, ctx_);
    gw_->process(buf_, n);

    auto* accepted = std::get_if<OrderAccepted>(&ol_.events()[0]);
    ASSERT_NE(accepted, nullptr);
    OrderId order_id = accepted->id;

    // Modify it.
    ModifyRequest mod{};
    mod.order_id = order_id;
    mod.client_order_id = 2;
    mod.new_price = 49000000;
    mod.new_quantity = 20000;
    mod.timestamp = 3000;

    std::memset(buf_, 0, sizeof(buf_));
    ctx_.seq_num = 2;
    n = sbe::ilink3::encode_modify_order(buf_, mod, order_id, Side::Buy,
                                          OrderType::Limit, TimeInForce::GTC, ctx_);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kOk);

    // Find the modify event.
    bool found_modify = false;
    for (const auto& evt : ol_.events()) {
        if (auto* modified = std::get_if<OrderModified>(&evt)) {
            EXPECT_EQ(modified->id, order_id);
            EXPECT_EQ(modified->new_price, 49000000);
            EXPECT_EQ(modified->new_qty, 20000);
            found_modify = true;
        }
    }
    EXPECT_TRUE(found_modify);
}

// ---------------------------------------------------------------------------
// OrderMassActionRequest -> mass_cancel
// ---------------------------------------------------------------------------

TEST_F(ILink3GatewayTest, MassCancelCancelsAllOrders) {
    // Place two orders.
    OrderRequest req1{};
    req1.client_order_id = 1;
    req1.side = Side::Buy;
    req1.type = OrderType::Limit;
    req1.tif = TimeInForce::GTC;
    req1.price = 50000000;
    req1.quantity = 10000;
    req1.timestamp = 1000;
    size_t n = sbe::ilink3::encode_new_order(buf_, req1, ctx_);
    gw_->process(buf_, n);

    OrderRequest req2{};
    req2.client_order_id = 2;
    req2.side = Side::Sell;
    req2.type = OrderType::Limit;
    req2.tif = TimeInForce::GTC;
    req2.price = 51000000;
    req2.quantity = 10000;
    req2.timestamp = 1001;
    ctx_.seq_num = 2;
    std::memset(buf_, 0, sizeof(buf_));
    n = sbe::ilink3::encode_new_order(buf_, req2, ctx_);
    gw_->process(buf_, n);

    // Mass cancel.
    std::memset(buf_, 0, sizeof(buf_));
    ctx_.seq_num = 3;
    n = sbe::ilink3::encode_mass_cancel(buf_, sbe::ilink3::MassActionScope::Instrument,
                                         5000, ctx_);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kOk);

    // Both orders should be cancelled.
    int cancel_count = 0;
    for (const auto& evt : ol_.events()) {
        if (std::holds_alternative<OrderCancelled>(evt)) {
            ++cancel_count;
        }
    }
    EXPECT_EQ(cancel_count, 2);
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST_F(ILink3GatewayTest, UnknownInstrumentReturnsError) {
    OrderRequest req{};
    req.client_order_id = 1;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::GTC;
    req.price = 50000000;
    req.quantity = 10000;
    req.timestamp = 1000;

    // Use a different security_id that doesn't exist.
    auto ctx = ctx_;
    ctx.security_id = 9999;
    size_t n = sbe::ilink3::encode_new_order(buf_, req, ctx);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kUnknownInstrument);
}

TEST_F(ILink3GatewayTest, TruncatedBufferReturnsDecodeError) {
    auto rc = gw_->process(buf_, 4);  // Too short for header
    EXPECT_EQ(rc, GatewayResult::kDecodeError);
}

TEST_F(ILink3GatewayTest, ExecReportReturnsUnsupported) {
    // Encode an ExecutionReportNew522 (exchange->client, not handled by gateway).
    OrderAccepted evt{};
    evt.id = 1;
    evt.client_order_id = 1;
    evt.ts = 1000;

    Order order{};
    order.id = 1;
    order.client_order_id = 1;
    order.price = 50000000;
    order.quantity = 10000;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = TimeInForce::GTC;

    size_t n = sbe::ilink3::encode_exec_new(buf_, evt, order, ctx_);
    ASSERT_GT(n, 0u);

    auto rc = gw_->process(buf_, n);
    EXPECT_EQ(rc, GatewayResult::kUnsupportedMessage);
}

TEST_F(ILink3GatewayTest, CrossingOrderProducesFill) {
    // Place a resting buy order.
    OrderRequest buy{};
    buy.client_order_id = 1;
    buy.side = Side::Buy;
    buy.type = OrderType::Limit;
    buy.tif = TimeInForce::GTC;
    buy.price = 50000000;
    buy.quantity = 10000;
    buy.timestamp = 1000;
    size_t n = sbe::ilink3::encode_new_order(buf_, buy, ctx_);
    gw_->process(buf_, n);

    // Place a crossing sell order from a different account (avoids SMP).
    OrderRequest sell{};
    sell.client_order_id = 2;
    sell.side = Side::Sell;
    sell.type = OrderType::Limit;
    sell.tif = TimeInForce::GTC;
    sell.price = 50000000;
    sell.quantity = 10000;
    sell.timestamp = 2000;
    ctx_.seq_num = 2;
    ctx_.party_details_list_req_id = 300;  // Different account to avoid SMP
    std::memset(buf_, 0, sizeof(buf_));
    n = sbe::ilink3::encode_new_order(buf_, sell, ctx_);
    gw_->process(buf_, n);

    // Verify a fill occurred.
    bool found_fill = false;
    for (const auto& evt : ol_.events()) {
        if (std::holds_alternative<OrderFilled>(evt)) {
            const auto& fill = std::get<OrderFilled>(evt);
            EXPECT_EQ(fill.price, 50000000);
            EXPECT_EQ(fill.quantity, 10000);
            found_fill = true;
        }
    }
    EXPECT_TRUE(found_fill);
}

}  // namespace exchange::cme
