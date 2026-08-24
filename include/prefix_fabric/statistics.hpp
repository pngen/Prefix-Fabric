// Prefix Fabric - bounded, observability-only statistics.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>

namespace prefix_fabric {

// Per-prefix statistics. These are informational only and MUST NOT affect
// correctness. Thread-unsafe by design; read via atomic snapshot APIs.
struct PrefixStatistics {
  std::uint64_t lookup_count = 0;
  std::uint64_t hit_count = 0;
  std::uint64_t exact_hit_count = 0;
  std::uint64_t partial_hit_count = 0;
  std::uint64_t reused_tokens = 0;
  std::uint64_t last_access_ns = 0;
  std::uint64_t branch_fanout = 0;
  std::uint64_t descendant_count = 0;
  std::uint64_t attached_state_count = 0;
  std::uint64_t attached_state_available = 0;
};

// Aggregated runtime statistics.
struct RuntimeStatistics {
  std::uint64_t prefix_count = 0;
  std::uint64_t segment_count = 0;
  std::uint64_t node_count = 0;
  std::uint64_t state_reference_count = 0;
  std::uint64_t token_index_footprint_bytes = 0;
  std::uint64_t total_lookups = 0;
  std::uint64_t total_hits = 0;
  std::uint64_t total_exact_hits = 0;
  std::uint64_t total_partial_hits = 0;
  std::uint64_t total_reused_tokens = 0;
  std::uint64_t total_invalidations = 0;
  std::uint64_t total_evictions = 0;
  std::uint64_t total_reference_attachments = 0;
  std::uint64_t total_reference_retirements = 0;
  std::uint64_t total_stale_authority_rejections = 0;
  std::uint64_t total_persistence_failures = 0;
  std::uint64_t total_corruption_rejections = 0;
  std::uint64_t total_cycles_rejected = 0;
};

}  // namespace prefix_fabric
