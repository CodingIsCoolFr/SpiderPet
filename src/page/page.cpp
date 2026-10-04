#include "page/page.hpp"

#include "core/util.hpp"

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace sp {

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::Sentence: return "sentence";
    case Kind::Heading: return "heading";
    case Kind::Title: return "title";
    case Kind::Doi: return "doi";
    case Kind::Isbn: return "isbn";
    case Kind::Id: return "id";
    case Kind::Link: return "link";
    case Kind::Url: return "url";
    case Kind::Email: return "email";
    case Kind::Date: return "date";
    case Kind::Number: return "number";
    case Kind::Handle: return "handle";
    case Kind::Item: return "item";
  }
  return "";
}

float kind_priority(Kind k) {
  switch (k) {
    case Kind::Sentence: return 2.2f;
    case Kind::Heading: return 1.8f;
    case Kind::Title: return 2.4f;
    case Kind::Doi: return 2.1f;
    case Kind::Isbn: return 2.2f;
    case Kind::Id: return 1.7f;
    case Kind::Link: return 1.4f;
    case Kind::Url: return 1.5f;
    case Kind::Email: return 1.6f;
    case Kind::Date: return 0.9f;
    case Kind::Number: return 1.2f;
    case Kind::Handle: return 1.3f;
    case Kind::Item: return 1.5f;
  }
  return 1.f;
}

namespace {

const std::unordered_set<std::wstring>& stopwords() {
  static const std::unordered_set<std::wstring> s = {
      L"about", L"after", L"also", L"because", L"been", L"being", L"between", L"both", L"could", L"does", L"each",
      L"from", L"have", L"into", L"many", L"more", L"most", L"much", L"only", L"other", L"over", L"same", L"some",
      L"such", L"than", L"that", L"their", L"them", L"then", L"there", L"these", L"they", L"this", L"those",
      L"through", L"under", L"very", L"were", L"what", L"when", L"where", L"which", L"while", L"with", L"would",
      L"your", L"wikipedia", L"google", L"chrome", L"brave", L"edge", L"firefox", L"mozilla", L"microsoft",
      L"youtube", L"twitter", L"reddit", L"home", L"page", L"retrieved", L"archived", L"original", L"journal"};
  return s;
}

std::wstring core_of(const std::wstring& w) {
  size_t a = 0, b = w.size();
  auto edge = [](wchar_t c) {
    return c == L'.' || c == L',' || c == L';' || c == L':' || c == L'(' || c == L')' || c == L'[' || c == L']' ||
           c == L'"' || c == L'\'' || c == 0x201C || c == 0x201D || c == 0x2018 || c == 0x2019 || c == L'!' ||
           c == L'?';
  };
  while (a < b && edge(w[a])) ++a;
  while (b > a && edge(w[b - 1])) --b;
  return w.substr(a, b - a);
}

int count_digits(std::wstring_view s) {
  int n = 0;
  for (wchar_t c : s) n += std::iswdigit(c) ? 1 : 0;
  return n;
}

int count_letters(std::wstring_view s) {
  int n = 0;
  for (wchar_t c : s) n += std::iswalpha(c) ? 1 : 0;
  return n;
}

bool is_month(const std::wstring& lw) {
  static const wchar_t* m[] = {L"january", L"february", L"march", L"april", L"may", L"june", L"july", L"august",
                               L"september", L"october", L"november", L"december", L"jan", L"feb", L"mar", L"apr",
                               L"jun", L"jul", L"aug", L"sep", L"sept", L"oct", L"nov", L"dec"};
  for (const wchar_t* s : m)
    if (lw == s) return true;
  return false;
}

bool is_year(const std::wstring& c) {
  return c.size() == 4 && count_digits(c) == 4 && (c[0] == L'1' || c[0] == L'2');
}

bool is_day(const std::wstring& c) {
  if (c.empty() || c.size() > 2 || count_digits(c) != static_cast<int>(c.size())) return false;
  const int d = _wtoi(c.c_str());
  return d >= 1 && d <= 31;
}

bool is_domain(const std::wstring& c) {
  static const wchar_t* tlds[] = {L".com", L".org", L".net", L".io", L".gov", L".edu", L".co", L".uk", L".de",
                                  L".ai", L".dev", L".app", L".info", L".me", L".tv", L".gg"};
  const std::wstring lw = lower(c);
  if (lw.size() < 5 || count_letters(lw) < 3) return false;
  for (const wchar_t* t : tlds) {
    const size_t at = lw.find(t);
    if (at == std::wstring::npos || at == 0) continue;
    const size_t end = at + wcslen(t);
    if (end == lw.size() || lw[end] == L'/') return true;
  }
  return false;
}

bool opens_quote(const std::wstring& w) { return !w.empty() && (w[0] == L'"' || w[0] == 0x201C); }

bool closes_quote(const std::wstring& w) {
  std::wstring s = w;
  while (!s.empty() && (s.back() == L'.' || s.back() == L',' || s.back() == L';' || s.back() == L':')) s.pop_back();
  return !s.empty() && (s.back() == L'"' || s.back() == 0x201D);
}

std::wstring strip_quotes(std::wstring s) {
  while (!s.empty() && (s.back() == L'.' || s.back() == L',' || s.back() == L';' || s.back() == L':')) s.pop_back();
  auto q = [](wchar_t c) { return c == L'"' || c == 0x201C || c == 0x201D; };
  while (!s.empty() && q(s.front())) s.erase(s.begin());
  while (!s.empty() && q(s.back())) s.pop_back();
  return s;
}

// Wikipedia-style reference markers: [12], [a], [citation needed].
std::wstring strip_refs(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == L'[') {
      // OCR often loses the closing bracket: "[31" at the end of a word.
      size_t j = i + 1;
      while (j < in.size() && j - i <= 5 && std::iswalnum(in[j])) ++j;
      if (j > i + 1 && (j == in.size() || in[j] == L' ' || in[j] == L'[')) {
        i = j - 1;
        continue;
      }
      const size_t close = in.find(L']', i);
      if (close != std::wstring::npos && close - i <= 20) {
        i = close;
        continue;
      }
    }
    out += in[i];
  }
  std::wstring squeezed;
  for (wchar_t c : out) {
    if (c == L' ' && !squeezed.empty() && squeezed.back() == L' ') continue;
    squeezed += c;
  }
  return trim(squeezed);
}

