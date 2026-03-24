#include "exchange-core/exchange_core.h"

#include <gtest/gtest.h>

#include <string>

namespace exchange {
namespace {

TEST(ExchangeCoreTest, VersionIsSet) {
    std::string version = Version();
    EXPECT_FALSE(version.empty());
    EXPECT_EQ(version, "0.1.0");
}

}  // namespace
}  // namespace exchange
