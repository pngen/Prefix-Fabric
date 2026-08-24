// Prefix Fabric - distributed node implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/node.hpp"

#include <random>
#include <sstream>

#include "prefix_fabric/net.hpp"
#include "prefix_fabric/protocol.hpp"

namespace prefix_fabric {

namespace {
std::string RandomBootId() {
  std::random_device rd;
  std::ostringstream os;
  for (int i = 0; i < 16; ++i) {
    unsigned char b = static_cast<unsigned char>(rd());
    static const char* h = "0123456789abcdef";
    os << h[(b >> 4) & 0xf] << h[b & 0xf];
  }
  return os.str();
}
}  // namespace

struct PrefixNode::Impl {
  Options opts;
  PrefixStore local;
  std::unique_ptr<PrefixClient> client;
  std::string boot_id;
  bool connected = false;
  Impl(const Options& o) : opts(o), local(PrefixStore::Options{o.ns, o.default_compat, o.segmentation, o.capacity, o.eviction}) {}
};

PrefixNode::PrefixNode(const Options& options) : impl_(std::make_unique<Impl>(options)) {}
PrefixNode::~PrefixNode() { Stop(); }

Result<void> PrefixNode::Start() {
  Impl* I = impl_.get();
  if (I->connected) return Result<void>::Ok();
  if (!NetInit()) return Result<void>::Err(Error{ErrorCode::kNetworkError, "winsock init"});
  if (I->boot_id.empty()) I->boot_id = I->opts.boot_id.empty() ? RandomBootId() : I->opts.boot_id;
  PrefixClient::Options copts;
  copts.host = I->opts.coordinator_host;
  copts.port = I->opts.coordinator_port;
  I->client = std::make_unique<PrefixClient>(copts);
  auto r = I->client->Connect(I->boot_id, I->opts.node_name);
  if (r.is_err()) return r;
  I->connected = true;
  return Result<void>::Ok();
}

void PrefixNode::Stop() { if (impl_->connected) { impl_->client->Close(); impl_->connected = false; } }
bool PrefixNode::Connected() const { return impl_->connected; }
const std::string& PrefixNode::BootId() const { return impl_->boot_id; }
std::uint64_t PrefixNode::Epoch() const { return impl_->client ? impl_->client->Epoch() : 0; }
PrefixStore& PrefixNode::Local() { return impl_->local; }

Result<PrefixRecord> PrefixNode::Publish(const CompatibilityKey& compat, const TokenSequence& tokens,
                                         const std::vector<StateReference>& refs, const std::string& provenance) {
  auto local_rec = impl_->local.Add(compat, tokens, refs, provenance);
  if (local_rec.is_err()) return local_rec;
  auto remote = impl_->client->Publish(compat, tokens, refs, provenance);
  if (remote.is_err()) return Result<PrefixRecord>::Err(Error{ErrorCode::kNetworkError, remote.error().message()});
  return local_rec;
}

Result<PrefixMatch> PrefixNode::RemoteLongestMatch(const TokenSequence& request, const CompatibilityKey& compat) {
  return impl_->client ? impl_->client->LongestMatch(request, compat) : Result<PrefixMatch>::Err(Error{ErrorCode::kNetworkError, "not connected"});
}
Result<PrefixMatch> PrefixNode::RemoteExactMatch(const TokenSequence& request, const CompatibilityKey& compat) {
  return impl_->client ? impl_->client->ExactMatch(request, compat) : Result<PrefixMatch>::Err(Error{ErrorCode::kNetworkError, "not connected"});
}
Result<void> PrefixNode::RemoteInvalidate(const PrefixId& id, bool subtree) {
  return impl_->client ? impl_->client->Invalidate(id, subtree) : Result<void>::Err(Error{ErrorCode::kNetworkError, "not connected"});
}
Result<std::vector<StateReference>> PrefixNode::RemoteAttach(const PrefixId& id, const StateReference& ref) {
  return impl_->client ? impl_->client->AttachState(id, ref) : Result<std::vector<StateReference>>::Err(Error{ErrorCode::kNetworkError, "not connected"});
}
Result<void> PrefixNode::Heartbeat() { return impl_->client ? impl_->client->Heartbeat() : Result<void>::Err(Error{ErrorCode::kNetworkError, "not connected"}); }
Result<PrefixRecord> PrefixNode::RemoteGet(const PrefixId& id) { return impl_->client ? impl_->client->Get(id) : Result<PrefixRecord>::Err(Error{ErrorCode::kNetworkError, "not connected"}); }

}  // namespace prefix_fabric