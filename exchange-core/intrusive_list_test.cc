#include "exchange-core/intrusive_list.h"

#include <gtest/gtest.h>

namespace exchange {
namespace {

// Minimal node type for testing — does NOT depend on types.h.
struct Node {
    int value;
    Node* prev{nullptr};
    Node* next{nullptr};

    explicit Node(int v) : value(v) {}
};

// Helper: collect list values into a vector for easy assertion.
std::vector<int> to_vec(const Node* head) {
    std::vector<int> out;
    for (const Node* n = head; n; n = n->next) out.push_back(n->value);
    return out;
}

// ---------------------------------------------------------------------------
// list_push_back
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, PushBackSingleNode) {
    Node a(1);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_back(head, tail, &a);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(a.next, nullptr);
    EXPECT_EQ(list_size(head), 1u);
}

TEST(IntrusiveListTest, PushBackMultipleNodesPreservesOrder) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &c);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));

    // Verify prev pointers
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(b.prev, &a);
    EXPECT_EQ(c.prev, &b);
}

// ---------------------------------------------------------------------------
// list_push_front
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, PushFrontSingleNode) {
    Node a(1);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_front(head, tail, &a);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &a);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(a.next, nullptr);
}

TEST(IntrusiveListTest, PushFrontMultipleNodesPreservesOrder) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_front(head, tail, &c);
    list_push_front(head, tail, &b);
    list_push_front(head, tail, &a);

    EXPECT_EQ(head, &a);
    EXPECT_EQ(tail, &c);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));

    // Verify next pointers
    EXPECT_EQ(a.next, &b);
    EXPECT_EQ(b.next, &c);
    EXPECT_EQ(c.next, nullptr);
}

// ---------------------------------------------------------------------------
// list_insert_before
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, InsertBeforeHead) {
    Node a(2), b(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);

    Node x(1);
    list_insert_before(head, tail, &a, &x);

    EXPECT_EQ(head, &x);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));
    EXPECT_EQ(x.prev, nullptr);
    EXPECT_EQ(x.next, &a);
}

TEST(IntrusiveListTest, InsertBeforeMiddle) {
    Node a(1), b(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);

    Node x(2);
    list_insert_before(head, tail, &b, &x);

    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));
    EXPECT_EQ(x.prev, &a);
    EXPECT_EQ(x.next, &b);
    EXPECT_EQ(a.next, &x);
    EXPECT_EQ(b.prev, &x);
}

TEST(IntrusiveListTest, InsertBeforeTail) {
    Node a(1), b(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);

    Node x(2);
    list_insert_before(head, tail, &b, &x);

    EXPECT_EQ(tail, &b);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));
}

// ---------------------------------------------------------------------------
// list_insert_after
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, InsertAfterHead) {
    Node a(1), b(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);

    Node x(2);
    list_insert_after(head, tail, &a, &x);

    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));
    EXPECT_EQ(x.prev, &a);
    EXPECT_EQ(x.next, &b);
    EXPECT_EQ(a.next, &x);
    EXPECT_EQ(b.prev, &x);
}

TEST(IntrusiveListTest, InsertAfterMiddle) {
    Node a(1), b(2), c(4);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    Node x(3);
    list_insert_after(head, tail, &b, &x);

    EXPECT_EQ((std::vector<int>{1, 2, 3, 4}), to_vec(head));
}

TEST(IntrusiveListTest, InsertAfterTail) {
    Node a(1), b(2);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);

    Node x(3);
    list_insert_after(head, tail, &b, &x);

    EXPECT_EQ(tail, &x);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));
    EXPECT_EQ(x.prev, &b);
    EXPECT_EQ(x.next, nullptr);
}

// ---------------------------------------------------------------------------
// list_remove
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, RemoveHead) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    list_remove(head, tail, &a);

    EXPECT_EQ(head, &b);
    EXPECT_EQ(b.prev, nullptr);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(a.next, nullptr);
    EXPECT_EQ((std::vector<int>{2, 3}), to_vec(head));
}

