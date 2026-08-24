// Prefix Fabric - opaque external reusable-state references.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/error.hpp"
#include "prefix_fabric/id.hpp"

namespace prefix_fabric {

// An opaque reference to reusable state owned by ANOTHER subsystem (KV Fabric,
// Tensor Cache, Context Fabric, an engine-native cache, or application-defined
// state). Prefix Fabric records, attaches, retires, invalidates, and returns
// these references. It NEVER allocates, moves, persists, reclaims, restores, or
// interprets the payload the reference points to.
//
// The reference is backend-neutral: the target object id is an opaque string
// interpreted only by the owning subsystem. Prefix Fabric carries enough
// metadata to reject stale references (namespace, object id, generation,
// freshness, integrity token) without knowing what the bytes are.
class StateReference {
 public:
  // Category of referenced state. Supplied by the owning subsystem.
  enum class Type : int {
    kOpaque = 0,
    kKvFabric,      // KV Fabric object
    kTensorCache,   // Tensor Cache object
    kContextFabric, // Context Fabric object
    kEngineNative,  // serving-engine native cache object
    kApplication,   // application-defined reusable state
  };
  static const char* TypeName(Type t);

  std::string ns;             // external namespace
  std::string object_id;      // opaque external object id
  Generation generation = 0;  // external generation (for staleness)
  Type type = Type::kOpaque;
  CompatibilityKey compatibility;  // compatibility that must hold to use this ref
  std::string location_hint;  // optional hint; never used to move bytes
  std::optional<std::uint64_t> freshness_ts;  // optional freshness timestamp
  std::string integrity_token;  // optional opaque integrity token

  bool operator==(const StateReference& o) const;
  bool operator!=(const StateReference& o) const { return !(*this == o); }

  bool IsWellFormed() const;
  // Identity string used for invalidation / attach reconciliations.
  std::string Key() const;

};

}  // namespace prefix_fabric
