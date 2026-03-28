#include "tools/cme_secdef.h"

#include "cme/cme_products.h"
#include "cme/codec/mdp3_encoder.h"
#include "tools/udp_multicast.h"

#include <thread>

#include "gtest/gtest.h"

namespace exchange {
namespace {

using namespace cme::sbe::mdp3;

// Use a unique port per test to avoid bind conflicts with parallel tests.
constexpr const char* TEST_GROUP = "239.0.31.3";
constexpr uint16_t    TEST_PORT  = 14399;

TEST(CmeSecdefConsumerTest, DiscoverSingleInstrument) {
    auto products = cme::get_cme_products();
    auto& es = products[0];  // ES

    // Encode secdef message.
    char buf[MAX_MDP3_ENCODED_SIZE];
    size_t n = encode_instrument_definition(buf, es, 1);

    // Publish in a background thread.
    std::thread publisher([&] {
        // Small delay to let receiver bind first.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        UdpMulticastPublisher pub(TEST_GROUP, TEST_PORT);
        pub.send(buf, n);
    });

    CmeSecdefConsumer consumer(TEST_GROUP, TEST_PORT);
    auto result = consumer.discover(3);

    publisher.join();

    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result.count("ES"));

    const auto& info = result.at("ES");
    EXPECT_EQ(info.security_id, 1u);
    EXPECT_EQ(info.symbol, "ES");
    EXPECT_EQ(info.tick_size, 2500);
    EXPECT_EQ(info.lot_size, 10000);
    EXPECT_EQ(info.max_order_size, 10000 * 2000);
    EXPECT_EQ(info.match_algorithm, 'F');
    EXPECT_EQ(info.currency, "USD");
    EXPECT_DOUBLE_EQ(info.display_factor, 0.0001);
}

TEST(CmeSecdefConsumerTest, DiscoverMultipleInstruments) {
    auto products = cme::get_cme_products();

    // Encode first 3 products.
    struct EncodedMsg {
        char buf[MAX_MDP3_ENCODED_SIZE];
        size_t len;
    };
    EncodedMsg msgs[3];
    for (int i = 0; i < 3; ++i) {
        msgs[i].len = encode_instrument_definition(
            msgs[i].buf, products[i], 3);
    }

    constexpr uint16_t port = TEST_PORT + 1;
    std::thread publisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        UdpMulticastPublisher pub(TEST_GROUP, port);
        for (int i = 0; i < 3; ++i) {
            pub.send(msgs[i].buf, msgs[i].len);
        }
    });

    CmeSecdefConsumer consumer(TEST_GROUP, port);
    auto result = consumer.discover(3);

    publisher.join();

    ASSERT_EQ(result.size(), 3u);
    EXPECT_TRUE(result.count("ES"));
    EXPECT_TRUE(result.count("NQ"));
    EXPECT_TRUE(result.count("CL"));

    EXPECT_EQ(result.at("ES").security_id, 1u);
    EXPECT_EQ(result.at("NQ").security_id, 2u);
    EXPECT_EQ(result.at("CL").security_id, 3u);
    EXPECT_EQ(result.at("CL").tick_size, 100);
}

TEST(CmeSecdefConsumerTest, TimeoutReturnsEmptyMap) {
    // No publisher -- consumer should timeout with empty map.
    constexpr uint16_t port = TEST_PORT + 2;
    CmeSecdefConsumer consumer(TEST_GROUP, port);
    auto result = consumer.discover(1);  // 1-second timeout
    EXPECT_TRUE(result.empty());
}

TEST(CmeSecdefConsumerTest, DuplicateDeduplication) {
    auto products = cme::get_cme_products();
    auto& es = products[0];

    char buf[MAX_MDP3_ENCODED_SIZE];
    size_t n = encode_instrument_definition(buf, es, 1);

    constexpr uint16_t port = TEST_PORT + 3;
    std::thread publisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        UdpMulticastPublisher pub(TEST_GROUP, port);
        // Send same instrument twice.
        pub.send(buf, n);
        pub.send(buf, n);
    });

    CmeSecdefConsumer consumer(TEST_GROUP, port);
    auto result = consumer.discover(3);

    publisher.join();

    // Should still be exactly 1 instrument (deduped by symbol key).
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.at("ES").security_id, 1u);
}

}  // namespace
}  // namespace exchange
