#include "mind/net.hpp"

#include "core/util.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace sp {
namespace {

const wchar_t* kAgent = L"SpiderPet/3.0 (+https://github.com/CodingIsCoolFr/SpiderPet)";

long request(const std::wstring& host, INTERNET_PORT port, bool secure, const char* method, const std::wstring& path,
             const std::string* body, std::string& out, unsigned timeout_ms, size_t cap) {
  HINTERNET session = WinHttpOpen(kAgent, secure ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  const int t = static_cast<int>(timeout_ms);
  WinHttpSetTimeouts(session, 4000, 4000, t, t);
  HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
  const std::wstring verb = wide(method);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, verb.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                       : nullptr;
  long status = 0;
  if (req) {
    BOOL ok = body ? WinHttpSendRequest(req, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1L),
                                        const_cast<char*>(body->data()), static_cast<DWORD>(body->size()),
                                        static_cast<DWORD>(body->size()), 0)
                   : WinHttpSendRequest(req, L"Accept: application/json, application/xml;q=0.9, */*;q=0.5\r\n",
                                        static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (ok) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &code, &size, WINHTTP_NO_HEADER_INDEX);
      status = static_cast<long>(code);
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && out.size() < cap) {
        std::string chunk(avail, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
        out.append(chunk.data(), got);
      }
    }
    WinHttpCloseHandle(req);
  }
  if (conn) WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  return status;
}

}  // namespace

// Public sources ask for a gentle pace: PubMed allows 3 requests a second
// without a key, Crossref asks for a few. One find at a time stays under that.
void pace(const std::wstring& host) {
  static std::mutex mu;
  static std::map<std::wstring, std::chrono::steady_clock::time_point> next;
  double gap = 0.1;
  if (host.find(L"ncbi.nlm.nih.gov") != std::wstring::npos) gap = 0.4;
  else if (host.find(L"crossref.org") != std::wstring::npos) gap = 0.25;
  else if (host.find(L"arxiv.org") != std::wstring::npos) gap = 1.0;  // arXiv asks for one every few seconds
  std::chrono::steady_clock::time_point at;
  {
    std::lock_guard lock(mu);
    const auto now = std::chrono::steady_clock::now();
    auto& slot = next[host];
    at = std::max(slot, now);
    slot = at + std::chrono::milliseconds(static_cast<int>(gap * 1000));
  }
  std::this_thread::sleep_until(at);
}

long web_get(const std::string& url, std::string& body, unsigned timeout_ms) {
  body.clear();
  const std::wstring w = wide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {}, path[4096] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4096;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return 0;
  std::wstring full = path;
  if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) full.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  if (full.empty()) full = L"/";
  long code = 0;
  // Too many requests, a busy server or a dropped line: wait and try again, twice.
  for (int attempt = 0; attempt < 3; ++attempt) {
    pace(host);
    body.clear();
    code = request(host, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS, "GET", full, nullptr, body, timeout_ms,
                   4 * 1024 * 1024);
    if (code != 429 && code != 503 && code != 502 && code != 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500 * (attempt + 1)));
  }
  return code;
}

long local_http(const char* method, const std::string& path, const std::string* body, std::string& out,
                unsigned timeout_ms) {
  out.clear();
  return request(L"127.0.0.1", 11434, false, method, wide(path), body, out, timeout_ms, 1024 * 1024);
}

std::string url_encode(const std::string& s, const char* safe) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || (c && std::strchr(safe, c))) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

}  // namespace sp
