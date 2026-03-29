#include "tools/krx_recovery.h"
#include "tools/display_state.h"
#include "tools/udp_multicast.h"
#include "krx/fast/fast_encoder.h"
#include "krx/fast/fast_types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

using namespace exchange;
using namespace exchange::krx::fast;

// Publish a single FullSnapshot on the given multicast group/port.
void publish_test_snapshot(
    const char* group, uint16_t port,
    const FastFullSnapshot& snap,
    std::atomic<bool>& stop)
{
    UdpMulticastPublisher pub(group, port, 1, true);

    while (!stop.load(std::memory_order_relaxed)) {
        uint8_t buf[kMaxFastEncodedSize]{};
        size_t n = encode_full_snapshot(buf, sizeof(buf), snap);
        if (n > 0) {
            pub.send(reinterpret_cast<const char*>(buf), n);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

TEST(KrxRecovery, RecoverFullBook) {
    const char* group = "239.255.0.44";
    uint16_t port = 19044;

    // Build a snapshot with 3 bid and 2 ask levels.
    FastFullSnapshot snap{};
    snap.instrument_id = 1;
    snap.seq_num = 42;
    snap.num_bid_levels = 3;
    snap.num_ask_levels = 2;
    snap.bids[0] = {3275000, 500000, 10};
    snap.bids[1] = {3270000, 300000, 5};
    snap.bids[2] = {3265000, 100000, 2};
    snap.asks[0] = {3280000, 400000, 8};
    snap.asks[1] = {3285000, 200000, 3};
    snap.timestamp = 1711612800000000000LL;

    std::atomic<bool> stop{false};
    std::thread publisher(publish_test_snapshot, group, port,
                          std::cref(snap), std::ref(stop));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Recover.
    KrxRecovery recovery(group, port, 1, /* timeout_sec= */ 5);
    DisplayState ds{};
    recovery.recover("KS", ds);

    stop.store(true, std::memory_order_relaxed);
    publisher.join();

    // Verify book state.
    EXPECT_EQ(ds.bid_levels, 3);
    EXPECT_EQ(ds.ask_levels, 2);
    EXPECT_EQ(ds.bids[0].price, 3275000);
    EXPECT_EQ(ds.bids[0].qty, 500000);
    EXPECT_EQ(ds.bids[0].order_count, 10);
    EXPECT_EQ(ds.bids[2].price, 3265000);
    EXPECT_EQ(ds.asks[0].price, 3280000);
    EXPECT_EQ(ds.asks[1].qty, 200000);
    EXPECT_EQ(ds.last_seq, 42u);
}

TEST(KrxRecovery, TimeoutReturnsEmptyBook) {
    KrxRecovery recovery("239.255.0.45", 19045, 1, /* timeout_sec= */ 1);
    DisplayState ds{};
    recovery.recover("KS", ds);

    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
}

TEST(KrxRecovery, IgnoreNonMatchingInstrument) {
    const char* group = "239.255.0.46";
    uint16_t port = 19046;

    // Publish snapshot for instrument_id=2, but recover for instrument_id=1.
    FastFullSnapshot snap{};
    snap.instrument_id = 2;
    snap.seq_num = 1;
    snap.num_bid_levels = 1;
    snap.bids[0] = {100000, 50000, 1};
    snap.timestamp = 100;

    std::atomic<bool> stop{false};
    std::thread publisher(publish_test_snapshot, group, port,
                          std::cref(snap), std::ref(stop));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    KrxRecovery recovery(group, port, 1, /* timeout_sec= */ 1);
    DisplayState ds{};
    recovery.recover("KS", ds);

    stop.store(true, std::memory_order_relaxed);
    publisher.join();

    // Should timeout with empty book since instrument doesn't match.
    EXPECT_EQ(ds.bid_levels, 0);
}

}  // namespace
