// SpiderPet.exe: the brain and the library behind the browser spider.
//
// The extension walks the page and harvests DOIs, ISBNs, ids, titles, links
// and key sentences from the real page text. Everything it finds comes here
// (through SpiderHost.exe), is checked against real sources with the local
// model as the judge, ranked against what you are looking for, and kept in
// one library across browsers. The window shows it; the tray keeps it running.
//
//   SpiderPet.exe          open the window
//   SpiderPet.exe --tray   start in the tray (what SpiderHost does)
#include "app/bridge.hpp"
#include "app/library.hpp"
#include "core/util.hpp"
#include "mind/check.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace sp {
namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kShowMsg = WM_APP + 2;
constexpr UINT kWakeMsg = WM_APP + 3;
constexpr const char* kVersion = "3.1.0";

ImVec4 hexv(uint32_t c, float a = 1.f) {
  return ImVec4(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}

// The colors the spider restyles each kind with on the page.
uint32_t kind_color(const std::string& k) {
  static const std::map<std::string, uint32_t> m = {{"sentence", 0xE64CF2}, {"heading", 0xE64CF2}, {"title", 0xF2836B},
                                                    {"doi", 0x93F5AE},      {"isbn", 0x7D8BFF},    {"id", 0x4E8FF0},
                                                    {"link", 0x7FD6FF},     {"number", 0xFF5C5C},  {"date", 0xFFC46B}};
  auto it = m.find(k);
  return it == m.end() ? 0xC8CCD8 : it->second;
}

struct Pill {
  const char* text;
  uint32_t bg;
  uint32_t fg;
};

Pill pill_for(const json& v) {
  const std::string st = v.is_object() ? v.value("status", "") : "";
  if (st == "verified") return {"VERIFIED", 0x2E7D4F, 0xE9FFF0};
  if (st == "mismatch") return {"WRONG", 0xB4561E, 0xFFF3E8};
  if (st == "not_found") return {"NOT FOUND", 0xA8323A, 0xFFECEC};
  if (st == "unverified") return {"UNCLEAR", 0x3A3E52, 0xD6D9E6};
  if (st == "opinion") return {"OPINION", 0x6A3C8C, 0xF4E9FF};
  if (st == "promo") return {"AD", 0x6A3C8C, 0xF4E9FF};
  if (st == "queued" || st == "checking") return {"CHECKING", 0x24426E, 0xDCEBFF};
  if (st == "error") return {"NOT CHECKED", 0x4A2A2E, 0xF0C8CC};
  if (st == "skipped") return {"", 0, 0};
  return {"", 0, 0};
}

bool problem(const json& v) {
  const std::string st = v.is_object() ? v.value("status", "") : "";
  return st == "mismatch" || st == "not_found";
}

std::string jstr(const json& j, const char* k) {
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// What relevance means something for: words a person reads. Ids ride on their titles.
bool rankable(const json& f) {
  const std::string k = jstr(f, "kind");
  return k == "sentence" || k == "heading" || k == "link" || k == "title";
}

// Words that would match half of any page.
bool stopword(const std::string& w) {
  static const std::set<std::string> s = {
      "the", "and", "for", "with", "about", "what", "how", "who", "why", "when", "where", "which", "that", "this",
      "are", "was", "were", "from", "into", "its", "find", "show", "look", "looking", "info", "information", "anything",
      "something", "page", "pages", "some", "any", "all", "more", "most", "best", "does", "did", "can", "you", "your"};
  return w.size() < 3 || s.count(w) > 0;
}

std::string lower_utf8(const std::string& s) { return utf8(lower(wide(s))); }

// The search as words the spider can match right away, before the model has
// thought of synonyms: the whole phrase, then each word that means something.
json quick_terms(const std::string& goal) {
  json out = json::array();
  std::set<std::string> seen;
  auto put = [&](const std::string& t) {
    if (!t.empty() && seen.insert(t).second) out.push_back(t);
  };
  const std::string g = utf8(trim(lower(wide(goal))));
  if (g.empty()) return out;
  std::string word;
  std::vector<std::string> words;
  for (const char c : g + " ") {
    // Letters, digits, and every byte of a non-English letter stay in the word.
    if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80 || c == '-' || c == '\'') {
      word += c;
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
    }
  }
  if (words.size() > 1 && g.size() <= 60) put(g);
  for (const std::string& w : words)
    if (!stopword(w) || words.size() == 1) put(w);
  return out;
}

std::wstring exe_dir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p = path;
  return p.substr(0, p.find_last_of(L"\\/"));
}

