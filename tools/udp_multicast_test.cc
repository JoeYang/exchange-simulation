#include "tools/udp_multicast.h"

#include <sys/socket.h>

#include <cstring>
#include <string>

#include "gtest/gtest.h"

namespace exchange {
namespace {

// Use a link-local multicast group for loopback testing.
constexpr const char* kTestGroup = "224.0.0.1";

// Helper: pick a port unlikely to collide. Each test uses a different port
// to allow parallel execution.
constexpr uint16_t kBasePort = 23400;

// Set a receive timeout so tests don't hang on failure.
void SetRecvTimeout(int fd, int ms) {
    struct timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Extract the sequence header from a received buffer.
McastSeqHeader ExtractSeqHeader(const char* buf) {
    McastSeqHeader hdr;
    std::memcpy(&hdr, buf, sizeof(hdr));
    return hdr;
}

// ---------------------------------------------------------------------------
// Basic send and receive (with sequence header)
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, SendAndReceive) {
    constexpr uint16_t port = kBasePort + 1;

    UdpMulticastReceiver receiver;
    receiver.join_group(kTestGroup, port);
    SetRecvTimeout(receiver.fd(), 2000);

    UdpMulticastPublisher publisher(kTestGroup, port);

    const std::string msg = "hello multicast";
    ASSERT_EQ(publisher.send(msg.data(), msg.size()), SendResult::kOk);

    char buf[256]{};
    ssize_t n = receiver.receive(buf, sizeof(buf));
    ASSERT_EQ(n, static_cast<ssize_t>(sizeof(McastSeqHeader) + msg.size()));

    // Verify sequence header.
    auto hdr = ExtractSeqHeader(buf);
    EXPECT_EQ(hdr.seq_num, 0u);

    // Verify payload after header.
    EXPECT_EQ(std::string(buf + sizeof(McastSeqHeader),
                          static_cast<size_t>(n) - sizeof(McastSeqHeader)),
              msg);
}

// ---------------------------------------------------------------------------
// Multiple messages: sequence numbers increment, content preserved
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, MultipleMessagesWithSequenceNumbers) {
    constexpr uint16_t port = kBasePort + 2;

    UdpMulticastReceiver receiver;
    receiver.join_group(kTestGroup, port);
    SetRecvTimeout(receiver.fd(), 2000);

    UdpMulticastPublisher publisher(kTestGroup, port);
    EXPECT_EQ(publisher.sequence_number(), 0u);

    constexpr int kCount = 10;
    for (int i = 0; i < kCount; ++i) {
        std::string msg = "msg-" + std::to_string(i);
        ASSERT_EQ(publisher.send(msg.data(), msg.size()), SendResult::kOk);
    }
    EXPECT_EQ(publisher.sequence_number(), static_cast<uint32_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        char buf[256]{};
        ssize_t n = receiver.receive(buf, sizeof(buf));
        ASSERT_GT(n, static_cast<ssize_t>(sizeof(McastSeqHeader)))
            << "failed to receive message " << i;

        auto hdr = ExtractSeqHeader(buf);
        EXPECT_EQ(hdr.seq_num, static_cast<uint32_t>(i));

        std::string expected = "msg-" + std::to_string(i);
        EXPECT_EQ(std::string(buf + sizeof(McastSeqHeader),
                              static_cast<size_t>(n) - sizeof(McastSeqHeader)),
                  expected);
    }
}

// ---------------------------------------------------------------------------
// Message integrity: binary payload round-trip
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, BinaryPayloadIntegrity) {
    constexpr uint16_t port = kBasePort + 3;

    UdpMulticastReceiver receiver;
    receiver.join_group(kTestGroup, port);
    SetRecvTimeout(receiver.fd(), 2000);

    UdpMulticastPublisher publisher(kTestGroup, port);

    // Build a binary payload with all byte values 0-255.
    char payload[256];
    for (int i = 0; i < 256; ++i) payload[i] = static_cast<char>(i);

    ASSERT_EQ(publisher.send(payload, sizeof(payload)), SendResult::kOk);

    char buf[512]{};
    ssize_t n = receiver.receive(buf, sizeof(buf));
    ASSERT_EQ(n, static_cast<ssize_t>(sizeof(McastSeqHeader) + sizeof(payload)));
    EXPECT_EQ(std::memcmp(payload, buf + sizeof(McastSeqHeader),
                          sizeof(payload)), 0);
}

// ---------------------------------------------------------------------------
// join_group / leave_group / is_joined
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, JoinLeaveState) {
    UdpMulticastReceiver receiver;
    EXPECT_FALSE(receiver.is_joined());

    receiver.join_group(kTestGroup, kBasePort + 4);
    EXPECT_TRUE(receiver.is_joined());

    receiver.leave_group();
    EXPECT_FALSE(receiver.is_joined());

    // leave_group is idempotent.
    receiver.leave_group();
    EXPECT_FALSE(receiver.is_joined());
}

