// Prefix Fabric - structural index implementation (compressed radix trie).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/index.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "binary.hpp"

namespace prefix_fabric {

using pf_internal::BinaryReader;
using pf_internal::BinaryWriter;

namespace {

bool EdgeStartsWith(const TokenSequence& e, TokenId t) {
  return !e.empty() && e.front() == t;
}

std::size_t CommonPrefix(const TokenSequence& a, std::size_t a_off,
                         const TokenSequence& b, std::size_t b_off) {
  std::size_t a_len = a.size() > a_off ? a.size() - a_off : 0;
  std::size_t b_len = b.size() > b_off ? b.size() - b_off : 0;
  std::size_t m = std::min(a_len, b_len);
  std::size_t n = 0;
  for (std::size_t i = 0; i < m; ++i) {
    if (a[a_off + i] != b[b_off + i]) break;
    ++n;
  }
  return n;
}

std::string TokensText(const TokenSequence& t) {
  std::ostringstream os;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (i) os << ' ';
    os << t[i];
  }
  return os.str();
}

void WCompat(BinaryWriter& w, const CompatibilityKey& c) {
  w.Bytes(c.model);
  w.Bytes(c.tokenizer);
  w.Bytes(c.tokenization_version);
  w.Bytes(c.vocabulary);
  w.Bytes(c.special_token_policy);
  w.Bytes(c.prompt_template);
  w.Bytes(c.position_encoding);
  w.Bytes(c.kv_layout);
  w.Bytes(c.runtime_format);
  w.U64(static_cast<std::uint64_t>(c.extra.size()));
  for (const auto& kv : c.extra) { w.Bytes(kv.first); w.Bytes(kv.second); }
}

Result<CompatibilityKey> RCompat(BinaryReader& r) {
  CompatibilityKey c;
  auto m = r.Str(1u << 20); if (!m) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat model"});
  c.model = *m;
  auto tk = r.Str(1u << 20); if (!tk) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat tokenizer"});
  c.tokenizer = *tk;
  auto tv = r.Str(1u << 20); if (!tv) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat tokenization_version"});
  c.tokenization_version = *tv;
  auto vb = r.Str(1u << 20); if (!vb) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat vocab"});
  c.vocabulary = *vb;
  auto sp = r.Str(1u << 20); if (!sp) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat special"});
  c.special_token_policy = *sp;
  auto pt = r.Str(1u << 20); if (!pt) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat prompt"});
  c.prompt_template = *pt;
  auto pe = r.Str(1u << 20); if (!pe) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat posenc"});
  c.position_encoding = *pe;
  auto kv = r.Str(1u << 20); if (!kv) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat kv"});
  c.kv_layout = *kv;
  auto rf = r.Str(1u << 20); if (!rf) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat rft"});
  c.runtime_format = *rf;
  auto extra_n = r.U64(); if (!extra_n) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat extra count"});
  if (*extra_n > 100000) return Result<CompatibilityKey>::Err(Error{ErrorCode::kInvalidArgument, "compat extra too large"});
  for (std::uint64_t i = 0; i < *extra_n; ++i) {
    auto k = r.Str(1u << 20); if (!k) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat extra key"});
    auto v = r.Str(1u << 20); if (!v) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption, "compat extra val"});
    c.extra.emplace_back(*k, *v);
  }
  return Result<CompatibilityKey>::Ok(std::move(c));
}

void WRef(BinaryWriter& w, const StateReference& ref) {
  w.Bytes(ref.ns);
  w.Bytes(ref.object_id);
  w.U64(ref.generation);
  w.U32(static_cast<std::uint32_t>(ref.type));
  WCompat(w, ref.compatibility);
  w.Bytes(ref.location_hint);
  w.U8(ref.freshness_ts ? 1 : 0);
  if (ref.freshness_ts) w.U64(*ref.freshness_ts);
  w.Bytes(ref.integrity_token);
}

