// Prefix Fabric - framed wire protocol implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/protocol.hpp"

#include "binary.hpp"

using pf_internal::BinaryReader;
using pf_internal::BinaryWriter;

namespace prefix_fabric {
namespace proto {

namespace {
void WCompat(BinaryWriter& w, const CompatibilityKey& c) {
  w.Bytes(c.model); w.Bytes(c.tokenizer); w.Bytes(c.tokenization_version);
  w.Bytes(c.vocabulary); w.Bytes(c.special_token_policy); w.Bytes(c.prompt_template);
  w.Bytes(c.position_encoding); w.Bytes(c.kv_layout); w.Bytes(c.runtime_format);
  w.U64(static_cast<std::uint64_t>(c.extra.size()));
  for (const auto& kv : c.extra) { w.Bytes(kv.first); w.Bytes(kv.second); }
}
Result<CompatibilityKey> RCompat(BinaryReader& r) {
  CompatibilityKey c;
  auto m = r.Str(1u<<20); if (!m) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"model"}); c.model=*m;
  auto tk = r.Str(1u<<20); if (!tk) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"tok"}); c.tokenizer=*tk;
  auto tv = r.Str(1u<<20); if (!tv) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"tv"}); c.tokenization_version=*tv;
  auto vb = r.Str(1u<<20); if (!vb) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"vb"}); c.vocabulary=*vb;
  auto sp = r.Str(1u<<20); if (!sp) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"sp"}); c.special_token_policy=*sp;
  auto pt = r.Str(1u<<20); if (!pt) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"pt"}); c.prompt_template=*pt;
  auto pe = r.Str(1u<<20); if (!pe) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"pe"}); c.position_encoding=*pe;
  auto kv = r.Str(1u<<20); if (!kv) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"kv"}); c.kv_layout=*kv;
  auto rf = r.Str(1u<<20); if (!rf) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"rf"}); c.runtime_format=*rf;
  auto en = r.U64(); if (!en || *en > 100000) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"extra"});
  for (std::uint64_t i=0;i<*en;++i){ auto k=r.Str(1u<<20); if(!k) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"ek"}); auto v=r.Str(1u<<20); if(!v) return Result<CompatibilityKey>::Err(Error{ErrorCode::kCorruption,"ev"}); c.extra.emplace_back(*k,*v); }
  return Result<CompatibilityKey>::Ok(std::move(c));
}
void WRef(BinaryWriter& w, const StateReference& ref) {
  w.Bytes(ref.ns); w.Bytes(ref.object_id); w.U64(ref.generation);
  w.U32(static_cast<std::uint32_t>(ref.type)); WCompat(w, ref.compatibility);
  w.Bytes(ref.location_hint); w.U8(ref.freshness_ts?1:0);
  if (ref.freshness_ts) w.U64(*ref.freshness_ts);
  w.Bytes(ref.integrity_token);
}
Result<StateReference> RRef(BinaryReader& r) {
  StateReference ref;
  auto ns = r.Str(1u<<20); if (!ns) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"ns"}); ref.ns=*ns;
  auto oid = r.Str(1u<<20); if (!oid) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"oid"}); ref.object_id=*oid;
  auto gen = r.U64(); if (!gen) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"g"}); ref.generation=*gen;
  auto ty = r.U32(); if (!ty) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"t"}); ref.type=static_cast<StateReference::Type>(*ty);
  auto cp = RCompat(r); if (!cp) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"c"}); ref.compatibility=*cp;
  auto lh = r.Str(1u<<20); if (!lh) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"lh"}); ref.location_hint=*lh;
  auto hf = r.U8(); if (!hf) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"hf"});
  if (*hf) { auto f=r.U64(); if (!f) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"f"}); ref.freshness_ts=*f; }
  auto itk = r.Str(1u<<20); if (!itk) return Result<StateReference>::Err(Error{ErrorCode::kCorruption,"it"}); ref.integrity_token=*itk;
  return Result<StateReference>::Ok(std::move(ref));
}
}  // namespace