// ---------------------------------------------------------------------------
// Receiver can rejoin after leaving
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, RejoinAfterLeave) {
    constexpr uint16_t port = kBasePort + 5;

    UdpMulticastReceiver receiver;
    receiver.join_group(kTestGroup, port);
    receiver.leave_group();

    // Rejoin on same port.
    receiver.join_group(kTestGroup, port);
    SetRecvTimeout(receiver.fd(), 2000);
    EXPECT_TRUE(receiver.is_joined());

    UdpMulticastPublisher publisher(kTestGroup, port);
    const std::string msg = "after-rejoin";
    ASSERT_EQ(publisher.send(msg.data(), msg.size()), SendResult::kOk);

    char buf[256]{};
    ssize_t n = receiver.receive(buf, sizeof(buf));
    ASSERT_GT(n, static_cast<ssize_t>(sizeof(McastSeqHeader)));
    EXPECT_EQ(std::string(buf + sizeof(McastSeqHeader),
                          static_cast<size_t>(n) - sizeof(McastSeqHeader)),
              msg);
}

// ---------------------------------------------------------------------------
// Publisher move constructor preserves sequence number
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, PublisherMoveConstructPreservesSeq) {
    constexpr uint16_t port = kBasePort + 6;

    UdpMulticastReceiver receiver;
    receiver.join_group(kTestGroup, port);
    SetRecvTimeout(receiver.fd(), 2000);

    UdpMulticastPublisher pub1(kTestGroup, port);
    // Send 3 messages to advance the sequence counter.
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(pub1.send("x", 1), SendResult::kOk);
    }
    EXPECT_EQ(pub1.sequence_number(), 3u);

    UdpMulticastPublisher pub2(std::move(pub1));

    // pub1 should be invalidated.
    EXPECT_EQ(pub1.fd(), -1);
    // pub2 inherits the sequence number.
    EXPECT_EQ(pub2.sequence_number(), 3u);

    const std::string msg = "moved";
    ASSERT_EQ(pub2.send(msg.data(), msg.size()), SendResult::kOk);
    EXPECT_EQ(pub2.sequence_number(), 4u);

    // Drain the 3 warmup messages.
    for (int i = 0; i < 3; ++i) {
        char discard[64]{};
        receiver.receive(discard, sizeof(discard));
    }

    // Receive the "moved" message and verify seq=3.
    char buf[256]{};
    ssize_t n = receiver.receive(buf, sizeof(buf));
    ASSERT_GT(n, static_cast<ssize_t>(sizeof(McastSeqHeader)));
    auto hdr = ExtractSeqHeader(buf);
    EXPECT_EQ(hdr.seq_num, 3u);
    EXPECT_EQ(std::string(buf + sizeof(McastSeqHeader),
                          static_cast<size_t>(n) - sizeof(McastSeqHeader)),
              msg);
}

// ---------------------------------------------------------------------------
// RAII: destructor closes socket
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, DestructorClosesSocket) {
    int pub_fd, recv_fd;
    {
        UdpMulticastPublisher publisher(kTestGroup, kBasePort + 7);
        pub_fd = publisher.fd();
        EXPECT_GE(pub_fd, 0);

        UdpMulticastReceiver receiver;
        receiver.join_group(kTestGroup, kBasePort + 7);
        recv_fd = receiver.fd();
        EXPECT_GE(recv_fd, 0);
    }
    // After destruction, the file descriptors should be closed.
    int optval = 0;
    socklen_t optlen = sizeof(optval);
    EXPECT_EQ(::getsockopt(pub_fd, SOL_SOCKET, SO_TYPE, &optval, &optlen), -1);
    EXPECT_EQ(::getsockopt(recv_fd, SOL_SOCKET, SO_TYPE, &optval, &optlen), -1);
}

// ---------------------------------------------------------------------------
// Sequence number starts at zero and increments
// ---------------------------------------------------------------------------

TEST(UdpMulticastTest, SequenceNumberIncrement) {
    constexpr uint16_t port = kBasePort + 8;

    UdpMulticastPublisher publisher(kTestGroup, port);
    EXPECT_EQ(publisher.sequence_number(), 0u);

    for (uint32_t i = 0; i < 5; ++i) {
        ASSERT_EQ(publisher.send("x", 1), SendResult::kOk);
        EXPECT_EQ(publisher.sequence_number(), i + 1);
    }
}

}  // namespace
}  // namespace exchange
