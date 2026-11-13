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

static StateReference Ref(const char* object, std::uint64_t generation) {
  StateReference r;
  r.ns = "kv"; r.object_id = object; r.generation = generation;
  r.type = StateReference::Type::kKvFabric;
  return r;
}

static std::size_t CountOf(const std::string& text, const std::string& what) {
  std::size_t n = 0;
  for (std::size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + what.size())) ++n;
  return n;
}

PF_TEST(Capacity_BoundsAreExact) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.capacity.max_prefixes = 2;
  PrefixStore store(o);
  CHECK(store.Add(c, {1, 2, 3}).is_ok());
  // An insert that shares a prefix with a stored one adds two prefixes (the
  // shared part and the new sequence). That does not fit under a bound of two.
  auto over = store.Add(c, {1, 2, 4});
  CHECK(over.is_err());
  if (over.is_err()) CHECK(over.error().code() == ErrorCode::kCapacityExceeded);
  CHECK_EQ(store.PrefixCount(), 1u);
  CHECK(store.Add(c, {7}).is_ok());  // exactly reaches the bound
  CHECK_EQ(store.PrefixCount(), 2u);
  // Adding what is already stored needs no room, even at the bound.
  CHECK(store.Add(c, {1, 2, 3}).is_ok());
  CHECK_EQ(store.PrefixCount(), 2u);
  // A refused insert leaves nothing behind, not even an empty tree for a new key.
  auto other = Compat("m2");
  CHECK(store.Add(other, {5, 6}).is_err());
  CHECK_EQ(CountOf(store.DebugDump(), "tree["), 1u);
}

PF_TEST(Capacity_SegmentBoundCountsSegments) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.segmentation.policy = SegmentationPolicy::kSingle;  // one segment per prefix
  o.segmentation.fixed_size = 0;
  o.capacity.max_segments = 4;
  PrefixStore store(o);
  TokenSequence long_prefix(100);
  for (std::size_t i = 0; i < long_prefix.size(); ++i) long_prefix[i] = static_cast<TokenId>(i + 1);
  CHECK(store.Add(c, long_prefix).is_ok());
  CHECK_EQ(store.SegmentCount(), 1u);
}

PF_TEST(Capacity_UnsetBoundsUseDefaults) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  CapacityPolicy p;  // one bound chosen; zero means "the default" for the others
  p.max_prefixes = 10;
  o.capacity = p;
  PrefixStore store(o);
  CHECK(store.Capacity().max_nodes_total > 0);
  CHECK(store.Capacity().max_event_backlog > 0);
  auto r = store.Add(c, {1, 2, 3});
  CHECK(r.is_ok());
  if (r.is_ok()) {
    CHECK(store.Subscribe([](const PrefixEvent&) {}).is_ok());
    CHECK(store.LongestPrefixMatch({1, 2, 3, 4}).is_matched());
    CHECK(!store.LongestPrefixMatch({9}).is_matched());
  }
}

PF_TEST(Split_DoesNotInheritReferences) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  CHECK(store.Add(c, {1, 2, 3, 4}).is_ok());
  auto longer = store.Add(c, {1, 2, 3, 5}, {Ref("blk-5", 1)});
  CHECK(longer.is_ok());
  CHECK_EQ(store.ReferenceCount(), 1u);
  // {1,2,3} is a shorter prefix that nobody attached state to.
  auto plan = store.BuildReusePlan({1, 2, 3, 9});
  CHECK(plan.matched);
  CHECK_EQ(plan.matched_token_count, 3u);
  CHECK(plan.usable_refs.empty());
  // Retiring the reference from the prefix it was attached to retires it everywhere.
  if (longer.is_ok()) CHECK(store.RetireState(longer.value().id, Ref("blk-5", 1)).is_ok());
  CHECK_EQ(store.ReferenceCount(), 0u);
  CHECK(store.BuildReusePlan({1, 2, 3, 5, 7}).usable_refs.empty());
  CHECK(store.BuildReusePlan({1, 2, 3, 9}).usable_refs.empty());
}

