// Prefix Fabric - store facade implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/store.hpp"

#include <algorithm>
#include "persist.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace prefix_fabric {

namespace {
const CompatibilityKey& Pick(const CompatibilityKey& dflt,
                             const CompatibilityKey* c) {
  return c ? *c : dflt;
}
bool RefUsable(const StateReference& r, const CompatibilityKey& compat) {
  bool stale = r.freshness_ts && *r.freshness_ts == 0;
  if (stale) return false;
  return r.compatibility.IsEmpty() || r.compatibility == compat;
}
}  // namespace

struct PrefixStore::Impl {
  Options opts;
  IndexConfig icfg;
  std::unique_ptr<PrefixIndex> index;

  // Event hub.
  struct Hub {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<PrefixEvent> queue;
    std::vector<PrefixSubscription> subs;
    std::uint64_t next_sub = 1;
    std::uint64_t seq = 0;
    std::uint64_t max_backlog = 100000;
    bool stop = false;
    bool started = false;
  };
  std::unique_ptr<Hub> hub;
  std::thread dispatch_thread;

  // Runtime aggregate statistics (observability only).
  std::atomic<std::uint64_t> total_lookups{0}, total_hits{0}, total_exact{0},
      total_partial{0}, total_reused_tokens{0}, total_attachments{0},
      total_retirements{0};

  explicit Impl(const Options& o) : opts(o) {
    icfg.ns = o.ns;
    icfg.default_compat = o.default_compat;
    icfg.segmentation = o.segmentation;
    icfg.capacity = o.capacity;
    icfg.eviction = o.eviction;
    index = std::make_unique<PrefixIndex>(icfg);
    hub = std::make_unique<Hub>();
    hub->max_backlog = o.max_event_backlog ? o.max_event_backlog : o.capacity.max_event_backlog;
  }

  void Emit(EventType type, const PrefixId& id, std::string detail) {
    if (!hub) return;
    std::lock_guard<std::mutex> lk(hub->mtx);
    PrefixEvent ev;
    ev.type = type;
    ev.sequence = ++hub->seq;
    ev.timestamp_ns = static_cast<std::int64_t>(hub->seq);
    ev.id = id;
    ev.detail = std::move(detail);
    if (hub->queue.size() >= hub->max_backlog) hub->queue.pop_front();
    hub->queue.push_back(std::move(ev));
    hub->cv.notify_one();
  }

  void StartDispatch() {
    if (hub->started) return;
    hub->started = true;
    dispatch_thread = std::thread([this] {
      for (;;) {
        std::vector<PrefixEvent> batch;
        std::vector<PrefixSubscription> subs;
        {
          std::unique_lock<std::mutex> lk(hub->mtx);
          hub->cv.wait(lk, [this] { return hub->stop || !hub->queue.empty(); });
          if (hub->stop && hub->queue.empty()) break;
          while (!hub->queue.empty()) { batch.push_back(hub->queue.front()); hub->queue.pop_front(); }
          subs = hub->subs;
        }
        for (const auto& ev : batch) {
          for (const auto& s : subs) {
            if (!s.filter.empty() &&
                std::find(s.filter.begin(), s.filter.end(), ev.type) == s.filter.end())
              continue;
            if (s.callback) {
              try { s.callback(ev); } catch (...) { /* callback failure must not corrupt runtime */ }
            }
          }
        }
      }
    });
  }

  void StopDispatch() {
    if (!hub || !hub->started) return;
    {
      std::lock_guard<std::mutex> lk(hub->mtx);
      hub->stop = true;
    }
    hub->cv.notify_all();
    if (dispatch_thread.joinable()) dispatch_thread.join();
    hub->started = false;
  }
};

PrefixStore::PrefixStore(const Options& options) : impl_(std::make_unique<Impl>(options)) {
  impl_->StartDispatch();
}
PrefixStore::~PrefixStore() {
  if (impl_) impl_->StopDispatch();
}

const Namespace& PrefixStore::NamespaceName() const { return impl_->index->NamespaceName(); }
const CapacityPolicy& PrefixStore::Capacity() const { return impl_->index->Capacity(); }
const PrefixIndex& PrefixStore::Index() const { return *impl_->index; }
PrefixIndex& PrefixStore::Index() { return *impl_->index; }

Result<PrefixRecord> PrefixStore::Add(const CompatibilityKey& compat,
                                      const TokenSequence& tokens,
                                      const std::vector<StateReference>& refs,
                                      const std::string& provenance) {
  auto r = impl_->index->Insert(compat, tokens, refs, provenance);
  if (r.is_ok()) {
    impl_->total_attachments += static_cast<std::uint64_t>(refs.size());
    impl_->Emit(EventType::kPrefixAdded, r.value().id, "tokens=" + std::to_string(tokens.size()));
    for (auto& s : r.value().segments) impl_->Emit(EventType::kSegmentCreated, s.id, "off=" + std::to_string(s.token_offset));
  }
  return r;
}

