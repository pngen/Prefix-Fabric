// Prefix Fabric - canonical content bytes.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/canonical.hpp"

namespace prefix_fabric {

std::string CanonicalPrefixBytes(const Namespace& ns,
                                 const CompatibilityKey& compat,
                                 const TokenSequence& tokens) {
  std::string b;
  b.reserve(ns.size() + 16 + tokens.size() * 4 + 64);
  AppendStr(b, ns);
  std::string cid = compat.Identity();
  AppendStr(b, cid);
  AppendU64(b, static_cast<std::uint64_t>(tokens.size()));
  for (TokenId t : tokens) {
    AppendU64(b, t);  // token ids are stored as 64-bit in canonical bytes
  }
  return b;
}

std::string CanonicalSegmentBytes(const Namespace& ns,
                                  const CompatibilityKey& compat,
                                  const TokenSequence& tokens,
                                  std::uint64_t offset, std::uint64_t count) {
  std::string b;
  AppendStr(b, ns);
  std::string cid = compat.Identity();
  AppendStr(b, cid);
  AppendU64(b, offset);
  AppendU64(b, count);
  AppendU64(b, static_cast<std::uint64_t>(tokens.size()));
  if (offset > tokens.size()) return b;  // invalid; hash will differ
  std::uint64_t end = offset + count;
  if (end > tokens.size()) end = tokens.size();
  for (std::uint64_t i = offset; i < end; ++i) {
    AppendU64(b, tokens[static_cast<std::size_t>(i)]);
  }
  return b;
}

}  // namespace prefix_fabric