Result<StateReference> RRef(BinaryReader& r) {
  StateReference ref;
  auto ns = r.Str(1u << 20); if (!ns) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref ns"});
  ref.ns = *ns;
  auto oid = r.Str(1u << 20); if (!oid) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref oid"});
  ref.object_id = *oid;
  auto gen = r.U64(); if (!gen) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref gen"});
  ref.generation = *gen;
  auto ty = r.U32(); if (!ty) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref type"});
  ref.type = static_cast<StateReference::Type>(*ty);
  auto cp = RCompat(r); if (!cp) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref compat"});
  ref.compatibility = *cp;
  auto lh = r.Str(1u << 20); if (!lh) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref loc"});
  ref.location_hint = *lh;
  auto has_fresh = r.U8(); if (!has_fresh) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref fresh"});
  if (*has_fresh) { auto f = r.U64(); if (!f) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref fresh val"}); ref.freshness_ts = *f; }
  auto itk = r.Str(1u << 20); if (!itk) return Result<StateReference>::Err(Error{ErrorCode::kCorruption, "ref itok"});
  ref.integrity_token = *itk;
  return Result<StateReference>::Ok(std::move(ref));
}

void WSeg(BinaryWriter& w, const PrefixSegment& s) {
  w.U64(s.id.high());
  w.U64(s.id.low());
  w.U64(s.token_offset);
  w.U64(s.token_count);
  w.RawBytes(s.content_hash.data(), s.content_hash.size());
  w.U8(s.state_ref ? 1 : 0);
  if (s.state_ref) WRef(w, *s.state_ref);
  w.U64(s.generation);
  w.U8(s.active ? 1 : 0);
}

Result<PrefixSegment> RSeg(BinaryReader& r) {
  PrefixSegment s;
  auto hi = r.U64(); if (!hi) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg hi"});
  auto lo = r.U64(); if (!lo) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg lo"});
  s.id = PrefixId(*hi, *lo);
  auto off = r.U64(); if (!off) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg off"});
  s.token_offset = *off;
  auto cnt = r.U64(); if (!cnt) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg count"});
  s.token_count = *cnt;
  auto h = r.ReadRaw(32); if (!h || h->size() != 32) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg hash"});
  std::copy(h->begin(), h->end(), s.content_hash.begin());
  auto has = r.U8(); if (!has) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg ref flag"});
  if (*has) { auto rf = RRef(r); if (!rf) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg ref"}); s.state_ref = *rf; }
  auto gen = r.U64(); if (!gen) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg gen"});
  s.generation = *gen;
  auto act = r.U8(); if (!act) return Result<PrefixSegment>::Err(Error{ErrorCode::kCorruption, "seg active"});
  s.active = *act != 0;
  return Result<PrefixSegment>::Ok(std::move(s));
}

}  // namespace

struct TreeNode {
  TokenSequence edge;               // label from parent (empty for root)
  PrefixId id;
  ContentHash content_hash{};
  std::uint64_t token_count = 0;    // cumulative length of full sequence
  Generation generation = 1;
  bool active = true;
  std::uint32_t depth = 0;          // root depth = 0
  std::vector<PrefixSegment> segments;
  std::vector<StateReference> state_refs;
  std::string provenance;
  mutable std::optional<TokenSequence> full_spell;
  TreeNode* parent = nullptr;
  std::vector<std::unique_ptr<TreeNode>> children;  // sorted by edge label
  std::int64_t descendant_count = 0;                 // active descendants
  PrefixStatistics stats;

  bool IsLeaf() const { return children.empty(); }
};

// One compressed radix trie for a fixed CompatibilityKey under a fixed namespace.
struct Trie {
  CompatibilityKey compat;
  std::string key;                         // compat.IdentityHex()
  std::unique_ptr<TreeNode> root;
  std::unordered_map<PrefixId, TreeNode*> by_id;
  std::uint64_t prefixes = 0;   // active records (nodes with token_count>0)
  std::uint64_t nodes = 0;
  std::uint64_t segments = 0;
  std::uint64_t refs = 0;
  std::uint64_t tokens = 0;
  std::uint64_t footprint = 0;  // bytes

  Trie(CompatibilityKey c, const Namespace& ns, const SegmentationConfig& seg)
      : compat(std::move(c)), key(compat.IdentityHex()) {
    (void)seg;
    root = std::make_unique<TreeNode>();
    root->generation = 0;
    root->active = true;
    root->token_count = 0;
    root->depth = 0;
    root->content_hash = ComputePrefixContentHash(ns, compat, {});
    root->id = PrefixId::FromContentHash(root->content_hash);
    by_id[root->id] = root.get();
    nodes = 1;
  }
};

inline constexpr std::size_t kNumShards = 16;
struct IdEntry { std::size_t shard; std::string key; };

struct PrefixIndex::Impl {
  IndexConfig cfg;
  static constexpr std::size_t kNumShards = 16;

  struct Shard {
    mutable std::mutex mtx;
    std::vector<std::unique_ptr<Trie>> trees;
    std::unordered_map<std::string, Trie*> by_key;
  };

  std::array<Shard, kNumShards> shards_;
  mutable std::shared_mutex id_mtx_;
  std::unordered_map<PrefixId, IdEntry> id_index_;

  std::atomic<std::uint64_t> access_clock_{0};
  std::atomic<std::uint64_t> total_invalidations_{0};
  std::atomic<std::uint64_t> total_evictions_{0};
  std::atomic<std::uint64_t> g_prefixes_{0}, g_segments_{0}, g_refs_{0},
      g_nodes_{0}, g_tokens_{0}, g_footprint_{0};

  struct Counts {
    std::uint64_t prefixes = 0, nodes = 0, segments = 0, refs = 0, tokens = 0, footprint = 0;
  };

  explicit Impl(const IndexConfig& c) : cfg(c) {
    // A zero bound means "the default bound" (policy.hpp), never "nothing is allowed".
    const CapacityPolicy d = CapacityPolicy::Defaults();
    CapacityPolicy& p = cfg.capacity;
    if (p.max_prefixes == 0) p.max_prefixes = d.max_prefixes;
    if (p.max_segments == 0) p.max_segments = d.max_segments;
    if (p.max_token_index_footprint == 0) p.max_token_index_footprint = d.max_token_index_footprint;
    if (p.max_state_references == 0) p.max_state_references = d.max_state_references;
    if (p.max_nodes_total == 0) p.max_nodes_total = d.max_nodes_total;
    if (p.max_serialized_object_bytes == 0) p.max_serialized_object_bytes = d.max_serialized_object_bytes;
    if (p.max_network_frame_bytes == 0) p.max_network_frame_bytes = d.max_network_frame_bytes;
    if (p.max_subscriptions == 0) p.max_subscriptions = d.max_subscriptions;
    if (p.max_event_backlog == 0) p.max_event_backlog = d.max_event_backlog;
  }

  // --- counters ---
  static Counts CountsOf(const Trie* t) {
    Counts c;
    if (t) {
      c.prefixes = t->prefixes; c.nodes = t->nodes; c.segments = t->segments;
      c.refs = t->refs; c.tokens = t->tokens; c.footprint = t->footprint;
    }
    return c;
  }
  // The global counters follow the per-trie counters by exact differences, so no
  // operation overwrites them with a sum that was taken at another moment. `held`
  // is what Reserve() already added for this change. Unsigned wrap-around makes a
  // shrinking difference a subtraction.
  void Settle(const Counts& before, const Counts& held, const Counts& after) {
    g_prefixes_ += after.prefixes - before.prefixes - held.prefixes;
    g_nodes_ += after.nodes - before.nodes - held.nodes;
    g_segments_ += after.segments - before.segments - held.segments;
    g_refs_ += after.refs - before.refs - held.refs;
    g_tokens_ += after.tokens - before.tokens - held.tokens;
    g_footprint_ += after.footprint - before.footprint - held.footprint;
  }
  static bool TryAdd(std::atomic<std::uint64_t>& c, std::uint64_t add, std::uint64_t limit) {
    if (add == 0) return true;
    std::uint64_t cur = c.load();
    for (;;) {
      if (add > limit || cur > limit - add) return false;
      if (c.compare_exchange_weak(cur, cur + add)) return true;
    }
  }
  // Take room for `plan` under the hard bounds, all or nothing. Returns the bound
  // that is full, or nullptr when the room is held.
  const char* Reserve(const Counts& plan) {
    const CapacityPolicy& cap = cfg.capacity;
    if (!TryAdd(g_prefixes_, plan.prefixes, cap.max_prefixes)) return "prefix capacity reached";
    if (!TryAdd(g_nodes_, plan.nodes, cap.max_nodes_total)) {
      g_prefixes_ -= plan.prefixes;
      return "node capacity reached";
    }
    if (!TryAdd(g_segments_, plan.segments, cap.max_segments)) {
      g_prefixes_ -= plan.prefixes; g_nodes_ -= plan.nodes;
      return "segment capacity reached";
    }
    if (!TryAdd(g_refs_, plan.refs, cap.max_state_references)) {
      g_prefixes_ -= plan.prefixes; g_nodes_ -= plan.nodes; g_segments_ -= plan.segments;
      return "reference capacity reached";
    }
    if (!TryAdd(g_tokens_, plan.tokens, cap.max_token_index_footprint / 4u)) {
      g_prefixes_ -= plan.prefixes; g_nodes_ -= plan.nodes; g_segments_ -= plan.segments;
      g_refs_ -= plan.refs;
      return "token capacity reached";
    }
    g_footprint_ += plan.footprint;
    return nullptr;
  }

  std::size_t ShardIndex(const std::string& key) const {
    return std::hash<std::string>{}(key) % kNumShards;
  }
  Shard& ShardAt(std::size_t i) { return shards_[i]; }
  const Shard& ShardAt(std::size_t i) const { return shards_[i]; }

  Trie* FindTree(Shard& s, const std::string& key) {
    auto it = s.by_key.find(key);
    return it != s.by_key.end() ? it->second : nullptr;
  }
  Trie* GetOrCreateTree(Shard& s, const CompatibilityKey& compat) {
    std::string key = compat.IdentityHex();
    if (auto* t = FindTree(s, key)) return t;
    auto t = std::make_unique<Trie>(compat, cfg.ns, cfg.segmentation);
    Trie* raw = t.get();
    s.trees.push_back(std::move(t));
    s.by_key[key] = raw;
    return raw;
  }
  std::size_t KeyShard(const std::string& key) const { return ShardIndex(key); }

  // --- full-sequence / identity ---
  const TokenSequence& Full(TreeNode* n) const {
    if (!n->full_spell) {
      std::vector<const TreeNode*> chain;
      for (auto* p = n; p; p = p->parent) chain.push_back(p);
      TokenSequence seq;
      for (std::size_t i = chain.size(); i > 0; --i) {
        const auto& e = chain[i - 1]->edge;
        seq.insert(seq.end(), e.begin(), e.end());
      }
      n->full_spell.emplace(std::move(seq));
    }
    return *n->full_spell;
  }
  void RefreshIdentity(Trie* t, TreeNode* n) {
    n->full_spell.reset();
    const TokenSequence& full = Full(n);
    n->token_count = full.size();
    n->content_hash = ComputePrefixContentHash(cfg.ns, t->compat, full);
    n->id = PrefixId::FromContentHash(n->content_hash);
  }

  std::vector<PrefixSegment> ComputeSegmentsFor(const CompatibilityKey& compat,
                                                const TokenSequence& full) const {
    std::vector<PrefixSegment> segs;
    if (full.empty()) {
      PrefixSegment s;
      s.token_offset = 0; s.token_count = 0; s.generation = 1; s.active = true;
      s.content_hash = ComputeSegmentContentHash(cfg.ns, compat, full, 0, 0);
      s.id = PrefixId::FromContentHash(s.content_hash);
      segs.push_back(std::move(s));
      return segs;
    }
    if (cfg.segmentation.policy == SegmentationPolicy::kSingle) {
      PrefixSegment s;
      s.token_offset = 0; s.token_count = full.size(); s.generation = 1; s.active = true;
      s.content_hash = ComputeSegmentContentHash(cfg.ns, compat, full, 0, full.size());
      s.id = PrefixId::FromContentHash(s.content_hash);
      segs.push_back(std::move(s));
      return segs;
    }
    std::uint64_t sz = 1;
    if (cfg.segmentation.policy == SegmentationPolicy::kFixed)
      sz = cfg.segmentation.fixed_size == 0 ? 1 : cfg.segmentation.fixed_size;
    else if (cfg.segmentation.policy == SegmentationPolicy::kAdaptive)
      sz = cfg.segmentation.target_size == 0 ? 256 : cfg.segmentation.target_size;
    std::uint64_t off = 0;
    while (off < full.size()) {
      std::uint64_t len = std::min<std::uint64_t>(sz, full.size() - off);
      PrefixSegment s;
      s.token_offset = off; s.token_count = len; s.generation = 1; s.active = true;
      s.content_hash = ComputeSegmentContentHash(cfg.ns, compat, full, off, len);
      s.id = PrefixId::FromContentHash(s.content_hash);
      off += len;
      segs.push_back(std::move(s));
    }
    return segs;
  }

  void InsertChildSorted(TreeNode* parent, std::unique_ptr<TreeNode> c) {
    auto& children = parent->children;
    auto it = std::lower_bound(children.begin(), children.end(), c,
        [](const std::unique_ptr<TreeNode>& a, const std::unique_ptr<TreeNode>& b) {
          return a->edge < b->edge;
        });
    children.insert(it, std::move(c));
  }

  // The number of segments ComputeSegmentsFor produces for a prefix of n tokens.
  std::uint64_t SegmentCountFor(std::uint64_t n) const {
    if (n == 0 || cfg.segmentation.policy == SegmentationPolicy::kSingle) return 1;
    std::uint64_t sz = 1;
    if (cfg.segmentation.policy == SegmentationPolicy::kFixed)
      sz = cfg.segmentation.fixed_size == 0 ? 1 : cfg.segmentation.fixed_size;
    else if (cfg.segmentation.policy == SegmentationPolicy::kAdaptive)
      sz = cfg.segmentation.target_size == 0 ? 256 : cfg.segmentation.target_size;
    return n / sz + (n % sz ? 1 : 0);
  }

  static bool IsStale(const StateReference& r) { return r.freshness_ts && *r.freshness_ts == 0; }

  // How many of `refs` would become new attachments on a node that holds `have`.
  static std::uint64_t NewRefCount(const std::vector<StateReference>* have,
                                   const std::vector<StateReference>& refs) {
    std::uint64_t n = 0;
    std::vector<std::string> seen;
    for (const auto& r : refs) {
      if (!r.IsWellFormed()) continue;
      const std::string k = r.Key();
      bool present = std::find(seen.begin(), seen.end(), k) != seen.end();
      if (!present && have)
        for (const auto& e : *have) if (e.Key() == k) { present = true; break; }
      if (!present) { seen.push_back(k); ++n; }
    }
    return n;
  }

  // What TrieInsert would add for these tokens: the same walk, nothing changed.
  Counts PlanInsert(const Trie* t, const TokenSequence& tokens,
                    const std::vector<StateReference>& refs) const {
    Counts d;
    if (!t) d.nodes += 1;  // the root of a new trie
    if (tokens.empty()) return d;
    const std::uint64_t n = tokens.size();
    auto leaf = [&](std::uint64_t edge_len) {
      d.nodes += 1; d.prefixes += 1; d.segments += SegmentCountFor(n); d.tokens += n;
      d.footprint += 4u * edge_len; d.refs += NewRefCount(nullptr, refs);
    };
    if (!t) { leaf(n); return d; }
    const TreeNode* node = t->root.get();
    std::size_t i = 0;
    for (;;) {
      if (i == tokens.size()) {  // the prefix is already a node
        if (node->active) {
          d.refs += NewRefCount(&node->state_refs, refs);
        } else {
          const std::uint64_t fresh = NewRefCount(nullptr, refs);
          if (fresh > node->state_refs.size()) d.refs += fresh - node->state_refs.size();
        }
        const std::uint64_t segs = SegmentCountFor(node->token_count);
        if (segs > node->segments.size()) d.segments += segs - node->segments.size();
        return d;
      }
      const TreeNode* child = nullptr;
      for (auto& c : node->children) if (EdgeStartsWith(c->edge, tokens[i])) { child = c.get(); break; }
      if (!child) { leaf(n - i); return d; }
      const std::size_t lcp = CommonPrefix(child->edge, 0, tokens, i);
      if (lcp == child->edge.size()) { node = child; i += lcp; continue; }
      const std::uint64_t len = i + lcp;  // a split node for the shared part
      d.nodes += 1; d.prefixes += 1; d.segments += SegmentCountFor(len); d.tokens += len;
      if (len == n) { d.refs += NewRefCount(nullptr, refs); return d; }
      leaf(n - len);
      return d;
    }
  }

  // Attach `r` to `n`. A reference to an object that is already attached is the
  // same attachment: a newer generation replaces it (so does the same generation
  // when the attached one was marked stale), an older generation is refused.
  bool AttachRef(Trie* t, TreeNode* n, const StateReference& r) {
    for (auto& e : n->state_refs) {
      if (e.Key() != r.Key()) continue;
      if (r.generation < e.generation) return false;
      if (r.generation > e.generation || IsStale(e)) e = r;
      return true;
    }
    n->state_refs.push_back(r);
    ++t->refs;
    return true;
  }

  void AddStateRefs(Trie* t, TreeNode* n, const std::vector<StateReference>& refs) {
    for (const auto& r : refs) if (r.IsWellFormed()) AttachRef(t, n, r);
  }

  // Per-trie counters only; the caller settles the global ones.
  void FinalizeNew(Trie* t, TreeNode* n, const std::vector<StateReference>& refs,
                   const std::string& provenance, std::size_t shard) {
    n->provenance = provenance;
    RefreshIdentity(t, n);
    n->segments = ComputeSegmentsFor(t->compat, Full(n));
    AddStateRefs(t, n, refs);
    t->nodes += 1;
    t->segments += static_cast<std::uint64_t>(n->segments.size());
    t->tokens += n->token_count;
    t->footprint += TokenFootprint(n->edge);
    if (n->token_count > 0) ++t->prefixes;
    // A prefix that was just added is the most recently used one.
    n->stats.last_access_ns = ++access_clock_;
    t->by_id[n->id] = n;
    {
      std::unique_lock<std::shared_mutex> lk(id_mtx_);
      id_index_[n->id] = IdEntry{shard, t->key};
    }
    for (auto* a = n->parent; a; a = a->parent) a->descendant_count += 1;
  }

  TreeNode* TrieInsert(Trie* t, const TokenSequence& tokens,
                       const std::vector<StateReference>& refs,
                       const std::string& provenance, std::size_t shard) {
    if (tokens.empty()) return t->root.get();
    TreeNode* node = t->root.get();
    std::size_t i = 0;
    while (true) {
      if (i == tokens.size()) {
        if (!node->active) {
          // Adding an invalidated prefix again makes it current under a new
          // generation. What was attached to the retired one does not come back.
          node->active = true;
          t->refs -= static_cast<std::uint64_t>(node->state_refs.size());
          node->state_refs.clear();
        }
        node->generation += 1;
        AddStateRefs(t, node, refs);
        if (!provenance.empty()) node->provenance = provenance;
        const std::uint64_t had = static_cast<std::uint64_t>(node->segments.size());
        node->segments = ComputeSegmentsFor(t->compat, Full(node));
        t->segments += static_cast<std::uint64_t>(node->segments.size()) - had;
        node->stats.last_access_ns = ++access_clock_;
        return node;
      }
      TreeNode* child = nullptr;
      for (auto& c : node->children) if (EdgeStartsWith(c->edge, tokens[i])) { child = c.get(); break; }
      if (!child) {
        auto c = std::make_unique<TreeNode>();
        c->parent = node;
        c->edge.assign(tokens.begin() + static_cast<std::ptrdiff_t>(i), tokens.end());
        c->depth = node->depth + 1;
        TreeNode* raw = c.get();
        InsertChildSorted(node, std::move(c));
        FinalizeNew(t, raw, refs, provenance, shard);
        return raw;
      }
      std::size_t lcp = CommonPrefix(child->edge, 0, tokens, i);
      if (lcp == child->edge.size()) { node = child; i += lcp; continue; }
      // reparent child under a new split node
      std::size_t cidx = 0;
      for (std::size_t k = 0; k < node->children.size(); ++k)
        if (node->children[k].get() == child) { cidx = k; break; }
      std::unique_ptr<TreeNode> child_owned = std::move(node->children[cidx]);
      node->children.erase(node->children.begin() + static_cast<std::ptrdiff_t>(cidx));
      auto split_ptr = std::make_unique<TreeNode>();
      TreeNode* split = split_ptr.get();
      split->parent = node;
      split->edge.assign(child->edge.begin(), child->edge.begin() + static_cast<std::ptrdiff_t>(lcp));
      split->depth = node->depth + 1;
      child->edge.assign(child->edge.begin() + static_cast<std::ptrdiff_t>(lcp), child->edge.end());
      child->parent = split;
      // The whole moved subtree is one level deeper and hangs below the split node.
      {
        std::vector<TreeNode*> moved{child};
        while (!moved.empty()) {
          TreeNode* m = moved.back(); moved.pop_back();
          m->depth += 1;
          for (auto& g : m->children) moved.push_back(g.get());
        }
      }
      split->descendant_count = 1 + child->descendant_count;
      InsertChildSorted(split, std::move(child_owned));
      InsertChildSorted(node, std::move(split_ptr));
      // The references describe the prefix being inserted. The split node is a
      // different, shorter prefix unless the insert ends exactly here.
      static const std::vector<StateReference> kNoRefs;
      FinalizeNew(t, split, (i + lcp == tokens.size()) ? refs : kNoRefs, provenance, shard);
      t->footprint -= TokenFootprint(split->edge);  // those tokens moved off the child's edge
      node = split;
      i += lcp;
    }
  }

  Result<PrefixRecord> MaterializeRecord(Trie* t, TreeNode* n) {
    if (!n) return Result<PrefixRecord>::Err(Error{ErrorCode::kNotFound, "no node"});
    PrefixRecord r;
    r.id = n->id;
    r.ns = cfg.ns;
    r.compat = t->compat;
    r.generation = n->generation;
    r.token_count = n->token_count;
    r.tokens = Full(n);
    r.content_hash = n->content_hash;
    if (n->parent) r.parent = n->parent->id;
    r.branch_depth = n->depth;
    r.segments = n->segments;
    r.state_refs = n->state_refs;
    r.provenance = n->provenance;
    r.active = n->active;
    return Result<PrefixRecord>::Ok(std::move(r));
  }

  bool FindIdEntry(const PrefixId& id, IdEntry& out) {
    std::shared_lock<std::shared_mutex> lk(id_mtx_);
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;
    out = it->second;
    return true;
  }

  // Rebuild a trie's by_id, id_index_ and all counters from a full walk.
  void RecomputeTrie(std::size_t shard, Trie* t) {
    t->by_id.clear();
    t->nodes = 0; t->prefixes = 0; t->segments = 0; t->refs = 0; t->tokens = 0; t->footprint = 0;
    t->by_id[t->root->id] = t->root.get();
    t->nodes = 1;
    std::vector<TreeNode*> stack;
    for (auto& ch : t->root->children) stack.push_back(ch.get());
    while (!stack.empty()) {
      TreeNode* cur = stack.back(); stack.pop_back();
      t->by_id[cur->id] = cur;
      ++t->nodes;
      t->segments += static_cast<std::uint64_t>(cur->segments.size());
      t->refs += static_cast<std::uint64_t>(cur->state_refs.size());
      t->tokens += cur->token_count;
      t->footprint += TokenFootprint(cur->edge);
      if (cur->token_count > 0) ++t->prefixes;
      for (auto& ch : cur->children) stack.push_back(ch.get());
    }
    // Descendant counts, children before their parents.
    std::vector<TreeNode*> order = Preorder(t->root.get());
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      TreeNode* n = *it;
      n->descendant_count = 0;
      for (auto& c : n->children) n->descendant_count += (1 + c->descendant_count);
    }
    {
      // The root is not a prefix: its id is never published.
      std::unique_lock<std::shared_mutex> lk(id_mtx_);
      for (auto& kv : t->by_id)
        if (kv.second != t->root.get()) id_index_[kv.first] = IdEntry{shard, t->key};
    }
  }

  // Rebuild global counters by summing all trees. MUST be called with no shard
  // locks held.
  void RebuildGlobalCounters() {
    std::uint64_t prefixes = 0, segs = 0, refs = 0, nodes = 0, tokens = 0, fp = 0;
    for (auto& sh : shards_) {
      std::lock_guard<std::mutex> lk(sh.mtx);
      for (auto& t : sh.trees) {
        prefixes += t->prefixes; segs += t->segments; refs += t->refs;
        nodes += t->nodes; tokens += t->tokens; fp += t->footprint;
      }
    }
    g_prefixes_ = prefixes; g_segments_ = segs; g_refs_ = refs;
    g_nodes_ = nodes; g_tokens_ = tokens; g_footprint_ = fp;
  }

  static double EvictionScoreNode(const EvictionConfig& cfg, const TreeNode* n) {
    double freq = static_cast<double>(n->stats.hit_count + n->stats.lookup_count);
    double recency = static_cast<double>(n->stats.last_access_ns);
    double structural = static_cast<double>(n->children.size());
    switch (cfg.policy) {
      case EvictionPolicy::kLruRecency: return recency;
      case EvictionPolicy::kLfuFrequency: return freq;
      case EvictionPolicy::kStructuralValue: return structural;
      case EvictionPolicy::kComposite: break;
    }
    return cfg.frequency_weight * freq + cfg.recency_weight * recency +
           cfg.structural_weight * structural;
  }

  // Remove a node subtree from a trie. returns removed node count.
  std::size_t RemoveSubtree(std::size_t shard, Trie* t, TreeNode* n, std::string& err) {
    if (n == t->root.get()) { err = "cannot remove root"; return 0; }
    if (!n->parent) { err = "orphaned node"; return 0; }
    const Counts before = CountsOf(t);
    std::vector<PrefixId> gone;
    std::vector<TreeNode*> stack{n};
    while (!stack.empty()) {
      TreeNode* cur = stack.back(); stack.pop_back();
      gone.push_back(cur->id);
      for (auto& ch : cur->children) stack.push_back(ch.get());
    }
    // Detach n (and its subtree) by releasing the owning unique_ptr.
    auto& sib = n->parent->children;
    for (std::size_t k = 0; k < sib.size(); ++k)
      if (sib[k].get() == n) { sib[k].reset(); sib.erase(sib.begin() + static_cast<std::ptrdiff_t>(k)); break; }
    // Recompute counters and id mappings for the whole trie.
    RecomputeTrie(shard, t);
    {
      // The ids of removed prefixes no longer resolve; the id map does not keep them.
      std::unique_lock<std::shared_mutex> lk(id_mtx_);
      for (const auto& id : gone) {
        auto it = id_index_.find(id);
        if (it != id_index_.end() && it->second.shard == shard && it->second.key == t->key) id_index_.erase(it);
      }
    }
    Settle(before, Counts{}, CountsOf(t));
    return gone.size();
  }

  // Evict up to `target` prefixes, lowest score first. Only a prefix without
  // descendants is ever removed, so nothing disappears that is not reported;
  // removing it can expose its parent, which the next round then considers.
  // Invalidated prefixes go first. `keep`, when set, is left alone.
  std::vector<PrefixId> EvictLeaves(std::size_t target, const PrefixId* keep) {
    struct Cand { std::size_t shard; std::string key; PrefixId id; bool active; double score; };
    std::vector<PrefixId> removed;
    while (removed.size() < target) {
      std::vector<Cand> cands;
      {
        std::vector<std::unique_lock<std::mutex>> locks;
        for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(shards_[s].mtx);
        for (std::size_t s = 0; s < kNumShards; ++s) {
          for (auto& t : shards_[s].trees) {
            for (TreeNode* n : Preorder(t->root.get())) {
              if (!n->parent || !n->children.empty()) continue;
              if (keep && n->id == *keep) continue;
              cands.push_back(Cand{s, t->key, n->id, n->active, EvictionScoreNode(cfg.eviction, n)});
            }
          }
        }
      }
      if (cands.empty()) break;
      std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.active != b.active) return !a.active;
        if (a.score != b.score) return a.score < b.score;
        return a.id < b.id;
      });
      const std::size_t had = removed.size();
      for (auto& c : cands) {
        if (removed.size() >= target) break;
        auto& sh = shards_[c.shard];
        std::lock_guard<std::mutex> lk(sh.mtx);
        Trie* t = FindTree(sh, c.key);
        if (!t) continue;
        auto it = t->by_id.find(c.id);
        if (it == t->by_id.end()) continue;
        TreeNode* n = it->second;
        if (!n->parent || !n->children.empty()) continue;
        std::string err;
        RemoveSubtree(c.shard, t, n, err);
        if (err.empty()) { removed.push_back(c.id); ++total_evictions_; }
      }
      if (removed.size() == had) break;
    }
    return removed;
  }

  struct WalkRes { TreeNode* node; std::size_t matched; };
  WalkRes Walk(const Trie* t, const TokenSequence& request) const {
    TreeNode* node = t->root.get();
    std::size_t i = 0;
    while (true) {
      if (i >= request.size()) break;
      TreeNode* child = nullptr;
      for (auto& c : node->children) if (EdgeStartsWith(c->edge, request[i])) { child = c.get(); break; }
      if (!child) break;
      std::size_t lcp = CommonPrefix(child->edge, 0, request, i);
      // Only descend when the child edge is entirely consumed (the request is
      // at least as long as the edge); otherwise stop here so we never report a
      // match longer than the request.
      if (lcp != child->edge.size()) break;
      i += lcp;
      node = child;
    }
    return WalkRes{node, i};
  }

  // Returns node-to-index mapping for deterministic preorder serialization.
  static std::vector<TreeNode*> Preorder(TreeNode* root) {
    std::vector<TreeNode*> order;
    std::vector<TreeNode*> stack;
    for (auto rit = root->children.rbegin(); rit != root->children.rend(); ++rit) stack.push_back(rit->get());
    order.push_back(root);
    while (!stack.empty()) {
      TreeNode* cur = stack.back(); stack.pop_back();
      order.push_back(cur);
      for (auto rit = cur->children.rbegin(); rit != cur->children.rend(); ++rit) stack.push_back(rit->get());
    }
    return order;
  }
};

