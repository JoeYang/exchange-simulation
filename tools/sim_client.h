#pragma once

// SimClient: TCP connection + ProtocolCodec + order state + P&L + journal writer.
// Used by exchange-trader to connect to cme-sim or ice-sim.

#include "tools/tcp_client.h"
#include "tools/trading_strategy.h"

#include "cme/codec/ilink3_decoder.h"
#include "cme/codec/ilink3_encoder.h"
#include "cme/codec/ilink3_messages.h"
#include "cme/codec/sbe_header.h"
#include "exchange-core/types.h"
#include "ice/fix/fix_encoder.h"
#include "ice/fix/fix_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace exchange {

// ---------------------------------------------------------------------------
// Timestamp helper
// ---------------------------------------------------------------------------

inline Timestamp now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// ProtocolCodec — abstract interface for CME (iLink3 SBE) and ICE (FIX 4.2).
// ---------------------------------------------------------------------------

struct ExecReport {
    enum Type { Accepted, Fill, Cancelled, Rejected, CancelRejected, Replaced };
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
    bool is_aggressor{false};
    Timestamp ts{0};
};

class ProtocolCodec {
public:
    virtual ~ProtocolCodec() = default;

    // Encode client->exchange new order. Returns bytes written to buf.
    virtual size_t encode_new_order(
        char* buf, uint64_t cl_ord_id, Side side, Price price,
        Quantity qty, OrderType type, TimeInForce tif) = 0;

    // Encode client->exchange cancel. Returns bytes written.
    virtual size_t encode_cancel(
        char* buf, uint64_t cl_ord_id, uint64_t order_id, Side side) = 0;

    // Encode client->exchange replace. Returns bytes written.
    virtual size_t encode_replace(
        char* buf, uint64_t cl_ord_id, uint64_t order_id,
        Side side, Price new_price, Quantity new_qty) = 0;

    // Decode an exchange->client response. Returns true on success.
    virtual bool decode_response(
        const char* buf, size_t len, ExecReport& out) = 0;

    // Name of the protocol for journal ACTION lines.
    virtual const char* protocol_name() const = 0;
};

// ---------------------------------------------------------------------------
// CmeCodec — iLink3 SBE encoding/decoding
// ---------------------------------------------------------------------------

class CmeCodec : public ProtocolCodec {
    cme::sbe::ilink3::EncodeContext ctx_{};

public:
    explicit CmeCodec(int32_t security_id, const std::string& account) {
        ctx_.uuid = 1;
        ctx_.seq_num = 1;
        ctx_.security_id = security_id;
        size_t copy_len = std::min(account.size(), sizeof(ctx_.sender_id) - 1);
        std::memcpy(ctx_.sender_id, account.c_str(), copy_len);
        std::memcpy(ctx_.location, "US,IL", 5);
        ctx_.party_details_list_req_id = 1;
    }

    size_t encode_new_order(
        char* buf, uint64_t cl_ord_id, Side side, Price price,
        Quantity qty, OrderType type, TimeInForce tif) override
    {
        OrderRequest req{};
        req.client_order_id = cl_ord_id;
        req.side = side;
        req.type = type;
        req.tif = tif;
        req.price = price;
        req.quantity = qty;
        req.timestamp = now_ns();
        auto n = cme::sbe::ilink3::encode_new_order(buf, req, ctx_);
        ctx_.seq_num++;
        return n;
    }

    size_t encode_cancel(
        char* buf, uint64_t cl_ord_id, uint64_t order_id, Side side) override
    {
        auto n = cme::sbe::ilink3::encode_cancel_order(
            buf, order_id, cl_ord_id, side, now_ns(), ctx_);
        ctx_.seq_num++;
        return n;
    }

    size_t encode_replace(
        char* buf, uint64_t cl_ord_id, uint64_t order_id,
        Side side, Price new_price, Quantity new_qty) override
    {
        ModifyRequest req{};
        req.order_id = order_id;
        req.client_order_id = cl_ord_id;
        req.new_price = new_price;
        req.new_quantity = new_qty;
        req.timestamp = now_ns();
        auto n = cme::sbe::ilink3::encode_modify_order(
            buf, req, order_id, side, OrderType::Limit, TimeInForce::DAY, ctx_);
        ctx_.seq_num++;
        return n;
    }

