// Prefix Fabric - structured events and bounded subscriptions.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "prefix_fabric/id.hpp"

namespace prefix_fabric {

enum class EventType : int {
  kPrefixAdded = 0,
  kPrefixMatched,
  kExactHit,
  kLongestPrefixHit,
  kMiss,
  kBranchCreated,
  kSegmentCreated,
  kStateReferenceAttached,
  kStateReferenceRetired,
  kInvalidation,
  kEviction,
  kRecovery,
  kOwnershipTransfer,
  kStaleAuthorityRejection,
  kPersistenceFailure,
  kCorruptionRejection,
  kRemoteNodeJoin,
  kRemoteNodeLeave,
};

const char* EventTypeName(EventType t);

// A structured event. Subscribers receive a copy; callbacks never run while
// internal locks are held.
struct PrefixEvent {
  EventType type = EventType::kPrefixAdded;
  std::uint64_t sequence = 0;   // monotonically increasing
  std::int64_t timestamp_ns = 0;
  PrefixId id;
  std::string detail;
};

// A bounded subscription. The callback is invoked asynchronously outside the
// runtime's internal locks so reentrant subscribers cannot deadlock.
struct PrefixSubscription {
  std::uint64_t id = 0;
  std::function<void(const PrefixEvent&)> callback;
  std::vector<EventType> filter;  // empty => all
};

}  // namespace prefix_fabric
