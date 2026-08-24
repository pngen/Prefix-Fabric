// Prefix Fabric - thread-safe store facade.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "prefix_fabric/event.hpp"
#include "prefix_fabric/index.hpp"
#include "prefix_fabric/match.hpp"
#include "prefix_fabric/record.hpp"
#include "prefix_fabric/segment.hpp"
#include "prefix_fabric/snapshot.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/statistics.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

// The primary, genuinely-thread-safe entry point. PrefixStore owns a structural
// PrefixIndex and adds policy, event emission, runtime statistics, and atomic
// persistence. It operates in one Namespace.
//
// Prefix Fabric never owns the payload referenced by StateReferences; it only
// records and returns them.
class PrefixStore {
 public:
  // config: index configuration. persistence_dir_for_save is optional.
  struct Options {
    Namespace ns = "default";
    CompatibilityKey default_compat;
    SegmentationConfig segmentation;
    CapacityPolicy capacity = CapacityPolicy::Defaults();
    EvictionConfig eviction;
    std::uint64_t max_event_backlog = 0;   // 0 => use capacity.max_event_backlog
  };

  explicit PrefixStore(const Options& options);
  ~PrefixStore();
  PrefixStore(const PrefixStore&) = delete;
  PrefixStore& operator=(const PrefixStore&) = delete;

  const Namespace& NamespaceName() const;
  const CapacityPolicy& Capacity() const;
  const PrefixIndex& Index() const;
  PrefixIndex& Index();

  // --- add / match ---
  Result<PrefixRecord> Add(const CompatibilityKey& compat, const TokenSequence& tokens,
                           const std::vector<StateReference>& refs = {},
                           const std::string& provenance = {});
  PrefixMatch LongestPrefixMatch(const TokenSequence& request,
                                 const CompatibilityKey* compat = nullptr);
  PrefixMatch ExactMatch(const TokenSequence& request,
                         const CompatibilityKey* compat = nullptr);
  std::vector<PrefixMatch> AllMatches(const TokenSequence& request,
                                      const CompatibilityKey* compat = nullptr);
  // Advisory reuse plan (does not execute inference).
  PrefixReusePlan BuildReusePlan(const TokenSequence& request,
                                 const CompatibilityKey* compat = nullptr,
                                 std::uint64_t* common_prefix_len = nullptr);
  std::uint64_t CommonPrefixLength(const TokenSequence& a, const TokenSequence& b,
                                   const CompatibilityKey* compat = nullptr);

  // --- lookup / lineage ---
  Result<PrefixRecord> Get(const PrefixId& id);
  Result<PrefixPath> Path(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Ancestors(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Descendants(const PrefixId& id);
  Result<std::vector<PrefixRecord>> Children(const PrefixId& id);
  Result<std::vector<PrefixRecord>> BranchPoints(const PrefixId& id);
  Result<std::vector<PrefixRecord>> CommonAncestors(const PrefixId& a, const PrefixId& b);
  Result<std::vector<PrefixSegment>> Segments(const PrefixId& id);
  Result<PrefixStatistics> Statistics(const PrefixId& id);

  // --- composition ---
  Result<PrefixRecord> Slice(const PrefixId& id, std::uint64_t n);
  Result<PrefixRecord> Append(const PrefixId& id, const TokenSequence& suffix);
  Result<PrefixRecord> Compose(const CompatibilityKey& compat,
                               const std::vector<PrefixId>& pieces);
  Result<PrefixRecord> Fork(const PrefixId& id, const TokenSequence& suffix);

  // --- state references ---
  Result<std::vector<StateReference>> AttachState(const PrefixId& id,
                                                  const StateReference& ref);
  Result<void> RetireState(const PrefixId& id, const StateReference& ref);
  Result<void> InvalidateStateByObject(const std::string& ns,
                                       const std::string& object_id);

  // --- invalidation / removal ---
  Result<void> Invalidate(const PrefixId& id, bool subtree = false);
  Result<void> InvalidateByCompat(const CompatibilityKey& compat);
  Result<std::size_t> Remove(const PrefixId& id, bool cascade = false);
  Result<std::vector<PrefixId>> Evict(std::size_t target_count);

  // --- introspection ---
  std::uint64_t PrefixCount() const;
  std::uint64_t NodeCount() const;
  std::uint64_t SegmentCount() const;
  std::uint64_t ReferenceCount() const;
  std::uint64_t TokenFootprintBytes() const;
  std::uint64_t PrefixTokens() const;
  RuntimeStatistics RuntimeStats() const;
  Result<std::vector<PrefixRecord>> ListAll();
  PrefixSnapshot Snapshot(const CompatibilityKey* compat_filter = nullptr);
  std::string DebugDump() const;

  // --- persistence (atomic) ---
  Result<void> SaveToFile(const std::string& path);
  Result<std::size_t> LoadFromFile(const std::string& path);

  // --- events and subscriptions (callbacks run outside internal locks) ---
  struct SubscriptionHandle { std::uint64_t id; };
  Result<SubscriptionHandle> Subscribe(std::function<void(const PrefixEvent&)> cb,
                                       std::vector<EventType> filter = {});
  void Unsubscribe(std::uint64_t id);
  void DrainEvents();   // process queued events synchronously

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace prefix_fabric
