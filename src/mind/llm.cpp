#include "mind/llm.hpp"

#include "core/util.hpp"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>

namespace sp {
namespace {

const char* kPreferred[] = {"qwen2.5:3b", "llama3.2:3b", "qwen3:4b", "gemma3:4b", "phi4-mini", "qwen2.5:7b"};

// Plain HTTP to Ollama on this machine. Returns the status code, or 0 when
// nothing answered (Ollama not running).
long http(const wchar_t* path, const std::string* body, std::string& out, DWORD timeout_ms) {
  HINTERNET session = WinHttpOpen(L"SpiderPet/2.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  WinHttpSetTimeouts(session, 2000, 2000, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
  HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", 11434, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(conn, body ? L"POST" : L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                       : nullptr;
  long status = 0;
  if (req) {
    BOOL ok = body ? WinHttpSendRequest(req, L"Content-Type: application/json\r\n", static_cast<DWORD>(-1L),
                                        const_cast<char*>(body->data()), static_cast<DWORD>(body->size()),
                                        static_cast<DWORD>(body->size()), 0)
                   : WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    if (ok) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &code, &size, WINHTTP_NO_HEADER_INDEX);
      status = static_cast<long>(code);
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && out.size() < 256 * 1024) {
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

std::string prompt_for(const std::string& task, const std::string& text, const std::string& title) {
  if (task == "gist")
    return "Here is the start of a web page.\n\nTitle: " + title + "\n" + text.substr(0, 1600) +
           "\n\nIn at most 6 words, what is this page about? Reply with the words only.";
  return "Sentence: \"" + text.substr(0, 420) +
         "\"\n\nRestate its main point in at most 7 plain words. Reply with the words only, no quotes.";
}

// Small models like to say "The sentence says that..." and wrap things in quotes.
std::string clean(std::string s) {
  const auto think = s.find("</think>");
  if (think != std::string::npos) s = s.substr(think + 8);
  size_t a = s.find_first_not_of(" \r\n\t");
  if (a == std::string::npos) return {};
  s = s.substr(a);
  s = s.substr(0, s.find('\n'));
  std::wstring w = wide(s);
  const std::wstring lw = lower(w);
  for (const wchar_t* p : {L"answer:", L"summary:", L"main point:", L"topic:"})
    if (lw.rfind(p, 0) == 0) w = trim(w.substr(wcslen(p)));
  for (const wchar_t* p : {L"the sentence says that ", L"the sentence says ", L"the page is about ", L"this page is about "})
    if (lower(w).rfind(p, 0) == 0) w = w.substr(wcslen(p));
  auto quote = [](wchar_t c) {
    return c == L'"' || c == L'\'' || c == L'*' || c == L'`' || c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019;
  };
  while (!w.empty() && quote(w.front())) w.erase(w.begin());
  while (!w.empty() && (quote(w.back()) || w.back() == L'.' || w.back() == L'!')) w.pop_back();
  w = trim(w);
  // A 3B model sometimes talks about the task instead of doing it.
  const std::wstring low = lower(w);
  for (const wchar_t* bad : {L"cannot", L"can't", L"unable", L"sorry", L"exceeds", L"main point", L"sentence", L"words"})
    if (low.find(bad) != std::wstring::npos) return {};
  if (std::count(w.begin(), w.end(), L' ') > 11) return {};
  if (w.size() > 64) w = w.substr(0, 61) + L"…";
  if (!w.empty()) w[0] = static_cast<wchar_t>(std::towlower(w[0]));
  return utf8(w);
}

}  // namespace

void Llm::start() {
  quit_ = false;
  thread_ = std::thread([this] { loop(); });
}

void Llm::stop() {
  quit_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Llm::ask(const Ask& a, uint64_t page, const std::wstring& title) {
  std::lock_guard lock(mu_);
  jobs_.push_back({a, page, title});
  // The spider moves on quickly; old sentence requests are not worth it.
  while (jobs_.size() > 6) jobs_.pop_front();
  cv_.notify_all();
}

std::vector<Llm::Answer> Llm::take() {
  std::lock_guard lock(mu_);
  return std::exchange(done_, {});
}

std::string Llm::status() const {
  std::lock_guard lock(mu_);
  return status_;
}

std::string Llm::model() const {
  std::lock_guard lock(mu_);
  return model_;
}

void Llm::set_model(const std::string& m) {
  std::lock_guard lock(mu_);
  wanted_ = m;
  model_checked_ = -100;
}

bool Llm::ensure_model() {
  {
    std::lock_guard lock(mu_);
    if (!model_.empty() && now_seconds() - model_checked_ < 120) return true;
    if (model_.empty() && now_seconds() - model_checked_ < 20) return false;
    model_checked_ = now_seconds();
  }
  std::string body;
  const long code = http(L"/api/tags", nullptr, body, 4000);
  std::string pick, why;
  if (code == 200) {
    try {
      std::vector<std::string> names;
      for (const auto& m : nlohmann::json::parse(body).value("models", nlohmann::json::array()))
        names.push_back(m.value("name", ""));
      std::string wanted;
      {
        std::lock_guard lock(mu_);
        wanted = wanted_;
      }
      if (!wanted.empty() && std::find(names.begin(), names.end(), wanted) != names.end()) pick = wanted;
      for (const char* p : kPreferred)
        if (pick.empty() && std::find(names.begin(), names.end(), p) != names.end()) pick = p;
      for (const auto& n : names)
        if (pick.empty() && n.find("embed") == std::string::npos) pick = n;
      if (pick.empty()) why = "no model installed";
    } catch (...) {
      why = "odd reply from Ollama";
    }
  } else {
    why = code == 0 ? "Ollama is not running" : "Ollama said " + std::to_string(code);
  }
  std::lock_guard lock(mu_);
  model_ = pick;
  status_ = pick.empty() ? why : pick;
  return !pick.empty();
}

void Llm::loop() {
  while (!quit_) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return quit_.load() || !jobs_.empty(); });
      if (quit_) break;
      if (jobs_.empty()) continue;
      job = std::move(jobs_.back());  // newest first: that is what the spider is looking at
      jobs_.pop_back();
    }
    if (!ensure_model()) continue;
    const nlohmann::json req = {
        {"model", model()},
        {"prompt", prompt_for(job.ask.task, utf8(job.ask.text), utf8(job.title))},
        {"stream", false},
        {"keep_alive", "10m"},
        {"options", {{"temperature", 0.2}, {"num_predict", 28}, {"num_ctx", 2048}}},
    };
    const std::string payload = req.dump();
    std::string body;
    const long code = http(L"/api/generate", &payload, body, 30000);
    std::string text;
    if (code == 200) {
      try {
        text = clean(nlohmann::json::parse(body).value("response", ""));
      } catch (...) {
      }
    } else {
      std::lock_guard lock(mu_);
      model_checked_ = -100;
      status_ = code == 0 ? "Ollama is not running" : "Ollama said " + std::to_string(code);
    }
    if (text.empty()) continue;
    std::lock_guard lock(mu_);
    done_.push_back({job.ask, job.page, text});
  }
}

}  // namespace sp
