#include "see/glyphs.hpp"

#include <algorithm>
#include <cmath>

namespace sp {
namespace {

struct Rgb {
  float r = 0, g = 0, b = 0;
};

Rgb split(uint32_t c) {
  return {static_cast<float>((c >> 16) & 0xFF), static_cast<float>((c >> 8) & 0xFF), static_cast<float>(c & 0xFF)};
}

uint32_t pack(Rgb c) {
  auto ch = [](float v) { return static_cast<uint32_t>(std::clamp(v + 0.5f, 0.f, 255.f)); };
  return ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b);
}

float dist(Rgb a, Rgb b) {
  const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return std::sqrt(dr * dr + dg * dg + db * db);
}

}  // namespace

bool cut_glyphs(const Frame& f, const Rect& box, Glyphs& out) {
  if (f.w < 8 || f.h < 8 || f.px.size() < static_cast<size_t>(f.w) * f.h) return false;
  int x0 = std::clamp(static_cast<int>(std::floor(box.x)), 0, f.w);
  int y0 = std::clamp(static_cast<int>(std::floor(box.y)), 0, f.h);
  int x1 = std::clamp(static_cast<int>(std::ceil(box.right())), 0, f.w);  // exclusive
  int y1 = std::clamp(static_cast<int>(std::ceil(box.bottom())), 0, f.h);
  if (x1 - x0 < 2 || y1 - y0 < 3) return false;
  const int box_h = y1 - y0, box_w = x1 - x0;
  auto px = [&](int x, int y) { return split(f.at(x, y)); };

  // Paper: the middle color of a ring a little outside the box. A photo or a
  // gradient has no single paper color, and then nothing is lifted.
  std::vector<Rgb> ring;
  auto take = [&](int x, int y) {
    if (x >= 0 && y >= 0 && x < f.w && y < f.h) ring.push_back(px(x, y));
  };
  for (int x = x0 - 3; x < x1 + 3; ++x) {
    take(x, y0 - 3), take(x, y0 - 2), take(x, y1 + 1), take(x, y1 + 2);
  }
  for (int y = y0; y < y1; ++y) {
    take(x0 - 3, y), take(x0 - 2, y), take(x1 + 1, y), take(x1 + 2, y);
  }
  if (ring.size() < 12) return false;
  auto middle = [&](float Rgb::*ch) {
    std::vector<float> v;
    v.reserve(ring.size());
    for (const Rgb& c : ring) v.push_back(c.*ch);
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
  };
  const Rgb paper{middle(&Rgb::r), middle(&Rgb::g), middle(&Rgb::b)};
  int calm = 0;
  for (const Rgb& c : ring) calm += dist(c, paper) < 24.f ? 1 : 0;
  if (calm * 10 < static_cast<int>(ring.size()) * 6) return false;

  // Ink: the average of the pixels inside that stand out most from the paper.
  float far = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) far = std::max(far, dist(px(x, y), paper));
  if (far < 48.f) return false;
  Rgb ink;
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const Rgb c = px(x, y);
      if (dist(c, paper) < 0.7f * far) continue;
      ink.r += c.r, ink.g += c.g, ink.b += c.b;
      ++n;
    }
  ink.r /= static_cast<float>(n), ink.g /= static_cast<float>(n), ink.b /= static_cast<float>(n);
  const float contrast = dist(ink, paper);
  if (contrast < 40.f) return false;
  const float strong = std::max(22.f, 0.25f * contrast);  // surely ink
  const float faint = std::max(10.f, 0.08f * contrast);   // the soft edge of a letter
  const float clean = std::max(16.f, 0.15f * contrast);   // nothing anyone would see

  auto row_ink = [&](int y, float thr) {
    for (int x = x0; x < x1; ++x)
      if (dist(px(x, y), paper) > thr) return true;
    return false;
  };
  auto col_ink = [&](int x, float thr) {
    for (int y = y0; y < y1; ++y)
      if (dist(px(x, y), paper) > thr) return true;
    return false;
  };

  // Grow to whole letters: reader boxes can clip ascenders, descenders and
  // the soft edges of glyphs.
  const int grow_v = std::max(2, static_cast<int>(box_h * 0.35f));
  const int grow_h = std::max(2, static_cast<int>(box_h * 0.3f));
  for (int i = 0; i < grow_v && y0 > 0 && row_ink(y0 - 1, strong); ++i) --y0;
  for (int i = 0; i < grow_v && y1 < f.h && row_ink(y1, strong); ++i) ++y1;
  for (int i = 0; i < grow_h && x0 > 0 && col_ink(x0 - 1, strong); ++i) --x0;
  for (int i = 0; i < grow_h && x1 < f.w && col_ink(x1, strong); ++i) ++x1;
  if (y0 > 0 && row_ink(y0 - 1, faint)) --y0;
  if (y1 < f.h && row_ink(y1, faint)) ++y1;
  if (x0 > 0 && col_ink(x0 - 1, faint)) --x0;
  if (x1 < f.w && col_ink(x1, faint)) ++x1;

  // Clean paper on each side, as far as a restyle could ever use. The frame
  // edge counts as room: the overlay is clipped there anyway.
  const int far_x = std::max(3 * box_h, box_w * 3 / 5), far_y = box_h;
  int rl = 0, rr = 0, rt = 0, rb = 0;
  while (rl < far_x && x0 - rl - 1 >= 0 && !col_ink(x0 - rl - 1, clean)) ++rl;
  while (rr < far_x && x1 + rr < f.w && !col_ink(x1 + rr, clean)) ++rr;
  while (rt < far_y && y0 - rt - 1 >= 0 && !row_ink(y0 - rt - 1, clean)) ++rt;
  while (rb < far_y && y1 + rb < f.h && !row_ink(y1 + rb, clean)) ++rb;

  // A button or a tag has its own patch of color under the letters. It counts
  // as paper too, so it is kept, not painted over.
  Rgb patch = paper;
  bool chip = false;
  {
    std::vector<Rgb> mid;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const Rgb c = px(x, y);
        if (dist(c, paper) > clean && dist(c, ink) > 0.45f * contrast) mid.push_back(c);
      }
    if (mid.size() * 5 >= static_cast<size_t>(x1 - x0) * (y1 - y0)) {
      auto med = [&](float Rgb::*ch) {
        std::vector<float> v;
        v.reserve(mid.size());
        for (const Rgb& c : mid) v.push_back(c.*ch);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
      };
      patch = {med(&Rgb::r), med(&Rgb::g), med(&Rgb::b)};
      chip = dist(ink, patch) >= 40.f;
      if (!chip) patch = paper;
    }
  }
  const float patch_contrast = dist(ink, patch);

  // Coverage: how far each pixel is from the nearer paper, against the ink.
  // This also keeps an icon inside the box whole, just in the new color.
  out.w = x1 - x0;
  out.h = y1 - y0;
  out.at = {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(out.w), static_cast<float>(out.h)};
  out.a.resize(static_cast<size_t>(out.w) * out.h);
  uint32_t hash = 2166136261u;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const Rgb c = px(x, y);
      const float dp = dist(c, paper), dc = chip ? dist(c, patch) : dp;
      const float d = std::min(dp, dc), full = dc < dp ? patch_contrast : contrast;
      const float t = std::clamp((d - 0.5f * faint) / std::max(1.f, full - 0.5f * faint), 0.f, 1.f);
      const uint8_t v = static_cast<uint8_t>(t * 255.f + 0.5f);
      out.a[static_cast<size_t>(y - y0) * out.w + (x - x0)] = v;
      hash = (hash ^ v) * 16777619u;
    }
  out.fg = pack(ink);
  out.bg = pack(paper);
  out.chip = chip;
  out.room_l = static_cast<float>(rl);
  out.room_r = static_cast<float>(rr);
  out.room_t = static_cast<float>(rt);
  out.room_b = static_cast<float>(rb);
  out.stamp = hash ^ static_cast<uint32_t>(out.w * 73856093) ^ static_cast<uint32_t>(out.h * 19349663);
  return true;
}

}  // namespace sp
