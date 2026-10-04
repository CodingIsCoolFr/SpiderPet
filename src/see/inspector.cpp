#include "see/inspector.hpp"

#include "core/util.hpp"

#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace sp {
namespace {

std::wstring bstr(BSTR b) {
  std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
  if (b) SysFreeString(b);
  return s;
}

ComPtr<IUIAutomationCondition> type_is(IUIAutomation* uia, long type) {
  VARIANT v;
  VariantInit(&v);
  v.vt = VT_I4;
  v.lVal = type;
  ComPtr<IUIAutomationCondition> c;
  uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &c);
  return c;
}

// The largest on-screen Document under the window that contains `hint`.
ComPtr<IUIAutomationElement> find_document(IUIAutomation* uia, IUIAutomationElement* root, POINT hint, RECT& out) {
  ComPtr<IUIAutomationElement> best;
  auto cond = type_is(uia, UIA_DocumentControlTypeId);
  if (!cond) return best;
  ComPtr<IUIAutomationElementArray> arr;
  if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) return best;
  int n = 0;
  arr->get_Length(&n);
  long best_area = 0;
  for (int i = 0; i < n && i < 40; ++i) {
    ComPtr<IUIAutomationElement> el;
    if (FAILED(arr->GetElement(i, &el)) || !el) continue;
    BOOL off = FALSE;
    el->get_CurrentIsOffscreen(&off);
    RECT r{};
    if (off || FAILED(el->get_CurrentBoundingRectangle(&r))) continue;
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (area < 200 * 150) continue;
    const bool contains = PtInRect(&r, hint) != FALSE;
    // The page is the biggest document; an inner frame or a half-loaded one
    // is smaller. Containing the drop point only breaks near-ties.
    const long score = area + (contains ? area / 10 : 0);
    if (score > best_area) {
      best_area = score;
      best = el;
      out = r;
    }
  }
  return best;
}

// Browsers show the address in an Edit hint the top of the window.
std::wstring find_address(IUIAutomation* uia, IUIAutomationElement* root, const RECT& window) {
  auto cond = type_is(uia, UIA_EditControlTypeId);
  ComPtr<IUIAutomationElementArray> arr;
  if (!cond || FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) return {};
  int n = 0;
  arr->get_Length(&n);
  for (int i = 0; i < n && i < 25; ++i) {
    ComPtr<IUIAutomationElement> el;
    if (FAILED(arr->GetElement(i, &el)) || !el) continue;
    RECT r{};
    if (FAILED(el->get_CurrentBoundingRectangle(&r)) || r.top > window.top + 170) continue;
    ComPtr<IUIAutomationValuePattern> val;
    if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&val))) || !val) continue;
    BSTR b = nullptr;
    val->get_CurrentValue(&b);
    const std::wstring v = bstr(b);
    if (v.find(L'.') != std::wstring::npos && v.find(L' ') == std::wstring::npos && v.size() > 3) return v;
  }
  return {};
}

std::wstring cached_string(IUIAutomationElement* el, PROPERTYID id) {
  VARIANT v;
  VariantInit(&v);
  std::wstring out;
  if (SUCCEEDED(el->GetCachedPropertyValue(id, &v)) && v.vt == VT_BSTR && v.bstrVal) out.assign(v.bstrVal, SysStringLen(v.bstrVal));
  VariantClear(&v);
  return out;
}