std::string Encode(std::uint32_t type, const std::string& body) {
  std::string out;
  out.reserve(4 + body.size());
  out.push_back(static_cast<char>(type & 0xffu));
  out.push_back(static_cast<char>((type >> 8) & 0xffu));
  out.push_back(static_cast<char>((type >> 16) & 0xffu));
  out.push_back(static_cast<char>((type >> 24) & 0xffu));
  out.append(body);
  return out;
}

Result<std::pair<std::uint32_t, std::string>> Decode(std::string_view frame) {
  if (frame.size() < 4) return Result<std::pair<std::uint32_t, std::string>>::Err(Error{ErrorCode::kProtocolError, "frame too small"});
  std::uint32_t type = static_cast<std::uint32_t>(std::uint8_t(frame[0])) | (static_cast<std::uint32_t>(std::uint8_t(frame[1])) << 8) |
                       (static_cast<std::uint32_t>(std::uint8_t(frame[2])) << 16) | (static_cast<std::uint32_t>(std::uint8_t(frame[3])) << 24);
  std::string body(frame.substr(4));
  return Result<std::pair<std::uint32_t, std::string>>::Ok(std::make_pair(type, std::move(body)));
}

std::string BuildRegister(const std::string& boot_id, const std::string& node_name) {
  BinaryWriter w; w.Bytes(boot_id); w.Bytes(node_name); return w.take();
}
std::string BuildHeartbeat(const std::string& boot_id) { BinaryWriter w; w.Bytes(boot_id); return w.take(); }
std::string BuildPublish(const std::string& boot_id, const CompatibilityKey& compat,
                         const TokenSequence& tokens, const std::vector<StateReference>& refs,
                         const std::string& provenance) {
  BinaryWriter w; w.Bytes(boot_id); WCompat(w, compat);
  w.U64(static_cast<std::uint64_t>(tokens.size()));
  for (TokenId t : tokens) w.U32(t);
  w.U64(static_cast<std::uint64_t>(refs.size()));
  for (const auto& rf : refs) WRef(w, rf);
  w.Bytes(provenance);
  return w.take();
}
std::string BuildLookup(const std::string& boot_id, const CompatibilityKey& compat,
                        const TokenSequence& request, bool exact) {
  BinaryWriter w; w.Bytes(boot_id); WCompat(w, compat);
  w.U64(static_cast<std::uint64_t>(request.size()));
  for (TokenId t : request) w.U32(t);
  w.U8(exact ? 1 : 0);
  return w.take();
}
std::string BuildInvalidate(const std::string& boot_id, const PrefixId& id, bool subtree) {
  BinaryWriter w; w.Bytes(boot_id); w.U64(id.high()); w.U64(id.low()); w.U8(subtree?1:0); return w.take();
}
std::string BuildAttach(const std::string& boot_id, const PrefixId& id, const StateReference& ref) {
  BinaryWriter w; w.Bytes(boot_id); w.U64(id.high()); w.U64(id.low()); WRef(w, ref); return w.take();
}
std::string BuildGet(const std::string& boot_id, const PrefixId& id) {
  BinaryWriter w; w.Bytes(boot_id); w.U64(id.high()); w.U64(id.low()); return w.take();
}
std::string BuildList(const std::string& boot_id) { BinaryWriter w; w.Bytes(boot_id); return w.take(); }
std::string BuildStats(const std::string& boot_id) { BinaryWriter w; w.Bytes(boot_id); return w.take(); }

