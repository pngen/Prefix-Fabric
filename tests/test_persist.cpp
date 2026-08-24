// Prefix Fabric - persistence / recovery / corruption tests.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "test_util.hpp"
#include "prefix_fabric/store.hpp"
#include "prefix_fabric/compatibility.hpp"

#include <cstdio>
#include <fstream>
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

int main() { return pftest::RunAll(); }