    bool decode_response(const char* buf, size_t len, ExecReport& out) override {
        using namespace cme::sbe::ilink3;
        bool ok = false;
        auto rc = decode_ilink3_message(buf, len, [&](const auto& decoded) {
            using T = std::decay_t<decltype(decoded)>;
            if constexpr (std::is_same_v<T, DecodedExecNew522>) {
                out.type = ExecReport::Accepted;
                out.order_id = decoded.root.order_id;
                out.cl_ord_id = decode_cl_ord_id(decoded.root.cl_ord_id);
                out.side = decode_side(decoded.root.side);
                out.price = price9_to_engine(decoded.root.price);
                out.qty = wire_qty_to_engine(decoded.root.order_qty);
                out.ts = static_cast<Timestamp>(decoded.root.transact_time);
                ok = true;
            } else if constexpr (std::is_same_v<T, DecodedExecTrade525>) {
                out.type = ExecReport::Fill;
                out.order_id = decoded.root.order_id;
                out.cl_ord_id = decode_cl_ord_id(decoded.root.cl_ord_id);
                out.side = decode_side(decoded.root.side);
                out.price = price9_to_engine(decoded.root.price);
                out.fill_price = price9_to_engine(decoded.root.last_px);
                out.fill_qty = wire_qty_to_engine(decoded.root.last_qty);
                out.leaves_qty = wire_qty_to_engine(decoded.root.leaves_qty);
                out.cum_qty = wire_qty_to_engine(decoded.root.cum_qty);
                out.is_aggressor = decoded.root.aggressor_indicator != 0;
                out.ts = static_cast<Timestamp>(decoded.root.transact_time);
                ok = true;
            } else if constexpr (std::is_same_v<T, DecodedExecCancel534>) {
                out.type = ExecReport::Cancelled;
                out.order_id = decoded.root.order_id;
                out.cl_ord_id = decode_cl_ord_id(decoded.root.cl_ord_id);
                out.ts = static_cast<Timestamp>(decoded.root.transact_time);
                ok = true;
            } else if constexpr (std::is_same_v<T, DecodedExecReject523>) {
                out.type = ExecReport::Rejected;
                out.cl_ord_id = decode_cl_ord_id(decoded.root.cl_ord_id);
                out.ts = static_cast<Timestamp>(decoded.root.transact_time);
                ok = true;
            } else if constexpr (std::is_same_v<T, DecodedCancelReject535>) {
                out.type = ExecReport::CancelRejected;
                out.order_id = decoded.root.order_id;
                out.cl_ord_id = decode_cl_ord_id(decoded.root.cl_ord_id);
                out.ts = static_cast<Timestamp>(decoded.root.transact_time);
                ok = true;
            } else {
                // Ignore unexpected message types
            }
        });
        return ok && rc == DecodeResult::kOk;
    }

    const char* protocol_name() const override { return "ILINK3"; }
};

// ---------------------------------------------------------------------------
// IceCodec — FIX 4.2 encoding/decoding
//
// Client-side FIX messages:
//   NewOrderSingle (35=D), CancelRequest (35=F), ReplaceRequest (35=G)
// Exchange-side responses:
//   ExecutionReport (35=8), parsed via fix_parser.h
// ---------------------------------------------------------------------------

class IceCodec : public ProtocolCodec {
    ice::fix::EncodeContext ctx_{};  // reuse for seq_num tracking
    std::string symbol_;

public:
    IceCodec(const std::string& symbol, const std::string& sender_comp_id,
             const std::string& target_comp_id)
        : symbol_(symbol)
    {
        ctx_.sender_comp_id = sender_comp_id;
        ctx_.target_comp_id = target_comp_id;
        ctx_.symbol = symbol;
    }