void open_url(const std::string& url) {
  if (url.rfind("http", 0) == 0) ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_path(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void copy_text(HWND hwnd, const std::string& text) {
  const std::wstring w = wide(text);
  if (!OpenClipboard(hwnd)) return;
  EmptyClipboard();
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (mem) {
    memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    SetClipboardData(CF_UNICODETEXT, mem);
  }
  CloseClipboard();
}

}  // namespace

class App {
 public:
  int run(HINSTANCE inst, bool tray);

 private:
  // window, tray, drawing
  bool create_window(HINSTANCE inst, bool show);
  void make_target();
  void destroy_window();
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
  void tray_add();
  void tray_remove();
  void show();
  void hide();
  void style();
  void draw();
  void draw_header();
  void draw_page();
  void draw_library();
  void draw_settings();
  void draw_setup();
  void draw_find(const json& f, bool show_page);
  void wake() { if (hwnd_) PostMessageW(hwnd_, kWakeMsg, 0, 0); }

  // the brain
  void on_message(int client, const json& m);
  void on_gone(int client);
  void queue_check(const std::string& id, bool urgent);
  void queue_rank(const std::string& url, const std::vector<std::string>& ids);
  void rerank(const std::string& url);
  void worker();
  json state_msg();
  void apply_settings(const json& partial);
  void reveal(const std::string& url, const std::string& id);
  void commit_goal(const std::string& goal, bool force);
  void send_spider(bool on);
  int matches(const json& f) const;

  struct Job {
    enum Type { Check, Rank, Gist, Expand } type;
    std::string url, id;
    json data;
  };

  // A connected browser and the tab you have open in it.
  struct Browser {
    std::string name;  // "Firefox", "Brave"...
    int tab = -1;
    std::string url, title;
    bool web = false;     // a page the spider can go on (not a browser page)
    bool spider = false;  // the spider is on that tab
    double at = 0;        // when you last looked at it
  };
  // The browser you used last: where Start spider and a search send the spider. Called with mu_ held.
  const Browser* current(int* client = nullptr) const;

  HWND hwnd_ = nullptr;
  HINSTANCE inst_ = nullptr;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ImGuiContext* imgui_ = nullptr;
  ImFont* body_ = nullptr;
  ImFont* title_ = nullptr;
  ImFont* small_ = nullptr;
  ImFont* bold_ = nullptr;
  float scale_ = 1.f;
  bool visible_ = false;
  bool quitting_ = false;
  NOTIFYICONDATAW tray_{};

  std::mutex mu_;
  Library lib_;
  Settings settings_;
  Checker checker_;
  Bridge bridge_;
  std::map<int, Browser> browsers_;      // by client
  std::string active_url_;               // the page "This page" shows: the tab you are on
  json terms_ = json::array();           // the search as words to match: yours first, then the model's
  int goal_seq_ = 0;                     // bumps with every search, so a late model answer is dropped
  json status_ = {{"ollama", false}, {"model", ""}};
  std::string busy_;                     // what the worker is doing
  std::deque<Job> urgent_, jobs_;
  std::set<std::string> queued_;         // check ids waiting, so nothing is queued twice
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> quit_{false};
  bool registered_ = false;
  std::wstring register_error_;

  // UI state
  char goal_[512] = {};
  char search_[256] = {};
  int status_filter_ = 0;
  std::string notice_;
  double notice_at_ = 0;
  json pending_;  // settings changed in the window this frame
  double goal_edit_at_ = 0;
  bool goal_dirty_ = false;  // typed in, not sent yet
  bool goal_force_ = false;
  bool focus_goal_ = false;  // the window just opened: the search box takes the keyboard
  std::optional<std::string> pending_goal_;
  std::optional<bool> pending_spider_;
};

int App::run(HINSTANCE inst, bool tray) {
  inst_ = inst;
  settings_.load();
  lib_.load();
  // Checks that were waiting when the app last closed start over.
  for (auto& [id, f] : lib_.finds().items()) {
    const json& v = f["verdict"];
    if (v.is_object() && (jstr(v, "status") == "queued" || jstr(v, "status") == "checking")) lib_.set_verdict(id, nullptr);
  }
  checker_.set_model(settings_.model);
  if (!lib_.page_order().empty()) active_url_ = lib_.page_order().front();  // the last page you were on
  snprintf(goal_, sizeof(goal_), "%s", settings_.goal.c_str());
  terms_ = quick_terms(settings_.goal);
  if (!settings_.goal.empty()) urgent_.push_back({Job::Expand, "", "", {{"goal", settings_.goal}, {"seq", goal_seq_}}});

  const std::wstring host = exe_dir() + L"\\SpiderHost.exe";
  if (_wgetenv(L"SPIDERPET_DATA")) registered_ = true;  // a test copy: leave the real registration alone
  else if (GetFileAttributesW(host.c_str()) != INVALID_FILE_ATTRIBUTES) registered_ = register_native_host(host, &register_error_);
  else register_error_ = L"SpiderHost.exe is missing next to SpiderPet.exe";

  status_ = checker_.status();  // so the first hello already knows about the model
  if (!create_window(inst, !tray)) return 1;
  tray_add();
  bridge_.start([this](int c, const json& m) { on_message(c, m); }, [this](int c) { on_gone(c); });
  worker_ = std::thread([this] { worker(); });

  double saved_at = now_seconds();
  MSG msg;
  while (!quitting_) {
    if (visible_) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) quitting_ = true;
      }
      if (quitting_) break;
      draw();
    } else {
      // Hidden in the tray: sleep until something happens.
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 1000, QS_ALLINPUT);
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) quitting_ = true;
      }
    }
    if (now_seconds() - saved_at > 3.0) {
      saved_at = now_seconds();
      std::lock_guard lock(mu_);
      lib_.save();
    }
  }

  quit_ = true;
  cv_.notify_all();
  bridge_.stop();
  if (worker_.joinable()) worker_.join();
  {
    std::lock_guard lock(mu_);
    lib_.save();
  }
  settings_.save();
  tray_remove();
  destroy_window();
  return 0;
}

// ---------------------------------------------------------------- the brain

json App::state_msg() {
  return {{"type", "state"}, {"app", kVersion},   {"settings", settings_.to_json()},
          {"status", status_}, {"goal", settings_.goal}, {"terms", terms_}};
}

const App::Browser* App::current(int* client) const {
  const Browser* best = nullptr;
  for (const auto& [c, b] : browsers_)
    if (b.tab >= 0 && (!best || b.at > best->at)) {
      best = &b;
      if (client) *client = c;
    }
  return best;
}

// Ids, titles and sentences that contain a word of the search. Called with mu_ held.
int App::matches(const json& f) const {
  if (terms_.empty()) return 0;
  const std::string t = lower_utf8(jstr(f, "text"));
  int n = 0;
  for (const json& w : terms_)
    if (w.is_string() && t.find(w.get_ref<const std::string&>()) != std::string::npos) ++n;
  return n;
}

