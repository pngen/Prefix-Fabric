// Prefix Fabric - canonical hashing (SHA-256) and content hashes.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prefix_fabric {

// Compact, self-contained SHA-256 (FIPS 180-4). No external dependency.
class Sha256 {
 public:
  Sha256() { Init(); }
  void Init();
  Sha256& Update(const void* data, std::size_t len);
  Sha256& Update(std::string_view sv) {
    return Update(sv.data(), sv.size());
  }
  // Finalize and return the 32-byte digest. Finalize may be called once.
  std::array<std::uint8_t, 32> Final();
  static std::array<std::uint8_t, 32> HashString(std::string_view s) {
    Sha256 h; h.Update(s); return h.Final();
  }
  static std::array<std::uint8_t, 32> HashBytes(const void* d, std::size_t n) {
    Sha256 h; h.Update(d, n); return h.Final();
  }
 private:
  void Transform(const std::uint8_t* block);
  std::uint32_t state_[8];
  std::uint64_t bitlen_;
  std::uint8_t buffer_[64];
  std::size_t buf_len_;
};

// Hex encoding helpers for diagnostics / stable textual keys.
std::string ToHex(const std::uint8_t* data, std::size_t len);
std::string ToHex(const std::array<std::uint8_t, 32>& digest);

// The full 256-bit canonical content hash of a prefix / segment.
using ContentHash = std::array<std::uint8_t, 32>;

// Convenience builders.
inline std::array<std::uint8_t, 32> HashString(std::string_view s) {
  return Sha256::HashString(s);
}
inline std::array<std::uint8_t, 32> HashBytes(const void* d, std::size_t n) {
  return Sha256::HashBytes(d, n);
}

}  // namespace prefix_fabric
