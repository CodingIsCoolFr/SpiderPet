#include "see/page_text.hpp"

#include "core/util.hpp"
#include "see/ocr.hpp"

#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace sp {

struct PageText::Impl {
  ComPtr<IUIAutomation> uia;
  HWND window = nullptr;
  ComPtr<IUIAutomationElement> doc;
  ComPtr<IUIAutomationTextPattern> text;
  RECT doc_rect{};
  double missed_at = -100;  // last time no document was found
  HWND slow = nullptr;      // reading this window took too long: OCR it instead
};

PageText::PageText() : impl_(std::make_unique<Impl>()) {
  CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->uia));
}

PageText::~PageText() = default;

void PageText::forget() {
  impl_->doc.Reset();
  impl_->text.Reset();
  impl_->window = nullptr;
}

namespace {

std::wstring take(BSTR b) {
  std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
  if (b) SysFreeString(b);
  return s;
}

long area_of(const RECT& r) { return std::max(0L, r.right - r.left) * std::max(0L, r.bottom - r.top); }

// Icon fonts (private use area), embedded-object marks and zero-width
// characters are not words a person reads.
std::wstring readable(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if ((c >= 0xE000 && c <= 0xF8FF) || c == 0xFFFC || c == 0xFFFD || c == 0x200B || c == 0x200C || c == 0x200D ||
        c == 0xFEFF || c == 0x00AD)
      continue;
    if (c == 0x00A0) c = L' ';
    out += c;
  }
  return out;
}

// Every box of a range, screen pixels: one per line or inline run.
void all_boxes(IUIAutomationTextRange* r, std::vector<Rect>& out) {
  out.clear();
  SAFEARRAY* sa = nullptr;
  if (FAILED(r->GetBoundingRectangles(&sa)) || !sa) return;
  double* d = nullptr;
  if (SUCCEEDED(SafeArrayAccessData(sa, reinterpret_cast<void**>(&d)))) {
    const LONG n = sa->rgsabound[0].cElements;
    for (LONG i = 0; i + 3 < n; i += 4)
      if (d[i + 2] >= 1 && d[i + 3] >= 1)
        out.push_back({static_cast<float>(d[i]), static_cast<float>(d[i + 1]), static_cast<float>(d[i + 2]),
                       static_cast<float>(d[i + 3])});
    SafeArrayUnaccessData(sa);
  }
  SafeArrayDestroy(sa);
}

std::vector<std::wstring> tokens_of(const std::wstring& s) {
  std::vector<std::wstring> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::iswspace(s[i])) ++i;
    const size_t a = i;
    while (i < s.size() && !std::iswspace(s[i])) ++i;
    if (i > a) out.push_back(s.substr(a, i - a));
  }
  return out;
}

// One line's text and boxes into pieces, one per row on screen. `text` keeps
// its spaces at both ends so exact_pass can tell "link" + "." (one word) from
// "see" + "link" (two). Word boxes are spread by characters for now.
int add_pieces(const std::wstring& text, std::vector<Rect> rects, const Rect& area, std::vector<OcrLine>& out) {
  std::vector<std::wstring> toks = tokens_of(text);
  if (toks.empty()) return 0;
  std::erase_if(rects, [&](const Rect& r) { return !area.contains(r.center()); });
  if (rects.empty()) return 0;
  std::sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) {
    if (std::fabs(a.center().y - b.center().y) > std::min(a.h, b.h) * 0.5f) return a.y < b.y;
    return a.x < b.x;
  });
  std::vector<Rect> rows;
  for (const Rect& r : rects) {
    if (!rows.empty() && std::fabs(rows.back().center().y - r.center().y) < std::min(rows.back().h, r.h) * 0.5f)
      rows.back() = unite(rows.back(), r);
    else
      rows.push_back(r);
  }
  // Several rows for one line (a wrapped run): share the words out by width.
  float total_w = 0;
  for (const Rect& r : rows) total_w += r.w;
  size_t total_chars = 0;
  for (const auto& t : toks) total_chars += t.size() + 1;
  size_t next = 0;
  float used_w = 0;
  for (size_t ri = 0; ri < rows.size() && next < toks.size(); ++ri) {
    used_w += rows[ri].w;
    const size_t want = ri + 1 == rows.size() ? total_chars : static_cast<size_t>(total_chars * used_w / total_w + 0.5f);
    OcrLine piece;
    piece.box = rows[ri];
    size_t chars = 0;
    for (size_t k = 0; k < next; ++k) chars += toks[k].size() + 1;
    while (next < toks.size() && (piece.words.empty() || chars + toks[next].size() / 2 < want)) {
      OcrWord w;
      w.text = toks[next];
      piece.words.push_back(std::move(w));
      chars += toks[next].size() + 1;
      ++next;
    }
    piece.text = (ri == 0 && std::iswspace(text.front()) ? L" " : L"");
    for (size_t k = 0; k < piece.words.size(); ++k) piece.text += (k ? L" " : L"") + piece.words[k].text;
    if (next == toks.size() && std::iswspace(text.back())) piece.text += L' ';
    else if (next < toks.size()) piece.text += L' ';
    // Character-share boxes until the pixels place them.
    size_t n = 0;
    for (const OcrWord& w : piece.words) n += w.text.size() + 1;
    float x = piece.box.x;
    const float per = piece.box.w / static_cast<float>(std::max<size_t>(1, n - 1));
    for (OcrWord& w : piece.words) {
      w.box = {x, piece.box.y, per * w.text.size(), piece.box.h};
      x += per * (w.text.size() + 1);
    }
    out.push_back(std::move(piece));
  }
  return static_cast<int>(toks.size());
}

}  // namespace

