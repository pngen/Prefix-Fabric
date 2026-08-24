// Prefix Fabric - core correctness tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"

#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

using namespace prefix_fabric;

static CompatibilityKey Compat(const char* model) {
  CompatibilityKey c;
  c.model = model;
  c.tokenizer = "tok-v1";
  c.vocabulary = "v0";
  c.tokenization_version = "1";
  c.special_token_policy = "default";
  c.prompt_template = "none";
  c.position_encoding = "rope";
  c.kv_layout = "gqa";
  c.runtime_format = "pf-cpp";
  return c;
}

static PrefixStore::Options Opts(const CompatibilityKey& c, const Namespace& ns = "test") {
  PrefixStore::Options o;
  o.ns = ns;
  o.default_compat = c;
  o.segmentation.policy = SegmentationPolicy::kFixed;
  o.segmentation.fixed_size = 16;
  o.capacity = CapacityPolicy::Defaults();
  o.eviction.auto_evict = false;
  return o;
}

PF_TEST(Identity_Deterministic) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  PrefixStore store(o);
  TokenSequence t = {10, 20, 30, 40, 50};
  auto a = store.Add(c, t);
  CHECK(a.is_ok());
  // A fresh store with identical inputs must produce the identical id.
  PrefixStore store2(Opts(c));
  auto b = store2.Add(c, t);
  CHECK(b.is_ok());
  CHECK(a.value().id == b.value().id);
  CHECK_EQ(a.value().token_count, 5u);
}

PF_TEST(Branching_SharedPrefix) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  TokenSequence abcde = {1, 2, 3, 4, 5};
  TokenSequence abcfg = {1, 2, 3, 6, 7};
  TokenSequence abh   = {1, 2, 8};
  TokenSequence aij   = {1, 9, 10};
  auto r1 = store.Add(c, abcde);
  auto r2 = store.Add(c, abcfg);
  auto r3 = store.Add(c, abh);
  auto r4 = store.Add(c, aij);
  CHECK(r1.is_ok() && r2.is_ok() && r3.is_ok() && r4.is_ok());
  // Longest match for a request that extends AB C F G.
  TokenSequence req = {1, 2, 3, 6, 9};
  PrefixMatch m = store.LongestPrefixMatch(req);
  CHECK(m.is_matched());
  CHECK_EQ(m.matched_token_count(), 3u);   // A B C (a structural shared prefix)
  CHECK(m.matched_prefix() == TokenSequence({1, 2, 3}));
  // Longest match for A B alone should return the structural AB prefix (len 2).
  TokenSequence reqAB = {1, 2};
  PrefixMatch mAB = store.LongestPrefixMatch(reqAB);
  CHECK(mAB.is_matched());
  CHECK_EQ(mAB.matched_token_count(), 2u);
  // Exact match for ABCDE.
  PrefixMatch ex = store.ExactMatch(abcde);
  CHECK(ex.is_matched());
  CHECK(ex.exact());
  // Common-prefix sharing: node count should be far fewer than 4 sequences.
  CHECK(store.NodeCount() < 20);
}

PF_TEST(Compatibility_MismatchNoHit) {
  auto ca = Compat("modelA");
  auto cb = Compat("modelB");
  PrefixStore store(Opts(ca));
  TokenSequence t = {10, 20, 30};
  auto r = store.Add(ca, t);
  CHECK(r.is_ok());
  // Same tokens under a DIFFERENT compatibility key must never hit.
  PrefixMatch m = store.LongestPrefixMatch(t, &cb);
  CHECK(!m.is_matched());
  // Exact match under the wrong compat must not hit either.
  PrefixMatch me = store.ExactMatch(t, &cb);
  CHECK(!me.is_matched());
}

PF_TEST(Segmentation_Fixed) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.segmentation.policy = SegmentationPolicy::kFixed;
  o.segmentation.fixed_size = 2;
  PrefixStore store(o);
  TokenSequence t = {1, 2, 3, 4, 5};
  auto r = store.Add(c, t);
  CHECK(r.is_ok());
  auto segs = store.Segments(r.value().id);
  CHECK(segs.is_ok());
  CHECK_EQ(segs.value().size(), 3u);  // [12][34][5]
  CHECK_EQ(segs.value()[0].token_count, 2u);
  CHECK_EQ(segs.value()[1].token_count, 2u);
  CHECK_EQ(segs.value()[2].token_count, 1u);
}

