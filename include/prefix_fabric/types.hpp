// Prefix Fabric - core token types and a strict validation mode.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prefix_fabric/error.hpp"

namespace prefix_fabric {

// A token is an opaque engine-neutral integer id (from a tokenizer vocabulary).
// Token-content meaning is scoped by the CompatibilityKey (tokenizer + vocab).
using TokenId = std::uint32_t;

// A token sequence is the canonical logical content of a prefix. Tokens are
// never reinterpreted by Prefix Fabric; they are compared for equality only.
using TokenSequence = std::vector<TokenId>;

// Namespace scoping for prefix identity (mirrors namespacing of registries).
using Namespace = std::string;

// Strict parsing/validation mode for serialization inputs.
struct InputValidation {
  bool strict = true;
};

// Bounded token-representation options. Prefix Fabric stores token ids but may
// expose an optional human-readable string for diagnostics. The string is never
// used for identity; identity is always derived from the token-id sequence.
struct TokenText {
  std::string text;
};

// Common comparison used everywhere tokens are compared.
inline bool TokensEqual(const TokenSequence& a, const TokenSequence& b) {
  return a == b;
}

// Compute the "byte length" a canonical token array would occupy (for cost
// accounting). Each token contributes 4 bytes.
inline std::uint64_t TokenFootprint(const TokenSequence& t) {
  return static_cast<std::uint64_t>(t.size()) * 4u;
}

// Validate that a token count is representable and non-overflowing at the
// request boundary. Returns an error rather than coercing invalid input.
inline Result<void> ValidateTokenCount(std::uint64_t count) {
  if (count > (1ull << 31)) {
    return Result<void>::Err(Error{ErrorCode::kInvalidArgument,
                                   "token count exceeds supported bound"});
  }
  return Result<void>::Ok();
}

}  // namespace prefix_fabric