    size_t encode_new_order(
        char* buf, uint64_t cl_ord_id, Side side, Price price,
        Quantity qty, OrderType /*type*/, TimeInForce /*tif*/) override
    {
        // FIX 4.2 NewOrderSingle (35=D)
        std::string body;
        body.reserve(256);
        using namespace ice::fix;
        using namespace ice::fix::detail;
        append_tag(body, "35", std::string("D"));
        append_tag(body, "49", ctx_.sender_comp_id);
        append_tag(body, "56", ctx_.target_comp_id);
        append_tag(body, "34", static_cast<uint64_t>(ctx_.next_seq_num++));
        append_tag(body, "52", format_sending_time(now_ns()));
        append_tag(body, "11", cl_ord_id);
        // Tag 1 (Account): required by KRX for SMP (Self-Match Prevention).
        // Without this tag, account_id defaults to 0 and all orders from
        // different traders are treated as self-matches, blocking all fills.
        append_tag(body, "1", ctx_.sender_comp_id);
        append_tag(body, "55", symbol_);
        append_tag(body, "54", encode_side(side));
        append_tag(body, "44", price_to_fix_str(price));
        append_tag(body, "38", qty_to_fix_str(qty));
        append_tag(body, "40", '2');  // OrdType=Limit
        append_tag(body, "59", '0');  // TIF=Day
        std::string msg = assemble_message(body);
        std::memcpy(buf, msg.data(), msg.size());
        return msg.size();
    }

    size_t encode_cancel(
        char* buf, uint64_t cl_ord_id, uint64_t order_id, Side side) override
    {
        // FIX 4.2 OrderCancelRequest (35=F)
        std::string body;
        body.reserve(256);
        using namespace ice::fix;
        using namespace ice::fix::detail;
        append_tag(body, "35", std::string("F"));
        append_tag(body, "49", ctx_.sender_comp_id);
        append_tag(body, "56", ctx_.target_comp_id);
        append_tag(body, "34", static_cast<uint64_t>(ctx_.next_seq_num++));
        append_tag(body, "52", format_sending_time(now_ns()));
        append_tag(body, "41", cl_ord_id);      // OrigClOrdID
        append_tag(body, "11", cl_ord_id);      // ClOrdID
        append_tag(body, "37", order_id);        // OrderID
        append_tag(body, "55", symbol_);
        append_tag(body, "54", encode_side(side));
        std::string msg = assemble_message(body);
        std::memcpy(buf, msg.data(), msg.size());
        return msg.size();
    }

    size_t encode_replace(
        char* buf, uint64_t cl_ord_id, uint64_t order_id,
        Side side, Price new_price, Quantity new_qty) override
    {
        // FIX 4.2 OrderCancelReplaceRequest (35=G)
        std::string body;
        body.reserve(256);
        using namespace ice::fix;
        using namespace ice::fix::detail;
        append_tag(body, "35", std::string("G"));
        append_tag(body, "49", ctx_.sender_comp_id);
        append_tag(body, "56", ctx_.target_comp_id);
        append_tag(body, "34", static_cast<uint64_t>(ctx_.next_seq_num++));
        append_tag(body, "52", format_sending_time(now_ns()));
        append_tag(body, "41", cl_ord_id);      // OrigClOrdID
        append_tag(body, "11", cl_ord_id);      // ClOrdID (new)
        append_tag(body, "37", order_id);        // OrderID
        append_tag(body, "55", symbol_);
        append_tag(body, "54", encode_side(side));
        append_tag(body, "44", price_to_fix_str(new_price));
        append_tag(body, "38", qty_to_fix_str(new_qty));
        append_tag(body, "40", '2');  // OrdType=Limit
        std::string msg = assemble_message(body);
        std::memcpy(buf, msg.data(), msg.size());
        return msg.size();
    }