void App::on_message(int client, const json& m) {
  const std::string type = jstr(m, "type");
  std::unique_lock lock(mu_);
  if (type == "hello") {
    browsers_[client].name = jstr(m, "browser").empty() ? "browser" : jstr(m, "browser");
    const json st = state_msg();
    lock.unlock();
    bridge_.send(client, st);
    wake();
    return;
  }
  if (type == "tab") {
    // The tab you are looking at: "This page" follows it, and it is where the spider goes.
    Browser& b = browsers_[client];
    b.tab = m.value("tabId", -1);
    b.url = jstr(m, "url");
    b.title = jstr(m, "title");
    b.web = m.value("web", false);
    b.spider = m.value("spider", false);
    b.at = now_seconds();
    if (b.web && !b.url.empty()) active_url_ = b.url;
    wake();
    return;
  }
  if (type == "notice") {
    notice_ = jstr(m, "text");
    notice_at_ = now_seconds();
    wake();
    return;
  }
  if (type == "page") {
    const std::string url = jstr(m, "url");
    if (url.empty()) return;
    if (!current()) active_url_ = url;  // a browser that does not say which tab you are on
    lib_.touch_page(url, jstr(m, "title"), jstr(m, "lang"));
    const json* p = lib_.page(url);
    const bool has_gist = p && p->contains("gist") && (*p)["gist"].is_object() && !(*p)["gist"].contains("error");
    if (has_gist) {
      const json g = {{"type", "gist"}, {"url", url}, {"gist", (*p)["gist"]}};
      lock.unlock();
      bridge_.send(client, g);
    } else if (!jstr(m, "text").empty()) {
      urgent_.push_back({Job::Gist, url, "", {{"title", jstr(m, "title")}, {"text", jstr(m, "text")}}});
      cv_.notify_all();
    }
    wake();
    return;
  }
  if (type == "finds") {
    const std::string url = jstr(m, "url");
    if (url.empty()) return;
    if (!current()) active_url_ = url;
    const json& finds = m.contains("finds") ? m["finds"] : json::array();
    const auto fresh = lib_.add(url, jstr(m, "title"), jstr(m, "lang"), finds);
    // What was already known from an earlier visit goes straight back.
    json known = json::object();
    std::vector<std::string> unranked;
    for (const json& f : finds) {
      const std::string id = jstr(f, "id");
      const json* x = lib_.find(id);
      if (!x) continue;
      const json& v = (*x)["verdict"];
      const int score = (*x).value("score", -1);
      if (v.is_object() || score >= 0) known[id] = {{"verdict", v}, {"score", score}};
      if (score < 0 && rankable(*x)) unranked.push_back(id);
      // A sentence costs two model calls: only the ones that match your
      // search are checked up front, the rest when the spider eats them.
      const bool sentence = jstr(*x, "kind") == "sentence";
      const bool match = f.value("match", 0) > 0;
      if (settings_.auto_check && !v.is_object() && checkable(*x, settings_) && (!sentence || match))
        queue_check(id, match);  // what you searched for is checked first
    }
    lock.unlock();
    if (!known.empty()) bridge_.send(client, {{"type", "known"}, {"url", url}, {"items", known}});
    lock.lock();
    // Ranking only means something against a goal; without one, the whole page is on topic.
    if (!unranked.empty() && !settings_.goal.empty()) queue_rank(url, unranked);
    wake();
    return;
  }
  if (type == "check") {
    const std::string id = jstr(m, "id");
    if (lib_.find(id)) {
      lib_.set_verdict(id, nullptr);
      queue_check(id, true);
    }
    wake();
    return;
  }
  if (type == "soon") {
    // The spider just ate it: its check moves to the front.
    const std::string id = jstr(m, "id");
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.type == Job::Check && j.id == id; });
    if (it != jobs_.end()) {
      urgent_.push_front(std::move(*it));
      jobs_.erase(it);
      cv_.notify_all();
    } else if (const json* f = lib_.find(id);
               f && settings_.auto_check && !(*f)["verdict"].is_object() && checkable(*f, settings_)) {
      queue_check(id, true);
    }
    return;
  }
  if (type == "settings") {
    lock.unlock();
    apply_settings(m.value("settings", json::object()));
    return;
  }
  if (type == "open-app") {
    lock.unlock();
    if (hwnd_) PostMessageW(hwnd_, kShowMsg, 0, 0);
    return;
  }
}

void App::on_gone(int client) {
  std::lock_guard lock(mu_);
  browsers_.erase(client);
  wake();
}

// Called with mu_ held.
void App::queue_check(const std::string& id, bool urgent) {
  if (queued_.count(id)) return;
  queued_.insert(id);
  lib_.set_verdict(id, {{"status", "queued"}});
  (urgent ? urgent_ : jobs_).push_back({Job::Check, "", id, nullptr});
  cv_.notify_all();
}

// Called with mu_ held. Batches of 40: one model call each.
void App::queue_rank(const std::string& url, const std::vector<std::string>& all) {
  std::vector<std::string> ids;
  for (const std::string& id : all)
    if (const json* f = lib_.find(id); f && rankable(*f)) ids.push_back(id);
  for (size_t i = 0; i < ids.size(); i += 40) {
    json items = json::array();
    for (size_t k = i; k < ids.size() && k < i + 40; ++k)
      if (const json* f = lib_.find(ids[k])) {
        // An id alone says nothing; the citation around it does.
        const std::string kind = jstr(*f, "kind");
        std::string text = jstr(*f, "text");
        if ((kind == "doi" || kind == "isbn" || kind == "id") && !jstr(*f, "context").empty())
          text = kind + " " + text + " in: " + jstr(*f, "context").substr(0, 220);
        items.push_back({{"id", ids[k]}, {"kind", kind}, {"text", text}});
      }
    if (!items.empty()) urgent_.push_back({Job::Rank, url, "", items});
  }
  cv_.notify_all();
}

void App::rerank(const std::string& url) {
  std::lock_guard lock(mu_);
  if (url.empty()) return;
  std::erase_if(urgent_, [&](const Job& j) { return j.type == Job::Rank && j.url == url; });
  queue_rank(url, lib_.finds_of(url));
}

void App::apply_settings(const json& partial) {
  json out;
  {
    std::lock_guard lock(mu_);
    const std::string old_goal = settings_.goal;
    const std::string old_model = settings_.model;
    settings_.from_json(partial);
    settings_.save();
    if (settings_.model != old_model) checker_.set_model(settings_.model);
    snprintf(goal_, sizeof(goal_), "%s", settings_.goal.c_str());
    out = {{"type", "settings"}, {"settings", settings_.to_json()}};
    if (settings_.goal != old_goal && !active_url_.empty()) {
      std::erase_if(urgent_, [&](const Job& j) { return j.type == Job::Rank; });
      queue_rank(active_url_, lib_.finds_of(active_url_));
    }
  }
  bridge_.broadcast(out);
  wake();
}

void App::reveal(const std::string& url, const std::string& id) {
  bridge_.broadcast({{"type", "reveal"}, {"url", url}, {"id", id}});
}

