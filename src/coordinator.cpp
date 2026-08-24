// Prefix Fabric - coordinator implementation (authoritative directory).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/coordinator.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "prefix_fabric/net.hpp"
#include "prefix_fabric/protocol.hpp"

namespace prefix_fabric {
using proto::MT_ERROR;

struct PrefixCoordinator::Impl {
  Options opts;
  PrefixStore store;
  std::uint64_t epoch = 0;
  TcpListener listener;
  std::atomic<bool> stop{false};
  std::thread accept_thread;
  std::mutex conn_mtx;
  std::vector<SOCKET> live_conns;
  std::mutex nodes_mtx;
  std::map<std::string, std::string> nodes;
  std::map<std::string, std::int64_t> last_seen;
  std::vector<std::thread> handlers;
  std::uint64_t max_frame = 32 * 1024 * 1024;

  explicit Impl(const Options& o) : opts(o), store(PrefixStore::Options{o.ns, o.default_compat, o.segmentation, o.capacity, o.eviction}) {}

  bool IsRegistered(const std::string& boot_id) {
    std::lock_guard<std::mutex> lk(nodes_mtx);
    return nodes.count(boot_id) != 0;
  }
  void Touch(const std::string& boot_id) {
    std::lock_guard<std::mutex> lk(nodes_mtx);
    last_seen[boot_id] = std::chrono::steady_clock::now().time_since_epoch().count();
  }
};

PrefixCoordinator::PrefixCoordinator(const Options& options) : impl_(std::make_unique<Impl>(options)) {}
PrefixCoordinator::~PrefixCoordinator() { Stop(); }

Result<std::uint16_t> PrefixCoordinator::Start() {
  if (!NetInit()) return Result<std::uint16_t>::Err(Error{ErrorCode::kNetworkError, "winsock init"});
  Impl* I = impl_.get();
  std::random_device rd;
  std::uint64_t r = (std::uint64_t(rd()) << 32) ^ rd();
  I->epoch = r | 1;
  if (!I->opts.persistence_path.empty()) I->store.LoadFromFile(I->opts.persistence_path);
  auto port = I->listener.Listen(I->opts.bind_host, I->opts.listen_port);
  if (port.is_err()) return Result<std::uint16_t>::Err(port.error());
  I->stop = false;
  I->accept_thread = std::thread([this]() {
    Impl* I = this->impl_.get();
    for (;;) {
      if (I->stop.load()) break;
      auto conn = I->listener.Accept();
      if (conn.is_err()) { if (I->stop.load()) break; continue; }
      TcpConnection c = std::move(conn.value());
      SOCKET s = c.NativeHandle();
      { std::lock_guard<std::mutex> lk(I->conn_mtx); I->live_conns.push_back(s); }
      auto* I2 = I;
      I->handlers.emplace_back([I2, c = std::move(c)]() mutable {
        TcpConnection conn = std::move(c);
        for (;;) {
          if (I2->stop.load()) break;
          auto frame = conn.RecvFrame(I2->max_frame);
          if (frame.is_err()) break;
          auto d = proto::Decode(frame.value());
          if (d.is_err()) break;
          std::uint32_t type = d.value().first;
          std::string body = std::move(d.value().second);
          std::string reply;
          switch (type) {
            case proto::MT_REGISTER: {
              auto rb = proto::ParseRegister(body);
              if (rb.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kProtocolError, "bad register")); }
              else {
                { std::lock_guard<std::mutex> lk(I2->nodes_mtx); I2->nodes[rb.value().boot_id] = rb.value().node_name; }
                I2->Touch(rb.value().boot_id);
                reply = proto::Encode(proto::MT_REGISTER, proto::BuildRegisterOk(I2->epoch));
              }
              break;
            }
            case proto::MT_HEARTBEAT: {
              auto bid = proto::ParseBootId(body);
              if (bid.is_err() || !I2->IsRegistered(bid.value())) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kStaleAuthority, "stale boot id")); }
              else { I2->Touch(bid.value()); reply = proto::Encode(proto::MT_HEARTBEAT, proto::BuildOk()); }
              break;
            }
            case proto::MT_PUBLISH: {
              auto p = proto::ParsePublish(body);
              if (p.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kProtocolError, "bad publish")); }
              else if (!I2->IsRegistered(p.value().boot_id)) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kStaleAuthority, "stale boot id")); }
              else {
                auto rr = I2->store.Add(p.value().compat, p.value().tokens, p.value().refs, p.value().provenance);
                if (rr.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(rr.error().code(), rr.error().message().c_str())); }
                else { reply = proto::Encode(proto::MT_PUBLISH, proto::BuildPublishOk(rr.value().id, rr.value().generation)); }
              }
              break;
            }
            case proto::MT_LOOKUP_LONGEST:
            case proto::MT_LOOKUP_EXACT: {
              auto l = proto::ParseLookup(body);
              if (l.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kProtocolError, "bad lookup")); }
              else if (!I2->IsRegistered(l.value().boot_id)) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kStaleAuthority, "stale boot id")); }
              else {
                PrefixMatch m = (type == proto::MT_LOOKUP_EXACT) ? I2->store.ExactMatch(l.value().request, &l.value().compat) : I2->store.LongestPrefixMatch(l.value().request, &l.value().compat);
                std::vector<StateReference> refs;
                if (m.is_matched()) { auto rec = I2->store.Get(m.id()); if (rec.is_ok()) refs = rec.value().state_refs; }
                reply = proto::Encode(proto::MT_LOOKUP_LONGEST, proto::BuildLookupOk(m.is_matched(), m.id(), m.matched_token_count(), m.exact(), std::move(refs)));
              }
              break;
            }
            case proto::MT_INVALIDATE: {
              auto ib = proto::ParseInvalidate(body);
              if (ib.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kProtocolError, "bad inv")); }
              else if (!I2->IsRegistered(ib.value().boot_id)) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kStaleAuthority, "stale boot id")); }
              else { auto rr = I2->store.Invalidate(ib.value().id, ib.value().subtree); if (rr.is_err()) reply = proto::Encode(MT_ERROR, proto::BuildError(rr.error().code(), rr.error().message().c_str())); else reply = proto::Encode(proto::MT_INVALIDATE, proto::BuildOk()); }
              break;
            }
            case proto::MT_ATTACH_STATE: {
              auto ab = proto::ParseAttach(body);
              if (ab.is_err()) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kProtocolError, "bad attach")); }
              else if (!I2->IsRegistered(ab.value().boot_id)) { reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kStaleAuthority, "stale boot id")); }
              else { auto rr = I2->store.AttachState(ab.value().id, ab.value().ref); if (rr.is_err()) reply = proto::Encode(MT_ERROR, proto::BuildError(rr.error().code(), rr.error().message().c_str())); else reply = proto::Encode(proto::MT_ATTACH_STATE, proto::BuildOk()); }
              break;
            }
            default: {
              reply = proto::Encode(MT_ERROR, proto::BuildError(ErrorCode::kNotSupported, "unknown op"));
              break;
            }
          }
          if (conn.SendFrame(reply).is_err()) break;
        }
        conn.Close();
        { std::lock_guard<std::mutex> lk(I2->conn_mtx);
          for (std::size_t i = 0; i < I2->live_conns.size(); ++i) if (I2->live_conns[i] == conn.NativeHandle()) { I2->live_conns.erase(I2->live_conns.begin() + static_cast<std::ptrdiff_t>(i)); break; }
        }
      });
    }
  });
  return port;
}

void PrefixCoordinator::Stop() {
  Impl* I = impl_.get();
  I->stop = true;
  I->listener.Close();
  if (I->accept_thread.joinable()) I->accept_thread.join();
  {
    std::lock_guard<std::mutex> lk(I->conn_mtx);
    for (SOCKET s : I->live_conns) closesocket(s);
    I->live_conns.clear();
  }
  for (auto& h : I->handlers) if (h.joinable()) h.join();
  I->handlers.clear();
}

bool PrefixCoordinator::Running() const { return !impl_->stop.load(); }
std::uint64_t PrefixCoordinator::Epoch() const { return impl_->epoch; }
std::size_t PrefixCoordinator::NodeCount() const {
  std::lock_guard<std::mutex> lk(impl_->nodes_mtx);
  return impl_->nodes.size();
}
Result<RuntimeStatistics> PrefixCoordinator::Stats() const { return impl_->store.RuntimeStats(); }
PrefixStore& PrefixCoordinator::Store() { return impl_->store; }

}  // namespace prefix_fabric