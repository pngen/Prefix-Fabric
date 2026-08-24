// Prefix Fabric - micro-benchmarks. Measures completed synchronous work.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include <chrono>
#include <cstdio>
#include <vector>
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

using namespace prefix_fabric;
using Clock = std::chrono::steady_clock;

static CompatibilityKey C() { CompatibilityKey c; c.model="m"; c.tokenizer="t"; c.vocabulary="v"; c.tokenization_version="1"; c.kv_layout="gqa"; return c; }

static double ns(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

static void Report(const char* op, std::size_t n, double total_ns) {
  std::printf("  %-22s n=%8zu  %12.1f ns/op  %10.0f ops/s\n", op, n, total_ns / (double)n, 1e9 * (double)n / total_ns);
}

int main() {
  std::printf("Prefix Fabric benchmarks (Windows x64, MSVC)\n");
  std::printf("scaling points: 1, 10, 100, 1000, 10000, 100000 prefixes\n");
  for (std::size_t scale : {1ul, 10ul, 100ul, 1000ul, 10000ul, 100000ul}) {
    std::printf("\n=== scale = %zu ===\n", scale);
    CompatibilityKey ca = C();
    PrefixStore::Options o; o.ns="bench"; o.default_compat=ca; o.segmentation.policy = SegmentationPolicy::kSingle;
    o.capacity = CapacityPolicy::Defaults(); o.capacity.max_prefixes = 1000000; o.eviction.auto_evict = false;
    PrefixStore store(o);
    // Insertion (build a balanced set of prefixes so matching is realistic).
    std::vector<std::vector<TokenId>> seqs;
    seqs.reserve(scale);
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < scale; ++i) {
      std::vector<TokenId> t;
      // Tokens share long common prefixes via a branching structure.
      std::size_t x = i;
      for (int j = 0; j < 12; ++j) { t.push_back(static_cast<TokenId>(x % 16)); x = x / 16 + (x % 4); }
      store.Add(ca, t, {}, "bench");
      if (i % 1000 == 0) seqs.push_back(t);
    }
    auto t1 = Clock::now(); Report("insert (batch)", scale, ns(t0, t1));

    if (seqs.empty()) continue;
    // Exact lookup.
    auto e0 = Clock::now();
    for (std::size_t i = 0; i < seqs.size(); ++i) { PrefixMatch m = store.ExactMatch(seqs[i], &ca); (void)m; }
    auto e1 = Clock::now(); Report("exact lookup", seqs.size(), ns(e0, e1));

    // Longest-prefix lookup.
    auto l0 = Clock::now();
    for (std::size_t i = 0; i < seqs.size(); ++i) { PrefixMatch m = store.LongestPrefixMatch(seqs[i], &ca); (void)m; }
    auto l1 = Clock::now(); Report("longest-prefix lookup", seqs.size(), ns(l0, l1));

    // Segmentation of a representative prefix.
    auto s0 = Clock::now();
    auto rec = store.Get(store.LongestPrefixMatch(seqs.front(), &ca).id());
    if (rec.is_ok()) { auto sg = store.Segments(rec.value().id); (void)sg; }
    auto s1 = Clock::now(); Report("segmentation", 1, ns(s0, s1));

    // StateReference attach.
    auto a0 = Clock::now();
    if (rec.is_ok()) { StateReference ref; ref.ns="kv"; ref.object_id="x"; ref.generation=1; ref.compatibility=ca; ref.freshness_ts=1; store.AttachState(rec.value().id, ref); }
    auto a1 = Clock::now(); Report("state-ref attach", 1, ns(a0, a1));

    // Concurrent touch: snapshot (serialization of the whole structure).
    auto p0 = Clock::now();
    std::string blob = store.Index().SerializeBinary();
    auto p1 = Clock::now(); Report("serialization", 1, ns(p0, p1));
    auto sn0 = Clock::now();
    auto snap = store.Snapshot();
    auto sn1 = Clock::now(); Report("snapshot", 1, ns(sn0, sn1));
    std::printf("  footprint=%zu bytes, prefixes=%llu, segments=%llu\n", blob.size(), (unsigned long long)store.PrefixCount(), (unsigned long long)store.SegmentCount());
  }
  std::printf("\ndistributed round trip: see test_distributed (coordinator + 2 nodes over real TCP)\n");
  return 0;
}