bool is_ref_token(const std::wstring& w) { return !w.empty() && w[0] == L'[' && w.size() <= 8; }

// On light-on-dark text the reader sometimes sees a period as a dash:
// "organisms-" before a capital letter is the end of a sentence.
bool ocr_period(const std::wstring& raw) {
  const std::wstring w = strip_refs(raw);
  return w.size() >= 4 && w.back() == L'-' && std::iswlower(w[w.size() - 2]);
}

bool ends_sentence(const std::wstring& raw) {
  std::wstring w = strip_refs(raw);
  // drop trailing "[10]" and closing quotes or brackets
  while (!w.empty()) {
    if (w.back() == L']') {
      const size_t open = w.rfind(L'[');
      if (open == std::wstring::npos) break;
      w.resize(open);
      continue;
    }
    if (w.back() == L'"' || w.back() == 0x201D || w.back() == L')' || w.back() == L'\'' || w.back() == 0x2019) {
      w.pop_back();
      continue;
    }
    break;
  }
  if (w.empty()) return false;
  const wchar_t last = w.back();
  if (last != L'.' && last != L'!' && last != L'?') return false;
  if (last != L'.') return true;
  const std::wstring lw = lower(w);
  static const wchar_t* abbr[] = {L"e.g.", L"i.e.", L"etc.", L"vs.", L"dr.", L"mr.", L"mrs.", L"ms.", L"st.",
                                  L"jr.", L"sr.", L"no.", L"fig.", L"approx.", L"al.", L"u.s.", L"ca.", L"cf."};
  for (const wchar_t* a : abbr)
    if (lw == a) return false;
  // Initials like "J." or "G.C.L."
  if (w.size() <= 2 && std::iswupper(w[0])) return false;
  if (w.size() >= 4 && w[w.size() - 3] == L'.' && std::iswupper(w[w.size() - 2])) return false;
  return true;
}

bool starts_upper(const std::wstring& w) {
  for (wchar_t c : w) {
    if (c == L'"' || c == 0x201C || c == L'(' || c == L'\'' || c == 0x2018) continue;
    return std::iswupper(c) || std::iswdigit(c);
  }
  return false;
}

uint32_t quantize(uint32_t c) { return c & 0xF0F0F0; }

int color_dist(uint32_t a, uint32_t b) {
  return std::abs(int((a >> 16) & 0xFF) - int((b >> 16) & 0xFF)) + std::abs(int((a >> 8) & 0xFF) - int((b >> 8) & 0xFF)) +
         std::abs(int(a & 0xFF) - int(b & 0xFF));
}

float line_font_px(const Rect& box) { return std::max(8.f, box.h / 0.92f); }

}  // namespace

void Page::clear() {
  facts_.clear();
  lines_.clear();
  blocks_.clear();
  entities_.clear();
  topic_words_.clear();
}

void Page::set_title(const std::wstring& title) {
  title_words_.clear();
  std::wstring cur;
  auto flush = [&] {
    if (cur.size() >= 4 && !stopwords().count(cur)) title_words_.push_back(cur);
    cur.clear();
  };
  for (wchar_t c : lower(title)) {
    if (std::iswalpha(c)) cur += c;
    else flush();
  }
  flush();
}

Entity* Page::find(int id) {
  for (Entity& e : entities_)
    if (e.id == id) return &e;
  return nullptr;
}

int Page::harvested() const {
  int n = 0;
  for (const Entity& e : entities_) n += e.mark == Mark::Done ? 1 : 0;
  return n;
}

void Page::merge_facts(std::vector<Fact> facts, float y0, float y1) {
  std::erase_if(facts_, [&](const Fact& f) { return f.box.center().y >= y0 && f.box.center().y <= y1; });
  for (Fact& f : facts) facts_.push_back(std::move(f));
  std::vector<Entity> found;
  extract(y0, y1, found);
  reconcile(found, y0, y1, nullptr);
}

// The page's own text inside an area, in reading order.
std::wstring Page::facts_text(const Rect& area) const {
  std::vector<const Fact*> in;
  for (const Fact& f : facts_)
    if (f.type != Fact::Button && !f.text.empty() && area.inflated(3.f).contains(f.box.center())) in.push_back(&f);
  std::sort(in.begin(), in.end(), [](const Fact* a, const Fact* b) {
    if (std::fabs(a->box.center().y - b->box.center().y) > std::min(a->box.h, b->box.h) * 0.5f)
      return a->box.y < b->box.y;
    return a->box.x < b->box.x;
  });
  std::wstring out;
  for (const Fact* f : in) {
    if (!out.empty() && out.find(f->text) != std::wstring::npos) continue;  // a link inside its paragraph
    if (!out.empty()) out += L' ';
    out += f->text;
  }
  return trim(out);
}

void Page::shift(float dy) {
  for (Fact& f : facts_) f.box.y += dy;
  for (TextLine& l : lines_) {
    l.box.y += dy;
    for (OcrWord& w : l.words) w.box.y += dy;
  }
  for (Block& b : blocks_) b.box.y += dy;
  for (Entity& e : entities_) {
    e.box.y += dy;
    for (Rect& r : e.lines) r.y += dy;
  }
}

