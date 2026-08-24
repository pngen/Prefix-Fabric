// Prefix Fabric - minimal test harness.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>
#include <functional>

namespace pftest {

struct TestCase { std::string name; std::function<void()> fn; };
inline std::vector<TestCase>& Registry() { static std::vector<TestCase> r; return r; }
inline int& Failures() { static int f = 0; return f; }
inline int& Checks() { static int c = 0; return c; }
inline int& CurrentFailures() { static int f = 0; return f; }

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { Registry().push_back({name, fn}); }
};

inline const char* BoolStr(bool b) { return b ? "OK" : "FAIL"; }

class Reporter {
 public:
  static Reporter& I() { static Reporter r; return r; }
  void Check(bool cond, const char* expr, const char* file, int line) {
    ++Checks();
    if (!cond) { ++Failures(); ++CurrentFailures(); std::printf("    [FAIL] %s (%s:%d)\n", expr, file, line); }
  }
  void CheckEq64(std::uint64_t a, std::uint64_t b, const char* ea, const char* eb, const char* file, int line) {
    ++Checks();
    if (a != b) { ++Failures(); ++CurrentFailures(); std::printf("    [FAIL] %s=%llu != %s=%llu (%s:%d)\n", ea, (unsigned long long)a, eb, (unsigned long long)b, file, line); }
  }
};

int RunAll() {
  int passed = 0;
  for (auto& tc : Registry()) {
    CurrentFailures() = 0;
    std::printf("RUN %s\n", tc.name.c_str());
    bool threw = false;
    try { tc.fn(); } catch (const std::exception& e) { ++Failures(); ++CurrentFailures(); std::printf("    [THREW] %s\n", e.what()); threw = true; }
    catch (...) { ++Failures(); ++CurrentFailures(); std::printf("    [THREW] unknown\n"); threw = true; }
    if (CurrentFailures() == 0) { ++passed; std::printf("  PASS\n"); } else { std::printf("  FAIL (%d)\n", CurrentFailures()); }
  }
  std::printf("\n=== %d/%zu tests passed, %d checks, %d failures ===\n", passed, Registry().size(), Checks(), Failures());
  return Failures() == 0 ? 0 : 1;
}

}  // namespace pftest

#define PF_TEST(name) \
  static void pf_test_##name(); \
  static pftest::Registrar pf_reg_##name(#name, pf_test_##name); \
  static void pf_test_##name()

#define CHECK(cond) ::pftest::Reporter::I().Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::pftest::Reporter::I().CheckEq64(static_cast<std::uint64_t>(a), static_cast<std::uint64_t>(b), #a, #b, __FILE__, __LINE__)

// A small helper to build a token sequence from an initializer list / vector.
