// Prefix Fabric - stable identities.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "prefix_fabric/error.hpp"
#include "prefix_fabric/hash.hpp"

namespace prefix_fabric {

// A stable, content-derived 128-bit prefix identity. Two identical logical
// prefixes under the same namespace + compatibility identity produce the same
// PrefixId. It is never a process-local pointer or a runtime handle.
class PrefixId {
 public:
  PrefixId() = default;
  PrefixId(std::uint64_t hi, std::uint64_t lo) : hi_(hi), lo_(lo) {}

  // Derive a PrefixId from the first 128 bits of a 256-bit content hash.
  static PrefixId FromContentHash(const ContentHash& h) {
    std::uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; ++i) hi = (hi << 8) | h[static_cast<size_t>(i)];
    for (int i = 8; i < 16; ++i) lo = (lo << 8) | h[static_cast<size_t>(i)];
    return PrefixId(hi, lo);
  }

  std::uint64_t high() const { return hi_; }
  std::uint64_t low() const { return lo_; }

  std::string ToString() const {
    std::uint8_t bytes[16];
    for (int i = 0; i < 8; ++i) {
      bytes[static_cast<size_t>(i)] = static_cast<std::uint8_t>((hi_ >> (8 * (7 - i))) & 0xffu);
      bytes[static_cast<size_t>(8 + i)] = static_cast<std::uint8_t>((lo_ >> (8 * (7 - i))) & 0xffu);
    }
    return ToHex(bytes, 16);
  }

  // Parse 32 hex chars (128 bits). Rejects malformed or non-canonical input.
  static Result<PrefixId> Parse(std::string_view s) {
    if (s.size() != 32) {
      return Result<PrefixId>::Err(Error{ErrorCode::kInvalidArgument,
                                         "PrefixId must be 32 hex chars"});
    }
    return PrefixIdFromCleanParse(s);
  }

  bool operator==(const PrefixId& o) const { return hi_ == o.hi_ && lo_ == o.lo_; }
  bool operator!=(const PrefixId& o) const { return !(*this == o); }
  bool operator<(const PrefixId& o) const {
    if (hi_ != o.hi_) return hi_ < o.hi_;
    return lo_ < o.lo_;
  }
  void Reset() { hi_ = 0; lo_ = 0; }
  bool IsZero() const { return hi_ == 0 && lo_ == 0; }

 private:
  static int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  }
  static Result<PrefixId> PrefixIdFromCleanParse(std::string_view s) {
    std::uint64_t hi = 0, lo = 0;
    int nibble_count = 0;
    std::uint64_t* cur = &hi;
    for (char c : s) {
      int n = HexNibble(c);
      if (n < 0) {
        return Result<PrefixId>::Err(Error{ErrorCode::kInvalidArgument,
                                           "non-hex character in PrefixId"});
      }
      *cur = (*cur << 4) | static_cast<std::uint64_t>(n);
      if (++nibble_count == 16) cur = &lo;
    }
    return Result<PrefixId>::Ok(PrefixId(hi, lo));
  }
  std::uint64_t hi_ = 0;
  std::uint64_t lo_ = 0;
};

// A stable segment identity (a logical component of a composed prefix).
using SegmentId = PrefixId;

// A mutation/validity generation counter. Garbage-collected stale records are
// distinguished from current records by generation, never silently overwritten.
using Generation = std::uint64_t;

// A partition epoch/token for authoritative fencing.
using Epoch = std::uint64_t;

}  // namespace prefix_fabric

namespace std {
template <>
struct hash<prefix_fabric::PrefixId> {
  size_t operator()(const prefix_fabric::PrefixId& id) const noexcept {
    size_t h = std::hash<std::uint64_t>{}(id.high());
    h ^= std::hash<std::uint64_t>{}(id.low()) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};
}  // namespace std
