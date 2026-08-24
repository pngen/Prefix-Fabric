// Prefix Fabric - CLI.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "prefix_fabric/store.hpp"
#include "prefix_fabric/coordinator.hpp"
#include "prefix_fabric/node.hpp"
#include "prefix_fabric/version.hpp"

using namespace prefix_fabric;

static void PrintUsage() {
  std::printf("prefix-fabric <command> [options]\n");
  std::printf("Commands: info add match longest-match get list ancestors descendants branches segments\n");
  std::printf("          attach-state retire-state invalidate stats explain snapshot save restore\n");
  std::printf("          coordinator node remote-publish remote-match benchmark\n");
}

static bool Has(const std::vector<std::string>& a, const std::string& name) {
  for (const auto& x : a) if (x == name) return true;
  return false;
}

static CompatibilityKey CompatFromArgs(const std::vector<std::string>& a) {
  CompatibilityKey c;
  c.model = "cli-model"; c.tokenizer = "cli-tok"; c.vocabulary = "cli-vocab";
  c.tokenization_version = "1"; c.special_token_policy = "default";
  c.prompt_template = "none"; c.position_encoding = "rope"; c.kv_layout = "gqa"; c.runtime_format = "pf-cpp";
  for (std::size_t i = 0; i + 1 < a.size(); ++i) {
    if (a[i] == "--model") c.model = a[i+1];
    else if (a[i] == "--tokenizer") c.tokenizer = a[i+1];
    else if (a[i] == "--vocab") c.vocabulary = a[i+1];
    else if (a[i] == "--kv-layout") c.kv_layout = a[i+1];
  }
  return c;
}

static TokenSequence ParseTokens(const std::string& s) {
  TokenSequence t;
  std::string cur;
  for (char ch : s) {
    if (ch == ' ' || ch == ',') { if (!cur.empty()) { t.push_back(static_cast<TokenId>(std::stoul(cur))); cur.clear(); } }
    else cur.push_back(ch);
  }
  if (!cur.empty()) t.push_back(static_cast<TokenId>(std::stoul(cur)));
  return t;
}

static PrefixStore::Options StoreOpts(const std::vector<std::string>& a) {
  PrefixStore::Options o;
  o.ns = "cli";
  o.default_compat = CompatFromArgs(a);
  o.segmentation.policy = SegmentationPolicy::kFixed;
  o.segmentation.fixed_size = 16;
  o.capacity = CapacityPolicy::Defaults();
  o.eviction.auto_evict = false;
  return o;
}

static std::string TokensStr(const TokenSequence& t) {
  std::string s;
  for (std::size_t i = 0; i < t.size(); ++i) { if (i) s += ' '; s += std::to_string(t[i]); }
  return s;
}

