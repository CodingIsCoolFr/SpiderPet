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
#include <cmath>
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
constexpr const char* kVersion = "3.4.0";

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

// Words that would match half of any page, and the words a question is built from.
bool stopword(const std::string& w) {
  static const std::set<std::string> s = {
      "the", "and", "for", "with", "about", "what", "what's", "whats", "how", "who", "who's", "why", "when", "where",
      "which", "that", "this", "these", "those", "there", "their", "they", "them", "are", "was", "were", "is", "from",
      "into", "its", "it's", "find", "show", "look", "looking", "info", "information", "anything", "something", "page",
      "pages", "site", "some", "any", "all", "more", "most", "best", "does", "did", "can", "could", "would", "should",
      "will", "you", "your", "have", "has", "had", "tell", "give", "list", "explain", "describe", "mean", "means"};
  return w.size() < 3 || s.count(w) > 0;
}

std::string lower_utf8(const std::string& s) { return utf8(lower(wide(s))); }

// Lowercase, trimmed, with typographic apostrophes made plain ("what’s" -> "what's").
std::string plain(const std::string& s) {
  std::string g = utf8(trim(lower(wide(s))));
  for (size_t at; (at = g.find("\xE2\x80\x99")) != std::string::npos;) g.replace(at, 3, "'");
  return g;
}

std::vector<std::string> words_in(const std::string& g) {
  std::vector<std::string> words;
  std::string word;
  for (const char c : g + " ") {
    // Letters, digits, and every byte of a non-English letter stay in the word.
    if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80 || c == '-' || c == '\'') {
      word += c;
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
    }
  }
  return words;
}

// What you typed: words to hunt for, a question about the page, or a request
// for what the page is about.
enum class Ask { None, About, Question };

Ask ask_kind(const std::string& goal) {
  const std::string g = plain(goal);
  if (g.empty()) return Ask::None;
  const auto has = [&](const char* s) { return g.find(s) != std::string::npos; };
  const std::vector<std::string> w = words_in(g);
  if (has("summar") || has("tl;dr") || has("tldr") || has("gist") || has("overview") || has("main point") || has("key point"))
    return Ask::About;
  if ((has("what") || has("whats")) && has("about")) return Ask::About;  // "what's this page about"
  if (g == "what is this" || g == "what's this" || g == "whats this" || g == "explain this" || g == "explain this page" ||
      g == "describe this page" || g == "what is this page" || g == "what's this page")
    return Ask::About;
  static const std::set<std::string> openers = {"what", "what's", "whats", "who", "who's", "whom", "whose", "when", "where",
                                                "why", "how", "which", "is", "are", "was", "were", "does", "do", "did",
                                                "can", "could", "should", "will", "would", "explain", "tell", "describe",
                                                "list", "has", "have"};
  if (g.back() == '?' || (w.size() >= 3 && openers.count(w.front()))) return Ask::Question;
  return Ask::None;
}

// The search as words the spider can match right away, before the model has
// thought of synonyms: the whole phrase, then each word that means something.
// A question's own wording is never on the page, so only its words count.
json quick_terms(const std::string& goal) {
  json out = json::array();
  std::set<std::string> seen;
  auto put = [&](const std::string& t) {
    if (!t.empty() && seen.insert(t).second) out.push_back(t);
  };
  const std::string g = plain(goal);
  if (g.empty()) return out;
  const Ask kind = ask_kind(goal);
  if (kind == Ask::About) return out;  // nothing to hunt: the spider reads the page
  const std::vector<std::string> words = words_in(g);
  if (kind == Ask::None && words.size() > 1 && g.size() <= 60) put(g);
  for (const std::string& w : words)
    if (!stopword(w) || (words.size() == 1 && kind == Ask::None)) put(w);
  return out;
}

// The page's own sentences, for a quick answer when the AI cannot run.
std::vector<std::string> sentences_of(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    std::string s = utf8(trim(wide(cur)));
    cur.clear();
    if (s.size() < 40 || s.size() > 400 || words_in(s).size() < 6) return;
    const std::string l = plain(s);
    for (const char* junk : {"cookie", "sign in", "log in", "sign up", "subscribe", "privacy", "terms of", "javascript"})
      if (l.find(junk) != std::string::npos) return;
    // Notes about the page rather than its content ("For other uses, see...").
    for (const char* note : {"for other uses", "this article is about", "not to be confused", "redirects here", "see also",
                             "jump to", "from wikipedia"})
      if (l.rfind(note, 0) == 0 || l.find(std::string("\"") + note) == 0) return;
    out.push_back(s);
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n') {
      flush();
      continue;
    }
    cur += c;
    if ((c == '.' || c == '!' || c == '?') && (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\n')) flush();
  }
  flush();
  return out;
}

