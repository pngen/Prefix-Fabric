// Prefix Fabric - snapshots (consistent, deterministic views).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "prefix_fabric/record.hpp"

namespace prefix_fabric {

// A point-in-time, consistent snapshot of the prefix metadata graph. Snapshots
// are immutable after creation and serialize deterministically.
struct PrefixSnapshot {
  std::uint64_t sequence = 0;               // snapshot number
  std::int64_t captured_at_ns = 0;
  CompatibilityKey compat;                  // if focused on one compat tree
  std::vector<PrefixRecord> records;        // all active records
  std::uint64_t prefix_count = 0;
  std::uint64_t segment_count = 0;
  std::uint64_t token_count = 0;
  std::uint64_t state_reference_count = 0;

  bool empty() const { return records.empty(); }
  std::uint64_t SizeBytesApprox() const;
};

}  // namespace prefix_fabric
