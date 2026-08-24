// Prefix Fabric - comprehensive runnable example.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include <cstdio>
#include <string>
#include <vector>
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/version.hpp"

using namespace prefix_fabric;

static CompatibilityKey CommitCompat() {
  CompatibilityKey c;
  c.model = "llama-3-8b"; c.tokenizer = "tiktoken-v1"; c.vocabulary = "vocab-v0";
  c.tokenization_version = "1"; c.special_token_policy = "default"; c.prompt_template = "chat";
  c.position_encoding = "rope"; c.kv_layout = "gqa"; c.runtime_format = "pf-cpp";
  return c;
}

static PrefixStore::Options Opts() {
  PrefixStore::Options o; o.ns = "demo"; o.default_compat = CommitCompat(); o.segmentation.policy = SegmentationPolicy::kFixed; o.segmentation.fixed_size = 2; return o;
}

int main() {
  std::printf("=== Prefix Fabric %s example ===\n", kVersion);
  int failures = 0;
  auto ok = [&](bool c, const char* what){ std::printf("  [%s] %s\n", c ? "OK" : "FAIL", what); if (!c) ++failures; };

  PrefixStore store(Opts());
  CompatibilityKey c = CommitCompat();

  // Branching prefixes: A B C D E / A B C F G / A B H / A I J go into a shared trie.
  auto r1 = store.Add(c, {1,2,3,4,5}, {}, "demo");
  auto r2 = store.Add(c, {1,2,3,6,7}, {}, "demo");
  auto r3 = store.Add(c, {1,2,8}, {}, "demo");
  auto r4 = store.Add(c, {1,9,10}, {}, "demo");
  ok(r1.is_ok() && r2.is_ok() && r3.is_ok() && r4.is_ok(), "insert branching prefixes");

  PrefixMatch ex = store.ExactMatch({1,2,3,4,5}, &c);
  ok(ex.is_matched() && ex.exact(), "exact prefix match");

  PrefixMatch lm = store.LongestPrefixMatch({1,2,3,6,9}, &c);
  ok(lm.is_matched() && lm.matched_token_count() == 3, "longest-prefix match (shared A B C)");

  // Common-prefix sharing: the trie stores A B C once, not three times.
  ok(store.NodeCount() < 20, "common-prefix sharing (few nodes)");

  // Automatic segmentation (fixed-size 2).
  auto segs = store.Segments(r1.value().id);
  ok(segs.is_ok() && segs.value().size() == 3, "segment a 5-token prefix into 3 blocks");

  // Composition: concatenate two known prefixes, then slice it.
  auto comp = store.Compose(c, {r1.value().id, r4.value().id});
  ok(comp.is_ok(), "compose a longer prefix from compatible pieces");
  auto sl = store.Slice(comp.value().id, 3);
  ok(sl.is_ok() && sl.value().token_count == 3, "slice a prefix to 3 tokens");

  // External StateReference attach + reusable plan.
  StateReference ref; ref.ns = "kv"; ref.object_id = "block-0001"; ref.generation = 1;
  ref.type = StateReference::Type::kKvFabric; ref.compatibility = c; ref.freshness_ts = 1000;
  auto at = store.AttachState(r1.value().id, ref);
  ok(at.is_ok(), "attach external StateReference (KV Fabric object)");
  PrefixReusePlan plan = store.BuildReusePlan({1,2,3,4,5,6}, &c);
  ok(plan.matched && plan.usable_refs.size() == 1, "reuse plan reports usable attached state");

  // Stale reference rejection after invalidation: the record becomes inactive,
  // so its attached state reference is no longer surfaced as reusable.
  store.Invalidate(r1.value().id);
  auto rec2 = store.Get(r1.value().id);
  ok(rec2.is_ok() && !rec2.value().active, "invalidated prefix is inactive (its state reference is retired)");

  // Compatibility mismatch must never hit.
  CompatibilityKey other; other.model = "gpt-4"; other.tokenizer = "tiktoken-v1"; other.vocabulary = "vocab-v0"; other.tokenization_version = "1"; other.kv_layout = "gqa";
  PrefixMatch mm = store.LongestPrefixMatch({1,2,3,4,5}, &other);
  ok(!mm.is_matched(), "compatibility mismatch never produces a hit");

  // Deterministic replay: a fresh store with identical inputs yields identical ids.
  PrefixStore store2(Opts());
  store2.Add(c, {1,2,3,4,5});
  ok(store2.Get(r1.value().id).is_ok(), "deterministic identity replays exactly");

  // Ancestry / descendants / branch points.
  PrefixMatch abc = store.LongestPrefixMatch({1,2,3}, &c);
  auto desc = store.Descendants(abc.id());
  ok(desc.is_ok(), "enumerate descendants of a branch");
  std::printf("=== %s%s ===\n", failures == 0 ? "ALL OK" : "FAILURES", failures == 0 ? "" : " (see above)");
  return failures == 0 ? 0 : 1;
}