PrefixMatch PrefixStore::LongestPrefixMatch(const TokenSequence& request,
                                            const CompatibilityKey* compat) {
  const CompatibilityKey& c = Pick(impl_->opts.default_compat, compat);
  PrefixMatch m = impl_->index->LongestPrefixMatch(c, request);
  impl_->total_lookups += 1;
  if (m.is_matched()) {
    impl_->total_hits += 1;
    impl_->total_reused_tokens += m.matched_token_count();
    if (m.exact()) { impl_->total_exact += 1; impl_->Emit(EventType::kExactHit, m.id(), "tok=" + std::to_string(m.matched_token_count())); }
    else { impl_->total_partial += 1; impl_->Emit(EventType::kLongestPrefixHit, m.id(), "tok=" + std::to_string(m.matched_token_count())); }
    impl_->Emit(EventType::kPrefixMatched, m.id(), "tok=" + std::to_string(m.matched_token_count()));
  } else {
    impl_->Emit(EventType::kMiss, PrefixId{}, "hash=" + c.IdentityHex());
  }
  return m;
}

PrefixMatch PrefixStore::ExactMatch(const TokenSequence& request,
                                    const CompatibilityKey* compat) {
  const CompatibilityKey& c = Pick(impl_->opts.default_compat, compat);
  PrefixMatch m = impl_->index->ExactMatch(c, request);
  impl_->total_lookups += 1;
  if (m.is_matched()) { impl_->total_hits += 1; impl_->total_exact += 1; impl_->total_reused_tokens += m.matched_token_count(); }
  else impl_->Emit(EventType::kMiss, PrefixId{}, "exact");
  return m;
}

std::vector<PrefixMatch> PrefixStore::AllMatches(const TokenSequence& request,
                                                 const CompatibilityKey* compat) {
  const CompatibilityKey& c = Pick(impl_->opts.default_compat, compat);
  std::vector<PrefixMatch> v = impl_->index->AllMatches(c, request);
  impl_->total_lookups += 1;
  if (!v.empty()) { impl_->total_hits += 1; impl_->total_reused_tokens += v.back().matched_token_count(); }
  return v;
}

PrefixReusePlan PrefixStore::BuildReusePlan(const TokenSequence& request,
                                            const CompatibilityKey* compat,
                                            std::uint64_t* common_prefix_len) {
  const CompatibilityKey& c = Pick(impl_->opts.default_compat, compat);
  PrefixReusePlan plan;
  plan.request_token_count = request.size();
  plan.compatibility_reason = c.IdentityHex() + (c.IsOverPermissive() ? " (over-permissive)" : "");
  PrefixMatch m = impl_->index->LongestPrefixMatch(c, request);
  if (!m.is_matched()) {
    plan.confidence = 0.0f;
    plan.matched = false;
    if (common_prefix_len) *common_prefix_len = 0;
    // candidates: any match at all (0) => none.
    return plan;
  }
  plan.matched = true;
  plan.matched_id = m.id();
  plan.matched_token_count = m.matched_token_count();
  plan.matched_prefix = m.matched_prefix();
  plan.unmatched_suffix = m.unmatched_suffix();
  plan.exact_match = m.exact();
  if (common_prefix_len) *common_prefix_len = m.matched_token_count();
  plan.confidence = request.empty() ? 0.0f : static_cast<float>(m.matched_token_count()) / static_cast<float>(request.size());
  auto rec = impl_->index->Get(m.id());
  if (rec.is_ok()) {
    for (auto& ref : rec.value().state_refs) {
      bool stale = ref.freshness_ts && *ref.freshness_ts == 0;
      if (stale) plan.stale_refs.push_back(ref);
      else if (RefUsable(ref, c)) plan.usable_refs.push_back(ref);
      else plan.invalid_refs.push_back(ref);
    }
  }
  // Structural path.
  auto pth = impl_->index->Path(m.id());
  if (pth.is_ok()) {
    for (auto& e : pth.value().edge_hex) { if (!plan.structural_path.empty()) plan.structural_path += "/"; plan.structural_path += e; }
  }
  // Candidates: all matches; the chosen longest is accepted.
  auto all = impl_->index->AllMatches(c, request);
  for (auto& am : all) {
    PrefixCandidate cand;
    cand.id = am.id();
    cand.token_count = am.matched_token_count();
    cand.common_prefix_length = am.matched_token_count();
    cand.compat = c;
    auto r2 = impl_->index->Get(am.id());
    if (r2.is_ok()) for (auto& ref : r2.value().state_refs) if (RefUsable(ref, c)) cand.refs.push_back(ref);
    bool chosen = (am.id() == m.id());
    cand.accepted = chosen || !cand.refs.empty();
    if (!cand.accepted) cand.rejection_reason = "shorter than chosen prefix or no usable attached state";
    plan.candidates.push_back(std::move(cand));
  }
  if (all.empty()) plan.rejected_reasons.push_back("no reusable prefix in this compatibility tree");
  impl_->total_lookups += 1;
  if (plan.matched) impl_->total_hits += 1;
  return plan;
}

