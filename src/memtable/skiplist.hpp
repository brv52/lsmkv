// Lock-free, insert-only concurrent skiplist (arena allocated).
//
// Any number of threads may Insert() concurrently with each other and with
// readers. Nodes are never removed; the whole list is freed with its arena.
//
// Insert links a node bottom-up with one CAS per level. Linking level 0 first
// is what makes the node visible; upper levels are only shortcuts, so a
// reader racing with an insert either sees the node or skips over it at an
// upper level and finds it on a lower one. When a CAS fails because another
// thread linked a node between prev and next, the splice for that level is
// recomputed starting from prev (still < key since nothing is deleted).
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <new>

#include "memtable/arena.hpp"

namespace lsm {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;

  SkipList(Comparator cmp, ConcurrentArena* arena)
      : compare_(cmp), arena_(arena), head_(NewNode(Key{}, kMaxHeight)) {
    for (int i = 0; i < kMaxHeight; ++i) head_->SetNextRelaxed(i, nullptr);
  }
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no equal key is ever inserted (internal keys are unique).
  void Insert(const Key& key) {
    const int height = RandomHeight();
    Node* x = NewNode(key, height);

    int max_h = max_height_.load(std::memory_order_relaxed);
    while (height > max_h) {
      if (max_height_.compare_exchange_weak(max_h, height, std::memory_order_relaxed)) {
        max_h = height;
        break;
      }
    }

    Node* prev[kMaxHeight];
    Node* next[kMaxHeight];
    Node* p = head_;
    for (int level = max_h - 1; level >= 0; --level) {
      FindSpliceForLevel(key, p, level, &prev[level], &next[level]);
      p = prev[level];
    }
    for (int i = 0; i < height; ++i) {
      for (;;) {
        x->SetNextRelaxed(i, next[i]);
        if (prev[i]->CasNext(i, next[i], x)) break;
        FindSpliceForLevel(key, prev[i], i, &prev[i], &next[i]);
      }
    }
  }

  [[nodiscard]] bool Contains(const Key& key) const {
    Node* x = FindGreaterOrEqual(key);
    return x != nullptr && compare_(key, x->key) == 0;
  }

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}
    [[nodiscard]] bool Valid() const noexcept { return node_ != nullptr; }
    [[nodiscard]] const Key& key() const noexcept {
      assert(Valid());
      return node_->key;
    }
    void Next() noexcept { node_ = node_->Next(0); }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target); }
    void SeekToFirst() noexcept { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  struct Node {
    explicit Node(const Key& k) : key(k) {}
    Key const key;

    Node* Next(int n) const noexcept { return next_[n].load(std::memory_order_acquire); }
    void SetNextRelaxed(int n, Node* x) noexcept { next_[n].store(x, std::memory_order_relaxed); }
    bool CasNext(int n, Node* expected, Node* x) noexcept {
      return next_[n].compare_exchange_strong(expected, x, std::memory_order_release, std::memory_order_relaxed);
    }

    // Over-allocated: a node of height h owns next_[0..h-1].
    std::atomic<Node*> next_[1];
  };

  Node* NewNode(const Key& key, int height) {
    const size_t bytes = sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1);
    char* mem = arena_->Allocate(bytes);
    Node* n = new (mem) Node(key);
    for (int i = 1; i < height; ++i) new (&n->next_[i]) std::atomic<Node*>(nullptr);
    return n;
  }

  static int RandomHeight() noexcept {
    // xorshift64*, one generator per thread.
    thread_local uint64_t state = 0x9e3779b97f4a7c15ULL ^ reinterpret_cast<uintptr_t>(&state);
    int h = 1;
    for (;;) {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      const uint64_t r = state * 0x2545f4914f6cdd1dULL;
      if (h >= kMaxHeight || (r >> 32) % kBranching != 0) break;
      ++h;
    }
    return h;
  }

  void FindSpliceForLevel(const Key& key, Node* before, int level, Node** out_prev, Node** out_next) const {
    for (;;) {
      Node* n = before->Next(level);
      if (n != nullptr && compare_(n->key, key) < 0) {
        before = n;
      } else {
        *out_prev = before;
        *out_next = n;
        return;
      }
    }
  }

  Node* FindGreaterOrEqual(const Key& key) const {
    Node* x = head_;
    int level = max_height_.load(std::memory_order_relaxed) - 1;
    for (;;) {
      Node* next = x->Next(level);
      if (next != nullptr && compare_(next->key, key) < 0) {
        x = next;
      } else if (level == 0) {
        return next;
      } else {
        --level;
      }
    }
  }

  Comparator const compare_;
  ConcurrentArena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_{1};
};

}  // namespace lsm
