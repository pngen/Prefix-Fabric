// Prefix Fabric - distributed prefix node.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "prefix_fabric/client.hpp"
#include "prefix_fabric/error.hpp"
#include "prefix_fabric/policy.hpp"
#include "prefix_fabric/record.hpp"
#include "prefix_fabric/segment.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/store.hpp"

namespace prefix_fabric {

// A node runs a LOCAL prefix store and participates in the distributed
// directory via a remote client to the coordinator. It can publish local
// prefixes and discover reusable prefixes across other nodes.
class PrefixNode {
 public:
  struct Options {
    Namespace ns = "default";
    CompatibilityKey default_compat;
    std::string coordinator_host = "127.0.0.1";
    std::uint16_t coordinator_port = 0;
    std::string node_name = "node";
    SegmentationConfig segmentation;
    CapacityPolicy capacity = CapacityPolicy::Defaults();
    EvictionConfig eviction;
    std::string boot_id;   // optional fixed boot identity (for fencing tests)
  };

  explicit PrefixNode(const Options& options);
  ~PrefixNode();
  PrefixNode(const PrefixNode&) = delete;
  PrefixNode& operator=(const PrefixNode&) = delete;

  // Connect + register with the coordinator.
  Result<void> Start();
  void Stop();
  bool Connected() const;

  const std::string& BootId() const;
  std::uint64_t Epoch() const;
  PrefixStore& Local();

  // Publish a prefix to the coordinator directory (and add it locally).
  Result<PrefixRecord> Publish(const CompatibilityKey& compat, const TokenSequence& tokens,
                               const std::vector<StateReference>& refs = {}, const std::string& provenance = {});
  Result<PrefixMatch> RemoteLongestMatch(const TokenSequence& request, const CompatibilityKey& compat);
  Result<PrefixMatch> RemoteExactMatch(const TokenSequence& request, const CompatibilityKey& compat);
  Result<void> RemoteInvalidate(const PrefixId& id, bool subtree);
  Result<std::vector<StateReference>> RemoteAttach(const PrefixId& id, const StateReference& ref);
  Result<void> Heartbeat();
  Result<PrefixRecord> RemoteGet(const PrefixId& id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace prefix_fabric