float Page::merge(const OcrPass& pass, const std::function<bool(const Rect&)>& visible) {
  const float scroll = static_cast<float>(pass.scroll);
  const float y0 = scroll + 2.f;
  const float y1 = scroll + static_cast<float>(pass.h) - 2.f;

  std::vector<TextLine> fresh;
  for (const OcrLine& src : pass.lines) {
    if (src.box.y < 1.5f || src.box.bottom() > pass.h - 1.5f) continue;  // cut by the frame edge
    if (!pass.fixed.empty()) {  // a sticky bar, not page content
      int fixed = 0, rows = 0;
      for (int y = static_cast<int>(src.box.y); y < static_cast<int>(src.box.bottom()); ++y, ++rows)
        if (y >= 0 && y < static_cast<int>(pass.fixed.size())) fixed += pass.fixed[y];
      if (rows > 0 && fixed * 2 > rows) continue;
    }
    TextLine l;
    l.text = src.text;
    l.box = src.box.moved(0, scroll);
    l.fg = src.fg;
    l.bg = src.bg;
    for (const OcrWord& w : src.words) {
      OcrWord cw = w;
      cw.box.y += scroll;
      l.words.push_back(std::move(cw));
    }
    if (visible && !visible(l.box)) continue;
    fresh.push_back(std::move(l));
  }

  // The reader sometimes splits one visual line in two at a footnote mark.
  // Pieces on the same row with a small gap are one line again.
  // Left to right, so each piece finds the piece just before it on its row.
  std::sort(fresh.begin(), fresh.end(), [](const TextLine& a, const TextLine& b) { return a.box.x < b.box.x; });
  std::vector<TextLine> joined;
  for (TextLine& l : fresh) {
    TextLine* best = nullptr;
    float best_gap = 3.6f * word_h_;
    for (TextLine& p : joined) {
      const float ov = std::min(p.box.bottom(), l.box.bottom()) - std::max(p.box.y, l.box.y);
      const float gap = l.box.x - p.box.right();
      // Same paper too: a side box next to the text has its own background.
      if (ov >= 0.6f * std::min(p.box.h, l.box.h) && gap > -2.f && gap < best_gap && color_dist(p.bg, l.bg) < 40) {
        best = &p;
        best_gap = gap;
      }
    }
    if (!best) {
      joined.push_back(std::move(l));
      continue;
    }
    best->text += L' ';
    best->text += l.text;
    best->box = unite(best->box, l.box);
    for (OcrWord& w : l.words) best->words.push_back(std::move(w));
  }
  fresh = std::move(joined);

  // Drift: the scroll estimate is good but not perfect. Lines read again in
  // the same place say how far off it is.
  float drift = 0;
  {
    std::vector<float> dys;
    const float window = std::max(40.f, line_h_ * 3.f);
    for (const TextLine& n : fresh) {
      if (n.text.size() < 12) continue;
      float best = 1e9f;
      for (const TextLine& o : lines_) {
        if (o.text != n.text || std::fabs(o.box.x - n.box.x) > 6.f) continue;
        const float dy = n.box.y - o.box.y;
        if (std::fabs(dy) < window && std::fabs(dy) < std::fabs(best)) best = dy;
      }
      if (best < 1e8f) dys.push_back(best);
    }
    if (dys.size() >= 3) {
      std::nth_element(dys.begin(), dys.begin() + dys.size() / 2, dys.end());
      const float m = dys[dys.size() / 2];
      int agree = 0;
      for (float d : dys) agree += std::fabs(d - m) <= 2.f ? 1 : 0;
      if (std::fabs(m) >= 1.5f && agree * 10 >= static_cast<int>(dys.size()) * 6) {
        shift(m);
        drift = m;
      }
    }
  }

  std::erase_if(lines_, [&](const TextLine& l) {
    return l.box.y >= y0 && l.box.bottom() <= y1 && (!visible || visible(l.box));
  });
  for (TextLine& l : fresh) lines_.push_back(std::move(l));
  std::sort(lines_.begin(), lines_.end(), [](const TextLine& a, const TextLine& b) {
    return a.box.y != b.box.y ? a.box.y < b.box.y : a.box.x < b.box.x;
  });

  rebuild_stats();
  build_blocks();
  std::vector<Entity> found;
  extract(y0, y1, found);
  reconcile(found, y0, y1, visible);
  return drift;
}

void Page::rebuild_stats() {
  if (lines_.empty()) return;
  std::vector<float> hs, ws;
  std::unordered_map<uint32_t, int> paper, ink;
  for (const TextLine& l : lines_) {
    hs.push_back(l.box.h);
    for (const OcrWord& w : l.words) ws.push_back(w.box.h);
    paper[quantize(l.bg)] += static_cast<int>(l.text.size());
    // Body text is gray or white; colored text (links, titles in apps like
    // YouTube Studio) must not become "normal" just because there is a lot of it.
    for (const OcrWord& w : l.words) {
      const int r = (w.fg >> 16) & 0xFF, g = (w.fg >> 8) & 0xFF, b = w.fg & 0xFF;
      const int chroma = std::max({r, g, b}) - std::min({r, g, b});
      ink[quantize(w.fg)] += static_cast<int>(w.text.size()) * (chroma < 40 ? 4 : 1);
    }
  }
  std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
  line_h_ = std::max(6.f, hs[hs.size() / 2]);
  if (!ws.empty()) {
    std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());
    word_h_ = std::max(4.f, ws[ws.size() / 2]);
  }
  auto top = [](const std::unordered_map<uint32_t, int>& m) {
    uint32_t best = 0;
    int n = -1;
    for (auto& [k, v] : m)
      if (v > n) {
        n = v;
        best = k;
      }
    return best;
  };
  const uint32_t pq = top(paper), iq = top(ink);
  for (const TextLine& l : lines_)
    if (quantize(l.bg) == pq) {
      paper_ = l.bg;
      break;
    }
  for (const TextLine& l : lines_)
    for (const OcrWord& w : l.words)
      if (quantize(w.fg) == iq) {
        ink_ = w.fg;
        goto found_ink;
      }