Result<RegisterBody> ParseRegister(std::string_view body) {
  BinaryReader r(body);
  auto b = r.Str(1u<<20); if (!b) return Result<RegisterBody>::Err(Error{ErrorCode::kCorruption,"reg boot"});
  auto n = r.Str(1u<<20); if (!n) return Result<RegisterBody>::Err(Error{ErrorCode::kCorruption,"reg name"});
  return Result<RegisterBody>::Ok(RegisterBody{*b, *n});
}
Result<std::string> ParseBootId(std::string_view body) {
  BinaryReader r(body); auto b = r.Str(1u<<20); if (!b) return Result<std::string>::Err(Error{ErrorCode::kCorruption,"boot"}); return *b;
}
Result<PublishBody> ParsePublish(std::string_view body) {
  BinaryReader r(body);
  PublishBody p;
  auto b = r.Str(1u<<20); if (!b) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"pub boot"}); p.boot_id=*b;
  auto c = RCompat(r); if (!c) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"pub compat"}); p.compat=*c;
  auto n = r.U64(); if (!n || *n > (1u<<24)) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"pub count"});
  p.tokens.reserve(static_cast<std::size_t>(*n));
  for (std::uint64_t i=0;i<*n;++i){ auto t=r.U32(); if(!t) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"tok"}); p.tokens.push_back(*t); }
  auto rn = r.U64(); if (!rn || *rn > (1u<<20)) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"ref count"});
  for (std::uint64_t i=0;i<*rn;++i){ auto rf=RRef(r); if(!rf) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"ref"}); p.refs.push_back(std::move(*rf)); }
  auto pr = r.Str(1u<<20); if (!pr) return Result<PublishBody>::Err(Error{ErrorCode::kCorruption,"provenance"}); p.provenance=*pr;
  return Result<PublishBody>::Ok(std::move(p));
}
Result<LookupBody> ParseLookup(std::string_view body) {
  BinaryReader r(body);
  LookupBody l;
  auto b = r.Str(1u<<20); if (!b) return Result<LookupBody>::Err(Error{ErrorCode::kCorruption,"lk boot"}); l.boot_id=*b;
  auto c = RCompat(r); if (!c) return Result<LookupBody>::Err(Error{ErrorCode::kCorruption,"lk compat"}); l.compat=*c;
  auto n = r.U64(); if (!n || *n > (1u<<24)) return Result<LookupBody>::Err(Error{ErrorCode::kCorruption,"lk count"});
  l.request.reserve(static_cast<std::size_t>(*n));
  for (std::uint64_t i=0;i<*n;++i){ auto t=r.U32(); if(!t) return Result<LookupBody>::Err(Error{ErrorCode::kCorruption,"tok"}); l.request.push_back(*t); }
  auto ex = r.U8(); if (!ex) return Result<LookupBody>::Err(Error{ErrorCode::kCorruption,"exact"});
  return Result<LookupBody>::Ok(std::move(l));
}
Result<InvalidateBody> ParseInvalidate(std::string_view body) {
  BinaryReader r(body);
  auto b = r.Str(1u<<20); if (!b) return Result<InvalidateBody>::Err(Error{ErrorCode::kCorruption,"inv boot"});
  auto hi = r.U64(); if (!hi) return Result<InvalidateBody>::Err(Error{ErrorCode::kCorruption,"hi"});
  auto lo = r.U64(); if (!lo) return Result<InvalidateBody>::Err(Error{ErrorCode::kCorruption,"lo"});
  auto s = r.U8(); if (!s) return Result<InvalidateBody>::Err(Error{ErrorCode::kCorruption,"subtree"});
  return Result<InvalidateBody>::Ok(InvalidateBody{*b, PrefixId(*hi,*lo), *s!=0});
}
Result<AttachBody> ParseAttach(std::string_view body) {
  BinaryReader r(body);
  auto b = r.Str(1u<<20); if (!b) return Result<AttachBody>::Err(Error{ErrorCode::kCorruption,"at boot"});
  auto hi = r.U64(); if (!hi) return Result<AttachBody>::Err(Error{ErrorCode::kCorruption,"hi"});
  auto lo = r.U64(); if (!lo) return Result<AttachBody>::Err(Error{ErrorCode::kCorruption,"lo"});
  auto rf = RRef(r); if (!rf) return Result<AttachBody>::Err(Error{ErrorCode::kCorruption,"ref"});
  return Result<AttachBody>::Ok(AttachBody{*b, PrefixId(*hi,*lo), std::move(*rf)});
}
Result<IdBody> ParseGet(std::string_view body) {
  BinaryReader r(body);
  auto b = r.Str(1u<<20); if (!b) return Result<IdBody>::Err(Error{ErrorCode::kCorruption,"get boot"});
  auto hi = r.U64(); if (!hi) return Result<IdBody>::Err(Error{ErrorCode::kCorruption,"hi"});
  auto lo = r.U64(); if (!lo) return Result<IdBody>::Err(Error{ErrorCode::kCorruption,"lo"});
  return Result<IdBody>::Ok(IdBody{*b, PrefixId(*hi,*lo)});
}

