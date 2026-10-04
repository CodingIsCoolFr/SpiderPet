#pragma once

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sp {

// The browser extension talks to SpiderHost.exe (native messaging: a 4-byte
// length, then JSON). SpiderHost relays every message over a named pipe to
// the running SpiderPet app, which owns the library and the checker.
//
//   extension <-> SpiderHost.exe <-> \\.\pipe\SpiderPet-<user> <-> SpiderPet.exe

inline constexpr const wchar_t* kHostName = L"com.spiderpet.host";
inline constexpr const char* kFirefoxId = "spiderpet@codingiscoolfr";
inline constexpr const char* kChromiumId = "ehhpkencpadengfinambeflbhgkplfpa";

std::wstring pipe_name();
// Whole frames on an overlapped handle, so one thread can read while another writes.
bool read_frame(HANDLE h, std::string& out, HANDLE stop = nullptr);
bool write_frame(HANDLE h, const std::string& data);
// Tells Firefox, Chrome, Brave and Edge where SpiderHost.exe is (per user, HKCU).
bool register_native_host(const std::wstring& host_exe, std::wstring* error = nullptr);

class Bridge {
 public:
  using Handler = std::function<void(int client, const nlohmann::json& msg)>;
  using Gone = std::function<void(int client)>;

  bool start(Handler on_message, Gone on_gone);
  void stop();
  void send(int client, const nlohmann::json& msg);
  void broadcast(const nlohmann::json& msg);
  int clients() const;

 private:
  struct Client {
    int id = 0;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mu;
    std::thread reader;
    std::atomic<bool> done{false};
  };
  void accept_loop();
  void serve(std::shared_ptr<Client> c);

  Handler on_message_;
  Gone on_gone_;
  mutable std::mutex mu_;
  std::map<int, std::shared_ptr<Client>> clients_;
  int next_id_ = 1;
  std::atomic<bool> quit_{false};
  HANDLE stop_event_ = nullptr;
  std::thread acceptor_;
};

}  // namespace sp
