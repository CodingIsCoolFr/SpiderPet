#include "crawl/crawler.hpp"

#include "core/util.hpp"
#include "crawl/stage.hpp"
#include "eyes/eyes.hpp"
#include "eyes/shot.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

namespace sp::crawl {

using json = nlohmann::json;

namespace {

std::string jstr(const json& j, const char* k) {
  if (!j.is_object()) return {};
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string squash(const std::string& s) {
  std::string out;
  bool sp = false;
  for (unsigned char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      sp = !out.empty();
      continue;
    }
    if (sp) out += ' ';
    sp = false;
    out += char(c);
  }
  return out;
}

std::string lower_ascii(std::string s) {
  for (auto& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

std::string clip(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t cut = n;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;  // not inside a UTF-8 letter
  return s.substr(0, cut) + "\xE2\x80\xA6";
}

// Shorter, cut between words (for the thought cloud).
std::string clip_words(const std::string& s, size_t n) {
  if (s.size() <= n) return s;
  size_t cut = s.rfind(' ', n);
  if (cut == std::string::npos || cut < n / 2) return clip(s, n);
  while (cut > 0 && std::strchr(",;:-", s[cut - 1])) --cut;
  return s.substr(0, cut) + "\xE2\x80\xA6";
}

std::string fnv36(const std::string& s) {
  uint32_t h = 0x811c9dc5u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x01000193u;
  }
  std::string out;
  do {
    int d = int(h % 36);
    out += char(d < 10 ? '0' + d : 'a' + d - 10);
    h /= 36;
  } while (h);
  std::reverse(out.begin(), out.end());
  return out;
}

bool isbn_ok(const std::string& raw) {
  std::string d;
  for (char c : raw)
    if (std::isdigit((unsigned char)c) || c == 'X' || c == 'x') d += char(std::toupper((unsigned char)c));
  if (d.size() == 10) {
    int s = 0;
    for (int i = 0; i < 10; ++i) {
      int v = d[i] == 'X' ? (i == 9 ? 10 : -1000) : d[i] - '0';
      s += v * (10 - i);
    }
    return s >= 0 && s % 11 == 0;
  }
  if (d.size() == 13 && d.find('X') == std::string::npos) {
    int s = 0;
    for (int i = 0; i < 13; ++i) s += (d[i] - '0') * (i % 2 ? 3 : 1);
    return s % 10 == 0;
  }
  return false;
}

// Reference marks ([13], [a], [citation needed]) are not part of a sentence.
std::string strip_refs(const std::string& s) {
  static const std::regex refs(R"(\[(?:\d+|[a-z]|citation needed|note \d+)\])", std::regex::icase);
  static const std::regex lead(R"(^\s*(?:\d+|[a-z])\]\s*)", std::regex::icase);
  return squash(std::regex_replace(std::regex_replace(s, refs, ""), lead, ""));
}

std::string words(const std::string& s) {
  std::string out;
  bool sp = false;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c >= 0x80) {
      if (sp && !out.empty()) out += ' ';
      sp = false;
      out += char(std::tolower(c));
    } else {
      sp = true;
    }
  }
  return out;
}

int verdict_code(const json& v) {
  std::string st = v.is_object() ? jstr(v, "status") : std::string();
  if (st == "queued" || st == "checking") return 1;
  if (st == "verified") return 2;
  if (st == "mismatch") return 3;
  if (st == "not_found") return 4;
  if (st == "unverified") return 5;
  if (st == "opinion") return 6;
  if (st == "promo") return 7;
  return 0;
}

const std::map<std::string, double> kKindPrio = {{"doi", 3}, {"isbn", 3}, {"id", 2.6}, {"title", 2.4}, {"sentence", 1.4}, {"heading", 1.2}, {"link", 0.8}};

// Words that say a click may cost money, send something, or can't be undone (the extension's own list).
const std::regex kRisky(
    R"(\b(buy|pay|purchase|order|checkout|check out|subscribe|unsubscribe|delete|remove|send|publish|submit|confirm|transfer|donate|sign up|register|log ?out|sign out|commit|merge|save changes|deactivate|close account|uninstall|format|empty recycle|accept|agree|consent|allow all)\b|^(post|reply|tweet|share|save|update|apply)\b)",
    std::regex::icase);

struct Find {
  std::string id, kind, text, label, href, context;
  bool page = false, key = false;
  int order = 0, block = -1;
  bool eaten = false;
  int verdict = 0, score = -1;
  std::vector<eyes::Box> boxes;  // content space
  double boxes_at = 0, skip_until = 0;
  unsigned long long boxes_ms = 0;  // when they were measured (GetTickCount64)
  unsigned boxes_layout = 0;
};

// Lowercase letters and digits only, for matching text against OCR words.
std::string alnum_lower(const std::string& s) {
  std::string out;
  for (unsigned char c : s)
    if (std::isalnum(c) || c >= 0x80) out += char(std::tolower(c));
  return out;
}

}  // namespace

struct Crawler::Impl {
  Handler on_msg;
  Gone on_gone;
  eyes::Eyes eyes;
  std::unique_ptr<Stage> stage;
  std::thread th;
  std::atomic<bool> quit{false};
  std::mutex mu;
  std::condition_variable cv;
  std::deque<json> inbox;
  std::atomic<HWND> want{nullptr};
  std::atomic<bool> want_pick{false}, want_lift{false};
  std::atomic<HWND> target_pub{nullptr};

  // ------------------------------------------------ crawler thread only
  HWND target = nullptr;
  bool attached = false;
  bool spider_on = true;
  eyes::AppInfo app;
  std::string url, title, lang, how, text_hash, text;
  std::vector<std::string> blocks;
  std::vector<eyes::Box> spans;
  std::vector<int> heading;
  std::vector<bool> seen_block;
  std::vector<eyes::Look::Link> links;
  std::vector<std::string> controls;
  std::vector<Find> finds;
  std::map<std::string, size_t> by_id;
  std::vector<eyes::OcrLine> ocr;
  unsigned long long ocr_ms = 0;
  unsigned ocr_layout = 0;
  double last_look = -100, last_locate = -100, last_say = -100, last_ocr = -100, attached_at = 0;
  unsigned long long located_ms = 0;  // the last measuring of where finds are that nothing moved during
  // Pop-ups (setting "Close pop-ups that get in the way").
  bool close_popups = true;
  std::set<std::string> popups_done;  // each pop-up is handled once
  double next_popups = 0, last_snap_at = -1000;
  int popup_looks = 0;
  std::vector<eyes::Dialog> snap_dialogs;  // the pop-ups of the last look for a task
  unsigned spans_layout = ~0u;        // eyes layout of `spans` (other: out of date)
  std::string last_tree_hash;
  unsigned last_gen = 0;
  std::string goal;
  std::vector<std::string> terms;
  Scene scene;
  std::string eating;
  double eat_since = -1, choose_at = 0, next_off = 0;
  bool done_said = false;
  int found = 0;
  struct Act {
    int seq = -1;
    json step;
    int item = -1;
    std::vector<eyes::Item> pool;
    int fallback = -1;
    std::string desc;
    bool warn = false;
  } act;
  std::vector<eyes::Item> snap_items;
  std::set<std::string> snap_labels;
  std::string snap_url;

  std::mutex smu;
  json stat = {{"on", false}};

  Impl() {
    stage = std::make_unique<Stage>(eyes);
  }
  ~Impl() { stop(); }

  void start() {
    quit = false;
    th = std::thread([this] { run(); });
  }
  void stop() {
    quit = true;
    cv.notify_all();
    if (th.joinable()) th.join();
  }

  void post(const json& m) {
    {
      std::lock_guard<std::mutex> lock(mu);
      inbox.push_back(m);
    }
    cv.notify_all();
  }

  void tell(json m) {
    if (on_msg) on_msg(1, m);
  }

  void say(const std::string& t, bool force = false) {
    double now = now_seconds();
    if (!force && now - last_say < 2.2) return;
    last_say = now;
    scene.say = t;
    ++scene.say_id;
  }

  // ------------------------------------------------ the loop

