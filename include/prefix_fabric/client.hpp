// Prefix Fabric - remote prefix directory client.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "prefix_fabric/error.hpp"
#include "prefix_fabric/id.hpp"
#include "prefix_fabric/match.hpp"
#include "prefix_fabric/record.hpp"
#include "prefix_fabric/state_reference.hpp"

namespace prefix_fabric {

// A framed TCP client to a coordinator. All operations carry the client boot id
// so the coordinator can fence stale authorities.
class PrefixClient {
 public:
  struct Options { std::string host = "127.0.0.1"; std::uint16_t port = 0; int timeout_ms = 5000; };
  explicit PrefixClient(const Options& options);
  ~PrefixClient();
  PrefixClient(const PrefixClient&) = delete;
  PrefixClient& operator=(const PrefixClient&) = delete;

  Result<void> Connect(const std::string& boot_id, const std::string& node_name = "client");
  void Close();
  bool Connected() const;
  std::uint64_t Epoch() const;

  Result<PrefixRecord> Publish(const CompatibilityKey& compat, const TokenSequence& tokens,
                               const std::vector<StateReference>& refs = {}, const std::string& provenance = {});
  Result<PrefixMatch> LongestMatch(const TokenSequence& request, const CompatibilityKey& compat);
  Result<PrefixMatch> ExactMatch(const TokenSequence& request, const CompatibilityKey& compat);
  Result<void> Invalidate(const PrefixId& id, bool subtree);
  Result<std::vector<StateReference>> AttachState(const PrefixId& id, const StateReference& ref);
  Result<void> Heartbeat();
  Result<PrefixRecord> Get(const PrefixId& id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace prefix_fabric