void App::worker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  double status_at = -100;
  while (!quit_) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait_for(lock, std::chrono::seconds(5), [&] { return quit_.load() || !urgent_.empty() || !jobs_.empty(); });
      if (quit_) break;
      if (urgent_.empty() && jobs_.empty()) {
        lock.unlock();
        const json st = checker_.status();
        lock.lock();
        if (st != status_) {
          status_ = st;
          const json msg = state_msg();
          lock.unlock();
          bridge_.broadcast(msg);
          wake();
        }
        continue;
      }
      // While the graphics card is too full for the model, only the work that
      // needs no model goes on (ids and titles are looked up and compared by words).
      const bool gpu_busy = checker_.gpu_busy();
      auto needs_model = [&](const Job& j) {
        if (j.type != Job::Check) return true;
        const json* f = lib_.find(j.id);
        return f && jstr(*f, "kind") == "sentence";
      };
      std::deque<Job>* q = nullptr;
      size_t at = 0;
      for (std::deque<Job>* d : {&urgent_, &jobs_}) {
        for (size_t i = 0; i < d->size() && !q; ++i)
          if (!gpu_busy || !needs_model((*d)[i])) q = d, at = i;
        if (q) break;
      }
      if (!q) {
        wake();
        cv_.wait_for(lock, std::chrono::seconds(3), [&] { return quit_.load(); });
        continue;
      }
      job = std::move((*q)[at]);
      q->erase(q->begin() + static_cast<std::ptrdiff_t>(at));
    }
    if (now_seconds() - status_at > 15) {
      status_at = now_seconds();
      const json st = checker_.status();
      std::lock_guard lock(mu_);
      status_ = st;
    }

    if (job.type == Job::Check) {
      json find, page;
      bool online;
      {
        std::lock_guard lock(mu_);
        queued_.erase(job.id);
        const json* f = lib_.find(job.id);
        if (!f) continue;
        find = *f;
        const json* p = lib_.page(jstr(find, "url"));
        page = {{"url", jstr(find, "url")}, {"title", p ? jstr(*p, "title") : ""}, {"lang", p ? jstr(*p, "lang") : ""}};
        online = settings_.online;
        busy_ = "checking: " + jstr(find, "text").substr(0, 60);
        lib_.set_verdict(job.id, {{"status", "checking"}});
      }
      wake();
      bridge_.broadcast({{"type", "verdict"}, {"url", jstr(find, "url")}, {"id", job.id}, {"verdict", {{"status", "checking"}}}});
      const json v = checker_.check(find, page, online);
      const int tries = job.data.is_object() ? job.data.value("tries", 0) : 0;
      {
        std::lock_guard lock(mu_);
        busy_.clear();
        // A source that failed now gets one more go at the end of the line.
        if (jstr(v, "status") == "error" && tries < 1 && !queued_.count(job.id)) {
          queued_.insert(job.id);
          lib_.set_verdict(job.id, {{"status", "queued"}});
          jobs_.push_back({Job::Check, "", job.id, {{"tries", tries + 1}}});
          continue;
        }
        lib_.set_verdict(job.id, v);
      }
      bridge_.broadcast({{"type", "verdict"}, {"url", jstr(find, "url")}, {"id", job.id}, {"verdict", v}});
    } else if (job.type == Job::Rank) {
      std::string goal, title;
      {
        std::lock_guard lock(mu_);
        goal = settings_.goal;
        const json* p = lib_.page(job.url);
        title = p ? jstr(*p, "title") : "";
        busy_ = "ranking what matters";
      }
      wake();
      const json r = checker_.rank(goal, title, job.data);
      json scores = r.value("scores", json::object());
      {
        std::lock_guard lock(mu_);
        for (auto& [id, sc] : scores.items())
          if (sc.is_number()) lib_.set_score(id, sc.get<int>());
        busy_.clear();
      }
      if (!scores.empty()) bridge_.broadcast({{"type", "scores"}, {"url", job.url}, {"scores", scores}});
    } else if (job.type == Job::Gist) {
      {
        std::lock_guard lock(mu_);
        busy_ = "reading the page";
      }
      wake();
      const json g = checker_.gist(jstr(job.data, "title"), job.url, jstr(job.data, "text"));
      {
        std::lock_guard lock(mu_);
        if (!g.contains("error")) lib_.set_gist(job.url, g);
        busy_.clear();
      }
      bridge_.broadcast({{"type", "gist"}, {"url", job.url}, {"gist", g}});
    } else if (job.type == Job::Expand) {
      const std::string goal = jstr(job.data, "goal");
      {
        std::lock_guard lock(mu_);
        busy_ = "thinking of other words for your search";
      }
      wake();
      const json more = checker_.expand(goal);
      json terms;
      {
        std::lock_guard lock(mu_);
        busy_.clear();
        // Only if you have not typed something else in the meantime.
        if (settings_.goal == goal && job.data.value("seq", -1) == goal_seq_) {
          std::set<std::string> seen;
          for (const json& t : terms_) seen.insert(t.get<std::string>());
          const size_t before = terms_.size();
          for (const json& t : more)
            if (t.is_string() && terms_.size() < 20 && !stopword(t.get<std::string>()) && seen.insert(t.get<std::string>()).second)
              terms_.push_back(t);
          if (terms_.size() > before) terms = terms_;
        }
      }
      if (terms.is_array()) bridge_.broadcast({{"type", "goal"}, {"goal", goal}, {"terms", terms}});
    }
    wake();
  }
  CoUninitialize();
}

// A search typed in the window: every spider learns the words at once, the
// spider on the page you are looking at goes hunting, and the model adds
// synonyms a moment later.
void App::commit_goal(const std::string& goal, bool force) {
  int client = -1;
  json terms;
  {
    std::lock_guard lock(mu_);
    if (goal == settings_.goal && !force) return;
    terms_ = quick_terms(goal);
    terms = terms_;
    ++goal_seq_;
    std::erase_if(urgent_, [](const Job& j) { return j.type == Job::Expand; });
    if (!goal.empty()) {
      urgent_.push_front({Job::Expand, "", "", {{"goal", goal}, {"seq", goal_seq_}}});
      cv_.notify_all();
    }
    const Browser* b = current(&client);
    if (!goal.empty() && (!b || !b->web)) {
      notice_ = b ? "Open a web page in your browser: the spider can't go on this one." : "No browser is connected.";
      notice_at_ = now_seconds();
    }
  }
  apply_settings({{"goal", goal}});  // saved, and the page is ranked against it
  const json hunt = {{"type", "hunt"}, {"goal", goal}, {"terms", terms}};
  const json words = {{"type", "goal"}, {"goal", goal}, {"terms", terms}};
  std::vector<int> all;
  {
    std::lock_guard lock(mu_);
    for (const auto& [c, b] : browsers_) all.push_back(c);
  }
  for (int c : all) bridge_.send(c, c == client && !goal.empty() ? hunt : words);
}

void App::send_spider(bool on) {
  int client = -1;
  json msg;
  {
    std::lock_guard lock(mu_);
    const Browser* b = current(&client);
    if (!b) return;
    msg = {{"type", "spider"}, {"on", on}, {"tabId", b->tab}};
  }
  bridge_.send(client, msg);
}

