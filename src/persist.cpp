// Prefix Fabric - atomic persistence (Windows durable file ops).
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "persist.hpp"

#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <filesystem>
#include <fstream>
#endif

namespace pf_internal {

#ifdef _WIN32
static std::wstring ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
#endif

Result<void> ReadFile(const std::string& path, std::string& out) {
#ifdef _WIN32
  HANDLE h = CreateFileW(ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return Result<void>::Err(Error{ErrorCode::kIoError, "create/read file failed"});
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return Result<void>::Err(Error{ErrorCode::kIoError, "size"}); }
  out.resize(static_cast<std::size_t>(sz.QuadPart));
  DWORD total = 0;
  while (total < static_cast<DWORD>(sz.QuadPart)) {
    DWORD chunk = static_cast<DWORD>(sz.QuadPart - total);
    DWORD read = 0;
    if (!::ReadFile(h, out.data() + total, chunk, &read, nullptr)) { CloseHandle(h); return Result<void>::Err(Error{ErrorCode::kIoError, "read"}); }
    if (read == 0) break;
    total += read;
  }
  CloseHandle(h);
  return Result<void>::Ok();
#else
  std::ifstream f(path, std::ios::binary);
  if (!f) return Result<void>::Err(Error{ErrorCode::kIoError, "open"});
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return Result<void>::Ok();
#endif
}

Result<void> WriteFileAtomic(const std::string& path, const std::string& data) {
#ifdef _WIN32
  std::string tmp = path + ".tmp";
  HANDLE h = CreateFileW(ToWide(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return Result<void>::Err(Error{ErrorCode::kIoError, "create temp file failed"});
  DWORD total = 0;
  while (total < static_cast<DWORD>(data.size())) {
    DWORD chunk = static_cast<DWORD>(data.size() - total);
    DWORD written = 0;
    if (!WriteFile(h, data.data() + total, chunk, &written, nullptr)) {
      CloseHandle(h);
      return Result<void>::Err(Error{ErrorCode::kIoError, "write"});
    }
    if (written == 0) { CloseHandle(h); return Result<void>::Err(Error{ErrorCode::kIoError, "short write"}); }
    total += written;
  }
  if (!FlushFileBuffers(h)) { CloseHandle(h); return Result<void>::Err(Error{ErrorCode::kIoError, "flush"}); }
  CloseHandle(h);
  if (!MoveFileExW(ToWide(tmp).c_str(), ToWide(path).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return Result<void>::Err(Error{ErrorCode::kIoError, "atomic rename failed"});
  }
  return Result<void>::Ok();
#else
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return Result<void>::Err(Error{ErrorCode::kIoError, "open tmp"});
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.flush();
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) return Result<void>::Err(Error{ErrorCode::kIoError, ec.message()});
  return Result<void>::Ok();
#endif
}

}  // namespace pf_internal