PF_TEST(Compose_And_Slice) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  TokenSequence segA = {1, 2};
  TokenSequence segB = {3, 4};
  auto a = store.Add(c, segA);
  auto b = store.Add(c, segB);
  CHECK(a.is_ok() && b.is_ok());
  auto comp = store.Compose(c, {a.value().id, b.value().id});
  CHECK(comp.is_ok());
  CHECK_EQ(comp.value().token_count, 4u);
  // The composed prefix should be exactly [1,2,3,4] and matchable.
  TokenSequence req = {1, 2, 3, 4};
  PrefixMatch m = store.ExactMatch(req);
  CHECK(m.is_matched());
  // Slice the composed prefix to 3 tokens -> [1,2,3].
  auto sl = store.Slice(comp.value().id, 3);
  CHECK(sl.is_ok());
  CHECK_EQ(sl.value().token_count, 3u);
}

PF_TEST(StateReference_StaleRejected) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  TokenSequence t = {5, 6, 7, 8};
  auto r = store.Add(c, t);
  CHECK(r.is_ok());
  StateReference ref;
  ref.ns = "kv";
  ref.object_id = "blk-0001";
  ref.generation = 1;
  ref.type = StateReference::Type::kKvFabric;
  ref.compatibility = c;
  ref.freshness_ts = 1000;
  auto at = store.AttachState(r.value().id, ref);
  CHECK(at.is_ok());
  TokenSequence req = {5, 6, 7, 8, 9};
  auto plan = store.BuildReusePlan(req);
  CHECK(plan.matched);
  CHECK_EQ(plan.usable_refs.size(), 1u);
  // Invalidate the prefix -> its refs become stale and must not be "usable".
  store.Invalidate(r.value().id);
  auto plan2 = store.BuildReusePlan(req);
  CHECK(!plan2.matched);   // invalidated prefix is not reusable
}

PF_TEST(Persistence_RoundTrip) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  TokenSequence t = {1, 2, 3, 4, 5};
  auto r = store.Add(c, t);
  CHECK(r.is_ok());
  std::string path = "tests_output_core.bin";
  auto sv = store.SaveToFile(path);
  CHECK(sv.is_ok());
  PrefixStore store2(Opts(c));
  auto load = store2.LoadFromFile(path);
  if (load.is_err()) { std::printf("    [INFO] load error: %s\n", load.error().ToString().c_str()); }
  CHECK(load.is_ok());
  CHECK_EQ(load.value(), 1u);
  PrefixMatch m = store2.ExactMatch(t);
  CHECK(m.is_matched());
  PrefixMatch mp = store2.LongestPrefixMatch({1, 2, 3, 4, 5, 6});
  CHECK(mp.is_matched());
  CHECK_EQ(mp.matched_token_count(), 5u);
  // Identity preserved across round trip.
  auto rr = store2.Get(r.value().id);
  CHECK(rr.is_ok());
  CHECK(rr.value().id == r.value().id);
  std::remove(path.c_str());
}

PF_TEST(Ancestors_Descendants_Branches) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  TokenSequence abcde = {1, 2, 3, 4, 5};
  TokenSequence abcfg = {1, 2, 3, 6, 7};
  TokenSequence abh   = {1, 2, 8};
  store.Add(c, abcde);
  store.Add(c, abcfg);
  store.Add(c, abh);
  // Find the ABC node and check its descendants.
  TokenSequence abc = {1, 2, 3};
  PrefixMatch m = store.LongestPrefixMatch(abc);
  CHECK(m.is_matched());
  auto desc = store.Descendants(m.id());
  CHECK(desc.is_ok());
  CHECK_EQ(desc.value().size(), 2u);  // ABCDE and ABCFG
  auto anc = store.Ancestors(m.id());
  CHECK(anc.is_ok());
  // With abcde/abcfg/abh, ancestors of ABC is AB (len2) only.
  CHECK_EQ(anc.value().size(), 1u);
  CHECK_EQ(anc.value()[0].token_count, 2u);
  // Descendants of ABC are ABCDE and ABCFG (2).
  CHECK_EQ(desc.value().size(), 2u);
  auto bp = store.BranchPoints(m.id());
  CHECK(bp.is_ok());
  // Branch points are AB and ABC (2).
  CHECK_EQ(bp.value().size(), 2u);
}

PF_TEST(Invalidate_Subtree) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  store.Add(c, {1, 2, 3, 4});
  store.Add(c, {1, 2, 3, 5});
  TokenSequence abc = {1, 2, 3};
  PrefixMatch m = store.LongestPrefixMatch(abc);
  CHECK(m.is_matched());
  CHECK_EQ(m.matched_token_count(), 3u);
  store.Invalidate(m.id(), /*subtree*/ true);
  // The whole subtree containing [1,2,3,4] and [1,2,3,5] is invalidated.
  PrefixMatch m2 = store.LongestPrefixMatch({1, 2, 3, 4, 9});
  CHECK(!m2.is_matched());
}

int main() { return pftest::RunAll(); }