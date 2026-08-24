// Prefix Fabric - event type names.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/event.hpp"

namespace prefix_fabric {

const char* EventTypeName(EventType t) {
  switch (t) {
    case EventType::kPrefixAdded: return "prefix-added";
    case EventType::kPrefixMatched: return "prefix-matched";
    case EventType::kExactHit: return "exact-hit";
    case EventType::kLongestPrefixHit: return "longest-prefix-hit";
    case EventType::kMiss: return "miss";
    case EventType::kBranchCreated: return "branch-created";
    case EventType::kSegmentCreated: return "segment-created";
    case EventType::kStateReferenceAttached: return "state-ref-attached";
    case EventType::kStateReferenceRetired: return "state-ref-retired";
    case EventType::kInvalidation: return "invalidation";
    case EventType::kEviction: return "eviction";
    case EventType::kRecovery: return "recovery";
    case EventType::kOwnershipTransfer: return "ownership-transfer";
    case EventType::kStaleAuthorityRejection: return "stale-authority-rejection";
    case EventType::kPersistenceFailure: return "persistence-failure";
    case EventType::kCorruptionRejection: return "corruption-rejection";
    case EventType::kRemoteNodeJoin: return "remote-node-join";
    case EventType::kRemoteNodeLeave: return "remote-node-leave";
  }
  return "unknown";
}

}  // namespace prefix_fabric
