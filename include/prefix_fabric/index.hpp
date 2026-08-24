// Prefix Fabric - structural prefix index.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "prefix_fabric/canonical.hpp"
#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/error.hpp"
#include "prefix_fabric/id.hpp"
#include "prefix_fabric/match.hpp"
#include "prefix_fabric/policy.hpp"
#include "prefix_fabric/record.hpp"
#include "prefix_fabric/segment.hpp"
#include "prefix_fabric/snapshot.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/statistics.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

// Configuration for a structural index.
struct IndexConfig {
  Namespace ns;                                // the namespace this index serves
  CompatibilityKey default_compat;             // optional convenience default
  SegmentationConfig segmentation;             // deterministic segmentation
  CapacityPolicy capacity = CapacityPolicy::Defaults();
  EvictionConfig eviction;                     // deterministic eviction policy
};

// A structural prefix index: a sharded collection of compressed radix tries
// over token sequences, one trie per (namespace, compatibility identity).
//
// Prefix Fabric owns ONLY structure + prefix metadata here. It never owns the
// bytes referenced by StateReferences.
//
// The index is internally synchronised (readers on a compat tree run without
// taking a global lock; structural mutation is serialised per shard). For a
// higher-level API with policy, events, persistence and reference management,
// use PrefixStore.
class PrefixIndex {
 public:
  explicit PrefixIndex(const IndexConfig& config);
  ~PrefixIndex();
  PrefixIndex(const PrefixIndex&) = delete;
  PrefixIndex& operator=(const PrefixIndex&) = delete;

  const Namespace& NamespaceName() const;
  const CapacityPolicy& Capacity() const;
  const SegmentationConfig& Segmentation() const;

  // --- matching ---
  PrefixMatch LongestPrefixMatch(const CompatibilityKey& compat,
                                 const TokenSequence& request);
  PrefixMatch ExactMatch(const CompatibilityKey& compat,
                         const TokenSequence& request);
  std::vector<PrefixMatch> AllMatches(const CompatibilityKey& compat,
                                      const TokenSequence& request);
  std::uint64_t CommonPrefixLength(const CompatibilityKey& compat,
                                   const TokenSequence& a,
                                   const TokenSequence& b);

  // --- mutation ---
  Result<PrefixRecord> Insert(const CompatibilityKey& compat,
                              const TokenSequence& tokens,
                              const std::vector<StateReference>& refs,
                              const std::string& provenance);
  Result<void> Invalidate(const PrefixId& id, bool subtree);
  Result<void> InvalidateByCompat(const CompatibilityKey& compat);
  // Remove a prefix and, when cascade, its descendants. Returns removed count.
  Result<std::size_t> Remove(const PrefixId& id, bool cascade);

  // --- lookup by id ---
  Result<PrefixRecord> Get(const PrefixId& id);
  Result<PrefixPath> Path(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Ancestors(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Descendants(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Children(const PrefixId& id);
  Result<std::vector<PrefixRecord>> BranchPoints(const PrefixId& id);
  Result<std::vector<PrefixRecord>> CommonAncestors(const PrefixId& a,
                                                   const PrefixId& b);

  // --- composition / slicing (deterministic derived identities) ---
  Result<PrefixRecord> Slice(const PrefixId& id, std::uint64_t n);
  Result<PrefixRecord> Append(const PrefixId& id, const TokenSequence& suffix);
  Result<PrefixRecord> Compose(const CompatibilityKey& compat,
                               const std::vector<PrefixId>& pieces);
  Result<PrefixRecord> Fork(const PrefixId& id, const TokenSequence& suffix);

  // --- segmentation ---
  Result<std::vector<PrefixSegment>> Segments(const PrefixId& id);

  // --- state references ---
  Result<std::vector<StateReference>> AttachState(const PrefixId& id,
                                                  const StateReference& ref);
  Result<void> RetireState(const PrefixId& id, const StateReference& ref);
  Result<void> InvalidateStateByObject(const std::string& ns,
                                       const std::string& object_id);

  // --- capacity / eviction ---
  Result<std::vector<PrefixId>> Evict(const std::size_t target_count);
  std::uint64_t PrefixCount() const;
  std::uint64_t NodeCount() const;
  std::uint64_t SegmentCount() const;
  std::uint64_t ReferenceCount() const;
  std::uint64_t TokenFootprintBytes() const;
  std::uint64_t PrefixTokens() const;

  // --- statistics ---
  Result<PrefixStatistics> Statistics(const PrefixId& id);

  // --- introspection / snapshot ---
  Result<std::vector<PrefixRecord>> ListAll();
  PrefixSnapshot Snapshot(const CompatibilityKey* compat_filter);
  std::string DebugDump() const;

  // --- serialization support (deterministic) ---
  // Serialize the whole index (binary). Used by persistence.
  std::string SerializeBinary() const;
  // Deserialize; replaces current state. Returns the number of records loaded.
  Result<std::size_t> DeserializeBinary(std::string_view bytes,
                                        std::string* error_detail);

  // --- deterministic replay / scratch ---
  void Clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace prefix_fabric