// ---- public wrappers ----

PrefixIndex::PrefixIndex(const IndexConfig& config) : impl_(std::make_unique<Impl>(config)) {}
PrefixIndex::~PrefixIndex() = default;

const Namespace& PrefixIndex::NamespaceName() const { return impl_->cfg.ns; }
const CapacityPolicy& PrefixIndex::Capacity() const { return impl_->cfg.capacity; }
const SegmentationConfig& PrefixIndex::Segmentation() const { return impl_->cfg.segmentation; }


// ---- matching ----
PrefixMatch PrefixIndex::LongestPrefixMatch(const CompatibilityKey& compat,
                                            const TokenSequence& request) {
  auto& impl = *impl_;
  std::string key = compat.IdentityHex();
  std::size_t shard = impl.KeyShard(key);
  auto& sh = impl.ShardAt(shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, key);
  if (!t) return PrefixMatch::None();
  auto wr = impl.Walk(t, request);
  TreeNode* node = wr.node;
  // climb to nearest active, non-empty node
  while (node && (!node->active || node->token_count == 0)) node = node->parent;
  if (!node || node->token_count == 0) return PrefixMatch::None();
  const TokenSequence& full = impl.Full(node);
  TokenSequence mp(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(node->token_count));
  TokenSequence us(request.begin() + static_cast<std::ptrdiff_t>(node->token_count), request.end());
  bool exact = (node->token_count == request.size());
  node->stats.lookup_count += 1;
  node->stats.last_access_ns = ++impl.access_clock_;
  node->stats.hit_count += 1;
  node->stats.reused_tokens += node->token_count;
  if (exact) node->stats.exact_hit_count += 1; else node->stats.partial_hit_count += 1;
  PrefixMatch m;
  m.set_prefix(node->id, node->token_count, mp, us, exact);
  return m;
}

