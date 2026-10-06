#include "eyes/eyes.hpp"

#include "core/util.hpp"

#include <dwmapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <cstring>
#include <regex>
#include <set>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace sp::eyes {

namespace {

std::string bstr_utf8(BSTR b) { return b ? utf8(std::wstring_view(b, SysStringLen(b))) : std::string(); }

Box to_box(const RECT& r) { return Box{float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top)}; }

// Who an element is (its runtime id), to compare elements without asking the app again.
std::vector<int> rid_of(IUIAutomationElement* e, bool cached) {
  std::vector<int> id;
  SAFEARRAY* sa = nullptr;
  VARIANT v;
  VariantInit(&v);
  if (cached) {
    if (SUCCEEDED(e->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &v)) && (v.vt & VT_ARRAY) && v.parray) sa = v.parray;
  } else if (FAILED(e->GetRuntimeId(&sa))) {
    sa = nullptr;
  }
  if (sa) {
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(sa, 1, &lo);
    SafeArrayGetUBound(sa, 1, &hi);
    for (LONG k = lo; k <= hi && k - lo < 16; ++k) {
      int x = 0;
      SafeArrayGetElement(sa, &k, &x);
      id.push_back(x);
    }
    if (!cached) SafeArrayDestroy(sa);
  }
  VariantClear(&v);
  return id;
}

std::string squash(const std::string& s) {
  std::string out;
  bool sp = false;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    // nbsp, and the object placeholder U+FFFC browsers put where an image is
    if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA0) { ++i; c = ' '; }
    else if (c == 0xEF && i + 2 < s.size() && (unsigned char)s[i + 1] == 0xBF && (unsigned char)s[i + 2] == 0xBC) { i += 2; c = ' '; }
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

std::string range_text(IUIAutomationTextRange* r, int max = -1) {
  BSTR b = nullptr;
  if (FAILED(r->GetText(max, &b))) return {};
  std::string s = bstr_utf8(b);
  SysFreeString(b);
  return s;
}

std::vector<Box> range_rects(IUIAutomationTextRange* r) {
  std::vector<Box> out;
  SAFEARRAY* sa = nullptr;
  if (FAILED(r->GetBoundingRectangles(&sa)) || !sa) return out;
  LONG lo = 0, hi = -1;
  SafeArrayGetLBound(sa, 1, &lo);
  SafeArrayGetUBound(sa, 1, &hi);
  double* v = nullptr;
  if (SUCCEEDED(SafeArrayAccessData(sa, reinterpret_cast<void**>(&v)))) {
    for (LONG i = 0; i + 3 <= hi - lo; i += 4)
      if (v[i + 2] > 0 && v[i + 3] > 0) out.push_back(Box{float(v[i]), float(v[i + 1]), float(v[i + 2]), float(v[i + 3])});
    SafeArrayUnaccessData(sa);
  }
  SafeArrayDestroy(sa);
  return out;
}

// Lowercase letters and digits only: matching on this survives differences in
// spaces, quotes, dashes and where the app splits its text into pieces.
void alnum_append(const std::wstring& s, int leaf, std::wstring& out, std::vector<std::pair<int, int>>* map) {
  for (size_t i = 0; i < s.size(); ++i) {
    wchar_t c = s[i];
    if (!IsCharAlphaNumericW(c)) continue;
    wchar_t lc = c;
    CharLowerBuffW(&lc, 1);
    out += lc;
    if (map) map->push_back({leaf, int(i)});
  }
}

std::wstring alnum(const std::string& s) {
  std::wstring out;
  alnum_append(wide(s), 0, out, nullptr);
  return out;
}

std::wstring exe_path(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return {};
  wchar_t buf[MAX_PATH * 2];
  DWORD n = MAX_PATH * 2;
  std::wstring out;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) out.assign(buf, n);
  CloseHandle(p);
  return out;
}

// The program's own name for itself ("Mozilla Firefox", "Discord").
std::string product_name(const std::wstring& path) {
  if (path.empty()) return {};
  DWORD dummy = 0;
  DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
  if (!size) return {};
  std::vector<BYTE> buf(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
  struct LangCp { WORD lang, cp; }* tr = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) || !tr || len < sizeof(LangCp)) return {};
  for (const wchar_t* key : {L"FileDescription", L"ProductName"}) {
    wchar_t q[128];
    swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, key);
    wchar_t* val = nullptr;
    UINT vlen = 0;
    if (VerQueryValueW(buf.data(), q, reinterpret_cast<void**>(&val), &vlen) && val && vlen > 1) {
      std::string s = squash(utf8(std::wstring_view(val, wcsnlen(val, vlen))));
      if (!s.empty()) return s;
    }
  }
  return {};
}

const std::regex kSecretRx(R"(passw|card.?num|cvv|cvc|security.?code|iban|ssn|social.?sec|\botp\b|one.?time.?code|pin code)", std::regex::icase);

}  // namespace

bool is_ours(HWND h) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  return pid == GetCurrentProcessId();
}

AppInfo identify(HWND h) {
  AppInfo a;
  if (!h || !IsWindow(h)) return a;
  a.hwnd = h;
  GetWindowThreadProcessId(h, &a.pid);
  wchar_t t[512] = {};
  GetWindowTextW(h, t, 512);
  a.title = utf8(t);
  wchar_t c[128] = {};
  GetClassNameW(h, c, 128);
  a.cls = utf8(c);
  std::wstring path = exe_path(a.pid);
  size_t slash = path.find_last_of(L"\\/");
  a.exe = utf8(lower(slash == std::wstring::npos ? path : path.substr(slash + 1)));
  a.name = product_name(path);
  if (a.name.empty()) {
    a.name = a.exe;
    if (a.name.size() > 4 && a.name.compare(a.name.size() - 4, 4, ".exe") == 0) a.name.resize(a.name.size() - 4);
    if (!a.name.empty()) a.name[0] = char(std::toupper((unsigned char)a.name[0]));
  }
  static const std::set<std::string> chromium = {"brave.exe", "chrome.exe", "msedge.exe", "vivaldi.exe", "opera.exe",
                                                 "chromium.exe", "thorium.exe", "arc.exe", "yandex.exe"};
  static const std::set<std::string> gecko = {"firefox.exe", "librewolf.exe", "waterfox.exe", "floorp.exe", "zen.exe", "mullvadbrowser.exe"};
  if (a.cls == "Chrome_WidgetWin_1" && chromium.count(a.exe)) a.engine = "chromium";
  else if (a.cls == "MozillaWindowClass" && gecko.count(a.exe)) a.engine = "firefox";
  a.browser = !a.engine.empty();
  // Electron and other Chromium-built apps (Discord, VS Code, Slack) read the same way as a browser page.
  if (!a.browser && a.cls == "Chrome_WidgetWin_1") a.engine = "chromium";
  return a;
}

HWND window_at(POINT pt) {
  HWND h = WindowFromPoint(pt);
  h = h ? GetAncestor(h, GA_ROOT) : nullptr;
  if (!h || is_ours(h)) return nullptr;
  wchar_t c[64] = {};
  GetClassNameW(h, c, 64);
  if (wcscmp(c, L"Progman") == 0 || wcscmp(c, L"WorkerW") == 0 || wcscmp(c, L"Shell_TrayWnd") == 0) return nullptr;
  return h;
}

struct Eyes::Impl {
  // ------------------------------------------------ thread plumbing
  std::thread th;
  std::mutex qmu;
  std::condition_variable qcv;
  std::deque<std::function<void()>> tasks;
  std::deque<std::pair<POINT, std::function<void(const WordHit&)>>> words;
  bool quit = false;

  // ------------------------------------------------ worker thread only
  ComPtr<IUIAutomation> uia;
  ComPtr<IUIAutomationTreeWalker> walker;
  ComPtr<IUIAutomationElement> root, doc, main;
  ComPtr<IUIAutomationTextPattern> tp;
  HWND hwnd = nullptr;
  AppInfo app;
  std::wstring title;
  std::string url;
  ULONGLONG last_url_check = 0;

  struct Leaf {
    ComPtr<IUIAutomationElement> el;
    std::wstring text;
    RECT r{};
    int block = 0;
    int heading = 0;
    bool own = false;  // an app's row or list item: a line of its own
    std::string href;
    int box = 0;       // the element it sits in (a paragraph, a cell), links aside
    int item = 0;      // the list item, row or tree item it sits in (0: none)
    std::vector<int> rid;                // who it is
    std::vector<std::vector<int>> up;    // the small elements around it (a link, a paragraph), nearest first
  };
  std::vector<Leaf> leaves;
  int box_seq = 0, cur_box = 0, cur_item = 0;  // while collecting: where the pieces sit
  std::vector<std::pair<std::vector<int>, long long>> anc;  // while collecting: the elements above, and their areas
  long long small_area = 0;                                  // an element this big or smaller is "around" a piece, not a page section
  // Hit tests (is this find really showing?), kept a moment: find id + where -> (when, yes/no).
  std::map<std::string, std::pair<ULONGLONG, bool>> hits_seen;
  std::vector<std::string> blocks;
  std::vector<int> block_heading;
  std::vector<Box> block_geom;
  std::vector<std::string> controls;
  std::wstring flat;
  std::vector<std::pair<int, int>> flat_map;
  bool leaves_ready = false;
  size_t seen_elements = 0;
  int text_lookups = 0;
  ComPtr<IUIAutomationElement> anchor;
  double anchor_base = 0;
  ComPtr<IUIAutomationElement> scroller;  // no anchor: the content's scroller still shows that it moved
  long line_h = 0;                        // the usual height of a line of text here
  ULONGLONG leaves_at_ms = 0;             // when the pieces were read
  unsigned leaves_layout = 0;
  bool leaves_steady = false;             // nothing moved while they were read
  RECT last_view{};

  // ------------------------------------------------ the tracking thread
  // Geometry only, ~120 times a second, on its own thread: a long read on the
  // worker never freezes the overlay. UI Automation objects of an MTA client
  // can be used from any MTA thread.
  std::thread tth;
  std::atomic<bool> tquit{false};
  std::atomic<bool> anchor_lost{false};
  std::mutex tmu;
  struct Track {
    HWND hwnd = nullptr;
    ComPtr<IUIAutomationElement> doc, anchor, scroller;
    double anchor_base = 0;
    unsigned ver = 0;
  } trk;

