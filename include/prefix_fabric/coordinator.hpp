// Prefix Fabric - distributed prefix directory coordinator.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "prefix_fabric/error.hpp"
#include "prefix_fabric/id.hpp"
#include "prefix_fabric/policy.hpp"
#include "prefix_fabric/segment.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/store.hpp"

namespace prefix_fabric {

// The coordinator is the authoritative metadata authority for a distributed
// prefix directory. Nodes register with a boot identity and issue remote
// publish/lookup/invalidate/attach operations. The coordinator fences stale
// authorities using a partition epoch + registered boot-id liveness.
class PrefixCoordinator {
 public:
  struct Options {
    Namespace ns = "default";
    CompatibilityKey default_compat;
    std::string persistence_path;   // optional; empty => in-memory only
    std::uint16_t listen_port = 0;  // 0 => ephemeral
    std::string bind_host = "127.0.0.1";
    SegmentationConfig segmentation;
    CapacityPolicy capacity = CapacityPolicy::Defaults();
    EvictionConfig eviction;
  };

  explicit PrefixCoordinator(const Options& options);
  ~PrefixCoordinator();
  PrefixCoordinator(const PrefixCoordinator&) = delete;
  PrefixCoordinator& operator=(const PrefixCoordinator&) = delete;

  // Bind and start the accept loop. Returns the bound port.
  Result<std::uint16_t> Start();
  void Stop();
  bool Running() const;

  std::uint64_t Epoch() const;
  std::size_t NodeCount() const;
  Result<RuntimeStatistics> Stats() const;
  PrefixStore& Store();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace prefix_fabric
