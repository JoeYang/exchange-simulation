#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_encoder.h"

#include <cstring>
#include <gtest/gtest.h>

namespace exchange {
namespace {

using namespace cme::sbe;
// Selective imports to avoid TimeInForce collision with exchange::TimeInForce.
using cme::sbe::ilink3::EncodeContext;
using cme::sbe::ilink3::DecodeResult;
using cme::sbe::ilink3::MassActionScope;
using cme::sbe::ilink3::decode_ilink3_message;
using cme::sbe::ilink3::encode_new_order;
using cme::sbe::ilink3::encode_cancel_order;
using cme::sbe::ilink3::encode_modify_order;
using cme::sbe::ilink3::encode_mass_cancel;
using cme::sbe::ilink3::encode_exec_new;
using cme::sbe::ilink3::encode_exec_fill;
using cme::sbe::ilink3::encode_exec_cancel;
using cme::sbe::ilink3::encode_exec_reject;
using cme::sbe::ilink3::encode_cancel_reject;
using cme::sbe::ilink3::DecodedNewOrder514;
using cme::sbe::ilink3::DecodedCancelRequest516;
using cme::sbe::ilink3::DecodedReplaceRequest515;
using cme::sbe::ilink3::DecodedMassAction529;
using cme::sbe::ilink3::DecodedExecNew522;
using cme::sbe::ilink3::DecodedExecReject523;
using cme::sbe::ilink3::DecodedExecTrade525;
using cme::sbe::ilink3::DecodedExecCancel534;
using cme::sbe::ilink3::DecodedCancelReject535;

// ---------------------------------------------------------------------------
// Common test context
// ---------------------------------------------------------------------------

EncodeContext make_ilink_ctx() {
    EncodeContext ctx{};
    ctx.seq_num = 42;
    ctx.uuid = 0xDEAD;
    ctx.party_details_list_req_id = 99;
    ctx.security_id = 12345;
    std::strncpy(ctx.sender_id, "ROUNDTRIP", sizeof(ctx.sender_id));
    std::strncpy(ctx.location, "TEST", sizeof(ctx.location));
    return ctx;
}

// ---------------------------------------------------------------------------
// iLink3 round-trips: encode -> decode -> verify
// ---------------------------------------------------------------------------

TEST(ILink3RoundTrip, NewOrderSingle514) {
    auto ctx = make_ilink_ctx();
    OrderRequest req{};
    req.client_order_id = 1001;
    req.account_id = 100;
    req.side = Side::Buy;
    req.type = OrderType::Limit;
    req.tif = TimeInForce::DAY;
    req.price = 1005000;   // 100.50
    req.quantity = 50000;  // 5 contracts
    req.stop_price = 0;
    req.timestamp = 1700000000000000000ll;

    char buf[512];
    size_t n = encode_new_order(buf, req, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedNewOrder514>) {
            EXPECT_NEAR(msg.root.price.to_double(), 100.5, 1e-6);
            EXPECT_EQ(msg.root.order_qty, 5u);
            EXPECT_EQ(msg.root.security_id, 12345);
            EXPECT_EQ(msg.root.side, 1);
            EXPECT_EQ(msg.root.seq_num, 42u);
            EXPECT_EQ(msg.root.order_request_id, 1001u);
            EXPECT_TRUE(msg.root.stop_px.is_null());
            EXPECT_EQ(msg.root.ord_type, 2);  // Limit
            EXPECT_EQ(msg.root.time_in_force, 0);  // Day
        } else {
            ADD_FAILURE() << "Expected DecodedNewOrder514";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, OrderCancelRequest516) {
    auto ctx = make_ilink_ctx();
    char buf[512];
    size_t n = encode_cancel_order(buf, 777, 2002, Side::Sell,
                                    1700000000000000000ll, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedCancelRequest516>) {
            EXPECT_EQ(msg.root.order_id, 777u);
            EXPECT_EQ(msg.root.security_id, 12345);
            EXPECT_EQ(msg.root.side, 2);  // Sell
            EXPECT_EQ(msg.root.order_request_id, 2002u);
        } else {
            ADD_FAILURE() << "Expected DecodedCancelRequest516";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, OrderCancelReplaceRequest515) {
    auto ctx = make_ilink_ctx();
    ModifyRequest req{};
    req.order_id = 555;
    req.client_order_id = 3003;
    req.new_price = 1010000;  // 101.00
    req.new_quantity = 30000;
    req.timestamp = 1700000000000000000ll;

    char buf[512];
    size_t n = encode_modify_order(buf, req, 555, Side::Buy,
                                    OrderType::Limit, TimeInForce::GTC, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedReplaceRequest515>) {
            EXPECT_NEAR(msg.root.price.to_double(), 101.0, 1e-6);
            EXPECT_EQ(msg.root.order_qty, 3u);
            EXPECT_EQ(msg.root.order_id, 555u);
            EXPECT_EQ(msg.root.side, 1);
            EXPECT_TRUE(msg.root.stop_px.is_null());
        } else {
            ADD_FAILURE() << "Expected DecodedReplaceRequest515";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, OrderMassActionRequest529) {
    auto ctx = make_ilink_ctx();
    char buf[512];
    size_t n = encode_mass_cancel(buf, MassActionScope::Instrument,
                                   1700000000000000000ll, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedMassAction529>) {
            EXPECT_EQ(msg.root.mass_action_scope, 1);
            EXPECT_EQ(msg.root.security_id, 12345);
            EXPECT_EQ(msg.root.side, UINT8_NULL);
        } else {
            ADD_FAILURE() << "Expected DecodedMassAction529";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, ExecutionReportNew522) {
    auto ctx = make_ilink_ctx();
    OrderAccepted evt{};
    evt.id = 42;
    evt.client_order_id = 4004;
    evt.ts = 1700000000000000000ll;

    Order order{};
    order.id = 42;
    order.client_order_id = 4004;
    order.price = 1005000;
    order.quantity = 50000;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = TimeInForce::GTC;

    char buf[512];
    size_t n = encode_exec_new(buf, evt, order, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedExecNew522>) {
            EXPECT_EQ(msg.root.order_id, 42u);
            EXPECT_NEAR(msg.root.price.to_double(), 100.5, 1e-6);
            EXPECT_EQ(msg.root.order_qty, 5u);
            EXPECT_EQ(msg.root.side, 1);
            EXPECT_EQ(msg.root.cross_id, UINT64_NULL);
        } else {
            ADD_FAILURE() << "Expected DecodedExecNew522";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, ExecutionReportReject523) {
    auto ctx = make_ilink_ctx();
    OrderRejected evt{};
    evt.client_order_id = 5005;
    evt.ts = 1700000000000000000ll;
    evt.reason = RejectReason::InvalidPrice;

    char buf[1024];
    size_t n = encode_exec_reject(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedExecReject523>) {
            EXPECT_EQ(msg.root.ord_rej_reason,
                      static_cast<uint16_t>(RejectReason::InvalidPrice));
            EXPECT_EQ(msg.root.order_request_id, 5005u);
            EXPECT_TRUE(msg.root.price.is_null());
            EXPECT_EQ(msg.root.order_id, UINT64_NULL);
        } else {
            ADD_FAILURE() << "Expected DecodedExecReject523";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, ExecutionReportTradeOutright525) {
    auto ctx = make_ilink_ctx();
    OrderFilled evt{};
    evt.aggressor_id = 10;
    evt.resting_id = 20;
    evt.price = 1005000;
    evt.quantity = 30000;
    evt.ts = 1700000000000000000ll;

    Order order{};
    order.id = 20;
    order.client_order_id = 6006;
    order.price = 1005000;
    order.quantity = 30000;
    order.filled_quantity = 0;
    order.side = Side::Sell;
    order.type = OrderType::Limit;
    order.tif = TimeInForce::GTC;

    char buf[1024];
    size_t n = encode_exec_fill(buf, evt, order, true, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedExecTrade525>) {
            EXPECT_NEAR(msg.root.last_px.to_double(), 100.5, 1e-6);
            EXPECT_EQ(msg.root.last_qty, 3u);
            EXPECT_EQ(msg.root.order_id, 20u);
            EXPECT_EQ(msg.root.side, 2);  // Sell
            EXPECT_EQ(msg.root.aggressor_indicator, 1);
            EXPECT_EQ(msg.num_fills, 0);  // empty group
        } else {
            ADD_FAILURE() << "Expected DecodedExecTrade525";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, ExecutionReportCancel534) {
    auto ctx = make_ilink_ctx();
    OrderCancelled evt{};
    evt.id = 77;
    evt.ts = 1700000000000000000ll;
    evt.reason = CancelReason::UserRequested;

    Order order{};
    order.id = 77;
    order.client_order_id = 7007;
    order.price = 990000;
    order.quantity = 100000;
    order.filled_quantity = 30000;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.tif = TimeInForce::GTC;

    char buf[512];
    size_t n = encode_exec_cancel(buf, evt, order, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedExecCancel534>) {
            EXPECT_EQ(msg.root.order_id, 77u);
            EXPECT_NEAR(msg.root.price.to_double(), 99.0, 1e-6);
            EXPECT_EQ(msg.root.order_qty, 10u);
            EXPECT_EQ(msg.root.cum_qty, 3u);
        } else {
            ADD_FAILURE() << "Expected DecodedExecCancel534";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

TEST(ILink3RoundTrip, OrderCancelReject535) {
    auto ctx = make_ilink_ctx();
    OrderCancelRejected evt{};
    evt.id = 99;
    evt.client_order_id = 8008;
    evt.ts = 1700000000000000000ll;
    evt.reason = RejectReason::UnknownOrder;

    char buf[1024];
    size_t n = encode_cancel_reject(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    DecodeResult rc = decode_ilink3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedCancelReject535>) {
            EXPECT_EQ(msg.root.order_id, 99u);
            EXPECT_EQ(msg.root.cxl_rej_reason,
                      static_cast<uint16_t>(RejectReason::UnknownOrder));
            EXPECT_EQ(msg.root.order_request_id, 8008u);
        } else {
            ADD_FAILURE() << "Expected DecodedCancelReject535";
        }
    });
    EXPECT_EQ(rc, DecodeResult::kOk);
}

// ---------------------------------------------------------------------------
// MDP3 round-trips: encode -> decode -> verify
// ---------------------------------------------------------------------------

using namespace cme::sbe::mdp3;

TEST(MDP3RoundTrip, MDIncrementalRefreshBook46) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 54321;

    DepthUpdate evt{};
    evt.side = Side::Buy;
    evt.price = 2000000;  // 200.00
    evt.total_qty = 50000;
    evt.order_count = 3;
    evt.action = DepthUpdate::Add;
    evt.ts = 1700000000000000000ll;

    char buf[256];
    size_t n = encode_depth_update(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    auto rc = decode_mdp3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedRefreshBook46>) {
            EXPECT_EQ(msg.root.transact_time, 1700000000000000000ull);
            EXPECT_EQ(msg.num_md_entries, 1);
            EXPECT_NEAR(msg.md_entries[0].md_entry_px.to_double(), 200.0, 1e-6);
            EXPECT_EQ(msg.md_entries[0].md_entry_size, 5);
            EXPECT_EQ(msg.md_entries[0].security_id, 54321);
            EXPECT_EQ(msg.md_entries[0].number_of_orders, 3);
            EXPECT_EQ(msg.md_entries[0].md_update_action,
                      static_cast<uint8_t>(MDUpdateAction::New));
            EXPECT_EQ(msg.md_entries[0].md_entry_type,
                      static_cast<char>(MDEntryTypeBook::Bid));
            EXPECT_EQ(msg.num_order_entries, 0);
        } else {
            ADD_FAILURE() << "Expected DecodedRefreshBook46";
        }
    });
    EXPECT_EQ(rc, mdp3::DecodeResult::kOk);
}

TEST(MDP3RoundTrip, MDIncrementalRefreshTradeSummary48) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 11111;

    Trade evt{};
    evt.price = 3000000;  // 300.00
    evt.quantity = 20000;
    evt.aggressor_id = 1;
    evt.resting_id = 2;
    evt.aggressor_side = Side::Sell;
    evt.ts = 1700000000000000000ll;

    char buf[256];
    size_t n = encode_trade(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    auto rc = decode_mdp3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedTradeSummary48>) {
            EXPECT_EQ(msg.root.transact_time, 1700000000000000000ull);
            EXPECT_EQ(msg.num_md_entries, 1);
            EXPECT_NEAR(msg.md_entries[0].md_entry_px.to_double(), 300.0, 1e-6);
            EXPECT_EQ(msg.md_entries[0].md_entry_size, 2);
            EXPECT_EQ(msg.md_entries[0].security_id, 11111);
            EXPECT_EQ(msg.md_entries[0].aggressor_side,
                      static_cast<uint8_t>(AggressorSide::Sell));
            EXPECT_EQ(msg.num_order_entries, 0);
        } else {
            ADD_FAILURE() << "Expected DecodedTradeSummary48";
        }
    });
    EXPECT_EQ(rc, mdp3::DecodeResult::kOk);
}

TEST(MDP3RoundTrip, SecurityStatus30) {
    Mdp3EncodeContext ctx{};
    ctx.security_id = 99999;
    std::strncpy(ctx.security_group, "ES", sizeof(ctx.security_group));
    std::strncpy(ctx.asset, "ES", sizeof(ctx.asset));

    MarketStatus evt{};
    evt.state = SessionState::Continuous;
    evt.ts = 1700000000000000000ll;

    char buf[128];
    size_t n = encode_market_status(buf, evt, ctx);
    ASSERT_GT(n, 0u);

    auto rc = decode_mdp3_message(buf, n, [&](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, DecodedSecurityStatus30>) {
            EXPECT_EQ(msg.root.transact_time, 1700000000000000000ull);
            EXPECT_EQ(msg.root.security_id, 99999);
            EXPECT_EQ(msg.root.security_trading_status,
                      static_cast<uint8_t>(SecurityTradingStatus::ReadyToTrade));
            EXPECT_EQ(std::string(msg.root.security_group, 2), "ES");
        } else {
            ADD_FAILURE() << "Expected DecodedSecurityStatus30";
        }
    });
    EXPECT_EQ(rc, mdp3::DecodeResult::kOk);
}

}  // namespace
}  // namespace exchange
