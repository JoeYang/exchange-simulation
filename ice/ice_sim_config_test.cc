#include "ice/ice_sim_config.h"

#include <gtest/gtest.h>

namespace exchange::ice {
namespace {

TEST(IceSimConfigTest, DefaultValues) {
    IceSimConfig cfg{};
    EXPECT_EQ(cfg.fix_port, 9200);
    EXPECT_EQ(cfg.impact_group, "239.0.32.1");
    EXPECT_EQ(cfg.impact_port, 14400);
    EXPECT_EQ(cfg.snapshot_port, 14401);
    EXPECT_TRUE(cfg.shm_path.empty());
    EXPECT_TRUE(cfg.products.empty());
}

TEST(IceSimConfigTest, ParseAllFlags) {
    const char* argv[] = {
        "ice-sim",
        "--fix-port", "9300",
        "--impact-group", "239.0.32.5",
        "--impact-port", "14500",
        "--shm-path", "/dev/shm/ice-test",
        "--products", "B,G,I",
    };
    int argc = sizeof(argv) / sizeof(argv[0]);

    IceSimConfig cfg{};
    ASSERT_TRUE(IceSimConfig::parse(argc, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.fix_port, 9300);
    EXPECT_EQ(cfg.impact_group, "239.0.32.5");
    EXPECT_EQ(cfg.impact_port, 14500);
    EXPECT_EQ(cfg.shm_path, "/dev/shm/ice-test");
    ASSERT_EQ(cfg.products.size(), 3u);
    EXPECT_EQ(cfg.products[0], "B");
    EXPECT_EQ(cfg.products[1], "G");
    EXPECT_EQ(cfg.products[2], "I");
}

TEST(IceSimConfigTest, ParseNoArgs) {
    const char* argv[] = {"ice-sim"};
    IceSimConfig cfg{};
    ASSERT_TRUE(IceSimConfig::parse(1, const_cast<char**>(argv), cfg));
    // All defaults
    EXPECT_EQ(cfg.fix_port, 9200);
}

TEST(IceSimConfigTest, ParseHelp) {
    const char* argv[] = {"ice-sim", "--help"};
    IceSimConfig cfg{};
    EXPECT_FALSE(IceSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

TEST(IceSimConfigTest, ParseUnknownFlag) {
    const char* argv[] = {"ice-sim", "--bogus"};
    IceSimConfig cfg{};
    EXPECT_FALSE(IceSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

TEST(IceSimConfigTest, ParsePartialProducts) {
    const char* argv[] = {"ice-sim", "--products", "B"};
    IceSimConfig cfg{};
    ASSERT_TRUE(IceSimConfig::parse(3, const_cast<char**>(argv), cfg));
    ASSERT_EQ(cfg.products.size(), 1u);
    EXPECT_EQ(cfg.products[0], "B");
}

TEST(IceSimConfigTest, ParseSnapshotPort) {
    const char* argv[] = {"ice-sim", "--snapshot-port", "15000"};
    IceSimConfig cfg{};
    ASSERT_TRUE(IceSimConfig::parse(3, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.snapshot_port, 15000);
    // Other fields should be defaults.
    EXPECT_EQ(cfg.fix_port, 9200);
    EXPECT_EQ(cfg.impact_port, 14400);
}

}  // namespace
}  // namespace exchange::ice
