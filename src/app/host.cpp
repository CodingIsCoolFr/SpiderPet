// SpiderHost.exe: the browser starts this (native messaging) when the
// extension connects. It relays every message, unchanged, between the
// browser and the SpiderPet app over a named pipe, and starts the app in the
// tray if it is not running.
//
//   SpiderHost.exe --register    tell the browsers where this file is
#include "app/bridge.hpp"
#include "core/util.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

using namespace sp;

namespace {

bool read_std(HANDLE h, void* data, DWORD size) {
  auto* p = static_cast<char*>(data);
  while (size > 0) {
    DWORD got = 0;
    if (!ReadFile(h, p, size, &got, nullptr) || got == 0) return false;
    p += got;
    size -= got;
  }
  return true;
}

bool write_std(HANDLE h, const void* data, DWORD size) {
  const auto* p = static_cast<const char*>(data);
  while (size > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, size, &put, nullptr) || put == 0) return false;
    p += put;
    size -= put;
  }
  return true;
}

void to_browser(HANDLE out, const std::string& msg) {
  const uint32_t len = static_cast<uint32_t>(msg.size());
  write_std(out, &len, 4);
  write_std(out, msg.data(), len);
}

HANDLE open_pipe() {
  const std::wstring name = pipe_name();
  HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(name.c_str(), 2000))
    h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  return h;
}

std::wstring here() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p = path;
  return p.substr(0, p.find_last_of(L"\\/"));
}

// The app is started outside the browser's job, so closing the browser does
// not take the app (and its library) with it.
bool start_app() {
  std::wstring cmd = L"\"" + here() + L"\\SpiderPet.exe\" --tray";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const DWORD base = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, base | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                           here().c_str(), &si, &pi);
  if (!ok) ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, base, nullptr, here().c_str(), &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2 && std::wstring(argv[1]) == L"--register") {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring err;
    const bool ok = register_native_host(self, &err);
    std::printf(ok ? "SpiderPet is registered with Firefox, Chrome, Brave and Edge.\n" : "Registration failed.\n");
    return ok ? 0 : 1;
  }

  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE pipe = open_pipe();
  if (pipe == INVALID_HANDLE_VALUE && start_app()) {
    for (int i = 0; i < 80 && pipe == INVALID_HANDLE_VALUE; ++i) {
      Sleep(100);
      pipe = open_pipe();
    }
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    to_browser(out, R"({"type":"error","text":"SpiderPet.exe is not running and could not be started."})");
    return 1;
  }

  // App -> browser on its own thread; browser -> app here.
  std::thread back([pipe, out] {
    std::string frame;
    while (read_frame(pipe, frame)) to_browser(out, frame);
    ExitProcess(0);  // the app went away: let the extension reconnect
  });
  for (;;) {
    uint32_t len = 0;
    if (!read_std(in, &len, 4) || len > 64u * 1024 * 1024) break;
    std::string msg(len, '\0');
    if (len && !read_std(in, msg.data(), len)) break;
    if (!write_frame(pipe, msg)) break;
  }
  // The browser closed the connection.
  CloseHandle(pipe);
  ExitProcess(0);
}
