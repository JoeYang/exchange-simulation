#pragma once

#include "krx/fast/fast_decoder.h"
#include "krx/fast/fast_encoder.h"
#include "tools/display_state.h"
#include "tools/recovery_strategy.h"
#include "tools/udp_multicast.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/time.h>

// KrxRecovery -- snapshot recovery via FAST FullSnapshot (template 6).
//
// Joins the KRX snapshot multicast group, waits for a datagram containing
// a FastFullSnapshot matching the target instrument_id, decodes all levels
// into DisplayState, then returns.
//
// Timeout: if no matching snapshot arrives within timeout_sec (default 10s),
// logs a warning and returns with DisplayState unchanged (empty book).
class KrxRecovery : public RecoveryStrategy {
    std::string snapshot_group_;
    uint16_t    snapshot_port_;
    uint32_t    instrument_id_;
    int         timeout_sec_;

public:
    KrxRecovery(std::string group, uint16_t port, uint32_t instrument_id,
                int timeout_sec = 10)
        : snapshot_group_(std::move(group))
        , snapshot_port_(port)
        , instrument_id_(instrument_id)
        , timeout_sec_(timeout_sec) {}

    void recover(const std::string& instrument, DisplayState& ds) override {
        using namespace exchange::krx::fast;

        std::fprintf(stderr, "[KrxRecovery] Joining snapshot channel %s:%u "
                     "for %s (instrument_id=%u)\n",
                     snapshot_group_.c_str(), snapshot_port_,
                     instrument.c_str(), instrument_id_);

        exchange::UdpMulticastReceiver recv;
        try {
            recv.join_group(snapshot_group_.c_str(), snapshot_port_);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[KrxRecovery] Failed to join snapshot group: "
                         "%s\n", e.what());
            return;
        }

        // Set receive timeout.
        struct timeval tv{};
        tv.tv_sec = timeout_sec_;
        tv.tv_usec = 0;
        if (::setsockopt(recv.fd(), SOL_SOCKET, SO_RCVTIMEO,
                         &tv, sizeof(tv)) < 0) {
            std::fprintf(stderr, "[KrxRecovery] Warning: failed to set "
                         "receive timeout\n");
        }

        // Visitor that captures the first matching FullSnapshot.
        struct SnapVisitor : public FastDecoderVisitorBase {
            uint32_t target_id;
            bool found{false};
            FastFullSnapshot result{};

            explicit SnapVisitor(uint32_t id) : target_id(id) {}

            void on_full_snapshot(const FastFullSnapshot& snap) {
                if (snap.instrument_id == target_id) {
                    result = snap;
                    found = true;
                }
            }
        };

        SnapVisitor visitor(instrument_id_);

        constexpr size_t BUF_SIZE = 2048;
        char buf[BUF_SIZE];

        // Track wall-clock time for a hard timeout (the SO_RCVTIMEO only
        // triggers when no data arrives; if non-matching data keeps flowing,
        // we need a separate deadline).
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_sec_);

        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr, "[KrxRecovery] Deadline expired waiting "
                             "for matching snapshot -- starting with empty book\n");
                return;
            }
            ssize_t n = recv.receive(buf, BUF_SIZE);
            if (n <= 0) {
                std::fprintf(stderr, "[KrxRecovery] Timeout waiting for "
                             "snapshot -- starting with empty book\n");
                return;
            }

            if (static_cast<size_t>(n) <= sizeof(exchange::McastSeqHeader))
                continue;

            // Strip McastSeqHeader (4 bytes).
            const uint8_t* msg_buf =
                reinterpret_cast<const uint8_t*>(buf) +
                sizeof(exchange::McastSeqHeader);
            size_t msg_len =
                static_cast<size_t>(n) - sizeof(exchange::McastSeqHeader);

            decode_message(msg_buf, msg_len, visitor);

            if (visitor.found) {
                apply_snapshot(ds, visitor.result);
                std::fprintf(stderr, "[KrxRecovery] Recovered for %s "
                             "(bids=%d, asks=%d, seq=%u)\n",
                             instrument.c_str(),
                             ds.bid_levels, ds.ask_levels,
                             visitor.result.seq_num);
                return;
            }
        }
    }

private:
    static void apply_snapshot(DisplayState& ds,
                               const exchange::krx::fast::FastFullSnapshot& snap) {
        // Clear existing book state.
        ds.bid_levels = 0;
        ds.ask_levels = 0;
        for (int i = 0; i < BOOK_DEPTH; ++i) {
            ds.bids[i] = BookLevel{};
            ds.asks[i] = BookLevel{};
        }

        // Populate bids.
        for (uint8_t i = 0; i < snap.num_bid_levels && i < BOOK_DEPTH; ++i) {
            ds.bids[i].price = snap.bids[i].price;
            ds.bids[i].qty = static_cast<int32_t>(snap.bids[i].quantity);
            ds.bids[i].order_count = static_cast<int32_t>(snap.bids[i].order_count);
            ++ds.bid_levels;
        }

        // Populate asks.
        for (uint8_t i = 0; i < snap.num_ask_levels && i < BOOK_DEPTH; ++i) {
            ds.asks[i].price = snap.asks[i].price;
            ds.asks[i].qty = static_cast<int32_t>(snap.asks[i].quantity);
            ds.asks[i].order_count = static_cast<int32_t>(snap.asks[i].order_count);
            ++ds.ask_levels;
        }

        // Track the snapshot sequence for gap detection.
        ds.last_seq = snap.seq_num;
    }
};