    bool decode_response(const char* buf, size_t len, ExecReport& out) override {
        auto result = ::ice::fix::parse_fix_message(buf, len);
        if (!result.has_value()) return false;

        const auto& msg = result.value();
        if (msg.msg_type != "8") return false;  // ExecutionReport only

        char exec_type = msg.get_char(150);
        out.cl_ord_id = static_cast<uint64_t>(msg.get_int(11));
        out.order_id = static_cast<uint64_t>(msg.get_int(37));
        out.side = (msg.get_char(54) == '2') ? Side::Sell : Side::Buy;
        out.price = static_cast<Price>(msg.get_double(44) * PRICE_SCALE);

        switch (exec_type) {
            case '0':  // New
                out.type = ExecReport::Accepted;
                out.qty = static_cast<Quantity>(msg.get_double(38) * PRICE_SCALE);
                break;
            case '1':  // PartialFill
            case '2':  // Fill
                out.type = ExecReport::Fill;
                out.fill_price = static_cast<Price>(msg.get_double(31) * PRICE_SCALE);
                out.fill_qty = static_cast<Quantity>(msg.get_double(32) * PRICE_SCALE);
                out.leaves_qty = static_cast<Quantity>(msg.get_double(151) * PRICE_SCALE);
                out.cum_qty = static_cast<Quantity>(msg.get_double(14) * PRICE_SCALE);
                break;
            case '4':  // Cancelled
                out.type = ExecReport::Cancelled;
                break;
            case '5':  // Replace
                out.type = ExecReport::Replaced;
                out.price = static_cast<Price>(msg.get_double(44) * PRICE_SCALE);
                out.qty = static_cast<Quantity>(msg.get_double(38) * PRICE_SCALE);
                break;
            case '8':  // Rejected
                out.type = ExecReport::Rejected;
                break;
            default:
                return false;
        }
        // FIX doesn't include nanosecond ts directly; use local time.
        out.ts = now_ns();
        return true;
    }

    const char* protocol_name() const override { return "ICE_FIX"; }
};

// ---------------------------------------------------------------------------
// SimClient: manages TCP connection, codec, order state, P&L, journal.
// ---------------------------------------------------------------------------

class SimClient {
public:
    SimClient(std::unique_ptr<ProtocolCodec> codec, const std::string& instrument)
        : codec_(std::move(codec)), instrument_(instrument) {}

    ~SimClient() { close_journal(); }

    SimClient(const SimClient&) = delete;
    SimClient& operator=(const SimClient&) = delete;

    // --- Connection ---

    bool connect(const char* host, uint16_t port) {
        if (!tcp_.connect_to(host, port)) return false;
        tcp_.set_nonblocking();
        return true;
    }

    // --- Journal ---

    bool open_journal(const char* path) {
        journal_ = std::fopen(path, "w");
        return journal_ != nullptr;
    }

    void close_journal() {
        if (journal_) { std::fclose(journal_); journal_ = nullptr; }
    }

    // --- Order entry ---

    bool send_new_order(uint64_t cl_ord_id, Side side, Price price, Quantity qty,
                        OrderType type = OrderType::Limit,
                        TimeInForce tif = TimeInForce::DAY)
    {
        size_t n = codec_->encode_new_order(
            send_buf_, cl_ord_id, side, price, qty, type, tif);
        if (n == 0) return false;
        if (!tcp_.send_message(send_buf_, n)) return false;

        // Track pending order
        OpenOrder oo;
        oo.cl_ord_id = cl_ord_id;
        oo.side = side;
        oo.price = price;
        oo.qty = qty;
        oo.created_at = now_ns();
        pending_orders_[cl_ord_id] = oo;

        // Journal ACTION
        write_action("NEW_ORDER", cl_ord_id, side, price, qty);
        return true;
    }

    bool send_cancel(uint64_t cl_ord_id) {
        auto it = orders_.find(cl_ord_id);
        if (it == orders_.end()) {
            // Try pending
            it = pending_orders_.find(cl_ord_id);
            if (it == pending_orders_.end()) return false;
        }
        const auto& order = it->second;
        size_t n = codec_->encode_cancel(
            send_buf_, cl_ord_id, order_id_map_[cl_ord_id], order.side);
        if (n == 0) return false;
        if (!tcp_.send_message(send_buf_, n)) return false;

        write_action_cancel(cl_ord_id);
        return true;
    }

    bool send_replace(uint64_t orig_cl_ord_id, uint64_t new_cl_ord_id,
                      Price new_price, Quantity new_qty)
    {
        auto it = orders_.find(orig_cl_ord_id);
        if (it == orders_.end()) return false;
        const auto& order = it->second;
        size_t n = codec_->encode_replace(
            send_buf_, new_cl_ord_id, order_id_map_[orig_cl_ord_id],
            order.side, new_price, new_qty);
        if (n == 0) return false;
        if (!tcp_.send_message(send_buf_, n)) return false;

        // Track the replacement as pending
        OpenOrder oo = order;
        oo.cl_ord_id = new_cl_ord_id;
        oo.price = new_price;
        oo.qty = new_qty;
        pending_orders_[new_cl_ord_id] = oo;

        write_action_replace(orig_cl_ord_id, new_cl_ord_id, new_price, new_qty);
        return true;
    }