std::uint64_t PrefixStore::CommonPrefixLength(const TokenSequence& a,
                                              const TokenSequence& b,
                                              const CompatibilityKey* compat) {
  const CompatibilityKey& c = Pick(impl_->opts.default_compat, compat);
  return impl_->index->CommonPrefixLength(c, a, b);
}

Result<PrefixRecord> PrefixStore::Get(const PrefixId& id) { return impl_->index->Get(id); }
Result<PrefixPath> PrefixStore::Path(const PrefixId& id) { return impl_->index->Path(id); }
Result<std::vector<PrefixRecord>> PrefixStore::Ancestors(const PrefixId& id) { return impl_->index->Ancestors(id); }
Result<std::vector<PrefixRecord>> PrefixStore::Descendants(const PrefixId& id) { return impl_->index->Descendants(id); }
Result<std::vector<PrefixRecord>> PrefixStore::Children(const PrefixId& id) { return impl_->index->Children(id); }
Result<std::vector<PrefixRecord>> PrefixStore::BranchPoints(const PrefixId& id) { return impl_->index->BranchPoints(id); }
Result<std::vector<PrefixRecord>> PrefixStore::CommonAncestors(const PrefixId& a, const PrefixId& b) { return impl_->index->CommonAncestors(a, b); }
Result<std::vector<PrefixSegment>> PrefixStore::Segments(const PrefixId& id) { return impl_->index->Segments(id); }
Result<PrefixStatistics> PrefixStore::Statistics(const PrefixId& id) { return impl_->index->Statistics(id); }

Result<PrefixRecord> PrefixStore::Slice(const PrefixId& id, std::uint64_t n) { return impl_->index->Slice(id, n); }
Result<PrefixRecord> PrefixStore::Append(const PrefixId& id, const TokenSequence& suffix) { return impl_->index->Append(id, suffix); }
Result<PrefixRecord> PrefixStore::Compose(const CompatibilityKey& compat, const std::vector<PrefixId>& pieces) {
  auto r = impl_->index->Compose(compat, pieces);
  if (r.is_ok()) impl_->Emit(EventType::kBranchCreated, r.value().id, "compose");
  return r;
}
Result<PrefixRecord> PrefixStore::Fork(const PrefixId& id, const TokenSequence& suffix) {
  auto r = impl_->index->Fork(id, suffix);
  if (r.is_ok()) impl_->Emit(EventType::kBranchCreated, r.value().id, "fork");
  return r;
}

Result<std::vector<StateReference>> PrefixStore::AttachState(const PrefixId& id,
                                                             const StateReference& ref) {
  auto r = impl_->index->AttachState(id, ref);
  if (r.is_ok()) { ++impl_->total_attachments; impl_->Emit(EventType::kStateReferenceAttached, id, ref.ns + "/" + ref.object_id); }
  return r;
}
Result<void> PrefixStore::RetireState(const PrefixId& id, const StateReference& ref) {
  auto r = impl_->index->RetireState(id, ref);
  if (r.is_ok()) { ++impl_->total_retirements; impl_->Emit(EventType::kStateReferenceRetired, id, ref.ns + "/" + ref.object_id); }
  return r;
}
Result<void> PrefixStore::InvalidateStateByObject(const std::string& ns, const std::string& object_id) {
  return impl_->index->InvalidateStateByObject(ns, object_id);
}

Result<void> PrefixStore::Invalidate(const PrefixId& id, bool subtree) {
  auto r = impl_->index->Invalidate(id, subtree);
  if (r.is_ok()) impl_->Emit(EventType::kInvalidation, id, subtree ? "subtree" : "single");
  return r;
}
Result<void> PrefixStore::InvalidateByCompat(const CompatibilityKey& compat) {
  auto r = impl_->index->InvalidateByCompat(compat);
  if (r.is_ok()) impl_->Emit(EventType::kInvalidation, PrefixId{}, "compat=" + compat.IdentityHex());
  return r;
}
Result<std::size_t> PrefixStore::Remove(const PrefixId& id, bool cascade) {
  auto r = impl_->index->Remove(id, cascade);
  if (r.is_ok()) impl_->Emit(EventType::kEviction, id, "removed=" + std::to_string(r.value()));
  return r;
}
Result<std::vector<PrefixId>> PrefixStore::Evict(std::size_t target_count) {
  auto r = impl_->index->Evict(target_count);
  return r;
}

