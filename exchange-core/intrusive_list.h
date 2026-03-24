#pragma once
#include <cstddef>

namespace exchange {

// Free function templates for intrusive doubly-linked list operations.
//
// Requirements on T:
//   - T* prev  -- pointer to predecessor node (nullptr if head)
//   - T* next  -- pointer to successor node   (nullptr if tail)
//
// All operations are O(1) except list_size which is O(n).

template <typename T>
void list_push_back(T*& head, T*& tail, T* node) {
    node->prev = tail;
    node->next = nullptr;
    if (tail) tail->next = node;
    else head = node;
    tail = node;
}

template <typename T>
void list_push_front(T*& head, T*& tail, T* node) {
    node->prev = nullptr;
    node->next = head;
    if (head) head->prev = node;
    else tail = node;
    head = node;
}

template <typename T>
void list_insert_before(T*& head, T*& tail, T* before, T* node) {
    (void)tail;  // tail unchanged; parameter kept for uniform API symmetry
    node->next = before;
    node->prev = before->prev;
    if (before->prev) before->prev->next = node;
    else head = node;
    before->prev = node;
}

template <typename T>
void list_insert_after(T*& head, T*& tail, T* after, T* node) {
    (void)head;  // head unchanged; parameter kept for uniform API symmetry
    node->prev = after;
    node->next = after->next;
    if (after->next) after->next->prev = node;
    else tail = node;
    after->next = node;
}

template <typename T>
void list_remove(T*& head, T*& tail, T* node) {
    if (node->prev) node->prev->next = node->next;
    else head = node->next;
    if (node->next) node->next->prev = node->prev;
    else tail = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
}

template <typename T>
bool list_empty(const T* head) { return head == nullptr; }

template <typename T>
size_t list_size(const T* head) {
    size_t count = 0;
    for (const T* n = head; n; n = n->next) ++count;
    return count;
}

}  // namespace exchange
