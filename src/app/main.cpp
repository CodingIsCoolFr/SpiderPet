// SpiderPet: a node-and-line spider that walks on whatever window you drop it
// on, reads it with Windows OCR, and harvests what is worth keeping.
#include "app/store.hpp"
#include "core/util.hpp"
#include "mind/llm.hpp"
#include "page/page.hpp"
#include "pet/pet.hpp"
#include "render/overlay.hpp"
#include "render/paint.hpp"
#include "render/scene.hpp"
#include "see/inspector.hpp"
#include "see/vision.hpp"
#include "ui/panel.hpp"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace sp;

namespace {

std::wstring class_of(HWND h) {
  wchar_t b[128]{};
  GetClassNameW(h, b, 128);
  return b;
}

std::wstring title_of(HWND h) {
  wchar_t b[512]{};
  GetWindowTextW(h, b, 512);
  return b;
}

// "(3) Home / X" and "Home / X" are the same page.
std::wstring page_key(std::wstring t) {
  if (!t.empty() && t[0] == L'(') {
    const size_t c = t.find(L") ");
    if (c != std::wstring::npos && c < 8) t = t.substr(c + 2);
  }
  return t;
}

bool cloaked(HWND h) {
  BOOL c = FALSE;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c));
  return c != FALSE;
}

bool is_shell(HWND h) {
  const std::wstring c = class_of(h);
  return c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd";
}

// Windows that are safe to scroll with a wheel message. SpiderTestPage is the
// test viewer in SpiderTool.
bool is_browser(HWND h) {
  const std::wstring c = class_of(h);
  return c == L"Chrome_WidgetWin_1" || c == L"MozillaWindowClass" || c == L"SpiderTestPage";
}

RECT monitor_rect(POINT p, bool work) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
  return work ? mi.rcWork : mi.rcMonitor;
}

float dpi_scale(POINT p) {
  UINT x = 96, y = 96;
  GetDpiForMonitor(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y);
  return x / 96.f;
}

Rect to_rect(const RECT& r) {
  return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
          static_cast<float>(r.bottom - r.top)};
}

double idle_seconds() {
  LASTINPUTINFO li{sizeof(li)};
  if (!GetLastInputInfo(&li)) return 0;
  return (GetTickCount() - li.dwTime) / 1000.0;
}

void copy_text(HWND owner, const std::string& text) {
  const std::wstring w = wide(text);
  if (!OpenClipboard(owner)) return;
  EmptyClipboard();
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
    std::memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    SetClipboardData(CF_UNICODETEXT, mem);
  }
  CloseClipboard();
}

struct Options {
  std::wstring attach_title;  // start on the first window whose title contains this
  double seconds = 0;         // quit after this long (tests)
  bool quiet = false;         // start with the panel minimized, never take focus
  bool second = false;        // tests: run beside a copy that is already open
  bool panel_at = false;      // put the panel here instead of the default spot
  POINT panel_pos{};
};

class App {
 public:
  int run(HINSTANCE inst, const Options& opt);

 private:
  Vec2 offset() const;  // content = screen + offset
  Vec2 to_content(POINT p) const { return Vec2{static_cast<float>(p.x), static_cast<float>(p.y)} + offset(); }
  Vec2 to_screen(Vec2 c) const { return c - offset(); }
  HWND window_at(POINT p) const;
  void attach(HWND w, POINT at, double now);
  void detach();
  void new_page(double now);
  void follow_target(double now);
  void refresh_above(double now);
  bool covered(Vec2 screen) const;
  View view() const;
  void frame(double now, float dt);
  void handle(const PanelActions& a, double now);

  Overlay overlay_;
  Panel panel_;
  Vision vision_;
  Inspector inspector_;
  Llm llm_;
  Store store_;
  Page page_;
  Pet pet_;
  Scene scene_;
  Painter painter_;
  Settings settings_;
  HWND panel_hwnd_ = nullptr;