std::uint64_t PrefixStore::PrefixCount() const { return impl_->index->PrefixCount(); }
std::uint64_t PrefixStore::NodeCount() const { return impl_->index->NodeCount(); }
std::uint64_t PrefixStore::SegmentCount() const { return impl_->index->SegmentCount(); }
std::uint64_t PrefixStore::ReferenceCount() const { return impl_->index->ReferenceCount(); }
std::uint64_t PrefixStore::TokenFootprintBytes() const { return impl_->index->TokenFootprintBytes(); }
std::uint64_t PrefixStore::PrefixTokens() const { return impl_->index->PrefixTokens(); }

RuntimeStatistics PrefixStore::RuntimeStats() const {
  RuntimeStatistics st;
  st.prefix_count = impl_->index->PrefixCount();
  st.segment_count = impl_->index->SegmentCount();
  st.node_count = impl_->index->NodeCount();
  st.state_reference_count = impl_->index->ReferenceCount();
  st.token_index_footprint_bytes = impl_->index->TokenFootprintBytes();
  st.total_lookups = impl_->total_lookups.load();
  st.total_hits = impl_->total_hits.load();
  st.total_exact_hits = impl_->total_exact.load();
  st.total_partial_hits = impl_->total_partial.load();
  st.total_reused_tokens = impl_->total_reused_tokens.load();
  st.total_reference_attachments = impl_->total_attachments.load();
  st.total_reference_retirements = impl_->total_retirements.load();
  return st;
}

Result<std::vector<PrefixRecord>> PrefixStore::ListAll() { return impl_->index->ListAll(); }
PrefixSnapshot PrefixStore::Snapshot(const CompatibilityKey* compat_filter) { return impl_->index->Snapshot(compat_filter); }
std::string PrefixStore::DebugDump() const { return impl_->index->DebugDump(); }

Result<void> PrefixStore::SaveToFile(const std::string& path) {
  // Serialize the index and write atomically. Implementation in persist.cpp is
  // wired via PrefixIndex::SerializeBinary here for now.
  std::string blob = impl_->index->SerializeBinary();
  auto r = pf_internal::WriteFileAtomic(path, blob);
  return r;
}
Result<std::size_t> PrefixStore::LoadFromFile(const std::string& path) {
  std::string blob;
  auto r = pf_internal::ReadFile(path, blob);
  if (r.is_err()) return Result<std::size_t>::Err(r.error());
  std::string detail;
  auto res = impl_->index->DeserializeBinary(blob, &detail);
  return res;
}

// --- events ---
Result<PrefixStore::SubscriptionHandle> PrefixStore::Subscribe(
    std::function<void(const PrefixEvent&)> cb, std::vector<EventType> filter) {
  if (!cb) return Result<SubscriptionHandle>::Err(Error{ErrorCode::kInvalidArgument, "null callback"});
  auto& h = impl_->hub;
  std::lock_guard<std::mutex> lk(h->mtx);
  if (h->subs.size() >= impl_->opts.capacity.max_subscriptions)
    return Result<SubscriptionHandle>::Err(Error{ErrorCode::kCapacityExceeded, "subscription limit"});
  PrefixSubscription s;
  s.id = h->next_sub++;
  s.callback = std::move(cb);
  s.filter = std::move(filter);
  h->subs.push_back(s);
  return Result<SubscriptionHandle>::Ok(SubscriptionHandle{s.id});
}
void PrefixStore::Unsubscribe(std::uint64_t id) {
  auto& h = impl_->hub;
  std::lock_guard<std::mutex> lk(h->mtx);
  for (std::size_t i = 0; i < h->subs.size(); ++i)
    if (h->subs[i].id == id) { h->subs.erase(h->subs.begin() + static_cast<std::ptrdiff_t>(i)); break; }
}
void PrefixStore::DrainEvents() {
  auto& h = impl_->hub;
  for (;;) {
    std::vector<PrefixEvent> batch;
    std::vector<PrefixSubscription> subs;
    {
      std::lock_guard<std::mutex> lk(h->mtx);
      while (!h->queue.empty()) { batch.push_back(h->queue.front()); h->queue.pop_front(); }
      subs = h->subs;
    }
    if (batch.empty()) return;
    for (auto& ev : batch)
      for (auto& s : subs) {
        if (!s.filter.empty() && std::find(s.filter.begin(), s.filter.end(), ev.type) == s.filter.end()) continue;
        if (s.callback) { try { s.callback(ev); } catch (...) {} }
      }
  }
}

}  // namespace prefix_fabric