// Everything on screen that carries text: links (with targets), headings
// (by their real role), text runs and buttons. One cached cross-process call.
void read_tree(IUIAutomation* uia, IUIAutomationElement* doc, std::vector<Inspector::Node>& out) {
  RECT view{};
  if (FAILED(doc->get_CurrentBoundingRectangle(&view))) return;
  ComPtr<IUIAutomationCacheRequest> cache;
  if (FAILED(uia->CreateCacheRequest(&cache))) return;
  for (PROPERTYID id : {UIA_NamePropertyId, UIA_BoundingRectanglePropertyId, UIA_ControlTypePropertyId,
                        UIA_IsOffscreenPropertyId, UIA_HeadingLevelPropertyId, UIA_ValueValuePropertyId,
                        UIA_LegacyIAccessibleValuePropertyId, UIA_LocalizedControlTypePropertyId})
    cache->AddProperty(id);
  ComPtr<IUIAutomationCondition> cond;
  uia->CreateTrueCondition(&cond);
  ComPtr<IUIAutomationElementArray> arr;
  if (FAILED(doc->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cache.Get(), &arr)) || !arr) return;
  int n = 0;
  arr->get_Length(&n);
  for (int i = 0; i < n && i < 8000; ++i) {
    ComPtr<IUIAutomationElement> el;
    if (FAILED(arr->GetElement(i, &el)) || !el) continue;
    BOOL off = FALSE;
    el->get_CachedIsOffscreen(&off);
    RECT r{};
    if (off || FAILED(el->get_CachedBoundingRectangle(&r)) || r.right - r.left < 2 || r.bottom - r.top < 2) continue;
    const LONG cy = (r.top + r.bottom) / 2;
    if (cy < view.top || cy > view.bottom || r.right < view.left || r.left > view.right) continue;
    BSTR name = nullptr;
    el->get_CachedName(&name);
    std::wstring text = trim(bstr(name));
    if (text.empty() || text.size() > 4000) continue;
    CONTROLTYPEID type = 0;
    el->get_CachedControlType(&type);
    VARIANT lv;
    VariantInit(&lv);
    int level = 0;
    if (SUCCEEDED(el->GetCachedPropertyValue(UIA_HeadingLevelPropertyId, &lv)) && lv.vt == VT_I4 &&
        lv.lVal > HeadingLevel_None && lv.lVal <= HeadingLevel9)
      level = lv.lVal - HeadingLevel_None;
    VariantClear(&lv);
    const std::wstring role = lower(cached_string(el.Get(), UIA_LocalizedControlTypePropertyId));
    Inspector::Node node;
    node.text = std::move(text);
    node.rect = r;
    node.level = level;
    if (type == UIA_HyperlinkControlTypeId) {
      node.type = Inspector::Node::Link;
      std::wstring url = cached_string(el.Get(), UIA_ValueValuePropertyId);
      if (url.rfind(L"http", 0) != 0) url = cached_string(el.Get(), UIA_LegacyIAccessibleValuePropertyId);
      if (url.rfind(L"http", 0) == 0) node.url = std::move(url);
    } else if (level > 0 || role == L"heading") {
      node.type = Inspector::Node::Heading;
      if (level == 0) level = 2;
    } else if (type == UIA_TextControlTypeId) {
      node.type = Inspector::Node::Text;
    } else if (type == UIA_ButtonControlTypeId) {
      node.type = Inspector::Node::Button;
    } else {
      continue;
    }
    out.push_back(std::move(node));
  }
}

}  // namespace

void Inspector::start() {
  quit_ = false;
  thread_ = std::thread([this] { loop(); });
}

