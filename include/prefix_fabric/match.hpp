// Prefix Fabric - prefix matches, candidates, and reuse plans.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "prefix_fabric/id.hpp"
#include "prefix_fabric/state_reference.hpp"
#include "prefix_fabric/types.hpp"

namespace prefix_fabric {

// A candidate prefix considered for reuse, plus an explicit accept/reject reason.
struct PrefixCandidate {
  PrefixId id;
  std::uint64_t token_count = 0;
  std::uint64_t common_prefix_length = 0;
  CompatibilityKey compat;
  std::vector<StateReference> refs;
  bool accepted = false;
  std::string rejection_reason;  // empty iff accepted
};

// An advisory reuse plan. Prefix Fabric never executes inference; this is the
// description of where reusable prefix structure exists and which external state
// may be referenced.
struct PrefixReusePlan {
  bool matched = false;
  PrefixId matched_id;
  std::uint64_t matched_token_count = 0;
  std::uint64_t request_token_count = 0;
  TokenSequence matched_prefix;      // the exact token prefix matched
  TokenSequence unmatched_suffix;    // remaining request tokens after the match
  bool exact_match = false;          // true iff the whole request was matched
  std::vector<StateReference> usable_refs;  // attached state that is not stale
  std::vector<StateReference> stale_refs;   // attached state that is stale
  std::vector<StateReference> invalid_refs;// attached state whose compat is invalid
  std::string compatibility_reason;   // the compatibility identity used
  float confidence = 0.0f;            // reused_token_count / request_token_count
  std::vector<PrefixCandidate> candidates;
  std::string structural_path;        // hex path from root
  std::vector<std::string> rejected_reasons;  // why candidates were rejected

  bool HasUsableRefs() const { return !usable_refs.empty(); }
};

// A bare matching result (used for exact / longest-prefix primitives).
class PrefixMatch {
 public:
  PrefixMatch() : matched_(false) {}
  PrefixMatch(bool m) : matched_(m) {}
  static PrefixMatch None() { return PrefixMatch(false); }

  bool is_matched() const { return matched_; }
  bool exact() const { return exact_; }
  const PrefixId& id() const { return id_; }
  void set_id(PrefixId id) { id_ = id; matched_ = true; }
  std::uint64_t matched_token_count() const { return matched_count_; }
  void set_matched_count(std::uint64_t c) { matched_count_ = c; }

  const TokenSequence& matched_prefix() const { return matched_prefix_; }
  const TokenSequence& unmatched_suffix() const { return unmatched_suffix_; }
  void set_exact(bool e) { exact_ = e; }
  void set_match(const TokenSequence& mp, const TokenSequence& us) {
    matched_prefix_ = mp;
    unmatched_suffix_ = us;
    matched_ = true;
    matched_count_ = mp.size();
  }
  void set_prefix(PrefixId id, std::uint64_t matched, const TokenSequence& mp,
                  const TokenSequence& us, bool exact) {
    id_ = id; matched_count_ = matched; matched_prefix_ = mp;
    unmatched_suffix_ = us; exact_ = exact; matched_ = true;
  }

 private:
  bool matched_ = false;
  bool exact_ = false;
  PrefixId id_;
  std::uint64_t matched_count_ = 0;
  TokenSequence matched_prefix_;
  TokenSequence unmatched_suffix_;
};

}  // namespace prefix_fabric
