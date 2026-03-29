// ice_fix_pipeline_test.cc -- Verifies the full ICE order entry pipeline:
//
//   IceCodec (encode FIX NOS) -> IceFixGateway (decode + dispatch)
//                              -> IceSimulator (matching engine)
//                              -> IceFixExecPublisher (encode FIX response)
//                              -> IceCodec (decode FIX response)
//
// This test reproduces the exact flow from the smoke test:
//   exchange-trader --exchange ice -> ice-sim -> exchange-observer
//
// The root cause of the pipeline failure was that IceFixExecPublisher
// registered orders keyed by client_order_id, but on_order_accepted
// looked them up by engine-assigned OrderId. The two-phase pending_/orders_
// fix ensures the keying is correct.

#include "ice/fix/ice_fix_exec_publisher.h"
#include "ice/fix/ice_fix_gateway.h"
#include "ice/fix/ice_fix_messages.h"
#include "ice/ice_products.h"
#include "ice/ice_simulator.h"
#include "ice/impact/impact_publisher.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_encoder.h"
#include "ice/fix/fix_parser.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <unordered_map>

namespace exchange::ice::fix {
namespace {

// ---------------------------------------------------------------------------
// Minimal IceCodec-style FIX encoder for test use.
// Replicates what the exchange-trader binary sends via IceCodec.
// ---------------------------------------------------------------------------

struct TestFixEncoder {
    EncodeContext ctx;
    std::string symbol;

    TestFixEncoder(const std::string& sym, const std::string& sender,
                   const std::string& target)
        : symbol(sym)
    {
        ctx.sender_comp_id = sender;
        ctx.target_comp_id = target;
        ctx.symbol = sym;
    }

    std::string encode_new_order(uint64_t cl_ord_id, Side side,
                                 Price price, Quantity qty,
                                 uint64_t account = 0) {
        using namespace detail;
        std::string body;
        body.reserve(256);
        append_tag(body, "35", std::string("D"));
        append_tag(body, "49", ctx.sender_comp_id);
        append_tag(body, "56", ctx.target_comp_id);
        append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
        append_tag(body, "52", format_sending_time(0));
        append_tag(body, "11", cl_ord_id);
        if (account > 0) {
            append_tag(body, "1", account);
        }
        append_tag(body, "55", symbol);
        append_tag(body, "54", encode_side(side));
        append_tag(body, "44", price_to_fix_str(price));
        append_tag(body, "38", qty_to_fix_str(qty));
        append_tag(body, "40", '2');  // OrdType=Limit
        append_tag(body, "59", '0');  // TIF=Day
        return assemble_message(body);
    }