void Inspector::stop() {
  quit_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Inspector::inspect(HWND root, POINT hint, uint64_t token) {
  std::lock_guard lock(mu_);
  std::erase_if(jobs_, [](const Job& j) { return j.kind == 0; });
  jobs_.push_back({0, root, hint, token, 0});
  cv_.notify_all();
}

void Inspector::scan(HWND root, uint64_t token) {
  std::lock_guard lock(mu_);
  for (const Job& j : jobs_)
    if (j.kind == 2) return;  // one read at a time is plenty
  jobs_.push_back({2, root, {}, token, 0, 0});
  cv_.notify_all();
}

bool Inspector::take_scan(Scan& out) {
  std::lock_guard lock(mu_);
  if (scans_.empty()) return false;
  out = std::move(scans_.front());
  scans_.pop_front();
  return true;
}

void Inspector::scroll(HWND root, POINT at, int notches, int method) {
  std::lock_guard lock(mu_);
  jobs_.push_back({1, root, at, 0, notches, method});
  cv_.notify_all();
}

bool Inspector::take(Result& out) {
  std::lock_guard lock(mu_);
  if (results_.empty()) return false;
  out = std::move(results_.front());
  results_.pop_front();
  return true;
}

void Inspector::loop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IUIAutomation> uia;
  CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
  HWND doc_root = nullptr;
  ComPtr<IUIAutomationElement> doc;

  while (!quit_) {
    Job job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return quit_.load() || !jobs_.empty(); });
      if (quit_) break;
      job = jobs_.front();
      jobs_.pop_front();
    }
    if (job.kind == 2) {
      Scan out;
      out.token = job.token;
      const double t0 = now_seconds();
      if (uia && doc && doc_root == job.root) read_tree(uia.Get(), doc.Get(), out.nodes);
      out.took = now_seconds() - t0;
      std::lock_guard lock(mu_);
      scans_.push_back(std::move(out));
      while (scans_.size() > 2) scans_.pop_front();
      continue;
    }
    if (job.kind == 0) {
      Result r;
      r.token = job.token;
      doc.Reset();
      doc_root = job.root;
      ComPtr<IUIAutomationElement> root;
      if (uia && SUCCEEDED(uia->ElementFromHandle(job.root, &root)) && root) {
        RECT rect{};
        doc = find_document(uia.Get(), root.Get(), job.at, rect);
        if (doc) {
          r.found = true;
          r.doc = rect;
          ComPtr<IUIAutomationScrollPattern> sc;
          BOOL v = FALSE;
          if (SUCCEEDED(doc->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sc))) && sc &&
              SUCCEEDED(sc->get_CurrentVerticallyScrollable(&v)))
            r.can_scroll = v != FALSE;
        }
        RECT window{};
        GetWindowRect(job.root, &window);
        r.url = find_address(uia.Get(), root.Get(), window);
      }
      std::lock_guard lock(mu_);
      results_.push_back(std::move(r));
      continue;
    }
    // Scroll. Method 0: the scroll pattern of whatever scrolls under the point
    // (many web apps scroll an inner box, not the page), else the document.
    // Method 1, or when that fails: a wheel message at the point.
    bool done = false;
    if (job.method == 0 && uia && doc_root == job.root) {
      ComPtr<IUIAutomationScrollPattern> sc;
      ComPtr<IUIAutomationElement> el;
      ComPtr<IUIAutomationTreeWalker> walker;
      uia->get_ControlViewWalker(&walker);
      if (SUCCEEDED(uia->ElementFromPoint(job.at, &el)) && el && walker) {
        for (int up = 0; up < 14 && el && !sc; ++up) {
          ComPtr<IUIAutomationScrollPattern> cand;
          BOOL v = FALSE;
          if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&cand))) && cand &&
              SUCCEEDED(cand->get_CurrentVerticallyScrollable(&v)) && v)
            sc = cand;
          ComPtr<IUIAutomationElement> parent;
          if (FAILED(walker->GetParentElement(el.Get(), &parent))) break;
          el = parent;
        }
      }
      if (!sc && doc) doc->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sc));
      if (sc) {
        done = true;
        // A small increment is about one arrow-key step; three feel like one wheel notch.
        for (int i = 0; i < std::abs(job.notches) * 3 && done; ++i)
          done = SUCCEEDED(sc->Scroll(ScrollAmount_NoAmount,
                                      job.notches > 0 ? ScrollAmount_SmallIncrement : ScrollAmount_SmallDecrement));
      }
    }
    if (!done && IsWindow(job.root)) {
      const WPARAM wp = MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(-WHEEL_DELTA * job.notches)));
      PostMessageW(job.root, WM_MOUSEWHEEL, wp, MAKELPARAM(job.at.x, job.at.y));
    }
  }
  doc.Reset();
  uia.Reset();
  CoUninitialize();
}

}  // namespace sp