bool PageText::read(HWND window, const RECT& area, std::vector<OcrLine>& out, Stats* stats) {
  Impl& m = *impl_;
  out.clear();
  if (!m.uia || !window) return false;
  const double t0 = now_seconds();
  if (window == m.slow) return false;
  if (window != m.window) forget();

  // The page: the biggest on-screen Document over the area that publishes text.
  // Browsers keep one per tab; the hidden tabs report themselves off screen.
  if (m.text) {
    BOOL off = TRUE;
    RECT r{};
    if (FAILED(m.doc->get_CurrentIsOffscreen(&off)) || off || FAILED(m.doc->get_CurrentBoundingRectangle(&r))) forget();
    else m.doc_rect = r;
  }
  if (!m.text) {
    if (window == m.window && t0 - m.missed_at < 3.0) return false;  // looked a moment ago
    m.window = window;
    ComPtr<IUIAutomationElement> root;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_DocumentControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(m.uia->ElementFromHandle(window, &root)) || !root ||
        FAILED(m.uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond)) ||
        FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) {
      m.missed_at = t0;
      return false;
    }
    int n = 0;
    arr->get_Length(&n);
    long best = 0;
    for (int i = 0; i < n && i < 40; ++i) {
      ComPtr<IUIAutomationElement> el;
      if (FAILED(arr->GetElement(i, &el)) || !el) continue;
      BOOL off = TRUE;
      RECT r{}, both{};
      if (FAILED(el->get_CurrentIsOffscreen(&off)) || off || FAILED(el->get_CurrentBoundingRectangle(&r))) continue;
      if (!IntersectRect(&both, &r, &area)) continue;
      const long a = area_of(both);
      if (a <= best) continue;
      ComPtr<IUIAutomationTextPattern> tp;
      if (FAILED(el->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&tp))) || !tp) continue;
      best = a;
      m.doc = el;
      m.text = tp;
      m.doc_rect = r;
    }
    if (!m.text) {
      m.missed_at = t0;
      return false;
    }
  }

  RECT both{};
  IntersectRect(&both, &m.doc_rect, &area);
  Stats st;
  st.cover = area_of(area) > 0 ? static_cast<float>(area_of(both)) / static_cast<float>(area_of(area)) : 0.f;

  ComPtr<IUIAutomationTextRangeArray> ranges;
  if (FAILED(m.text->GetVisibleRanges(&ranges)) || !ranges) {
    forget();
    return false;
  }
  ++st.calls;
  int nr = 0;
  ranges->get_Length(&nr);
  const Rect box{static_cast<float>(area.left), static_cast<float>(area.top), static_cast<float>(area.right - area.left),
                 static_cast<float>(area.bottom - area.top)};

  // Line by line: a handful of calls per line instead of several per word.
  // Each piece is one line's text and its box; the words inside are placed
  // later from the pixels (exact_pass), which is free.
  // Where a line is: -1 above the area, 0 in it, 1 below, 2 no box at all.
  // Chromium reports lines scrolled out of view as 1-pixel boxes pinned to
  // the edge they left by, so a thin box says which side it is on.
  std::vector<Rect> rects;
  auto where = [&](const std::vector<Rect>& rs) {
    bool above = false, below = false;
    const float mid = (m.doc_rect.top + m.doc_rect.bottom) * 0.5f;
    for (const Rect& r : rs) {
      if (r.w >= 2 && r.h >= 2 && box.contains(r.center())) return 0;
      if (r.w < 2 || r.h < 2) {  // pinned to the edge of the page it scrolled past
        (r.y < mid ? above : below) = true;
        continue;
      }
      above |= r.bottom() <= box.y + 2.f;
      below |= r.y >= box.bottom() - 2.f;
    }
    return above ? -1 : below ? 1 : 2;  // 2: beside the area, or no box
  };
  auto where_line = [&](IUIAutomationTextRange* at) {
    ComPtr<IUIAutomationTextRange> c;
    if (FAILED(at->Clone(&c)) || !c) return 2;
    c->ExpandToEnclosingUnit(TextUnit_Line);
    all_boxes(c.Get(), rects);
    st.calls += 3;
    return where(rects);
  };

  int words = 0;
  for (int i = 0; i < nr && words < 4000 && now_seconds() - t0 < 1.0; ++i) {
    ComPtr<IUIAutomationTextRange> r, w;
    if (FAILED(ranges->GetElement(i, &r)) || !r || FAILED(r->Clone(&w)) || !w) continue;
    w->MoveEndpointByRange(TextPatternRangeEndpoint_End, w.Get(), TextPatternRangeEndpoint_Start);
    // Chromium's "visible" range can be the whole page. Gallop past the lines
    // above the view, then home in on the first line inside it.
    if (where_line(w.Get()) == -1) {
      int step = 1, lo = 0, hi = -1;
      for (int tries = 0; tries < 24; ++tries) {
        ComPtr<IUIAutomationTextRange> c;
        int moved = 0;
        if (FAILED(w->Clone(&c)) || !c || FAILED(c->Move(TextUnit_Line, lo + step, &moved)) || moved < lo + step) break;
        ++st.calls;
        if (where_line(c.Get()) == -1) {
          lo += step;
          step *= 2;
        } else {
          hi = lo + step;
          break;
        }
      }
      if (hi < 0) continue;  // nothing of this range is in view
      while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        ComPtr<IUIAutomationTextRange> c;
        int moved = 0;
        if (FAILED(w->Clone(&c)) || !c || FAILED(c->Move(TextUnit_Line, mid, &moved))) break;
        ++st.calls;
        (where_line(c.Get()) == -1 ? lo : hi) = mid;
      }
      int moved = 0;
      w->Move(TextUnit_Line, hi, &moved);
      w->MoveEndpointByRange(TextPatternRangeEndpoint_End, w.Get(), TextPatternRangeEndpoint_Start);
    }
    std::wstring last_text;
    Rect last_box;
    bool seen = false;
    int out_of_view = 0;
    for (int guard = 0; guard < 600; ++guard) {
      int moved = 0;
      if (FAILED(w->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Line, 1, &moved)) || moved == 0) break;
      int past = 0;
      w->CompareEndpoints(TextPatternRangeEndpoint_End, r.Get(), TextPatternRangeEndpoint_End, &past);
      if (past > 0) w->MoveEndpointByRange(TextPatternRangeEndpoint_End, r.Get(), TextPatternRangeEndpoint_End);
      BSTR b = nullptr;
      w->GetText(4000, &b);
      const std::wstring text = readable(take(b));
      all_boxes(w.Get(), rects);
      st.calls += 5;
      const int at = where(rects);
      // Text boxes can make the line step crawl one letter at a time
      // ("Search", "earch", "arch"...): same box, shrinking text. Leave.
      const Rect first = rects.empty() ? Rect{} : rects.front();
      if (!last_text.empty() && first.x == last_box.x && first.y == last_box.y && first.w == last_box.w &&
          last_text.size() > text.size() && last_text.compare(last_text.size() - text.size(), text.size(), text) == 0)
        break;
      last_text = text;
      last_box = first;
      if (at == 0) {
        seen = true;
        out_of_view = 0;
        words += add_pieces(text, rects, box, out);
      } else if (at == 1 || (seen && ++out_of_view > 12)) {
        break;  // past the bottom of the view
      }
      if (past >= 0) break;
      w->MoveEndpointByRange(TextPatternRangeEndpoint_Start, w.Get(), TextPatternRangeEndpoint_End);
    }
  }

  st.words = words;
  st.took = now_seconds() - t0;
  // Chromium answers each question about 1.5 ms late, so a full screen takes
  // seconds. Then OCR places the words and the page tree fixes their spelling.
  if (st.took > 0.8) {
    m.slow = window;
    debug_log("page text too slow (" + std::to_string(static_cast<int>(st.took * 1000)) + " ms), using OCR for this window");
  }
  if (stats) *stats = st;
  return true;
}

