// Prefix Fabric - explicit error results.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <stdexcept>

namespace prefix_fabric {

// Stable error codes. All recoverable runtime errors are surfaced as Result,
// never as abrupt process abort().
enum class ErrorCode : int {
  kOk = 0,
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kOverflow,
  kCompatibilityMismatch,
  kStaleGeneration,
  kStaleAuthority,
  kInvalidState,
  kSerializationError,
  kCorruption,
  kCapacityExceeded,
  kIoError,
  kNetworkError,
  kNetworkTimeout,
  kProtocolError,
  kAmbiguousResult,
  kNotSupported,
  kCycleDetected,
  kCallableFailed,
  kOutOfMemory,
};

inline const char* ToString(ErrorCode c) {
  switch (c) {
    case ErrorCode::kOk: return "ok";
    case ErrorCode::kInvalidArgument: return "invalid argument";
    case ErrorCode::kNotFound: return "not found";
    case ErrorCode::kAlreadyExists: return "already exists";
    case ErrorCode::kOverflow: return "overflow";
    case ErrorCode::kCompatibilityMismatch: return "compatibility mismatch";
    case ErrorCode::kStaleGeneration: return "stale generation";
    case ErrorCode::kStaleAuthority: return "stale authority";
    case ErrorCode::kInvalidState: return "invalid state";
    case ErrorCode::kSerializationError: return "serialization error";
    case ErrorCode::kCorruption: return "corruption";
    case ErrorCode::kCapacityExceeded: return "capacity exceeded";
    case ErrorCode::kIoError: return "io error";
    case ErrorCode::kNetworkError: return "network error";
    case ErrorCode::kNetworkTimeout: return "network timeout";
    case ErrorCode::kProtocolError: return "protocol error";
    case ErrorCode::kAmbiguousResult: return "ambiguous result";
    case ErrorCode::kNotSupported: return "not supported";
    case ErrorCode::kCycleDetected: return "cycle detected";
    case ErrorCode::kCallableFailed: return "callable failed";
    case ErrorCode::kOutOfMemory: return "out of memory";
  }
  return "unknown";
}

// An expected runtime error. Never carries a throwing payload across a C boundary.
class Error {
 public:
  Error() : code_(ErrorCode::kOk) {}
  explicit Error(ErrorCode code, std::string message = {})
      : code_(code), message_(std::move(message)) {}
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }
  std::string ToString() const {
    std::string s = ::prefix_fabric::ToString(code_);
    if (!message_.empty()) { s += ": " + message_; }
    return s;
  }
  bool ok() const { return code_ == ErrorCode::kOk; }
 private:
  ErrorCode code_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result() : storage_(Error{ErrorCode::kInvalidState, "uninitialized result"}) {}
  Result(const T& value) : storage_(value) {}
  Result(T&& value) : storage_(std::move(value)) {}
  Result(const Error& error) : storage_(error) {}
  Result(Error&& error) : storage_(std::move(error)) {}

  static Result Ok(T value) { return Result(std::move(value)); }
  static Result Err(Error error) { return Result(std::move(error)); }

  bool is_ok() const { return std::holds_alternative<T>(storage_); }
  bool is_err() const { return !is_ok(); }
  explicit operator bool() const { return is_ok(); }

  T& value() { return std::get<T>(storage_); }
  const T& value() const { return std::get<T>(storage_); }

  Error error() const { return std::get<Error>(storage_); }

  // value_or returns the value or a default-constructed fallback.
  T value_or(const T& fallback) const { return is_ok() ? value() : fallback; }

  // ok() returns an optional that is empty on error.
  T* ok() { return is_ok() ? &std::get<T>(storage_) : nullptr; }
  const T* ok() const { return is_ok() ? &std::get<T>(storage_) : nullptr; }

  // Optional-like accessors (callers must check is_ok() first; they throw on error).
  T& operator*() { return value(); }
  const T& operator*() const { return value(); }
  T* operator->() { return ok(); }
  const T* operator->() const { return ok(); }

  // It is a programming error to call value()/error() on the wrong side; throw.
  T& value_or_throw() {
    if (!is_ok()) throw std::runtime_error(error().ToString());
    return value();
  }

 private:
  std::variant<T, Error> storage_;
};

// Result<void> specialization: value() is meaningless, is_ok() is what matters.
template <>
class Result<void> {
 public:
  Result() : ok_(true) {}
  Result(Error error) : ok_(false), error_(std::move(error)) {}
  static Result Ok() { return Result(); }
  static Result Err(Error error) { return Result(std::move(error)); }
  bool is_ok() const { return ok_; }
  bool is_err() const { return !ok_; }
  explicit operator bool() const { return ok_; }
  Error error() const { return error_; }
  void value() const {}  // only valid if ok
 private:
  bool ok_;
  Error error_;
};

}  // namespace prefix_fabric
