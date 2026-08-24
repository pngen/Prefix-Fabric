// Prefix Fabric - compatibility key implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/compatibility.hpp"

#include <algorithm>
#include <sstream>

namespace prefix_fabric {

bool CompatibilityKey::operator==(const CompatibilityKey& o) const {
  return model == o.model && tokenizer == o.tokenizer &&
         tokenization_version == o.tokenization_version &&
         vocabulary == o.vocabulary &&
         special_token_policy == o.special_token_policy &&
         prompt_template == o.prompt_template &&
         position_encoding == o.position_encoding &&
         kv_layout == o.kv_layout && runtime_format == o.runtime_format &&
         extra == o.extra;
}

bool CompatibilityKey::operator<(const CompatibilityKey& o) const {
  if (model != o.model) return model < o.model;
  if (tokenizer != o.tokenizer) return tokenizer < o.tokenizer;
  if (tokenization_version != o.tokenization_version) return tokenization_version < o.tokenization_version;
  if (vocabulary != o.vocabulary) return vocabulary < o.vocabulary;
  if (special_token_policy != o.special_token_policy) return special_token_policy < o.special_token_policy;
  if (prompt_template != o.prompt_template) return prompt_template < o.prompt_template;
  if (position_encoding != o.position_encoding) return position_encoding < o.position_encoding;
  if (kv_layout != o.kv_layout) return kv_layout < o.kv_layout;
  if (runtime_format != o.runtime_format) return runtime_format < o.runtime_format;
  return extra < o.extra;
}

std::string CompatibilityKey::Identity() const {
  // Deterministic field order; extra pairs sorted by key.
  std::ostringstream os;
  os << "model=" << model
     << "|tokenizer=" << tokenizer
     << "|tokver=" << tokenization_version
     << "|vocab=" << vocabulary
     << "|special=" << special_token_policy
     << "|prompt=" << prompt_template
     << "|posenc=" << position_encoding
     << "|kv=" << kv_layout
     << "|runtime=" << runtime_format;
  if (!extra.empty()) {
    std::vector<std::pair<std::string, std::string>> sorted = extra;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
      os << "|x" << i << "=" << sorted[i].first << ":" << sorted[i].second;
    }
  }
  return os.str();
}

ContentHash CompatibilityKey::IdentityHash() const {
  std::string id = Identity();
  return Sha256::HashBytes(id.data(), id.size());
}

}  // namespace prefix_fabric