found_ink:
  dark_ = luma(paper_) < 0.45f;
}

bool Page::is_link_color(uint32_t fg) const {
  const int r = (fg >> 16) & 0xFF, g = (fg >> 8) & 0xFF, b = fg & 0xFF;
  const int chroma = std::max({r, g, b}) - std::min({r, g, b});
  return chroma > 40 && color_dist(fg, ink_) > 70;
}

void Page::build_blocks() {
  blocks_.clear();
  for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
    const TextLine& l = lines_[i];
    int pick = -1;
    // Every block still open just above this line, however many side-menu
    // items sit between them in reading order.
    for (int b = static_cast<int>(blocks_.size()) - 1; b >= 0; --b) {
      const TextLine& last = lines_[blocks_[b].lines.back()];
      if (last.box.bottom() < l.box.y - 3.f * line_h_) continue;
      const TextLine& first = lines_[blocks_[b].lines.front()];
      const float gap = l.box.y - last.box.bottom();
      const float h = std::max(l.box.h, last.box.h);
      if (gap < -0.3f * h || gap > 0.95f * h) continue;
      if (std::max(l.box.h, last.box.h) > 1.35f * std::min(l.box.h, last.box.h)) continue;
      const bool aligned = std::fabs(l.box.x - first.box.x) < 2.5f * line_h_;
      const float ov = std::min(l.box.right(), last.box.right()) - std::max(l.box.x, last.box.x);
      if (!aligned && ov < 0.5f * std::min(l.box.w, last.box.w)) continue;
      pick = b;
      break;
    }
    if (pick < 0) {
      blocks_.push_back({});
      pick = static_cast<int>(blocks_.size()) - 1;
    }
    blocks_[pick].lines.push_back(i);
    blocks_[pick].box = unite(blocks_[pick].box, l.box);
  }

  std::vector<float> gaps;
  for (const Block& b : blocks_)
    for (size_t i = 1; i < b.lines.size(); ++i) gaps.push_back(lines_[b.lines[i]].box.y - lines_[b.lines[i - 1]].box.y);
  if (gaps.size() >= 3) {
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    pitch_ = std::max(8.f, gaps[gaps.size() / 2]);
  }

  std::vector<float> lefts;
  std::unordered_map<std::wstring, int> freq;
  line_block_.assign(lines_.size(), -1);
  for (int bi = 0; bi < static_cast<int>(blocks_.size()); ++bi)
    for (int li : blocks_[bi].lines) line_block_[li] = bi;
  for (Block& b : blocks_) {
    int chars = 0, digits = 0;
    for (int i : b.lines) {
      chars += static_cast<int>(lines_[i].text.size());
      digits += count_digits(lines_[i].text);
    }
    b.prose = chars >= 80 && digits * 100 < chars * 12 && (b.lines.size() >= 2 || chars >= 120);
    if (!b.prose) continue;
    lefts.push_back(b.box.x);
    for (int i : b.lines)
      for (const OcrWord& w : lines_[i].words) {
        const std::wstring c = lower(core_of(w.text));
        if (c.size() >= 5 && count_letters(c) == static_cast<int>(c.size()) && !stopwords().count(c)) ++freq[c];
      }
  }
  if (!lefts.empty()) {
    std::nth_element(lefts.begin(), lefts.begin() + lefts.size() / 2, lefts.end());
    column_x_ = lefts[lefts.size() / 2];
  }
  std::vector<std::pair<int, std::wstring>> ranked;
  for (auto& [w, n] : freq)
    if (n >= 3) ranked.push_back({n, w});
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
  topic_words_.clear();
  for (size_t i = 0; i < ranked.size() && i < 15; ++i) topic_words_.push_back(ranked[i].second);
}

void Page::extract(float y0, float y1, std::vector<Entity>& out) const {
  const float pad = line_h_ * 2.f;
  for (const Block& b : blocks_) {
    if (b.box.bottom() < y0 - pad || b.box.y > y1 + pad) continue;
    for (int i : b.lines) {
      const TextLine& l = lines_[i];
      if (l.box.bottom() < y0 || l.box.y > y1) continue;
      extract_line(l, b.prose, out);
    }
    if (b.prose) extract_sentences(b, out);
  }
  extract_items(y0, y1, out);
}