PrefixMatch PrefixIndex::ExactMatch(const CompatibilityKey& compat,
                                    const TokenSequence& request) {
  auto& impl = *impl_;
  std::string key = compat.IdentityHex();
  std::size_t shard = impl.KeyShard(key);
  auto& sh = impl.ShardAt(shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, key);
  if (!t) return PrefixMatch::None();
  auto wr = impl.Walk(t, request);
  TreeNode* node = wr.node;
  // Exact hit requires the walked node's full sequence to equal the request
  // (i.e. the whole request was consumed) and the node to be active.
  if (!node || node->token_count != request.size() || !node->active) return PrefixMatch::None();
  const TokenSequence& full = impl.Full(node);
  if (!TokensEqual(full, request)) return PrefixMatch::None();
  node->stats.lookup_count += 1;
  node->stats.hit_count += 1;
  node->stats.exact_hit_count += 1;
  node->stats.reused_tokens += node->token_count;
  node->stats.last_access_ns = ++impl.access_clock_;
  PrefixMatch em;
  TokenSequence us;
  em.set_prefix(node->id, node->token_count, full, us, true);
  return em;
}

std::vector<PrefixMatch> PrefixIndex::AllMatches(const CompatibilityKey& compat,
                                                 const TokenSequence& request) {
  auto& impl = *impl_;
  std::string key = compat.IdentityHex();
  std::size_t shard = impl.KeyShard(key);
  auto& sh = impl.ShardAt(shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  std::vector<PrefixMatch> out;
  Trie* t = impl.FindTree(sh, key);
  if (!t) return out;
  TreeNode* node = t->root.get();
  std::size_t i = 0;
  while (true) {
    if (node->token_count > 0 && node->active && node->token_count <= request.size()) {
      const TokenSequence& full = impl.Full(node);
      TokenSequence mp(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(node->token_count));
      TokenSequence us(request.begin() + static_cast<std::ptrdiff_t>(node->token_count), request.end());
      PrefixMatch m;
      m.set_prefix(node->id, node->token_count, mp, us, node->token_count == request.size());
      out.push_back(std::move(m));
    }
    if (i >= request.size()) break;
    TreeNode* child = nullptr;
    for (auto& c : node->children) if (EdgeStartsWith(c->edge, request[i])) { child = c.get(); break; }
    if (!child) break;
    std::size_t lcp = CommonPrefix(child->edge, 0, request, i);
    if (lcp != child->edge.size()) break;
    i += lcp;
    node = child;
  }
  return out;
}

std::uint64_t PrefixIndex::CommonPrefixLength(const CompatibilityKey& compat,
                                              const TokenSequence& a,
                                              const TokenSequence& b) {
  // A property of the two sequences alone: it does not depend on what is indexed.
  (void)compat;
  return static_cast<std::uint64_t>(CommonPrefix(a, 0, b, 0));
}

// ---- mutation ----
Result<PrefixRecord> PrefixIndex::Insert(const CompatibilityKey& compat,
                                         const TokenSequence& tokens,
                                         const std::vector<StateReference>& refs,
                                         const std::string& provenance) {
  auto& impl = *impl_;
  std::string key = compat.IdentityHex();
  std::size_t shard = impl.KeyShard(key);
  const std::uint64_t cap = impl.cfg.capacity.max_prefixes;
  const bool hard = !impl.cfg.eviction.auto_evict;
  Result<PrefixRecord> res;
  {
    auto& sh = impl.ShardAt(shard);
    std::lock_guard<std::mutex> lk(sh.mtx);
    Trie* t = impl.FindTree(sh, key);
    // Hard bounds (auto_evict off): work out exactly what this insert adds and take
    // that room before anything changes, so a bound is never passed and a refused
    // insert leaves nothing behind.
    Impl::Counts held;
    if (hard) {
      held = impl.PlanInsert(t, tokens, refs);
      if (const char* full = impl.Reserve(held))
        return Result<PrefixRecord>::Err(Error{ErrorCode::kCapacityExceeded, full});
    }
    const Impl::Counts before = Impl::CountsOf(t);
    if (!t) t = impl.GetOrCreateTree(sh, compat);
    TreeNode* n = impl.TrieInsert(t, tokens, refs, provenance, shard);
    impl.Settle(before, held, Impl::CountsOf(t));
    res = impl.MaterializeRecord(t, n);
  }
  // Post-insert enforcement: when auto_evict is on, evict after the insert so the
  // capacity bound is held immediately (even when one insert branches into
  // intermediate prefixes). Room is made with other prefixes first; the bound
  // still wins when that is not enough.
  if (!hard && impl.g_prefixes_.load() > cap) {
    const PrefixId added = res.value().id;
    impl.EvictLeaves(static_cast<std::size_t>(impl.g_prefixes_.load() - cap), &added);
    const std::uint64_t cur = impl.g_prefixes_.load();
    if (cur > cap) impl.EvictLeaves(static_cast<std::size_t>(cur - cap), nullptr);
  }
  return res;
}

Result<void> PrefixIndex::Invalidate(const PrefixId& id, bool subtree) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  TreeNode* n = it->second;
  std::vector<TreeNode*> targets;
  if (!subtree) { targets.push_back(n); }
  else {
    std::vector<TreeNode*> stack{n};
    while (!stack.empty()) { TreeNode* c = stack.back(); stack.pop_back(); targets.push_back(c); for (auto& ch : c->children) stack.push_back(ch.get()); }
  }
  for (TreeNode* c : targets) {
    if (c->active) {
      c->active = false;
      c->generation += 1;
      for (auto& r : c->state_refs) r.freshness_ts = std::uint64_t{0};  // stale, timestamped or not
      ++impl.total_invalidations_;
    }
  }
  return Result<void>::Ok();
}

