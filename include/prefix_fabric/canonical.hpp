// Prefix Fabric - canonical content hashing.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/hash.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

// Canonical byte serialization used for content hashing. The layout is
// unambiguous (length-prefixed), deterministic, and never truncates equality to
// a weak checksum: SHA-256 over the full canonical bytes.
inline void AppendU64(std::string& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
  }
}
inline void AppendStr(std::string& out, const std::string& s) {
  AppendU64(out, static_cast<std::uint64_t>(s.size()));
  out.append(s);
}

// Canonical bytes of a namespace + compat + token sequence.
std::string CanonicalPrefixBytes(const Namespace& ns,
                                 const CompatibilityKey& compat,
                                 const TokenSequence& tokens);

// Canonical bytes of a segment slice [offset, offset+count).
std::string CanonicalSegmentBytes(const Namespace& ns,
                                  const CompatibilityKey& compat,
                                  const TokenSequence& tokens,
                                  std::uint64_t offset, std::uint64_t count);

// Full 256-bit content hash of a logical prefix. Identical logical inputs
// (ns + compat + tokens) produce identical hashes regardless of other state.
inline ContentHash ComputePrefixContentHash(const Namespace& ns,
                                            const CompatibilityKey& compat,
                                            const TokenSequence& tokens) {
  std::string b = CanonicalPrefixBytes(ns, compat, tokens);
  return Sha256::HashBytes(b.data(), b.size());
}

// Full 256-bit content hash of a segment slice.
inline ContentHash ComputeSegmentContentHash(const Namespace& ns,
                                             const CompatibilityKey& compat,
                                             const TokenSequence& tokens,
                                             std::uint64_t offset,
                                             std::uint64_t count) {
  std::string b = CanonicalSegmentBytes(ns, compat, tokens, offset, count);
  return Sha256::HashBytes(b.data(), b.size());
}

}  // namespace prefix_fabric