  struct Held {
    ComPtr<IUIAutomationElement> el;
    Item item;
    CONTROLTYPEID ct = 0;
  };
  std::vector<Held> held;  // the numbered things from the last items()
  std::vector<Dialog> dialogs_seen;  // the pop-ups from the last items()

  // ------------------------------------------------ published
  std::mutex smu;
  State st;

  Impl() {
    th = std::thread([this] { run(); });
    tth = std::thread([this] { track_run(); });
  }
  ~Impl() {
    tquit = true;
    if (tth.joinable()) tth.join();
    {
      std::lock_guard<std::mutex> lock(qmu);
      quit = true;
    }
    qcv.notify_all();
    if (th.joinable()) th.join();
  }

  // Hands the tracker what it measures (worker thread).
  void publish_track() {
    std::lock_guard<std::mutex> lock(tmu);
    trk.hwnd = hwnd;
    trk.doc = doc;
    trk.anchor = anchor;
    trk.scroller = scroller;
    trk.anchor_base = anchor_base;
    ++trk.ver;
  }

  void track_run() {
    SetThreadDescription(GetCurrentThread(), L"spiderpet-track");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Track t;
    unsigned ver = ~0u;
    RECT last_w{}, last_v{}, last_a{};
    bool had_anchor = false, had_sp = false, first = true;
    double last_sp[2] = {-1, -1};
    while (!tquit) {
      {
        std::lock_guard<std::mutex> lock(tmu);
        if (trk.ver != ver) {
          // A new anchor or scroller is not a move; a new window is.
          if (trk.hwnd != t.hwnd) first = true;
          ver = trk.ver;
          t = trk;
          had_anchor = had_sp = false;
        }
      }
      if (!t.hwnd || !IsWindow(t.hwnd)) {
        Sleep(30);
        continue;
      }
      RECT wr{};
      GetWindowRect(t.hwnd, &wr);
      // On screen: shown, not minimized, not on another virtual desktop. It
      // need not be in front: the spider stays on its window while you work in
      // another one (windows on top of it cover the spider too).
      BOOL cloaked = FALSE;
      DwmGetWindowAttribute(t.hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
      bool on = IsWindowVisible(t.hwnd) && !IsIconic(t.hwnd) && !cloaked;
      ULONGLONG now = GetTickCount64();
      if (!on) {
        std::lock_guard<std::mutex> lock(smu);
        if (st.hwnd == t.hwnd) {
          if (st.visible) st.moved_ms = now;
          st.visible = false;
          st.window = to_box(wr);
        }
        first = true;
        Sleep(30);
        continue;
      }
      RECT v{};
      bool doc_ok = t.doc && SUCCEEDED(t.doc->get_CurrentBoundingRectangle(&v)) && v.right > v.left;
      if (!doc_ok) {
        GetClientRect(t.hwnd, &v);
        POINT tl{v.left, v.top}, br{v.right, v.bottom};
        ClientToScreen(t.hwnd, &tl);
        ClientToScreen(t.hwnd, &br);
        v = RECT{tl.x, tl.y, br.x, br.y};
      }
      // Scroll: an anchor paragraph's real position (browsers report it even
      // off screen) gives the exact amount.
      RECT ar{};
      bool anchor_ok = t.anchor && SUCCEEDED(t.anchor->get_CurrentBoundingRectangle(&ar)) && ar.bottom > ar.top;
      double sp[2] = {-1, -1};
      bool sp_ok = false;
      if (!anchor_ok && t.scroller) {
        ComPtr<IUIAutomationScrollPattern> p;
        if (SUCCEEDED(t.scroller->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&p))) && p)
          sp_ok = SUCCEEDED(p->get_CurrentVerticalScrollPercent(&sp[0])) && SUCCEEDED(p->get_CurrentHorizontalScrollPercent(&sp[1]));
      }
      bool moved = first || !EqualRect(&wr, &last_w) || !EqualRect(&v, &last_v);
      // A new width flows the text again; a new height (a bar opening) only moves it.
      bool relaid = !first && (v.right - v.left) != (last_v.right - last_v.left);
      if (anchor_ok && had_anchor) {
        if (ar.top - v.top != last_a.top - last_v.top || ar.left - v.left != last_a.left - last_v.left) moved = true;
        // The same paragraph, another size: the text flowed again (a zoom, a new width).
        if (ar.right - ar.left != last_a.right - last_a.left || ar.bottom - ar.top != last_a.bottom - last_a.top) moved = relaid = true;
      }
      if (!anchor_ok && had_anchor) {
        moved = relaid = true;  // the anchor is gone (a page that rebuilt itself): the worker picks a new one
        anchor_lost = true;
        t.anchor.Reset();
      }
      if (sp_ok && had_sp && (sp[0] != last_sp[0] || sp[1] != last_sp[1])) moved = true;
      {
        std::lock_guard<std::mutex> lock(smu);
        if (st.hwnd == t.hwnd) {
          st.visible = true;
          st.window = to_box(wr);
          st.view = to_box(v);
          st.dpi_scale = GetDpiForWindow(t.hwnd) / 96.0;
          if (anchor_ok) {
            double s = t.anchor_base - double(ar.top - v.top);
            if (std::fabs(s - st.scroll) > 0.5) st.scroll = s;
          }
          if (moved) st.moved_ms = now;
          if (relaid) ++st.layout;
        }
      }
      last_w = wr;
      last_v = v;
      last_a = ar;
      had_anchor = anchor_ok;
      had_sp = sp_ok;
      last_sp[0] = sp[0];
      last_sp[1] = sp[1];
      first = false;
      Sleep(8);
    }
    t = Track{};
    {
      std::lock_guard<std::mutex> lock(tmu);
      trk = Track{};
    }
    CoUninitialize();
  }

  void post(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> lock(qmu);
      tasks.push_back(std::move(f));
    }
    qcv.notify_all();
  }

  template <class F>
  auto call(F f, int timeout_ms) -> decltype(f()) {
    using R = decltype(f());
    auto p = std::make_shared<std::promise<R>>();
    auto fut = p->get_future();
    post([p, f]() mutable {
      try {
        p->set_value(f());
      } catch (...) {
        p->set_exception(std::current_exception());
      }
    });
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) return R{};
    try {
      return fut.get();
    } catch (...) {
      return R{};
    }
  }

  void run() {
    SetThreadDescription(GetCurrentThread(), L"spiderpet-eyes");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (uia) {
      uia->get_ControlViewWalker(&walker);
      // Apps can be slow to answer while they load; never hang on them.
      ComPtr<IUIAutomation2> u2;
      if (SUCCEEDED(uia.As(&u2))) {
        u2->put_ConnectionTimeout(3000);
        u2->put_TransactionTimeout(4000);
      }
    }
    while (true) {
      std::deque<std::function<void()>> batch;
      std::deque<std::pair<POINT, std::function<void(const WordHit&)>>> wbatch;
      {
        std::unique_lock<std::mutex> lock(qmu);
        qcv.wait_for(lock, std::chrono::milliseconds(hwnd ? 33 : 250), [&] { return quit || !tasks.empty(); });
        if (quit) break;
        batch.swap(tasks);
        while (words.size() > 8) words.pop_front();
        for (int i = 0; i < 6 && !words.empty(); ++i) {
          wbatch.push_back(words.front());
          words.pop_front();
        }
      }
      for (auto& t : batch) t();
      for (auto& [pt, cb] : wbatch) cb(word_at_now(pt));
      if (hwnd) tick();
    }
    {
      std::lock_guard<std::mutex> lock(tmu);
      trk = Track{};
    }
    held.clear();
    leaves.clear();
    anchor.Reset();
    scroller.Reset();
    tp.Reset();
    main.Reset();
    doc.Reset();
    root.Reset();
    walker.Reset();
    uia.Reset();
    CoUninitialize();
  }

  // ------------------------------------------------ finding the content

  // A document filling much of the window: a web page, an Electron app's view,
  // a text editor. Firefox keeps documents for background tabs too, so the
  // visible one is the biggest.
  ComPtr<IUIAutomationElement> find_doc() {
    ComPtr<IUIAutomationElement> best;
    if (!root) return best;
    VARIANT v;
    v.vt = VT_I4;
    v.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond);
    ComPtr<IUIAutomationCacheRequest> cr;
    uia->CreateCacheRequest(&cr);
    cr->AddProperty(UIA_BoundingRectanglePropertyId);
    cr->AddProperty(UIA_IsOffscreenPropertyId);
    ComPtr<IUIAutomationElementArray> docs;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cr.Get(), &docs)) || !docs) return best;
    int n = 0;
    docs->get_Length(&n);
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    long win_area = std::max(1L, (wr.right - wr.left) * (wr.bottom - wr.top));
    long best_area = 0;
    for (int i = 0; i < n; ++i) {
      ComPtr<IUIAutomationElement> e;
      docs->GetElement(i, &e);
      RECT r{};
      e->get_CachedBoundingRectangle(&r);
      BOOL off = FALSE;
      e->get_CachedIsOffscreen(&off);
      long area = (r.right - r.left) * (r.bottom - r.top);
      if (off) area /= 4;
      if (area > best_area) {
        best_area = area;
        best = e;
      }
    }
    // A small text box inside an app is not the app's content.
    if (best && best_area * 5 < win_area) best.Reset();
    return best;
  }

  void reset_page() {
    leaves.clear();
    blocks.clear();
    block_heading.clear();
    block_geom.clear();
    controls.clear();
    flat.clear();
    flat_map.clear();
    leaves_ready = false;
    anchor.Reset();
    scroller.Reset();
    held.clear();
    publish_track();
    std::lock_guard<std::mutex> lock(smu);
    st.gen++;
    st.scroll = 0;
    st.jump++;
    st.layout++;
    st.moved_ms = GetTickCount64();
  }

  bool bind_doc(ComPtr<IUIAutomationElement> dd) {
    doc = dd;
    tp.Reset();
    main.Reset();
    if (!doc) return false;
    doc->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp));
    // The page's main landmark, when it holds most of the text, skips menus and footers.
    if (tp) {
      VARIANT lm;
      lm.vt = VT_I4;
      lm.lVal = UIA_MainLandmarkTypeId;
      ComPtr<IUIAutomationCondition> c;
      uia->CreatePropertyCondition(UIA_LandmarkTypePropertyId, lm, &c);
      ComPtr<IUIAutomationElement> m;
      if (SUCCEEDED(doc->FindFirst(TreeScope_Descendants, c.Get(), &m)) && m) {
        ComPtr<IUIAutomationTextRange> r;
        if (SUCCEEDED(tp->RangeFromChild(m.Get(), &r)) && r && range_text(r.Get(), 2000).size() >= 500) main = m;
      }
    }
    url = doc_url();
    RECT dr{};
    if (SUCCEEDED(doc->get_CurrentBoundingRectangle(&dr)) && dr.right > dr.left) last_view = dr;
    publish_track();
    return true;
  }

  std::string doc_url() {
    if (!doc) return {};
    VARIANT v;
    VariantInit(&v);
    std::string u;
    if (SUCCEEDED(doc->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &v)) && v.vt == VT_BSTR) u = bstr_utf8(v.bstrVal);
    VariantClear(&v);
    // Only web pages have a real address; editors put their text in Value.
    if (u.rfind("http", 0) != 0 && u.rfind("file:", 0) != 0) u.clear();
    return u;
  }

  std::string app_url() {
    std::string t = utf8(title);
    // "*name.txt": unsaved changes are the same file.
    while (!t.empty() && (t[0] == '*' || t[0] == ' ')) t.erase(t.begin());
    for (auto& c : t)
      if (c == '/' || c == '\\' || c == '?' || c == '#') c = ' ';
    return "app://" + app.exe + "/" + squash(t).substr(0, 120);
  }

  bool attach_now(HWND h, int timeout_ms, std::string* err) {
    if (!uia) {
      if (err) *err = "Windows' screen-reader interface is not available";
      return false;
    }
    if (h != hwnd) {
      detach_now();
      hwnd = h;
    }
    app = identify(h);
    root.Reset();
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) {
      if (err) *err = "can't read this window";
      hwnd = nullptr;
      return false;
    }
    // The first request switches a browser's accessibility on; its page tree
    // fills in over the next second or two.
    ULONGLONG until = GetTickCount64() + timeout_ms;
    while (true) {
      ComPtr<IUIAutomationElement> dd = find_doc();
      if (dd) {
        bind_doc(dd);
        if (!app.engine.empty() ? doc_text_len() > 200 : true) break;
      } else if (app.engine.empty() || GetTickCount64() > until) {
        break;  // an app without a document: its controls are its content
      }
      if (GetTickCount64() > until) break;
      Sleep(250);
    }
    wchar_t t[512] = {};
    GetWindowTextW(hwnd, t, 512);
    title = t;
    reset_page();
    if (url.empty()) url = app_url();
    std::lock_guard<std::mutex> lock(smu);
    st.attached = true;
    st.hwnd = hwnd;
    st.url = url;
    return true;
  }

  void detach_now() {
    hwnd = nullptr;
    held.clear();
    leaves.clear();
    leaves_ready = false;
    anchor.Reset();
    scroller.Reset();
    tp.Reset();
    main.Reset();
    doc.Reset();
    root.Reset();
    publish_track();
    std::lock_guard<std::mutex> lock(smu);
    unsigned layout = st.layout + 1;
    st = State{};
    st.layout = layout;
  }

  size_t doc_text_len() {
    if (!tp) return 0;
    ComPtr<IUIAutomationTextRange> all;
    if (FAILED(tp->get_DocumentRange(&all)) || !all) return 0;
    return range_text(all.Get(), 4000).size();
  }

  // ------------------------------------------------ reading

  ComPtr<IUIAutomationCacheRequest> reading_request(bool raw) {
    ComPtr<IUIAutomationCacheRequest> cr;
    uia->CreateCacheRequest(&cr);
    for (PROPERTYID p : {UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId, UIA_ValueValuePropertyId,
                         UIA_IsOffscreenPropertyId, UIA_IsPasswordPropertyId, UIA_HeadingLevelPropertyId, UIA_RuntimeIdPropertyId})
      cr->AddProperty(p);
    cr->put_TreeScope(TreeScope_Subtree);
    if (raw) {
      ComPtr<IUIAutomationCondition> rawc;
      uia->get_RawViewCondition(&rawc);
      cr->put_TreeFilter(rawc.Get());
    }
    return cr;
  }

  static int heading_of(IUIAutomationElement* e) {
    VARIANT v;
    VariantInit(&v);
    int lvl = 0;
    if (SUCCEEDED(e->GetCachedPropertyValue(UIA_HeadingLevelPropertyId, &v)) && v.vt == VT_I4 && v.lVal >= HeadingLevel1 && v.lVal <= HeadingLevel9)
      lvl = int(v.lVal - HeadingLevel1 + 1);
    VariantClear(&v);
    return lvl;
  }

  // Text pieces in reading order from one cached copy of the tree (one round
  // trip to the app). A web page: its text leaves. An app: the text of its
  // labels, lists, rows and boxes; buttons and menus go to `controls`.
  void collect(IUIAutomationElement* e, const std::string& href, int depth, int heading, bool page) {
    // Into a child: a new box (a link is part of its sentence's box).
    auto into = [&](IUIAutomationElement* c, const std::string& link, int h, CONTROLTYPEID ct) {
      int ob = cur_box, oi = cur_item;
      if (ct != UIA_HyperlinkControlTypeId) cur_box = ++box_seq;
      if (ct == UIA_ListItemControlTypeId || ct == UIA_TreeItemControlTypeId || ct == UIA_DataItemControlTypeId) cur_item = cur_box;
      RECT cr{};
      c->get_CachedBoundingRectangle(&cr);
      anc.push_back({rid_of(c, true), (long long)(cr.right - cr.left) * (cr.bottom - cr.top)});
      collect(c, link, depth + 1, h, page);
      anc.pop_back();
      cur_box = ob;
      cur_item = oi;
    };
    // Who a piece is, and the small elements right around it (for hit tests).
    auto who = [&](Leaf& l, IUIAutomationElement* c) {
      l.rid = rid_of(c, true);
      for (size_t k = anc.size(); k > 0 && l.up.size() < 3; --k)
        if (anc[k - 1].second <= small_area && !anc[k - 1].first.empty()) l.up.push_back(anc[k - 1].first);
    };
    ComPtr<IUIAutomationElementArray> kids;
    if (depth > 120 || FAILED(e->GetCachedChildren(&kids)) || !kids) return;
    int n = 0;
    kids->get_Length(&n);
    for (int i = 0; i < n && seen_elements < 20000; ++i) {
      ComPtr<IUIAutomationElement> c;
      kids->GetElement(i, &c);
      ++seen_elements;
      CONTROLTYPEID ct = 0;
      c->get_CachedControlType(&ct);
      // A closed drop-down's choices are not on screen, but Firefox gives them
      // boxes below it as if they were: reading them put silk and reading
      // marks over empty space. Its chosen value is a control, not page text.
      if (ct == UIA_ComboBoxControlTypeId) continue;
      std::string link = href;
      if (ct == UIA_HyperlinkControlTypeId) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(c->GetCachedPropertyValue(UIA_ValueValuePropertyId, &v)) && v.vt == VT_BSTR) link = bstr_utf8(v.bstrVal);
        VariantClear(&v);
      }
      int h = heading_of(c.Get());
      if (!h) h = heading;
      ComPtr<IUIAutomationElementArray> gk;
      int gn = 0;
      if (SUCCEEDED(c->GetCachedChildren(&gk)) && gk) gk->get_Length(&gn);
      BSTR b = nullptr;
      c->get_CachedName(&b);
      std::wstring name = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
      SysFreeString(b);
      RECT r{};
      c->get_CachedBoundingRectangle(&r);
      // Hidden pieces sometimes report a box far off the screen.
      if (r.bottom - r.top > 20000 || r.right - r.left > 20000 || r.top < -20000) r = RECT{};

      if (!page) {
        // An app: its chrome tells what it is; its labels, rows and boxes are what it says.
        switch (ct) {
          case UIA_ButtonControlTypeId:
          case UIA_SplitButtonControlTypeId:
          case UIA_MenuItemControlTypeId:
          case UIA_TabItemControlTypeId:
          case UIA_CheckBoxControlTypeId:
          case UIA_RadioButtonControlTypeId:
            if (!name.empty() && controls.size() < 200) controls.push_back(utf8(name).substr(0, 60));
            if (gn > 0 && ct != UIA_ButtonControlTypeId) into(c.Get(), link, h, ct);
            continue;
          case UIA_TitleBarControlTypeId:
          case UIA_ScrollBarControlTypeId:
          case UIA_ThumbControlTypeId:
          case UIA_SeparatorControlTypeId:
            continue;
          case UIA_EditControlTypeId:
          case UIA_DocumentControlTypeId: {
            BOOL pw = FALSE;
            c->get_CachedIsPassword(&pw);
            if (pw) continue;  // never read a password box
            std::wstring text;
            ComPtr<IUIAutomationTextPattern> etp;
            if (SUCCEEDED(c->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&etp))) && etp) {
              ComPtr<IUIAutomationTextRange> all;
              if (SUCCEEDED(etp->get_DocumentRange(&all)) && all) text = wide(range_text(all.Get(), 60000));
            }
            if (text.empty()) {
              VARIANT v;
              VariantInit(&v);
              if (SUCCEEDED(c->GetCachedPropertyValue(UIA_ValueValuePropertyId, &v)) && v.vt == VT_BSTR) text.assign(v.bstrVal, SysStringLen(v.bstrVal));
              VariantClear(&v);
            }
            if (text.empty() && gn > 0) {
              into(c.Get(), link, h, ct);
              continue;
            }
            // A long text (an editor) is split into its lines so they read as paragraphs.
            // The box is the whole editor's, not a line's: those lines get none
            // (where a find is in them comes from the editor's text pattern).
            size_t at = 0, first = leaves.size();
            int lines = 0;
            while (at < text.size() && lines < 4000) {
              size_t nl = text.find_first_of(L"\r\n", at);
              std::wstring line = text.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
              if (!line.empty()) {
                Leaf l;
                l.el = c;
                l.text = line;
                l.r = r;
                l.href = link;
                l.box = cur_box;
                l.item = cur_item;
                who(l, c.Get());
                leaves.push_back(std::move(l));
                ++lines;
              }
              if (nl == std::wstring::npos) break;
              at = nl + 1;
            }
            if (lines > 1)
              for (size_t k = first; k < leaves.size(); ++k) leaves[k].r = RECT{};
            continue;
          }
          default:
            break;
        }
      }
      bool leafy = ct == UIA_TextControlTypeId || ct == UIA_HyperlinkControlTypeId ||
                   (!page && (ct == UIA_ListItemControlTypeId || ct == UIA_TreeItemControlTypeId || ct == UIA_DataItemControlTypeId ||
                              ct == UIA_HeaderItemControlTypeId || ct == UIA_StatusBarControlTypeId || ct == UIA_ToolTipControlTypeId));
      if (gn == 0 && leafy) {
        std::wstring text = name;
        // Firefox gives some paragraphs no name: their text is only in the text pattern.
        if (text.empty() && tp && page && text_lookups < 4000) {
          ++text_lookups;
          ComPtr<IUIAutomationTextRange> rr;
          if (SUCCEEDED(tp->RangeFromChild(c.Get(), &rr)) && rr) text = wide(range_text(rr.Get(), 20000));
        }
        if (!text.empty()) {
          Leaf l;
          l.el = c;
          l.text = text;
          l.r = r;
          l.href = link;
          l.heading = h;
          l.box = cur_box;
          l.item = cur_item;
          who(l, c.Get());
          l.own = !page && ct != UIA_TextControlTypeId && ct != UIA_HyperlinkControlTypeId;
          leaves.push_back(std::move(l));
        }
      } else if (gn > 0) {
        into(c.Get(), link, h, ct);
      } else if (!page && ct == UIA_ImageControlTypeId && !name.empty() && controls.size() < 200) {
        controls.push_back("picture: " + utf8(name).substr(0, 60));
      }
    }
  }

  // Paragraphs from geometry: apps and browsers disagree on what a paragraph
  // is, but a gap taller than the usual line gap is a break everywhere.
  void split_blocks_by_layout() {
    // One line's height: a whole paragraph can be one piece (Firefox gives a
    // plain paragraph as one text), so its height says nothing about the gap
    // between paragraphs. The short pieces tell.
    std::vector<long> hs;
    for (auto& l : leaves)
      if (l.r.bottom - l.r.top > 4) hs.push_back(l.r.bottom - l.r.top);
    std::sort(hs.begin(), hs.end());
    long line1 = hs.empty() ? 20 : std::clamp(hs[hs.size() / 5], 8L, 60L);
    // The gap between two lines of one paragraph, measured inside paragraphs
    // only (pieces of the same box on the next line); else a usual leading.
    std::vector<long> gaps;
    for (size_t i = 1; i < leaves.size(); ++i) {
      const RECT &a = leaves[i - 1].r, &b = leaves[i].r;
      long lh = b.bottom - b.top, g = b.top - a.bottom;
      if (leaves[i - 1].box == leaves[i].box && g >= -1 && lh > 0 && g <= lh && lh < 2 * line1) gaps.push_back(g);
    }
    std::sort(gaps.begin(), gaps.end());
    long line_gap = gaps.empty() ? line1 / 5 : gaps[gaps.size() / 2];
    int block = 0;
    long line_bottom = leaves.empty() ? 0 : leaves[0].r.bottom;
    for (size_t i = 0; i < leaves.size(); ++i) {
      Leaf& l = leaves[i];
      if (i > 0) {
        const RECT& a = leaves[i - 1].r;
        const RECT& b = l.r;
        bool same_el = leaves[i - 1].el.Get() == l.el.Get();  // an editor's lines
        bool a_none = a.right <= a.left, b_none = b.right <= b.left;
        bool new_line = b.top >= line_bottom - 2;
        bool gap = new_line && b.top - line_bottom > line_gap + std::max(4L, line1 * 30 / 100);
        // Text that goes back up, or sits beside a box of several lines: another
        // column, or a box floated beside the text (not the next word on a line).
        bool beside = (b.right <= a.left || b.left >= a.right) && b.top + line1 / 2 < a.bottom && a.bottom - a.top > line1 * 16 / 10;
        bool column = !a_none && !b_none && (b.bottom <= a.top || b.top + std::max(4L, line1 / 2) < a.top || beside);
        bool head = (l.heading != leaves[i - 1].heading);
        // Another list item (a reference, a search result) on a new line.
        bool item = l.item != leaves[i - 1].item && (new_line || column);
        if (same_el || gap || column || item || a_none != b_none || head || l.own || leaves[i - 1].own) ++block;
        if (new_line || column) line_bottom = b.bottom;
        else line_bottom = std::max(line_bottom, b.bottom);
      }
      l.block = block;
    }
    blocks.assign(size_t(block + 1), std::string());
    block_heading.assign(size_t(block + 1), 0);
    std::vector<std::wstring> w(size_t(block + 1));
    const Leaf* prev = nullptr;
    for (auto& l : leaves) {
      std::wstring& b = w[size_t(l.block)];
      // Separate pieces (tabs, a title and its links) get a space between them;
      // a link inside a sentence already has its spaces.
      if (!b.empty() && prev && prev->block == l.block && !l.text.empty() && !iswspace(b.back()) && !iswspace(l.text.front()) &&
          !iswpunct(l.text.front())) {
        bool other_line = l.r.top >= prev->r.bottom - 2;
        bool gap = l.r.left > prev->r.right + 2;
        if (other_line || gap) b += L' ';
      }
      b += l.text;
      prev = &l;
      if (l.heading) block_heading[size_t(l.block)] = l.heading;
    }
    for (size_t i = 0; i < w.size(); ++i) blocks[i] = squash(utf8(w[i]));
  }

  void load_leaves() {
    if (leaves_ready || (!doc && !root)) return;
    leaves_ready = true;
    leaves.clear();
    blocks.clear();
    controls.clear();
    flat.clear();
    flat_map.clear();
    seen_elements = 0;
    text_lookups = 0;
    bool page = doc != nullptr;
    {
      std::lock_guard<std::mutex> lock(smu);
      leaves_layout = st.layout;
    }
    leaves_at_ms = GetTickCount64();
    // Firefox hides the text inside many boxes from the control view; the raw
    // view has it, with names, and needs no slow per-paragraph lookups.
    ComPtr<IUIAutomationCacheRequest> cr = reading_request(page && app.engine == "firefox");
    ComPtr<IUIAutomationElement> cached;
    IUIAutomationElement* scope = main ? main.Get() : page ? doc.Get() : root.Get();
    if (FAILED(scope->BuildUpdatedCache(cr.Get(), &cached)) || !cached) return;
    box_seq = cur_box = cur_item = 0;
    anc.clear();
    hits_seen.clear();
    {
      RECT vr = view_rect();
      small_area = std::max(1LL, (long long)(vr.right - vr.left) * (vr.bottom - vr.top) / 4);
    }
    collect(cached.Get(), std::string(), 0, 0, page);
    split_blocks_by_layout();
    for (size_t i = 0; i < leaves.size(); ++i) alnum_append(leaves[i].text, int(i), flat, &flat_map);
    std::vector<long> hs;
    for (auto& l : leaves)
      if (l.r.bottom - l.r.top > 0 && l.r.bottom - l.r.top < 200) hs.push_back(l.r.bottom - l.r.top);
    std::sort(hs.begin(), hs.end());
    line_h = hs.empty() ? 0 : hs[hs.size() / 2];

    // Content-space boxes of the blocks, and an anchor for exact scrolling: a
    // line of ordinary text near the middle of the view. The scroll now comes
    // from the old anchor measured right now, so the new one carries on from it.
    RECT v = view_rect();
    double sc;
    {
      std::lock_guard<std::mutex> lock(smu);
      sc = st.scroll;
    }
    RECT oar{};
    if (anchor && SUCCEEDED(anchor->get_CurrentBoundingRectangle(&oar)) && oar.bottom > oar.top) sc = anchor_base - double(oar.top - v.top);
    block_geom.assign(blocks.size(), Box{});
    std::vector<float> right(blocks.size(), 0), bottom(blocks.size(), 0);
    long mid_lo = v.top + (v.bottom - v.top) / 4, mid_hi = v.bottom - (v.bottom - v.top) / 4;
    int best = -1;
    for (size_t i = 0; i < leaves.size(); ++i) {
      const RECT& r = leaves[i].r;
      if (r.right <= r.left) continue;
      size_t bi = size_t(leaves[i].block);
      Box& g = block_geom[bi];
      float x = float(r.left - v.left), y = float(double(r.top - v.top) + sc);
      if (g.w == 0 && g.h == 0) {
        g.x = x;
        g.y = y;
      }
      g.x = std::min(g.x, x);
      g.y = std::min(g.y, y);
      right[bi] = std::max(right[bi], float(r.right - v.left));
      bottom[bi] = std::max(bottom[bi], float(double(r.bottom - v.top) + sc));
      g.w = std::max(1.f, right[bi] - g.x);
      g.h = std::max(1.f, bottom[bi] - g.y);
      if (best < 0 && leaves[i].text.size() > (page ? 20u : 6u) && leaves[i].href.empty() && r.top >= mid_lo && r.top <= mid_hi) best = int(i);
    }
    if (best < 0)
      for (size_t i = 0; i < leaves.size() && best < 0; ++i)
        if (leaves[i].r.right > leaves[i].r.left && leaves[i].text.size() > (page ? 20u : 6u)) best = int(i);
    RECT nar{};
    if (best >= 0 && SUCCEEDED(leaves[size_t(best)].el->get_CurrentBoundingRectangle(&nar)) && nar.bottom > nar.top) {
      anchor = leaves[size_t(best)].el;
      anchor_base = double(nar.top - v.top) + sc;
    } else {
      anchor.Reset();
      // No anchor (a plain editor, a list that rebuilds itself): its scroller
      // still shows that the content moved.
      if (!scroller) scroller = find_scroller();
    }
    anchor_lost = false;
    publish_track();
    std::lock_guard<std::mutex> lock(smu);
    leaves_steady = st.moved_ms < leaves_at_ms && st.layout == leaves_layout;
  }

  // The content's own scroller: the page, else the biggest one in the window.
  ComPtr<IUIAutomationElement> find_scroller() {
    ComPtr<IUIAutomationElement> best_el;
    ComPtr<IUIAutomationScrollPattern> sp;
    if (doc && SUCCEEDED(doc->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp))) && sp) return doc;
    if (!root) return best_el;
    VARIANT v;
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> c;
    uia->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, v, &c);
    ComPtr<IUIAutomationElementArray> all;
    if (FAILED(root->FindAll(TreeScope_Descendants, c.Get(), &all)) || !all) return best_el;
    int n = 0;
    all->get_Length(&n);
    long best = 0;
    for (int i = 0; i < n; ++i) {
      ComPtr<IUIAutomationElement> e;
      all->GetElement(i, &e);
      RECT r{};
      e->get_CurrentBoundingRectangle(&r);
      long a = (r.right - r.left) * (r.bottom - r.top);
      ComPtr<IUIAutomationScrollPattern> p;
      BOOL vs = FALSE;
      if (a > best && SUCCEEDED(e->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&p))) && p && SUCCEEDED(p->get_CurrentVerticallyScrollable(&vs)) && vs) {
        best = a;
        best_el = e;
      }
    }
    return best_el;
  }

  RECT view_rect() {
    RECT r{};
    if (doc && SUCCEEDED(doc->get_CurrentBoundingRectangle(&r)) && r.right > r.left) return r;
    if (hwnd) {
      GetClientRect(hwnd, &r);
      POINT tl{r.left, r.top}, br{r.right, r.bottom};
      ClientToScreen(hwnd, &tl);
      ClientToScreen(hwnd, &br);
      return RECT{tl.x, tl.y, br.x, br.y};
    }
    return r;
  }

  Look look_now() {
    Look out;
    if (!hwnd || !IsWindow(hwnd)) {
      out.error = "the window is gone";
      return out;
    }
    app = identify(hwnd);
    out.app = app;
    // A page that changed under us is read fresh.
    if (doc) {
      RECT dr{};
      if (FAILED(doc->get_CurrentBoundingRectangle(&dr)) || dr.right <= dr.left) {
        ComPtr<IUIAutomationElement> dd = find_doc();
        if (dd) bind_doc(dd);
      }
    } else if (!app.engine.empty()) {
      ComPtr<IUIAutomationElement> dd = find_doc();
      if (dd) bind_doc(dd);
    }
    leaves_ready = false;
    load_leaves();
    out.how = doc ? "page" : "app";
    out.url = doc ? (doc_url().empty() ? app_url() : doc_url()) : app_url();
    {
      std::lock_guard<std::mutex> lock(smu);
      st.url = out.url;
    }
    url = out.url;
    wchar_t t[512] = {};
    GetWindowTextW(hwnd, t, 512);
    title = t;
    std::string tt = utf8(title);
    if (doc) {
      BSTR nm = nullptr;
      doc->get_CurrentName(&nm);
      std::string dn = squash(bstr_utf8(nm));
      SysFreeString(nm);
      if (!dn.empty()) tt = dn;
    }
    // Browsers append their own name to the title.
    for (const char* suf : {" \xE2\x80\x94 Mozilla Firefox", " - Mozilla Firefox", " - Brave", " - Google Chrome", " - Microsoft\xE2\x80\x8B Edge",
                            " - Microsoft Edge", " - Opera", " - Vivaldi"}) {
      size_t p = tt.rfind(suf);
      if (p != std::string::npos && p + strlen(suf) == tt.size()) {
        tt.resize(p);
        break;
      }
    }
    out.title = tt;
    VARIANT cul;
    VariantInit(&cul);
    if (doc && SUCCEEDED(doc->GetCurrentPropertyValue(UIA_CulturePropertyId, &cul)) && cul.vt == VT_I4 && cul.lVal) {
      wchar_t nm[16] = {};
      if (GetLocaleInfoW(MAKELCID(LCID(cul.lVal), SORT_DEFAULT), LOCALE_SISO639LANGNAME, nm, 16) > 0) out.lang = utf8(nm);
    }
    VariantClear(&cul);
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].empty()) continue;
      out.blocks.push_back(blocks[i]);
      out.spans.push_back(i < block_geom.size() ? block_geom[i] : Box{});
      out.heading.push_back(i < block_heading.size() ? block_heading[i] : 0);
    }
    // When the tree gave little, the text pattern may have it all (Chromium puts line breaks in).
    size_t chars = 0;
    for (auto& b : out.blocks) chars += b.size();
    if (doc && tp && chars < 400) {
      ComPtr<IUIAutomationTextRange> r;
      if (main) tp->RangeFromChild(main.Get(), &r);
      if (!r) tp->get_DocumentRange(&r);
      if (r) {
        std::string text = range_text(r.Get(), 200000);
        if (text.size() > chars * 3) {
          out.blocks.clear();
          out.spans.clear();
          out.heading.clear();
          size_t at = 0;
          while (at < text.size()) {
            size_t nl = text.find_first_of("\r\n", at);
            std::string line = squash(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
            if (!line.empty()) {
              out.blocks.push_back(line);
              out.spans.push_back(Box{});
              out.heading.push_back(0);
            }
            if (nl == std::string::npos) break;
            at = nl + 1;
          }
          // Where the lines on screen are: the text pattern finds them (a plain editor has no pieces).
          RECT v = view_rect();
          double sc;
          {
            std::lock_guard<std::mutex> lock(smu);
            sc = st.scroll;
          }
          ComPtr<IUIAutomationTextRange> all;
          if (leaves.empty() && SUCCEEDED(tp->get_DocumentRange(&all)) && all)
            for (size_t i = 0; i < out.blocks.size() && i < 60; ++i) {
              std::wstring head = wide(out.blocks[i].substr(0, 60));
              BSTR bs = SysAllocString(head.c_str());
              ComPtr<IUIAutomationTextRange> hit;
              if (SUCCEEDED(all->FindText(bs, FALSE, FALSE, &hit)) && hit) {
                auto rs = range_rects(hit.Get());
                if (!rs.empty()) out.spans[i] = Box{rs[0].x - float(v.left), float(double(rs[0].y - float(v.top)) + sc), rs[0].w, rs[0].h};
              }
              SysFreeString(bs);
            }
        }
      }
    }
    // Links: neighbouring pieces of one link are one link.
    std::string cur_href, cur_text;
    long cur_top = 0;
    auto flush = [&] {
      if (!cur_href.empty()) out.links.push_back({cur_href, squash(cur_text)});
      cur_href.clear();
      cur_text.clear();
    };
    for (auto& l : leaves) {
      if (l.href != cur_href) flush();
      if (!l.href.empty()) {
        if (!cur_text.empty() && std::abs(l.r.top - cur_top) > (l.r.bottom - l.r.top) / 2) cur_text += " | ";
        cur_href = l.href;
        cur_text += utf8(l.text);
        cur_top = l.r.top;
      }
    }
    flush();
    for (auto& b : out.blocks) {
      if (out.text.size() > 12000) break;
      out.text += b + "\n";
    }
    out.controls = controls;
    RECT v = view_rect();
    out.view = to_box(v);
    out.elements = seen_elements;
    {
      std::lock_guard<std::mutex> lock(smu);
      out.at_ms = leaves_at_ms;
      out.layout = leaves_layout;
      out.steady = st.moved_ms < leaves_at_ms && st.layout == leaves_layout;
    }
    out.ok = !out.blocks.empty() || !out.controls.empty();
    if (!out.ok) out.error = "nothing readable in this window";
    return out;
  }

  // ------------------------------------------------ where text is

  ComPtr<IUIAutomationTextRange> range_for(size_t s, size_t e) {
    ComPtr<IUIAutomationTextRange> r;
    if (!tp || s >= e || e > flat_map.size()) return r;
    auto [ls, os] = flat_map[s];
    auto [le, oe] = flat_map[e - 1];
    int end_off = oe + 1;
    if (FAILED(tp->RangeFromChild(leaves[size_t(ls)].el.Get(), &r)) || !r) return nullptr;
    int moved = 0;
    r->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, os, &moved);
    if (le == ls) {
      int len = int(leaves[size_t(ls)].text.size());
      r->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, -(len - end_off), &moved);
    } else {
      ComPtr<IUIAutomationTextRange> r2;
      if (SUCCEEDED(tp->RangeFromChild(leaves[size_t(le)].el.Get(), &r2)) && r2) {
        int len = int(leaves[size_t(le)].text.size());
        r2->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, -(len - end_off), &moved);
        r->MoveEndpointByRange(TextPatternRangeEndpoint_End, r2.Get(), TextPatternRangeEndpoint_End);
      }
    }
    return r;
  }

  std::vector<Box> boxes_for(const std::string& text, size_t anchor_at, int* leaf_out = nullptr) {
    std::vector<Box> out;
    if (leaf_out) *leaf_out = -1;
    std::wstring q = alnum(text);
    if (q.size() < 2) return out;
    size_t best = std::wstring::npos, best_d = SIZE_MAX;
    for (size_t p = flat.find(q); p != std::wstring::npos; p = flat.find(q, p + 1)) {
      size_t dd = p >= anchor_at ? p - anchor_at : (anchor_at - p) * 2;
      if (dd < best_d) {
        best_d = dd;
        best = p;
      }
      if (dd == 0) break;
    }
    // An editor's own text pattern finds the exact words (Notepad, an app's text box).
    auto search = [&](IUIAutomationTextPattern* p) {
      ComPtr<IUIAutomationTextRange> all, hit;
      BSTR bs = SysAllocString(wide(text).c_str());
      if (SUCCEEDED(p->get_DocumentRange(&all)) && all && SUCCEEDED(all->FindText(bs, FALSE, TRUE, &hit)) && hit) out = range_rects(hit.Get());
      SysFreeString(bs);
    };
    if (best == std::wstring::npos) {
      // A plain editor (Notepad, Word): no pieces, but the text pattern can search.
      if (leaves.empty() && tp) search(tp.Get());
    } else {
      size_t s = best, e = best + q.size();
      if (leaf_out) *leaf_out = flat_map[s].first;
      // A web page: the exact words.
      if (tp && doc) {
        ComPtr<IUIAutomationTextRange> r = range_for(s, e);
        if (r) out = range_rects(r.Get());
      }
      // An app's text box: its own text pattern.
      if (out.empty() && !doc) {
        const Leaf& l = leaves[size_t(flat_map[s].first)];
        ComPtr<IUIAutomationTextPattern> etp;
        if (SUCCEEDED(l.el->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&etp))) && etp) search(etp.Get());
      }
      // Else the boxes of the pieces it is in (a row, a label).
      if (out.empty()) {
        int a = flat_map[s].first, b = flat_map[e - 1].first;
        for (int i = a; i <= b && i < int(leaves.size()); ++i)
          if (leaves[size_t(i)].r.right > leaves[size_t(i)].r.left) out.push_back(to_box(leaves[size_t(i)].r));
      }
    }
    // Only what is inside the view, and only boxes that can hold this text:
    // browsers sometimes give a line far off as a page-wide bar.
    RECT v = view_rect();
    float lh = line_h > 0 ? float(line_h) : 22.f * float(GetDpiForWindow(hwnd)) / 96.f;
    std::vector<Box> keep;
    for (auto& bx : out) {
      if (!(bx.y + bx.h > v.top && bx.y < v.bottom && bx.x + bx.w > v.left && bx.x < v.right)) continue;
      if (bx.h > std::max(lh * 3.2f, 40.f)) continue;
      if (bx.w > float(text.size()) * bx.h * 1.25f + 24.f) continue;
      keep.push_back(bx);
    }
    return keep;
  }

  // Is the piece really what shows at this box? Windows says what is at the
  // box's middle (as Playwright and browser-use ask the page with
  // elementFromPoint before a click): the piece itself, a small element right
  // around it (its link, its paragraph), or something inside it. Anything
  // else (a page's own pop-up on top, empty space where hidden text claims to
  // be) means the find is not showing there. Unknown answers keep it.
  bool shows_at(int leaf, const Box& b) {
    if (leaf < 0 || leaf >= int(leaves.size()) || leaves[size_t(leaf)].rid.empty() || b.empty()) return true;
    const Leaf& l = leaves[size_t(leaf)];
    POINT pt{LONG(b.x + b.w / 2), LONG(b.y + b.h / 2)};
    ComPtr<IUIAutomationElement> h;
    if (FAILED(uia->ElementFromPoint(pt, &h)) || !h) return true;
    int pid = 0;
    if (SUCCEEDED(h->get_CurrentProcessId(&pid)) && DWORD(pid) == GetCurrentProcessId()) return true;  // our own layer: no answer
    std::vector<int> hr = rid_of(h.Get(), false);
    if (hr.empty()) return true;
    if (hr == l.rid) return true;
    for (auto& a : l.up)
      if (a == hr) return true;
    if (!walker) return false;
    ComPtr<IUIAutomationTreeWalker> raw;
    uia->get_RawViewWalker(&raw);
    ComPtr<IUIAutomationElement> p;
    if (raw && SUCCEEDED(raw->GetParentElement(h.Get(), &p)) && p && rid_of(p.Get(), false) == l.rid) return true;  // its own text
    return false;
  }

  std::vector<Box> find_text_now(const std::string& text, const std::string& near_block, int* leaf_out = nullptr) {
    if (!leaves_ready) load_leaves();
    size_t at = 0;
    if (!near_block.empty()) {
      std::wstring b = alnum(near_block);
      if (b.size() >= 12) {
        size_t p = flat.find(b.substr(0, std::min<size_t>(40, b.size())));
        if (p != std::wstring::npos) at = p;
      }
    }
    return boxes_for(text, at, leaf_out);
  }

  // Brings text on screen, and says so only when it really is: the text
  // range scrolls itself into view, else the piece it is in does, else the
  // page's scroller moves by the distance to it (some browsers say yes to the
  // first two and do nothing). `para` picks the right one of several.
  bool scroll_to_text_now(const std::string& text, const std::string& para) {
    if (!leaves_ready) load_leaves();
    std::wstring q = alnum(text);
    if (q.size() < 2) return false;
    size_t from = 0;
    if (std::wstring b = alnum(para); b.size() >= 12) {
      size_t p = flat.find(b.substr(0, std::min<size_t>(40, b.size())));
      if (p != std::wstring::npos) from = p;
    }
    size_t p = flat.find(q, from);
    if (p == std::wstring::npos) p = flat.find(q);
    if (p == std::wstring::npos) return false;
    auto on_screen = [&] {
      Sleep(150);  // smooth scrolling takes a moment
      return !boxes_for(text, p).empty();
    };
    if (tp && doc) {
      ComPtr<IUIAutomationTextRange> r = range_for(p, p + q.size());
      if (r && SUCCEEDED(r->ScrollIntoView(TRUE)) && on_screen()) return true;
    }
    const Leaf& leaf = leaves[size_t(flat_map[p].first)];
    ComPtr<IUIAutomationScrollItemPattern> si;
    if (SUCCEEDED(leaf.el->GetCurrentPatternAs(UIA_ScrollItemPatternId, IID_PPV_ARGS(&si))) && si && SUCCEEDED(si->ScrollIntoView()) && on_screen())
      return true;
    // The scroller, by the distance: the piece a third of the way down the screen.
    RECT er{}, v = view_rect();
    ComPtr<IUIAutomationElement> sc = find_scroller();
    ComPtr<IUIAutomationScrollPattern> sp;
    double pct = -1, size = 0;
    if (sc && SUCCEEDED(leaf.el->get_CurrentBoundingRectangle(&er)) && er.bottom > er.top &&
        SUCCEEDED(sc->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp))) && sp &&
        SUCCEEDED(sp->get_CurrentVerticalScrollPercent(&pct)) && SUCCEEDED(sp->get_CurrentVerticalViewSize(&size)) && pct >= 0 && size > 0 &&
        size < 100) {
      double view_h = double(v.bottom - v.top), range = view_h * 100.0 / size - view_h;
      double target = std::clamp(pct / 100.0 * range + double(er.top - v.top) - view_h / 3.0, 0.0, range);
      if (range > 0 && SUCCEEDED(sp->SetScrollPercent(UIA_ScrollPatternNoScroll, target / range * 100.0)) && on_screen()) return true;
    }
    return false;
  }

  WordHit word_at_now(POINT pt) {
    WordHit w;
    if (tp && doc) {
      ComPtr<IUIAutomationTextRange> r;
      if (FAILED(tp->RangeFromPoint(pt, &r)) || !r) return w;
      r->ExpandToEnclosingUnit(TextUnit_Word);
      auto rects = range_rects(r.Get());
      if (rects.empty()) return w;
      w.word = rects[0];
      for (auto& rc : rects)
        if (rc.contains(float(pt.x), float(pt.y))) {
          w.word = rc;
          break;
        }
      if (w.word.w > 600 || w.word.h > 120) return w;  // an image or a whole block, not a word
      w.text = squash(range_text(r.Get(), 48));
      if (w.text.empty()) return w;
      w.ok = true;
      ComPtr<IUIAutomationTextRange3> r3;
      ComPtr<IUIAutomationElement> enc;
      ComPtr<IUIAutomationCacheRequest> cr;
      uia->CreateCacheRequest(&cr);
      cr->AddProperty(UIA_ControlTypePropertyId);
      cr->AddProperty(UIA_BoundingRectanglePropertyId);
      cr->AddProperty(UIA_NamePropertyId);
      if (SUCCEEDED(r.As(&r3)) && SUCCEEDED(r3->GetEnclosingElementBuildCache(cr.Get(), &enc)) && enc) {
        for (int up = 0; up < 2 && enc; ++up) {
          CONTROLTYPEID ct = 0;
          enc->get_CachedControlType(&ct);
          if (ct == UIA_HyperlinkControlTypeId) {
            RECT lr{};
            enc->get_CachedBoundingRectangle(&lr);
            w.link = true;
            w.link_rect = to_box(lr);
            BSTR nm = nullptr;
            enc->get_CachedName(&nm);
            w.link_text = squash(bstr_utf8(nm));
            SysFreeString(nm);
            break;
          }
          if (ct != UIA_TextControlTypeId) break;
          ComPtr<IUIAutomationElement> p;
          walker->GetParentElementBuildCache(enc.Get(), cr.Get(), &p);
          enc = p;
        }
      }
      return w;
    }
    // An app: the label or row under the foot.
    if (!leaves_ready) return w;
    for (auto& l : leaves) {
      if (l.r.right <= l.r.left || l.r.bottom - l.r.top > 80) continue;
      if (pt.x >= l.r.left && pt.x < l.r.right && pt.y >= l.r.top && pt.y < l.r.bottom) {
        w.ok = true;
        w.word = to_box(l.r);
        w.text = utf8(l.text).substr(0, 48);
        w.link = !l.href.empty();
        if (w.link) {
          w.link_rect = w.word;
          w.link_text = w.text;
        }
        break;
      }
    }
    return w;
  }

  // ------------------------------------------------ tasks

  static std::string kind_of(CONTROLTYPEID ct) {
    switch (ct) {
      case UIA_HyperlinkControlTypeId: return "link";
      case UIA_EditControlTypeId: return "field";
      case UIA_ComboBoxControlTypeId: return "list";
      case UIA_CheckBoxControlTypeId:
      case UIA_RadioButtonControlTypeId: return "checkbox";
      case UIA_TabItemControlTypeId: return "tab";
      case UIA_MenuItemControlTypeId: return "menu item";
      case UIA_ListItemControlTypeId:
      case UIA_TreeItemControlTypeId:
      case UIA_DataItemControlTypeId: return "item";
      case UIA_SliderControlTypeId:
      case UIA_SpinnerControlTypeId: return "slider";
      default: return "button";
    }
  }

  // What can be clicked or typed into, by Windows-MCP's rules: enabled,
  // visible (an off-screen box still counts), a real control, and of a kind
  // you act on. Each keeps the words of the row it sits in.
  void gather(IUIAutomationElement* e, int depth, const std::string& row, RECT v, std::set<std::vector<int>>& ancestors, bool in_search,
              int dialog) {
    ComPtr<IUIAutomationElementArray> kids;
    if (depth > 120 || held.size() >= 400 || FAILED(e->GetCachedChildren(&kids)) || !kids) return;
    int n = 0;
    kids->get_Length(&n);
    for (int i = 0; i < n && held.size() < 400; ++i) {
      ComPtr<IUIAutomationElement> c;
      kids->GetElement(i, &c);
      // A provider can hand back an ancestor as its own child: cut the loop.
      std::vector<int> id;
      VARIANT rv;
      VariantInit(&rv);
      if (SUCCEEDED(c->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &rv)) && (rv.vt & VT_ARRAY) && rv.parray) {
        LONG lo = 0, hi = -1;
        SafeArrayGetLBound(rv.parray, 1, &lo);
        SafeArrayGetUBound(rv.parray, 1, &hi);
        for (LONG k = lo; k <= hi && k - lo < 16; ++k) {
          int x = 0;
          SafeArrayGetElement(rv.parray, &k, &x);
          id.push_back(x);
        }
      }
      VariantClear(&rv);
      if (!id.empty() && ancestors.count(id)) continue;
      if (!id.empty()) ancestors.insert(id);
      CONTROLTYPEID ct = 0;
      c->get_CachedControlType(&ct);
      // Inside the page's search area (a form marked as search): its box is a search box, whatever its label says.
      VARIANT lmv;
      VariantInit(&lmv);
      bool search_here = in_search;
      if (SUCCEEDED(c->GetCachedPropertyValue(UIA_LandmarkTypePropertyId, &lmv)) && lmv.vt == VT_I4 && lmv.lVal == UIA_SearchLandmarkTypeId)
        search_here = true;
      VariantClear(&lmv);
      BSTR b = nullptr;
      c->get_CachedName(&b);
      std::string name = squash(bstr_utf8(b));
      SysFreeString(b);
      RECT r{};
      c->get_CachedBoundingRectangle(&r);
      // A pop-up (role dialog / alert dialog): what is inside it is marked, and its words kept.
      int in_dialog = dialog;
      {
        VARIANT dv;
        VariantInit(&dv);
        bool is_dialog = SUCCEEDED(c->GetCachedPropertyValue(UIA_IsDialogPropertyId, &dv)) && dv.vt == VT_BOOL && dv.boolVal == VARIANT_TRUE;
        VariantClear(&dv);
        if (!is_dialog && SUCCEEDED(c->GetCachedPropertyValue(UIA_LocalizedControlTypePropertyId, &dv)) && dv.vt == VT_BSTR)
          is_dialog = lower_ascii(bstr_utf8(dv.bstrVal)).find("dialog") != std::string::npos;
        VariantClear(&dv);
        if (is_dialog && dialog < 0 && r.right > r.left && r.bottom > r.top && dialogs_seen.size() < 8) {
          dialogs_seen.push_back(Dialog{name.substr(0, 120), std::string(), to_box(r)});
          in_dialog = int(dialogs_seen.size()) - 1;
        } else if (in_dialog >= 0 && !name.empty() && (ct == UIA_TextControlTypeId || ct == UIA_HyperlinkControlTypeId) &&
                   dialogs_seen[size_t(in_dialog)].text.size() < 800) {
          dialogs_seen[size_t(in_dialog)].text += name.substr(0, 300) + " ";
        }
      }
      BOOL off = FALSE, enabled = TRUE, pw = FALSE, focusable = FALSE;
      c->get_CachedIsOffscreen(&off);
      c->get_CachedIsEnabled(&enabled);
      c->get_CachedIsPassword(&pw);
      c->get_CachedIsKeyboardFocusable(&focusable);
      bool area = r.right > r.left && r.bottom > r.top;
      static const std::set<CONTROLTYPEID> acts = {
          UIA_ButtonControlTypeId, UIA_SplitButtonControlTypeId, UIA_HyperlinkControlTypeId, UIA_EditControlTypeId, UIA_ComboBoxControlTypeId,
          UIA_CheckBoxControlTypeId, UIA_RadioButtonControlTypeId, UIA_TabItemControlTypeId, UIA_MenuItemControlTypeId,
          UIA_ListItemControlTypeId, UIA_TreeItemControlTypeId, UIA_DataItemControlTypeId, UIA_SliderControlTypeId, UIA_SpinnerControlTypeId};
      // An app's text area (Notepad, a chat's message box) is a box to type in; a browser's page is not.
      bool text_area = ct == UIA_DocumentControlTypeId && focusable && app.engine.empty();
      bool actable = acts.count(ct) || (ct == UIA_ImageControlTypeId && focusable) || text_area;
      bool visible = area && (!off || ct == UIA_EditControlTypeId);
      if (actable && enabled && visible) {
        Held hd;
        hd.el = c;
        hd.ct = ct;
        Item& it = hd.item;
        it.i = int(held.size());
        it.kind = text_area ? "field" : kind_of(ct);
        it.label = name.size() > 80 ? name.substr(0, 79) + "\xE2\x80\xA6" : name;
        VARIANT vv;
        VariantInit(&vv);
        std::string val;
        if (SUCCEEDED(c->GetCachedPropertyValue(UIA_ValueValuePropertyId, &vv)) && vv.vt == VT_BSTR) val = squash(bstr_utf8(vv.bstrVal));
        VariantClear(&vv);
        if (ct == UIA_HyperlinkControlTypeId) it.href = val.substr(0, 90);
        it.secret = pw || std::regex_search(name, kSecretRx);
        if ((ct == UIA_EditControlTypeId || ct == UIA_ComboBoxControlTypeId || text_area) && !it.secret) it.value = val.substr(0, 60);
        BSTR aid = nullptr;
        c->get_CachedAutomationId(&aid);
        std::string auto_id = lower_ascii(bstr_utf8(aid));
        SysFreeString(aid);
        if (std::regex_search(auto_id, kSecretRx)) it.secret = true;
        std::string ln = lower_ascii(name);
        it.search = ct == UIA_EditControlTypeId && (search_here || ln.find("search") != std::string::npos ||
                                                    auto_id.find("search") != std::string::npos || auto_id == "q" ||
                                                    ln.find("address") != std::string::npos);
        if (it.label.empty() && !auto_id.empty() && ct != UIA_EditControlTypeId) it.label = auto_id;
        if (row.size() > it.label.size() + 4 && row.size() < 220 && (it.label.size() < 12)) it.row = row.substr(0, 140);
        it.r = to_box(r);
        it.dialog = in_dialog;
        it.in_view = !off && r.bottom > v.top && r.top < v.bottom && r.right > v.left && r.left < v.right;
        if (!it.label.empty() || it.kind == "field" || it.kind == "list") held.push_back(std::move(hd));
      }
      // The words of a row or card, for the buttons inside it.
      std::string sub = row;
      if ((ct == UIA_ListItemControlTypeId || ct == UIA_DataItemControlTypeId || ct == UIA_GroupControlTypeId || ct == UIA_TreeItemControlTypeId) &&
          name.size() > 3 && name.size() < 220)
        sub = name;
      gather(c.Get(), depth + 1, sub, v, ancestors, search_here, in_dialog);
      if (!id.empty()) ancestors.erase(id);
    }
  }

  std::vector<Item> items_now(int max) {
    held.clear();
    dialogs_seen.clear();
    std::vector<Item> out;
    if (!root) return out;
    ComPtr<IUIAutomationCacheRequest> cr;
    uia->CreateCacheRequest(&cr);
    for (PROPERTYID p : {UIA_NamePropertyId, UIA_ControlTypePropertyId, UIA_BoundingRectanglePropertyId, UIA_ValueValuePropertyId,
                         UIA_IsOffscreenPropertyId, UIA_IsEnabledPropertyId, UIA_IsPasswordPropertyId, UIA_IsKeyboardFocusablePropertyId,
                         UIA_AutomationIdPropertyId, UIA_RuntimeIdPropertyId, UIA_LandmarkTypePropertyId, UIA_IsDialogPropertyId,
                         UIA_LocalizedControlTypePropertyId})
      cr->AddProperty(p);
    cr->put_TreeScope(TreeScope_Subtree);
    // The whole window: a page's own buttons, and the app's (the address bar,
    // tabs, a chat's send box). Firefox's page needs the raw view.
    if (doc && app.engine == "firefox") {
      ComPtr<IUIAutomationCondition> rawc;
      uia->get_RawViewCondition(&rawc);
      cr->put_TreeFilter(rawc.Get());
    }
    ComPtr<IUIAutomationElement> cached;
    if (FAILED(root->BuildUpdatedCache(cr.Get(), &cached)) || !cached) return out;
    RECT v{};
    GetWindowRect(hwnd, &v);
    std::set<std::vector<int>> anc;
    gather(cached.Get(), 0, std::string(), v, anc, false, -1);
    // On screen first, then the rest in reading order.
    std::stable_sort(held.begin(), held.end(), [](const Held& a, const Held& b) { return a.item.in_view > b.item.in_view; });
    if (int(held.size()) > max) held.resize(size_t(max));
    for (size_t i = 0; i < held.size(); ++i) {
      held[i].item.i = int(i);
      out.push_back(held[i].item);
    }
    return out;
  }

  bool in_front() {
    HWND fg = GetForegroundWindow();
    return hwnd && fg && (fg == hwnd || GetAncestor(fg, GA_ROOTOWNER) == hwnd);
  }

  static void send_keys(std::initializer_list<std::pair<WORD, bool>> keys) {
    std::vector<INPUT> in;
    for (auto [vk, up] : keys) {
      INPUT x{};
      x.type = INPUT_KEYBOARD;
      x.ki.wVk = vk;
      x.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
      in.push_back(x);
    }
    SendInput(UINT(in.size()), in.data(), sizeof(INPUT));
  }

  static void type_unicode(const std::wstring& s) {
    for (wchar_t ch : s) {
      INPUT in[2] = {};
      in[0].type = in[1].type = INPUT_KEYBOARD;
      in[0].ki.wScan = in[1].ki.wScan = ch;
      in[0].ki.dwFlags = KEYEVENTF_UNICODE;
      in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      SendInput(2, in, sizeof(INPUT));
      Sleep(12);
    }
  }

  bool click_now(int i, std::string* note) {
    if (i < 0 || i >= int(held.size())) {
      if (note) *note = "that thing is gone from the window.";
      return false;
    }
    IUIAutomationElement* e = held[size_t(i)].el.Get();
    ComPtr<IUIAutomationInvokePattern> inv;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv && SUCCEEDED(inv->Invoke())) return true;
    ComPtr<IUIAutomationTogglePattern> tg;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&tg))) && tg && SUCCEEDED(tg->Toggle())) return true;
    ComPtr<IUIAutomationSelectionItemPattern> sel;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sel))) && sel && SUCCEEDED(sel->Select())) return true;
    ComPtr<IUIAutomationExpandCollapsePattern> ec;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&ec))) && ec && SUCCEEDED(ec->Expand())) return true;
    ComPtr<IUIAutomationLegacyIAccessiblePattern> acc;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&acc))) && acc && SUCCEEDED(acc->DoDefaultAction())) return true;
    if (note) *note = "it can't be clicked from here.";
    return false;
  }

  bool type_now(int i, const std::string& text, bool enter, std::string* note) {
    if (i < 0 || i >= int(held.size())) {
      if (note) *note = "that box is gone from the window.";
      return false;
    }
    Held& h = held[size_t(i)];
    if (h.item.secret) {
      if (note) *note = "that is a password or payment box: the spider never types there.";
      return false;
    }
    IUIAutomationElement* e = h.el.Get();
    std::wstring w = wide(text);
    // In front: focus it and type like a person, so web pages see real key presses.
    if (in_front()) {
      e->SetFocus();
      Sleep(80);
      BOOL focused = FALSE;
      e->get_CurrentHasKeyboardFocus(&focused);
      if (focused) {
        send_keys({{VK_CONTROL, false}, {'A', false}, {'A', true}, {VK_CONTROL, true}});
        Sleep(30);
        type_unicode(w);
        if (enter) {
          Sleep(60);
          // Enter goes only to the box that was typed into.
          e->get_CurrentHasKeyboardFocus(&focused);
          if (focused) send_keys({{VK_RETURN, false}, {VK_RETURN, true}});
        }
        return true;
      }
    }
    ComPtr<IUIAutomationValuePattern> vp;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&vp))) && vp) {
      BSTR bs = SysAllocString(w.c_str());
      HRESULT hr = vp->SetValue(bs);
      SysFreeString(bs);
      if (SUCCEEDED(hr)) {
        if (enter && in_front()) {
          e->SetFocus();
          send_keys({{VK_RETURN, false}, {VK_RETURN, true}});
        }
        return true;
      }
    }
    if (note) *note = "the spider can't type there while the window is in the back.";
    return false;
  }

  bool show_now(int i) {
    if (i < 0 || i >= int(held.size())) return false;
    ComPtr<IUIAutomationScrollItemPattern> si;
    return SUCCEEDED(held[size_t(i)].el->GetCurrentPatternAs(UIA_ScrollItemPatternId, IID_PPV_ARGS(&si))) && si && SUCCEEDED(si->ScrollIntoView());
  }

  Box item_box_now(int i) {
    if (i < 0 || i >= int(held.size())) return {};
    RECT r{};
    held[size_t(i)].el->get_CurrentBoundingRectangle(&r);
    return to_box(r);
  }

  bool scroll_now(const std::string& dir) {
    ComPtr<IUIAutomationScrollPattern> sp;
    if (ComPtr<IUIAutomationElement> e = find_scroller()) e->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp));
    bool ok = false;
    if (sp) {
      if (dir == "top") ok = SUCCEEDED(sp->SetScrollPercent(UIA_ScrollPatternNoScroll, 0));
      else if (dir == "bottom") ok = SUCCEEDED(sp->SetScrollPercent(UIA_ScrollPatternNoScroll, 100));
      else ok = SUCCEEDED(sp->Scroll(ScrollAmount_NoAmount, dir == "up" ? ScrollAmount_LargeDecrement : ScrollAmount_LargeIncrement));
    }
    if (!ok && in_front()) {
      WORD vk = dir == "top" ? VK_HOME : dir == "bottom" ? VK_END : dir == "up" ? VK_PRIOR : VK_NEXT;
      send_keys({{vk, false}, {vk, true}});
      ok = true;
    }
    return ok;
  }

  bool press_now(const std::string& key) {
    static const std::map<std::string, WORD> keys = {
        {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},     {"ArrowUp", VK_UP},   {"ArrowDown", VK_DOWN}, {"ArrowLeft", VK_LEFT},
        {"ArrowRight", VK_RIGHT}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"Home", VK_HOME},   {"End", VK_END}, {" ", VK_SPACE}, {"Enter", VK_RETURN}};
    auto it = keys.find(key);
    if (it == keys.end() || !in_front()) return false;
    send_keys({{it->second, false}, {it->second, true}});
    return true;
  }

  // ------------------------------------------------ tracking

  void tick() {
    if (!IsWindow(hwnd)) {
      detach_now();
      return;
    }
    // Where things are is the tracking thread's job; this watches for a new
    // tab, page or file. Nothing to watch while the window is off screen.
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || cloaked) return;
    // The tracker lost its anchor (the page rebuilt that part): read again for a new one.
    if (anchor_lost.exchange(false)) {
      anchor.Reset();
      std::lock_guard<std::mutex> lock(smu);
      st.gen++;
    }
    // A new title usually means another tab, page or file.
    wchar_t t[512] = {};
    GetWindowTextW(hwnd, t, 512);
    RECT dr{};
    bool doc_ok = doc && SUCCEEDED(doc->get_CurrentBoundingRectangle(&dr)) && dr.right > dr.left;
    if (title != t || (doc && !doc_ok)) {
      title = t;
      ComPtr<IUIAutomationElement> dd = !app.engine.empty() || doc ? find_doc() : nullptr;
      BOOL same = FALSE;
      if (dd && doc) uia->CompareElements(dd.Get(), doc.Get(), &same);
      if (!same || !dd) {
        if (dd) bind_doc(dd);
        else {
          doc.Reset();
          tp.Reset();
          main.Reset();
        }
        reset_page();
        url = doc ? (doc_url().empty() ? app_url() : doc_url()) : app_url();
      } else {
        std::lock_guard<std::mutex> lock(smu);
        st.gen++;  // same document, new title: still worth a fresh look
      }
      doc_ok = doc && SUCCEEDED(doc->get_CurrentBoundingRectangle(&dr)) && dr.right > dr.left;
    }
    ULONGLONG now = GetTickCount64();
    if (doc && now - last_url_check > 700) {
      last_url_check = now;
      std::string u = doc_url();
      auto strip = [](const std::string& s) { return s.substr(0, s.find('#')); };
      if (!u.empty() && strip(u) != strip(url)) {
        url = u;
        bind_doc(doc);
        reset_page();
      }
    }
    if (doc_ok) last_view = dr;
    std::lock_guard<std::mutex> lock(smu);
    st.attached = true;
    st.hwnd = hwnd;
    st.url = url;
  }
};