Result<void> PrefixIndex::InvalidateByCompat(const CompatibilityKey& compat) {
  auto& impl = *impl_;
  std::string key = compat.IdentityHex();
  std::size_t shard = impl.KeyShard(key);
  auto& s = impl.ShardAt(shard);
  std::lock_guard<std::mutex> lk(s.mtx);
  Trie* t = impl.FindTree(s, key);
  if (!t) return Result<void>::Ok();
  std::vector<TreeNode*> stack;
  for (auto& c : t->root->children) stack.push_back(c.get());
  while (!stack.empty()) {
    TreeNode* n = stack.back(); stack.pop_back();
    if (n->active) {
      n->active = false; n->generation += 1;
      for (auto& r : n->state_refs) r.freshness_ts = std::uint64_t{0};  // stale
      ++impl.total_invalidations_;
    }
    for (auto& c : n->children) stack.push_back(c.get());
  }
  return Result<void>::Ok();
}

Result<std::size_t> PrefixIndex::Remove(const PrefixId& id, bool cascade) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::size_t>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::size_t removed = 0;
  {
    auto& sh = impl.ShardAt(loc.shard);
    std::lock_guard<std::mutex> lk(sh.mtx);
    Trie* t = impl.FindTree(sh, loc.key);
    if (!t) return Result<std::size_t>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
    auto it = t->by_id.find(id);
    if (it == t->by_id.end()) return Result<std::size_t>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
    TreeNode* n = it->second;
    std::string err;
    if (!cascade && !n->IsLeaf()) {
      // only allow removing a leaf unless cascade
      return Result<std::size_t>::Err(Error{ErrorCode::kInvalidState, "cannot remove prefix with live descendants without cascade"});
    }
    removed = impl.RemoveSubtree(loc.shard, t, n, err);
    if (!err.empty()) return Result<std::size_t>::Err(Error{ErrorCode::kInvalidState, err});
  }
  ++impl.total_evictions_;
  return Result<std::size_t>::Ok(removed);
}

// ---- lookup by id ----
Result<PrefixRecord> PrefixIndex::Get(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<PrefixRecord>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<PrefixRecord>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<PrefixRecord>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  return impl.MaterializeRecord(t, it->second);
}


