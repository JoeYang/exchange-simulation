#include "tools/krx_secdef.h"
#include "tools/udp_multicast.h"
#include "krx/fast/fast_encoder.h"
#include "krx/fast/fast_types.h"
#include "krx/krx_products.h"

#include <gtest/gtest.h>

#include <cstring>
#include <thread>
#include <vector>

namespace exchange {
namespace {

// Publish FAST-encoded secdef messages on the given multicast group/port.
// Runs in a background thread, sends all products then sleeps and repeats
// until stop is signaled.
void publish_test_secdefs(
    const char* group, uint16_t port,
    const std::vector<krx::KrxProductConfig>& products,
    std::atomic<bool>& stop)
{
    using namespace krx::fast;

    UdpMulticastPublisher pub(group, port, 1, true);

    auto total = static_cast<uint32_t>(products.size());
    while (!stop.load(std::memory_order_relaxed)) {
        for (const auto& p : products) {
            FastInstrumentDef def{};
            def.instrument_id = p.instrument_id;
            std::memset(def.symbol, 0, sizeof(def.symbol));
            auto sym_len = std::min(p.symbol.size(), sizeof(def.symbol));
            std::memcpy(def.symbol, p.symbol.c_str(), sym_len);
            std::memset(def.description, 0, sizeof(def.description));
            auto desc_len = std::min(p.description.size(), sizeof(def.description));
            std::memcpy(def.description, p.description.c_str(), desc_len);
            def.product_group = static_cast<uint8_t>(p.product_group);
            def.tick_size = p.tick_size;
            def.lot_size = p.lot_size;
            def.max_order_size = p.max_order_size;
            def.total_instruments = total;
            def.timestamp = 1711612800000000000LL;

            uint8_t buf[kMaxFastEncodedSize]{};
            size_t n = encode_instrument_def(buf, sizeof(buf), def);
            if (n > 0) {
                pub.send(reinterpret_cast<const char*>(buf), n);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

TEST(KrxSecdefConsumer, DiscoverAllProducts) {
    // Use a test-only multicast group to avoid conflicts.
    const char* group = "239.255.0.42";
    uint16_t port = 19042;

    auto products = krx::get_krx_products();
    ASSERT_FALSE(products.empty());

    // Start publisher in background thread.
    std::atomic<bool> stop{false};
    std::thread publisher(publish_test_secdefs, group, port, products, std::ref(stop));

    // Give publisher time to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Discover instruments.
    KrxSecdefConsumer consumer(group, port);
    auto instruments = consumer.discover(/* timeout_secs= */ 5);

    stop.store(true, std::memory_order_relaxed);
    publisher.join();

    // Verify all 10 products discovered.
    ASSERT_EQ(instruments.size(), products.size());

    // Spot-check a few instruments.
    ASSERT_TRUE(instruments.count("KS"));
    const auto& ks = instruments.at("KS");
    EXPECT_EQ(ks.security_id, 1u);
    EXPECT_EQ(ks.tick_size, 500);
    EXPECT_EQ(ks.lot_size, 10000);
    EXPECT_EQ(ks.currency, "KRW");
    EXPECT_EQ(ks.product_group, "Futures");

    ASSERT_TRUE(instruments.count("KTB"));
    const auto& ktb = instruments.at("KTB");
    EXPECT_EQ(ktb.security_id, 9u);
    EXPECT_EQ(ktb.tick_size, 100);
    EXPECT_EQ(ktb.product_group, "Bond");

    ASSERT_TRUE(instruments.count("USD"));
    const auto& usd = instruments.at("USD");
    EXPECT_EQ(usd.product_group, "FX");
}

TEST(KrxSecdefConsumer, TimeoutReturnsEmpty) {
    // Use an unused port -- no publisher running.
    KrxSecdefConsumer consumer("239.255.0.43", 19043);
    auto instruments = consumer.discover(/* timeout_secs= */ 1);
    EXPECT_TRUE(instruments.empty());
}

}  // namespace
}  // namespace exchange