// Pages made of cards, tiles and tables (dashboards, new-tab pages, YouTube
// Studio) have little prose. Their text units are worth harvesting too:
// a table row as one record, a card's text as one item.
void Page::extract_items(float y0, float y1, std::vector<Entity>& out) const {
  const int n = static_cast<int>(lines_.size());
  std::vector<char> taken(n, 0);
  auto in_band = [&](const TextLine& l) { return l.box.bottom() >= y0 && l.box.y <= y1; };
  auto prose_line = [&](int i) { return line_block_.size() == lines_.size() && line_block_[i] >= 0 && blocks_[line_block_[i]].prose; };
  auto make = [&](std::vector<int> idx, const wchar_t* glue) {
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return lines_[a].box.x < lines_[b].box.x; });
    Entity e;
    e.kind = Kind::Item;
    int letters = 0;
    for (int i : idx) {
      const TextLine& l = lines_[i];
      std::wstring part = has_facts() ? facts_text(l.box) : std::wstring();
      // In browser mode, text the page does not have is a picture or a logo
      // misread by OCR: leave it out.
      if (part.empty() && has_facts()) continue;
      if (part.empty()) part = l.text;
      if (!e.text.empty()) e.text += glue;
      e.text += part;
      e.lines.push_back(l.box);
      e.box = unite(e.box, l.box);
      letters += count_letters(part);
    }
    if (e.lines.empty() || letters < 4 || e.text.size() > 220) return;
    e.font_px = line_font_px(lines_[idx.front()].box);
    e.fg = lines_[idx.front()].fg;
    e.bg = lines_[idx.front()].bg;
    e.score = kind_priority(Kind::Item);
    // Sidebars next to an article are menus, not content.
    if (column_x_ > 0 && e.box.right() < column_x_ - 20.f) e.score -= 0.8f;
    out.push_back(std::move(e));
    for (int i : idx) taken[i] = 1;
  };

  // Rows: two or more pieces of text side by side on the same baseline.
  for (int i = 0; i < n; ++i) {
    const TextLine& a = lines_[i];
    if (taken[i] || prose_line(i) || !in_band(a)) continue;
    std::vector<int> row{i};
    for (int j = i + 1; j < n && lines_[j].box.y < a.box.bottom(); ++j) {
      const TextLine& b = lines_[j];
      if (taken[j] || prose_line(j)) continue;
      const float dy = std::fabs(a.box.center().y - b.box.center().y);
      if (dy < 0.45f * std::min(a.box.h, b.box.h) && std::max(a.box.h, b.box.h) < 1.5f * std::min(a.box.h, b.box.h))
        row.push_back(j);
    }
    if (row.size() >= 2 && row.size() <= 8) make(row, L"  ·  ");
  }

  // Cards and labels: the rest of the short non-prose blocks.
  for (const Block& b : blocks_) {
    if (b.prose || b.lines.size() > 4) continue;
    std::vector<int> idx;
    int words = 0, letters = 0;
    for (int li : b.lines) {
      if (taken[li] || !in_band(lines_[li])) continue;
      idx.push_back(li);
      words += static_cast<int>(lines_[li].words.size());
      letters += count_letters(lines_[li].text);
    }
    if (idx.empty() || (words < 2 && letters < 8)) continue;
    // A citation already yields its own DOIs, titles and ids.
    bool typed = false;
    for (const Entity& e : out)
      typed |= e.kind != Kind::Item && e.kind != Kind::Sentence && overlap_area(e.box, b.box) > 0;
    if (typed) continue;
    std::sort(idx.begin(), idx.end(), [&](int x, int y) { return lines_[x].box.y < lines_[y].box.y; });
    make(idx, L" ");
  }
}