// ---- lineage ----
Result<PrefixPath> PrefixIndex::Path(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<PrefixPath>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<PrefixPath>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<PrefixPath>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  TreeNode* n = it->second;
  std::vector<TreeNode*> chain;
  for (auto* x = n; x && x->parent; x = x->parent) chain.push_back(x);
  std::reverse(chain.begin(), chain.end());  // root-adjacent first
  PrefixPath path;
  path.token_count = n->token_count;
  for (TreeNode* x : chain) {
    path.nodes.push_back(x->id);
    path.edge_labels.push_back(x->edge);
    path.edge_hex.push_back(TokensText(x->edge));
  }
  return Result<PrefixPath>::Ok(std::move(path));
}

Result<std::vector<PrefixRecord>> PrefixIndex::Ancestors(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::vector<PrefixRecord> out;
  for (auto* x = it->second->parent; x && x->parent; x = x->parent) {
    PrefixRecord r = impl.MaterializeRecord(t, x).value_or_throw();
    out.push_back(std::move(r));
  }
  std::reverse(out.begin(), out.end());
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

Result<std::vector<PrefixRecord>> PrefixIndex::Descendants(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::vector<PrefixRecord> out;
  TreeNode* n = it->second;
  // children are sorted; preorder
  std::vector<TreeNode*> stack;
  for (auto rit = n->children.rbegin(); rit != n->children.rend(); ++rit) stack.push_back(rit->get());
  while (!stack.empty()) {
    TreeNode* c = stack.back(); stack.pop_back();
    out.push_back(impl.MaterializeRecord(t, c).value_or_throw());
    for (auto rit = c->children.rbegin(); rit != c->children.rend(); ++rit) stack.push_back(rit->get());
  }
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

Result<std::vector<PrefixRecord>> PrefixIndex::Children(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::vector<PrefixRecord> out;
  for (auto& c : it->second->children) out.push_back(impl.MaterializeRecord(t, c.get()).value_or_throw());
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

Result<std::vector<PrefixRecord>> PrefixIndex::BranchPoints(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::vector<PrefixRecord> out;
  for (auto* x = it->second; x && x->parent; x = x->parent) {
    if (x->children.size() > 1) out.push_back(impl.MaterializeRecord(t, x).value_or_throw());
  }
  std::reverse(out.begin(), out.end());
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

Result<std::vector<PrefixRecord>> PrefixIndex::CommonAncestors(const PrefixId& a, const PrefixId& b) {
  auto& impl = *impl_;
  PrefixId pa = a, pb = b;
  auto rek = [&](const PrefixId& x) -> Result<std::vector<PrefixId>> {
    // walk ancestor chain (including self) using the id lookup
    std::vector<PrefixId> ids;
    IdEntry loc;
    if (!impl.FindIdEntry(x, loc)) return Result<std::vector<PrefixId>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
    auto& sh = impl.ShardAt(loc.shard);
    std::lock_guard<std::mutex> lk(sh.mtx);
    Trie* t = impl.FindTree(sh, loc.key);
    if (!t) return Result<std::vector<PrefixId>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
    auto it = t->by_id.find(x);
    if (it == t->by_id.end()) return Result<std::vector<PrefixId>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
    for (auto* n = it->second; n && n->parent; n = n->parent) ids.push_back(n->id);  // the root is not a prefix
    return Result<std::vector<PrefixId>>::Ok(std::move(ids));
  };
  auto ra = rek(a);
  auto rb = rek(b);
  if (ra.is_err() || rb.is_err()) return Result<std::vector<PrefixRecord>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  std::unordered_map<PrefixId, int> aset;
  for (auto& id : ra.value()) aset[id] = 1;
  std::vector<PrefixId> common;
  for (auto& id : rb.value()) if (aset.count(id)) common.push_back(id);
  std::vector<PrefixRecord> out;
  for (auto& id : common) { auto r = Get(id); if (r.is_ok()) out.push_back(r.value()); }
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

// ---- composition / slicing ----
Result<PrefixRecord> PrefixIndex::Slice(const PrefixId& id, std::uint64_t n) {
  auto src = Get(id);
  if (src.is_err()) return src;
  if (n == 0) return Result<PrefixRecord>::Err(Error{ErrorCode::kInvalidArgument, "slice length must be > 0"});
  if (n > src.value().token_count) return Result<PrefixRecord>::Err(Error{ErrorCode::kInvalidArgument, "slice length exceeds prefix length"});
  if (n == src.value().token_count) return src;
  TokenSequence target(src.value().tokens.begin(), src.value().tokens.begin() + static_cast<std::ptrdiff_t>(n));
  return Insert(src.value().compat, target, {}, "slice:" + id.ToString());
}

Result<PrefixRecord> PrefixIndex::Append(const PrefixId& id, const TokenSequence& suffix) {
  auto src = Get(id);
  if (src.is_err()) return src;
  TokenSequence target = src.value().tokens;
  target.insert(target.end(), suffix.begin(), suffix.end());
  return Insert(src.value().compat, target, {}, "append:" + id.ToString());
}

Result<PrefixRecord> PrefixIndex::Compose(const CompatibilityKey& compat,
                                          const std::vector<PrefixId>& pieces) {
  TokenSequence total;
  for (const auto& pid : pieces) {
    auto r = Get(pid);
    if (r.is_err()) return r;
    if (!(r.value().compat == compat)) {
      return Result<PrefixRecord>::Err(Error{ErrorCode::kCompatibilityMismatch, "incompatible composition: piece compat differs"});
    }
    total.insert(total.end(), r.value().tokens.begin(), r.value().tokens.end());
  }
  return Insert(compat, total, {}, "compose");
}

Result<PrefixRecord> PrefixIndex::Fork(const PrefixId& id, const TokenSequence& suffix) {
  return Append(id, suffix);
}

// ---- segmentation ----
Result<std::vector<PrefixSegment>> PrefixIndex::Segments(const PrefixId& id) {
  auto r = Get(id);
  if (r.is_err()) return Result<std::vector<PrefixSegment>>::Err(r.error());
  return Result<std::vector<PrefixSegment>>::Ok(r.value().segments);
}

// ---- state references ----
Result<std::vector<StateReference>> PrefixIndex::AttachState(const PrefixId& id,
                                                             const StateReference& ref) {
  if (!ref.IsWellFormed()) return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kInvalidArgument, "malformed state reference"});
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  TreeNode* n = it->second;
  bool known = false;
  for (auto& e : n->state_refs) {
    if (e.Key() != ref.Key()) continue;
    known = true;
    if (ref.generation < e.generation)
      return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kStaleGeneration, "an attached reference to this object has a newer generation"});
    break;
  }
  Impl::Counts held;
  if (!known && !impl.cfg.eviction.auto_evict) {
    if (!Impl::TryAdd(impl.g_refs_, 1, impl.cfg.capacity.max_state_references))
      return Result<std::vector<StateReference>>::Err(Error{ErrorCode::kCapacityExceeded, "reference capacity reached"});
    held.refs = 1;
  }
  const Impl::Counts before = Impl::CountsOf(t);
  impl.AttachRef(t, n, ref);
  impl.Settle(before, held, Impl::CountsOf(t));
  return Result<std::vector<StateReference>>::Ok(n->state_refs);
}

Result<void> PrefixIndex::RetireState(const PrefixId& id, const StateReference& ref) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<void>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  TreeNode* n = it->second;
  auto& vec = n->state_refs;
  const Impl::Counts before = Impl::CountsOf(t);
  for (std::size_t k = 0; k < vec.size(); ++k) {
    if (vec[k].Key() == ref.Key()) { vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(k)); if (t->refs > 0) --t->refs; break; }
  }
  impl.Settle(before, Impl::Counts{}, Impl::CountsOf(t));
  return Result<void>::Ok();
}

Result<void> PrefixIndex::InvalidateStateByObject(const std::string& ns,
                                                  const std::string& object_id) {
  auto& impl = *impl_;
  std::vector<std::unique_lock<std::mutex>> locks;
  for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(impl.shards_[s].mtx);
  for (auto& sh : impl.shards_) {
    for (auto& t : sh.trees) {
      std::vector<TreeNode*> order = impl.Preorder(t->root.get());
      for (TreeNode* n : order) {
        auto& vec = n->state_refs;
        for (std::size_t k = 0; k < vec.size(); ++k) {
          if (vec[k].ns == ns && vec[k].object_id == object_id) {
            vec[k].generation += 1;
            vec[k].freshness_ts = std::uint64_t{0};  // stale, timestamped or not
          }
        }
      }
    }
  }
  return Result<void>::Ok();
}

// ---- eviction ----
Result<std::vector<PrefixId>> PrefixIndex::Evict(std::size_t target) {
  if (target == 0) return Result<std::vector<PrefixId>>::Ok({});
  return Result<std::vector<PrefixId>>::Ok(impl_->EvictLeaves(target, nullptr));
}

