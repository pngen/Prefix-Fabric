// Prefix Fabric - state reference implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/state_reference.hpp"

namespace prefix_fabric {

const char* StateReference::TypeName(Type t) {
  switch (t) {
    case Type::kOpaque: return "opaque";
    case Type::kKvFabric: return "kv-fabric";
    case Type::kTensorCache: return "tensor-cache";
    case Type::kContextFabric: return "context-fabric";
    case Type::kEngineNative: return "engine-native";
    case Type::kApplication: return "application";
  }
  return "opaque";
}

bool StateReference::operator==(const StateReference& o) const {
  return ns == o.ns && object_id == o.object_id && generation == o.generation &&
         type == o.type && compatibility == o.compatibility &&
         location_hint == o.location_hint && freshness_ts == o.freshness_ts &&
         integrity_token == o.integrity_token;
}

bool StateReference::IsWellFormed() const {
  if (ns.empty()) return false;
  if (object_id.empty()) return false;
  return true;
}

std::string StateReference::Key() const {
  return ns + "#" + object_id;
}

}  // namespace prefix_fabric