// ---------------------------------------------------------------- window

bool App::create_window(HINSTANCE inst, bool show_now) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &App::proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x0F, 0x14));
  wc.lpszClassName = L"SpiderPetApp";
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"SpiderPet", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 820,
                          nullptr, nullptr, inst, this);
  if (!hwnd_) return false;
  scale_ = GetDpiForWindow(hwnd_) / 96.f;
  SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(560 * scale_), static_cast<int>(820 * scale_),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
  const COLORREF caption = 0x00140F0E;
  DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));

  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device_,
                               nullptr, &ctx_)))
    return false;
  ComPtr<IDXGIDevice> dxgi;
  device_.As(&dxgi);
  ComPtr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_))) return false;
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  make_target();

  IMGUI_CHECKVERSION();
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  // Latin, Greek, Cyrillic and general punctuation: titles and names in other scripts.
  static const ImWchar ranges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
  body_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.f * scale_, &cfg, ranges);
  bold_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 16.f * scale_, &cfg, ranges);
  title_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 24.f * scale_, &cfg, ranges);
  small_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 13.5f * scale_, &cfg, ranges);
  if (!body_) body_ = io.Fonts->AddFontDefault();
  if (!bold_) bold_ = body_;
  if (!title_) title_ = body_;
  if (!small_) small_ = body_;
  style();
  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());
  if (show_now) show();
  return true;
}

void App::make_target() {
  rtv_.Reset();
  ComPtr<ID3D11Texture2D> back;
  if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
}

void App::destroy_window() {
  if (imgui_) {
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
  }
  rtv_.Reset();
  swap_.Reset();
  ctx_.Reset();
  device_.Reset();
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

void App::show() {
  ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd_);
  visible_ = true;
  focus_goal_ = true;
}

void App::hide() {
  ShowWindow(hwnd_, SW_HIDE);
  visible_ = false;
}

void App::tray_add() {
  tray_.cbSize = sizeof(tray_);
  tray_.hWnd = hwnd_;
  tray_.uID = 1;
  tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  tray_.uCallbackMessage = kTrayMsg;
  tray_.hIcon = LoadIconW(inst_, MAKEINTRESOURCEW(1));
  wcscpy_s(tray_.szTip, L"SpiderPet");
  Shell_NotifyIconW(NIM_ADD, &tray_);
}

void App::tray_remove() { Shell_NotifyIconW(NIM_DELETE, &tray_); }

LRESULT CALLBACK App::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && (self->hwnd_ == hwnd || !self->hwnd_)) {
    if (!self->hwnd_) self->hwnd_ = hwnd;
    return self->handle(msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (imgui_) {
    ImGui::SetCurrentContext(imgui_);
    if (ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wp, lp)) return TRUE;
  }
  switch (msg) {
    case WM_SIZE:
      if (swap_ && wp != SIZE_MINIMIZED) {
        rtv_.Reset();
        swap_->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        make_target();
      }
      if (wp == SIZE_MINIMIZED) visible_ = false;
      else if (IsWindowVisible(hwnd_)) visible_ = true;
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {static_cast<LONG>(420 * scale_), static_cast<LONG>(480 * scale_)};
      return 0;
    }
    case WM_CLOSE:
      hide();  // the browser stays connected; quit from the tray
      return 0;
    case kShowMsg:
      show();
      return 0;
    case kTrayMsg:
      if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
        show();
      } else if (LOWORD(lp) == WM_RBUTTONUP) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Open SpiderPet");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, 2, L"Quit");
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(hwnd_);
        const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd_, nullptr);
        DestroyMenu(m);
        if (cmd == 1) show();
        if (cmd == 2) quitting_ = true;
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::style() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.ChildRounding = 8;
  s.FrameRounding = 6;
  s.GrabRounding = 6;
  s.PopupRounding = 6;
  s.TabRounding = 6;
  s.WindowPadding = ImVec2(18, 16);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(8, 8);
  s.ScrollbarSize = 10;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = hexv(0x0E0F14);
  c[ImGuiCol_ChildBg] = hexv(0x15161D);
  c[ImGuiCol_PopupBg] = hexv(0x1B1C25);
  c[ImGuiCol_Text] = hexv(0xE9EBF2);
  c[ImGuiCol_TextDisabled] = hexv(0x8A8FA3);
  c[ImGuiCol_FrameBg] = hexv(0x1E2029);
  c[ImGuiCol_FrameBgHovered] = hexv(0x272A36);
  c[ImGuiCol_FrameBgActive] = hexv(0x2E3240);
  c[ImGuiCol_Button] = hexv(0x22242F);
  c[ImGuiCol_ButtonHovered] = hexv(0x2F3242);
  c[ImGuiCol_ButtonActive] = hexv(0x3A3E52);
  c[ImGuiCol_CheckMark] = hexv(0x8BF5A6);
  c[ImGuiCol_Header] = hexv(0x1A1B23);
  c[ImGuiCol_HeaderHovered] = hexv(0x23252F);
  c[ImGuiCol_HeaderActive] = hexv(0x2A2D3B);
  c[ImGuiCol_Tab] = hexv(0x15161D);
  c[ImGuiCol_TabHovered] = hexv(0x2A2D3B);
  c[ImGuiCol_TabSelected] = hexv(0x262838);
  c[ImGuiCol_TabSelectedOverline] = hexv(0xE64CF2);
  c[ImGuiCol_Separator] = hexv(0xFFFFFF, 0.08f);
  c[ImGuiCol_ScrollbarBg] = hexv(0x000000, 0);
  c[ImGuiCol_ScrollbarGrab] = hexv(0x2E3140);
  c[ImGuiCol_Border] = hexv(0xFFFFFF, 0.06f);
  s.ScaleAllSizes(scale_);
}