// A quick answer from the page itself: its own description and its leading
// sentences for "what is it about", the sentences sharing the most words with
// a question otherwise.
std::string quick_answer(Ask kind, const std::string& goal, const std::string& title, const std::string& desc,
                         const std::string& text) {
  const std::vector<std::string> sents = sentences_of(text);
  if (kind == Ask::About) {
    std::string out = title.empty() ? "" : "\"" + title + "\". ";
    if (desc.size() >= 40) {
      // Many pages (GitHub, shops) put the same words in the title and the description: say them once.
      const std::string t = plain(title), d = plain(desc);
      if (t.find(d.substr(0, 40)) != std::string::npos || d.find(t.substr(0, std::min<size_t>(40, t.size()))) != std::string::npos)
        return desc;
      return out + desc;
    }
    for (size_t i = 0; i < sents.size() && i < 2; ++i) out += sents[i] + " ";
    return out.empty() ? "The page has too little text to say." : out;
  }
  // The question's words, cut to their stem ("spiders" finds "spider", "eat" finds "eaten" but not "breathe").
  std::vector<std::string> want;
  for (const std::string& w : words_in(plain(goal)))
    if (!stopword(w)) want.push_back(w.size() > 5 ? w.substr(0, 5) : w);
  std::vector<std::vector<bool>> has(sents.size(), std::vector<bool>(want.size(), false));
  std::vector<int> df(want.size(), 0);
  for (size_t i = 0; i < sents.size(); ++i) {
    const std::vector<std::string> ws = words_in(plain(sents[i]));
    for (size_t k = 0; k < want.size(); ++k) {
      for (const std::string& w : ws)
        if (w.rfind(want[k], 0) == 0) {
          has[i][k] = true;
          ++df[k];
          break;
        }
    }
  }
  // A word every sentence has (the page's own subject) says little; a rare one says a lot.
  std::vector<std::pair<double, size_t>> scored;
  for (size_t i = 0; i < sents.size(); ++i) {
    double s = 0;
    for (size_t k = 0; k < want.size(); ++k)
      if (has[i][k]) s += 1.0 / (1.0 + std::log(1.0 + df[k]));
    if (s > 0) scored.push_back({s, i});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (scored.empty()) return "The page does not seem to say.";
  std::string out;
  for (size_t k = 0; k < scored.size() && k < 2; ++k) out += sents[scored[k].second] + " ";
  return out;
}

// ---------------------------------------------------------------- commands

// English letters made small, every other byte left as it is, so a position
// in the copy is the same position in what you typed.
std::string ascii_lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string trim_cmd(const std::string& s) {
  std::string t = utf8(trim(wide(s)));
  while (!t.empty() && (t.back() == '.' || t.back() == '!' || t.back() == ' ')) t.pop_back();
  return t;
}

std::string unquote(std::string s) {
  s = trim_cmd(s);
  for (const char* q : {"\"", "'", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE2\x80\x98", "\xE2\x80\x99"}) {
    const size_t n = strlen(q);
    if (s.size() >= n && s.compare(0, n, q) == 0) s.erase(0, n);
    if (s.size() >= n && s.compare(s.size() - n, n, q) == 0) s.erase(s.size() - n);
  }
  return s;
}

bool is_quoted(const std::string& s) {
  const std::string t = trim_cmd(s);
  return !t.empty() && (t[0] == '"' || t[0] == '\'' || t.rfind("\xE2\x80\x9C", 0) == 0);
}

// "youtube.com", "https://en.wikipedia.org/wiki/Spider" -> a web address; "youtube" -> nothing.
std::string as_url(const std::string& s) {
  const std::string t = unquote(s);
  if (t.empty() || t.find(' ') != std::string::npos) return "";
  const std::string lo = ascii_lower(t);
  if (lo.rfind("http://", 0) == 0 || lo.rfind("https://", 0) == 0) return t;
  const std::string host = lo.substr(0, lo.find('/'));
  const size_t dot = host.rfind('.');
  if (dot == std::string::npos || dot == 0 || host.size() - dot - 1 < 2 || host.size() - dot - 1 > 24) return "";
  for (size_t i = dot + 1; i < host.size(); ++i)
    if (host[i] < 'a' || host[i] > 'z') return "";
  return "https://" + t;
}

// One step of a command, or null when it is not one.
json parse_step(const std::string& part) {
  const std::string p = trim_cmd(part);
  const std::string lo = ascii_lower(p);
  if (p.empty()) return nullptr;
  auto is = [&](std::initializer_list<const char*> all) {
    for (const char* a : all)
      if (lo == a) return true;
    return false;
  };
  // The words after one of these openers, as you typed them.
  std::string rest;
  auto after = [&](std::initializer_list<const char*> openers) {
    for (const char* o : openers) {
      const size_t n = strlen(o);
      if (lo.size() > n && lo.compare(0, n, o) == 0) {
        rest = trim_cmd(p.substr(n));
        if (!rest.empty()) return true;
      }
    }
    return false;
  };
  if (is({"scroll", "scroll down", "scroll down a bit", "page down"})) return {{"verb", "scroll"}, {"dir", "down"}};
  if (is({"scroll up", "scroll up a bit", "page up"})) return {{"verb", "scroll"}, {"dir", "up"}};
  if (is({"scroll to top", "scroll to the top", "go to top", "go to the top", "back to top", "back to the top"}))
    return {{"verb", "scroll"}, {"dir", "top"}};
  if (is({"scroll to bottom", "scroll to the bottom", "go to bottom", "go to the bottom"})) return {{"verb", "scroll"}, {"dir", "bottom"}};
  if (is({"back", "go back", "go back a page", "previous page"})) return {{"verb", "back"}};
  if (is({"forward", "go forward"})) return {{"verb", "forward"}};
  if (is({"reload", "refresh", "reload page", "refresh page", "reload the page", "refresh the page"})) return {{"verb", "reload"}};
  if (is({"press enter", "hit enter", "press return", "submit", "submit it"})) return {{"verb", "enter"}};

  // Going somewhere: a web address opens in the tab; anything else is a link to click.
  if (after({"go to ", "goto ", "visit ", "navigate to ", "open up ", "open "})) {
    const std::string url = as_url(rest);
    if (!url.empty()) return {{"verb", "goto"}, {"url", url}};
    // "open source" is something to hunt for; "open the menu" is a command.
    const std::string r = ascii_lower(rest);
    const bool opener_is_open = lo.rfind("open", 0) == 0;
    const bool thing = r.rfind("the ", 0) == 0 || r.rfind("my ", 0) == 0 || is_quoted(rest);
    const bool named = r.ends_with(" link") || r.ends_with(" button") || r.ends_with(" tab") || r.ends_with(" menu") ||
                       r.ends_with(" page") || r.ends_with(" settings");
    if (!opener_is_open || thing || named) return {{"verb", "click"}, {"what", unquote(rest)}};
    return nullptr;
  }
  if (after({"click on ", "click ", "tap on ", "tap ", "press on ", "press the ", "hit the ", "select the ", "choose the ",
             "tick the ", "untick the ", "uncheck the ", "toggle the "})) {
    // "press the X" keeps "the" out of what to find; the page matcher drops it anyway.
    return {{"verb", "click"}, {"what", unquote(rest)}};
  }
  if (after({"search the site for ", "search this site for ", "search the website for ", "search this website for "}))
    return {{"verb", "type"}, {"text", unquote(rest)}, {"what", "search"}, {"enter", true}};
  if (after({"fill in ", "fill "})) {
    const size_t at = ascii_lower(rest).rfind(" with ");
    if (at != std::string::npos)
      return {{"verb", "type"}, {"what", unquote(rest.substr(0, at))}, {"text", unquote(rest.substr(at + 6))}, {"enter", false}};
    return nullptr;
  }
  if (after({"type ", "write ", "enter ", "put ", "input "})) {
    bool enter = false;
    std::string r = rest;
    for (const char* tail : {" and press enter", " and hit enter", " and press return", " and submit", " and search", " and go"}) {
      const std::string rl = ascii_lower(r);
      if (rl.ends_with(tail)) {
        r = trim_cmd(r.substr(0, r.size() - strlen(tail)));
        enter = true;
        break;
      }
    }
    // The last " into " / " in " splits what to type from where: "type log in into search".
    const std::string rl = ascii_lower(r);
    size_t at = rl.rfind(" into ");
    size_t skip = 6;
    if (at == std::string::npos) at = rl.rfind(" in "), skip = 4;
    if (at == std::string::npos) return nullptr;
    const std::string text = r.substr(0, at), where = trim_cmd(r.substr(at + skip));
    // "type 2 diabetes in children" is a search; "type cats into the search box" is a command.
    const std::string wl = ascii_lower(where);
    bool box = is_quoted(text) || wl.rfind("the ", 0) == 0;
    for (const char* w : {"search", "box", "field", "bar", "input", "textbox", "text area", "comment", "message"})
      if (wl.find(w) != std::string::npos) box = true;
    if (!box || text.empty() || where.empty()) return nullptr;
    return {{"verb", "type"}, {"text", unquote(text)}, {"what", unquote(where)}, {"enter", enter}};
  }
  return nullptr;
}

// A command for the spider ("click Sign in", "type cats into the search box
// then press enter"): its steps, or an empty list for words to hunt or a question.
json parse_task(const std::string& typed) {
  const std::string all = trim_cmd(typed);
  const std::string lo = ascii_lower(all);
  std::vector<std::string> parts;
  for (size_t from = 0;;) {
    size_t best = std::string::npos, len = 0;
    for (const char* sep : {", and then ", " and then ", ", then ", " then ", "; "}) {
      const size_t at = lo.find(sep, from);
      if (at < best) best = at, len = strlen(sep);
    }
    parts.push_back(all.substr(from, best == std::string::npos ? std::string::npos : best - from));
    if (best == std::string::npos) break;
    from = best + len;
  }
  json steps = json::array();
  for (const std::string& part : parts) {
    json s = parse_step(part);
    if (s.is_null()) {
      if (steps.empty()) return json::array();  // it does not start like a command: words to hunt
      s = {{"verb", "click"}, {"what", unquote(part)}};  // "click Menu then Settings"
    }
    if (jstr(s, "verb") == "enter") {
      // "type cats into search then press enter": Enter belongs to the typing.
      if (steps.empty() || jstr(steps.back(), "verb") != "type") return json::array();
      steps.back()["enter"] = true;
      continue;
    }
    steps.push_back(s);
  }
  return steps.size() <= 8 ? steps : json::array();
}

// What a step will do, before the spider has found the thing on the page.
std::string step_text(const json& s) {
  const std::string v = jstr(s, "verb");
  if (v == "scroll") return "Scroll " + jstr(s, "dir");
  if (v == "back") return "Go back";
  if (v == "forward") return "Go forward";
  if (v == "reload") return "Reload the page";
  if (v == "goto") return "Open " + jstr(s, "url");
  if (v == "type")
    return "Type \"" + jstr(s, "text") + "\" into " + jstr(s, "what") + (s.value("enter", false) ? " and press Enter" : "");
  return "Click " + jstr(s, "what");
}

// A goal in your own words ("play a cat video on YouTube", "please sign me up
// for the newsletter"): the local AI plans it step by step on the page.
// Simple commands ("click Sign in"), searches and questions are not goals.
bool is_agent_task(const std::string& typed) {
  std::string g = ascii_lower(trim_cmd(typed));
  bool polite = false;
  for (const char* p : {"please ", "can you ", "could you ", "would you ", "will you ", "spider, ", "spider ", "task: ", "do: "})
    if (g.rfind(p, 0) == 0) {
      g = g.substr(strlen(p));
      polite = true;
    }
  const std::vector<std::string> w = words_in(g);
  if (w.size() < 2) return false;
  if (polite) return true;
  // Two things to do: "find a cat video and play it", "go to youtube and search cats".
  static const std::set<std::string> verbs = {
      "play", "watch", "listen", "buy", "order", "book", "subscribe", "unsubscribe", "download", "reply", "comment",
      "post", "send", "share", "like", "follow", "unfollow", "star", "apply", "register", "join", "sign", "log", "login",
      "create", "start", "save", "bookmark", "take", "complete", "finish", "add", "remove", "delete", "cancel", "accept",
      "decline", "compare", "help", "fill", "click", "open", "search", "type", "pick", "choose", "select", "show", "tell",
      "read", "go", "find", "get", "check", "scroll", "press", "tap", "visit", "write", "enter", "upvote", "downvote"};
  for (size_t i = 1; i + 1 < w.size(); ++i)
    if (w[i] == "and" && verbs.count(w[i + 1]) && w[i + 1] != "go" && w[i + 1] != "get") return true;
  const std::string& v = w[0];
  if ((v == "sign" || v == "log") && (w[1] == "in" || w[1] == "up" || w[1] == "out" || w[1] == "me")) return true;
  if (v == "show" && w[1] == "me") return w.size() >= 3;
  if (v == "search") return w.size() >= 3 && w[1] != "for" && w[1] != "and";
  if (v == "take" && w[1] == "me") return true;
  static const std::set<std::string> starts = {
      "play", "watch", "listen", "buy", "order", "book", "subscribe", "unsubscribe", "download", "reply", "comment",
      "post", "send", "share", "like", "follow", "unfollow", "star", "apply", "register", "join", "login", "create",
      "start", "save", "bookmark", "complete", "finish", "add", "remove", "delete", "cancel", "accept", "decline",
      "compare", "help", "fill", "upvote", "downvote"};
  return starts.count(v) && w.size() >= 3;
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
  void forget_page(const std::string& url);  // called with mu_ held
  void forget_all();                         // called with mu_ held
  bool confirm(const char* label, const char* again, const std::string& id);
  void send_spider(bool on);
  int matches(const json& f) const;
  // commands: the spider does it on the page, one step at a time, after your "Do it"
  void start_task(const std::string& command, const json& steps);  // called with mu_ held
  void task_tick();
  void task_stop(const std::string& note);  // called with mu_ held
  void task_next(bool ok = true, const std::string& note = "");  // the step is over; called with mu_ held
  void start_agent(const std::string& goal);                    // called with mu_ held
  void agent_step(const json& action, int seq);                 // the AI's next step; called with mu_ held
  void draw_task();                         // called with mu_ held

  struct Job {
    enum Type { Check, Rank, Gist, Expand, Answer, Blockers, Pick, Agent } type;
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
  // The answer to what you asked (a question, or what the page is about).
  struct Reply {
    std::string goal, url, text, quote;
    bool busy = false;
    bool by_ai = false;
  } reply_;
  struct PageText {
    std::string title, desc, text;
  };
  std::map<std::string, PageText> texts_;  // the words of recent pages, for answers
  void queue_answer();                     // called with mu_ held
  // A command you typed, step by step.
  struct Task {
    enum State { Idle, Waiting, Snapping, Thinking, Finding, Ready, Doing, Done, Failed } state = Idle;
    bool agent = false;                // a goal the AI plans, not a list of steps you gave
    int actions = 0, fails = 0;        // a goal: steps taken, failures in a row
    std::vector<std::string> history;  // a goal: what was done, for the AI
    std::string say;                   // a goal: the AI's last words (its plan, or the result)
    bool auto_go = false;              // a safe step: it goes by itself at auto_at
    double auto_at = 0;
    std::string command;
    json steps = json::array();
    size_t at = 0;
    int seq = 0;  // the step on its way to the page; late answers about older steps are dropped
    int client = -1, tab = -1;
    std::string desc, note;
    bool warn = false;
    double since = 0;  // when this state began (Waiting: when the next step may go)
    std::vector<std::pair<std::string, bool>> log;  // finished steps
  } task_;
  int act_seq_ = 0;
  std::vector<std::pair<int, json>> outbox_;  // sent after mu_ is let go
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
  std::string armed_;        // the forget button that was clicked once and waits for the second click
  double armed_at_ = -100;
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
    task_tick();
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

// Invisible marks that ask for a color emoji or glue emoji together (U+FE0E,
// U+FE0F, U+200D). The app draws emoji in one color, where they only leave gaps.
static std::string drop_marks(std::string s) {
  for (const char* mark : {"\xEF\xB8\x8E", "\xEF\xB8\x8F", "\xE2\x80\x8D"})
    for (size_t at; (at = s.find(mark)) != std::string::npos;) s.erase(at, 3);
  return s;
}

static json without_marks(const json& j) {
  if (j.is_string()) return drop_marks(j.get<std::string>());
  if (!j.is_structured()) return j;
  json out = j;
  for (json& v : out) v = without_marks(v);
  return out;
}

void App::on_message(int client, const json& raw) {
  const json m = without_marks(raw);
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
    // A question follows you to the next page: it is about the page you are on.
    if (!reply_.goal.empty() && b.web && !b.url.empty() && b.url != reply_.url) {
      reply_ = {reply_.goal, b.url, "", "", true, false};
      queue_answer();
    }
    wake();
    return;
  }
  if (type == "notice") {
    notice_ = jstr(m, "text");
    notice_at_ = now_seconds();
    wake();
    return;
  }
  if (type == "blockers") {
    // Something floats over a page and the spider can't tell if it is a popup: the AI decides, first in line.
    urgent_.push_front({Job::Blockers, jstr(m, "url"), "", {{"client", client}, {"items", m.value("items", json::array())}}});
    cv_.notify_all();
    return;
  }
  if (type == "page") {
    const std::string url = jstr(m, "url");
    if (url.empty()) return;
    if (!current()) active_url_ = url;  // a browser that does not say which tab you are on
    lib_.touch_page(url, jstr(m, "title"), jstr(m, "lang"));
    if (texts_.size() > 40) texts_.erase(texts_.begin());
    texts_[url] = {jstr(m, "title"), jstr(m, "desc"), jstr(m, "text")};
    if (!reply_.goal.empty() && (reply_.url == url || reply_.url.empty()) && reply_.text.empty()) {
      reply_.url = url;
      queue_answer();
    }
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
  if (type == "snap") {
    // A fresh look at the page for the goal: the AI decides the next step.
    if (m.value("seq", -1) != task_.seq || task_.state != Task::Snapping) return;
    const json snap = m.value("snap", json());
    if (!snap.is_object()) {
      task_stop(jstr(m, "note").empty() ? "The spider can't see this page." : jstr(m, "note"));
      return;
    }
    task_.client = client;
    task_.tab = m.value("tabId", -1);
    task_.state = Task::Thinking;
    task_.since = now_seconds();
    json hist = json::array();
    for (const std::string& h : task_.history) hist.push_back(h);
    urgent_.push_front({Job::Agent, jstr(snap, "url"), "", {{"seq", task_.seq}, {"goal", task_.command}, {"history", hist}, {"snap", snap}}});
    cv_.notify_all();
    wake();
    return;
  }
  if (type == "act-ready" || type == "act-ask" || type == "act-done") {
    // The spider's answer about the step it was given. Older steps are done with.
    if (m.value("seq", -1) != task_.seq || client != task_.client) return;
    if (type == "act-ready" && task_.state == Task::Finding) {
      task_.state = Task::Ready;
      task_.desc = jstr(m, "desc");
      task_.warn = m.value("warn", false);
      task_.since = now_seconds();
      // Safe steps go by themselves after a moment (you see the spider hold it); risky ones wait for Do it.
      task_.auto_go = !task_.warn && !settings_.ask_every_step;
      task_.auto_at = now_seconds() + 0.9;
    } else if (type == "act-ask" && task_.state == Task::Finding) {
      // Not sure which thing you mean: the AI picks from the page's buttons and links.
      urgent_.push_front({Job::Pick, jstr(m, "url"), "",
                          {{"client", client}, {"tabId", task_.tab}, {"seq", task_.seq},
                           {"want", step_text(task_.steps[task_.at])}, {"items", m.value("items", json::array())}}});
      task_.since = now_seconds();
      cv_.notify_all();
    } else if (type == "act-done" && task_.state != Task::Idle && task_.state != Task::Done && task_.state != Task::Failed) {
      const std::string note = jstr(m, "note");
      task_next(m.value("ok", false), note.empty() ? "It did not work." : note);
    }
    wake();
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
        // Without the model these still answer: from the page's own sentences, or by the spider's rules.
        if (j.type == Job::Answer || j.type == Job::Blockers || j.type == Job::Pick) return false;
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
    } else if (job.type == Job::Answer) {
      const std::string goal = jstr(job.data, "goal");
      const Ask kind = ask_kind(goal);
      PageText page;
      json gist;
      {
        std::lock_guard lock(mu_);
        if (reply_.goal != goal || reply_.url != job.url || !texts_.count(job.url)) continue;  // you asked something else since
        page = texts_[job.url];
        if (const json* p = lib_.page(job.url); p && p->contains("gist") && (*p)["gist"].is_object() && !(*p)["gist"].contains("error"))
          gist = (*p)["gist"];
        busy_ = "answering: " + goal.substr(0, 60);
      }
      wake();
      std::string text, quote;
      bool by_ai = false;
      if (kind == Ask::About) {
        // What the page is about: its summary, made now if there is none yet.
        if (!gist.is_object()) {
          const json g = checker_.gist(page.title, job.url, page.text);
          if (!g.contains("error")) {
            gist = g;
            std::lock_guard lock(mu_);
            lib_.set_gist(job.url, g);
          }
        }
        if (gist.is_object()) {
          by_ai = true;
          text = jstr(gist, "summary");
          if (gist.contains("points") && gist["points"].is_array())
            for (const json& pt : gist["points"])
              if (pt.is_string()) text += "\n- " + pt.get<std::string>();
          bridge_.broadcast({{"type", "gist"}, {"url", job.url}, {"gist", gist}});
        }
      } else {
        const json a = checker_.answer(goal, page.title, page.text);
        if (a.is_object() && !jstr(a, "answer").empty()) {
          by_ai = true;
          text = jstr(a, "answer");
          quote = jstr(a, "quote");
        }
      }
      if (text.empty()) text = quick_answer(kind, goal, page.title, page.desc, page.text);  // the AI is paused or off
      {
        std::lock_guard lock(mu_);
        busy_.clear();
        if (reply_.goal == goal && reply_.url == job.url) reply_ = {goal, job.url, text, quote, false, by_ai};
      }
      bridge_.broadcast({{"type", "answer"}, {"url", job.url}, {"goal", goal}, {"text", text}});
    } else if (job.type == Job::Blockers) {
      {
        std::lock_guard lock(mu_);
        busy_ = "looking at a popup";
      }
      wake();
      const json d = checker_.blockers(job.data.value("items", json::array()));
      {
        std::lock_guard lock(mu_);
        busy_.clear();
      }
      json reply = {{"type", "blockers"}, {"url", job.url}};
      if (d.is_object()) reply["decisions"] = d;
      else reply["fallback"] = true;  // no AI now: the spider's own rules decide
      bridge_.send(job.data.value("client", -1), reply);
    } else if (job.type == Job::Agent) {
      {
        std::lock_guard lock(mu_);
        if (job.data.value("seq", -1) != task_.seq || task_.state != Task::Thinking) continue;  // you stopped it
        busy_ = "planning the next step";
      }
      wake();
      const json a = checker_.next_action(jstr(job.data, "goal"), job.data["history"], job.data["snap"]);
      std::lock_guard lock(mu_);
      busy_.clear();
      if (job.data.value("seq", -1) == task_.seq && task_.state == Task::Thinking) agent_step(a, task_.seq);
      continue;
    } else if (job.type == Job::Pick) {
      {
        std::lock_guard lock(mu_);
        if (job.data.value("seq", -1) != task_.seq || task_.state != Task::Finding) continue;  // you cancelled
        busy_ = "finding what you meant";
      }
      wake();
      // Without the AI (paused or off) the spider takes its own best guess; you still say yes first.
      const json p = checker_.pick(jstr(job.data, "want"), job.data.value("items", json::array()));
      {
        std::lock_guard lock(mu_);
        busy_.clear();
      }
      bridge_.send(job.data.value("client", -1), {{"type", "act-pick"},
                                                  {"seq", job.data.value("seq", -1)},
                                                  {"tabId", job.data.value("tabId", -1)},
                                                  {"index", p.is_object() ? p.value("index", -1) : -1},
                                                  {"why", p.is_object() ? jstr(p, "why") : ""}});
    }
    wake();
  }
  CoUninitialize();
}

// A search typed in the window: every spider learns the words at once, the
// spider on the page you are looking at goes hunting, and the model adds
// synonyms a moment later.
void App::commit_goal(const std::string& goal, bool force) {
  // A command ("click Sign in") is not a search: the spider does it instead of hunting.
  if (const json steps = parse_task(goal); !steps.empty()) {
    // "go to youtube and play a cat video" looks like a click on "youtube and play...": it is a goal.
    bool hidden_goal = false;
    for (const json& s : steps)
      if (jstr(s, "what").find(" and ") != std::string::npos && is_agent_task("x and " + jstr(s, "what"))) hidden_goal = true;
    std::lock_guard lock(mu_);
    if (hidden_goal) start_agent(goal);
    else start_task(goal, steps);
    return;
  }
  if (is_agent_task(goal)) {
    std::lock_guard lock(mu_);
    start_agent(goal);
    return;
  }
  int client = -1;
  json terms;
  {
    std::lock_guard lock(mu_);
    if (goal == settings_.goal && !force) return;
    const Ask kind = ask_kind(goal);
    terms_ = quick_terms(goal);
    terms = terms_;
    ++goal_seq_;
    std::erase_if(urgent_, [](const Job& j) { return j.type == Job::Expand || j.type == Job::Answer; });
    if (!goal.empty() && kind == Ask::None) {
      urgent_.push_front({Job::Expand, "", "", {{"goal", goal}, {"seq", goal_seq_}}});
      cv_.notify_all();
    }
    const Browser* b = current(&client);
    if (!goal.empty() && (!b || !b->web)) {
      notice_ = b ? "Open a web page in your browser: the spider can't go on this one." : "No browser is connected.";
      notice_at_ = now_seconds();
    }
    // A question gets an answer, from the page you are on.
    reply_ = {};
    if (kind != Ask::None) {
      reply_ = {goal, b && b->web ? b->url : active_url_, "", "", true, false};
      queue_answer();
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

void App::queue_answer() {
  if (reply_.goal.empty() || reply_.url.empty() || !texts_.count(reply_.url)) return;  // waits for the page's words
  std::erase_if(urgent_, [](const Job& j) { return j.type == Job::Answer; });
  urgent_.push_front({Job::Answer, reply_.url, "", {{"goal", reply_.goal}}});
  cv_.notify_all();
}

void App::forget_page(const std::string& url) {
  for (const std::string& id : lib_.finds_of(url)) queued_.erase(id);
  std::erase_if(urgent_, [&](const Job& j) { return j.url == url; });
  lib_.remove_page(url);
  notice_ = "Forgot that page.";
  notice_at_ = now_seconds();
}

// Everything it has harvested and every answer it remembers. Settings stay.
void App::forget_all() {
  lib_.clear();
  std::erase_if(urgent_, [](const Job& j) { return j.type != Job::Expand; });
  jobs_.clear();
  queued_.clear();
  checker_.forget();
  notice_ = "Forgot everything: the library and the remembered checks.";
  notice_at_ = now_seconds();
}

// A button that has to be clicked twice, so nothing is lost by a slip.
bool App::confirm(const char* label, const char* again, const std::string& id) {
  const bool armed = armed_ == id && now_seconds() - armed_at_ < 3;
  if (armed) {
    ImGui::PushStyleColor(ImGuiCol_Button, hexv(0x8A2B33));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hexv(0xA3333D));
  }
  const bool clicked = ImGui::SmallButton((std::string(armed ? again : label) + "##" + id).c_str());
  if (armed) ImGui::PopStyleColor(2);
  if (!clicked) return false;
  if (armed) {
    armed_.clear();
    return true;
  }
  armed_ = id;
  armed_at_ = now_seconds();
  return false;
}

// ---------------------------------------------------------------- commands

void App::start_task(const std::string& command, const json& steps) {
  if (task_.state == Task::Finding || task_.state == Task::Ready)
    outbox_.push_back({task_.client, {{"type", "act-cancel"}, {"seq", task_.seq}, {"tabId", task_.tab}}});
  task_ = Task{};
  task_.command = command;
  task_.steps = steps;
  task_.state = Task::Waiting;
  wake();
}

void App::start_agent(const std::string& goal) {
  start_task(goal, json::array());
  task_.agent = true;
  task_.say = "Looking at the page to plan the first step.";
}

void App::task_next(bool ok, const std::string& note) {
  const std::string what = task_.desc.empty() ? step_text(task_.steps[task_.at]) : task_.desc;
  task_.auto_go = false;
  if (task_.agent) {
    // A goal goes on after a failed step too: the AI sees what went wrong and tries something else.
    task_.log.push_back({what + (ok ? "" : " (" + note + ")"), ok});
    task_.history.push_back(what + (ok ? ": done" : ": failed, " + note));
    task_.desc.clear();
    task_.warn = false;
    task_.fails = ok ? 0 : task_.fails + 1;
    if (task_.fails >= 3) return task_stop("Stopped: three steps in a row did not work.");
    if (++task_.actions >= 25) return task_stop("Stopped after 25 steps. Give it a smaller goal, or the next part of it.");
    task_.state = Task::Waiting;
    task_.since = now_seconds() + 1.5;  // a click may have opened a new page: let it load before the next look
    return;
  }
  if (!ok) return task_stop(note);
  task_.log.push_back({what, true});
  task_.desc.clear();
  task_.warn = false;
  if (++task_.at < task_.steps.size()) {
    task_.state = Task::Waiting;
    task_.since = now_seconds() + 1.2;  // a click may have opened a new page: let it start loading
  } else {
    task_.state = Task::Done;
    task_.since = now_seconds();
  }
}

void App::task_stop(const std::string& note) {
  if (task_.state == Task::Finding || task_.state == Task::Ready)
    outbox_.push_back({task_.client, {{"type", "act-cancel"}, {"seq", task_.seq}, {"tabId", task_.tab}}});
  if (!task_.desc.empty()) task_.log.push_back({task_.desc, false});
  task_.desc.clear();
  task_.state = Task::Failed;
  task_.note = note;
  task_.since = now_seconds();
  wake();
}

// Sends the next step when it is due, gives up on a page that does not answer,
// and sends what the window queued while it held the lock.
void App::task_tick() {
  std::vector<std::pair<int, json>> out;
  {
    std::lock_guard lock(mu_);
    const double t = now_seconds();
    if (task_.state == Task::Waiting && t >= task_.since && task_.agent) {
      // A goal: look at the page you are on, then the AI picks the next step.
      int client = -1;
      if (!current(&client)) {
        task_stop("No browser is connected.");
      } else {
        task_.client = client;
        task_.seq = ++act_seq_;
        task_.state = Task::Snapping;
        task_.since = t;
        out.push_back({client, {{"type", "snap"}, {"seq", task_.seq}}});
        wake();
      }
    } else if (task_.state == Task::Snapping && t - task_.since > 30) {
      task_stop("The page did not answer. Is the spider allowed on it?");
    } else if (task_.state == Task::Ready && task_.auto_go && t >= task_.auto_at) {
      task_.auto_go = false;
      task_.state = Task::Doing;
      task_.since = t;
      out.push_back({task_.client, {{"type", "act-go"}, {"seq", task_.seq}, {"tabId", task_.tab}}});
      wake();
    } else if (task_.state == Task::Waiting && t >= task_.since) {
      int client = -1;
      const Browser* b = current(&client);
      const json& step = task_.steps[task_.at];
      const std::string verb = jstr(step, "verb");
      const bool on_page = verb == "click" || verb == "type" || verb == "scroll";
      if (!b) {
        task_stop("No browser is connected.");
      } else if (on_page && !b->web) {
        task_stop("Open a web page first: the spider can't go on browser pages.");
      } else {
        // Each step goes to the tab you are looking at (a click may have opened a new one).
        task_.client = client;
        task_.tab = b->tab;
        task_.seq = ++act_seq_;
        task_.state = Task::Finding;
        task_.since = t;
        task_.desc.clear();
        out.push_back({client, {{"type", "act"}, {"seq", task_.seq}, {"tabId", task_.tab}, {"step", step}, {"command", task_.command}}});
        wake();
      }
    } else if (task_.state == Task::Finding && t - task_.since > 40) {
      task_stop("The page did not answer. Try again, or start the spider on it first.");
    } else if (task_.state == Task::Doing && t - task_.since > 8) {
      task_next();  // the page moved on before the spider could say so: the click opened a new page
      wake();
    }
    for (auto& o : outbox_) out.push_back(std::move(o));
    outbox_.clear();
  }
  for (auto& [c, m] : out) bridge_.send(c, m);
}

void App::draw_task() {
  if (task_.state == Task::Idle) return;
  const double t = now_seconds();
  if (task_.state == Task::Done && t - task_.since > 15) {
    task_ = Task{};
    return;
  }
  auto cancel = [&] {
    if (task_.state == Task::Finding || task_.state == Task::Ready)
      outbox_.push_back({task_.client, {{"type", "act-cancel"}, {"seq", task_.seq}, {"tabId", task_.tab}}});
    task_ = Task{};
  };
  const bool ready = task_.state == Task::Ready;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(ready && task_.warn ? 0x3A1A22 : 0x15233A));
  ImGui::BeginChild("##task", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::TextColored(hexv(0x7FD6FF), "%s", task_.command.c_str());
  if (task_.agent && task_.state != Task::Done && task_.state != Task::Failed) {
    ImGui::SameLine();
    ImGui::TextDisabled("   step %d", task_.actions + 1);
  } else if (task_.steps.size() > 1 && task_.state != Task::Done && task_.state != Task::Failed) {
    ImGui::SameLine();
    ImGui::TextDisabled("   step %zu of %zu", task_.at + 1, task_.steps.size());
  }
  for (size_t i = task_.log.size() > 6 ? task_.log.size() - 6 : 0; i < task_.log.size(); ++i) {
    const auto& [text, ok] = task_.log[i];
    ImGui::TextColored(hexv(ok ? 0x8BF5A6 : 0xFF8A6B), "%s %s", ok ? "done:" : "not done:", text.c_str());
  }
  ImGui::PopFont();

  bool closed = false;
  if (task_.agent && !task_.say.empty() && task_.state != Task::Done && task_.state != Task::Failed) {
    ImGui::PushFont(small_, small_->LegacySize);
    ImGui::TextColored(hexv(0xB9C0D8), "%s", task_.say.c_str());
    ImGui::PopFont();
  }
  switch (task_.state) {
    case Task::Waiting:
    case Task::Snapping:
    case Task::Thinking:
      if (task_.agent) {
        ImGui::TextDisabled(task_.state == Task::Thinking ? "Thinking about the next step..." : "Looking at the page...");
        if (ImGui::SmallButton("Stop")) closed = true;
        break;
      }
      [[fallthrough]];
    case Task::Finding:
      ImGui::TextDisabled("%s: the spider is looking for it...", step_text(task_.steps[task_.at]).c_str());
      if (ImGui::SmallButton(task_.agent ? "Stop" : "Cancel")) closed = true;
      break;
    case Task::Ready:
      if (task_.auto_go) {
        ImGui::TextWrapped("%s...", task_.desc.c_str());
        if (ImGui::SmallButton("Stop")) closed = true;
        break;
      }
      ImGui::TextWrapped("%s?", task_.desc.c_str());
      if (task_.warn)
        ImGui::TextColored(hexv(0xFF8A6B), "Careful: this may spend money, send something, or can't be undone. Look at the page first.");
      ImGui::PushStyleColor(ImGuiCol_Button, hexv(0x2E7D4F));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hexv(0x379660));
      if (ImGui::Button("Do it")) {
        task_.state = Task::Doing;
        task_.since = t;
        outbox_.push_back({task_.client, {{"type", "act-go"}, {"seq", task_.seq}, {"tabId", task_.tab}}});
      }
      ImGui::PopStyleColor(2);
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) closed = true;
      ImGui::PushFont(small_, small_->LegacySize);
      ImGui::TextDisabled("The spider holds it on the page. Nothing is clicked or typed until you press Do it.");
      ImGui::PopFont();
      break;
    case Task::Doing:
      ImGui::TextDisabled("Doing it...");
      break;
    case Task::Done:
      if (task_.agent && !task_.say.empty()) ImGui::TextWrapped("%s", task_.say.c_str());
      ImGui::TextColored(hexv(0x8BF5A6), "Done.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Close")) closed = true;
      break;
    case Task::Failed:
      ImGui::TextColored(hexv(0xFFC46B), "%s", task_.note.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Close")) closed = true;
      break;
    default:
      break;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (closed) cancel();
}

// What the AI chose. A click or typing goes to the spider by the thing's
// number from the last look; the spider holds it, and risky steps wait for you.
void App::agent_step(const json& a, int seq) {
  if (!a.is_object()) return task_stop("The local AI is not answering (it is off, or paused because a game is using the graphics card).");
  const std::string act = jstr(a, "action");
  task_.say = jstr(a, "say");
  if (act == "done") {
    task_.state = Task::Done;
    task_.since = now_seconds() + 45;  // stays up longer: it may hold an answer
    return;
  }
  if (act == "ask") return task_stop("The spider needs you: " + (task_.say.empty() ? std::string("it is stuck.") : task_.say));
  json step;
  const int index = a.value("index", -1);
  if (act == "click" || act == "type") {
    step = {{"verb", act}, {"ref", index}, {"what", "#" + std::to_string(index)}};
    if (act == "type") step["text"] = jstr(a, "text"), step["enter"] = a.value("enter", false);
  } else if (act == "scroll") {
    step = {{"verb", "scroll"}, {"dir", jstr(a, "dir") == "up" ? "up" : "down"}};
  } else if (act == "goto") {
    const std::string url = as_url(jstr(a, "url"));
    if (url.empty()) {
      task_.history.push_back("goto " + jstr(a, "url") + ": failed, not a web address");
      task_.state = Task::Waiting;
      task_.since = now_seconds();
      return;
    }
    step = {{"verb", "goto"}, {"url", url}};
  } else if (act == "back") {
    step = {{"verb", "back"}};
  } else {
    return task_stop("The AI's answer made no sense. Try saying it another way.");
  }
  (void)seq;
  task_.steps = json::array({step});
  task_.at = 0;
  task_.desc.clear();
  task_.seq = ++act_seq_;
  task_.state = Task::Finding;
  task_.since = now_seconds();
  outbox_.push_back({task_.client, {{"type", "act"}, {"seq", task_.seq}, {"tabId", task_.tab}, {"step", step}, {"command", task_.command}}});
  wake();
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
  // Segoe UI first; Windows' own fonts fill in every other script and emoji
  // (Japanese, Chinese, Korean, Indian scripts, symbols). Letters are loaded
  // only when a page needs them, so the extra fonts cost nothing up front.
  auto add = [&](const char* main, float size) -> ImFont* {
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    ImFont* f = io.Fonts->AddFontFromFileTTF(main, size, &cfg);
    if (!f) return nullptr;
    for (const char* extra : {"C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\malgun.ttf",
                              "C:\\Windows\\Fonts\\Nirmala.ttc", "C:\\Windows\\Fonts\\seguisym.ttf",
                              "C:\\Windows\\Fonts\\seguiemj.ttf"}) {
      if (GetFileAttributesA(extra) == INVALID_FILE_ATTRIBUTES) continue;
      ImFontConfig more;
      more.MergeMode = true;
      io.Fonts->AddFontFromFileTTF(extra, size, &more);
    }
    return f;
  };
  body_ = add("C:\\Windows\\Fonts\\segoeui.ttf", 16.f * scale_);
  bold_ = add("C:\\Windows\\Fonts\\seguisb.ttf", 16.f * scale_);
  title_ = add("C:\\Windows\\Fonts\\segoeuib.ttf", 24.f * scale_);
  small_ = add("C:\\Windows\\Fonts\\segoeui.ttf", 13.5f * scale_);
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
  const bool enter = ImGui::InputTextWithHint("##goal", "Look for something, ask a question, or give it a task", goal_,
                                              sizeof(goal_), ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemEdited()) {
    goal_dirty_ = true;
    goal_edit_at_ = now_seconds();
  }
  // Words are hunted while you type; a question waits until you stop, so half a question is not answered.
  // A command waits for Enter: half of one ("click sign") must not send the spider to the wrong thing.
  const double pause = ask_kind(goal_) == Ask::None ? 0.7 : 1.6;
  if (goal_dirty_ && !enter && now_seconds() - goal_edit_at_ > pause && !parse_task(goal_).empty()) goal_dirty_ = false;
  if (enter || (goal_dirty_ && now_seconds() - goal_edit_at_ > pause)) {
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
  draw_task();
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
  ImGui::TextColored(hexv(0x7D8BFF), "%s", active_url_.substr(0, 90).c_str());
  if (ImGui::IsItemClicked()) open_url(active_url_);
  ImGui::SameLine();
  if (confirm("Forget this page", "Click again: forget it", "page:" + active_url_)) {
    forget_page(active_url_);
    ImGui::PopFont();
    return;
  }
  ImGui::PopFont();
  // The answer to what you asked, first.
  const bool asked = !reply_.goal.empty() && reply_.url == active_url_;
  if (asked) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(0x1D1630));
    ImGui::BeginChild("##answer", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(small_, small_->LegacySize);
    ImGui::TextColored(hexv(0xE64CF2), "%s", reply_.goal.c_str());
    ImGui::PopFont();
    if (reply_.busy) {
      ImGui::TextDisabled("Reading the page...");
    } else {
      ImGui::TextWrapped("%s", reply_.text.c_str());
      ImGui::PushFont(small_, small_->LegacySize);
      if (!reply_.quote.empty()) ImGui::TextColored(hexv(0xB9C0D8), "from the page: \"%s\"", reply_.quote.c_str());
      ImGui::TextDisabled(reply_.by_ai ? "answered by your local AI, from this page only"
                                       : "a quick answer from the page's own sentences (the AI is paused or off)");
      ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }
  const bool about = asked && ask_kind(reply_.goal) == Ask::About;  // the answer already is the summary
  if (!about && p->contains("gist") && (*p)["gist"].is_object() && !(*p)["gist"].contains("error")) {
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
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::InputTextWithHint("##search", "Search the library", search_, sizeof(search_));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
  const char* filters[] = {"All", "Verified", "Problems", "Unclear", "Not checked"};
  ImGui::Combo("##status", &status_filter_, filters, 5);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  if (confirm("Forget everything", "Click again: forget all", "all")) forget_all();
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::TextDisabled("%zu pages, %zu finds. Forget clears the finds and the remembered checks; settings stay.",
                      lib_.page_order().size(), lib_.finds().size());
  ImGui::PopFont();
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
    const bool open = ImGui::CollapsingHeader(
        head.c_str(), ImGuiTreeNodeFlags_AllowOverlap | (url == active_url_ ? ImGuiTreeNodeFlags_DefaultOpen : 0));
    // A forget button on every page's row, at the right.
    const float bw = ImGui::CalcTextSize("Click again: forget").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw);
    if (confirm("Forget", "Click again: forget", url)) {
      forget_page(url);
      break;
    }
    if (open)
      for (const json* f : list) draw_find(*f, false);
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
  changed |= ImGui::Checkbox("Work through the whole page, not only the part you see (it never scrolls your page)", &s.crawl);
  changed |= ImGui::Checkbox("Tuck away popups, cookie banners and sign-up walls that block the page", &s.popups);
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::Indent();
  ImGui::TextWrapped("The spider hides them; it never clicks Accept or anything else. Unclear ones go to the local AI. "
                     "Stop the spider and they come back.");
  ImGui::Unindent();
  ImGui::PopFont();
  changed |= ImGui::Checkbox("Tasks: ask me before every step", &s.ask_every_step);
  ImGui::PushFont(small_, small_->LegacySize);
  ImGui::Indent();
  ImGui::TextWrapped("Off: safe steps (scrolling, opening links, searching) just happen, and the spider waits for your Do it "
                     "only before steps that could spend money, send something, sign you up or can't be undone. "
                     "It never types passwords or card numbers.");
  ImGui::Unindent();
  ImGui::PopFont();

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
  if (confirm("Forget everything", "Click again: forget all", "settings-all")) forget_all();
  ImGui::SameLine();
  ImGui::TextDisabled("the library and the remembered checks; settings stay");
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
  HANDLE once = CreateMutexW(nullptr, TRUE, _wgetenv(L"SPIDERPET_DATA") ? L"Local\\SpiderPet.App.test" : L"Local\\SpiderPet.App");
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