  HWND root_ = nullptr;
  RECT root_rect_{};
  RECT content_{};
  bool content_known_ = false;
  bool scrollable_ = false;
  std::wstring title_, key_, url_;
  uint64_t gen_ = 0, page_id_ = 0, token_ = 0;
  double inspect_at_ = 0, title_at_ = 0, scroll_req_at_ = 0, above_at_ = 0, retry_at_ = 0;
  // Did the last scroll request move anything? Pages that scroll an inner box
  // need the wheel; pages that cannot scroll (or are at the end) stop crawling.
  double scroll_check_at_ = 0, scroll_before_ = 0;
  int scroll_fails_ = 0, scroll_method_ = 0;
  // Browser mode: the page's own text, links and headings via accessibility.
  bool page_tree_ = false;
  double scan_at_ = 0, scan_scroll_ = 0;
  uint64_t scan_token_ = 0;
  bool scroll_dead_ = false;
  double user_scrolled_at_ = -100, motion_seen_ = 0;
  // "Show me where it is": scroll the page to a find, then point at it.
  int seek_ = 0;
  double seek_until_ = 0, seek_step_at_ = 0;
  void seek(const Record& r, double now);
  int inspect_tries_ = 0;
  double scroll_ = 0;
  RECT free_{};
  std::vector<RECT> above_;
  RECT box_{};  // overlay box while the spider is loose
  bool bubble_drag_ = false;
  Vec2 bubble_grab_{};
  Vec2 local(POINT p) const {
    const RECT b = overlay_.bounds();
    return {static_cast<float>(p.x - b.left), static_cast<float>(p.y - b.top)};
  }
  float dim_ = 0;
};

Vec2 App::offset() const {
  if (!root_) return {};
  return {static_cast<float>(-content_.left), static_cast<float>(-content_.top + scroll_)};
}

// Topmost real window under a point, skipping our own windows and the desktop.
HWND App::window_at(POINT p) const {
  for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
    if (h == overlay_.hwnd() || h == panel_hwnd_) continue;
    if (!IsWindowVisible(h) || IsIconic(h) || cloaked(h)) continue;
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) || ((ex & WS_EX_LAYERED) && (ex & WS_EX_TOOLWINDOW))) continue;
    RECT r;
    if (!GetWindowRect(h, &r) || !PtInRect(&r, p)) continue;
    if (is_shell(h)) return nullptr;
    return GetAncestor(h, GA_ROOT);
  }
  return nullptr;
}

void App::attach(HWND w, POINT at, double now) {
  if (!w || w == overlay_.hwnd() || w == panel_hwnd_) return;
  const Vec2 before = offset();
  root_ = w;
  GetWindowRect(w, &root_rect_);
  RECT cr;
  GetClientRect(w, &cr);
  MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&cr), 2);
  content_ = cr;  // until UI Automation finds the page area
  content_known_ = false;
  scroll_ = 0;
  title_ = title_of(w);
  key_ = page_key(title_);
  url_.clear();
  gen_ = vision_.watch(nullptr, RECT{});
  page_.clear();
  scene_.forget();
  pet_.forget_page();
  page_.set_title(title_);
  ++page_id_;
  inspector_.inspect(w, at, ++token_);
  inspect_at_ = now;
  inspect_tries_ = 1;
  retry_at_ = 0;
  scroll_fails_ = scroll_method_ = 0;
  scroll_dead_ = false;
  scroll_check_at_ = 0;
  page_tree_ = false;
  scrollable_ = false;
  debug_log("attach '" + utf8(title_) + "' class=" + utf8(class_of(w)));
  pet_.shift(offset() - before);
}

void App::detach() {
  if (!root_) return;
  const Vec2 before = offset();
  const Vec2 spot = to_screen(pet_.spider().pos());
  root_ = nullptr;
  content_known_ = false;
  vision_.watch(nullptr, RECT{});
  page_.clear();
  scene_.forget();
  pet_.forget_page();
  free_ = monitor_rect({static_cast<LONG>(spot.x), static_cast<LONG>(spot.y)}, true);
  pet_.shift(offset() - before);
}

