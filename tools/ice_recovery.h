#pragma once

#include "tools/display_state.h"
#include "tools/recovery_strategy.h"
#include "tools/tcp_client.h"
#include "ice/impact/impact_decoder.h"
#include "ice/impact/impact_messages.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// IceRecovery -- startup recovery via TCP snapshot server.
//
// Connects to the ICE simulator's snapshot port, sends "SNAP <instrument>\n",
// and reads SnapshotOrder messages until BundleEnd with sequence_number=0
// (the snapshot end marker). Populates DisplayState with the recovered book.
//
// On failure (connection refused, timeout, decode error), logs a warning and
// returns with the DisplayState unchanged (empty book fallback).
class IceRecovery : public RecoveryStrategy {
    std::string snapshot_host_;
    uint16_t    snapshot_port_;
    int32_t     instrument_id_;
    int         timeout_ms_;

public:
    IceRecovery(std::string host, uint16_t port, int32_t instrument_id,
                int timeout_ms = 5000)
        : snapshot_host_(std::move(host))
        , snapshot_port_(port)
        , instrument_id_(instrument_id)
        , timeout_ms_(timeout_ms) {}

    void recover(const std::string& instrument, DisplayState& ds) override {
        exchange::TcpClient client;

        if (!client.connect_to(snapshot_host_.c_str(), snapshot_port_)) {
            std::fprintf(stderr,
                "IceRecovery: failed to connect to %s:%u, starting with empty book\n",
                snapshot_host_.c_str(), snapshot_port_);
            return;
        }

        // Send "SNAP <instrument>\n" as a length-prefixed frame.
        std::string request = "SNAP " + instrument + "\n";
        if (!client.send_message(request.data(), request.size())) {
            std::fprintf(stderr, "IceRecovery: failed to send snapshot request\n");
            return;
        }

        // Wait for response with timeout.
        if (!client.poll_readable(timeout_ms_)) {
            std::fprintf(stderr,
                "IceRecovery: timeout waiting for snapshot response (%d ms)\n",
                timeout_ms_);
            return;
        }

        // Read the snapshot response (single length-prefixed frame).
        auto frame = client.recv_message();
        if (frame.empty()) {
            std::fprintf(stderr, "IceRecovery: empty or failed snapshot response\n");
            return;
        }

        // Decode iMpact messages from the frame.
        SnapshotVisitor visitor{ds, instrument_id_};
        exchange::ice::impact::decode_messages(frame.data(), frame.size(), visitor);

        if (!visitor.got_end_marker) {
            std::fprintf(stderr,
                "IceRecovery: snapshot response missing BundleEnd end marker\n");
        } else {
            std::fprintf(stderr,
                "IceRecovery: recovered %d bid + %d ask levels for %s\n",
                ds.bid_levels, ds.ask_levels, instrument.c_str());
        }
    }

private:
    // Visitor that populates DisplayState from SnapshotOrder messages.
    struct SnapshotVisitor {
        DisplayState& ds;
        int32_t target_instrument_id;
        bool got_end_marker{false};

        void on_bundle_start(const exchange::ice::impact::BundleStart&) {}

        void on_bundle_end(const exchange::ice::impact::BundleEnd& be) {
            if (be.sequence_number == 0) {
                got_end_marker = true;
            }
        }

        void on_snapshot_order(const exchange::ice::impact::SnapshotOrder& so) {
            if (so.instrument_id != target_instrument_id) return;

            bool is_bid = (so.side == static_cast<uint8_t>(
                exchange::ice::impact::Side::Buy));
            bool is_delete = false;  // snapshots are always adds
            int32_t qty = static_cast<int32_t>(so.quantity);
            int32_t orders = 1;  // snapshot orders represent one aggregated entry

            if (is_bid) {
                update_book_side(ds.bids, ds.bid_levels,
                                 so.price, qty, orders, is_delete, true);
            } else {
                update_book_side(ds.asks, ds.ask_levels,
                                 so.price, qty, orders, is_delete, false);
            }
        }

        // Stubs for other message types (not expected in snapshot responses).
        void on_add_modify_order(const exchange::ice::impact::AddModifyOrder&) {}
        void on_order_withdrawal(const exchange::ice::impact::OrderWithdrawal&) {}
        void on_deal_trade(const exchange::ice::impact::DealTrade&) {}
        void on_market_status(const exchange::ice::impact::MarketStatus&) {}
        void on_price_level(const exchange::ice::impact::PriceLevel&) {}
        void on_instrument_def(const exchange::ice::impact::InstrumentDefinition&) {}
    };
};
