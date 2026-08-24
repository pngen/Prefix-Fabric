// Prefix Fabric - randomized property tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

#include <random>

using namespace prefix_fabric;

static CompatibilityKey CompatA() { CompatibilityKey c; c.model = "mA"; c.tokenizer = "t"; c.vocabulary = "v"; c.tokenization_version = "1"; c.kv_layout = "gqa"; return c; }
static CompatibilityKey CompatB() { CompatibilityKey c; c.model = "mB"; c.tokenizer = "t"; c.vocabulary = "v"; c.tokenization_version = "1"; c.kv_layout = "gqa"; return c; }

static bool IsPrefix(const TokenSequence& a, const TokenSequence& b) {
  if (a.size() > b.size()) return false;
  return std::equal(a.begin(), a.end(), b.begin());
}

PF_TEST(Property_RandomizedInvariants) {
  std::mt19937 rng(12345);
  CompatibilityKey ca = CompatA();
  PrefixStore::Options o;
  o.ns = "prop"; o.default_compat = ca;
  o.segmentation.policy = SegmentationPolicy::kSingle;
  o.capacity = CapacityPolicy::Defaults();
  o.capacity.max_prefixes = 100000;
  o.eviction.auto_evict = false;
  PrefixStore store(o);
  // Deterministic id invariance: two stores with identical inserts produce identical ids.
  PrefixStore store2(o);
  std::vector<PrefixId> ids;
  std::vector<TokenSequence> seqs;
  // Insert a diverse family: random sequences + shared-prefix families + deep/wide branches.
  for (int i = 0; i < 600; ++i) {
    TokenSequence t;
    int len = 1 + (static_cast<int>(rng() % 12));
    for (int j = 0; j < len; ++j) t.push_back(1 + (rng() % 20));
    // Occasionally force a shared prefix with a previous sequence.
    if (i % 4 == 0 && !seqs.empty()) { t.assign(seqs[i % seqs.size()].begin(), seqs[i % seqs.size()].begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, seqs[i % seqs.size()].size()))); for (int j = 0; j < len; ++j) t.push_back(1 + (rng() % 20)); }
    auto r = store.Add(ca, t);
    CHECK(r.is_ok());
    ids.push_back(r.value().id);
    seqs.push_back(t);
  }
  // Verify exact match returns the same sequence (exact never returns a different sequence).
  for (std::size_t i = 0; i < seqs.size(); i += 37) {
    PrefixMatch m = store.ExactMatch(seqs[i], &ca);
    CHECK(m.is_matched());
    CHECK(m.exact());
    CHECK(m.matched_prefix() == TokenSequence(seqs[i]));
  }
  // Random request lookups: longest-prefix must be an actual prefix and never exceed request length.
  for (int i = 0; i < 1200; ++i) {
    TokenSequence req;
    int len = 1 + (static_cast<int>(rng() % 14));
    for (int j = 0; j < len; ++j) req.push_back(1 + (rng() % 20));
    PrefixMatch m = store.LongestPrefixMatch(req, &ca);
    if (m.is_matched()) {
      CHECK(m.matched_token_count() <= static_cast<std::uint64_t>(req.size()));
      CHECK(IsPrefix(m.matched_prefix(), req));
      CHECK(m.matched_token_count() == static_cast<std::uint64_t>(m.matched_prefix().size()));
    }
    // A request with the exact same sequence must match at least that length.
  }
  // Compatibility mismatch never hits.
  CompatibilityKey cb = CompatB();
  for (std::size_t i = 0; i < seqs.size(); i += 41) {
    PrefixMatch m = store.LongestPrefixMatch(seqs[i], &cb);
    CHECK(!m.is_matched());
  }
}

PF_TEST(Property_DeterministicIds) {
  std::mt19937 rng(999);
  CompatibilityKey ca = CompatA();
  PrefixStore::Options o; o.ns = "prop"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle; o.capacity = CapacityPolicy::Defaults(); o.eviction.auto_evict = false;
  PrefixStore s1(o), s2(o);
  for (int i = 0; i < 300; ++i) {
    TokenSequence t; int len = 1 + (rng() % 8); for (int j = 0; j < len; ++j) t.push_back(rng() % 20);
    auto a = s1.Add(ca, t); auto b = s2.Add(ca, t);
    CHECK(a.is_ok() && b.is_ok());
    CHECK(a.value().id == b.value().id);
  }
  // Snapshots must be identical (deterministic serialization ordering).
  auto snap1 = s1.Snapshot(); auto snap2 = s2.Snapshot();
  CHECK_EQ(snap1.records.size(), snap2.records.size());
  for (std::size_t i = 0; i < snap1.records.size() && i < snap2.records.size(); ++i)
    CHECK(snap1.records[i].id == snap2.records[i].id);
}

PF_TEST(Property_BoundedCapacityEviction) {
  CompatibilityKey ca = CompatA();
  PrefixStore::Options o; o.ns = "prop"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  o.capacity = CapacityPolicy::Defaults(); o.capacity.max_prefixes = 50;
  o.eviction.auto_evict = true;
  o.eviction.policy = EvictionPolicy::kComposite;
  PrefixStore store(o);
  std::mt19937 rng(7);
  for (int i = 0; i < 300; ++i) {
    TokenSequence t; int len = 1 + (rng() % 6); for (int j = 0; j < len; ++j) t.push_back(rng() % 10);
    auto r = store.Add(ca, t);
    CHECK(r.is_ok());
    // Prefix count must never exceed the bounded capacity.
    CHECK(store.PrefixCount() <= 50);
  }
  CHECK(store.PrefixCount() > 0);
}

int main() { return pftest::RunAll(); }