// Same window, different page: start over but keep the spider where it is.
void App::new_page(double now) {
  const Vec2 before = offset();
  title_ = title_of(root_);
  key_ = page_key(title_);
  page_.clear();
  scene_.forget();
  pet_.forget_page();
  page_.set_title(title_);
  ++page_id_;
  scroll_ = 0;
  if (content_known_) gen_ = vision_.watch(root_, content_);
  pet_.shift(offset() - before);
  POINT c{(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2};
  inspector_.inspect(root_, c, ++token_);
  inspect_at_ = now;
  inspect_tries_ = 1;
  retry_at_ = 0;
  scroll_fails_ = scroll_method_ = 0;
  scroll_dead_ = false;
  scroll_check_at_ = 0;
}

void App::follow_target(double now) {
  if (!root_) return;
  if (!IsWindow(root_) || IsIconic(root_) || !IsWindowVisible(root_) || cloaked(root_)) {
    detach();
    return;
  }
  RECT r;
  GetWindowRect(root_, &r);
  const bool resized = (r.right - r.left) != (root_rect_.right - root_rect_.left) ||
                       (r.bottom - r.top) != (root_rect_.bottom - root_rect_.top);
  if (resized) {
    root_rect_ = r;
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    attach(root_, c, now);  // text reflowed: read it fresh
    return;
  }
  if (r.left != root_rect_.left || r.top != root_rect_.top) {
    const Vec2 before = offset();
    OffsetRect(&content_, r.left - root_rect_.left, r.top - root_rect_.top);
    root_rect_ = r;
    if (content_known_) vision_.move(content_);
    pet_.shift(offset() - before);  // the spider rides along with the window
  }
  // Look at the page area again now and then: pages finish loading late.
  if (content_known_ && is_browser(root_) && retry_at_ == 0 && now - inspect_at_ > 4.0) {
    inspect_at_ = now;
    inspector_.inspect(root_, {(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2}, ++token_);
  }
  if (now - title_at_ > 0.4) {
    title_at_ = now;
    if (page_key(title_of(root_)) != key_) new_page(now);
  }

  // Chromium builds its accessibility tree only after the first request, so
  // a miss is asked again a couple of times before settling for the client area.
  if (retry_at_ > 0 && now >= retry_at_) {
    retry_at_ = 0;
    ++inspect_tries_;
    inspector_.inspect(root_, {(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2}, ++token_);
  }
  Inspector::Result res;
  while (inspector_.take(res)) {
    if (res.token != token_) continue;
    if (!res.url.empty()) url_ = res.url;
    if (!res.found && inspect_tries_ < 3) retry_at_ = now + 0.8;
    RECT doc = content_;
    if (res.found) IntersectRect(&doc, &res.doc, &root_rect_);
    debug_log("inspect try=" + std::to_string(inspect_tries_) + " found=" + std::to_string(res.found) + " doc=" +
              std::to_string(doc.left) + "," + std::to_string(doc.top) + "," + std::to_string(doc.right) + "," +
              std::to_string(doc.bottom) + " scroll=" + std::to_string(res.can_scroll) + " url=" + utf8(url_));
    if (content_known_ && EqualRect(&doc, &content_)) continue;
    // Later looks only replace the page area when they found a clearly better
    // one (the page finished loading, the first pick was an inner box).
    if (content_known_) {
      const auto area = [](const RECT& r) { return static_cast<double>(r.right - r.left) * (r.bottom - r.top); };
      RECT client;
      GetClientRect(root_, &client);
      const bool tiny = area(content_) < 0.25 * area(client);
      if (!res.found || (!tiny && area(doc) < area(content_) * 1.25)) continue;
    }
    const Vec2 before = offset();
    if (content_known_) {  // a better page area turned up late: start this page over
      page_.clear();
      scene_.forget();
      pet_.forget_page();
    }
    content_ = doc;
    page_tree_ = res.found;
    scrollable_ = res.can_scroll || is_browser(root_);
    content_known_ = true;
    scroll_ = 0;
    gen_ = vision_.watch(root_, content_);
    pet_.shift(offset() - before);
  }
  if (!content_known_ && now - inspect_at_ > 2.5) {
    const Vec2 before = offset();
    content_known_ = true;
    scrollable_ = is_browser(root_);
    scroll_ = 0;
    gen_ = vision_.watch(root_, content_);
    pet_.shift(offset() - before);
  }
}

// Windows stacked over the target (the panel counts; our overlay does not).
void App::refresh_above(double now) {
  if (now - above_at_ < 0.25) return;
  above_at_ = now;
  above_.clear();
  if (!root_) return;
  for (HWND h = GetWindow(root_, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) {
    if (h == overlay_.hwnd() || !IsWindowVisible(h) || IsIconic(h) || cloaked(h)) continue;
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) continue;
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 8 || r.bottom - r.top < 8) continue;
    if (ex & WS_EX_LAYERED) {  // full-screen overlays from other apps are see-through
      const RECT m = monitor_rect({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, false);
      if ((r.right - r.left) * 10 >= (m.right - m.left) * 9 && (r.bottom - r.top) * 10 >= (m.bottom - m.top) * 9)
        continue;
    }
    above_.push_back(r);
  }
}

bool App::covered(Vec2 s) const {
  const POINT p{static_cast<LONG>(s.x), static_cast<LONG>(s.y)};
  for (const RECT& r : above_)
    if (PtInRect(&r, p)) return true;
  return false;
}

View App::view() const {
  View v;
  if (root_) {
    v.visible = {0, static_cast<float>(scroll_), static_cast<float>(content_.right - content_.left),
                 static_cast<float>(content_.bottom - content_.top)};
    v.attached = content_known_;
    const Vec2 mid{(content_.left + content_.right) * 0.5f, (content_.top + content_.bottom) * 0.5f};
    // It keeps going on its own; it backs off for a few seconds after you
    // scroll yourself, and while you hold the mouse button (selecting text).
    const bool holding = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    v.crawl_ok = settings_.crawl && content_known_ && scrollable_ && !scroll_dead_ && !holding &&
                 now_seconds() - user_scrolled_at_ > 4.0 && !covered(mid);
    v.page_end = scroll_dead_;
  } else {
    v.visible = to_rect(free_);
  }
  return v;
}

void App::frame(double now, float dt) {
  follow_target(now);
  refresh_above(now);

  if (root_ && content_known_) {
    const Vision::State st = vision_.state();
    if (st.generation == gen_) {
      scroll_ = st.scroll;
      // Motion we did not ask for is the user scrolling: back off, and the
      // page may have more below again.
      if (st.last_motion > motion_seen_) {
        motion_seen_ = st.last_motion;
        if (st.last_motion - scroll_req_at_ > 1.2) {
          user_scrolled_at_ = st.last_motion;
          scroll_dead_ = false;
          scroll_fails_ = 0;
        }
      }
    }
    OcrPass pass;
    while (vision_.take(pass)) {
      if (pass.generation != gen_) continue;
      // Window capture sees the page even under other windows; a screen copy
      // does not, so then covered lines are left out.
      const bool own_pixels = vision_.state().window_capture;
      const float drift = page_.merge(pass, [this, own_pixels](const Rect& box) {
        return own_pixels || !covered(to_screen(box.center()));
      });
      if (drift != 0) pet_.shift({0, drift});
      // Fresh text geometry: ask the page what it says about the same view.
      if (page_tree_ && now - scan_at_ > 2.5) {
        scan_at_ = now;
        scan_scroll_ = scroll_;
        inspector_.scan(root_, ++scan_token_);
      }
      debug_log("ocr lines=" + std::to_string(pass.lines.size()) + " took=" + std::to_string(int(pass.took * 1000)) +
                "ms scroll=" + std::to_string(int(pass.scroll)) + " drift=" + std::to_string(drift) +
                " entities=" + std::to_string(page_.entities().size()) + " pitch=" + std::to_string(page_.pitch()));
    }
  }

  // The page's own words. Dropped if the page scrolled while it was read.
  Inspector::Scan tree;
  while (inspector_.take_scan(tree)) {
    if (!root_ || tree.token != scan_token_ || std::fabs(scroll_ - scan_scroll_) > 2.0) continue;
    std::vector<Fact> facts;
    for (const Inspector::Node& n : tree.nodes) {
      Fact f;
      f.type = static_cast<Fact::Type>(n.type);
      f.text = n.text;
      f.url = n.url;
      f.level = n.level;
      f.box = {static_cast<float>(n.rect.left - content_.left),
               static_cast<float>(n.rect.top - content_.top + scroll_), static_cast<float>(n.rect.right - n.rect.left),
               static_cast<float>(n.rect.bottom - n.rect.top)};
      facts.push_back(std::move(f));
    }
    const float y0 = static_cast<float>(scroll_), y1 = y0 + static_cast<float>(content_.bottom - content_.top);
    page_.merge_facts(std::move(facts), y0, y1);
    debug_log("page tree: " + std::to_string(tree.nodes.size()) + " nodes in " + std::to_string(int(tree.took * 1000)) +
              " ms, entities=" + std::to_string(page_.entities().size()));
  }

  const Vec2 spider_screen = to_screen(pet_.spider().pos());
  const POINT sp_pt{static_cast<LONG>(spider_screen.x), static_cast<LONG>(spider_screen.y)};
  float base = dpi_scale(sp_pt);
  if (root_ && !page_.empty()) base = std::clamp(page_.pitch() / 22.f, 0.7f, 1.7f);
  pet_.set_scale(base * settings_.size);

  View v = view();
  if (seek_) {
    v.crawl_ok = false;  // the page is ours to steer for a moment
    const Entity* e = page_.find(seek_);
    const float h = static_cast<float>(content_.bottom - content_.top);
    if (!root_ || !e || now > seek_until_) {
      seek_ = 0;
    } else {
      const float want = e->box.center().y - h * 0.4f;  // put it a bit above the middle
      const float delta = want - static_cast<float>(scroll_);
      if (std::fabs(delta) < h * 0.25f) {
        Entity* live = page_.find(seek_);
        live->applied_at = now;  // replay its glitch so it pops
        pet_.point(*live, now);
        user_scrolled_at_ = now + 4.0;  // and let you look before crawling on
        seek_ = 0;
      } else if (now - seek_step_at_ > 0.3 && now - vision_.state().last_motion > 0.12) {
        seek_step_at_ = now;
        const int notches = std::clamp(static_cast<int>(delta / 100.f), -6, 6);
        scroll_req_at_ = now;
        inspector_.scroll(root_, {(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2},
                          notches == 0 ? (delta > 0 ? 1 : -1) : notches, scroll_method_);
      }
    }
  }
  pet_.update(now, dt, page_, v, settings_);

  if (scroll_check_at_ > 0 && now >= scroll_check_at_) {
    scroll_check_at_ = 0;
    if (std::fabs(scroll_ - scroll_before_) < 4) {
      ++scroll_fails_;
      if (scroll_fails_ == 2) scroll_method_ = 1;  // try the wheel instead
      if (scroll_fails_ >= 4) scroll_dead_ = true;
      debug_log("scroll did not move, fails=" + std::to_string(scroll_fails_));
    } else {
      scroll_fails_ = 0;
    }
  }
  if (v.crawl_ok && pet_.wants_scroll() > 20.f * pet_.spider().scale() && now - scroll_req_at_ > 0.45 &&
      now - vision_.state().last_motion > 0.15 && scroll_check_at_ == 0) {
    scroll_req_at_ = now;
    scroll_before_ = scroll_;
    scroll_check_at_ = now + 0.8;
    debug_log("scroll request, spider wants " + std::to_string(int(pet_.wants_scroll())));
    inspector_.scroll(root_, {(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2}, 1, scroll_method_);
  }

  for (const Ask& a : pet_.take_asks())
    if (settings_.brain) llm_.ask(a, page_id_, title_);
  for (const Llm::Answer& ans : llm_.take()) {
    if (ans.page != page_id_) continue;
    pet_.answer(ans.ask, ans.text, page_, now);
    debug_log("llm " + ans.ask.task + ": " + ans.text);
    if (ans.ask.entity > 0) store_.summarize(page_id_, ans.ask.entity, ans.text);
  }
  for (const Harvest& h : pet_.take_harvest()) {
    store_.add(h, page_id_, url_.empty() ? title_ : url_);
    debug_log(std::string("harvest ") + kind_name(h.kind) + ": " + utf8(h.text));
  }

  // Hover and grab.
  POINT cur;
  GetCursorPos(&cur);
  const bool over = pet_.hit(to_content(cur));
  const bool over_bubble = scene_.bubble_hit(local(cur));
  overlay_.grab_cursor = over || over_bubble;
  overlay_.set_interactive(over || over_bubble || pet_.dragging() || bubble_drag_);

  // Keep the see-through window as small as the moment allows: the page plus
  // a margin for legs when attached, a box around the spider when loose. A
  // full-screen overlay would make Windows compose every game under it.
  const float reach = pet_.spider().reach();
  const Pet::State st = pet_.state();
  const bool airborne = st == Pet::State::Descend || st == Pet::State::Ascend || st == Pet::State::Thrown;
  RECT want{};
  if (root_ && !pet_.dragging()) {
    const RECT mon = monitor_rect({(content_.left + content_.right) / 2, (content_.top + content_.bottom) / 2}, false);
    RECT grown = content_;
    InflateRect(&grown, static_cast<int>(reach * 1.4f + 40), static_cast<int>(reach * 1.4f + 40));
    IntersectRect(&want, &grown, &mon);
    box_ = {};
  } else if (airborne && !pet_.dragging()) {
    want = monitor_rect(sp_pt, false);
    box_ = {};
  } else {
    // Loose or held: a fixed-size box that only moves when the spider nears its edge.
    const int half = static_cast<int>(reach * 2.4f + 140);  // room for the thought bubble
    const POINT c = pet_.dragging() ? cur : sp_pt;
    RECT inner = box_;
    InflateRect(&inner, -half / 2, -half / 2);
    if (box_.right - box_.left != 2 * half || !PtInRect(&inner, c)) box_ = {c.x - half, c.y - half, c.x + half, c.y + half};
    want = box_;
  }
  overlay_.cover(want);

  // Step aside entirely for full-screen apps and games, and when the page is
  // buried under other windows: no window of ours left for Windows to compose.
  bool step_aside = false;
  if (!pet_.dragging()) {
    HWND fg = GetForegroundWindow();
    if (fg && fg != root_ && fg != panel_hwnd_ && fg != overlay_.hwnd() && !is_shell(fg)) {
      RECT fr;
      const RECT fm = monitor_rect(sp_pt, false);
      if (GetWindowRect(fg, &fr) && fr.left <= fm.left && fr.top <= fm.top && fr.right >= fm.right &&
          fr.bottom >= fm.bottom && MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) == MonitorFromPoint(sp_pt, MONITOR_DEFAULTTONEAREST))
        step_aside = true;
    }
    if (root_) {
      int hidden_pts = 0;
      for (int gy = 0; gy < 6; ++gy)
        for (int gx = 0; gx < 6; ++gx) {
          const Vec2 q{content_.left + (content_.right - content_.left) * (gx + 0.5f) / 6.f,
                       content_.top + (content_.bottom - content_.top) * (gy + 0.5f) / 6.f};
          hidden_pts += covered(q) ? 1 : 0;
        }
      if (hidden_pts >= 34) step_aside = true;
    }
  }
  overlay_.set_visible(!step_aside);
  // Visible to Discord, OBS and screenshots, unless the page is read by copying
  // the screen (then the spider would read itself) or the user turned it off.
  overlay_.set_capturable(settings_.share && (!root_ || !content_known_ || vision_.state().window_capture));
  if (step_aside) {
    Sleep(15);  // nothing to draw; do not spin
    return;
  }

  // Gone completely, not just faded, when another window is over it.
  const bool hidden = root_ && covered(spider_screen) && !pet_.dragging();
  dim_ = lerp(dim_, hidden ? 1.f : 0.f, damp(10.f, dt));

  if (ID2D1DeviceContext* ctx = overlay_.begin()) {
    painter_.attach(ctx, overlay_.d2d(), overlay_.dwrite());
    const RECT b = overlay_.bounds();
    const Vec2 origin{static_cast<float>(-b.left), static_cast<float>(-b.top)};
    const Vec2 to_local = origin - offset();
    const Rect area = root_ ? to_rect(content_) : to_rect(b);
    const Rect clip = area.moved(origin.x, origin.y);
    // Windows stacked over the page: harvested marks must not paint over them.
    std::vector<Rect> holes;
    if (root_)
      for (const RECT& r : above_) {
        RECT hit;
        if (IntersectRect(&hit, &r, &content_)) holes.push_back(to_rect(hit).moved(origin.x, origin.y));
      }
    scene_.draw(painter_, page_, pet_, settings_, to_local, clip, holes, now, dim_, dt);
    overlay_.end();
  }
}

void App::seek(const Record& r, double now) {
  const Entity* e = (root_ && r.page == page_id_) ? page_.find(r.entity) : nullptr;
  if (!e) {
    // Not on the page we are on now: open where it came from, if that is a web address.
    const std::string where = !r.url.empty() ? r.url : r.source;
    if (where.rfind("http", 0) == 0) ShellExecuteW(nullptr, L"open", wide(where).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else panel_.notice("That page is not open in front of the spider anymore.");
    return;
  }
  SetForegroundWindow(root_);
  seek_ = r.entity;
  seek_until_ = now + 8.0;
  seek_step_at_ = 0;
}

void App::handle(const PanelActions& a, double now) {
  if (a.show >= 0 && a.show < static_cast<int>(store_.all().size())) seek(store_.all()[a.show], now);
  if (!a.open_url.empty()) ShellExecuteW(nullptr, L"open", wide(a.open_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (!a.clip.empty()) {
    copy_text(panel_hwnd_, a.clip);
    panel_.notice("Copied.");
  }
  if (a.reroll) pet_.reroll_name();
  if (a.recall) {
    detach();
    RECT pr;
    GetWindowRect(panel_hwnd_, &pr);
    free_ = monitor_rect({(pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2}, true);
    pet_.summon({(free_.left + free_.right) * 0.5f, free_.top + (free_.bottom - free_.top) * 0.4f}, view(), now);
  }
  if (a.clear_marks) {
    for (Entity& e : page_.entities())
      if (e.mark == Mark::Done) e.mark = Mark::Skip;
    pet_.clear_silk();
  }
  if (a.save_json) {
    const std::wstring p = store_.save_json();
    panel_.notice(p.empty() ? "Could not save." : "Saved " + utf8(p));
  }
  if (a.save_csv) {
    const std::wstring p = store_.save_csv();
    panel_.notice(p.empty() ? "Could not save." : "Saved " + utf8(p));
  }
  if (a.copy) {
    copy_text(panel_hwnd_, store_.as_text());
    panel_.notice("Copied " + std::to_string(store_.all().size()) + " items.");
  }
  if (a.open_folder) ShellExecuteW(nullptr, L"open", documents_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (a.clear_list) store_.clear();
  if (a.settings_changed) save_settings(settings_);
}

int App::run(HINSTANCE inst, const Options& opt) {
  settings_ = load_settings();
  if (!panel_.create(inst, opt.quiet, opt.panel_at ? &opt.panel_pos : nullptr)) return 1;
  panel_hwnd_ = panel_.hwnd();
  if (!overlay_.create(inst)) {
    MessageBoxW(panel_hwnd_, L"Could not create the spider window (Direct2D).", L"SpiderPet", MB_ICONERROR);
    return 1;
  }
  RegisterHotKey(overlay_.hwnd(), 1, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');
  vision_.start();
  inspector_.start();
  llm_.start();

  overlay_.on_mouse = [this](Overlay::MouseEvent ev, POINT p) {
    const double now = now_seconds();
    if (ev == Overlay::MouseEvent::Double) {
      if (scene_.bubble_hit(local(p))) scene_.unpin_bubble();  // back to following the spider
      return;
    }
    if (ev == Overlay::MouseEvent::Down) {
      if (pet_.hit(to_content(p))) {
        pet_.grab(to_content(p), now);
      } else if (scene_.bubble_hit(local(p))) {
        bubble_drag_ = true;
        bubble_grab_ = scene_.bubble_anchor() - local(p);
      }
    } else if (ev == Overlay::MouseEvent::Move) {
      if (pet_.dragging()) pet_.drag(to_content(p), now);
      if (bubble_drag_) scene_.pin_bubble(local(p) + bubble_grab_);
    } else if (bubble_drag_) {
      bubble_drag_ = false;
    } else if (pet_.dragging()) {
      pet_.release(now);
      const HWND w = window_at(p);
      if (!w) {
        detach();
        free_ = monitor_rect(p, true);
      } else if (w != root_) {
        attach(w, p, now);
      }
    }
  };
  // Alt+Shift+S: the spider rappels down onto whatever is under the mouse.
  overlay_.on_hotkey = [this] {
    const double now = now_seconds();
    POINT c;
    GetCursorPos(&c);
    const HWND w = window_at(c);
    if (!w) {
      detach();
      free_ = monitor_rect(c, true);
    } else if (w != root_) {
      attach(w, c, now);
    }
    pet_.summon(to_content(c), view(), now);
  };

  RECT pr;
  GetWindowRect(panel_hwnd_, &pr);
  free_ = monitor_rect({(pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2}, true);
  pet_.set_scale(dpi_scale({free_.left + 10, free_.top + 10}) * settings_.size);

  HWND start_on = nullptr;
  if (!opt.attach_title.empty()) {
    struct Find {
      std::wstring want;
      HWND hit = nullptr;
    } find{opt.attach_title, nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
          auto* f = reinterpret_cast<Find*>(lp);
          const std::wstring cls = class_of(h);
          if (cls == L"SpiderPetOverlay" || cls == L"SpiderPetPanel") return TRUE;
          if (IsWindowVisible(h) && !IsIconic(h) && title_of(h).find(f->want) != std::wstring::npos) {
            f->hit = h;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    start_on = find.hit;
  }
  const double t0 = now_seconds();
  if (start_on) {
    RECT r;
    GetWindowRect(start_on, &r);
    const POINT c{(r.left + r.right) / 2, r.top + (r.bottom - r.top) / 3};
    attach(start_on, c, t0);
    pet_.summon(to_content(c), view(), t0 + 0.3);
  } else {
    pet_.summon({free_.left + (free_.right - free_.left) * 0.62f, free_.top + (free_.bottom - free_.top) * 0.4f},
                view(), t0 + 0.3);
  }

  double last = now_seconds(), panel_at = 0;
  MSG msg;
  bool quit = false;
  while (!quit && !panel_.closed()) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    const double now = now_seconds();
    if (opt.seconds > 0 && now - t0 > opt.seconds) break;
    const float dt = static_cast<float>(std::min(0.05, now - last));
    last = now;
    frame(now, dt);
    if (now - panel_at > 1.0 / 30) {
      panel_at = now;
      PanelModel m;
      m.name = pet_.name();
      m.state = pet_.state_name();
      m.where = root_ ? page_key(title_) : L"";
      m.thought = pet_.thought();
      m.mind.assign(pet_.history().begin(), pet_.history().end());
      m.now = now;
      m.gist = pet_.gist();
      m.brain = settings_.brain ? llm_.status() : "off";
      m.reader_ready = vision_.reader_ready() || now - t0 < 3;
      m.attached = root_ != nullptr;
      m.found = static_cast<int>(page_.entities().size());
      m.harvested = page_.harvested();
      m.store = &store_;
      handle(panel_.draw(m, settings_), now);
    }
  }

  UnregisterHotKey(overlay_.hwnd(), 1);
  llm_.stop();
  inspector_.stop();
  vision_.stop();
  overlay_.destroy();
  panel_.destroy();
  return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Options opt;
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) {
      const std::wstring a = argv[i];
      if (a == L"--attach" && i + 1 < argc) opt.attach_title = argv[++i];
      else if (a == L"--seconds" && i + 1 < argc) opt.seconds = _wtof(argv[++i]);
      else if (a == L"--quiet") opt.quiet = true;
      else if (a == L"--second") opt.second = true;
      else if (a == L"--panel" && i + 2 < argc) {
        opt.panel_at = true;
        opt.panel_pos = {_wtoi(argv[i + 1]), _wtoi(argv[i + 2])};
        i += 2;
      }
    }
    LocalFree(argv);
  }
  HANDLE once = opt.second ? nullptr : CreateMutexW(nullptr, TRUE, L"SpiderPet.SingleInstance");
  if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND w = FindWindowW(L"SpiderPetPanel", nullptr)) {
      ShowWindow(w, SW_RESTORE);
      SetForegroundWindow(w);
    }
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int rc = 0;
  {
    App app;
    rc = app.run(inst, opt);
  }
  CoUninitialize();
  if (once) CloseHandle(once);
  return rc;
}
