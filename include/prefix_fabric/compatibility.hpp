// Prefix Fabric - explicit compatibility keys.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "prefix_fabric/error.hpp"
#include "prefix_fabric/hash.hpp"

namespace prefix_fabric {

// Compatibility identity identifies every axis that determines whether the
// tokens in a prefix are *semantically the same* across two contexts. Two
// CompatibilityKey objects are equal iff every documented field is equal.
// Prefix Fabric never infers compatibility; it only compares explicit keys.
class CompatibilityKey {
 public:
  // --- configurable identities (all optional defaults are the empty string) ---
  std::string model;                 // model / architecture identity
  std::string tokenizer;             // tokenizer identity
  std::string tokenization_version;  // tokenization version
  std::string vocabulary;            // vocabulary identity
  std::string special_token_policy;  // special-token handling policy
  std::string prompt_template;       // prompt / chat template identity
  std::string position_encoding;     // positional encoding regime
  std::string kv_layout;             // KV/layout compatibility (for external KV refs)
  std::string runtime_format;        // runtime format/version

  // Opaque extra axes, sorted for canonical ordering.
  std::vector<std::pair<std::string, std::string>> extra;

  bool operator==(const CompatibilityKey& o) const;
  bool operator!=(const CompatibilityKey& o) const { return !(*this == o); }
  bool operator<(const CompatibilityKey& o) const;

  // Canonical, deterministic textual identity.
  std::string Identity() const;
  // Canonical 256-bit content hash of the identity.
  ContentHash IdentityHash() const;
  // A compact hex identity (useful for keys / observability).
  std::string IdentityHex() const { return ToHex(IdentityHash()); }

  bool IsEmpty() const {
    return model.empty() && tokenizer.empty() && tokenization_version.empty() &&
           vocabulary.empty() && special_token_policy.empty() &&
           prompt_template.empty() && position_encoding.empty() &&
           kv_layout.empty() && runtime_format.empty() && extra.empty();
  }

  static CompatibilityKey Empty() { return CompatibilityKey{}; }

  // If the key omits the axes that would permit semantically identical token
  // sequences to be conflated across incompatible boundaries, warn loudly. This
  // only hints at risk; it never changes matching behavior.
  bool IsOverPermissive() const {
    return model.empty() || tokenizer.empty();
  }
};

}  // namespace prefix_fabric