std::string BuildRegisterOk(std::uint64_t epoch) { BinaryWriter w; w.U64(epoch); w.U8(1); return w.take(); }
std::string BuildOk() { BinaryWriter w; w.U8(1); return w.take(); }
std::string BuildError(ErrorCode code, const char* msg) { BinaryWriter w; w.U32(static_cast<std::uint32_t>(code)); w.Bytes(std::string(msg)); return w.take(); }
std::string BuildPublishOk(const PrefixId& id, Generation gen) { BinaryWriter w; w.U8(1); w.U64(id.high()); w.U64(id.low()); w.U64(gen); return w.take(); }
std::string BuildLookupOk(bool matched, const PrefixId& id, std::uint64_t c, bool exact, std::vector<StateReference> refs) {
  BinaryWriter w; w.U8(1); w.U8(matched?1:0); w.U64(id.high()); w.U64(id.low()); w.U64(c); w.U8(exact?1:0);
  w.U64(static_cast<std::uint64_t>(refs.size()));
  for (const auto& rf : refs) WRef(w, rf);
  return w.take();
}

Result<RegisterOk> ParseRegisterOk(std::string_view body) {
  BinaryReader r(body);
  auto e = r.U64(); if (!e) return Result<RegisterOk>::Err(Error{ErrorCode::kCorruption,"epoch"});
  auto ok = r.U8(); if (!ok) return Result<RegisterOk>::Err(Error{ErrorCode::kCorruption,"ok"});
  return Result<RegisterOk>::Ok(RegisterOk{*e, *ok!=0});
}
Result<std::uint64_t> ParseEpoch(std::string_view body) { BinaryReader r(body); auto e=r.U64(); if(!e) return Result<std::uint64_t>::Err(Error{ErrorCode::kCorruption,"epoch"}); return *e; }
Result<LookupReply> ParseLookupOk(std::string_view body) {
  BinaryReader r(body);
  LookupReply lr;
  auto ok = r.U8(); if (!ok) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"ok"}); lr.ok=*ok!=0;
  if (!lr.ok) return Result<LookupReply>::Ok(lr);
  auto m = r.U8(); if (!m) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"m"}); lr.matched=*m!=0;
  auto hi = r.U64(); if (!hi) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"hi"});
  auto lo = r.U64(); if (!lo) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"lo"}); lr.id=PrefixId(*hi,*lo);
  auto c = r.U64(); if (!c) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"c"}); lr.matched_count=*c;
  auto ex = r.U8(); if (!ex) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"ex"}); lr.exact=*ex!=0;
  auto rn = r.U64(); if (!rn || *rn > (1u<<20)) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"refs"});
  for (std::uint64_t i=0;i<*rn;++i){ auto rf=RRef(r); if(!rf) return Result<LookupReply>::Err(Error{ErrorCode::kCorruption,"ref"}); lr.refs.push_back(std::move(*rf)); }
  return Result<LookupReply>::Ok(std::move(lr));
}
Result<ErrorCode> ParseErrorCode(std::string_view body) { BinaryReader r(body); auto c=r.U32(); if(!c) return Result<ErrorCode>::Err(Error{ErrorCode::kCorruption,"ec"}); return static_cast<ErrorCode>(*c); }

}  // namespace proto
}  // namespace prefix_fabric