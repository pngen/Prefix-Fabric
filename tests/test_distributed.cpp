// Prefix Fabric - multiprocess distributed validation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "test_util.hpp"
#include "prefix_fabric/net.hpp"
#include "prefix_fabric/protocol.hpp"

#ifndef PF_CLI_EXE
#define PF_CLI_EXE "prefix-fabric.exe"
#endif

using namespace prefix_fabric;

namespace {

bool LaunchProc(const std::string& cmdline, HANDLE& hproc, HANDLE& rpipe, DWORD timeout_ms, std::string& out) {
  SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE; sa.lpSecurityDescriptor = nullptr;
  HANDLE wpipe = nullptr;
  if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
  SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOA si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wpipe; si.hStdError = wpipe; si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::string cmd = cmdline;
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rpipe); CloseHandle(wpipe); return false;
  }
  CloseHandle(wpipe);
  hproc = pi.hProcess;
  char buf[512]; out.clear();
  DWORD deadline = GetTickCount() + timeout_ms;
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(rpipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD got = 0; ReadFile(rpipe, buf, avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail, &got, nullptr); out.append(buf, got); continue;
    }
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
      DWORD avail2 = 0; while (PeekNamedPipe(rpipe, nullptr, 0, nullptr, &avail2, nullptr) && avail2 > 0) { DWORD got = 0; ReadFile(rpipe, buf, avail2 > sizeof(buf) ? (DWORD)sizeof(buf) : avail2, &got, nullptr); out.append(buf, got); }
      break;
    } else if (GetTickCount() >= deadline) { break; }
    Sleep(5);
  }
  CloseHandle(pi.hThread);
  return true;
}

bool RunSync(const std::string& cmdline, DWORD timeout_ms, std::string& out, DWORD& rc) {
  HANDLE hp = nullptr; HANDLE rp = nullptr;
  if (!LaunchProc(cmdline, hp, rp, timeout_ms, out)) return false;
  WaitForSingleObject(hp, INFINITE);
  GetExitCodeProcess(hp, &rc);
  DWORD avail = 0; char buf[512];
  while (PeekNamedPipe(rp, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) { DWORD got = 0; ReadFile(rp, buf, avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail, &got, nullptr); out.append(buf, got); }
  CloseHandle(rp); CloseHandle(hp);
  return true;
}

const char* CliExe() { return PF_CLI_EXE; }

}  // namespace

PF_TEST(MultiProcess_CoordinatorNodes) {
  std::string cmd = std::string(CliExe()) + " coordinator --port 0";
  HANDLE hcoord = nullptr; HANDLE rp = nullptr; std::string out;
  CHECK(LaunchProc(cmd, hcoord, rp, 8000, out));
  // Find PORT line.
  int port = 0;
  for (;;) {
    DWORD avail = 0; char buf[512];
    if (PeekNamedPipe(rp, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) { DWORD got = 0; ReadFile(rp, buf, avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail, &got, nullptr); out.append(buf, got); }
    std::size_t pos = out.find("PORT ");
    if (pos != std::string::npos) { port = std::stoi(out.substr(pos + 5)); break; }
    if (WaitForSingleObject(hcoord, 0) == WAIT_OBJECT_0) break;
    Sleep(5);
  }
  CHECK(port != 0);
  std::string pubcmd = std::string(CliExe()) + " node --port " + std::to_string(port) + " --publish --tokens \"10 20 30\"";
  std::string o1, o2, o3; DWORD rc;
  CHECK(RunSync(pubcmd, 15000, o1, rc)); CHECK(o1.find("PUBLISHED") != std::string::npos);
  std::string pubcmd2 = std::string(CliExe()) + " node --port " + std::to_string(port) + " --publish --tokens \"10 20 40\"";
  CHECK(RunSync(pubcmd2, 15000, o2, rc)); CHECK(o2.find("PUBLISHED") != std::string::npos);
  // Node3 matches a prefix published by a different node (cross-node discovery).
  std::string mcmd = std::string(CliExe()) + " node --port " + std::to_string(port) + " --tokens \"10 20 30 99\"";
  CHECK(RunSync(mcmd, 15000, o3, rc)); CHECK(o3.find("MATCH") != std::string::npos); CHECK(o3.find("matched_tokens=3") != std::string::npos);
  // Node4 matches an unrelated request -> NO_MATCH.
  std::string m2; std::string mcmd2 = std::string(CliExe()) + " node --port " + std::to_string(port) + " --tokens \"99 98 97\"";
  CHECK(RunSync(mcmd2, 15000, m2, rc)); CHECK(m2.find("NO_MATCH") != std::string::npos);
  // Stale authority: send a publish with a boot id never registered in this epoch.
  {
    TcpConnection c;
    CHECK(!NetInit() ? false : true);  // idempotent
    CHECK(c.Connect("127.0.0.1", static_cast<std::uint16_t>(port), 5000).is_ok());
    CompatibilityKey dcompat; dcompat.model = "m"; dcompat.tokenizer = "t"; dcompat.vocabulary = "v";
    std::string body = proto::BuildPublish("DEAD-BOOT", dcompat, {}, {}, "");
    auto fs = c.SendFrame(proto::Encode(proto::MT_PUBLISH, body));
    CHECK(fs.is_ok());
    auto frame = c.RecvFrame(16 * 1024 * 1024);
    CHECK(frame.is_ok());
    auto dec = proto::Decode(frame.value());
    CHECK(dec.is_ok());
    CHECK(dec.value().first == proto::MT_ERROR);   // stale-authority rejection
  }
  // Terminate coordinator.
  TerminateProcess(hcoord, 0);
  CloseHandle(rp); CloseHandle(hcoord);
  // Coordinator epoch rollover: start a fresh coordinator (new epoch).
  std::string cmd2 = std::string(CliExe()) + " coordinator --port 0";
  HANDLE hcoord2 = nullptr; HANDLE rp2 = nullptr; std::string out2;
  CHECK(LaunchProc(cmd2, hcoord2, rp2, 8000, out2));
  int port2 = 0;
  for (;;) {
    DWORD avail = 0; char buf[512];
    if (PeekNamedPipe(rp2, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) { DWORD got = 0; ReadFile(rp2, buf, avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail, &got, nullptr); out2.append(buf, got); }
    std::size_t pos = out2.find("PORT ");
    if (pos != std::string::npos) { port2 = std::stoi(out2.substr(pos + 5)); break; }
    if (WaitForSingleObject(hcoord2, 0) == WAIT_OBJECT_0) break;
    Sleep(5);
  }
  CHECK(port2 != 0 && port2 != port);
  // A publish on the new epoch works.
  std::string pub3; std::string pubcmd3 = std::string(CliExe()) + " node --port " + std::to_string(port2) + " --publish --tokens \"7 8 9\"";
  CHECK(RunSync(pubcmd3, 15000, pub3, rc)); CHECK(pub3.find("PUBLISHED") != std::string::npos);
  TerminateProcess(hcoord2, 0);
  CloseHandle(rp2); CloseHandle(hcoord2);
}

int main() { return pftest::RunAll(); }