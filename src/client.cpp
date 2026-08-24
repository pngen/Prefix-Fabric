// Prefix Fabric - remote client implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/client.hpp"

#include "prefix_fabric/net.hpp"
#include "prefix_fabric/protocol.hpp"

#include <cstdint>

namespace {
std::uint64_t read64(const std::string& s, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= std::uint64_t(std::uint8_t(s[off + i])) << (8 * i);
  return v;
}
}  // namespace

namespace prefix_fabric {
using proto::MT_ERROR;

struct PrefixClient::Impl {
  Options opts;
  TcpConnection conn;
  std::string boot_id;
  std::uint64_t epoch = 0;
  bool connected = false;
  std::uint64_t max_frame = 32 * 1024 * 1024;

  Result<std::string> Request(std::uint32_t type, const std::string& body) {
    if (!connected) return Result<std::string>::Err(Error{ErrorCode::kNetworkError, "not connected"});
    auto s = conn.SendFrame(proto::Encode(type, body));
    if (s.is_err()) return Result<std::string>::Err(Error{ErrorCode::kNetworkError, "send request"});
    auto f = conn.RecvFrame(max_frame);
    if (f.is_err()) return Result<std::string>::Err(Error{ErrorCode::kNetworkError, "recv reply"});
    auto d = proto::Decode(f.value());
    if (d.is_err()) return Result<std::string>::Err(Error{ErrorCode::kProtocolError, "bad reply"});
    if (d.value().first == MT_ERROR) {
      auto ec = proto::ParseErrorCode(d.value().second);
      auto msg = d.value().second;
      std::string msg_body = (msg.size() >= 4) ? msg.substr(4) : msg;
      return Result<std::string>::Err(Error{ec.is_ok()? ec.value() : ErrorCode::kProtocolError, msg_body});
    }
    return d.value().second;
  }
};

PrefixClient::PrefixClient(const Options& options) : impl_(std::make_unique<Impl>()) { impl_->opts = options; }
PrefixClient::~PrefixClient() { Close(); }

Result<void> PrefixClient::Connect(const std::string& boot_id, const std::string& node_name) {
  if (impl_->connected) Close();
  if (!NetInit()) return Result<void>::Err(Error{ErrorCode::kNetworkError, "winsock init"});
  impl_->boot_id = boot_id;
  auto r = impl_->conn.Connect(impl_->opts.host, impl_->opts.port, impl_->opts.timeout_ms);
  if (r.is_err()) return r;
  auto body = proto::BuildRegister(boot_id, node_name);
  if (impl_->conn.SendFrame(proto::Encode(proto::MT_REGISTER, body)).is_err()) {
    impl_->conn.Close();
    return Result<void>::Err(Error{ErrorCode::kNetworkError, "send register"});
  }
  auto f = impl_->conn.RecvFrame(impl_->max_frame);
  if (f.is_err()) { impl_->conn.Close(); return Result<void>::Err(Error{ErrorCode::kNetworkError, "recv register"}); }
  auto d = proto::Decode(f.value());
  if (d.is_err()) { impl_->conn.Close(); return Result<void>::Err(Error{ErrorCode::kProtocolError, "bad register"}); }
  if (d.value().first == MT_ERROR) {
    impl_->conn.Close();
    return Result<void>::Err(Error{ErrorCode::kStaleAuthority, "register rejected"});
  }
  auto ro = proto::ParseRegisterOk(d.value().second);
  if (ro.is_err() || !ro.value().ok) { impl_->conn.Close(); return Result<void>::Err(Error{ErrorCode::kNetworkError, "register failed"}); }
  impl_->epoch = ro.value().epoch;
  impl_->connected = true;
  return Result<void>::Ok();
}

void PrefixClient::Close() { if (impl_->connected) { impl_->conn.Close(); impl_->connected = false; } }
bool PrefixClient::Connected() const { return impl_->connected; }
std::uint64_t PrefixClient::Epoch() const { return impl_->epoch; }

Result<PrefixRecord> PrefixClient::Publish(const CompatibilityKey& compat, const TokenSequence& tokens,
                                           const std::vector<StateReference>& refs, const std::string& provenance) {
  auto body = proto::BuildPublish(impl_->boot_id, compat, tokens, refs, provenance);
  auto r = impl_->Request(proto::MT_PUBLISH, body);
  if (r.is_err()) return Result<PrefixRecord>::Err(r.error());
  // reply: [ok u8][id hi][id lo][gen u64]
  const std::string& s = r.value();
  if (s.size() < 25) return Result<PrefixRecord>::Err(Error{ErrorCode::kProtocolError, "short publish reply"});
  PrefixRecord rec;
  rec.id = PrefixId(read64(s, 1), read64(s, 9));
  rec.generation = read64(s, 17);
  rec.compat = compat;
  rec.tokens = tokens;
  rec.token_count = static_cast<std::uint64_t>(tokens.size());
  rec.ns = "";
  return Result<PrefixRecord>::Ok(std::move(rec));
}

Result<PrefixMatch> PrefixClient::LongestMatch(const TokenSequence& request, const CompatibilityKey& compat) {
  auto body = proto::BuildLookup(impl_->boot_id, compat, request, false);
  auto r = impl_->Request(proto::MT_LOOKUP_LONGEST, body);
  if (r.is_err()) return Result<PrefixMatch>::Err(r.error());
  auto lr = proto::ParseLookupOk(r.value());
  if (lr.is_err()) return Result<PrefixMatch>::Err(Error{ErrorCode::kProtocolError, "bad lookup"});
  if (!lr.value().matched) return Result<PrefixMatch>::Ok(PrefixMatch::None());
  PrefixMatch m;
  m.set_prefix(lr.value().id, lr.value().matched_count, TokenSequence{}, TokenSequence{}, lr.value().exact);
  return m;
}
Result<PrefixMatch> PrefixClient::ExactMatch(const TokenSequence& request, const CompatibilityKey& compat) {
  auto body = proto::BuildLookup(impl_->boot_id, compat, request, true);
  auto r = impl_->Request(proto::MT_LOOKUP_EXACT, body);
  if (r.is_err()) return Result<PrefixMatch>::Err(r.error());
  auto lr = proto::ParseLookupOk(r.value());
  if (lr.is_err()) return Result<PrefixMatch>::Err(Error{ErrorCode::kProtocolError, "bad lookup"});
  if (!lr.value().matched) return Result<PrefixMatch>::Ok(PrefixMatch::None());
  PrefixMatch m;
  m.set_prefix(lr.value().id, lr.value().matched_count, TokenSequence{}, TokenSequence{}, lr.value().exact);
  return Result<PrefixMatch>::Ok(m);
}
Result<void> PrefixClient::Invalidate(const PrefixId& id, bool subtree) {
  auto body = proto::BuildInvalidate(impl_->boot_id, id, subtree);
  auto r = impl_->Request(proto::MT_INVALIDATE, body);
  if (r.is_err()) return Result<void>::Err(r.error());
  return Result<void>::Ok();
}
Result<std::vector<StateReference>> PrefixClient::AttachState(const PrefixId& id, const StateReference& ref) {
  auto body = proto::BuildAttach(impl_->boot_id, id, ref);
  auto r = impl_->Request(proto::MT_ATTACH_STATE, body);
  if (r.is_err()) return Result<std::vector<StateReference>>::Err(r.error());
  return Result<std::vector<StateReference>>::Ok(std::vector<StateReference>{});
}
Result<void> PrefixClient::Heartbeat() {
  auto body = proto::BuildHeartbeat(impl_->boot_id);
  auto r = impl_->Request(proto::MT_HEARTBEAT, body);
  if (r.is_err()) return Result<void>::Err(r.error());
  return Result<void>::Ok();
}
Result<PrefixRecord> PrefixClient::Get(const PrefixId& id) {
  auto body = proto::BuildGet(impl_->boot_id, id);
  auto r = impl_->Request(proto::MT_GET, body);
  if (r.is_err()) return Result<PrefixRecord>::Err(r.error());
  return Result<PrefixRecord>::Err(Error{ErrorCode::kNotSupported, "remote get not yet supported"});
}

}  // namespace prefix_fabric