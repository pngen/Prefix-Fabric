// Prefix Fabric - concurrency tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

#include <atomic>
#include <cstdio>
#include <random>
#include <string>
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

static CompatibilityKey Cn(int n) { CompatibilityKey c = C(); c.model = "m" + std::to_string(n); return c; }

PF_TEST(Concurrency_HardBoundHolds) {
  // Many writers, each in its own compatibility tree, against one bound.
  for (int round = 0; round < 20; ++round) {
    PrefixStore::Options o; o.ns = "conc"; o.default_compat = C(); o.segmentation.policy = SegmentationPolicy::kSingle;
    o.capacity = CapacityPolicy::Defaults(); o.capacity.max_prefixes = 64; o.eviction.auto_evict = false;
    PrefixStore store(o);
    std::atomic<int> accepted{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&, t]() {
        CompatibilityKey c = Cn(t);
        while (!go.load()) std::this_thread::yield();
        for (TokenId i = 1; i <= 40; ++i) if (store.Add(c, {i}, {}, "conc").is_ok()) ++accepted;
      });
    }
    go = true;
    for (auto& th : threads) th.join();
    CHECK_EQ(store.PrefixCount(), 64u);
    CHECK_EQ(accepted.load(), 64);
  }
}

PF_TEST(Concurrency_CountersSurviveRemovals) {
  // Writers in several trees while another thread keeps adding and removing:
  // afterwards the running counters equal a count taken from scratch (a reloaded copy).
  PrefixStore::Options o; o.ns = "conc"; o.default_compat = C(); o.segmentation.policy = SegmentationPolicy::kSingle;
  o.capacity = CapacityPolicy::Defaults(); o.eviction.auto_evict = false;
  PrefixStore store(o);
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&, t]() {
      std::mt19937 rng(500 + t);
      CompatibilityKey c = Cn(t);
      for (int i = 0; i < 1500; ++i) {
        TokenSequence seq; int len = 1 + (rng() % 5); for (int j = 0; j < len; ++j) seq.push_back(rng() % 6);
        store.Add(c, seq, {}, "conc");
      }
    });
  }
  std::thread remover([&]() {
    CompatibilityKey c = Cn(100);
    TokenId i = 0;
    while (!done.load()) {
      auto r = store.Add(c, {++i, 1}, {}, "conc");
      if (r.is_ok()) store.Remove(r.value().id);
    }
  });
  for (auto& th : threads) th.join();
  done = true;
  remover.join();
  const std::string path = "conc_recount.bin";
  CHECK(store.SaveToFile(path).is_ok());
  PrefixStore recount(o);
  CHECK(recount.LoadFromFile(path).is_ok());
  CHECK_EQ(store.PrefixCount(), recount.PrefixCount());
  CHECK_EQ(store.NodeCount(), recount.NodeCount());
  CHECK_EQ(store.SegmentCount(), recount.SegmentCount());
  CHECK_EQ(store.PrefixTokens(), recount.PrefixTokens());
  CHECK_EQ(store.TokenFootprintBytes(), recount.TokenFootprintBytes());
  std::remove(path.c_str());
}

int main() { return pftest::RunAll(); }