// ---- counters ----
std::uint64_t PrefixIndex::PrefixCount() const { return impl_->g_prefixes_.load(); }
std::uint64_t PrefixIndex::NodeCount() const { return impl_->g_nodes_.load(); }
std::uint64_t PrefixIndex::SegmentCount() const { return impl_->g_segments_.load(); }
std::uint64_t PrefixIndex::ReferenceCount() const { return impl_->g_refs_.load(); }
std::uint64_t PrefixIndex::TokenFootprintBytes() const { return impl_->g_footprint_.load(); }
std::uint64_t PrefixIndex::PrefixTokens() const { return impl_->g_tokens_.load(); }

// ---- statistics ----
Result<PrefixStatistics> PrefixIndex::Statistics(const PrefixId& id) {
  auto& impl = *impl_;
  IdEntry loc;
  if (!impl.FindIdEntry(id, loc)) return Result<PrefixStatistics>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto& sh = impl.ShardAt(loc.shard);
  std::lock_guard<std::mutex> lk(sh.mtx);
  Trie* t = impl.FindTree(sh, loc.key);
  if (!t) return Result<PrefixStatistics>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  auto it = t->by_id.find(id);
  if (it == t->by_id.end()) return Result<PrefixStatistics>::Err(Error{ErrorCode::kNotFound, "no such prefix"});
  TreeNode* n = it->second;
  PrefixStatistics st = n->stats;
  st.branch_fanout = n->children.size();
  st.descendant_count = static_cast<std::uint64_t>(n->descendant_count);
  st.attached_state_count = n->state_refs.size();
  std::uint64_t avail = 0;
  for (auto& r : n->state_refs) {
    bool stale = r.freshness_ts && *r.freshness_ts == 0;
    if (!stale && (r.compatibility.IsEmpty() || r.compatibility == t->compat)) ++avail;
  }
  st.attached_state_available = avail;
  return Result<PrefixStatistics>::Ok(st);
}

// ---- list / snapshot ----
Result<std::vector<PrefixRecord>> PrefixIndex::ListAll() {
  auto& impl = *impl_;
  std::vector<std::unique_lock<std::mutex>> locks;
  for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(impl.shards_[s].mtx);
  std::vector<PrefixRecord> out;
  for (std::size_t s = 0; s < kNumShards; ++s) {
    auto& sh = impl.shards_[s];
    std::vector<Trie*> ts;
    for (auto& t : sh.trees) ts.push_back(t.get());
    std::sort(ts.begin(), ts.end(), [](const Trie* a, const Trie* b) { return a->key < b->key; });
    for (Trie* t : ts) {
      std::vector<TreeNode*> order = impl.Preorder(t->root.get());
      for (TreeNode* n : order) {
        if (n->token_count == 0 || !n->active) continue;
        out.push_back(impl.MaterializeRecord(t, n).value_or_throw());
      }
    }
  }
  return Result<std::vector<PrefixRecord>>::Ok(std::move(out));
}

PrefixSnapshot PrefixIndex::Snapshot(const CompatibilityKey* compat_filter) {
  auto& impl = *impl_;
  PrefixSnapshot snap;
  auto all = ListAll();
  snap.captured_at_ns = static_cast<std::int64_t>(impl.access_clock_.load());
  if (compat_filter) snap.compat = *compat_filter;
  for (auto& r : all.value_or(std::vector<PrefixRecord>{})) {
    if (compat_filter && !(r.compat == *compat_filter)) continue;
    snap.records.push_back(r);
    snap.prefix_count += 1;
    snap.segment_count += static_cast<std::uint64_t>(r.segments.size());
    snap.token_count += r.token_count;
    snap.state_reference_count += static_cast<std::uint64_t>(r.state_refs.size());
  }
  return snap;
}

std::string PrefixIndex::DebugDump() const {
  auto& impl = *impl_;
  std::ostringstream os;
  // Deterministic order: shards, then trees by key.
  for (std::size_t s = 0; s < kNumShards; ++s) {
    auto& sh = impl.shards_[s];
    std::lock_guard<std::mutex> lk(sh.mtx);
    std::vector<const Trie*> ts;
    for (auto& t : sh.trees) ts.push_back(t.get());
    std::sort(ts.begin(), ts.end(), [](const Trie* a, const Trie* b) { return a->key < b->key; });
    for (const Trie* t : ts) {
      os << "tree[" << s << "] compat=" << t->compat.IdentityHex() << " pk=" << t->prefixes << " nodes=" << t->nodes << "\n";
      std::function<void(const TreeNode*, int)> rec = [&](const TreeNode* n, int depth) {
        os << std::string(static_cast<std::size_t>(depth) * 2, ' ') << "| " << n->id.ToString() << " len=" << n->token_count
           << (n->active ? "" : " [inactive]") << " edge=[" << TokensText(n->edge) << "]\n";
        for (auto& c : n->children) rec(c.get(), depth + 1);
      };
      rec(t->root.get(), 1);
    }
  }
  return os.str();
}


