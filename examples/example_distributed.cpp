// Prefix Fabric - distributed coordinator + nodes example (in-process).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include <cstdio>
#include <string>
#include "prefix_fabric/coordinator.hpp"
#include "prefix_fabric/node.hpp"
#include "prefix_fabric/compatibility.hpp"

using namespace prefix_fabric;

static CompatibilityKey C() { CompatibilityKey c; c.model="m"; c.tokenizer="t"; c.vocabulary="v"; c.tokenization_version="1"; c.kv_layout="gqa"; return c; }

int main() {
  PrefixCoordinator::Options copts; copts.ns = "demo"; copts.default_compat = C(); copts.listen_port = 0;
  PrefixCoordinator coord(copts);
  auto port = coord.Start();
  if (port.is_err()) { std::printf("coordinator start failed\n"); return 1; }
  std::printf("coordinator listening on port %u, epoch %llu\n", (unsigned)port.value(), (unsigned long long)coord.Epoch());
{
    PrefixNode::Options oa; oa.ns="demo"; oa.default_compat=C(); oa.coordinator_port=port.value(); oa.node_name="node-A";
    PrefixNode n1(oa);
    if (!n1.Start().is_ok()) { std::printf("node A start failed\n"); return 1; }
    auto p1 = n1.Publish(C(), {1,2,3}, {}, "demo");
    if (!p1.is_ok()) { std::printf("node A publish failed\n"); return 1; }
    std::printf("node A published %s (tokens=3)\n", p1.value().id.ToString().c_str());
    PrefixNode::Options ob; ob.ns="demo"; ob.default_compat=C(); ob.coordinator_port=port.value(); ob.node_name="node-B";
    PrefixNode n2(ob);
    if (!n2.Start().is_ok()) { std::printf("node B start failed\n"); return 1; }
    auto m = n2.RemoteLongestMatch({1,2,3,9}, C());
    if (m.is_err() || !m.value().is_matched() || m.value().matched_token_count() != 3) { std::printf("cross-node discovery failed\n"); return 1; }
    std::printf("node B discovered node A prefix via remote longest-match (3 tokens)\n");
    n1.Stop(); n2.Stop();
  }
  coord.Stop();
  std::printf("example_distributed PASSED\n");
  return 0;
}