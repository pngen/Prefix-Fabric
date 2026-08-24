// Prefix Fabric - prefix records, paths, lineage.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/id.hpp"
#include "prefix_fabric/hash.hpp"
#include "prefix_fabric/segment.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

// A materialized prefix record. Represents a known, reusable prefix in the
// structural index. Prefix Fabric owns ONLY this metadata; the state the record
// may reference is owned by another subsystem.
struct PrefixRecord {
  PrefixId id;
  Namespace ns;
  CompatibilityKey compat;
  Generation generation = 0;
  std::uint64_t token_count = 0;
  TokenSequence tokens;       // canonical logical content
  ContentHash content_hash{};
  std::optional<PrefixId> parent;  // structural parent (root has none)
  std::uint32_t branch_depth = 0;
  std::vector<PrefixSegment> segments;    // segmentation
  std::vector<StateReference> state_refs; // external attached state
  std::string provenance;                 // creation provenance
  bool active = true;                     // false if retired/invalidated

  bool operator==(const PrefixRecord& o) const {
    return id == o.id && ns == o.ns && compat == o.compat &&
           generation == o.generation && token_count == o.token_count &&
           tokens == o.tokens && content_hash == o.content_hash &&
           parent == o.parent && branch_depth == o.branch_depth &&
           segments == o.segments && state_refs == o.state_refs &&
           provenance == o.provenance && active == o.active;
  }
};

// A structural path from the root to a prefix.
struct PrefixPath {
  std::vector<PrefixId> nodes;         // node ids along the path (excludes root)
  std::vector<TokenSequence> edge_labels;  // edge label for each node
  std::vector<std::string> edge_hex;    // shared hex label for diagnostics
  std::uint64_t token_count = 0;
};

// Lineage of a prefix within the structural DAG.
struct PrefixLineage {
  std::vector<PrefixRecord> ancestors;   // root .. parent
  std::vector<PrefixRecord> descendants; // all descendants (preorder)
  std::vector<PrefixId> common_ancestors;// shared ancestors for a query pair
  std::uint64_t branch_depth = 0;
  std::uint64_t descendant_count = 0;
  bool is_leaf = true;
};

}  // namespace prefix_fabric
