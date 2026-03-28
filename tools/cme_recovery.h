#pragma once

#include "cme/codec/mdp3_decoder.h"
#include "cme/codec/mdp3_encoder.h"
#include "tools/display_state.h"
#include "tools/recovery_strategy.h"
#include "tools/udp_multicast.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/time.h>

// CmeRecovery -- snapshot recovery via MDP3 SnapshotFullRefreshOrderBook53.
//
// Joins a snapshot multicast group, waits for a datagram containing a
// SnapshotFullRefreshOrderBook53 message matching the target security_id,
// decodes all entries into DisplayState, then returns.
//
// Timeout: if no matching snapshot arrives within timeout_sec (default 10s),
// logs a warning and returns with DisplayState unchanged (empty book).
class CmeRecovery : public RecoveryStrategy {
    std::string snapshot_group_;
    uint16_t    snapshot_port_;
    int32_t     security_id_;
    int         timeout_sec_;

public:
    CmeRecovery(std::string group, uint16_t port, int32_t security_id,
                int timeout_sec = 10)
        : snapshot_group_(std::move(group))
        , snapshot_port_(port)
        , security_id_(security_id)
        , timeout_sec_(timeout_sec) {}

    void recover(const std::string& instrument, DisplayState& ds) override {
        using namespace exchange;
        using namespace exchange::cme::sbe;
        using namespace exchange::cme::sbe::mdp3;

        std::fprintf(stderr, "[CmeRecovery] Joining snapshot channel %s:%u "
                     "for %s (security_id=%d)\n",
                     snapshot_group_.c_str(), snapshot_port_,
                     instrument.c_str(), security_id_);

        UdpMulticastReceiver recv;
        try {
            recv.join_group(snapshot_group_.c_str(), snapshot_port_);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[CmeRecovery] Failed to join snapshot group: "
                         "%s\n", e.what());
            return;
        }

        // Set receive timeout.
        struct timeval tv{};
        tv.tv_sec = timeout_sec_;
        tv.tv_usec = 0;
        if (::setsockopt(recv.fd(), SOL_SOCKET, SO_RCVTIMEO,
                         &tv, sizeof(tv)) < 0) {
            std::fprintf(stderr, "[CmeRecovery] Warning: failed to set "
                         "receive timeout\n");
        }

        // Receive loop: keep reading datagrams until we get a matching snapshot.
        constexpr size_t BUF_SIZE = MAX_SNAPSHOT_ENCODED_SIZE + sizeof(McastSeqHeader);
        char buf[BUF_SIZE];

        for (;;) {
            ssize_t n = recv.receive(buf, BUF_SIZE);
            if (n <= 0) {
                std::fprintf(stderr, "[CmeRecovery] Timeout waiting for "
                             "snapshot — starting with empty book\n");
                return;
            }

            if (static_cast<size_t>(n) <= sizeof(McastSeqHeader)) continue;

            // Strip McastSeqHeader (4 bytes).
            const char* sbe_data = buf + sizeof(McastSeqHeader);
            size_t sbe_len = static_cast<size_t>(n) - sizeof(McastSeqHeader);

            // Decode SBE header.
            if (sbe_len < sizeof(MessageHeader)) continue;

            MessageHeader hdr{};
            const char* body = MessageHeader::decode_from(sbe_data, hdr);
            size_t body_len = sbe_len - sizeof(MessageHeader);

            if (hdr.template_id != SNAPSHOT_FULL_REFRESH_ORDER_BOOK_53_ID)
                continue;

            DecodedSnapshot53 snap{};
            auto rc = decode_snapshot_53(body, body_len, hdr, snap);
            if (rc != DecodeResult::kOk) continue;

            // Check security_id.
            if (snap.root.security_id != security_id_) continue;

            // Populate DisplayState from snapshot entries.
            apply_snapshot(ds, snap);

            std::fprintf(stderr, "[CmeRecovery] Recovered %u levels for "
                         "%s (bids=%d, asks=%d)\n",
                         snap.num_md_entries, instrument.c_str(),
                         ds.bid_levels, ds.ask_levels);
            return;
        }
    }

private:
    static void apply_snapshot(DisplayState& ds,
                               const exchange::cme::sbe::mdp3::DecodedSnapshot53& snap) {
        using namespace exchange::cme::sbe;
        using namespace exchange::cme::sbe::mdp3;

        // Clear existing book state.
        ds.bid_levels = 0;
        ds.ask_levels = 0;
        for (int i = 0; i < BOOK_DEPTH; ++i) {
            ds.bids[i] = BookLevel{};
            ds.asks[i] = BookLevel{};
        }

        for (uint16_t i = 0; i < snap.num_md_entries; ++i) {
            const auto& e = snap.md_entries[i];

            // Convert PRICE9 mantissa back to display value (mantissa itself).
            int64_t price = e.md_entry_px.mantissa;
            int32_t qty = e.md_display_qty;

            bool is_bid = (e.md_entry_type == static_cast<char>(MDEntryTypeBook::Bid));

            if (is_bid) {
                update_book_side(ds.bids, ds.bid_levels,
                                 price, qty, 1, false, true);
            } else {
                update_book_side(ds.asks, ds.ask_levels,
                                 price, qty, 1, false, false);
            }
        }
    }
};