PF_TEST(Split_KeepsLineage) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  auto a = store.Add(c, {1, 2, 3, 4});
  auto b = store.Add(c, {1, 2, 3, 4, 5, 6});
  auto top = store.Add(c, {1, 2});  // becomes a node above both of them
  CHECK(a.is_ok() && b.is_ok() && top.is_ok());
  if (!(a.is_ok() && b.is_ok() && top.is_ok())) return;
  auto st = store.Statistics(top.value().id);
  CHECK(st.is_ok() && st.value().descendant_count == 2);
  auto deep = store.Get(b.value().id);
  CHECK(deep.is_ok() && deep.value().branch_depth == 3);
  // It has descendants: removing it without cascade is refused and removes nothing.
  CHECK(store.Remove(top.value().id, /*cascade*/ false).is_err());
  CHECK_EQ(store.PrefixCount(), 3u);
  CHECK(store.ExactMatch({1, 2, 3, 4, 5, 6}).is_matched());
}

PF_TEST(Evict_RemovesOnlyWhatItReports) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  CHECK(store.Add(c, {1}).is_ok());
  CHECK(store.Add(c, {1, 2}).is_ok());
  CHECK(store.Add(c, {1, 2, 3}).is_ok());
  for (int i = 0; i < 3; ++i) CHECK(store.ExactMatch({1, 2}).is_matched());  // the hot one
  auto ev = store.Evict(2);
  CHECK(ev.is_ok());
  if (ev.is_ok()) CHECK_EQ(ev.value().size(), 2u);
  // Two were asked for and two are reported, so one of the three is left.
  // Prefixes without descendants go first, which leaves the common ancestor.
  CHECK_EQ(store.PrefixCount(), 1u);
  CHECK(store.ExactMatch({1}).is_matched());
}

PF_TEST(Evict_AutoEvictionAdmitsNewWork) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.capacity.max_prefixes = 2;
  o.eviction.auto_evict = true;
  PrefixStore store(o);
  auto a = store.Add(c, {1});
  auto b = store.Add(c, {2});
  CHECK(a.is_ok() && b.is_ok());
  CHECK(store.ExactMatch({1}).is_matched());
  CHECK(store.ExactMatch({2}).is_matched());
  // At the bound: the new prefix is the most recent one and stays, and the least
  // recently used one makes room for it.
  CHECK(store.Add(c, {3}).is_ok());
  CHECK_EQ(store.PrefixCount(), 2u);
  CHECK(store.ExactMatch({3}).is_matched());
  CHECK(!store.ExactMatch({1}).is_matched());
  CHECK(store.ExactMatch({2}).is_matched());
  // An invalidated prefix is dead weight and goes before any live one.
  if (b.is_ok()) CHECK(store.Invalidate(b.value().id).is_ok());
  CHECK(store.Add(c, {4}).is_ok());
  CHECK_EQ(store.PrefixCount(), 2u);
  CHECK(store.ExactMatch({3}).is_matched());
  CHECK(store.ExactMatch({4}).is_matched());
}

PF_TEST(Evict_HonoursConfiguredPolicy) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.eviction.policy = EvictionPolicy::kLruRecency;
  PrefixStore store(o);
  CHECK(store.Add(c, {1}).is_ok());
  CHECK(store.Add(c, {2}).is_ok());
  for (int i = 0; i < 5; ++i) CHECK(store.ExactMatch({1}).is_matched());  // used often, but earlier
  CHECK(store.ExactMatch({2}).is_matched());                              // used once, most recently
  auto ev = store.Evict(1);
  CHECK(ev.is_ok() && ev.value().size() == 1);
  CHECK(!store.ExactMatch({1}).is_matched());  // least recently used is the one evicted
  CHECK(store.ExactMatch({2}).is_matched());
}

PF_TEST(StateReference_InvalidatedObjectIsNotUsable) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  auto r = store.Add(c, {5, 6, 7});
  CHECK(r.is_ok());
  if (!r.is_ok()) return;
  // A reference that carries no freshness timestamp.
  CHECK(store.AttachState(r.value().id, Ref("blk-1", 1)).is_ok());
  CHECK_EQ(store.BuildReusePlan({5, 6, 7, 8}).usable_refs.size(), 1u);
  CHECK(store.InvalidateStateByObject("kv", "blk-1").is_ok());
  auto plan = store.BuildReusePlan({5, 6, 7, 8});
  CHECK(plan.matched);
  CHECK(plan.usable_refs.empty());
  CHECK_EQ(plan.stale_refs.size(), 1u);
}