void Page::extract_line(const TextLine& line, bool prose, std::vector<Entity>& out) const {
  const auto& W = line.words;
  const int n = static_cast<int>(W.size());
  std::vector<bool> used(n, false);

  auto add = [&](Kind k, int a, int b, std::wstring text, std::wstring label = {}) {
    if (a < 0 || b >= n || a > b) return;
    for (int i = a; i <= b; ++i)
      if (used[i]) return;
    for (int i = a; i <= b; ++i) used[i] = true;
    // The external-link arrow sometimes reads as a stray accented letter glued
    // to the last word ("Spidersö"): drop a lone non-ASCII tail on ASCII text.
    if (k != Kind::Sentence && text.size() > 3 && text.back() > 127) {
      bool ascii = true;
      for (size_t i = 0; i + 1 < text.size(); ++i) ascii &= text[i] < 128;
      if (ascii) text.pop_back();
    }
    Entity e;
    e.kind = k;
    e.text = std::move(text);
    e.label = std::move(label);
    Rect r;
    for (int i = a; i <= b; ++i) r = unite(r, W[i].box);
    e.box = r;
    e.lines = {r};
    e.font_px = line_font_px(line.box);
    e.fg = W[a].fg;
    e.bg = line.bg;
    e.score = kind_priority(k);
    out.push_back(std::move(e));
  };

  // OCR often reads the "I" in an id label as "l", or drops a letter.
  static const std::pair<const wchar_t*, const wchar_t*> id_labels[] = {
      {L"pmid", L"PMID"},   {L"pmd", L"PMID"},   {L"pmc", L"PMC"},       {L"issn", L"ISSN"},  {L"lssn", L"ISSN"},
      {L"oclc", L"OCLC"},   {L"s2cid", L"S2CID"}, {L"s2cld", L"S2CID"},  {L"jstor", L"JSTOR"}, {L"arxiv", L"arXiv"},
      {L"bibcode", L"Bibcode"}, {L"hdl", L"HDL"}, {L"lccn", L"LCCN"},    {L"zbl", L"Zbl"}};
  auto id_label = [&](const std::wstring& lw) -> const wchar_t* {
    for (const auto& [key, name] : id_labels)
      if (lw == key) return name;
    return nullptr;
  };

  // What the page itself says comes first: headings by their real role, links
  // with their real text and target.
  if (has_facts()) {
    for (const Fact& f : facts_) {
      if (f.type != Fact::Heading && f.type != Fact::Link) continue;
      const float ov = std::min(f.box.bottom(), line.box.bottom()) - std::max(f.box.y, line.box.y);
      if (ov < 0.5f * std::min(f.box.h, line.box.h) || f.box.h > line.box.h * 1.9f + 4.f) continue;
      int a = -1, b = -1;
      for (int i = 0; i < n; ++i)
        if (f.box.inflated(2.f).contains(W[i].box.center())) {
          if (a < 0) a = i;
          b = i;
        }
      if (a < 0) continue;
      bool clash = false;
      for (int i = a; i <= b; ++i) clash |= used[i];
      if (clash) continue;
      const std::wstring t = trim(f.text);
      const std::wstring lu = lower(f.url);
      if (f.type == Fact::Heading) {
        if (t.size() >= 3 && t.size() <= 140) add(Kind::Heading, a, b, t);
        continue;
      }
      const std::wstring prev = a > 0 ? lower(core_of(W[a - 1].text)) : L"";
      if (lu.find(L"doi.org/") != std::wstring::npos || t.rfind(L"10.", 0) == 0) add(Kind::Doi, a, b, t);
      else if (lu.find(L"booksources") != std::wstring::npos || lu.find(L"isbn") != std::wstring::npos) add(Kind::Isbn, a, b, t, L"ISBN");
      else if (const wchar_t* lab = id_label(prev)) add(Kind::Id, a, b, t, lab);
      else if (count_letters(t) >= 6 && (t.find(L' ') != std::wstring::npos || count_letters(t) >= 10))
        add(opens_quote(a > 0 ? W[a - 1].text : L"") || opens_quote(W[a].text) ? Kind::Title : Kind::Link, a, b, t);
      else continue;
      if (!f.url.empty() && !out.empty()) out.back().href = f.url;
    }
  }

  // Headings: a short line set noticeably larger than the body text. Word
  // heights, not the line box, so a raised footnote mark does not count.
  // Most of its real words must be big: a radio button glyph next to a label
  // ("O Wide") is tall but is not a heading.
  int letters = 0, longest = 0, big_words = 0, real_words = 0;
  for (const OcrWord& w : W) {
    const int l = count_letters(w.text);
    letters += l;
    longest = std::max(longest, l);
    if (l >= 2) {
      ++real_words;
      big_words += w.box.h >= 1.3f * word_h_ ? 1 : 0;
    }
  }
  const bool big = real_words > 0 && big_words * 10 >= real_words * 7;
  const bool glyph_first = n > 0 && count_letters(W[0].text) + count_digits(W[0].text) <= 1 && W[0].text.size() <= 2;
  if (!has_facts() && big && !glyph_first && n <= 10 && longest >= 4 && line.text.find(L'[') == std::wstring::npos) {
    add(Kind::Heading, 0, n - 1, line.text);
    return;
  }


  for (int i = 0; i < n; ++i) {
    if (used[i]) continue;
    const std::wstring& w = W[i].text;
    const std::wstring c = core_of(w);
    const std::wstring lc = lower(c);

    const size_t doi = c.find(L"10.");
    if (doi != std::wstring::npos && (doi == 0 || lower(c.substr(0, doi)).find(L"doi") != std::wstring::npos) &&
        c.find(L'/', doi) != std::wstring::npos && c.size() - doi >= 8) {
      add(Kind::Doi, i, i, c.substr(doi));
      continue;
    }
    if (lc.rfind(L"isbn", 0) == 0) {
      std::wstring value;
      int j = i + 1, digits = 0;
      if (lc.size() > 5 && count_digits(lc) >= 10) {  // "ISBN:978-..."
        add(Kind::Isbn, i, i, c.substr(c.find_first_of(L"0123456789")), L"ISBN");
        continue;
      }
      while (j < n && digits < 13) {
        const std::wstring v = core_of(W[j].text);
        if (v.empty() || v.find_first_not_of(L"0123456789-Xx") != std::wstring::npos) break;
        digits += count_digits(v);
        value += v;
        ++j;
      }
      if (digits >= 10) add(Kind::Isbn, i + 1, j - 1, value, L"ISBN");
      continue;
    }
    if ((lc.rfind(L"978", 0) == 0 || lc.rfind(L"979", 0) == 0) && count_digits(lc) == 13 &&
        lc.find_first_not_of(L"0123456789-") == std::wstring::npos) {
      add(Kind::Isbn, i, i, c, L"ISBN");
      continue;
    }
    {
      const size_t colon = c.find(L':');
      const std::wstring head = lower(colon == std::wstring::npos ? c : c.substr(0, colon));
      if (const wchar_t* lab = id_label(head)) {
        const std::wstring label = lab;
        if (colon != std::wstring::npos && c.size() - colon > 4) {
          add(Kind::Id, i, i, c.substr(colon + 1), label);
          continue;
        }
        if (i + 1 < n && count_digits(W[i + 1].text) + count_letters(W[i + 1].text) >= 4) {
          add(Kind::Id, i + 1, i + 1, core_of(W[i + 1].text), label);
          continue;
        }
      }
      if (lc.size() > 5 && lc.rfind(L"pmc", 0) == 0 && count_digits(lc) == static_cast<int>(lc.size()) - 3) {
        add(Kind::Id, i, i, c, L"PMC");
        continue;
      }
    }
    {
      // Addresses can sit inside other text: "name](https://youtube.com/...)".
      size_t at = lc.find(L"https://");
      if (at == std::wstring::npos) at = lc.find(L"http://");
      if (at == std::wstring::npos && lc.rfind(L"www.", 0) == 0) at = 0;
      if (at != std::wstring::npos) {
        std::wstring url = c.substr(at);
        while (!url.empty() && (url.back() == L')' || url.back() == L']' || url.back() == L'>')) url.pop_back();
        if (url.size() > 10) add(Kind::Url, i, i, url);
        continue;
      }
    }
    if (c.find(L'@') != std::wstring::npos) {
      const size_t at = c.find(L'@');
      if (at == 0 && c.size() >= 3 && count_letters(c) >= 2) add(Kind::Handle, i, i, c);
      else if (at > 0 && c.find(L'.', at) != std::wstring::npos) add(Kind::Email, i, i, c);
      continue;
    }
    if (is_domain(c)) {
      add(Kind::Url, i, i, c);
      continue;
    }
    if (prose && is_month(lower(core_of(w)))) {
      int a = i, b = i;
      if (i > 0 && !used[i - 1] && is_day(core_of(W[i - 1].text))) a = i - 1;
      if (i + 1 < n && is_year(core_of(W[i + 1].text))) b = i + 1;
      else if (i + 2 < n && is_day(core_of(W[i + 1].text)) && is_year(core_of(W[i + 2].text))) b = i + 2;
      if (b > i) {
        std::wstring t;
        for (int k = a; k <= b; ++k) t += (k > a ? L" " : L"") + core_of(W[k].text);
        add(Kind::Date, a, b, t);
      }
      continue;
    }
    if (prose && count_digits(c) > 0) {
      const bool pct = !w.empty() && (c.back() == L'%');
      const bool money = !c.empty() && (c[0] == L'$' || c[0] == 0x20AC || c[0] == 0xA3);
      const std::wstring next = i + 1 < n ? lower(core_of(W[i + 1].text)) : L"";
      const bool big = next == L"million" || next == L"billion" || next == L"percent" || next == L"thousand";
      if (pct || money || big) add(Kind::Number, i, big ? i + 1 : i, big ? c + L" " + core_of(W[i + 1].text) : c);
    }
  }

  // Quoted titles, the citation style in the reference clip.
  for (int a = 0; a < n; ++a) {
    if (used[a] || !opens_quote(W[a].text)) continue;
    for (int b = a; b < n && b < a + 22; ++b) {
      if (used[b]) break;
      if (closes_quote(W[b].text) && (b > a || W[a].text.size() > 3)) {
        std::wstring t;
        for (int k = a; k <= b; ++k) t += (k > a ? L" " : L"") + W[k].text;
        t = strip_quotes(t);
        if (t.size() >= 10) add(Kind::Title, a, b, t);
        break;
      }
    }
  }

  // Links, found by color: a run of words inked differently from the body.
  int links = 0;
  const int link_cap = prose ? 1 : 2;
  for (int a = 0; a < n && links < link_cap && !has_facts(); ++a) {
    if (used[a] || !is_link_color(W[a].fg)) continue;
    int b = a;
    while (b + 1 < n && !used[b + 1] && is_link_color(W[b + 1].fg) && color_dist(W[b + 1].fg, W[a].fg) < 90) ++b;
    const int run_end = b;
    // The little arrow icon after external links reads as "e" or "g.": trim
    // one-letter scraps off both ends of the run.
    int s = a;
    while (s < b && core_of(W[s].text).size() <= 1) ++s;
    while (b > s && core_of(W[b].text).size() <= 1) --b;
    std::wstring t;
    int words = 0, real = 0;
    for (int k = s; k <= b; ++k) {
      t += (k > s ? L" " : L"") + W[k].text;
      ++words;
      real += count_letters(W[k].text) >= 3 ? 1 : 0;
    }
    t = core_of(t);
    // "a b c d e f" footnote backlinks are link-colored too; they are not links.
    // One-word links are mostly buttons and channel names ("commands").
    const bool wordy = real >= 2 && real * 2 >= words;
    if (wordy && count_letters(t) >= 6) {
      add(Kind::Link, s, b, t);
      ++links;
    }
    a = run_end;
  }
}

