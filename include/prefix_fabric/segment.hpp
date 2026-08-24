// Prefix Fabric - segment identity and segmentation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <optional>

#include "prefix_fabric/id.hpp"
#include "prefix_fabric/hash.hpp"
#include "prefix_fabric/state_reference.hpp"

namespace prefix_fabric {

// A logical prefix may be represented as one or more canonical segments. A
// segment references a contiguous slice of the prefix's tokens and may carry an
// external StateReference. Segmentation of the same logical prefix with the same
// policy is deterministic.
struct PrefixSegment {
  SegmentId id;             // stable segment identity
  std::uint64_t token_offset = 0;
  std::uint64_t token_count = 0;
  ContentHash content_hash{};
  std::optional<StateReference> state_ref;  // optional external state
  Generation generation = 0;               // segment lifetime generation
  bool active = true;                      // false if retired/invalidated

  bool operator==(const PrefixSegment& o) const {
    return id == o.id && token_offset == o.token_offset &&
           token_count == o.token_count && content_hash == o.content_hash &&
           state_ref == o.state_ref && generation == o.generation &&
           active == o.active;
  }
};

// Segmentation policy. Identical policy + identical prefix => identical segments.
enum class SegmentationPolicy : int {
  kSingle = 0,        // one segment covering the whole prefix
  kFixed = 1,         // fixed-size token blocks
  kCaller = 2,        // explicit caller-provided boundaries
  kAdaptive = 3,      // common-prefix-aware
};

struct SegmentationConfig {
  SegmentationPolicy policy = SegmentationPolicy::kSingle;
  std::uint64_t fixed_size = 0;      // for kFixed
  std::uint64_t target_size = 0;     // for kAdaptive
};

}  // namespace prefix_fabric
