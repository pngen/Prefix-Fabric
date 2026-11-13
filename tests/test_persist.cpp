// Prefix Fabric - persistence / recovery / corruption tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

#include "prefix_fabric/hash.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>

using namespace prefix_fabric;

static CompatibilityKey C() { CompatibilityKey c; c.model = "m"; c.tokenizer = "t"; c.vocabulary = "v"; c.tokenization_version = "1"; c.kv_layout = "gqa"; return c; }

static std::string Slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void Write(const std::string& p, const std::string& d) { std::ofstream f(p, std::ios::binary); f.write(d.data(), static_cast<std::streamsize>(d.size())); }

PF_TEST(Persistence_RoundTripExact) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kFixed; o.segmentation.fixed_size = 8;
  PrefixStore store(o);
  TokenSequence a = {1,2,3,4,5,6,7,8,9,10};
  TokenSequence b = {1,2,3,4,11,12};
  store.Add(ca, a); store.Add(ca, b);
  std::string path = "pers_round.bin";
  CHECK(store.SaveToFile(path).is_ok());
  PrefixStore store2(o);
  auto load = store2.LoadFromFile(path);
  CHECK(load.is_ok());
  // Snapshot counts equal live state.
  auto s1 = store.Snapshot(); auto s2 = store2.Snapshot();
  CHECK_EQ(s1.records.size(), s2.records.size());
  CHECK_EQ(store2.PrefixCount(), store.PrefixCount());
  // Exact match preserved.
  CHECK(store2.ExactMatch(a, &ca).is_matched());
  CHECK(store2.ExactMatch(b, &ca).is_matched());
  std::remove(path.c_str());
}

PF_TEST(Persistence_CorruptionRejected) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore store(o);
  store.Add(ca, {1,2,3,4,5}, {}, "pers");
  std::string path = "pers_corrupt.bin";
  CHECK(store.SaveToFile(path).is_ok());
  std::string bytes = Slurp(path);
  CHECK(bytes.size() > 40);
  // Flip a byte in the body (not the magic/checksum region boundaries).
  std::string corrupted = bytes;
  corrupted[corrupted.size() / 2] ^= 0xFF;
  std::string cpath = "pers_corrupt2.bin";
  Write(cpath, corrupted);
  PrefixStore store2(o);
  auto res = store2.LoadFromFile(cpath);
  CHECK(res.is_err());   // corruption must be rejected, not silently accepted.
  auto res2 = store2.LoadFromFile("pers_corrupt2.bin"); (void)res2;
  std::remove(path.c_str()); std::remove(cpath.c_str());
}

PF_TEST(Persistence_TruncationRejected) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore store(o);
  store.Add(ca, {1,2,3,4,5}, {}, "pers");
  std::string path = "pers_trunc.bin";
  CHECK(store.SaveToFile(path).is_ok());
  std::string bytes = Slurp(path);
  Write(path, bytes.substr(0, bytes.size() / 2));
  PrefixStore store2(o);
  auto res = store2.LoadFromFile(path);
  CHECK(res.is_err());
  std::remove(path.c_str());
}

PF_TEST(Persistence_RecoveryPreservesInvalidation) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore store(o);
  auto r = store.Add(ca, {1,2,3}, {}, "pers"); CHECK(r.is_ok());
  CHECK(store.Invalidate(r.value().id).is_ok());
  std::string path = "pers_inv.bin";
  CHECK(store.SaveToFile(path).is_ok());
  PrefixStore store2(o);
  CHECK(store2.LoadFromFile(path).is_ok());
  // The invalidated prefix must not be reported as reusable after recovery.
  PrefixMatch m = store2.LongestPrefixMatch({1,2,3,9}, &ca);
  CHECK(!m.is_matched());
  std::remove(path.c_str());
}

PF_TEST(Persistence_BadMagicRejected) {
  Write("pers_magic.bin", "NOTAPREFIXFABRICFILE!");
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = C(); o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore store(o);
  auto res = store.LoadFromFile("pers_magic.bin");
  CHECK(res.is_err());
  std::remove("pers_magic.bin");
}

// Recompute the trailing SHA-256 after editing the body, so the blob is intact
// as far as its checksum goes and only its content is wrong.
static std::string Resealed(std::string bytes) {
  const std::size_t body = bytes.size() - 32;
  const auto sum = Sha256::HashBytes(bytes.data(), body);
  for (std::size_t i = 0; i < 32; ++i) bytes[body + i] = static_cast<char>(sum[i]);
  return bytes;
}
static std::string U32Bytes(std::initializer_list<std::uint32_t> values) {
  std::string s;
  for (std::uint32_t v : values) for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xffu));
  return s;
}

