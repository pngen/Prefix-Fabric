// Prefix Fabric - internal bounds-checked binary reader/writer.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prefix_fabric/error.hpp"

namespace pf_internal {

using prefix_fabric::Result;
using prefix_fabric::Error;
using prefix_fabric::ErrorCode;

// A growable binary writer (little-endian).
class BinaryWriter {
 public:
  void U8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void U32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu)); }
  void U64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xffu)); }
  void Bytes(std::string_view s) { U64(static_cast<std::uint64_t>(s.size())); buf_.append(s.data(), s.size()); }
  void RawBytes(const void* d, std::size_t n) { buf_.append(static_cast<const char*>(d), n); }
  void RawBytes(std::string_view s) { buf_.append(s.data(), s.size()); }
  const std::string& data() const { return buf_; }
  std::string take() { return std::move(buf_); }
  std::size_t size() const { return buf_.size(); }
 private:
  std::string buf_;
};

// A bounds-checked binary reader. Malformed/truncated/out-of-range reads return
// an error (kCorruption / kInvalidArgument) rather than crashing.
class BinaryReader {
 public:
  BinaryReader() : data_(nullptr), size_(0), pos_(0) {}
  explicit BinaryReader(std::string_view b) : data_(b.data()), size_(b.size()), pos_(0) {}

  bool filled() const { return pos_ >= size_; }
  std::size_t remaining() const { return size_ >= pos_ ? size_ - pos_ : 0; }

  template <typename T>
  static Result<T> ErrC(const char* what) {
    return Result<T>::Err(Error{ErrorCode::kCorruption, what});
  }
  template <typename T>
  static Result<T> ErrA(const char* what) {
    return Result<T>::Err(Error{ErrorCode::kInvalidArgument, what});
  }

  Result<std::uint8_t> U8() {
    if (remaining() < 1) return ErrC<std::uint8_t>("U8 truncation");
    return std::uint8_t(data_[pos_++]);
  }
  Result<std::uint32_t> U32() {
    if (remaining() < 4) return ErrC<std::uint32_t>("U32 truncation");
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(std::uint8_t(data_[pos_++])) << (8 * i);
    return v;
  }
  Result<std::uint64_t> U64() {
    if (remaining() < 8) return ErrC<std::uint64_t>("U64 truncation");
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(std::uint8_t(data_[pos_++])) << (8 * i);
    return v;
  }
  Result<std::string> Str(std::uint64_t max_len) {
    auto n = U64();
    if (!n) return ErrC<std::string>("missing string length");
    if (*n > max_len) return ErrA<std::string>("string too large");
    if (remaining() < static_cast<std::size_t>(*n)) return ErrC<std::string>("string truncation");
    std::string s(data_ + pos_, static_cast<std::size_t>(*n));
    pos_ += static_cast<std::size_t>(*n);
    return s;
  }
  Result<std::string_view> Blob(std::uint64_t max_len) {
    auto n = U64();
    if (!n) return ErrC<std::string_view>("missing blob length");
    if (*n > max_len) return ErrA<std::string_view>("blob too large");
    if (remaining() < static_cast<std::size_t>(*n)) return ErrC<std::string_view>("blob truncation");
    std::string_view s(data_ + pos_, static_cast<std::size_t>(*n));
    pos_ += static_cast<std::size_t>(*n);
    return s;
  }
  Result<void> Skip(std::uint64_t n) {
    if (remaining() < static_cast<std::size_t>(n)) return ErrC<void>("skip truncation");
    pos_ += static_cast<std::size_t>(n);
    return Result<void>::Ok();
  }
  // Read exactly n raw bytes (no length prefix).
  Result<std::string_view> ReadRaw(std::size_t n) {
    if (remaining() < n) return ErrC<std::string_view>("raw truncation");
    std::string_view s(data_ + pos_, n);
    pos_ += n;
    return s;
  }
  std::string_view RemainingString() const {
    if (pos_ >= size_) return std::string_view();
    return std::string_view(data_ + pos_, remaining());
  }
  std::size_t position() const { return pos_; }
  void seek(std::size_t p) { pos_ = p; }
  std::size_t size() const { return size_; }

 private:
  const char* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t pos_ = 0;
};

}  // namespace pf_internal
