// Prefix Fabric - framed wire protocol (coordinator <-> nodes).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prefix_fabric/compatibility.hpp"
#include "prefix_fabric/error.hpp"
#include "prefix_fabric/id.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

namespace proto {

enum : std::uint32_t {
  MT_ERROR = 0,
  MT_REGISTER = 1,
  MT_HEARTBEAT = 2,
  MT_PUBLISH = 3,
  MT_LOOKUP_LONGEST = 4,
  MT_LOOKUP_EXACT = 5,
  MT_INVALIDATE = 6,
  MT_ATTACH_STATE = 7,
  MT_RETIRE_STATE = 8,
  MT_GET = 9,
  MT_LIST = 10,
  MT_STATS = 11,
  MT_DISCOVER = 12,
};

// Compose a frame: [type u32][body].
std::string Encode(std::uint32_t type, const std::string& body);
// Parse a frame into (type, body). Returns error on malformed input.
Result<std::pair<std::uint32_t, std::string>> Decode(std::string_view frame);

// --- request body builders ---
std::string BuildRegister(const std::string& boot_id, const std::string& node_name);
std::string BuildHeartbeat(const std::string& boot_id);
std::string BuildPublish(const std::string& boot_id, const CompatibilityKey& compat,
                         const TokenSequence& tokens, const std::vector<StateReference>& refs,
                         const std::string& provenance);
std::string BuildLookup(const std::string& boot_id, const CompatibilityKey& compat,
                        const TokenSequence& request, bool exact);
std::string BuildInvalidate(const std::string& boot_id, const PrefixId& id, bool subtree);
std::string BuildAttach(const std::string& boot_id, const PrefixId& id, const StateReference& ref);
std::string BuildGet(const std::string& boot_id, const PrefixId& id);
std::string BuildList(const std::string& boot_id);
std::string BuildStats(const std::string& boot_id);

// --- request body parsers (validate strictly; bound counts) ---
struct RegisterBody { std::string boot_id; std::string node_name; };
struct PublishBody { std::string boot_id; CompatibilityKey compat; TokenSequence tokens; std::vector<StateReference> refs; std::string provenance; };
struct LookupBody { std::string boot_id; CompatibilityKey compat; TokenSequence request; };
struct InvalidateBody { std::string boot_id; PrefixId id; bool subtree; };
struct AttachBody { std::string boot_id; PrefixId id; StateReference ref; };
struct IdBody { std::string boot_id; PrefixId id; };

Result<RegisterBody> ParseRegister(std::string_view body);
Result<std::string> ParseBootId(std::string_view body);
Result<PublishBody> ParsePublish(std::string_view body);
Result<LookupBody> ParseLookup(std::string_view body);
Result<InvalidateBody> ParseInvalidate(std::string_view body);
Result<AttachBody> ParseAttach(std::string_view body);
Result<IdBody> ParseGet(std::string_view body);

// --- reply builders/parsers ---
std::string BuildRegisterOk(std::uint64_t epoch);
std::string BuildOk();
std::string BuildError(ErrorCode code, const char* msg);
std::string BuildPublishOk(const PrefixId& id, Generation gen);
std::string BuildLookupOk(bool matched, const PrefixId& id, std::uint64_t matched_count,
                          bool exact, std::vector<StateReference> refs);

struct RegisterOk { std::uint64_t epoch; bool ok; };
struct LookupReply { bool ok; bool matched; PrefixId id; std::uint64_t matched_count; bool exact; std::vector<StateReference> refs; };
struct GenericReply { bool ok; ErrorCode code; std::string message; };

Result<RegisterOk> ParseRegisterOk(std::string_view body);
Result<std::uint64_t> ParseEpoch(std::string_view body);
Result<LookupReply> ParseLookupOk(std::string_view body);
Result<ErrorCode> ParseErrorCode(std::string_view body);

}  // namespace proto
}  // namespace prefix_fabric