PF_TEST(StateReference_GenerationSupersedes) {
  auto c = Compat("m1");
  PrefixStore::Options o = Opts(c);
  o.capacity.max_state_references = 2;
  PrefixStore store(o);
  auto r = store.Add(c, {5, 6, 7});
  CHECK(r.is_ok());
  if (!r.is_ok()) return;
  const PrefixId id = r.value().id;
  CHECK(store.AttachState(id, Ref("blk-1", 1)).is_ok());
  CHECK_EQ(store.ReferenceCount(), 1u);
  // The owner rebuilt the object: the newer generation replaces the attachment.
  auto newer = store.AttachState(id, Ref("blk-1", 2));
  CHECK(newer.is_ok() && newer.value().size() == 1);
  if (newer.is_ok() && newer.value().size() == 1) CHECK_EQ(newer.value()[0].generation, 2u);
  CHECK_EQ(store.ReferenceCount(), 1u);
  // A reference to the generation that was replaced is stale and refused.
  auto older = store.AttachState(id, Ref("blk-1", 1));
  CHECK(older.is_err());
  if (older.is_err()) CHECK(older.error().code() == ErrorCode::kStaleGeneration);
  // The reference bound applies to attachments too.
  CHECK(store.AttachState(id, Ref("blk-2", 1)).is_ok());
  auto third = store.AttachState(id, Ref("blk-3", 1));
  CHECK(third.is_err());
  if (third.is_err()) CHECK(third.error().code() == ErrorCode::kCapacityExceeded);
  CHECK_EQ(store.ReferenceCount(), 2u);
}

PF_TEST(Invalidated_PrefixCanBeAddedAgain) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  auto first = store.Add(c, {5, 6, 7}, {Ref("blk-old", 1)});
  CHECK(first.is_ok());
  if (!first.is_ok()) return;
  CHECK(store.Invalidate(first.value().id).is_ok());
  CHECK(!store.ExactMatch({5, 6, 7}).is_matched());
  // Adding it again makes it current under a newer generation. The state that
  // was attached to the retired generation does not come back with it.
  auto again = store.Add(c, {5, 6, 7}, {Ref("blk-new", 1)});
  CHECK(again.is_ok());
  if (!again.is_ok()) return;
  CHECK(again.value().id == first.value().id);
  CHECK(again.value().active);
  CHECK(again.value().generation > first.value().generation);
  CHECK(store.ExactMatch({5, 6, 7}).is_matched());
  auto plan = store.BuildReusePlan({5, 6, 7, 8});
  CHECK(plan.matched);
  CHECK_EQ(plan.usable_refs.size(), 1u);
  if (plan.usable_refs.size() == 1) CHECK(plan.usable_refs[0].object_id == "blk-new");
  CHECK_EQ(store.ReferenceCount(), 1u);
}

PF_TEST(Lineage_AnswersDoNotDependOnHistory) {
  auto c = Compat("m1");
  PrefixStore store(Opts(c));
  // The shared length of two sequences is a property of the sequences.
  CHECK_EQ(store.CommonPrefixLength({1, 2, 3}, {1, 2, 4}), 2u);
  auto a = store.Add(c, {1, 2});
  auto b = store.Add(c, {3, 4});
  auto x = store.Add(c, {5});
  CHECK(a.is_ok() && b.is_ok() && x.is_ok());
  if (!(a.is_ok() && b.is_ok() && x.is_ok())) return;
  auto before = store.CommonAncestors(a.value().id, b.value().id);
  CHECK(before.is_ok() && before.value().empty());
  CHECK(store.Remove(x.value().id).is_ok());
  // Removing an unrelated prefix changes nothing about these two.
  auto after = store.CommonAncestors(a.value().id, b.value().id);
  CHECK(after.is_ok() && after.value().empty());
  CHECK(store.Get(x.value().id).is_err());
}

int main() { return pftest::RunAll(); }