  void run() {
    SetThreadDescription(GetCurrentThread(), L"spiderpet-crawler");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool waiting_still = false;
    while (!quit) {
      std::deque<json> batch;
      {
        std::unique_lock<std::mutex> lock(mu);
        // Something moved: look often, to measure again as soon as it stops.
        cv.wait_for(lock, std::chrono::milliseconds(waiting_still ? 30 : 120),
                    [&] { return quit || !inbox.empty() || want_pick || want_lift || want.load(); });
        if (quit) break;
        batch.swap(inbox);
      }
      for (auto& m : batch) on_app(m);
      if (want_lift.exchange(false)) gone(true);
      if (want_pick.exchange(false)) stage->pick([this](HWND h) {
        if (h) {
          want = h;
          cv.notify_all();
        }
      });
      if (HWND w = want.exchange(nullptr)) attach(w);
      if (!attached) {
        publish({});
        continue;
      }
      eyes::State st = eyes.state();
      if (!st.attached) {
        gone(false);
        continue;
      }
      double t = now_seconds();
      // Still for a moment: what was measured before the move is measured again.
      unsigned long long now_ms = GetTickCount64();
      bool still = now_ms - st.moved_ms > 120;
      waiting_still = !still || located_ms <= st.moved_ms;
      if (st.gen != last_gen || t - last_look > (act.seq >= 0 ? 1.2 : 2.5) || (still && spans_layout != st.layout && t - last_look > 0.4)) {
        last_gen = st.gen;
        read(st);
        st = eyes.state();
      }
      if (spider_on && st.visible) {
        if (still && (t - last_locate > 0.7 || located_ms <= st.moved_ms)) locate(st);
        behave(t, st);
        mark_seen(st);
        // Pop-ups, on its own; during a task the look before each step does it
        // (the numbers of the AI's last look must not change under it).
        if (close_popups && act.seq < 0 && t - last_snap_at > 60 && t >= next_popups) {
          static const double after[] = {1.5, 3.5, 7, 20};  // soon after a page loads, then now and then
          next_popups = t + after[std::min<size_t>(size_t(popup_looks), 3)];
          ++popup_looks;
          handle_popups();
        }
      }
      scene.show = spider_on;
      scene.marks.clear();
      for (auto& f : finds)
        if (!f.boxes.empty() && now_ms - f.boxes_ms < 3000)
          scene.marks.push_back(Mark{f.id, f.kind, f.text, f.boxes, f.eaten, hits(f.text) > 0, f.verdict, f.boxes_ms, f.boxes_layout});
      scene.read.clear();
      scene.columns.clear();
      // Silk beside what it read: one thread down the left edge of the text,
      // not a dash per paragraph. Paragraphs read one after another, close
      // together and starting near the same edge, share a thread (a list
      // indented under a paragraph joins it, so the thread stays clear of its
      // numbers). Only on a page or document: an app's rows have icons there.
      eyes::Box run;
      for (size_t i = 0; i < spans.size(); ++i) {
        const eyes::Box& sp = spans[i];
        if (sp.empty()) continue;
        scene.columns.push_back(sp);
        if (how != "page" || i >= seen_block.size() || !seen_block[i]) continue;
        // Only real paragraphs: a tag, a date, a name or a button label would
        // each get its own little dash, all over the page.
        if (i >= blocks.size() || blocks[i].size() < 50) continue;
        bool joins =!run.empty() && sp.y >= run.y - 4 && sp.y - (run.y + run.h) < 48 && std::fabs(sp.x - run.x) < 64;
        if (joins) {
          float bottom = std::max(run.y + run.h, sp.y + sp.h);
          run.x = std::min(run.x, sp.x);
          run.h = bottom - run.y;
        } else {
          if (!run.empty()) scene.read.push_back(run);
          run = eyes::Box{sp.x, sp.y, 1, sp.h};
        }
      }
      if (!run.empty()) scene.read.push_back(run);
      scene.spans_layout = spans_layout;
      stage->set(scene);
      publish(st);
    }
    stage->set(Scene{});
    CoUninitialize();
  }

  void attach(HWND h) {
    if (attached && h == target) return;
    if (attached) gone(false);
    std::string err;
    eyes::AppInfo a = eyes::identify(h);
    if (!eyes.attach(h, 6000, &err)) {
      debug_log("crawler: can't read " + a.name + ": " + err);
      std::lock_guard<std::mutex> lock(smu);
      stat["error"] = "Can't read " + a.name + ": " + err;
      return;
    }
    target = h;
    target_pub = h;
    attached = true;
    app = a;
    url.clear();
    text_hash.clear();
    last_look = -100;
    attached_at = now_seconds();
    spider_on = true;
    bool hide = scene.hide_in_shares;
    scene = Scene{};
    scene.hide_in_shares = hide;
    tell({{"type", "hello"}, {"browser", app.name}});
    say("on " + app.name + ".", true);
    debug_log("crawler: on " + app.name + " (" + app.exe + ") " + app.title);
  }

  void gone(bool lifted) {
    if (attached) {
      attached = false;
      target = nullptr;
      target_pub = nullptr;
      eyes.detach();
      if (on_gone) on_gone(1);
    }
    finds.clear();
    by_id.clear();
    eating.clear();
    act = Act{};
    bool hide = scene.hide_in_shares;
    scene = Scene{};
    scene.hide_in_shares = hide;
    stage->set(scene);
    (void)lifted;
  }

  void publish(const eyes::State& st) {
    std::lock_guard<std::mutex> lock(smu);
    stat["on"] = attached;
    stat["picking"] = stage->picking();
    if (!attached) return;
    stat.erase("error");
    stat["app"] = app.name;
    stat["exe"] = app.exe;
    stat["title"] = title;
    stat["url"] = url;
    stat["how"] = how;
    stat["visible"] = st.visible;
    int seen = 0;
    for (bool b : seen_block) seen += b;
    stat["blocks"] = int(blocks.size());
    stat["seen"] = seen;
    int eaten = 0;
    for (auto& f : finds) eaten += f.eaten;
    stat["finds"] = int(finds.size());
    stat["eaten"] = eaten;
    stat["spider"] = spider_on;
    stat["controls"] = int(controls.size());
  }

  // ------------------------------------------------ reading

  int hits(const std::string& s) const {
    if (terms.empty() || s.empty()) return 0;
    std::string lt = lower_ascii(s);
    int n = 0;
    for (auto& t : terms)
      if (lt.find(t) != std::string::npos) ++n;
    return n;
  }
  int match_of(const Find& f) const {
    return hits(f.text) * 2 + ((f.kind == "doi" || f.kind == "isbn" || f.kind == "id") ? hits(f.context) : 0);
  }

  void read(const eyes::State& st) {
    last_look = now_seconds();
    eyes::Look l = eyes.look();
    if (!l.ok && l.error == "the window is gone") {
      gone(false);
      return;
    }
    // A browser (or an Electron app) whose page is not there yet: read as an
    // app, its text runs together and an id gets glued to the next line
    // ("…07.024" + "4. Vaswani"). Wait a little for the page instead.
    if (!app.engine.empty() && l.how == "app" && now_seconds() - attached_at < 15) return;
    size_t chars = 0;
    for (auto& b : l.blocks) chars += b.size();
    // Nothing new in the window's tree: nothing to do (pixels are read again
    // now and then, since a game or a video changes without its tree knowing).
    double now = now_seconds();
    // Where the paragraphs are now (the silk, the margins for labels): kept
    // only when nothing moved while they were measured.
    auto take_spans = [&] {
      if (l.spans.size() != blocks.size()) return;
      spans = l.spans;
      spans_layout = l.steady ? l.layout : ~0u;
    };
    std::string tree_hash = fnv36(l.url + "|" + l.how + "|" + l.text);
    bool same_tree = tree_hash == last_tree_hash;
    last_tree_hash = tree_hash;
    // A picture is taken again every 12 s, and soon after the window moved.
    bool picture_due = how == "picture" && (now - last_ocr > 12 || (ocr_ms <= st.moved_ms && now - last_ocr > 2));
    if (same_tree && l.url == url && !picture_due) {
      if (how != "picture") take_spans();
      return;
    }
    how = l.how;
    ocr.clear();
    // Little readable text (a game, a video, a picture): read the pixels. A
    // document that is just short (a new note) is read as it is.
    if (((how == "app" && chars < 150) || chars < 20) && st.visible) {
      last_ocr = now;
      eyes::State cs = eyes.state();
      unsigned long long shot_ms = GetTickCount64();
      eyes::Pixels px = eyes::grab(target);
      auto lines = eyes::read_pixels(px);
      size_t oc = 0;
      for (auto& x : lines) oc += x.text.size();
      if (oc > chars * 2 && oc > 40) {
        ocr = lines;
        ocr_ms = shot_ms;
        ocr_layout = cs.layout;
        l.blocks.clear();
        l.spans.clear();
        l.heading.clear();
        l.text.clear();
        for (auto& x : lines) {
          l.blocks.push_back(squash(x.text));
          l.spans.push_back(eyes::Box{float(x.r.left) - cs.view.x, float(x.r.top) - cs.view.y + float(cs.scroll), float(x.r.right - x.r.left),
                                      float(x.r.bottom - x.r.top)});
          l.heading.push_back(0);
          l.text += x.text + "\n";
        }
        eyes::State after = eyes.state();
        l.steady = after.moved_ms < shot_ms && after.layout == cs.layout;
        l.layout = cs.layout;
        how = "picture";
      }
    }
    bool new_page = l.url != url;
    if (new_page) {
      url = l.url;
      popup_looks = 0;
      next_popups = now_seconds() + 1.5;
      finds.clear();
      by_id.clear();
      eating.clear();
      done_said = false;
      found = 0;
      seen_block.clear();
    }
    bool title_changed = l.title != title;
    title = l.title;
    lang = l.lang;
    if (new_page || title_changed) tell({{"type", "tab"}, {"tabId", 1}, {"url", url}, {"title", title}, {"web", true}, {"spider", spider_on}});
    std::string hash = fnv36(l.text);
    if (hash == text_hash && !new_page) {
      take_spans();
      return;
    }
    text_hash = hash;
    blocks = l.blocks;
    take_spans();
    heading = l.heading;
    seen_block.resize(blocks.size(), false);
    links = l.links;
    controls = l.controls;
    text = l.text;
    // An app's own buttons and menus say what it is; the model gets them with the text.
    std::string page_text = text;
    if (how == "app" && !controls.empty()) {
      page_text += "\n[the window's buttons and menus: ";
      for (size_t i = 0; i < controls.size() && i < 60; ++i) page_text += (i ? ", " : "") + controls[i];
      page_text += "]";
    }
    tell({{"type", "page"}, {"url", url}, {"title", title}, {"lang", lang}, {"desc", ""}, {"text", clip(page_text, 8000)}, {"app", app.name}, {"how", how}});
    rescan(new_page);
    debug_log("crawler: read " + std::to_string(blocks.size()) + " parts by " + how + ", " + std::to_string(controls.size()) + " controls, " +
              std::to_string(finds.size()) + " finds" + (new_page ? " (new page " + url + ")" : ""));
  }