void App::draw() {
  if (!rtv_ || IsIconic(hwnd_)) {
    Sleep(30);
    return;
  }
  ImGui::SetCurrentContext(imgui_);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##app", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  {
    std::lock_guard lock(mu_);
    draw_header();
    if (ImGui::BeginTabBar("##tabs")) {
      if (ImGui::BeginTabItem("This page")) {
        draw_page();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Library")) {
        draw_library();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Settings")) {
        draw_settings();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Setup")) {
        draw_setup();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  if (!pending_.is_null()) {
    const json partial = std::move(pending_);
    pending_ = nullptr;
    apply_settings(partial);
  }
  if (pending_goal_) {
    const std::string g = *pending_goal_;
    pending_goal_.reset();
    commit_goal(g, goal_force_);
    goal_force_ = false;
  }
  if (pending_spider_) {
    const bool on = *pending_spider_;
    pending_spider_.reset();
    send_spider(on);
  }
  ImGui::Render();
  const float clear[4] = {0.055f, 0.059f, 0.078f, 1.f};
  ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  swap_->Present(1, 0);
}

void App::draw_header() {
  ImGui::PushFont(title_, title_->LegacySize);
  ImGui::TextUnformatted("SpiderPet");
  ImGui::PopFont();

  // One status line: browsers, the model, online checks, the queue.
  ImGui::PushFont(small_, small_->LegacySize);
  auto dot = [&](bool ok) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 4.f * scale_;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r,
                                                ImGui::GetColorU32(hexv(ok ? 0x8BF5A6 : 0xFF5C5C)));
    ImGui::Dummy(ImVec2(r * 2 + 4 * scale_, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, 0);
  };
  std::set<std::string> names;
  for (auto& [c, br] : browsers_)
    if (!br.name.empty()) names.insert(br.name);
  std::string b;
  for (const auto& n : names) b += (b.empty() ? "" : ", ") + n;
  dot(!names.empty());
  ImGui::Text(names.empty() ? "no browser connected" : "%s connected", b.c_str());
  ImGui::SameLine(0, 18 * scale_);
  const bool ai = status_.value("ollama", false) && !jstr(status_, "model").empty();
  dot(ai);
  if (ai) ImGui::Text("AI: %s", jstr(status_, "model").c_str());
  else ImGui::TextUnformatted(status_.value("ollama", false) ? "AI: no model installed" : "AI: Ollama is not running");
  ImGui::SameLine(0, 18 * scale_);
  dot(settings_.online);
  ImGui::TextUnformatted(settings_.online ? "online checks on" : "online checks off");
  if (checker_.gpu_busy())
    ImGui::TextColored(hexv(0xFFC46B), "AI paused: another app is using the graphics card memory. Ids are still checked.");
  const size_t waiting = queued_.size();
  if (!busy_.empty() || waiting) {
    ImGui::TextDisabled("%s%s", busy_.empty() ? "" : busy_.c_str(),
                        waiting ? ("   (" + std::to_string(waiting) + " waiting)").c_str() : "");
  }
  ImGui::PopFont();

  // The search and the spider's switch. Typing is enough: a short pause sends
  // the spider after it; Enter sends it again (on a new page, say).
  const Browser* cur = current();
  const bool on = cur && cur->spider;
  const char* label = on ? "Stop spider" : "Start spider";
  const float bw = ImGui::CalcTextSize("Start spider").x + ImGui::GetStyle().FramePadding.x * 2 + 8 * scale_;
  ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
  if (focus_goal_) {
    ImGui::SetKeyboardFocusHere();
    focus_goal_ = false;
  }
  const bool enter = ImGui::InputTextWithHint("##goal", "What are you looking for? Type it and the spider hunts for it.", goal_,
                                              sizeof(goal_), ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemEdited()) {
    goal_dirty_ = true;
    goal_edit_at_ = now_seconds();
  }
  if (enter || (goal_dirty_ && now_seconds() - goal_edit_at_ > 0.7)) {
    goal_dirty_ = false;
    goal_force_ = enter;
    pending_goal_ = std::string(goal_);  // sent after this frame, outside the lock
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!cur || (!cur->web && !on));
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, hexv(0x5A2368));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hexv(0x6E2C80));
  }
  if (ImGui::Button(label, ImVec2(bw, 0))) pending_spider_ = !on;
  if (on) ImGui::PopStyleColor(2);
  ImGui::EndDisabled();

  // Where the spider would go: the tab you are on in your browser.
  ImGui::PushFont(small_, small_->LegacySize);
  if (!cur) ImGui::TextDisabled("Open your browser: the spider works on the tab you are looking at.");
  else if (!cur->web) ImGui::TextDisabled("%s tab: a browser page. Open a web page for the spider.", cur->name.c_str());
  else
    ImGui::TextDisabled("%s tab: %s%s", cur->name.c_str(), cur->title.substr(0, 80).c_str(),
                        on ? (terms_.empty() ? "   (spider on)" : "   (spider hunting)") : "");
  ImGui::PopFont();
  if (!notice_.empty() && now_seconds() - notice_at_ < 6) ImGui::TextColored(hexv(0xFFC46B), "%s", notice_.c_str());
  ImGui::Spacing();
}