namespace {

int dist(uint32_t a, uint32_t b) {
  return std::abs(int((a >> 16) & 0xFF) - int((b >> 16) & 0xFF)) + std::abs(int((a >> 8) & 0xFF) - int((b >> 8) & 0xFF)) +
         std::abs(int(a & 0xFF) - int(b & 0xFF));
}

// Places the words of one piece by its pixels: spaces are the widest empty
// columns near where the characters say each break should be. Then each
// word box is tightened to its own ink. False when nothing is drawn there.
bool fit_words(const Frame& f, OcrLine& piece, uint32_t bg, int contrast) {
  const int x0 = std::clamp(static_cast<int>(piece.box.x), 0, f.w - 1);
  const int x1 = std::clamp(static_cast<int>(std::ceil(piece.box.right())), x0 + 1, f.w);
  const int y0 = std::clamp(static_cast<int>(piece.box.y), 0, f.h - 1);
  const int y1 = std::clamp(static_cast<int>(std::ceil(piece.box.bottom())), y0 + 1, f.h);
  const int thr = std::max(30, contrast / 3);
  std::vector<uint8_t> ink(static_cast<size_t>(x1 - x0), 0);
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      if (dist(f.at(x, y) & 0xFFFFFF, bg) > thr) {
        ink[x - x0] = 1;
        break;
      }
  int L = 0, R = x1 - x0;
  while (L < R && !ink[L]) ++L;
  while (R > L && !ink[R - 1]) --R;
  if (R <= L) return false;
  auto& W = piece.words;
  const float top = static_cast<float>(y0), hgt = static_cast<float>(y1 - y0);
  if (W.size() == 1) {
    W[0].box = {static_cast<float>(x0 + L), top, static_cast<float>(R - L), hgt};
    return true;
  }
  struct Gap {
    int a, b;
  };
  std::vector<Gap> gaps;
  for (int x = L; x < R;) {
    if (ink[x]) {
      ++x;
      continue;
    }
    const int a = x;
    while (x < R && !ink[x]) ++x;
    gaps.push_back({a, x});
  }
  size_t total = 0;
  for (const OcrWord& w : W) total += w.text.size() + 1;
  --total;
  const float per = static_cast<float>(R - L) / static_cast<float>(total);
  std::vector<int> start(W.size()), end(W.size());
  start[0] = L;
  size_t chars = 0, gi = 0;
  for (size_t i = 0; i + 1 < W.size(); ++i) {
    chars += W[i].text.size();
    const float want = L + per * (static_cast<float>(chars) + 0.5f);
    ++chars;  // the space
    int best = -1;
    float best_score = 3.5f * per;
    for (size_t g = gi; g < gaps.size(); ++g) {
      const float c = (gaps[g].a + gaps[g].b) * 0.5f;
      if (c - want > 3.5f * per) break;
      const float score = std::fabs(c - want) - 0.8f * static_cast<float>(gaps[g].b - gaps[g].a);
      if (gaps[g].a > start[i] && score < best_score) {
        best_score = score;
        best = static_cast<int>(g);
      }
    }
    if (best >= 0) {
      end[i] = gaps[best].a;
      start[i + 1] = gaps[best].b;
      gi = static_cast<size_t>(best) + 1;
    } else {  // no clean space (tight kerning): split where the characters say
      const int cut = std::clamp(static_cast<int>(want), start[i] + 1, R - 1);
      end[i] = cut;
      start[i + 1] = cut;
    }
  }
  end.back() = R;
  for (size_t i = 0; i < W.size(); ++i) {
    int a = start[i], b = std::max(end[i], a + 1);
    while (a < b - 1 && !ink[a]) ++a;
    while (b > a + 1 && !ink[b - 1]) --b;
    W[i].box = {static_cast<float>(x0 + a), top, static_cast<float>(b - a), hgt};
  }
  return true;
}

}  // namespace

