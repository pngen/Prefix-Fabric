// Prefix Fabric - atomic durable file persistence helpers.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>

#include "prefix_fabric/error.hpp"

namespace pf_internal {

using prefix_fabric::Result;
using prefix_fabric::Error;
using prefix_fabric::ErrorCode;

// Read an entire file into `out`. A file larger than `max_bytes` is refused
// before anything is allocated for it.
Result<void> ReadFile(const std::string& path, std::string& out, std::uint64_t max_bytes);

// Atomically write bytes to `path` via temp-file + flush + rename.
Result<void> WriteFileAtomic(const std::string& path, const std::string& data);

}  // namespace pf_internal
