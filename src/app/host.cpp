// SpiderHost.exe: the browser starts this (native messaging) when the
// extension connects. It relays every message, unchanged, between the
// browser and the SpiderPet app over a named pipe. When the app is not
// running it waits for it, so the app can drive the browser the moment you
// start it; it starts the app itself only when the browser asks for it (you
// pressed the spider button, or a spider has something to check).
//
//   SpiderHost.exe --register    tell the browsers where this file is
#include "app/bridge.hpp"
#include "core/util.hpp"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>
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

std::mutex out_mu;

void to_browser(HANDLE out, const std::string& msg) {
  std::lock_guard lock(out_mu);
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
  std::mutex mu;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  std::deque<std::string> backlog;  // what the browser said before the app was there
  bool started = false;
  double started_at = 0;

  // Browser -> app on its own thread. Until the app is there, messages wait,
  // and anything but a quiet hello starts it.
  std::thread from_browser([&] {
    for (;;) {
      uint32_t len = 0;
      if (!read_std(in, &len, 4) || len > 64u * 1024 * 1024) break;
      std::string msg(len, '\0');
      if (len && !read_std(in, msg.data(), len)) break;
      const nlohmann::json j = nlohmann::json::parse(msg, nullptr, false);
      const std::string type = j.is_object() ? j.value("type", "") : "";
      const bool wake = type == "wake" || type != "hello" || (j.is_object() && j.value("start", true));
      std::unique_lock lock(mu);
      if (pipe != INVALID_HANDLE_VALUE) {
        if (type != "wake" && !write_frame(pipe, msg)) break;
        continue;
      }
      if (type != "wake") backlog.push_back(std::move(msg));
      if (wake && !started) {
        started = true;
        started_at = now_seconds();
        lock.unlock();
        if (!start_app()) to_browser(out, R"({"type":"error","text":"SpiderPet.exe could not be started."})");
      }
    }
    // The browser closed the connection.
    ExitProcess(0);
  });

  // Wait for the app: quickly once it is starting, gently otherwise.
  bool warned = false;
  for (;;) {
    HANDLE h = open_pipe();
    if (h != INVALID_HANDLE_VALUE) {
      std::lock_guard lock(mu);
      for (const std::string& m : backlog) write_frame(h, m);
      backlog.clear();
      pipe = h;
      break;
    }
    bool starting;
    {
      std::lock_guard lock(mu);
      starting = started;
      if (started && !warned && now_seconds() - started_at > 10) {
        warned = true;
        to_browser(out, R"({"type":"error","text":"SpiderPet.exe is not answering."})");
      }
    }
    Sleep(starting ? 100 : 700);
  }

  // App -> browser here. When the app goes away, so does this host; the
  // extension reconnects and the next host waits for the app again.
  std::string frame;
  while (read_frame(pipe, frame)) to_browser(out, frame);
  ExitProcess(0);
}