  std::string context_of_block(int b) const {
    if (b < 0 || b >= int(blocks.size())) return {};
    return clip(blocks[size_t(b)], 700);
  }

  int block_with(const std::string& s) const {
    if (s.size() < 3) return -1;
    for (size_t i = 0; i < blocks.size(); ++i)
      if (blocks[i].find(s) != std::string::npos) return int(i);
    return -1;
  }

  // Everything worth harvesting in the window, the way the extension did it.
  std::vector<Find> harvest() {
    std::vector<Find> out;
    std::set<std::string> seen;
    int order = 0;
    auto put = [&](Find f) {
      f.text = squash(f.text);
      if (f.text.empty()) return;
      f.id = "f" + fnv36(f.kind + "|" + f.label + "|" + f.text + "|" + url);
      if (!seen.insert(f.id).second) return;
      f.order = order++;
      out.push_back(std::move(f));
    };
    // Ids that are links (Wikipedia, most reference lists).
    static const std::regex doi_href(R"(doi\.org/(10\.\d{4,9}/[^?#\s]+))", std::regex::icase);
    static const std::regex pmid_href(R"(pubmed\.ncbi\.nlm\.nih\.gov/(\d{4,9}))");
    static const std::regex pmc_href(R"((?:ncbi\.nlm\.nih\.gov/pmc/articles|pmc\.ncbi\.nlm\.nih\.gov/articles)/PMC(\d+))", std::regex::icase);
    static const std::regex arxiv_href(R"(arxiv\.org/abs/([^?#\s]+))", std::regex::icase);
    static const std::regex isbn_href(R"((?:Special:BookSources/|openlibrary\.org/isbn/)([\dXx-]{10,17}))", std::regex::icase);
    std::set<std::string> link_ids;
    for (auto& l : links) {
      std::smatch m;
      Find f;
      if (std::regex_search(l.href, m, doi_href)) {
        f.kind = "doi";
        f.text = m[1].str();
        while (!f.text.empty() && (f.text.back() == '.' || f.text.back() == ',' || f.text.back() == ';')) f.text.pop_back();
      } else if (std::regex_search(l.href, m, pmid_href)) {
        f.kind = "id", f.label = "PMID", f.text = m[1].str();
      } else if (std::regex_search(l.href, m, pmc_href)) {
        f.kind = "id", f.label = "PMC", f.text = m[1].str();
      } else if (std::regex_search(l.href, m, arxiv_href)) {
        f.kind = "id", f.label = "arXiv", f.text = m[1].str();
      } else if (std::regex_search(l.href, m, isbn_href) && isbn_ok(m[1].str())) {
        f.kind = "isbn", f.label = "ISBN", f.text = m[1].str();
      } else {
        continue;
      }
      // A title that links to its paper is a title, not an id.
      std::string digits;
      for (char c : f.text)
        if (std::isdigit((unsigned char)c)) digits += c;
      std::string shown_digits;
      for (char c : l.text)
        if (std::isdigit((unsigned char)c)) shown_digits += c;
      size_t nwords = std::count(l.text.begin(), l.text.end(), ' ') + 1;
      if (nwords >= 4 && !(digits.size() && shown_digits.find(digits) != std::string::npos)) continue;
      f.href = l.href;
      f.block = block_with(l.text);
      f.context = context_of_block(f.block);
      link_ids.insert(f.kind + f.text);
      put(f);
    }
    // Ids written as plain text.
    struct IdRx {
      const char* kind;
      const char* label;
      std::regex rx;
    };
    static const std::vector<IdRx> id_rx = {
        {"doi", "", std::regex(R"(\b10\.\d{4,9}/[^\s"'<>]+[^\s"'<>.,;:)\]])")},
        {"isbn", "ISBN", std::regex(R"(\bISBN(?:-1[03])?:?\s*((?:97[89][-\s]?)?\d{1,5}[-\s]?\d{1,7}[-\s]?\d{1,7}[-\s]?[\dXx])\b)")},
        {"id", "PMID", std::regex(R"(\bPMID:?\s*(\d{4,9})\b)")},
        {"id", "PMC", std::regex(R"(\bPMC\s?(\d{5,9})\b)")},
        {"id", "arXiv", std::regex(R"(\barXiv:\s*(\d{4}\.\d{4,5}(?:v\d+)?))", std::regex::icase)},
    };
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
      const std::string& t = blocks[bi];
      if (t.size() < 6 || t.find_first_of("0123456789") == std::string::npos) continue;
      for (auto& r : id_rx) {
        for (auto it = std::sregex_iterator(t.begin(), t.end(), r.rx); it != std::sregex_iterator(); ++it) {
          std::string value = (*it).size() > 1 && (*it)[1].matched ? (*it)[1].str() : (*it)[0].str();
          if (std::string(r.kind) == "isbn" && !isbn_ok(value)) continue;
          if (link_ids.count(std::string(r.kind) + value)) continue;
          Find f;
          f.kind = r.kind;
          f.label = r.label;
          f.text = value;
          f.block = int(bi);
          f.context = context_of_block(int(bi));
          put(f);
        }
      }
    }
    // Titles in citations: the quoted article title in a reference.
    static const std::regex ref_like(R"((19|20)\d\d)");
    static const std::regex ref_word(R"(doi|isbn|pp\.|retrieved|journal|vol\.|press|universit|proceedings|archived)", std::regex::icase);
    static const std::regex quoted("[\"\xE2\x80\x9C]([^\"\xE2\x80\x9D]{10,250})[\"\xE2\x80\x9D]");
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
      const std::string& t = blocks[bi];
      if (t.size() < 30 || !std::regex_search(t, ref_like) || !std::regex_search(t, ref_word)) continue;
      std::smatch m;
      if (std::regex_search(t, m, quoted)) {
        std::string title_text = m[1].str();
        while (!title_text.empty() && (title_text.back() == '.' || title_text.back() == ' ')) title_text.pop_back();
        if (std::count(title_text.begin(), title_text.end(), ' ') >= 1) {
          Find f;
          f.kind = "title";
          f.text = title_text;
          f.block = int(bi);
          f.context = context_of_block(int(bi));
          put(f);
        }
      }
    }
    // Headings: the shape of the page.
    for (size_t bi = 0; bi < blocks.size() && bi < heading.size(); ++bi) {
      if (!heading[bi]) continue;
      std::string ht = std::regex_replace(blocks[bi], std::regex(R"(\s*\[?\s*edit[^\]]*\]?\s*$)", std::regex::icase), "");
      if (ht.size() >= 3 && ht.size() <= 140) {
        Find f;
        f.kind = "heading";
        f.text = ht;
        f.block = int(bi);
        put(f);
      }
    }
    // Key sentences: the one or two that carry each paragraph, and with a
    // search, every sentence that matches it.
    std::set<std::string> title_words;
    {
      std::string tw = words(title);
      size_t st = 0;
      while (st < tw.size()) {
        size_t sp = tw.find(' ', st);
        std::string w = tw.substr(st, sp == std::string::npos ? std::string::npos : sp - st);
        if (w.size() > 3) title_words.insert(w);
        if (sp == std::string::npos) break;
        st = sp + 1;
      }
    }
    struct Cand {
      std::string text;
      double s;
      int block;
      bool match;
    };
    std::vector<Cand> sentences;
    static const std::regex split(R"([^.!?]+[.!?]+["\x27)\]]*\s*)");
    static const std::regex ends_full(R"([.!?]["\x27)\]]*$)");
    int pindex = 0;
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
      const std::string& raw = blocks[bi];
      if (bi < heading.size() && heading[bi]) continue;
      bool has_terms = !terms.empty();
      if (raw.size() < (has_terms ? 30u : 100u)) continue;
      if (std::regex_search(raw, ref_like) && std::regex_search(raw, ref_word) && raw.find("\xE2\x80\x9C") != std::string::npos) continue;
      std::vector<Cand> cands;
      int i = 0;
      for (auto it = std::sregex_iterator(raw.begin(), raw.end(), split); it != std::sregex_iterator(); ++it, ++i) {
        std::string tx = strip_refs((*it)[0].str());
        int match = hits(tx);
        if (tx.size() < (match ? 20u : 40u) || tx.size() > 420) continue;
        if (!match && !std::regex_search(tx, ends_full)) continue;
        std::string lt = lower_ascii(tx);
        double s = tx.size() >= 60 && tx.size() <= 260 ? 1 : 0.35;
        if (i == 0) s += 0.7;
        int th = 0;
        for (auto& w : title_words)
          if (lt.find(w) != std::string::npos) ++th;
        s += std::min(1.5, 0.5 * th);
        if (std::regex_search(lt, std::regex(" (is|are|was|were) (a|an|the) "))) s += 0.35;
        if (tx.find_first_of("0123456789") != std::string::npos && !std::isdigit((unsigned char)tx[0])) s += 0.3;
        if (std::regex_search(lt, std::regex("^(it|this|these|they|he|she|that|such)\\b"))) s -= 0.3;
        if (std::regex_search(lt, std::regex("(cookie|subscribe|sign in|log in|newsletter|javascript)"))) s = -9;
        if (match) s += 3 + match;
        cands.push_back({tx, s, int(bi), match > 0});
      }
      std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.s > b.s; });
      size_t keep = raw.size() > 900 ? 3 : raw.size() > 450 ? 2 : 1;
      for (size_t k = 0; k < cands.size(); ++k)
        if (cands[k].match || (k < keep && cands[k].s >= 0.9)) {
          Cand c = cands[k];
          c.s += pindex < 3 ? 0.4 : 0;
          sentences.push_back(c);
        }
      ++pindex;
    }
    std::sort(sentences.begin(), sentences.end(), [](const Cand& a, const Cand& b) { return a.s > b.s; });
    size_t cap = terms.empty() ? 30 : 80;
    for (size_t k = 0; k < sentences.size() && k < cap; ++k) {
      Find f;
      f.kind = "sentence";
      f.text = sentences[k].text;
      f.block = sentences[k].block;
      f.context = context_of_block(f.block);
      f.key = sentences[k].s >= 2.2;
      put(f);
    }
    // Links people would follow: real words, real addresses.
    int nl = 0;
    for (auto& l : links) {
      if (nl >= (terms.empty() ? 30 : 60)) break;
      if (l.href.rfind("http", 0) != 0 || l.href.find('#') == 0) continue;
      std::string lt = squash(l.text);
      size_t p = lt.find(" | ");
      if (p != std::string::npos) lt = lt.substr(p + 3);  // an address line before the title
      if (std::count(lt.begin(), lt.end(), ' ') < 1 && lt.size() < 10 && !hits(lt)) continue;
      Find f;
      f.kind = "link";
      f.text = lt;
      f.href = l.href;
      f.block = block_with(lt);
      put(f);
      ++nl;
    }
    return out;
  }

  json find_json(const Find& f) const {
    return {{"id", f.id}, {"kind", f.kind}, {"text", f.text}, {"label", f.label}, {"href", f.href}, {"context", f.context},
            {"page", f.page}, {"key", f.key}, {"order", f.order}, {"match", hits(f.text)}};
  }

  void rescan(bool send_all) {
    auto got = harvest();
    std::vector<size_t> fresh;
    for (auto& f : got) {
      auto it = by_id.find(f.id);
      if (it != by_id.end()) {
        finds[it->second].block = f.block;
        continue;
      }
      by_id[f.id] = finds.size();
      finds.push_back(f);
      fresh.push_back(finds.size() - 1);
    }
    if (!fresh.empty()) done_said = false;
    json list = json::array();
    if (send_all)
      for (auto& f : finds) list.push_back(find_json(f));
    else
      for (size_t i : fresh) list.push_back(find_json(finds[i]));
    if (!list.empty()) tell({{"type", "finds"}, {"url", url}, {"title", title}, {"lang", lang}, {"finds", list}});
  }

  // ------------------------------------------------ where things are, and what it does

  bool span_in_view(int b, const eyes::State& st) const {
    // Unknown, or measured before the text flowed again: the top of the text.
    if (b < 0 || b >= int(spans.size()) || spans[size_t(b)].empty() || spans_layout != st.layout) return b >= 0 && b < 40;
    const eyes::Box& s = spans[size_t(b)];
    float top = float(st.scroll), bot = top + st.view.h;
    return s.y + s.h > top - 40 && s.y < bot + 40;
  }

  void mark_seen(const eyes::State& st) {
    if (spans_layout != st.layout) return;
    float top = float(st.scroll), bot = top + st.view.h;
    for (size_t i = 0; i < spans.size() && i < seen_block.size(); ++i)
      if (!spans[i].empty() && spans[i].y >= top - 4 && spans[i].y + spans[i].h <= bot + 4) seen_block[i] = true;
  }

  // The words of an OCR line that hold this text, as one box per line.
  std::vector<eyes::Box> ocr_boxes(const std::string& text) const {
    std::string q = alnum_lower(text);
    if (q.size() < 2) return {};
    for (auto& line : ocr) {
      std::string flat;
      std::vector<size_t> word_of;
      for (size_t w = 0; w < line.words.size(); ++w) {
        std::string a = alnum_lower(line.words[w].text);
        flat += a;
        word_of.insert(word_of.end(), a.size(), w);
      }
      size_t p = flat.find(q);
      if (p == std::string::npos) continue;
      RECT u = line.words[word_of[p]].r;
      for (size_t k = p; k < p + q.size(); ++k) {
        const RECT& r = line.words[word_of[k]].r;
        u.left = std::min(u.left, r.left);
        u.top = std::min(u.top, r.top);
        u.right = std::max(u.right, r.right);
        u.bottom = std::max(u.bottom, r.bottom);
      }
      return {eyes::Box{float(u.left), float(u.top), float(u.right - u.left), float(u.bottom - u.top)}};
    }
    return {};
  }

  // Where the finds in view are now. Only a measuring that nothing moved
  // during counts; the stage shows a find only when it was measured after the
  // last move.
  void locate(const eyes::State& st) {
    last_locate = now_seconds();
    std::vector<eyes::Spot> want;
    for (auto& f : finds) {
      if (f.page || f.kind == "heading") continue;
      if (!span_in_view(f.block, st) && !(f.block < 0 && f.kind != "sentence")) continue;
      // Its paragraph tells which one it is when the same text is there twice.
      want.push_back({f.id, f.text, f.block >= 0 && f.block < int(blocks.size()) ? clip(blocks[size_t(f.block)], 200) : std::string()});
      if (want.size() >= 40) break;
    }
    if (want.empty()) {
      located_ms = GetTickCount64();
      return;
    }
    eyes::Spots got;
    if (how == "picture") {
      got.at_ms = ocr_ms;
      got.layout = ocr_layout;
      got.steady = true;
      for (auto& w : want)
        for (auto& b : ocr_boxes(w.text)) got.boxes[w.id].push_back(eyes::Box{b.x - st.view.x, b.y - st.view.y + float(st.scroll), b.w, b.h});
    } else {
      got = eyes.locate(want);
    }
    if (!got.steady) return;  // it moved meanwhile: measured again once it is still
    // (Read from a picture: the boxes keep the picture's time, so after a move
    // they wait for the next picture.)
    located_ms = how == "picture" ? GetTickCount64() : std::max<unsigned long long>(got.at_ms, 1);
    double t = now_seconds();
    for (auto& w : want) {
      auto it = by_id.find(w.id);
      if (it == by_id.end()) continue;
      Find& f = finds[it->second];
      auto g = got.boxes.find(w.id);
      f.boxes.clear();  // not there any more: no box (rather than an old one)
      if (g != got.boxes.end()) f.boxes = g->second;
      f.boxes_at = t;
      f.boxes_ms = got.at_ms;
      f.boxes_layout = got.layout;
    }
  }

  bool in_view(const Find& f, const eyes::State& st) const {
    if (f.boxes.empty()) return false;
    const eyes::Box& b = f.boxes[0];
    float top = float(st.scroll), bot = top + st.view.h;
    return b.y + b.h > top + 4 && b.y < bot - 20 && b.x < st.view.w && b.x + b.w > 0;
  }

  // What next: the most valuable find in view, in reading order. On a hunt,
  // only what matches the search (or what the AI rated high).
  Find* choose(const eyes::State& st, double t) {
    Find* best = nullptr;
    double best_s = -1e9;
    for (auto& f : finds) {
      if (f.eaten || f.page || f.kind == "heading" || t - f.boxes_at > 2.5 || !in_view(f, st)) continue;
      int m = match_of(f);
      if (!terms.empty() && m == 0 && !(f.score >= 7)) continue;
      double s = kKindPrio.count(f.kind) ? kKindPrio.at(f.kind) : 1.0;
      if (f.key) s += 0.8;
      if (f.score >= 0) s += f.score * 0.35 - 1.2;
      s += m * 2.5;
      s -= (f.boxes[0].y - float(st.scroll)) / 2400.0;
      if (s > best_s) {
        best_s = s;
        best = &f;
      }
    }
    return best;
  }

  std::string remark(const Find& f) const {
    if (f.kind == "doi") return "doi: " + clip(f.text, 40);
    if (f.kind == "isbn") return (f.order % 2 ? "book spore: " : "isbn: ") + f.text;
    if (f.kind == "id") return lower_ascii(f.label) + ": " + f.text;
    if (f.kind == "title") return "ooh: " + clip(f.text, 36);
    if (f.kind == "link") return "a link: " + clip(f.text, 36);
    if (f.kind == "sentence") return f.key ? "a key line." : std::string();
    return {};
  }

  void eat(Find& f) {
    debug_log("crawler: ate " + f.kind + " " + clip(f.text, 80));
    f.eaten = true;
    if (f.block >= 0 && f.block < int(seen_block.size())) seen_block[size_t(f.block)] = true;
    tell({{"type", "soon"}, {"id", f.id}});
    std::string r = remark(f);
    if (!r.empty()) say(r);
    if (!terms.empty() && match_of(f) > 0) ++found;
    eating.clear();
    eat_since = -1;
    scene.goal.clear();
    scene.hold = false;
    scene.reading.clear();
  }

  void behave(double t, const eyes::State& st) {
    if (act.seq >= 0) return;  // a task's step: the spider holds that thing
    if (!eating.empty()) {
      auto it = by_id.find(eating);
      Find* f = it == by_id.end() ? nullptr : &finds[it->second];
      if (!f || f->boxes.empty() || !in_view(*f, st)) {
        eating.clear();
        scene.goal.clear();
        scene.hold = false;
        scene.reading.clear();
      } else if (stage->at_goal()) {
        if (eat_since < 0) eat_since = t;
        scene.hold = true;
        if (t - eat_since > (f->kind == "sentence" ? 1.8 : 0.8)) eat(*f);
      } else if (t - choose_at > 8) {
        f->skip_until = t + 20;  // it could not get there
        eating.clear();
        scene.goal.clear();
      }
      return;
    }
    if (Find* f = choose(st, t); f && t >= f->skip_until) {
      eating = f->id;
      eat_since = -1;
      choose_at = t;
      scene.goal = f->id;
      scene.hold = false;
      if (f->kind == "sentence" && !f->boxes.empty()) {
        // A sentence is read, not grabbed: the stage follows its lines.
        scene.reading = f->id;
        scene.goal.clear();
      }
      return;
    }
    // Nothing left in view: the rest is read out of sight, one find at a time.
    if (t < next_off) return;
    next_off = t + 0.45;
    Find* best = nullptr;
    double best_r = -1e9;
    for (auto& f : finds) {
      if (f.eaten || f.page) continue;
      int m = terms.empty() ? 0 : match_of(f);
      if (!terms.empty() && m == 0 && !(f.score >= 7)) continue;
      double r = m * 10 + (kKindPrio.count(f.kind) ? kKindPrio.at(f.kind) : 1.0) + (f.key ? 0.8 : 0) - f.order / 10000.0;
      if (r > best_r) {
        best_r = r;
        best = &f;
      }
    }
    if (!best) {
      if (!done_said && !finds.empty()) {
        done_said = true;
        int seen = 0;
        for (bool b : seen_block) seen += b;
        say(!terms.empty() ? std::to_string(found) + " found for you." : "read it all: " + std::to_string(finds.size()) + " finds.", true);
      }
      return;
    }
    if (in_view(*best, st)) {
      best->skip_until = t + 3;
      return;
    }
    best->eaten = true;
    if (!terms.empty() && match_of(*best) > 0) {
      ++found;
      if (best->boxes.empty()) say(std::to_string(found) + " found so far.");
    }
  }

  // ------------------------------------------------ messages from the app

  void set_goal(const std::string& g, const json& list) {
    bool changed = g != goal;
    goal = g;
    terms.clear();
    if (!goal.empty() && list.is_array())
      for (auto& x : list)
        if (x.is_string()) {
          std::string t = lower_ascii(squash(x.get<std::string>()));
          if (t.size() >= 2 && std::find(terms.begin(), terms.end(), t) == terms.end()) terms.push_back(t);
        }
    if (changed) {
      found = 0;
      done_said = false;
      eating.clear();
      scene.goal.clear();
      say(goal.empty() ? std::string("reading along.") : terms.empty() ? "reading it for you\xE2\x80\xA6" : "hunting: " + clip(goal, 40), true);
    }
    if (attached) rescan(false);
  }

  Find* find_by(const std::string& id) {
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &finds[it->second];
  }

  void on_app(const json& m) {
    const std::string type = jstr(m, "type");
    if (type == "state" || type == "settings") {
      const json st = m.value("settings", json::object());
      if (st.is_object()) {
        scene.hide_in_shares = !st.value("show_in_shares", true);
        close_popups = st.value("close_popups", true);
      }
      if (type == "state") set_goal(jstr(m, "goal"), m.value("terms", json::array()));
    } else if (type == "goal" || type == "hunt") {
      set_goal(jstr(m, "goal"), m.value("terms", json::array()));
    } else if (type == "verdict") {
      if (Find* f = find_by(jstr(m, "id"))) {
        int v = verdict_code(m.value("verdict", json()));
        f->verdict = v;
        if (v == 3 && !f->boxes.empty()) say("that one is wrong!", true);
      }
    } else if (type == "known") {
      if (m.contains("items") && m["items"].is_object())
        for (auto& [id, x] : m["items"].items())
          if (Find* f = find_by(id)) {
            f->verdict = verdict_code(x.value("verdict", json()));
            f->score = x.value("score", -1);
          }
    } else if (type == "scores") {
      if (m.contains("scores") && m["scores"].is_object())
        for (auto& [id, x] : m["scores"].items())
          if (Find* f = find_by(id); f && x.is_number()) f->score = x.get<int>();
    } else if (type == "gist") {
      if (jstr(m, "url") == url) {
        std::string s = jstr(m.value("gist", json::object()), "summary");
        if (!s.empty()) say(clip_words(s, 200), true);  // the cloud wraps it onto up to four lines
      }
    } else if (type == "answer") {
      std::string tx = jstr(m, "text");
      if (!tx.empty()) say(clip(tx, 120), true);
    } else if (type == "reveal") {
      if (Find* f = find_by(jstr(m, "id"))) eyes.scroll_to_text(f->text);
    } else if (type == "spider") {
      spider_on = m.value("on", true);
      if (attached) tell({{"type", "tab"}, {"tabId", 1}, {"url", url}, {"title", title}, {"web", true}, {"spider", spider_on}});
    } else if (type == "snap") {
      tell({{"type", "snap"}, {"seq", m.value("seq", -1)}, {"tabId", 1}, {"snap", snapshot(m)}});
    } else if (type == "act") {
      start_act(m);
    } else if (type == "act-pick") {
      on_pick(m);
    } else if (type == "act-go") {
      go(m);
    } else if (type == "act-cancel") {
      if (m.value("seq", -2) == act.seq) end_act();
    }
  }

  // ------------------------------------------------ tasks

  // A pop-up handler (Playwright runs one before each action; DuckDuckGo's
  // autoconsent and Consent-O-Matic answer cookie boxes by themselves):
  // a cookie box gets a no (Reject all, Necessary only); a newsletter, app,
  // notification or survey pop-up gets closed. Terms to agree to are never
  // agreed to: the person is told (in a task the AI may propose it, and it
  // waits for their "Do it"). A pop-up of any other kind is left alone: the
  // person may have opened it. Each pop-up once. Returns what it did.
  std::string handle_popups() {
    if (!close_popups || !attached) return {};
    static const std::regex cookie_rx(R"(\b(cookies?|consent|gdpr|tracking technologies|our partners|personali[sz]ed (ads|advertising|content)|legitimate interest)\b)",
                                      std::regex::icase);
    static const std::regex agree_rx(R"(\b(agree to|terms of (use|service)|accept (the|our) terms|by continuing|by clicking|end user licen[cs]e|eula)\b)",
                                     std::regex::icase);
    static const std::regex nag_rx(
        R"(\b(newsletter|subscribe|sign up for|notifications?|discount|\d+ ?% off|special offer|exclusive deal|get the app|download (the|our) app|open in (the )?app|install (the|our) app|ad ?blocker|disable your ad|join (now|us|our)|become a member|survey|feedback|rate us|don't miss|limited time)\b)",
        std::regex::icase);
    static const std::regex no_rx(
        R"(^\s*(reject( all)?( cookies)?|reject (optional|non-essential|additional) cookies|decline( all)?( cookies)?|decline optional cookies|deny( all)?|refuse( all)?|disagree|(use )?(only )?(strictly )?(necessary|essential|required)( cookies)?( only)?|continue without (accepting|agreeing))\s*$)",
        std::regex::icase);
    static const std::regex close_rx(
        R"(^\s*(close( (dialog|popup|pop-up|this|banner|window|modal))?|x|\xC3\x97|\xE2\x9C\x95|\xE2\x9C\x96|no,? thanks?|no thank you|not now|maybe later|later|skip|dismiss|not interested|no)\s*$)",
        std::regex::icase);
    std::vector<eyes::Item> items = eyes.items(300);
    std::vector<eyes::Dialog> dl = eyes.dialogs();
    std::string page_key = url.substr(0, url.find('#'));
    auto click = [&](const eyes::Item& it, const std::string& key, const std::string& what) -> std::string {
      popups_done.insert(key);
      std::string note;
      if (!eyes.click_item(it.i, &note)) {
        debug_log("popup: could not click \"" + it.label + "\": " + note);
        return {};
      }
      debug_log("popup: " + what + " (\"" + it.label + "\") on " + page_key);
      say(what + ".", true);
      return what + " (clicked \"" + clip(it.label, 40) + "\")";
    };
    // Pop-ups (dialogs) first.
    for (size_t d = 0; d < dl.size(); ++d) {
      const eyes::Dialog& g = dl[d];
      std::string all = g.label + " " + g.text;
      std::string key = page_key + "|" + fnv36(all.substr(0, 400));
      if (popups_done.count(key)) continue;
      std::vector<const eyes::Item*> btns;
      for (auto& it : items)
        if (it.dialog == int(d) && it.in_view && !it.secret && (it.kind == "button" || it.kind == "link")) btns.push_back(&it);
      bool cookie = std::regex_search(all, cookie_rx);
      bool agree = !cookie && std::regex_search(all, agree_rx);
      bool nag = !cookie && !agree && std::regex_search(all, nag_rx);
      const eyes::Item* pick = nullptr;
      if (cookie) {
        for (auto* b : btns)
          if (!pick && std::regex_search(b->label, no_rx)) pick = b;
        if (!pick) {
          popups_done.insert(key);  // no "no" button: not for the spider to answer
          say("a cookie box with no way to say no: it is yours to answer.", true);
          continue;
        }
        return click(*pick, key, "said no to cookies");
      }
      if (agree) {
        popups_done.insert(key);
        say("a pop-up asks you to agree to terms: only you can say yes to that.", true);
        continue;
      }
      if (nag) {
        for (auto* b : btns)
          if (!pick && (std::regex_search(b->label, close_rx) || std::regex_search(b->label, no_rx))) pick = b;
        if (!pick) {
          popups_done.insert(key);
          continue;
        }
        return click(*pick, key, "closed a pop-up");
      }
    }
    // A cookie bar that is not a dialog: only an unmistakable "no" button, with cookie words on the page.
    bool cookie_words = false;
    for (auto& b : blocks)
      if (std::regex_search(b, cookie_rx)) {
        cookie_words = true;
        break;
      }
    if (cookie_words) {
      static const std::regex strict_no(
          R"(^\s*(reject all( cookies)?|reject (optional|non-essential|additional) cookies|decline (all|optional cookies)|refuse all|deny all|(use )?(only )?(strictly )?(necessary|essential) cookies( only)?|(only )?necessary only|continue without accepting)\s*$)",
          std::regex::icase);
      for (auto& it : items) {
        if (!it.in_view || it.dialog >= 0 || it.secret || (it.kind != "button" && it.kind != "link")) continue;
        std::string key = page_key + "|bar|" + it.label;
        if (popups_done.count(key) || !std::regex_search(it.label, strict_no)) continue;
        return click(it, key, "said no to cookies");
      }
    }
    return {};
  }

  // What the window offers right now, for the AI that plans a task: what is
  // on screen, where that is on the page ("screen 2 of 5", as Magentic-One's
  // web surfer says it), the page's headings, where the goal's words are on
  // the whole page, a numbered list of what can be clicked or typed into, and
  // (asked for) a picture with those numbers drawn on it.
  json snapshot(const json& req) {
    if (!attached) return nullptr;
    last_snap_at = now_seconds();
    // Pop-ups first, as Playwright's handlers run before each action.
    std::string popup_note = handle_popups();
    if (!popup_note.empty()) Sleep(900);  // the page settles after it closes
    eyes::State st = eyes.state();
    snap_items = eyes.items(90);
    snap_dialogs = eyes.dialogs();
    json items = json::array();
    std::set<std::string> labels;
    bool same = url == snap_url;
    bool playing = false, media = false;
    std::vector<eyes::Item> kept;
    for (auto& it : snap_items) {
      if (it.secret) continue;  // password and card boxes are never offered, not even to the AI
      std::string lw = lower_ascii(it.label);
      if (lw.rfind("pause", 0) == 0) media = playing = true;
      else if (lw.rfind("play", 0) == 0 && lw.find("playlist") == std::string::npos) media = true;
      json x = {{"i", it.i}, {"kind", it.kind}, {"label", it.label}, {"href", it.href}, {"value", it.value}, {"inView", it.in_view},
                {"search", it.search}, {"fresh", same && !snap_labels.count(it.label)}, {"near", it.row}, {"covered", false}};
      if (it.dialog >= 0 && size_t(it.dialog) < snap_dialogs.size())
        x["popup"] = snap_dialogs[size_t(it.dialog)].label.empty() ? std::string("a pop-up") : clip(snap_dialogs[size_t(it.dialog)].label, 50);
      items.push_back(x);
      labels.insert(it.label);
    }
    snap_labels = labels;
    snap_url = url;
    json media_list = json::array();
    if (media) media_list.push_back({{"kind", "video"}, {"playing", playing}, {"time", 0}, {"duration", 0}, {"muted", false}});

    // Where things are: on screen, or so many screens up or down.
    bool geo = spans_layout == st.layout && spans.size() == blocks.size() && st.view.h > 50;
    float top = float(st.scroll), vh = st.view.h, total = 0;
    if (geo)
      for (auto& s : spans)
        if (!s.empty()) total = std::max(total, s.y + s.h);
    auto where = [&](size_t i) -> std::string {
      if (!geo || i >= spans.size() || spans[i].empty()) return "";
      float mid = spans[i].y + spans[i].h / 2;
      if (mid >= top && mid <= top + vh) return "on screen";
      int n = std::max(1, int(std::ceil((mid < top ? top - mid : mid - top - vh) / vh)));
      return std::to_string(n) + (n == 1 ? " screen " : " screens ") + (mid < top ? "up" : "down");
    };
    std::vector<std::string> terms_l;
    if (req.contains("terms") && req["terms"].is_array())
      for (auto& x : req["terms"])
        if (x.is_string() && x.get<std::string>().size() >= 3) terms_l.push_back(lower_ascii(x.get<std::string>()));
    std::string view_text, outline, hits;
    // The paragraphs that hold the most of the goal's words, best first.
    struct Hit {
      size_t block, at;
      int words;
    };
    std::vector<Hit> found;
    for (size_t i = 0; i < blocks.size(); ++i) {
      std::string w = where(i);
      // On screen: any part of it (a long paragraph's middle may be off screen).
      bool shows = geo && i < spans.size() && !spans[i].empty() && spans[i].y < top + vh && spans[i].y + spans[i].h > top;
      if (shows && view_text.size() < 2400) view_text += blocks[i] + "\n";
      if (i < heading.size() && heading[i] > 0 && outline.size() < 1400) outline += "- " + clip(blocks[i], 90) + (w.empty() ? "" : " (" + w + ")") + "\n";
      if (terms_l.empty()) continue;
      std::string lb = lower_ascii(blocks[i]);
      size_t p = std::string::npos;
      int n = 0;
      for (auto& t : terms_l) {
        size_t q = lb.find(t);
        if (q != std::string::npos) ++n, p = std::min(p, q);
      }
      if (n) found.push_back({i, p, n});
    }
    std::stable_sort(found.begin(), found.end(), [](const Hit& a, const Hit& b) { return a.words > b.words; });
    for (size_t k = 0; k < found.size() && k < 8; ++k) {
      const std::string& b = blocks[found[k].block];
      size_t from = found[k].at > 80 ? found[k].at - 80 : 0;
      while (from > 0 && ((unsigned char)b[from] & 0xC0) == 0x80) --from;
      std::string w = where(found[k].block);
      hits += "- " + std::to_string(found[k].words) + " of your words: \"" + clip(b.substr(from), 200) + "\"" + (w.empty() ? "" : " (" + w + ")") + "\n";
    }
    if (view_text.empty()) view_text = clip(squash(text), 2500);  // no geometry: the start of the text
    std::string at;
    if (geo && total > vh) {
      int n = std::max(1, int(std::ceil(total / vh))), k = std::min(n, int(top / vh) + 1);
      at = "screen " + std::to_string(k) + " of " + std::to_string(n);
    } else if (geo) {
      at = "the whole page fits on the screen";
    }
    // The picture, with the number of everything on screen that can be clicked or typed into.
    std::string pic;
    if (req.value("picture", false) && st.visible) {
      eyes::Pixels px = eyes::grab(target);
      std::vector<std::pair<int, RECT>> marks;
      for (auto& it : snap_items)
        if (it.in_view && !it.secret && !it.r.empty())
          marks.push_back({it.i, RECT{LONG(it.r.x), LONG(it.r.y), LONG(it.r.x + it.r.w), LONG(it.r.y + it.r.h)}});
      eyes::draw_marks(px, marks);
      pic = eyes::jpeg_base64(px, 1024, 78);
    }
    return {{"url", url},     {"title", title},     {"text", clip(view_text, 2500)}, {"where", at},
            {"outline", outline}, {"hits", hits},   {"media", media_list},          {"scroll", 0},
            {"items", items}, {"app", app.name},    {"picture", pic}, {"popup_note", popup_note}};
  }

  void report(json m) {
    if (!m.contains("seq")) m["seq"] = act.seq;
    m["tabId"] = 1;
    tell(m);
  }

  void act_fail(const std::string& note) {
    if (act.seq >= 0) report({{"type", "act-done"}, {"ok", false}, {"note", note}});
    say(note, true);
    end_act();
  }

  void end_act() {
    act = Act{};
    scene.goal_screen = eyes::Box{};
    scene.hold = false;
    last_look = -100;  // look again soon: the window changed
  }

  // "the search box", "Sign in button" -> what to look for, and what kind of thing it is.
  static std::pair<std::string, std::string> want_of(const std::string& what) {
    std::string w = squash(what);
    while (!w.empty() && (w.front() == '"' || w.front() == '\'')) w.erase(w.begin());
    while (!w.empty() && (w.back() == '"' || w.back() == '\'')) w.pop_back();
    w = std::regex_replace(w, std::regex("^(the|a|an|on|on the)\\s+", std::regex::icase), "");
    std::string hint;
    std::smatch m;
    static const std::regex tail(R"(\s+(button|link|tab|field|box|text ?box|input|checkbox|check ?box|switch|toggle|icon|menu|option)$)", std::regex::icase);
    if (std::regex_search(w, m, tail)) {
      std::string k = lower_ascii(m[1].str());
      k.erase(std::remove(k.begin(), k.end(), ' '), k.end());
      w = w.substr(0, size_t(m.position(0)));
      static const std::map<std::string, std::string> kinds = {
          {"button", "button"}, {"icon", "button"},     {"link", "link"},       {"tab", "tab"},         {"field", "field"},       {"box", "field"},
          {"textbox", "field"}, {"input", "field"},     {"checkbox", "checkbox"}, {"switch", "checkbox"}, {"toggle", "checkbox"}, {"menu", "menu item"},
          {"option", "menu item"}};
      auto it = kinds.find(k);
      if (it != kinds.end()) hint = it->second;
    }
    return {w, hint};
  }

  // How well a thing fits what you said: its own words first.
  static double fit(const std::string& want, const std::string& hint, const eyes::Item& it) {
    std::string w = words(want), l = words(it.label);
    if (w.empty() || l.empty()) return 0;
    double s;
    if (l == w) s = 100;
    else if (l.rfind(w + " ", 0) == 0 || l.rfind(w, 0) == 0) s = 80;
    else if ((" " + l + " ").find(" " + w + " ") != std::string::npos) s = 70;
    else if (w.rfind(l, 0) == 0 && l.size() >= 3) s = 55;
    else {
      std::vector<std::string> ws;
      std::set<std::string> ls;
      size_t st = 0;
      while (st < w.size()) {
        size_t sp = w.find(' ', st);
        ws.push_back(w.substr(st, sp == std::string::npos ? std::string::npos : sp - st));
        if (sp == std::string::npos) break;
        st = sp + 1;
      }
      st = 0;
      while (st < l.size()) {
        size_t sp = l.find(' ', st);
        ls.insert(l.substr(st, sp == std::string::npos ? std::string::npos : sp - st));
        if (sp == std::string::npos) break;
        st = sp + 1;
      }
      int shared = 0;
      for (auto& x : ws) shared += ls.count(x) ? 1 : 0;
      s = double(shared) / double(ws.size()) * 50 - std::max(0.0, double(ls.size()) - double(ws.size())) * 1.5;
    }
    if (!hint.empty() && s > 0) s += (it.kind == hint || (hint == "field" && it.kind == "list")) ? 15 : -10;
    return s;
  }

  static std::string describe(const json& step, const eyes::Item& it) {
    std::string what = !it.label.empty() ? "\"" + clip(it.label, 60) + "\"" : "this " + it.kind;
    if (jstr(step, "verb") == "type")
      return "Type \"" + clip(jstr(step, "text"), 60) + "\" into " + what + (step.value("enter", false) ? " and press Enter" : "");
    return "Click " + what + " (" + it.kind + ")";
  }

  // A step the app must not do without your "Do it": it could spend money,
  // send or post something, sign you in or up, or can't be undone.
  bool risky(const json& step, const eyes::Item& it, const std::vector<eyes::Item>& all) const {
    if (std::regex_search(it.label, kRisky)) return true;
    // A link whose words say nothing ("", an icon) but whose address signs you out.
    static const std::regex out_href(R"(/(logout|signout|log-out|sign-out|logoff|signoff)\b)", std::regex::icase);
    if (std::regex_search(it.href, out_href)) return true;
    static const std::regex agree_rx(R"(\b(agree to|terms of (use|service)|accept (the|our) terms|by continuing|by clicking|end user licen[cs]e|eula)\b)",
                                     std::regex::icase);
    if (it.dialog >= 0 && size_t(it.dialog) < snap_dialogs.size() &&
        std::regex_search(snap_dialogs[size_t(it.dialog)].label + " " + snap_dialogs[size_t(it.dialog)].text, agree_rx))
      return true;
    if (jstr(step, "verb") == "type") return step.value("enter", false) && !it.search;
    // A button next to a password box signs you in or up.
    if (it.kind == "button")
      for (auto& x : all)
        if (x.secret) return true;
    return false;
  }

  void propose(const eyes::Item& it, const std::vector<eyes::Item>& all) {
    act.item = it.i;
    act.desc = describe(act.step, it);
    act.warn = risky(act.step, it, all);
    eyes.show_item(it.i);  // you have to see it to say yes
    Sleep(150);
    eyes::Box b = eyes.item_box(it.i);
    scene.goal.clear();
    scene.goal_screen = b.empty() ? it.r : b;
    scene.hold = true;
    say(lower_ascii(act.desc.substr(0, 1)) + act.desc.substr(1) + (act.warn ? "? say yes in the app." : "\xE2\x80\xA6"), true);
    report({{"type", "act-ready"}, {"desc", act.desc}, {"warn", act.warn}});
  }

  // A browser's own buttons (Back, Reload, the address bar) work from the back too.
  int item_named(const std::vector<eyes::Item>& all, std::initializer_list<const char*> names, const char* kind = nullptr) {
    for (auto& it : all) {
      std::string lw = lower_ascii(it.label);
      if (kind && it.kind != kind) continue;
      for (const char* n : names)
        if (lw.rfind(n, 0) == 0) return it.i;
    }
    return -1;
  }

  void start_act(const json& m) {
    if (act.seq >= 0) report({{"type", "act-done"}, {"ok", false}, {"note", "replaced by a new command"}});
    act = Act{};
    act.seq = m.value("seq", -1);
    act.step = m.value("step", json::object());
    const std::string verb = jstr(act.step, "verb");
    if (!attached) return act_fail("the spider is not on a window.");
    auto done = [&](bool ok, const std::string& note) {
      report({{"type", "act-done"}, {"ok", ok}, {"note", note}});
      if (!note.empty()) say(note);
      end_act();
    };
    if (verb == "scroll") {
      std::string dir = jstr(act.step, "dir");
      return done(eyes.scroll(dir.empty() ? "down" : dir), "scrolled");
    }
    if (verb == "find") {
      // Find on the page (Ctrl+F): the whole page at once, free and instant;
      // the spider jumps to the first one. It never fails: "not on this page"
      // is an answer too.
      std::string q = squash(jstr(act.step, "text"));
      if (q.size() < 2) return act_fail("nothing to find.");
      std::string lq = lower_ascii(q);
      int count = 0;
      struct Spot {
        size_t block, at;
      };
      std::vector<Spot> spots;  // the first match in each paragraph that has one
      for (size_t i = 0; i < blocks.size(); ++i) {
        std::string lb = lower_ascii(blocks[i]);
        bool here = false;
        for (size_t p = lb.find(lq); p != std::string::npos; p = lb.find(lq, p + 1)) {
          if (!here) spots.push_back({i, p}), here = true;
          ++count;
        }
      }
      if (spots.empty()) return done(true, "\"" + clip(q, 60) + "\" is not on this page");
      // It jumps to a real sentence: not a label or a line that only repeats
      // the words (a "9 results for ..." heading after a search).
      size_t go = 0;
      for (size_t k = 0; k < spots.size(); ++k)
        if (blocks[spots[k].block].size() > q.size() + 40) {
          go = k;
          break;
        }
      bool shown = eyes.scroll_to_text(q, blocks[spots[go].block]);
      auto around = [&](const Spot& s) {
        const std::string& b = blocks[s.block];
        size_t from = s.at > 90 ? s.at - 90 : 0;
        while (from > 0 && ((unsigned char)b[from] & 0xC0) == 0x80) --from;
        return "\"" + clip(b.substr(from), 220) + "\"";
      };
      std::string note = "found \"" + clip(q, 60) + "\" " + std::to_string(count) + (count == 1 ? " time: " : " times: ") + around(spots[go]) +
                         (shown ? " (now on screen)" : " (could not bring it on screen)");
      int more = 0;
      for (size_t k = 0; k < spots.size() && more < 2; ++k)
        if (k != go) note += (more++ ? "; " : "; also: ") + around(spots[k]);
      return done(true, note);
    }
    if (verb == "key") {
      std::string k = jstr(act.step, "key");
      if (eyes.press(k)) return done(true, "pressed " + (k == " " ? std::string("Space") : k));
      return act_fail("can't press \"" + k + "\" here (the window must be in front).");
    }
    if (verb == "goto" || verb == "back" || verb == "forward" || verb == "reload" || verb == "enter" || verb == "media") {
      std::vector<eyes::Item> all = eyes.items(300);
      if (verb == "goto") {
        std::string to = jstr(act.step, "url");
        int bar = item_named(all, {"search with", "address", "search or enter", "enter address"});
        if (app.browser && bar >= 0) {
          std::string note;
          if (eyes.type_item(bar, to, true, &note)) return done(true, "opening " + clip(to, 60));
          return act_fail(note);
        }
        // Not in a browser: the address opens in your browser.
        ShellExecuteW(nullptr, L"open", wide(to).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return done(true, "opened " + clip(to, 60) + " in your browser");
      }
      if (verb == "back" || verb == "forward" || verb == "reload") {
        int b = item_named(all, {verb == "back" ? "back" : verb == "forward" ? "forward" : "reload"}, "button");
        if (b < 0 && verb == "reload") b = item_named(all, {"refresh"}, "button");
        std::string note;
        if (b >= 0 && eyes.click_item(b, &note)) return done(true, verb == "back" ? "went back" : verb == "forward" ? "went forward" : "reloaded");
        return act_fail("this window has no " + verb + " button.");
      }
      if (verb == "enter") {
        // Enter can send a form: it waits for your "Do it" like any risky step.
        act.item = -2;
        act.desc = "Press Enter";
        act.warn = true;
        say("press enter? say yes in the app.", true);
        report({{"type", "act-ready"}, {"desc", act.desc}, {"warn", true}});
        return;
      }
      // play / pause: the window's own button.
      std::string op = jstr(act.step, "op");
      for (auto& it : all) {
        std::string lw = lower_ascii(it.label);
        if (it.kind == "button" && (lw.rfind(op == "pause" ? "pause" : "play", 0) == 0) && lw.find("playlist") == std::string::npos)
          return propose(it, all);
      }
      return act_fail("there is no " + std::string(op == "pause" ? "pause" : "play") + " button here.");
    }
    if (verb == "type" && jstr(act.step, "text").empty()) return act_fail("nothing to type.");
    // A task's step names a thing from the last look by its number.
    if (act.step.contains("ref") && act.step["ref"].is_number_integer()) {
      int ref = act.step["ref"].get<int>();
      const eyes::Item* it = nullptr;
      for (auto& x : snap_items)
        if (x.i == ref) it = &x;
      if (!it) return act_fail("that thing is gone from the window.");
      if (verb == "type" && (it->secret || (it->kind != "field" && it->kind != "list")))
        return act_fail("that isn't a box the spider may type in.");
      return propose(*it, snap_items);
    }
    bool typing = verb == "type";
    auto [want, hint] = want_of(jstr(act.step, "what").empty() ? (typing ? "search" : "") : jstr(act.step, "what"));
    std::vector<eyes::Item> all = eyes.items(300);
    snap_items = all;  // the numbers now refer to this list
    snap_dialogs = eyes.dialogs();  // and so do the pop-ups they sit in
    struct Scored {
      const eyes::Item* it;
      double s;
    };
    std::vector<Scored> list;
    for (auto& it : all) {
      if (typing && (it.secret || (it.kind != "field" && it.kind != "list"))) continue;  // never offered for typing
      double s = fit(want, hint, it);
      if (typing && lower_ascii(want).find("search") != std::string::npos && it.search) s = std::max(s, 75.0);
      if (s > 0 && it.in_view) s += 8;
      list.push_back({&it, s});
    }
    std::stable_sort(list.begin(), list.end(), [](const Scored& a, const Scored& b) { return a.s > b.s; });
    if (!list.empty()) {
      const Scored& best = list[0];
      bool clear = list.size() < 2 || list[1].s < best.s - 8 || words(list[1].it->label) == words(best.it->label);
      if (best.s >= 60 && clear) return propose(*best.it, all);
    }
    // Not sure: the app's AI picks from what is in the window (it only sees the labels).
    act.pool.clear();
    for (auto& x : list)
      if (x.s > 0 && act.pool.size() < 30) act.pool.push_back(*x.it);
    for (auto& x : list) {
      if (act.pool.size() >= 60) break;
      if (x.it->in_view && !x.it->label.empty() &&
          std::none_of(act.pool.begin(), act.pool.end(), [&](const eyes::Item& p) { return p.i == x.it->i; }))
        act.pool.push_back(*x.it);
    }
    act.fallback = !list.empty() && list[0].s >= 30 ? list[0].it->i : -1;
    if (act.pool.empty()) return act_fail("can't find \"" + clip(want, 40) + "\" here.");
    say("which one is it\xE2\x80\xA6", true);
    json items = json::array();
    for (size_t i = 0; i < act.pool.size(); ++i)
      items.push_back({{"i", int(i)}, {"kind", act.pool[i].kind}, {"label", act.pool[i].label}, {"href", act.pool[i].href}, {"inView", act.pool[i].in_view}});
    report({{"type", "act-ask"}, {"command", jstr(m, "command")}, {"items", items}});
  }

  void on_pick(const json& m) {
    if (act.seq < 0 || act.seq != m.value("seq", -2) || act.item >= 0) return;
    int index = m.value("index", -1);
    if (index >= 0 && index < int(act.pool.size())) return propose(act.pool[size_t(index)], snap_items);
    if (act.fallback >= 0)
      for (auto& it : snap_items)
        if (it.i == act.fallback) return propose(it, snap_items);
    act_fail(jstr(m, "why").empty() ? "can't tell which one you mean." : clip(jstr(m, "why"), 80));
  }

  void go(const json& m) {
    if (act.seq < 0 || act.seq != m.value("seq", -2) || act.item == -1) return;
    std::string note;
    bool ok;
    if (act.item == -2) {
      ok = eyes.press("Enter");
      if (!ok) note = "can't press Enter (the window must be in front).";
    } else if (jstr(act.step, "verb") == "type") ok = eyes.type_item(act.item, jstr(act.step, "text"), act.step.value("enter", false), &note);
    else ok = eyes.click_item(act.item, &note);
    if (!ok) return act_fail(note.empty() ? "it did not work." : note);
    report({{"type", "act-done"}, {"ok", true}, {"note", act.desc}});
    say("done.", true);
    end_act();
  }
};

