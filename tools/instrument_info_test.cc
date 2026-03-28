#include "tools/instrument_info.h"
#include "tools/secdef_consumer.h"

#include <unordered_map>

#include "gtest/gtest.h"

namespace exchange {
namespace {

TEST(InstrumentInfoTest, DefaultConstruction) {
    InstrumentInfo info;
    EXPECT_EQ(info.security_id, 0u);
    EXPECT_TRUE(info.symbol.empty());
    EXPECT_TRUE(info.description.empty());
    EXPECT_TRUE(info.product_group.empty());
    EXPECT_EQ(info.tick_size, 0);
    EXPECT_EQ(info.lot_size, 0);
    EXPECT_EQ(info.max_order_size, 0);
    EXPECT_EQ(info.match_algorithm, 'F');
    EXPECT_TRUE(info.currency.empty());
    EXPECT_DOUBLE_EQ(info.display_factor, 0.0);
}

TEST(InstrumentInfoTest, FieldAssignment) {
    InstrumentInfo info;
    info.security_id = 1;
    info.symbol = "ES";
    info.description = "E-mini S&P 500";
    info.product_group = "Equity Index";
    info.tick_size = 2500;
    info.lot_size = 10000;
    info.max_order_size = 20000000;
    info.match_algorithm = 'F';
    info.currency = "USD";
    info.display_factor = 0.01;

    EXPECT_EQ(info.security_id, 1u);
    EXPECT_EQ(info.symbol, "ES");
    EXPECT_EQ(info.description, "E-mini S&P 500");
    EXPECT_EQ(info.product_group, "Equity Index");
    EXPECT_EQ(info.tick_size, 2500);
    EXPECT_EQ(info.lot_size, 10000);
    EXPECT_EQ(info.max_order_size, 20000000);
    EXPECT_EQ(info.match_algorithm, 'F');
    EXPECT_EQ(info.currency, "USD");
    EXPECT_DOUBLE_EQ(info.display_factor, 0.01);
}

TEST(InstrumentInfoTest, MapKeyedBySymbol) {
    std::unordered_map<std::string, InstrumentInfo> instruments;

    InstrumentInfo es;
    es.security_id = 1;
    es.symbol = "ES";
    es.tick_size = 2500;
    instruments["ES"] = es;

    InstrumentInfo nq;
    nq.security_id = 2;
    nq.symbol = "NQ";
    nq.tick_size = 2500;
    instruments["NQ"] = nq;

    EXPECT_EQ(instruments.size(), 2u);
    EXPECT_EQ(instruments.at("ES").security_id, 1u);
    EXPECT_EQ(instruments.at("NQ").security_id, 2u);
    EXPECT_EQ(instruments.count("CL"), 0u);
}

// Verify SecdefConsumer is a proper ABC that can be subclassed.
class MockSecdefConsumer : public SecdefConsumer {
public:
    std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs) override {
        (void)timeout_secs;
        return instruments_;
    }

    void add(const InstrumentInfo& info) {
        instruments_[info.symbol] = info;
    }

private:
    std::unordered_map<std::string, InstrumentInfo> instruments_;
};

TEST(SecdefConsumerTest, MockDiscover) {
    MockSecdefConsumer consumer;

    InstrumentInfo es;
    es.security_id = 1;
    es.symbol = "ES";
    es.tick_size = 2500;
    consumer.add(es);

    auto result = consumer.discover(5);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result.at("ES").security_id, 1u);
    EXPECT_EQ(result.at("ES").tick_size, 2500);
}

TEST(SecdefConsumerTest, EmptyDiscover) {
    MockSecdefConsumer consumer;
    auto result = consumer.discover(1);
    EXPECT_TRUE(result.empty());
}

TEST(SecdefConsumerTest, PolymorphicAccess) {
    MockSecdefConsumer mock;
    InstrumentInfo info;
    info.symbol = "CL";
    info.security_id = 3;
    mock.add(info);

    // Access through base pointer.
    SecdefConsumer* base = &mock;
    auto result = base->discover(5);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result.at("CL").security_id, 3u);
}

}  // namespace
}  // namespace exchange