Eyes::Eyes() : d(std::make_unique<Impl>()) {}
Eyes::~Eyes() = default;

bool Eyes::attach(HWND h, int timeout_ms, std::string* error) {
  // Shared: a late answer after a timeout must not write into a finished call.
  auto err = std::make_shared<std::string>();
  bool ok = d->call([this, h, timeout_ms, err] { return d->attach_now(h, timeout_ms, err.get()); }, timeout_ms + 8000);
  if (error) *error = err->empty() && !ok ? "the window did not answer" : *err;
  return ok;
}

void Eyes::detach() {
  d->call([this] {
    d->detach_now();
    return true;
  }, 3000);
}

State Eyes::state() {
  std::lock_guard<std::mutex> lock(d->smu);
  return d->st;
}

Look Eyes::look() {
  Look l = d->call([this] { return d->look_now(); }, 20000);
  if (!l.ok && l.error.empty()) l.error = "the window did not answer in time";
  return l;
}

std::vector<Box> Eyes::find_text(const std::string& text, const std::string& near_block) {
  return d->call([this, text, near_block] { return d->find_text_now(text, near_block); }, 3000);
}

Spots Eyes::locate(const std::vector<Spot>& want) {
  return d->call([this, want] {
    Spots out;
    if (!d->leaves_ready) d->load_leaves();
    unsigned layout;
    {
      std::lock_guard<std::mutex> lock(d->smu);
      layout = d->st.layout;
    }
    ULONGLONG t0 = GetTickCount64();
    std::map<std::string, std::vector<Box>> screen;
    if (d->hits_seen.size() > 600) d->hits_seen.clear();
    for (auto& w : want) {
      int leaf = -1;
      auto b = d->find_text_now(w.text, w.para, &leaf);
      if (b.empty()) continue;
      // Really showing there, not under a pop-up or as hidden text? Asked
      // again every second, and whenever it is somewhere new on screen.
      std::string key = w.id + "|" + std::to_string(int(b[0].x)) + "," + std::to_string(int(b[0].y));
      ULONGLONG now = GetTickCount64();
      auto it = d->hits_seen.find(key);
      bool shows;
      if (it != d->hits_seen.end() && now - it->second.first < 1000) {
        shows = it->second.second;
      } else {
        shows = d->shows_at(leaf, b[0]);
        d->hits_seen[key] = {now, shows};
      }
      if (shows) screen[w.id] = std::move(b);
    }
    // Into content space with the geometry of this moment. When anything
    // moved meanwhile, the boxes may be off: the caller measures again.
    State s;
    {
      std::lock_guard<std::mutex> lock(d->smu);
      s = d->st;
    }
    out.at_ms = t0;
    out.layout = layout;
    out.steady = s.moved_ms < t0 && s.layout == layout;
    for (auto& [id, bs] : screen)
      for (auto& b : bs) out.boxes[id].push_back(Box{b.x - s.view.x, b.y - s.view.y + float(s.scroll), b.w, b.h});
    return out;
  }, 6000);
}

