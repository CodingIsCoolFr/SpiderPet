#include "core/util.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>

namespace sp {

double now_seconds() {
  static const double inv = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) * inv;
}

std::string utf8(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

static std::wstring known_folder(REFKNOWNFOLDERID id, const wchar_t* leaf) {
  PWSTR path = nullptr;
  std::wstring out;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) {
    out = std::wstring(path) + L"\\" + leaf;
    CoTaskMemFree(path);
    CreateDirectoryW(out.c_str(), nullptr);
  }
  return out;
}

// SPIDERPET_DATA points tests at a scratch folder instead of the real one.
std::wstring app_data_dir() {
  if (const wchar_t* dir = _wgetenv(L"SPIDERPET_DATA")) {
    CreateDirectoryW(dir, nullptr);
    return dir;
  }
  return known_folder(FOLDERID_RoamingAppData, L"SpiderPet");
}
std::wstring documents_dir() {
  if (const wchar_t* dir = _wgetenv(L"SPIDERPET_DATA")) {  // tests keep away from the real library
    CreateDirectoryW(dir, nullptr);
    return dir;
  }
  return known_folder(FOLDERID_Documents, L"SpiderPet");
}

uint32_t fnv1a(std::wstring_view s) {
  uint32_t h = 2166136261u;
  for (wchar_t c : s) {
    h ^= static_cast<uint32_t>(c);
    h *= 16777619u;
  }
  return h;
}

std::wstring lower(std::wstring_view s) {
  std::wstring o(s);
  for (wchar_t& c : o) c = static_cast<wchar_t>(std::towlower(c));
  return o;
}

std::wstring trim(std::wstring_view s) {
  while (!s.empty() && std::iswspace(s.front())) s.remove_prefix(1);
  while (!s.empty() && std::iswspace(s.back())) s.remove_suffix(1);
  return std::wstring(s);
}

void task_log(const std::string& line) {
  static std::mutex mu;
  std::lock_guard lock(mu);
  static const std::wstring path = app_data_dir() + L"\\tasks.log";
  // Kept small: the newest ~256 KB, the one before as tasks.old.log.
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
    MoveFileExW(path.c_str(), (app_data_dir() + L"\\tasks.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
  SYSTEMTIME t{};
  GetLocalTime(&t);
  std::fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, line.c_str());
  std::fclose(f);
}

void debug_log(const std::string& line) {
  static const bool on = [] {
    const char* v = std::getenv("SPIDERPET_LOG");
    return v && *v == '1';
  }();
  if (!on) return;
  static std::mutex mu;
  std::lock_guard lock(mu);
  static const std::wstring path = app_data_dir() + L"\\debug.log";
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) return;
  std::fprintf(f, "%.3f %s\n", now_seconds(), line.c_str());
  std::fclose(f);
}

}  // namespace sp