void App::draw_find(const json& f, bool show_page) {
  const std::string id = jstr(f, "id");
  const std::string kind = jstr(f, "kind");
  const json& v = f.contains("verdict") ? f["verdict"] : json();
  ImGui::PushID(id.c_str());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float pad = 4.f * scale_;

  // Kind tag and verdict pill on one line.
  ImGui::PushFont(small_, small_->LegacySize);
  std::string tag = kind == "id" && !jstr(f, "label").empty() ? jstr(f, "label") : kind;
  for (char& c : tag) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  ImGui::TextColored(hexv(kind_color(kind)), "%s", tag.c_str());
  const Pill pill = pill_for(v);
  if (pill.text[0]) {
    ImGui::SameLine();
    const ImVec2 sz = ImGui::CalcTextSize(pill.text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x - pad, p.y - 1), ImVec2(p.x + sz.x + pad, p.y + sz.y + 1), ImGui::GetColorU32(hexv(pill.bg)),
                      4.f * scale_);
    ImGui::TextColored(hexv(pill.fg), "%s", pill.text);
  }
  if (matches(f) > 0) {
    ImGui::SameLine();
    const char* mt = "MATCH";
    const ImVec2 sz = ImGui::CalcTextSize(mt);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p.x - pad, p.y - 1), ImVec2(p.x + sz.x + pad, p.y + sz.y + 1), ImGui::GetColorU32(hexv(0xFFC46B)),
                      4.f * scale_);
    ImGui::TextColored(hexv(0x1E120C), "%s", mt);
  }
  const int score = f.value("score", -1);
  if (score >= 0) {
    ImGui::SameLine(0, 12 * scale_);
    ImGui::TextDisabled("relevance %d/10", score);
  }
  ImGui::PopFont();

  // The find itself; click to see it on the page.
  ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 6 * scale_);
  ImGui::PushFont(kind == "heading" || kind == "title" ? bold_ : body_, body_->LegacySize);
  ImGui::TextUnformatted(jstr(f, "text").c_str());
  ImGui::PopFont();
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (ImGui::IsItemClicked()) reveal(jstr(f, "url"), id);

  ImGui::PushFont(small_, small_->LegacySize);
  if (v.is_object()) {
    const std::string note = jstr(v, "note");
    if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    if (v.contains("quote") && v["quote"].is_string() && !v["quote"].get<std::string>().empty())
      ImGui::TextColored(hexv(0xB9C0D8), "\"%s\"", jstr(v, "quote").c_str());
    if (v.contains("source") && v["source"].is_object() && !jstr(v["source"], "url").empty()) {
      const json& s = v["source"];
      ImGui::TextColored(hexv(0x7FD6FF), "source: %s", jstr(s, "name").c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", jstr(s, "url").c_str());
      }
      if (ImGui::IsItemClicked()) open_url(jstr(s, "url"));
    }
  }
  if (show_page) ImGui::TextDisabled("%s", jstr(f, "title").c_str());
  if (!jstr(f, "href").empty()) {
    ImGui::TextColored(hexv(0x7D8BFF), "%s", jstr(f, "href").substr(0, 90).c_str());
    if (ImGui::IsItemClicked()) open_url(jstr(f, "href"));
  }
  ImGui::PopFont();
  ImGui::PopTextWrapPos();

  ImGui::PushFont(small_, small_->LegacySize);
  if (ImGui::SmallButton("Show on page")) reveal(jstr(f, "url"), id);
  ImGui::SameLine();
  if (ImGui::SmallButton("Copy")) {
    copy_text(hwnd_, jstr(f, "text"));
    notice_ = "copied";
    notice_at_ = now_seconds();
  }
  if (checkable(f, settings_)) {
    ImGui::SameLine();
    if (ImGui::SmallButton(v.is_object() && jstr(v, "status") != "queued" && jstr(v, "status") != "checking" ? "Check again" : "Check")) {
      lib_.set_verdict(id, nullptr);
      queue_check(id, true);
    }
  }
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::PopID();
}