void Eyes::word_at(POINT pt, std::function<void(const WordHit&)> cb) {
  std::lock_guard<std::mutex> lock(d->qmu);
  d->words.push_back({pt, std::move(cb)});
}

bool Eyes::scroll_to_text(const std::string& text, const std::string& para) {
  return d->call([this, text, para] { return d->scroll_to_text_now(text, para); }, 5000);
}

std::vector<Item> Eyes::items(int max) {
  return d->call([this, max] { return d->items_now(max); }, 15000);
}

std::vector<Dialog> Eyes::dialogs() {
  return d->call([this] { return d->dialogs_seen; }, 3000);
}

bool Eyes::show_item(int i) {
  return d->call([this, i] { return d->show_now(i); }, 4000);
}

Box Eyes::item_box(int i) {
  return d->call([this, i] { return d->item_box_now(i); }, 3000);
}

bool Eyes::click_item(int i, std::string* note) {
  auto n = std::make_shared<std::string>();
  bool ok = d->call([this, i, n] { return d->click_now(i, n.get()); }, 6000);
  if (note) *note = ok ? std::string() : (n->empty() ? "the window did not answer." : *n);
  return ok;
}

bool Eyes::type_item(int i, const std::string& text, bool enter, std::string* note) {
  auto n = std::make_shared<std::string>();
  bool ok = d->call([this, i, text, enter, n] { return d->type_now(i, text, enter, n.get()); }, 15000);
  if (note) *note = ok ? std::string() : (n->empty() ? "the window did not answer." : *n);
  return ok;
}

bool Eyes::scroll(const std::string& dir) {
  return d->call([this, dir] { return d->scroll_now(dir); }, 5000);
}

bool Eyes::press(const std::string& key) {
  return d->call([this, key] { return d->press_now(key); }, 3000);
}

}  // namespace sp::eyes
