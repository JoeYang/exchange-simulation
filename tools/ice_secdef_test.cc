#include "tools/ice_secdef.h"

#include "ice/ice_products.h"
#include "ice/impact/impact_encoder.h"
#include "ice/impact/impact_messages.h"
#include "tools/udp_multicast.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>

namespace exchange {
namespace {

constexpr const char* TEST_GROUP = "239.0.32.99";
constexpr uint16_t    TEST_PORT  = 14499;

// Publish InstrumentDefinition bundles for the given products on loopback.
void publish_secdefs(
    const std::vector<ice::IceProductConfig>& products,
    const char* group, uint16_t port)
{
    using namespace ice::impact;

    UdpMulticastPublisher pub(group, port, 1, true);
    ImpactEncodeContext ctx{};

    char buf[MAX_IMPACT_ENCODED_SIZE];
    for (const auto& p : products) {
        size_t len = encode_instrument_definition(buf, sizeof(buf), p, ctx);
        if (len > 0) {
            pub.send(buf, len);
        }
    }
}

// ---------------------------------------------------------------------------
// Basic discovery: publish 2 products, verify both appear in result.
// ---------------------------------------------------------------------------

TEST(IceSecdefTest, DiscoverTwoProducts) {
    auto all = ice::get_ice_products();
    // Use Brent (id=1) and Gasoil (id=2).
    std::vector<ice::IceProductConfig> products;
    for (const auto& p : all) {
        if (p.symbol == "B" || p.symbol == "G") {
            products.push_back(p);
        }
    }
    ASSERT_EQ(products.size(), 2u);

    // Publish in a background thread (consumer blocks in discover).
    std::thread publisher([&] {
        // Small delay so the consumer has time to join.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        publish_secdefs(products, TEST_GROUP, TEST_PORT);
    });

    IceSecdefConsumer consumer(TEST_GROUP, TEST_PORT);
    auto result = consumer.discover(10);  // 10s timeout

    publisher.join();

    ASSERT_GE(result.size(), 2u);

    // Verify Brent.
    ASSERT_TRUE(result.count("B"));
    const auto& brent = result.at("B");
    EXPECT_EQ(brent.security_id, 1u);
    EXPECT_EQ(brent.symbol, "B");
    EXPECT_EQ(brent.description, "Brent Crude Futures");
    EXPECT_EQ(brent.product_group, "Energy");
    EXPECT_EQ(brent.tick_size, 100);
    EXPECT_EQ(brent.lot_size, 10000);
    EXPECT_EQ(brent.match_algorithm, 'F');
    EXPECT_EQ(brent.currency, "USD");

    // Verify Gasoil.
    ASSERT_TRUE(result.count("G"));
    const auto& gasoil = result.at("G");
    EXPECT_EQ(gasoil.security_id, 2u);
    EXPECT_EQ(gasoil.tick_size, 2500);
}

// ---------------------------------------------------------------------------
// Timeout: no messages -> returns empty map.
// ---------------------------------------------------------------------------

TEST(IceSecdefTest, TimeoutReturnsEmptyMap) {
    // Use a different port so no publisher is sending.
    IceSecdefConsumer consumer("239.0.32.98", 14498);
    auto result = consumer.discover(2);  // 2s timeout
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// Duplicate secdefs are deduplicated.
// ---------------------------------------------------------------------------

TEST(IceSecdefTest, DuplicateDedup) {
    auto all = ice::get_ice_products();
    std::vector<ice::IceProductConfig> products;
    for (const auto& p : all) {
        if (p.symbol == "B") {
            products.push_back(p);
            products.push_back(p);  // duplicate
            break;
        }
    }
    ASSERT_EQ(products.size(), 2u);

    std::thread publisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        publish_secdefs(products, "239.0.32.97", 14497);
    });

    IceSecdefConsumer consumer("239.0.32.97", 14497);
    auto result = consumer.discover(10);

    publisher.join();

    // Only one entry despite two publications.
    EXPECT_EQ(result.size(), 1u);
    EXPECT_TRUE(result.count("B"));
}

// ---------------------------------------------------------------------------
// Interleaved: secdef + market data messages. Only secdef extracted.
// ---------------------------------------------------------------------------

TEST(IceSecdefTest, InterleavedWithMarketData) {
    using namespace ice::impact;

    auto all = ice::get_ice_products();
    ice::IceProductConfig brent;
    for (const auto& p : all) {
        if (p.symbol == "B") { brent = p; break; }
    }

    std::thread publisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        UdpMulticastPublisher pub("239.0.32.96", 14496, 1, true);
        ImpactEncodeContext ctx{};
        ctx.instrument_id = static_cast<int32_t>(brent.instrument_id);
        char buf[MAX_IMPACT_ENCODED_SIZE];

        // Publish a PriceLevel first (market data).
        DepthUpdate evt{};
        evt.side = exchange::Side::Buy;
        evt.price = 750000;
        evt.total_qty = 100000;
        evt.order_count = 5;
        evt.action = DepthUpdate::Add;
        evt.ts = 1000;
        size_t len = encode_depth_update(buf, sizeof(buf), evt, ctx);
        if (len > 0) pub.send(buf, len);

        // Then publish secdef.
        len = encode_instrument_definition(buf, sizeof(buf), brent, ctx);
        if (len > 0) pub.send(buf, len);

        // Then another PriceLevel.
        len = encode_depth_update(buf, sizeof(buf), evt, ctx);
        if (len > 0) pub.send(buf, len);
    });

    IceSecdefConsumer consumer("239.0.32.96", 14496);
    auto result = consumer.discover(10);

    publisher.join();

    // Should discover Brent, ignoring PriceLevel messages.
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result.count("B"));
    EXPECT_EQ(result.at("B").tick_size, 100);
}

}  // namespace
}  // namespace exchange