PF_TEST(Persistence_RefusedLoadKeepsLiveState) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore source(o);
  source.Add(ca, {1,2,3,4,5}, {}, "pers");
  std::string path = "pers_keep.bin";
  CHECK(source.SaveToFile(path).is_ok());
  std::string bytes = Slurp(path);
  CHECK(bytes.size() > 44);
  if (bytes.size() <= 44) return;
  // A blob in a later payload format: intact, correctly checksummed, not readable here.
  bytes[8] = 2;
  Write(path, Resealed(bytes));
  PrefixStore live(o);
  CHECK(live.Add(ca, {7,8,9}, {}, "live").is_ok());
  CHECK(live.LoadFromFile(path).is_err());
  // A load that failed takes nothing away from what was live.
  CHECK_EQ(live.PrefixCount(), 1u);
  CHECK(live.ExactMatch({7,8,9}, &ca).is_matched());
  std::remove(path.c_str());
}

PF_TEST(Persistence_ContentMustMatchIdentity) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore source(o);
  source.Add(ca, {1000001, 1000002, 1000003}, {}, "pers");
  std::string path = "pers_ident.bin";
  CHECK(source.SaveToFile(path).is_ok());
  const std::string bytes = Slurp(path);
  const std::size_t at = bytes.find(U32Bytes({1000001, 1000002, 1000003}));
  CHECK(at != std::string::npos);
  if (at == std::string::npos) return;
  {
    // Different tokens under the id and hash of the original ones.
    std::string edited = bytes; edited[at] ^= 0x01;
    Write(path, Resealed(edited));
    PrefixStore s(o);
    CHECK(s.LoadFromFile(path).is_err());
    CHECK_EQ(s.PrefixCount(), 0u);
  }
  {
    // A token count the stored tokens do not add up to (edge, id, hash, then the count).
    std::string edited = bytes; edited[at + 12 + 16 + 32] = 9;
    Write(path, Resealed(edited));
    PrefixStore s(o);
    CHECK(s.LoadFromFile(path).is_err());
    CHECK_EQ(s.PrefixCount(), 0u);
  }
  {
    // A copy saved for another namespace is a different set of prefixes.
    Write(path, bytes);
    PrefixStore::Options other = o; other.ns = "elsewhere";
    PrefixStore s(other);
    CHECK(s.LoadFromFile(path).is_err());
    CHECK_EQ(s.PrefixCount(), 0u);
  }
  std::remove(path.c_str());
}

PF_TEST(Persistence_BoundsApplyToFiles) {
  CompatibilityKey ca = C();
  PrefixStore::Options o; o.ns = "pers"; o.default_compat = ca; o.segmentation.policy = SegmentationPolicy::kSingle;
  PrefixStore source(o);
  source.Add(ca, {1,2,3}, {}, "pers"); source.Add(ca, {4,5,6}, {}, "pers"); source.Add(ca, {7,8,9}, {}, "pers");
  std::string path = "pers_bounds.bin";
  CHECK(source.SaveToFile(path).is_ok());
  {
    // More prefixes than this store may hold.
    PrefixStore::Options small = o; small.capacity.max_prefixes = 2;
    PrefixStore s(small);
    auto res = s.LoadFromFile(path);
    CHECK(res.is_err());
    if (res.is_err()) CHECK(res.error().code() == ErrorCode::kCapacityExceeded);
    CHECK_EQ(s.PrefixCount(), 0u);
  }
  {
    // A file larger than the serialized-object bound is not read, and an index
    // that serializes to more than the bound is not written.
    PrefixStore::Options tiny = o; tiny.capacity.max_serialized_object_bytes = 64;
    PrefixStore s(tiny);
    auto res = s.LoadFromFile(path);
    CHECK(res.is_err());
    if (res.is_err()) CHECK(res.error().code() == ErrorCode::kCapacityExceeded);
    CHECK(s.Add(ca, {1,2,3}, {}, "pers").is_ok());
    auto sv = s.SaveToFile("pers_bounds_out.bin");
    CHECK(sv.is_err());
    if (sv.is_err()) CHECK(sv.error().code() == ErrorCode::kCapacityExceeded);
    std::remove("pers_bounds_out.bin");
  }
  std::remove(path.c_str());
}

int main() { return pftest::RunAll(); }