void Page::extract_sentences(const Block& b, std::vector<Entity>& out) const {
  struct Ref {
    const OcrWord* w;
    int line;
  };
  std::vector<Ref> words;
  for (int li : b.lines)
    for (const OcrWord& w : lines_[li].words) words.push_back({&w, li});
  if (words.size() < 5) return;

  struct Cand {
    int a, b;
    float score;
    std::wstring text;
  };
  std::vector<Cand> cands;
  int start = 0, index = 0;
  for (int k = 0; k < static_cast<int>(words.size()); ++k) {
    const bool last = k + 1 == static_cast<int>(words.size());
    if (!last) {
      // Look past footnote marks ("[4][5]") to the next real word.
      int nx = k + 1;
      while (nx < static_cast<int>(words.size()) && is_ref_token(words[nx].w->text)) ++nx;
      const bool next_upper = nx < static_cast<int>(words.size()) && starts_upper(words[nx].w->text);
      if (!(next_upper && (ends_sentence(words[k].w->text) || ocr_period(words[k].w->text)))) continue;
    }
    std::wstring t;
    for (int i = start; i <= k; ++i) {
      const std::wstring& wt = words[i].w->text;
      if (!t.empty()) {
        // Join "spi-" + "ders" across a line break.
        if (t.back() == L'-' && words[i].line != words[i - 1].line && !wt.empty() && std::iswlower(wt[0])) t.pop_back();
        else t += L' ';
      }
      t += wt;
    }
    t = strip_refs(t);
    // The reader often sees the final period as a dash.
    if (t.size() > 2 && t.back() == L'-' && std::iswlower(t[t.size() - 2])) t.back() = L'.';
    const int len = static_cast<int>(t.size());
    if (len >= 30 && len <= 420) {
      float s = (len >= 60 && len <= 260) ? 1.f : 0.35f;
      if (index == 0) s += 0.7f;
      const std::wstring lt = lower(t);
      int hits = 0;
      for (const std::wstring& tw : title_words_) hits += lt.find(tw) != std::wstring::npos ? 1 : 0;
      s += std::min(1.5f, 0.5f * hits);
      int topic = 0;
      for (const std::wstring& tw : topic_words_) topic += lt.find(tw) != std::wstring::npos ? 1 : 0;
      s += std::min(1.f, 0.2f * topic);
      if (count_digits(t) * 100 > len * 12) s -= 1.f;
      static const wchar_t* weak[] = {L"it ", L"this ", L"these ", L"they ", L"he ", L"she ", L"that ", L"such "};
      for (const wchar_t* p : weak)
        if (lt.rfind(p, 0) == 0) s -= 0.3f;
      if (lt.find(L" is a ") != std::wstring::npos || lt.find(L" are ") != std::wstring::npos ||
          lt.find(L" was a ") != std::wstring::npos)
        s += 0.35f;
      if (t.back() == L':') s -= 0.6f;
      // Reference-list entries are data to harvest, not prose to read.
      const wchar_t first = t.empty() ? 0 : t[0];
      if (first == L'"' || first == 0x201C || first == L'^') s = -10.f;
      for (const wchar_t* cite : {L"retrieved ", L"archived ", L"(pdf)", L"doi:", L"isbn", L"pmid", L"et al.", L"http", L"www."})
        if (lt.find(cite) != std::wstring::npos) s = -10.f;
      int caps = 0, ws = 0;
      for (int i = start; i <= k; ++i) {
        ++ws;
        caps += starts_upper(words[i].w->text) ? 1 : 0;
      }
      if (ws > 4 && caps * 2 > ws) s -= 0.6f;
      // Figure keys like "1 pedipalp 2. trichobothria 3. carapace" are lists, not prose.
      int numbered = 0;
      std::set<int> spans;
      for (int i = start; i <= k; ++i) {
        std::wstring c = core_of(words[i].w->text);
        while (!c.empty() && (c.back() == L'-' || c.back() == L'.')) c.pop_back();
        if (!c.empty() && count_digits(c) == static_cast<int>(c.size()) && c.size() <= 2) ++numbered;
        spans.insert(words[i].line);
      }
      if (numbered >= 3 || spans.size() > 5) s = -10.f;
      cands.push_back({start, k, s, t});
    }
    start = k + 1;
    ++index;
  }
  if (cands.empty()) return;

  int chars = 0;
  for (int li : b.lines) chars += static_cast<int>(lines_[li].text.size());
  const size_t keep = chars > 1400 ? 3 : chars > 700 ? 2 : 1;
  std::vector<Cand> ranked = cands;
  std::sort(ranked.begin(), ranked.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
  for (size_t r = 0; r < ranked.size() && r < keep; ++r) {
    const Cand& c = ranked[r];
    if (c.score < 0.9f) break;
    Entity e;
    e.kind = Kind::Sentence;
    e.text = c.text;
    std::map<int, Rect> per_line;
    for (int i = c.a; i <= c.b; ++i) per_line[words[i].line] = unite(per_line[words[i].line], words[i].w->box);
    for (auto& [li, r] : per_line) {
      e.lines.push_back(r);
      e.box = unite(e.box, r);
    }
    e.font_px = line_font_px(lines_[b.lines.front()].box);
    e.fg = ink_;
    e.bg = paper_;
    e.score = c.score;
    e.key = c.score >= 2.2f;
    out.push_back(std::move(e));
  }
}

void Page::reconcile(std::vector<Entity>& found, float y0, float y1, const std::function<bool(const Rect&)>& visible) {
  std::unordered_set<int> matched;
  for (Entity& c : found) {
    Entity* best = nullptr;
    for (Entity& e : entities_) {
      if (e.kind != c.kind || matched.count(e.id)) continue;
      const Rect& a = e.lines.empty() ? e.box : e.lines.front();
      const Rect& b = c.lines.empty() ? c.box : c.lines.front();
      const bool same_text = e.text == c.text;
      if (iou(a, b) > 0.3f || (same_text && distance(a.center(), b.center()) < 2.f * line_h_)) {
        best = &e;
        break;
      }
    }
    if (best) {
      matched.insert(best->id);
      best->misses = 0;
      best->lines = c.lines;
      best->box = c.box;
      if (best->mark != Mark::Done) {
        best->text = c.text;
        best->label = c.label;
        best->href = c.href;
        best->score = c.score;
        best->key = c.key;
        best->font_px = c.font_px;
        best->fg = c.fg;
        best->bg = c.bg;
      }
      continue;
    }
    c.id = next_id_++;
    c.variant = static_cast<int>(fnv1a(c.text) % 3);
    matched.insert(c.id);
    entities_.push_back(std::move(c));
  }
  std::erase_if(entities_, [&](Entity& e) {
    if (matched.count(e.id)) return false;
    const float cy = e.box.center().y;
    if (cy < y0 || cy > y1) return false;
    if (visible && !visible(e.box)) return false;
    ++e.misses;
    return e.misses >= (e.mark == Mark::Done ? 2 : 1);
  });
}

bool Page::snap(Vec2 p, float radius, Vec2& out) const {
  if (lines_.empty()) return false;
  const float reach = radius + line_h_ * 3.f;
  auto it = std::lower_bound(lines_.begin(), lines_.end(), p.y - reach,
                             [](const TextLine& l, float y) { return l.box.y < y; });
  float best = radius;
  bool hit = false;
  for (; it != lines_.end() && it->box.y <= p.y + reach; ++it) {
    if (p.x < it->box.x - radius || p.x > it->box.right() + radius) continue;
    for (const OcrWord& w : it->words) {
      const Vec2 q{std::clamp(p.x, w.box.x, w.box.right()), w.box.center().y};
      const float d = distance(p, q);
      if (d < best) {
        best = d;
        out = q;
        hit = true;
      }
    }
  }
  return hit;
}

std::wstring Page::sample(size_t max_chars) const {
  std::wstring out;
  for (const Block& b : blocks_) {
    if (!b.prose) continue;
    for (int i : b.lines) {
      if (!out.empty()) out += L' ';
      out += lines_[i].text;
      if (out.size() >= max_chars) return strip_refs(out.substr(0, max_chars));
    }
    out += L'\n';
  }
  return strip_refs(out);
}

}  // namespace sp
