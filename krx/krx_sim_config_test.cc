#include "krx/krx_sim_config.h"

#include <gtest/gtest.h>

namespace exchange::krx {
namespace {

TEST(KrxSimConfigTest, DefaultValues) {
    KrxSimConfig cfg{};
    EXPECT_EQ(cfg.fix_port, 9300);
    EXPECT_EQ(cfg.fast_group, "224.0.33.1");
    EXPECT_EQ(cfg.fast_port, 16000);
    EXPECT_EQ(cfg.secdef_group, "224.0.33.2");
    EXPECT_EQ(cfg.secdef_port, 16001);
    EXPECT_EQ(cfg.snapshot_group, "224.0.33.3");
    EXPECT_EQ(cfg.snapshot_port, 16002);
    EXPECT_TRUE(cfg.shm_path.empty());
    EXPECT_TRUE(cfg.products.empty());
    EXPECT_EQ(cfg.session_mode, "regular");
}

TEST(KrxSimConfigTest, ParseAllFlags) {
    const char* argv[] = {
        "krx-sim",
        "--fix-port", "9400",
        "--fast-group", "224.0.33.5",
        "--fast-port", "17000",
        "--secdef-group", "224.0.33.6",
        "--secdef-port", "17001",
        "--snapshot-group", "224.0.33.7",
        "--snapshot-port", "17002",
        "--shm-path", "/dev/shm/krx-test",
        "--products", "KOSPI200,KTB3Y,USDKRW",
        "--session-mode", "both",
    };
    int argc = sizeof(argv) / sizeof(argv[0]);

    KrxSimConfig cfg{};
    ASSERT_TRUE(KrxSimConfig::parse(argc, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.fix_port, 9400);
    EXPECT_EQ(cfg.fast_group, "224.0.33.5");
    EXPECT_EQ(cfg.fast_port, 17000);
    EXPECT_EQ(cfg.secdef_group, "224.0.33.6");
    EXPECT_EQ(cfg.secdef_port, 17001);
    EXPECT_EQ(cfg.snapshot_group, "224.0.33.7");
    EXPECT_EQ(cfg.snapshot_port, 17002);
    EXPECT_EQ(cfg.shm_path, "/dev/shm/krx-test");
    ASSERT_EQ(cfg.products.size(), 3u);
    EXPECT_EQ(cfg.products[0], "KOSPI200");
    EXPECT_EQ(cfg.products[1], "KTB3Y");
    EXPECT_EQ(cfg.products[2], "USDKRW");
    EXPECT_EQ(cfg.session_mode, "both");
}

TEST(KrxSimConfigTest, ParseNoArgs) {
    const char* argv[] = {"krx-sim"};
    KrxSimConfig cfg{};
    ASSERT_TRUE(KrxSimConfig::parse(1, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.fix_port, 9300);
    EXPECT_EQ(cfg.session_mode, "regular");
}

TEST(KrxSimConfigTest, ParseHelp) {
    const char* argv[] = {"krx-sim", "--help"};
    KrxSimConfig cfg{};
    EXPECT_FALSE(KrxSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

TEST(KrxSimConfigTest, ParseShortHelp) {
    const char* argv[] = {"krx-sim", "-h"};
    KrxSimConfig cfg{};
    EXPECT_FALSE(KrxSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

TEST(KrxSimConfigTest, ParseUnknownFlag) {
    const char* argv[] = {"krx-sim", "--bogus"};
    KrxSimConfig cfg{};
    EXPECT_FALSE(KrxSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

TEST(KrxSimConfigTest, ParseSingleProduct) {
    const char* argv[] = {"krx-sim", "--products", "KOSPI200"};
    KrxSimConfig cfg{};
    ASSERT_TRUE(KrxSimConfig::parse(3, const_cast<char**>(argv), cfg));
    ASSERT_EQ(cfg.products.size(), 1u);
    EXPECT_EQ(cfg.products[0], "KOSPI200");
}

TEST(KrxSimConfigTest, ParseSessionModeAfterHours) {
    const char* argv[] = {"krx-sim", "--session-mode", "after-hours"};
    KrxSimConfig cfg{};
    ASSERT_TRUE(KrxSimConfig::parse(3, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.session_mode, "after-hours");
}

TEST(KrxSimConfigTest, ParsePartialOverride) {
    const char* argv[] = {"krx-sim", "--fix-port", "9500", "--fast-port", "18000"};
    KrxSimConfig cfg{};
    ASSERT_TRUE(KrxSimConfig::parse(5, const_cast<char**>(argv), cfg));
    EXPECT_EQ(cfg.fix_port, 9500);
    EXPECT_EQ(cfg.fast_port, 18000);
    // Other fields should be defaults
    EXPECT_EQ(cfg.fast_group, "224.0.33.1");
    EXPECT_EQ(cfg.secdef_port, 16001);
    EXPECT_EQ(cfg.session_mode, "regular");
}

TEST(KrxSimConfigTest, ParseMissingValue) {
    // --fix-port without a following value (at end of argv)
    const char* argv[] = {"krx-sim", "--fix-port"};
    KrxSimConfig cfg{};
    // The i+1 < argc check should prevent this from being parsed
    EXPECT_FALSE(KrxSimConfig::parse(2, const_cast<char**>(argv), cfg));
}

}  // namespace
}  // namespace exchange::krx
