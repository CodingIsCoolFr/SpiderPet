#include "app/bridge.hpp"

#include "core/util.hpp"

#include <fstream>

namespace sp {

using json = nlohmann::json;

std::wstring pipe_name() {
  wchar_t user[256] = L"user";
  DWORD n = 256;
  GetUserNameW(user, &n);
  return std::wstring(L"\\\\.\\pipe\\SpiderPet-") + user;
}

namespace {

// One overlapped transfer of exactly `size` bytes. False on a broken pipe
// or when `stop` is signalled.
bool transfer(HANDLE h, bool write, void* data, DWORD size, HANDLE stop) {
  auto* p = static_cast<char*>(data);
  while (size > 0) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD done = 0;
    BOOL ok = write ? WriteFile(h, p, size, nullptr, &ov) : ReadFile(h, p, size, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING && GetLastError() != ERROR_MORE_DATA) {
      CloseHandle(ov.hEvent);
      return false;
    }
    if (stop) {
      HANDLE waits[2] = {ov.hEvent, stop};
      if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &done, TRUE);
        CloseHandle(ov.hEvent);
        return false;
      }
    }
    ok = GetOverlappedResult(h, &ov, &done, TRUE);
    CloseHandle(ov.hEvent);
    if ((!ok && GetLastError() != ERROR_MORE_DATA) || done == 0) return false;
    p += done;
    size -= done;
  }
  return true;
}

void write_text(const std::wstring& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}

bool set_default(const std::wstring& key, const std::wstring& value) {
  HKEY k = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
    return false;
  const LONG r = RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(k);
  return r == ERROR_SUCCESS;
}

}  // namespace

bool read_frame(HANDLE h, std::string& out, HANDLE stop) {
  uint32_t len = 0;
  if (!transfer(h, false, &len, 4, stop)) return false;
  if (len > 64u * 1024 * 1024) return false;
  out.assign(len, '\0');
  return len == 0 || transfer(h, false, out.data(), len, stop);
}

bool write_frame(HANDLE h, const std::string& data) {
  uint32_t len = static_cast<uint32_t>(data.size());
  return transfer(h, true, &len, 4, nullptr) && (len == 0 || transfer(h, true, const_cast<char*>(data.data()), len, nullptr));
}

bool register_native_host(const std::wstring& host_exe, std::wstring* error) {
  const std::wstring dir = app_data_dir();
  const std::string exe = utf8(host_exe);
  const json firefox = {{"name", utf8(kHostName)},
                        {"description", "SpiderPet: checks what the spider harvests"},
                        {"path", exe},
                        {"type", "stdio"},
                        {"allowed_extensions", {kFirefoxId}}};
  const json chromium = {{"name", utf8(kHostName)},
                         {"description", "SpiderPet: checks what the spider harvests"},
                         {"path", exe},
                         {"type", "stdio"},
                         {"allowed_origins", {std::string("chrome-extension://") + kChromiumId + "/"}}};
  const std::wstring ff = dir + L"\\native-host-firefox.json";
  const std::wstring ch = dir + L"\\native-host-chromium.json";
  write_text(ff, firefox.dump(2));
  write_text(ch, chromium.dump(2));
  const std::wstring name = kHostName;
  bool ok = set_default(L"Software\\Mozilla\\NativeMessagingHosts\\" + name, ff);
  for (const wchar_t* browser : {L"Software\\Google\\Chrome", L"Software\\BraveSoftware\\Brave-Browser",
                                 L"Software\\Microsoft\\Edge", L"Software\\Chromium"})
    ok &= set_default(std::wstring(browser) + L"\\NativeMessagingHosts\\" + name, ch);
  if (!ok && error) *error = L"could not write the browser registration";
  return ok;
}

bool Bridge::start(Handler on_message, Gone on_gone) {
  on_message_ = std::move(on_message);
  on_gone_ = std::move(on_gone);
  quit_ = false;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  acceptor_ = std::thread([this] { accept_loop(); });
  return true;
}

void Bridge::stop() {
  quit_ = true;
  if (stop_event_) SetEvent(stop_event_);
  if (acceptor_.joinable()) acceptor_.join();
  std::map<int, std::shared_ptr<Client>> all;
  {
    std::lock_guard lock(mu_);
    all.swap(clients_);
  }
  for (auto& [id, c] : all)
    if (c->reader.joinable()) c->reader.join();
  if (stop_event_) CloseHandle(stop_event_);
  stop_event_ = nullptr;
}

void Bridge::accept_loop() {
  const std::wstring name = pipe_name();
  while (!quit_) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(stop_event_, 500) == WAIT_OBJECT_0) break;
      continue;
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
    if (!connected) {
      const DWORD e = GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (e == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, stop_event_};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
          DWORD n = 0;
          connected = GetOverlappedResult(pipe, &ov, &n, FALSE) != FALSE;
        } else {
          CancelIoEx(pipe, &ov);
        }
      }
    }
    CloseHandle(ov.hEvent);
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }
    auto c = std::make_shared<Client>();
    c->pipe = pipe;
    {
      std::lock_guard lock(mu_);
      c->id = next_id_++;
      // Reap clients that have gone.
      for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second->done) {
          if (it->second->reader.joinable()) it->second->reader.join();
          it = clients_.erase(it);
        } else {
          ++it;
        }
      }
      clients_[c->id] = c;
    }
    c->reader = std::thread([this, c] { serve(c); });
  }
}

void Bridge::serve(std::shared_ptr<Client> c) {
  std::string frame;
  while (!quit_ && read_frame(c->pipe, frame, stop_event_)) {
    json msg = json::parse(frame, nullptr, false);
    if (msg.is_object() && on_message_) on_message_(c->id, msg);
  }
  {
    std::lock_guard lock(c->write_mu);
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    c->pipe = INVALID_HANDLE_VALUE;
  }
  c->done = true;
  if (on_gone_) on_gone_(c->id);
}

void Bridge::send(int client, const json& msg) {
  std::shared_ptr<Client> c;
  {
    std::lock_guard lock(mu_);
    auto it = clients_.find(client);
    if (it == clients_.end()) return;
    c = it->second;
  }
  const std::string data = msg.dump(-1, ' ', false, json::error_handler_t::replace);
  std::lock_guard lock(c->write_mu);
  if (c->pipe != INVALID_HANDLE_VALUE) write_frame(c->pipe, data);
}

void Bridge::broadcast(const json& msg) {
  std::vector<int> ids;
  {
    std::lock_guard lock(mu_);
    for (auto& [id, c] : clients_)
      if (!c->done) ids.push_back(id);
  }
  for (int id : ids) send(id, msg);
}

int Bridge::clients() const {
  std::lock_guard lock(mu_);
  int n = 0;
  for (auto& [id, c] : clients_) n += c->done ? 0 : 1;
  return n;
}

}  // namespace sp