TEST(IntrusiveListTest, RemoveMiddle) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    list_remove(head, tail, &b);

    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(b.prev, nullptr);
    EXPECT_EQ(b.next, nullptr);
    EXPECT_EQ((std::vector<int>{1, 3}), to_vec(head));
}

TEST(IntrusiveListTest, RemoveTail) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    list_remove(head, tail, &c);

    EXPECT_EQ(tail, &b);
    EXPECT_EQ(b.next, nullptr);
    EXPECT_EQ(c.prev, nullptr);
    EXPECT_EQ(c.next, nullptr);
    EXPECT_EQ((std::vector<int>{1, 2}), to_vec(head));
}

TEST(IntrusiveListTest, RemoveLastNodeYieldsEmptyList) {
    Node a(1);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);

    list_remove(head, tail, &a);

    EXPECT_EQ(head, nullptr);
    EXPECT_EQ(tail, nullptr);
    EXPECT_EQ(a.prev, nullptr);
    EXPECT_EQ(a.next, nullptr);
    EXPECT_TRUE(list_empty(head));
}

// ---------------------------------------------------------------------------
// list_empty and list_size
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, EmptyOnNullHead) {
    Node* head = nullptr;
    EXPECT_TRUE(list_empty(head));
}

TEST(IntrusiveListTest, NotEmptyAfterPush) {
    Node a(1);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    EXPECT_FALSE(list_empty(head));
}

TEST(IntrusiveListTest, SizeZeroOnEmptyList) {
    Node* head = nullptr;
    EXPECT_EQ(list_size(head), 0u);
}

TEST(IntrusiveListTest, SizeMatchesNumberOfPushes) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    EXPECT_EQ(list_size(head), 1u);
    list_push_back(head, tail, &b);
    EXPECT_EQ(list_size(head), 2u);
    list_push_back(head, tail, &c);
    EXPECT_EQ(list_size(head), 3u);
}

TEST(IntrusiveListTest, SizeDecreasesAfterRemove) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    list_remove(head, tail, &b);
    EXPECT_EQ(list_size(head), 2u);
}

// ---------------------------------------------------------------------------
// Bidirectional traversal integrity
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, ForwardAndBackwardTraversalConsistent) {
    Node a(1), b(2), c(3);
    Node* head = nullptr;
    Node* tail = nullptr;
    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_push_back(head, tail, &c);

    // Forward
    std::vector<int> fwd;
    for (Node* n = head; n; n = n->next) fwd.push_back(n->value);

    // Backward from tail
    std::vector<int> rev;
    for (Node* n = tail; n; n = n->prev) rev.push_back(n->value);

    std::reverse(rev.begin(), rev.end());
    EXPECT_EQ(fwd, rev);
}

// ---------------------------------------------------------------------------
// Mixed operations
// ---------------------------------------------------------------------------

TEST(IntrusiveListTest, MixedPushFrontAndBack) {
    Node a(2), b(3), c(1), d(4);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_back(head, tail, &a);   // [2]
    list_push_back(head, tail, &b);   // [2, 3]
    list_push_front(head, tail, &c);  // [1, 2, 3]
    list_push_back(head, tail, &d);   // [1, 2, 3, 4]

    EXPECT_EQ((std::vector<int>{1, 2, 3, 4}), to_vec(head));
    EXPECT_EQ(head, &c);
    EXPECT_EQ(tail, &d);
}

TEST(IntrusiveListTest, InsertAndRemoveSequence) {
    Node a(1), b(3), x(2);
    Node* head = nullptr;
    Node* tail = nullptr;

    list_push_back(head, tail, &a);
    list_push_back(head, tail, &b);
    list_insert_before(head, tail, &b, &x);
    EXPECT_EQ((std::vector<int>{1, 2, 3}), to_vec(head));

    list_remove(head, tail, &x);
    EXPECT_EQ((std::vector<int>{1, 3}), to_vec(head));
    EXPECT_EQ(list_size(head), 2u);
}

}  // namespace
}  // namespace exchange
