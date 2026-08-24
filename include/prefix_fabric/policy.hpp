// Prefix Fabric - capacity and eviction policy.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>

#include "prefix_fabric/error.hpp"

namespace prefix_fabric {

// Hard bounds that apply to every runtime. Exceeding them is a recoverable
// error, never a silent unbounded allocation.
struct CapacityPolicy {
  std::uint64_t max_prefixes = 0;            // 0 => large but bounded default
  std::uint64_t max_segments = 0;
  std::uint64_t max_token_index_footprint = 0;  // total stored token bytes
  std::uint64_t max_state_references = 0;
  std::uint64_t max_nodes_total = 0;         // structural index nodes
  std::uint64_t max_serialized_object_bytes = 0;
  std::uint64_t max_network_frame_bytes = 16 * 1024 * 1024;  // 16 MiB default
  std::uint64_t max_subscriptions = 0;
  std::uint64_t max_event_backlog = 0;

  static CapacityPolicy Defaults() {
    CapacityPolicy p;
    p.max_prefixes = 1'000'000;
    p.max_segments = 4'000'000;
    p.max_token_index_footprint = 512ULL * 1024 * 1024;
    p.max_state_references = 2'000'000;
    p.max_nodes_total = 2'000'000;
    p.max_serialized_object_bytes = 512ULL * 1024 * 1024;
    p.max_network_frame_bytes = 16 * 1024 * 1024;
    p.max_subscriptions = 4096;
    p.max_event_backlog = 1'000'000;
    return p;
  }

  Result<void> Validate() const {
    if (max_network_frame_bytes == 0) {
      return Result<void>::Err(Error{ErrorCode::kInvalidArgument,
                                     "max_network_frame_bytes must be > 0"});
    }
    return Result<void>::Ok();
  }
};

// Metadata eviction policy. Deterministic for a given state and configuration.
enum class EvictionPolicy : int {
  kLruRecency = 0,
  kLfuFrequency = 1,
  kComposite = 2,   // weighted recency + frequency + structural value
  kStructuralValue = 3,
};

struct EvictionConfig {
  EvictionPolicy policy = EvictionPolicy::kComposite;
  bool protect_ancestors_of_live_descendants = true;
  bool auto_evict = false;   // when true, inserts evict to stay within capacity
  std::uint64_t max_prefixes = 100'000;      // triggers eviction
  double recency_weight = 0.4;
  double frequency_weight = 0.4;
  double structural_weight = 0.2;            // value of a shared branch point
};

}  // namespace prefix_fabric