    // --- Response polling (non-blocking) ---

    int poll_responses() {
        int count = 0;
        while (tcp_.poll_readable(0)) {
            auto msg = tcp_.recv_message();
            if (msg.empty()) break;  // disconnected or error

            ExecReport rpt;
            if (!codec_->decode_response(msg.data(), msg.size(), rpt)) {
                ++decode_errors_;
                continue;
            }

            handle_exec_report(rpt);
            ++count;
        }
        return count;
    }

    // --- Graceful shutdown: cancel all open orders ---

    void cancel_all_open() {
        // Collect ids to cancel (can't modify map while iterating)
        std::vector<uint64_t> ids;
        ids.reserve(orders_.size());
        for (const auto& [id, _] : orders_) {
            ids.push_back(id);
        }
        for (uint64_t id : ids) {
            send_cancel(id);
        }
    }

    // --- Accessors ---

    const std::unordered_map<uint64_t, OpenOrder>& open_orders() const { return orders_; }
    int64_t position() const { return position_; }
    int64_t realized_pnl() const { return realized_pnl_; }
    uint32_t fill_count() const { return fill_count_; }
    Price last_fill_price() const { return last_fill_price_; }
    uint32_t decode_errors() const { return decode_errors_; }
    bool is_connected() const { return tcp_.is_connected(); }

    // Formatted status line for stderr display.
    std::string status_line() const {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "%s | pos=%ld | pnl=%ld | fills=%u | open=%zu | errs=%u",
            instrument_.c_str(),
            static_cast<long>(position_ / PRICE_SCALE),
            static_cast<long>(realized_pnl_),
            fill_count_,
            orders_.size(),
            decode_errors_);
        return buf;
    }

    // Provide mutable state for strategy tick.
    ClientState& state() { return state_; }

    void sync_state() {
        state_.open_orders = orders_;
        state_.position = position_;
        state_.realized_pnl = realized_pnl_;
        state_.last_fill_price = last_fill_price_;
        state_.fill_count = fill_count_;
        state_.now = now_ns();
    }