int main(int argc, char** argv) {
  std::vector<std::string> a(argv + 1, argv + argc);
  if (a.empty()) { PrintUsage(); return 1; }
  std::string cmd = a[0];
  std::string state;
  for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--state") state = a[i+1];

  if (cmd == "info") { std::printf("Prefix Fabric %s\n%s\n", kVersion, kCopyright); return 0; }

  if (cmd == "coordinator") {
    std::uint16_t port = 0; std::string persist;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) { if (a[i] == "--port") port = static_cast<std::uint16_t>(std::stoi(a[i+1])); else if (a[i] == "--persist") persist = a[i+1]; }
    PrefixCoordinator::Options o;
    o.ns = "cli"; o.default_compat = CompatFromArgs(a); o.persistence_path = persist; o.listen_port = port;
    PrefixCoordinator coord(o);
    auto p = coord.Start();
    if (p.is_err()) { std::printf("coordinator start failed: %s\n", p.error().ToString().c_str()); return 1; }
    std::printf("PORT %u\n", static_cast<unsigned>(p.value()));
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
  }

  if (cmd == "node" || cmd == "remote-publish" || cmd == "remote-match") {
    std::string host = "127.0.0.1"; std::uint16_t port = 0;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) { if (a[i] == "--coordinator" || a[i] == "--host") host = a[i+1]; else if (a[i] == "--port") port = static_cast<std::uint16_t>(std::stoi(a[i+1])); }
    if (port == 0) { std::printf("missing --port\n"); return 1; }
    PrefixNode::Options o;
    o.ns = "cli"; o.default_compat = CompatFromArgs(a); o.coordinator_host = host; o.coordinator_port = port;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--boot-id") o.boot_id = a[i+1];
    PrefixNode node(o);
    auto st = node.Start();
    if (st.is_err()) { std::printf("node start failed: %s\n", st.error().ToString().c_str()); return 2; }
    bool publish = (cmd == "remote-publish") || (cmd == "node" && Has(a, "--publish"));
    std::string toks;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--tokens") toks = a[i+1];
    if (publish) {
      if (toks.empty()) { std::printf("missing --tokens\n"); return 1; }
      auto r = node.Publish(o.default_compat, ParseTokens(toks), {}, "cli-publish");
      if (r.is_err()) { std::printf("publish failed: %s\n", r.error().ToString().c_str()); return 3; }
      std::printf("PUBLISHED %s tokens=%llu\n", r.value().id.ToString().c_str(), (unsigned long long)r.value().token_count);
      return 0;
    }
    auto m = node.RemoteLongestMatch(ParseTokens(toks), o.default_compat);
    if (m.is_err()) { std::printf("match failed: %s\n", m.error().ToString().c_str()); return 4; }
    if (m.value().is_matched()) std::printf("MATCH %s matched_tokens=%llu\n", m.value().id().ToString().c_str(), (unsigned long long)m.value().matched_token_count());
    else std::printf("NO_MATCH\n");
    return 0;
  }

  PrefixStore store(StoreOpts(a));
  if (!state.empty()) { auto lr = store.LoadFromFile(state); if (lr.is_err()) std::printf("(restore warning: %s)\n", lr.error().ToString().c_str()); }
  auto save = [&]() { if (!state.empty()) store.SaveToFile(state); };
  auto compat = CompatFromArgs(a);

  if (cmd == "add") {
    std::string toks; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--tokens") toks = a[i+1];
    if (toks.empty()) { std::printf("missing --tokens\n"); return 1; }
    auto r = store.Add(compat, ParseTokens(toks), {}, "cli");
    if (r.is_err()) { std::printf("add failed: %s\n", r.error().ToString().c_str()); return 3; }
    std::printf("ADDED %s tokens=%llu\n", r.value().id.ToString().c_str(), (unsigned long long)r.value().token_count);
    save(); return 0;
  }
  if (cmd == "match" || cmd == "longest-match") {
    std::string toks; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--tokens") toks = a[i+1];
    auto m = store.LongestPrefixMatch(ParseTokens(toks), &compat);
    if (m.is_matched()) std::printf("MATCH %s matched_tokens=%llu exact=%d\n", m.id().ToString().c_str(), (unsigned long long)m.matched_token_count(), m.exact()?1:0);
    else std::printf("NO_MATCH\n");
    return 0;
  }
  if (cmd == "get") {
    std::string idh; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--id") idh = a[i+1];
    auto id = PrefixId::Parse(idh);
    if (id.is_err()) { std::printf("bad id\n"); return 1; }
    auto r = store.Get(id.value());
    if (r.is_err()) { std::printf("not found\n"); return 5; }
    std::printf("ID %s gen=%llu tokens=%llu [%s]\n", r.value().id.ToString().c_str(), (unsigned long long)r.value().generation, (unsigned long long)r.value().token_count, TokensStr(r.value().tokens).c_str());
    return 0;
  }
  if (cmd == "list") {
    auto r = store.ListAll(); if (r.is_err()) { std::printf("list failed\n"); return 1; }
    std::printf("count=%zu\n", r.value().size());
    for (auto& rec : r.value()) std::printf("  %s tokens=%llu\n", rec.id.ToString().c_str(), (unsigned long long)rec.token_count);
    return 0;
  }
  if (cmd == "stats") {
    auto s = store.RuntimeStats();
    std::printf("prefixes=%llu segments=%llu nodes=%llu refs=%llu footprint=%llu lookups=%llu hits=%llu exact=%llu\n",
      (unsigned long long)s.prefix_count, (unsigned long long)s.segment_count, (unsigned long long)s.node_count,
      (unsigned long long)s.state_reference_count, (unsigned long long)s.token_index_footprint_bytes,
      (unsigned long long)s.total_lookups, (unsigned long long)s.total_hits, (unsigned long long)s.total_exact_hits);
    return 0;
  }
  if (cmd == "invalidate") {
    std::string idh; bool sub = Has(a, "--subtree"); for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--id") idh = a[i+1];
    auto id = PrefixId::Parse(idh);
    auto r = store.Invalidate(id.value(), sub);
    std::printf("%s\n", r.is_ok() ? "OK" : r.error().ToString().c_str());
    save(); return 0;
  }
  if (cmd == "attach-state") {
    std::string idh, ns, oid; std::uint64_t gen = 1;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) { if (a[i] == "--id") idh = a[i+1]; else if (a[i] == "--ns") ns = a[i+1]; else if (a[i] == "--object") oid = a[i+1]; else if (a[i] == "--gen") gen = std::stoull(a[i+1]); }
    auto id = PrefixId::Parse(idh);
    if (id.is_err() || ns.empty() || oid.empty()) { std::printf("attach-state needs --id --ns --object\n"); return 1; }
    StateReference ref; ref.ns = ns; ref.object_id = oid; ref.generation = gen; ref.type = StateReference::Type::kKvFabric; ref.compatibility = compat; ref.freshness_ts = 1;
    auto r = store.AttachState(id.value(), ref);
    std::printf("%s attached=%zu\n", r.is_ok() ? "OK" : r.error().ToString().c_str(), r.is_ok() ? r.value().size() : 0u);
    save(); return 0;
  }
  if (cmd == "retire-state") {
    std::string idh, ns, oid;
    for (std::size_t i = 0; i + 1 < a.size(); ++i) { if (a[i] == "--id") idh = a[i+1]; else if (a[i] == "--ns") ns = a[i+1]; else if (a[i] == "--object") oid = a[i+1]; }
    auto id = PrefixId::Parse(idh);
    if (id.is_err() || ns.empty() || oid.empty()) { std::printf("retire-state needs --id --ns --object\n"); return 1; }
    StateReference ref; ref.ns = ns; ref.object_id = oid; ref.compatibility = compat;
    auto r = store.RetireState(id.value(), ref);
    std::printf("%s\n", r.is_ok() ? "OK" : r.error().ToString().c_str());
    save(); return 0;
  }
  if (cmd == "snapshot") {
    auto sn = store.Snapshot();
    std::printf("snapshot records=%llu segments=%llu tokens=%llu refs=%llu\n", (unsigned long long)sn.prefix_count, (unsigned long long)sn.segment_count, (unsigned long long)sn.token_count, (unsigned long long)sn.state_reference_count);
    return 0;
  }
  if (cmd == "save") {
    std::string path; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--path") path = a[i+1];
    if (path.empty()) { std::printf("missing --path\n"); return 1; }
    auto r = store.SaveToFile(path); std::printf("save %s\n", r.is_ok() ? "OK" : r.error().ToString().c_str()); return 0;
  }
  if (cmd == "restore") {
    std::string path; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--path") path = a[i+1];
    auto r = store.LoadFromFile(path); std::printf("restore %s count=%llu\n", r.is_ok() ? "OK" : r.error().ToString().c_str(), r.is_ok() ? (unsigned long long)r.value() : 0ull); return 0;
  }
  if (cmd == "explain") {
    std::string toks; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--tokens") toks = a[i+1];
    auto plan = store.BuildReusePlan(ParseTokens(toks), &compat);
    std::printf("matched=%d matched_tokens=%llu exact=%d confidence=%f usable_refs=%zu stale_refs=%zu\n", plan.matched?1:0, (unsigned long long)plan.matched_token_count, plan.exact_match?1:0, (double)plan.confidence, plan.usable_refs.size(), plan.stale_refs.size());
    return 0;
  }
  if (cmd == "ancestors" || cmd == "descendants" || cmd == "branches" || cmd == "segments") {
    std::string idh; for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "--id") idh = a[i+1];
    auto id = PrefixId::Parse(idh);
    if (id.is_err()) { std::printf("bad id\n"); return 1; }
    if (cmd == "ancestors") { auto r = store.Ancestors(id.value()); for (auto& x : r.value_or({})) std::printf("anc %s len=%llu\n", x.id.ToString().c_str(), (unsigned long long)x.token_count); return 0; }
    if (cmd == "descendants") { auto r = store.Descendants(id.value()); for (auto& x : r.value_or({})) std::printf("desc %s len=%llu\n", x.id.ToString().c_str(), (unsigned long long)x.token_count); return 0; }
    if (cmd == "branches") { auto r = store.BranchPoints(id.value()); for (auto& x : r.value_or({})) std::printf("branch %s len=%llu\n", x.id.ToString().c_str(), (unsigned long long)x.token_count); return 0; }
    if (cmd == "segments") { auto r = store.Segments(id.value()); for (auto& x : r.value_or({})) std::printf("seg off=%llu len=%llu\n", (unsigned long long)x.token_offset, (unsigned long long)x.token_count); return 0; }
  }
  if (cmd == "benchmark") { std::printf("benchmark: run the dedicated benchmark binary (benchmarks/).\n"); return 0; }
  PrintUsage();
  return 1;
}
