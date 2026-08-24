// Prefix Fabric - concurrency tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace prefix_fabric;

static CompatibilityKey C() { CompatibilityKey c; c.model = "m"; c.tokenizer = "t"; c.vocabulary = "v"; c.tokenization_version = "1"; c.kv_layout = "gqa"; return c; }

PF_TEST(Concurrency_MixedOperations) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "conc"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle; o.capacity = CapacityPolicy::Defaults(); o.eviction.auto_evict = false;
  PrefixStore store(o);
  std::atomic<bool> done{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  // Insertion threads.
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      std::mt19937 rng(1000 + t);
      for (int i = 0; i < 500; ++i) {
        TokenSequence seq; int len = 1 + (rng() % 8); for (int j = 0; j < len; ++j) seq.push_back(rng() % 16);
        auto r = store.Add(ca, seq, {}, "conc");
        if (r.is_err()) ++errors;
      }
    });
  }
  // Lookup threads.
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      std::mt19937 rng(2000 + t);
      for (int i = 0; i < 800; ++i) {
        TokenSequence req; int len = 1 + (rng() % 10); for (int j = 0; j < len; ++j) req.push_back(rng() % 16);
        PrefixMatch m = store.LongestPrefixMatch(req, &ca);
        if (m.is_matched()) { CHECK(m.matched_token_count() <= static_cast<std::uint64_t>(req.size())); }
      }
    });
  }
  // Invalidation + attach threads (target a known prefix if possible).
  threads.emplace_back([&]() {
    std::mt19937 rng(3000);
    for (int i = 0; i < 300; ++i) {
      TokenSequence seq; int len = 1 + (rng() % 4); for (int j = 0; j < len; ++j) seq.push_back(rng() % 16);
      auto r = store.Add(ca, seq, {}, "conc");
      if (r.is_ok()) {
        StateReference ref; ref.ns = "kv"; ref.object_id = "o" + std::to_string(i); ref.generation = i; ref.compatibility = ca; ref.freshness_ts = 1;
        store.AttachState(r.value().id, ref);
        if (i % 3 == 0) store.Invalidate(r.value().id);
      }
    }
  });
  for (auto& th : threads) th.join();
  // After all work, the store must still be consistent and usable.
  CHECK(errors.load() == 0);
  auto snap = store.Snapshot();
  CHECK(store.PrefixCount() > 0);
  // A fresh add and match still work.
  TokenSequence t = {1, 2, 3, 4, 5};
  auto r = store.Add(ca, t); CHECK(r.is_ok());
  PrefixMatch m = store.LongestPrefixMatch({1, 2, 3, 4, 5, 6}, &ca);
  CHECK(m.is_matched());
  CHECK_EQ(m.matched_token_count(), 5u);
}

PF_TEST(Concurrency_ShutdownRaces) {
  // Repeated create/destroy of stores with event dispatch must not leak/crash.
  for (int i = 0; i < 20; ++i) {
    CompatibilityKey ca = C();
    PrefixStore::Options o; o.ns = "conc"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
    PrefixStore store(o);
    auto sub = store.Subscribe([](const PrefixEvent&){}, {});
    CHECK(sub.is_ok());
    auto r = store.Add(ca, {1,2,3,4}, {}, "conc"); CHECK(r.is_ok());
    store.DrainEvents();
    store.Unsubscribe(sub.value().id);
  }
}

int main() { return pftest::RunAll(); }
