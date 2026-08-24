// Prefix Fabric - SHA-256 implementation (FIPS 180-4).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/hash.hpp"

#include <cstdint>
#include <cstring>

namespace prefix_fabric {

namespace {
constexpr std::uint32_t kK[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
constexpr std::uint32_t kH0[8] = {
  0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
};
inline std::uint32_t Ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

void Sha256::Init() {
  for (int i = 0; i < 8; ++i) state_[i] = kH0[i];
  bitlen_ = 0;
  buf_len_ = 0;
}

void Sha256::Transform(const std::uint8_t* block) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
           (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    std::uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    std::uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    std::uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
    std::uint32_t ch = (e & f) ^ (~e & g);
    std::uint32_t t1 = h + S1 + ch + kK[i] + w[i];
    std::uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
    std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    std::uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

Sha256& Sha256::Update(const void* data, std::size_t len) {
  const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
  std::size_t i = 0;
  while (i < len) {
    std::size_t need = 64 - buf_len_;
    std::size_t take = std::min(need, len - i);
    std::memcpy(buffer_ + buf_len_, p + i, take);
    buf_len_ += take;
    i += take;
    if (buf_len_ == 64) {
      Transform(buffer_);
      bitlen_ += 512;
      buf_len_ = 0;
    }
  }
  return *this;
}

std::array<std::uint8_t, 32> Sha256::Final() {
  bitlen_ += static_cast<std::uint64_t>(buf_len_) * 8;
  buffer_[buf_len_++] = 0x80;
  if (buf_len_ > 56) {
    while (buf_len_ < 64) buffer_[buf_len_++] = 0;
    Transform(buffer_);
    buf_len_ = 0;
  }
  while (buf_len_ < 56) buffer_[buf_len_++] = 0;
  for (int i = 0; i < 8; ++i) buffer_[63 - i] = static_cast<std::uint8_t>((bitlen_ >> (8 * i)) & 0xffu);
  Transform(buffer_);
  std::array<std::uint8_t, 32> out{};
  for (int i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xffu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xffu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xffu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xffu);
  }
  return out;
}

std::string ToHex(const std::uint8_t* data, std::size_t len) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    s.push_back(hex[(data[i] >> 4) & 0xf]);
    s.push_back(hex[data[i] & 0xf]);
  }
  return s;
}

std::string ToHex(const std::array<std::uint8_t, 32>& digest) {
  return ToHex(digest.data(), digest.size());
}

}  // namespace prefix_fabric