    std::string encode_cancel(uint64_t cl_ord_id) {
        using namespace detail;
        std::string body;
        body.reserve(256);
        append_tag(body, "35", std::string("F"));
        append_tag(body, "49", ctx.sender_comp_id);
        append_tag(body, "56", ctx.target_comp_id);
        append_tag(body, "34", static_cast<uint64_t>(ctx.next_seq_num++));
        append_tag(body, "52", format_sending_time(0));
        append_tag(body, "41", cl_ord_id);
        append_tag(body, "11", cl_ord_id);
        append_tag(body, "55", symbol);
        append_tag(body, "54", '1');
        return assemble_message(body);
    }
};

// ---------------------------------------------------------------------------
// Decoded execution report -- same fields as IceCodec::decode_response.
// ---------------------------------------------------------------------------

struct TestExecReport {
    enum Type { Accepted, Fill, Cancelled, Rejected };
    Type type{Accepted};
    uint64_t order_id{0};
    uint64_t cl_ord_id{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity qty{0};
    Price fill_price{0};
    Quantity fill_qty{0};
    Quantity leaves_qty{0};
    Quantity cum_qty{0};
};

bool decode_exec_report(const std::string& fix_msg, TestExecReport& out) {
    auto result = ::ice::fix::parse_fix_message(
        fix_msg.data(), fix_msg.size());
    if (!result.has_value()) return false;
    const auto& msg = result.value();
    if (msg.msg_type != "8") return false;

    char exec_type = msg.get_char(150);
    out.cl_ord_id = static_cast<uint64_t>(msg.get_int(11));
    out.order_id = static_cast<uint64_t>(msg.get_int(37));
    out.side = (msg.get_char(54) == '2') ? Side::Sell : Side::Buy;
    out.price = static_cast<Price>(msg.get_double(44) * PRICE_SCALE);

    switch (exec_type) {
        case '0':
            out.type = TestExecReport::Accepted;
            out.qty = static_cast<Quantity>(
                msg.get_double(38) * PRICE_SCALE);
            break;
        case '1':
        case '2':
            out.type = TestExecReport::Fill;
            out.fill_price = static_cast<Price>(
                msg.get_double(31) * PRICE_SCALE);
            out.fill_qty = static_cast<Quantity>(
                msg.get_double(32) * PRICE_SCALE);
            out.leaves_qty = static_cast<Quantity>(
                msg.get_double(151) * PRICE_SCALE);
            out.cum_qty = static_cast<Quantity>(
                msg.get_double(14) * PRICE_SCALE);
            break;
        case '4':
            out.type = TestExecReport::Cancelled;
            break;
        case '8':
            out.type = TestExecReport::Rejected;
            break;
        default:
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fixture: wires up the same pipeline as ice_sim_runner.cc.
// ---------------------------------------------------------------------------

class IceFixPipelineTest : public ::testing::Test {
protected:
    using Sim = IceSimulator<IceFixExecPublisher, impact::ImpactFeedPublisher>;

    IceFixExecPublisher exec_pub_{"ICE-SIM", "CLIENT", "B"};
    impact::ImpactFeedPublisher md_pub_{1};
    Sim sim_{exec_pub_, md_pub_};

    // SymbolRouter: same pattern as ice_sim_runner.cc
    struct SymbolRouter {
        Sim& sim;
        uint32_t active_instrument{0};

        void new_order(const OrderRequest& req) {
            sim.new_order(active_instrument, req);
        }
        void cancel_order(OrderId id, Timestamp ts) {
            sim.cancel_order(active_instrument, id, ts);
        }
        void modify_order(const ModifyRequest& req) {
            sim.modify_order(active_instrument, req);
        }
        size_t mass_cancel(uint64_t, Timestamp) { return 0; }
    };

    SymbolRouter router_{sim_, 0};
    IceFixGateway<SymbolRouter> gateway_{router_};

    std::unordered_map<std::string, uint32_t> symbol_map_;

    // FIX encoder (mimics IceCodec in exchange-trader)
    TestFixEncoder encoder_{"B", "FIRM_A", "ICE-SIM"};

    static constexpr uint32_t BRENT_ID = 1;

    void SetUp() override {
        auto products = get_ice_products();
        sim_.load_products(products);

        for (const auto& p : products) {
            symbol_map_[p.symbol] = p.instrument_id;
        }

        sim_.start_trading_day(1000);
        sim_.open_market(1100);
        exec_pub_.clear_messages();
    }

    // Simulate the on_message handler from ice_sim_runner.cc:
    // pre-parse FIX, resolve symbol, register order, dispatch to gateway.
    bool dispatch_fix(const std::string& fix_msg, Timestamp ts = 2000) {
        auto pre_parse = ::ice::fix::parse_fix_message(
            fix_msg.data(), fix_msg.size());
        if (!pre_parse.has_value()) return false;

        const auto& msg = pre_parse.value();
        std::string symbol = msg.get_string(tags::Symbol);
        auto it = symbol_map_.find(symbol);
        if (it == symbol_map_.end()) return false;

        router_.active_instrument = it->second;

        // Register order for new orders (35=D).
        if (msg.msg_type == "D") {
            auto nos = FixNewOrderSingle::from_fix(msg);
            uint64_t cl_ord_id = static_cast<uint64_t>(
                std::strtoll(nos.cl_ord_id.c_str(), nullptr, 10));

            Side side;
            fix_to_side(nos.side, side);

            exec_pub_.register_order(
                cl_ord_id,
                fix_price_to_engine(msg.get_string(tags::Price)),
                fix_qty_to_engine(msg.get_string(tags::OrderQty)),
                side);
        }

        auto result = gateway_.on_message(fix_msg.data(), fix_msg.size(), ts);
        return result.ok;
    }
};

// ===========================================================================
// Test 1: Single order accepted.
// Verifies the full encode -> gateway -> engine -> publisher -> decode path.
// ===========================================================================

TEST_F(IceFixPipelineTest, SingleOrderAccepted) {
    // Brent at $80.00 = 800000 fixed-point, qty 1 lot = 10000
    std::string nos = encoder_.encode_new_order(
        1, Side::Buy, 800000, 10000);

    ASSERT_TRUE(dispatch_fix(nos, 2000));

    ASSERT_EQ(exec_pub_.messages().size(), 1u)
        << "Exec publisher must produce an ack for the accepted order";

    TestExecReport rpt;
    ASSERT_TRUE(decode_exec_report(exec_pub_.messages()[0], rpt))
        << "IceCodec must be able to decode the ICE exec report";

    EXPECT_EQ(rpt.type, TestExecReport::Accepted);
    EXPECT_EQ(rpt.cl_ord_id, 1u);
    EXPECT_EQ(rpt.side, Side::Buy);
    EXPECT_EQ(rpt.price, 800000);
    EXPECT_EQ(rpt.qty, 10000);
}

// ===========================================================================
// Test 2: Two crossing orders produce a fill.
// Uses different account IDs to avoid SMP cancellation (ICE uses
// CancelNewest SMP action).
// ===========================================================================

TEST_F(IceFixPipelineTest, CrossingOrdersProduceFill) {
    // Resting sell at $80.00 (account=1)
    std::string sell = encoder_.encode_new_order(
        10, Side::Sell, 800000, 10000, /*account=*/1);
    ASSERT_TRUE(dispatch_fix(sell, 2000));
    ASSERT_EQ(exec_pub_.messages().size(), 1u);
    exec_pub_.clear_messages();

    // Aggressive buy at $80.00 (account=2, different from sell to avoid SMP)
    std::string buy = encoder_.encode_new_order(
        20, Side::Buy, 800000, 10000, /*account=*/2);
    ASSERT_TRUE(dispatch_fix(buy, 2001));

    ASSERT_GE(exec_pub_.messages().size(), 1u)
        << "Must produce at least an ack and a fill";

    // Debug: dump all messages
    for (size_t i = 0; i < exec_pub_.messages().size(); ++i) {
        auto parse = ::ice::fix::parse_fix_message(
            exec_pub_.messages()[i].data(),
            exec_pub_.messages()[i].size());
        if (parse.has_value()) {
            const auto& m = parse.value();
            std::fprintf(stderr, "  msg[%zu]: type=%s exec_type=%c"
                         " cl_ord_id=%ld order_id=%ld\n",
                         i, m.msg_type.c_str(), m.get_char(150),
                         m.get_int(11), m.get_int(37));
        }
    }

    bool found_fill = false;
    for (const auto& msg : exec_pub_.messages()) {
        TestExecReport rpt;
        if (decode_exec_report(msg, rpt) && rpt.type == TestExecReport::Fill) {
            found_fill = true;
            EXPECT_EQ(rpt.fill_price, 800000);
            EXPECT_EQ(rpt.fill_qty, 10000);
            EXPECT_EQ(rpt.leaves_qty, 0);
            break;
        }
    }
    EXPECT_TRUE(found_fill) << "Must produce a fill execution report";
}

// ===========================================================================
// Test 3: Symbol routing -- orders routed to the correct instrument.
// ===========================================================================

TEST_F(IceFixPipelineTest, SymbolRoutesToCorrectInstrument) {
    std::string nos = encoder_.encode_new_order(
        1, Side::Buy, 800000, 10000);
    ASSERT_TRUE(dispatch_fix(nos, 2000));

    auto* engine = sim_.get_fifo_engine(BRENT_ID);
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(engine->active_order_count(), 1u)
        << "Order must be routed to Brent (instrument_id=1)";
}

// ===========================================================================
// Test 4: Unknown symbol is rejected gracefully.
// ===========================================================================

TEST_F(IceFixPipelineTest, UnknownSymbolRejected) {
    TestFixEncoder bad_encoder{"FAKE", "FIRM_A", "ICE-SIM"};
    std::string nos = bad_encoder.encode_new_order(
        1, Side::Buy, 800000, 10000);

    EXPECT_FALSE(dispatch_fix(nos, 2000));
    EXPECT_EQ(exec_pub_.messages().size(), 0u);
}

// ===========================================================================
// Test 5: Cancel through the pipeline.
// ===========================================================================

TEST_F(IceFixPipelineTest, CancelOrderThroughPipeline) {
    std::string nos = encoder_.encode_new_order(
        1, Side::Buy, 800000, 10000);
    ASSERT_TRUE(dispatch_fix(nos, 2000));
    ASSERT_EQ(exec_pub_.messages().size(), 1u);

    TestExecReport ack;
    ASSERT_TRUE(decode_exec_report(exec_pub_.messages()[0], ack));
    EXPECT_EQ(ack.type, TestExecReport::Accepted);
    exec_pub_.clear_messages();

    // Cancel using cl_ord_id=1 (sent as OrigClOrdID in tag 41).
    std::string cancel = encoder_.encode_cancel(1);
    ASSERT_TRUE(dispatch_fix(cancel, 2001));

    ASSERT_EQ(exec_pub_.messages().size(), 1u);
    TestExecReport cancel_rpt;
    ASSERT_TRUE(decode_exec_report(exec_pub_.messages()[0], cancel_rpt));
    EXPECT_EQ(cancel_rpt.type, TestExecReport::Cancelled);
}

// ===========================================================================
// Test 6: SMP triggers when same account crosses itself.
// ICE SMP action is CancelNewest — the aggressive order is cancelled.
// ===========================================================================

TEST_F(IceFixPipelineTest, SmpCancelsSameAccount) {
    // Resting sell at $80.00, account=1
    std::string sell = encoder_.encode_new_order(
        10, Side::Sell, 800000, 10000, /*account=*/1);
    ASSERT_TRUE(dispatch_fix(sell, 2000));
    exec_pub_.clear_messages();

    // Aggressive buy at $80.00, SAME account=1 — triggers SMP
    std::string buy = encoder_.encode_new_order(
        20, Side::Buy, 800000, 10000, /*account=*/1);
    ASSERT_TRUE(dispatch_fix(buy, 2001));

    // With CancelNewest, the aggressive order gets cancelled.
    // There should be NO fill — just an ack + cancel for the aggressor.
    bool found_fill = false;
    bool found_cancel = false;
    for (const auto& msg : exec_pub_.messages()) {
        TestExecReport rpt;
        if (decode_exec_report(msg, rpt)) {
            if (rpt.type == TestExecReport::Fill) found_fill = true;
            if (rpt.type == TestExecReport::Cancelled) found_cancel = true;
        }
    }
    EXPECT_FALSE(found_fill) << "SMP must prevent the fill";
    EXPECT_TRUE(found_cancel) << "SMP must cancel the aggressive order";
}

}  // namespace
}  // namespace exchange::ice::fix
