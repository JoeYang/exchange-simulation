#include "tools/cme_recovery.h"
#include "tools/display_state.h"
#include "tools/udp_multicast.h"
#include "cme/codec/mdp3_encoder.h"

#include <cstring>
#include <thread>

#include "gtest/gtest.h"

namespace {

using namespace exchange;
using namespace exchange::cme::sbe;
using namespace exchange::cme::sbe::mdp3;

constexpr const char* TEST_GROUP = "239.0.31.99";
constexpr uint16_t    TEST_PORT  = 14399;
constexpr int32_t     TEST_SEC_ID = 54321;

// Encode a snapshot with McastSeqHeader prepended, publish on multicast.
void publish_snapshot(UdpMulticastPublisher& pub,
                      const SnapshotEntry* entries, uint16_t num_entries,
                      int32_t security_id = TEST_SEC_ID) {
    char buf[MAX_SNAPSHOT_ENCODED_SIZE];
    size_t n = encode_snapshot(buf, security_id, 1000000000u, 10,
                                entries, num_entries);
    pub.send(buf, n);
}

// --- Test: successful recovery with matching security_id ---

TEST(CmeRecovery, RecoverPopulatesDisplayState) {
    DisplayState ds{};

    // Start publisher in a background thread after a short delay
    // so the receiver has time to join the group.
    std::thread publisher([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        UdpMulticastPublisher pub(TEST_GROUP, TEST_PORT);

        SnapshotEntry entries[] = {
            {45002500, 100000, Side::Buy},   // bid at 4500.25
            {45002400, 200000, Side::Buy},   // bid at 4500.24
            {45002600, 150000, Side::Sell},  // ask at 4500.26
        };

        publish_snapshot(pub, entries, 3);
    });

    CmeRecovery recovery(TEST_GROUP, TEST_PORT, TEST_SEC_ID, /*timeout=*/5);
    recovery.recover("ES", ds);

    publisher.join();

    // Verify bids and asks were populated.
    EXPECT_EQ(ds.bid_levels, 2);
    EXPECT_EQ(ds.ask_levels, 1);

    // Bids: descending price (4500.25 mantissa in PRICE9 > 4500.24).
    EXPECT_EQ(ds.bids[0].price, 45002500LL * ENGINE_TO_PRICE9_FACTOR);
    EXPECT_EQ(ds.bids[0].qty, 10);  // 100000 / PRICE_SCALE
    EXPECT_EQ(ds.bids[1].price, 45002400LL * ENGINE_TO_PRICE9_FACTOR);

    // Asks.
    EXPECT_EQ(ds.asks[0].price, 45002600LL * ENGINE_TO_PRICE9_FACTOR);
    EXPECT_EQ(ds.asks[0].qty, 15);
}

// --- Test: wrong security_id is ignored, then correct one arrives ---

TEST(CmeRecovery, IgnoresWrongSecurityId) {
    DisplayState ds{};

    std::thread publisher([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        UdpMulticastPublisher pub(TEST_GROUP, TEST_PORT + 1);

        // First: wrong security_id.
        SnapshotEntry wrong_entries[] = {
            {99990000, 500000, Side::Buy},
        };
        publish_snapshot(pub, wrong_entries, 1, /*security_id=*/99999);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Second: correct security_id.
        SnapshotEntry correct_entries[] = {
            {45002500, 100000, Side::Buy},
        };
        publish_snapshot(pub, correct_entries, 1);
    });

    CmeRecovery recovery(TEST_GROUP, TEST_PORT + 1, TEST_SEC_ID, /*timeout=*/5);
    recovery.recover("ES", ds);

    publisher.join();

    // Should have the correct snapshot, not the wrong one.
    EXPECT_EQ(ds.bid_levels, 1);
    EXPECT_EQ(ds.bids[0].price, 45002500LL * ENGINE_TO_PRICE9_FACTOR);
}

// --- Test: timeout when no snapshot arrives ---

TEST(CmeRecovery, TimeoutReturnsEmptyBook) {
    DisplayState ds{};

    // Use a port nobody is publishing on, with a short timeout.
    CmeRecovery recovery(TEST_GROUP, TEST_PORT + 2, TEST_SEC_ID, /*timeout=*/1);
    recovery.recover("ES", ds);

    // Book should remain empty.
    EXPECT_EQ(ds.bid_levels, 0);
    EXPECT_EQ(ds.ask_levels, 0);
}

}  // namespace