void App::draw_page() {
  const json* p = active_url_.empty() ? nullptr : lib_.page(active_url_);
  if (!p) {
    ImGui::Spacing();
    ImGui::TextWrapped("Open a page in your browser. Then type what you are looking for above, or press Start spider. "
                       "What the spider finds shows up here, with a check for each find.");
    if (!registered_) ImGui::TextColored(hexv(0xFF5C5C), "The browser connection is not set up. See Setup.");
    return;
  }
  ImGui::PushFont(bold_, bold_->LegacySize);
  ImGui::TextWrapped("%s", jstr(*p, "title").c_str());
  ImGui::PopFont();
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::TextColored(hexv(0x7D8BFF), "%s", active_url_.substr(0, 110).c_str());
  if (ImGui::IsItemClicked()) open_url(active_url_);
  ImGui::PopFont();
  if (p->contains("gist") && (*p)["gist"].is_object() && !(*p)["gist"].contains("error")) {
    const json& g = (*p)["gist"];
    ImGui::BeginChild("##gist", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(small_, small_->LegacySize);
    ImGui::TextColored(hexv(0xE64CF2), "%s", jstr(g, "type").c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("%s", jstr(g, "summary").c_str());
    if (g.contains("points") && g["points"].is_array())
      for (const json& pt : g["points"])
        if (pt.is_string()) ImGui::BulletText("%s", pt.get<std::string>().c_str());
    ImGui::EndChild();
  }
  ImGui::Spacing();

  std::vector<std::pair<int, const json*>> list;  // (how many search words it has, the find)
  for (const std::string& id : lib_.finds_of(active_url_)) {
    auto it = lib_.finds().find(id);
    if (it != lib_.finds().end()) list.push_back({matches(*it), &*it});
  }
  // What matches your search first, then the most relevant, problems before
  // quiet ones at the same relevance.
  std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
    if ((a.first > 0) != (b.first > 0)) return a.first > 0;
    const int sa = a.second->value("score", -1), sb = b.second->value("score", -1);
    if (sa != sb) return sa > sb;
    return problem((*a.second)["verdict"]) && !problem((*b.second)["verdict"]);
  });
  int verified = 0, problems = 0, matched = 0;
  for (const auto& [m, f] : list) {
    verified += jstr((*f)["verdict"], "status") == "verified" ? 1 : 0;
    problems += problem((*f)["verdict"]) ? 1 : 0;
    matched += m > 0 ? 1 : 0;
  }
  ImGui::PushFont(small_, small_->LegacySize);
  if (terms_.empty()) ImGui::TextDisabled("%zu finds   %d verified   %d problems", list.size(), verified, problems);
  else ImGui::TextDisabled("%d match your search   %zu finds   %d verified   %d problems", matched, list.size(), verified, problems);
  ImGui::PopFont();
  ImGui::BeginChild("##finds", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  for (const auto& [m, f] : list) draw_find(*f, false);
  ImGui::EndChild();
}

void App::draw_library() {
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  ImGui::InputTextWithHint("##search", "Search the library", search_, sizeof(search_));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1);
  const char* filters[] = {"All", "Verified", "Problems", "Unclear", "Not checked"};
  ImGui::Combo("##status", &status_filter_, filters, 5);
  const std::string q = lower(wide(search_)).empty() ? "" : utf8(lower(wide(search_)));

  ImGui::BeginChild("##lib", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  int shown = 0;
  for (const std::string& url : lib_.page_order()) {
    const json* p = lib_.page(url);
    if (!p) continue;
    std::vector<const json*> list;
    for (const std::string& id : lib_.finds_of(url)) {
      auto it = lib_.finds().find(id);
      if (it == lib_.finds().end()) continue;
      const json& f = *it;
      const std::string st = jstr(f["verdict"], "status");
      if (status_filter_ == 1 && st != "verified") continue;
      if (status_filter_ == 2 && !problem(f["verdict"])) continue;
      if (status_filter_ == 3 && st != "unverified") continue;
      if (status_filter_ == 4 && !st.empty() && st != "skipped") continue;
      if (!q.empty() && utf8(lower(wide(jstr(f, "text") + " " + jstr(f, "title")))).find(q) == std::string::npos) continue;
      list.push_back(&f);
    }
    if (list.empty()) continue;
    shown += static_cast<int>(list.size());
    const std::string head = (jstr(*p, "title").empty() ? url : jstr(*p, "title")) + "  (" + std::to_string(list.size()) + ")##" + url;
    if (ImGui::CollapsingHeader(head.c_str(), url == active_url_ ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
      for (const json* f : list) draw_find(*f, false);
      ImGui::PushFont(small_, small_->LegacySize);
      if (ImGui::SmallButton(("Remove this page##" + url).c_str())) {
        lib_.remove_page(url);
        ImGui::PopFont();
        break;
      }
      ImGui::PopFont();
    }
  }
  if (shown == 0) ImGui::TextDisabled("Nothing here yet.");
  ImGui::EndChild();
}

void App::draw_settings() {
  bool changed = false;
  Settings s = settings_;
  ImGui::Spacing();
  changed |= ImGui::Checkbox("Online checks", &s.online);
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::Indent();
  ImGui::TextWrapped("Looks each find up in Crossref, OpenLibrary, Google Books, PubMed, arXiv and Wikipedia. "
                     "Only the find leaves your PC (an id, a title, a few search words), never the page.");
  ImGui::Unindent();
  ImGui::PopFont();
  changed |= ImGui::Checkbox("Check every find as it is harvested", &s.auto_check);
  changed |= ImGui::Checkbox("Check claims in sentences the spider reads or that match your search (slower)", &s.check_sentences);
  changed |= ImGui::Checkbox("Let the spider scroll pages by itself", &s.crawl);

  ImGui::Spacing();
  ImGui::TextUnformatted("Model");
  ImGui::SetNextItemWidth(260 * scale_);
  const std::string current = s.model.empty() ? "best installed (" + jstr(status_, "model") + ")" : s.model;
  if (ImGui::BeginCombo("##model", current.c_str())) {
    if (ImGui::Selectable("best installed", s.model.empty())) s.model.clear(), changed = true;
    for (const char* m : {"qwen3.8-27b-uncensored-64k", "qwen3:8b", "qwen3:14b", "qwen2.5:7b", "qwen2.5:3b"})
      if (ImGui::Selectable(m, s.model == m)) s.model = m, changed = true;
    ImGui::EndCombo();
  }
  if (changed) pending_ = s.to_json();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Library");
  if (ImGui::Button("Save as JSON")) {
    const std::wstring path = documents_dir() + L"\\SpiderPet-export.json";
    lib_.export_json(path);
    open_path(documents_dir());
  }
  ImGui::SameLine();
  if (ImGui::Button("Save as CSV")) {
    const std::wstring path = documents_dir() + L"\\SpiderPet-export.csv";
    lib_.export_csv(path);
    open_path(documents_dir());
  }
  ImGui::SameLine();
  if (ImGui::Button("Open folder")) open_path(documents_dir());
  ImGui::Spacing();
  static double armed = 0;
  if (ImGui::Button(now_seconds() - armed < 3 ? "Click again to clear everything" : "Clear library")) {
    if (now_seconds() - armed < 3) {
      lib_.clear();
      armed = 0;
    } else {
      armed = now_seconds();
    }
  }
}

void App::draw_setup() {
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(hexv(registered_ ? 0x8BF5A6 : 0xFF5C5C), registered_ ? "Browsers know where SpiderPet is."
                                                                          : "Browser connection not set up.");
  if (!registered_ && !register_error_.empty()) ImGui::TextDisabled("%s", utf8(register_error_).c_str());
  if (ImGui::Button("Connect browsers again")) {
    const std::wstring host = exe_dir() + L"\\SpiderHost.exe";
    registered_ = register_native_host(host, &register_error_);
  }
  ImGui::Spacing();
  ImGui::PushFont(bold_, bold_->LegacySize);
  ImGui::TextUnformatted("Brave, Chrome, Edge");
  ImGui::PopFont();
  ImGui::TextUnformatted("1. Open brave://extensions (or chrome://extensions, edge://extensions).");
  ImGui::TextUnformatted("2. Turn on Developer mode.");
  ImGui::TextUnformatted("3. Click Load unpacked and pick the folder below.");
  const std::wstring chromium = exe_dir() + L"\\extension\\chromium";
  ImGui::TextColored(hexv(0x7D8BFF), "%s", utf8(chromium).c_str());
  if (ImGui::Button("Open that folder")) open_path(chromium);
  ImGui::Spacing();
  ImGui::PushFont(bold_, bold_->LegacySize);
  ImGui::TextUnformatted("Firefox");
  ImGui::PopFont();
  const std::wstring xpi = exe_dir() + L"\\extension\\spiderpet-firefox.xpi";
  if (GetFileAttributesW(xpi.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ImGui::TextUnformatted("Drag this file into a Firefox window and click Add:");
    ImGui::TextColored(hexv(0x7D8BFF), "%s", utf8(xpi).c_str());
    if (ImGui::Button("Show the file")) open_path(exe_dir() + L"\\extension");
  } else {
    ImGui::TextUnformatted("Firefox only installs extensions that Mozilla has signed.");
    ImGui::TextUnformatted("Run extension\\sign-firefox.ps1 once with your addons.mozilla.org API key; it makes "
                           "spiderpet-firefox.xpi here. Until then, load extension\\firefox\\manifest.json from "
                           "about:debugging > This Firefox > Load Temporary Add-on (it lasts until Firefox restarts).");
  }
  ImGui::Spacing();
  ImGui::PushFont(bold_, bold_->LegacySize);
  ImGui::TextUnformatted("Local AI");
  ImGui::PopFont();
  ImGui::TextUnformatted("Ollama runs the checks. The smartest installed model is used: Qwen 3.8 27B "
                         "(qwen3.8-27b-uncensored-64k) if you have it, then qwen3:8b and smaller ones.");
  ImGui::PopTextWrapPos();
}

}  // namespace sp

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool tray = false;
  for (int i = 1; i < argc; ++i)
    if (std::wstring(argv[i]) == L"--tray") tray = true;
  LocalFree(argv);

  // One app per user: a second start just brings the window up.
  HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\SpiderPet.App");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (!tray)
      if (HWND w = FindWindowW(L"SpiderPetApp", nullptr)) PostMessageW(w, WM_APP + 2, 0, 0);
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  sp::App app;
  const int code = app.run(inst, tray);
  CoUninitialize();
  if (once) CloseHandle(once);
  return code;
}