private:
    void handle_exec_report(const ExecReport& rpt) {
        switch (rpt.type) {
            case ExecReport::Accepted: {
                // Move from pending to active
                auto it = pending_orders_.find(rpt.cl_ord_id);
                if (it != pending_orders_.end()) {
                    orders_[rpt.cl_ord_id] = it->second;
                    pending_orders_.erase(it);
                }
                order_id_map_[rpt.cl_ord_id] = rpt.order_id;
                write_expect("ORDER_ACCEPTED", rpt);
                break;
            }
            case ExecReport::Fill: {
                // Update position and P&L
                auto it = orders_.find(rpt.cl_ord_id);
                if (it != orders_.end()) {
                    Side side = it->second.side;
                    Quantity fill_lots = rpt.fill_qty / PRICE_SCALE;
                    if (side == Side::Buy) {
                        // Buying: if we were short, realize P&L on covered portion
                        if (position_ < 0) {
                            Quantity covered = std::min(fill_lots * PRICE_SCALE,
                                                       -position_);
                            realized_pnl_ += (avg_sell_price_ - rpt.fill_price)
                                             * (covered / PRICE_SCALE);
                        }
                        position_ += rpt.fill_qty;
                    } else {
                        if (position_ > 0) {
                            Quantity covered = std::min(fill_lots * PRICE_SCALE,
                                                       position_);
                            realized_pnl_ += (rpt.fill_price - avg_buy_price_)
                                             * (covered / PRICE_SCALE);
                        }
                        position_ -= rpt.fill_qty;
                    }
                    // Track average prices for P&L
                    if (side == Side::Buy) avg_buy_price_ = rpt.fill_price;
                    else avg_sell_price_ = rpt.fill_price;

                    if (rpt.leaves_qty == 0) {
                        orders_.erase(it);
                    }
                }
                last_fill_price_ = rpt.fill_price;
                ++fill_count_;
                write_expect_fill(rpt);
                break;
            }
            case ExecReport::Cancelled:
                orders_.erase(rpt.cl_ord_id);
                write_expect("ORDER_CANCELLED", rpt);
                break;
            case ExecReport::Rejected:
                pending_orders_.erase(rpt.cl_ord_id);
                write_expect("ORDER_REJECTED", rpt);
                break;
            case ExecReport::CancelRejected:
                write_expect("CANCEL_REJECTED", rpt);
                break;
            case ExecReport::Replaced: {
                // The replace created a new order entry; remove old if tracked
                write_expect("ORDER_REPLACED", rpt);
                break;
            }
        }
    }

    // --- Journal writers ---

    void write_action(const char* action_type, uint64_t cl_ord_id,
                      Side side, Price price, Quantity qty)
    {
        if (!journal_) return;
        std::fprintf(journal_,
            "ACTION %s_%s ts=%ld instrument=%s cl_ord_id=%lu "
            "side=%s price=%ld qty=%ld type=LIMIT tif=DAY\n",
            codec_->protocol_name(), action_type,
            static_cast<long>(now_ns()),
            instrument_.c_str(),
            static_cast<unsigned long>(cl_ord_id),
            side == Side::Buy ? "BUY" : "SELL",
            static_cast<long>(price),
            static_cast<long>(qty));
        std::fflush(journal_);
    }

    void write_action_cancel(uint64_t cl_ord_id) {
        if (!journal_) return;
        std::fprintf(journal_,
            "ACTION %s_CANCEL ts=%ld instrument=%s cl_ord_id=%lu\n",
            codec_->protocol_name(),
            static_cast<long>(now_ns()),
            instrument_.c_str(),
            static_cast<unsigned long>(cl_ord_id));
        std::fflush(journal_);
    }

    void write_action_replace(uint64_t orig_id, uint64_t new_id,
                              Price new_price, Quantity new_qty)
    {
        if (!journal_) return;
        std::fprintf(journal_,
            "ACTION %s_REPLACE ts=%ld instrument=%s "
            "orig_cl_ord_id=%lu cl_ord_id=%lu price=%ld qty=%ld\n",
            codec_->protocol_name(),
            static_cast<long>(now_ns()),
            instrument_.c_str(),
            static_cast<unsigned long>(orig_id),
            static_cast<unsigned long>(new_id),
            static_cast<long>(new_price),
            static_cast<long>(new_qty));
        std::fflush(journal_);
    }

    void write_expect(const char* event_type, const ExecReport& rpt) {
        if (!journal_) return;
        std::fprintf(journal_,
            "EXPECT %s ts=%ld order_id=%lu cl_ord_id=%lu\n",
            event_type,
            static_cast<long>(rpt.ts),
            static_cast<unsigned long>(rpt.order_id),
            static_cast<unsigned long>(rpt.cl_ord_id));
        std::fflush(journal_);
    }

    void write_expect_fill(const ExecReport& rpt) {
        if (!journal_) return;
        std::fprintf(journal_,
            "EXPECT EXEC_FILL ts=%ld order_id=%lu cl_ord_id=%lu "
            "fill_price=%ld fill_qty=%ld leaves_qty=%ld\n",
            static_cast<long>(rpt.ts),
            static_cast<unsigned long>(rpt.order_id),
            static_cast<unsigned long>(rpt.cl_ord_id),
            static_cast<long>(rpt.fill_price),
            static_cast<long>(rpt.fill_qty),
            static_cast<long>(rpt.leaves_qty));
        std::fflush(journal_);
    }

    // --- Members ---

    TcpClient tcp_;
    std::unique_ptr<ProtocolCodec> codec_;
    std::string instrument_;
    FILE* journal_{nullptr};

    // Order tracking
    std::unordered_map<uint64_t, OpenOrder> orders_;          // active (accepted)
    std::unordered_map<uint64_t, OpenOrder> pending_orders_;  // sent, not yet ack'd
    std::unordered_map<uint64_t, uint64_t> order_id_map_;     // cl_ord_id -> exchange order_id

    // Position & P&L
    int64_t position_{0};
    int64_t realized_pnl_{0};
    Price avg_buy_price_{0};
    Price avg_sell_price_{0};
    Price last_fill_price_{0};
    uint32_t fill_count_{0};
    uint32_t decode_errors_{0};

    // Strategy state (synced before each tick)
    ClientState state_{};

    // Buffers
    char send_buf_[2048]{};
};

}  // namespace exchange