// ---- serialization ----
std::string PrefixIndex::SerializeBinary() const {
  auto& impl = *impl_;
  BinaryWriter body;
  body.RawBytes("PFSX", 4);
  body.U32(1);  // container version
  // payload
  BinaryWriter pay;
  pay.U32(1);  // payload format
  // One consistent view: every shard is held while the index is written, and the
  // trees are ordered by compatibility identity across the whole index.
  std::vector<std::unique_lock<std::mutex>> locks;
  for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(impl.shards_[s].mtx);
  std::vector<const Trie*> ts;
  for (auto& sh : impl.shards_) for (auto& t : sh.trees) ts.push_back(t.get());
  std::sort(ts.begin(), ts.end(), [](const Trie* a, const Trie* b) { return a->key < b->key; });
  pay.U64(static_cast<std::uint64_t>(ts.size()));
  for (const Trie* t : ts) {
    WCompat(pay, t->compat);
    std::vector<TreeNode*> order = impl.Preorder(t->root.get());
    pay.U64(static_cast<std::uint64_t>(order.size()));
    std::unordered_map<TreeNode*, std::uint64_t> idx;
    for (std::uint64_t k = 0; k < order.size(); ++k) idx[order[k]] = k;
    for (TreeNode* n : order) {
      std::uint64_t parent_idx = 0xFFFFFFFFFFFFFFFFULL;
      if (n->parent) parent_idx = idx[n->parent];
      pay.U64(parent_idx);
      pay.U64(static_cast<std::uint64_t>(n->edge.size()));
      for (TokenId tok : n->edge) pay.U32(tok);
      pay.U64(n->id.high());
      pay.U64(n->id.low());
      pay.RawBytes(n->content_hash.data(), n->content_hash.size());
      pay.U64(n->token_count);
      pay.U64(n->generation);
      pay.U8(n->active ? 1 : 0);
      pay.U32(n->depth);
      pay.Bytes(n->provenance);
      pay.U64(static_cast<std::uint64_t>(n->segments.size()));
      for (auto& sg : n->segments) WSeg(pay, sg);
      pay.U64(static_cast<std::uint64_t>(n->state_refs.size()));
      for (auto& rf : n->state_refs) WRef(pay, rf);
    }
  }
  locks.clear();
  std::string payload = pay.take();
  body.RawBytes(payload);
  std::string body_s = body.take();
  ContentHash chk = Sha256::HashBytes(body_s.data(), body_s.size());
  std::string out = body_s;
  out.append(reinterpret_cast<const char*>(chk.data()), chk.size());
  return out;
}
Result<std::size_t> PrefixIndex::DeserializeBinary(std::string_view bytes,
                                                   std::string* error_detail) {
  auto setErr = [&](const std::string& m) { if (error_detail) *error_detail = m; };
  auto corrupt = [&](const char* m) {
    setErr(m);
    return Result<std::size_t>::Err(Error{ErrorCode::kCorruption, m});
  };
  if (bytes.size() < 8 + 32) return corrupt("blob too small");
  if (bytes.substr(0, 4) != std::string_view("PFSX", 4)) return corrupt("bad magic");
  std::string_view body = bytes.substr(0, bytes.size() - 32);
  std::string_view stored = bytes.substr(bytes.size() - 32, 32);
  ContentHash computed = Sha256::HashBytes(body.data(), body.size());
  if (std::memcmp(computed.data(), stored.data(), 32) != 0) {
    setErr("checksum mismatch (corruption)");
    return Result<std::size_t>::Err(Error{ErrorCode::kCorruption, "checksum mismatch"});
  }
  BinaryReader r(body.substr(4));
  auto container = r.U32();
  if (!container || *container != 1) return corrupt("unsupported container version");
  auto ver = r.U32();
  if (!ver || *ver != 1) { setErr("unsupported payload version"); return Result<std::size_t>::Err(Error{ErrorCode::kCorruption, "unsupported version"}); }
  auto tree_count = r.U64();
  if (!tree_count) return corrupt("missing tree count");
  if (*tree_count > 1000000) { setErr("tree count too large"); return Result<std::size_t>::Err(Error{ErrorCode::kInvalidArgument, "tree count too large"}); }

  // The new state is built on the side and swapped in only when the whole blob is
  // valid, so a blob that is refused leaves the live index exactly as it was.
  PrefixIndex staged(impl_->cfg);
  Impl& st = *staged.impl_;
  const CapacityPolicy& cap = st.cfg.capacity;
  for (std::uint64_t ti = 0; ti < *tree_count; ++ti) {
    auto compat = RCompat(r);
    if (!compat) return corrupt("compat parse");
    const std::string key = compat->IdentityHex();
    std::size_t shard = st.KeyShard(key);
    auto& sh = st.shards_[shard];
    if (st.FindTree(sh, key)) return corrupt("duplicate compatibility tree");
    Trie* t = st.GetOrCreateTree(sh, *compat);
    auto node_count = r.U64();
    if (!node_count) return corrupt("missing node count");
    if (*node_count == 0) return corrupt("tree without a root");
    if (*node_count - 1 > cap.max_nodes_total) {
      setErr("node count exceeds capacity"); return Result<std::size_t>::Err(Error{ErrorCode::kInvalidArgument, "node count exceeds capacity"});
    }
    std::vector<TreeNode*> nodes;
    nodes.reserve(static_cast<std::size_t>(*node_count));
    std::vector<TreeNode*> chain;  // the previous node and its ancestors, root first
    TokenSequence path;            // the tokens spelled by the node being read
    for (std::uint64_t ni = 0; ni < *node_count; ++ni) {
      auto parent_idx = r.U64();
      if (!parent_idx) return corrupt("parent index");
      auto ecnt = r.U64();
      if (!ecnt) return corrupt("edge count");
      if (*ecnt > (1u << 24)) return corrupt("edge count implausible");
      TreeNode n;
      n.edge.reserve(static_cast<std::size_t>(*ecnt));
      for (std::uint64_t e = 0; e < *ecnt; ++e) {
        auto tok = r.U32();
        if (!tok) return corrupt("token");
        n.edge.push_back(*tok);
      }
      auto hi = r.U64(); auto lo = r.U64();
      if (!hi || !lo) return corrupt("id");
      n.id = PrefixId(*hi, *lo);
      auto h = r.ReadRaw(32);
      if (!h || h->size() != 32) return corrupt("hash");
      std::copy(h->begin(), h->end(), n.content_hash.begin());
      auto cnt = r.U64(); if (!cnt) return corrupt("count");
      n.token_count = *cnt;
      auto gen = r.U64(); if (!gen) return corrupt("generation");
      n.generation = *gen;
      auto act = r.U8(); if (!act) return corrupt("active");
      n.active = *act != 0;
      auto dep = r.U32(); if (!dep) return corrupt("depth");
      auto prov = r.Str(1u << 20); if (!prov) return corrupt("provenance");
      n.provenance = *prov;
      auto s_cnt = r.U64();
      if (!s_cnt || *s_cnt > 1000000) return corrupt("segment count");
      for (std::uint64_t si = 0; si < *s_cnt; ++si) { auto sg = RSeg(r); if (!sg) return corrupt("segment"); n.segments.push_back(std::move(*sg)); }
      auto rf_cnt = r.U64();
      if (!rf_cnt || *rf_cnt > 1000000) return corrupt("ref count");
      for (std::uint64_t ri = 0; ri < *rf_cnt; ++ri) { auto rf = RRef(r); if (!rf) return corrupt("ref"); n.state_refs.push_back(std::move(*rf)); }
      // What a node says about itself must follow from where it sits: a blob that
      // was written for another namespace or compatibility, or whose structure
      // does not hold together, is refused instead of being trusted.
      for (const auto& sg : n.segments)
        if (sg.token_offset > n.token_count || sg.token_count > n.token_count - sg.token_offset)
          return corrupt("segment outside its prefix");
      if (ni == 0) {
        if (*parent_idx != 0xFFFFFFFFFFFFFFFFULL || !n.edge.empty() || n.token_count != 0 ||
            n.id != t->root->id || n.content_hash != t->root->content_hash)
          return corrupt("root does not match this namespace and compatibility");
        t->root->generation = n.generation; t->root->active = n.active;
        t->root->provenance = n.provenance;
        t->root->segments = std::move(n.segments); t->root->state_refs = std::move(n.state_refs);
        nodes.push_back(t->root.get());
        chain.assign(1, t->root.get());
      } else {
        if (*parent_idx == 0xFFFFFFFFFFFFFFFFULL) return corrupt("orphan node");
        if (*parent_idx >= nodes.size()) return corrupt("bad parent index");
        TreeNode* parent = nodes[static_cast<std::size_t>(*parent_idx)];
        while (!chain.empty() && chain.back() != parent) chain.pop_back();
        if (chain.empty()) return corrupt("nodes are not in preorder");
        if (n.edge.empty()) return corrupt("empty edge");
        path.resize(static_cast<std::size_t>(parent->token_count));
        path.insert(path.end(), n.edge.begin(), n.edge.end());
        if (n.token_count != path.size()) return corrupt("token count does not match the path");
        const ContentHash want = ComputePrefixContentHash(st.cfg.ns, t->compat, path);
        if (n.content_hash != want || n.id != PrefixId::FromContentHash(want))
          return corrupt("identity does not match the content");
        auto& kids = parent->children;
        auto pos = std::lower_bound(kids.begin(), kids.end(), n.edge,
            [](const std::unique_ptr<TreeNode>& a, const TokenSequence& e) { return a->edge < e; });
        if ((pos != kids.end() && (*pos)->edge.front() == n.edge.front()) ||
            (pos != kids.begin() && (*(pos - 1))->edge.front() == n.edge.front()))
          return corrupt("two edges of one node start with the same token");
        auto up = std::make_unique<TreeNode>(std::move(n));
        TreeNode* raw = up.get();
        raw->parent = parent;
        raw->depth = parent->depth + 1;
        raw->full_spell.reset();
        st.InsertChildSorted(parent, std::move(up));
        nodes.push_back(raw);
        chain.push_back(raw);
      }
    }
    st.RecomputeTrie(shard, t);
  }
  st.RebuildGlobalCounters();
  if (!st.cfg.eviction.auto_evict &&
      (st.g_prefixes_.load() > cap.max_prefixes || st.g_nodes_.load() > cap.max_nodes_total ||
       st.g_segments_.load() > cap.max_segments || st.g_refs_.load() > cap.max_state_references ||
       st.g_tokens_.load() > cap.max_token_index_footprint / 4u)) {
    setErr("persisted index exceeds the configured capacity");
    return Result<std::size_t>::Err(Error{ErrorCode::kCapacityExceeded, "persisted index exceeds the configured capacity"});
  }

  Impl& live = *impl_;
  {
    std::vector<std::unique_lock<std::mutex>> locks;
    for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(live.shards_[s].mtx);
    for (std::size_t s = 0; s < kNumShards; ++s) {
      live.shards_[s].trees = std::move(st.shards_[s].trees);
      live.shards_[s].by_key = std::move(st.shards_[s].by_key);
    }
    {
      std::unique_lock<std::shared_mutex> lk(live.id_mtx_);
      live.id_index_ = std::move(st.id_index_);
    }
    live.g_prefixes_ = st.g_prefixes_.load(); live.g_segments_ = st.g_segments_.load();
    live.g_refs_ = st.g_refs_.load(); live.g_nodes_ = st.g_nodes_.load();
    live.g_tokens_ = st.g_tokens_.load(); live.g_footprint_ = st.g_footprint_.load();
    live.total_invalidations_ = 0; live.total_evictions_ = 0;
  }
  if (live.cfg.eviction.auto_evict && live.g_prefixes_.load() > cap.max_prefixes)
    live.EvictLeaves(static_cast<std::size_t>(live.g_prefixes_.load() - cap.max_prefixes), nullptr);
  return Result<std::size_t>::Ok(live.g_prefixes_.load());
}
void PrefixIndex::Clear() {
  auto& impl = *impl_;
  std::vector<std::unique_lock<std::mutex>> locks;
  for (std::size_t s = 0; s < kNumShards; ++s) locks.emplace_back(impl.shards_[s].mtx);
  for (auto& sh : impl.shards_) {
    sh.trees.clear();
    sh.by_key.clear();
  }
  {
    std::unique_lock<std::shared_mutex> lk(impl.id_mtx_);
    impl.id_index_.clear();
  }
  impl.g_prefixes_ = 0; impl.g_segments_ = 0; impl.g_refs_ = 0;
  impl.g_nodes_ = 0; impl.g_tokens_ = 0; impl.g_footprint_ = 0;
  impl.total_invalidations_ = 0; impl.total_evictions_ = 0;
}

}  // namespace prefix_fabric