bool exact_pass(const std::vector<OcrLine>& pieces, const RECT& area, const Frame& f, OcrPass& out, int* dropped) {
  out = {};
  int gone = 0;
  // Frame pixels, colors, and words placed by their ink.
  std::vector<OcrLine> fitted;
  for (const OcrLine& src : pieces) {
    OcrLine p = src;
    p.box = src.box.moved(static_cast<float>(-area.left), static_cast<float>(-area.top));
    if (p.box.x < 0 || p.box.y < 0 || p.box.right() > f.w + 1 || p.box.bottom() > f.h + 1) continue;
    // Text the page carries but does not paint (hidden labels, text under
    // a popup) has no ink in its box.
    const int contrast = sample_colors(f, p.box, p.fg, p.bg);
    if (contrast < 40 || !fit_words(f, p, p.bg, contrast)) {
      gone += static_cast<int>(p.words.size());
      continue;
    }
    for (OcrWord& w : p.words) {
      sample_colors(f, w.box, w.fg, w.bg);
      w.bg = p.bg;  // a word box is too tight to see its paper; the line's is right
    }
    fitted.push_back(std::move(p));
  }

  // Pieces into lines the way OCR reports them: same row, close together.
  // Browsers split a line at every link or bold run; with no space between
  // two pieces (a link, then a period), their touching words are one word.
  OcrLine line;
  bool open = true;  // the text so far ends in a space
  auto flush = [&] {
    if (!line.words.empty()) out.lines.push_back(std::move(line));
    line = {};
    open = true;
  };
  for (OcrLine& p : fitted) {
    if (!line.words.empty()) {
      const float cy = p.box.center().y;
      const float h = std::max(line.box.h, p.box.h);
      const float gap = p.box.x - line.box.right();
      if (cy < line.box.y || cy > line.box.bottom() || gap < -3.f || gap > 1.6f * h) flush();
    }
    const bool space_before = p.text.empty() || std::iswspace(p.text.front());
    size_t k = 0;
    if (!line.words.empty() && !open && !space_before) {
      OcrWord& prev = line.words.back();
      const OcrWord& first = p.words.front();
      if (first.box.x - prev.box.right() < 0.35f * p.box.h) {
        prev.text += first.text;
        prev.box = unite(prev.box, first.box);
        k = 1;
      }
    }
    for (; k < p.words.size(); ++k) line.words.push_back(std::move(p.words[k]));
    line.box = unite(line.box, p.box);
    line.fg = line.words.front().fg;
    line.bg = p.bg;
    open = p.text.empty() || std::iswspace(p.text.back());
  }
  flush();
  for (OcrLine& l : out.lines) {
    l.text.clear();
    l.box = {};
    for (const OcrWord& w : l.words) {
      l.text += (l.text.empty() ? L"" : L" ") + w.text;
      l.box = unite(l.box, w.box);
    }
  }

  // Two texts cannot both be visible in the same spot. Screen-reader-only
  // labels ("Repository files navigation" on GitHub) report a box on top of
  // the real text; they are short, so the line with fewer words goes.
  auto collide = [](const OcrLine& a, const OcrLine& b) {
    for (const OcrWord& x : a.words)
      for (const OcrWord& y : b.words)
        if (overlap_area(x.box, y.box) > 0.4f * std::min(x.box.w * x.box.h, y.box.w * y.box.h)) return true;
    return false;
  };
  std::vector<bool> hidden(out.lines.size(), false);
  for (size_t i = 0; i < out.lines.size(); ++i)
    for (size_t j = i + 1; j < out.lines.size(); ++j) {
      if (hidden[i] || hidden[j] || overlap_area(out.lines[i].box, out.lines[j].box) <= 0) continue;
      if (!collide(out.lines[i], out.lines[j])) continue;
      const bool drop_i = out.lines[i].words.size() < out.lines[j].words.size() ||
                          (out.lines[i].words.size() == out.lines[j].words.size() && out.lines[i].box.h > out.lines[j].box.h);
      hidden[drop_i ? i : j] = true;
    }
  for (size_t i = out.lines.size(); i-- > 0;)
    if (hidden[i]) {
      gone += static_cast<int>(out.lines[i].words.size());
      out.lines.erase(out.lines.begin() + static_cast<long>(i));
    }
  std::stable_sort(out.lines.begin(), out.lines.end(), [](const OcrLine& a, const OcrLine& b) {
    if (std::fabs(a.box.y - b.box.y) > std::min(a.box.h, b.box.h) * 0.5f) return a.box.y < b.box.y;
    return a.box.x < b.box.x;
  });
  if (dropped) *dropped = gone;
  if (out.lines.empty()) return false;
  out.exact = true;
  out.w = f.w;
  out.h = f.h;
  out.scroll = f.scroll;
  out.time = f.time;
  out.image = std::make_shared<const Frame>(f);
  return true;
}

}  // namespace sp