Crawler::Crawler() : d(std::make_unique<Impl>()) {}
Crawler::~Crawler() { d->stop(); }

bool Crawler::start(Handler on_message, Gone on_gone) {
  d->on_msg = std::move(on_message);
  d->on_gone = std::move(on_gone);
  d->start();
  return true;
}

void Crawler::stop() { d->stop(); }

void Crawler::send(int client, const json& msg) {
  if (client == 1) d->post(msg);
}

void Crawler::broadcast(const json& msg) { d->post(msg); }

int Crawler::clients() const { return d->target_pub.load() ? 1 : 0; }

void Crawler::drop_on(HWND h) {
  if (!h || eyes::is_ours(h)) return;
  d->want = h;
  d->cv.notify_all();
}

void Crawler::pick() {
  d->want_pick = true;
  d->cv.notify_all();
}

void Crawler::lift() {
  d->want_lift = true;
  d->cv.notify_all();
}

json Crawler::status() {
  std::lock_guard<std::mutex> lock(d->smu);
  return d->stat;
}

std::string Crawler::picture(int max_side) {
  HWND h = d->target_pub.load();
  if (!h) return {};
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  eyes::Pixels px = eyes::grab(h);
  return eyes::jpeg_base64(px, max_side);
}

}  // namespace sp::crawl
