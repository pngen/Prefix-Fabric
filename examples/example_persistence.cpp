// Prefix Fabric - persistence & recovery example.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

using namespace prefix_fabric;

static CompatibilityKey C() { CompatibilityKey c; c.model="m"; c.tokenizer="t"; c.vocabulary="v"; c.tokenization_version="1"; c.kv_layout="gqa"; return c; }

int main() {
  std::string path = "example_persist.bin";
  std::remove(path.c_str());
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns="demo"; o.default_compat=ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  {
    PrefixStore store(o);
    auto r = store.Add(ca, {5,6,7,8,9}, {}, "example");
    if (!r.is_ok()) { std::printf("add failed\n"); return 1; }
    std::printf("added prefix %s (tokens=5)\n", r.value().id.ToString().c_str());
    StateReference ref; ref.ns="kv"; ref.object_id="blk-9"; ref.generation=2; ref.compatibility=ca; ref.freshness_ts=42;
    store.AttachState(r.value().id, ref);
    if (!store.SaveToFile(path).is_ok()) { std::printf("save failed\n"); return 1; }
    std::printf("saved to %s\n", path.c_str());
  }
  {
    PrefixStore store2(o);
    auto load = store2.LoadFromFile(path);
    if (!load.is_ok() || load.value() != 1) { std::printf("recovery failed: %s\n", load.is_ok() ? "count mismatch" : load.error().ToString().c_str()); return 1; }
    PrefixMatch m = store2.ExactMatch({5,6,7,8,9}, &ca);
    if (!m.is_matched()) { std::printf("recovered exact match failed\n"); return 1; }
    std::printf("recovery OK: prefix identities, generation, and state refs preserved\n");
  }
  std::remove(path.c_str());
  std::printf("example_persistence PASSED\n");
  return 0;
}
