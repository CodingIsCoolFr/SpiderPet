#include "see/scroll.hpp"

#include <algorithm>
#include <cmath>

namespace sp {
namespace {

inline int luma8(uint32_t c) {
  return static_cast<int>((((c >> 16) & 0xFF) * 54 + ((c >> 8) & 0xFF) * 183 + (c & 0xFF) * 19) >> 8);
}

}  // namespace

// Per row and band: horizontal gradient energy (text has lots, flat paper has
// none) plus a little brightness so solid blocks still register. Narrow bands
// see individual words, which is what tells one text line from the next.
void ScrollTracker::profile(const uint32_t* px, int w, int h, int stride, std::vector<float>& out) const {
  out.assign(static_cast<size_t>(h) * kBands, 0.f);
  const int band_w = std::max(4, w / kBands);
  for (int y = 0; y < h; ++y) {
    const uint32_t* row = px + static_cast<size_t>(y) * stride;
    for (int b = 0; b < kBands; ++b) {
      const int x0 = b * band_w + 1;
      const int x1 = std::min(w, x0 + band_w - 1);
      int grad = 0, lum = 0, n = 0;
      int prev = luma8(row[x0 - 1]);
      for (int x = x0; x < x1; ++x) {
        const int l = luma8(row[x]);
        grad += std::abs(l - prev);
        lum += l;
        prev = l;
        ++n;
      }
      out[static_cast<size_t>(y) * kBands + b] = n ? (grad * 2.f + lum * 0.25f) / n : 0.f;
    }
  }
}

float ScrollTracker::cost(const std::vector<float>& a, const std::vector<float>& b, int rows, int bands, int shift,
                          float cap) {
  // a = new, b = old. New row y shows what old row y + shift showed.
  const int y0 = std::max(0, -shift);
  const int y1 = std::min(rows, rows - shift);
  if (y1 - y0 < rows * 3 / 10) return 1e9f;
  float sum = 0;
  for (int y = y0; y < y1; ++y) {
    const float* pa = &a[static_cast<size_t>(y) * bands];
    const float* pb = &b[static_cast<size_t>(y + shift) * bands];
    for (int k = 0; k < bands; ++k) sum += std::min(std::fabs(pa[k] - pb[k]), cap);
  }
  return sum / static_cast<float>((y1 - y0) * bands);
}

void ScrollTracker::learn_static(int s, float cap) {
  if (static_.size() != static_cast<size_t>(h_)) static_.assign(static_cast<size_t>(h_), 0.f);
  const int y0 = std::max(0, -s), y1 = std::min(h_, h_ - s);
  for (int y = y0; y < y1; ++y) {
    const float* now = &cur_[static_cast<size_t>(y) * kBands];
    const float* same = &prev_[static_cast<size_t>(y) * kBands];
    const float* moved = &prev_[static_cast<size_t>(y + s) * kBands];
    float e0 = 0, es = 0;
    for (int k = 0; k < kBands; ++k) {
      e0 += std::min(std::fabs(now[k] - same[k]), cap);
      es += std::min(std::fabs(now[k] - moved[k]), cap);
    }
    if (e0 + es < kBands * 0.5f) continue;  // flat row: says nothing
    if (e0 < es * 0.5f) static_[y] = static_[y] * 0.6f + 0.4f;
    else if (es < e0 * 0.5f) static_[y] *= 0.6f;
  }
}

ScrollTracker::Result ScrollTracker::feed(const uint32_t* px, int w, int h, int stride) {
  Result r;
  if (w < 64 || h < 32) return r;
  if (w != w_ || h != h_) {
    w_ = w;
    h_ = h;
    has_prev_ = false;
  }
  profile(px, w, h, stride, cur_);
  // Coarse cells average 4 rows and 4 bands. `phase` starts the grouping at
  // row 0..3, so a shift that is not a multiple of 4 still lines up.
  auto coarse = [&](const std::vector<float>& fine, int phase, std::vector<float>& out) {
    const int rows = (h_ - phase) / 4;
    out.assign(static_cast<size_t>(rows) * kCoarse, 0.f);
    for (int y = 0; y < rows * 4; ++y)
      for (int k = 0; k < kBands; ++k)
        out[static_cast<size_t>(y / 4) * kCoarse + k / 4] +=
            fine[static_cast<size_t>(y + phase) * kBands + k] * (1.f / 16.f);
  };
  const int hc = h_ / 4;
  coarse(cur_, 0, cur_c_);

  auto finish = [&] {
    std::swap(prev_, cur_);
    for (int p = 0; p < 4; ++p) coarse(prev_, p, prev_phase_[p]);
    return r;
  };
  if (!has_prev_) {
    has_prev_ = true;
    return finish();
  }

  float energy = 0;
  for (float v : cur_c_) energy += v;
  energy /= std::max<size_t>(1, cur_c_.size());
  const float cap = std::max(8.f, energy * 2.f);

  const float c0 = cost(cur_, prev_, h_, kBands, 0, cap);
  if (c0 < 0.05f) return finish();

  // Coarse pass: keep the few best local minima, not just the single best,
  // because evenly spaced text lines make the cost curve repeat.
  const int max_c = static_cast<int>(hc * 0.6f);
  auto coarse_cost = [&](int sc, int phase) {
    const std::vector<float>& old = prev_phase_[phase];
    const int old_rows = static_cast<int>(old.size() / kCoarse);
    const int y0 = std::max(0, -sc);
    const int y1 = std::min(hc, old_rows - sc);
    if (y1 - y0 < hc * 3 / 10) return 1e9f;
    float sum = 0;
    for (int y = y0; y < y1; ++y) {
      const float* pa = &cur_c_[static_cast<size_t>(y) * kCoarse];
      const float* pb = &old[static_cast<size_t>(y + sc) * kCoarse];
      for (int k = 0; k < kCoarse; ++k) sum += std::min(std::fabs(pa[k] - pb[k]), cap);
    }
    return sum / static_cast<float>((y1 - y0) * kCoarse);
  };
  // Pixel shift s = 4 * sc + phase. Sample every candidate, then keep the few
  // best local minima: evenly spaced text lines make the cost curve repeat.
  const int span = 8 * max_c + 4;
  std::vector<float> cc(static_cast<size_t>(span + 1), 1e9f);
  for (int sc = -max_c; sc <= max_c; ++sc)
    for (int p = 0; p < 4; ++p) {
      const int s = 4 * sc + p;
      if (s < -4 * max_c || s > 4 * max_c) continue;
      cc[static_cast<size_t>(s + 4 * max_c)] = coarse_cost(sc, p);
    }
  std::vector<std::pair<float, int>> minima;
  for (int i = 0; i <= span; ++i) {
    if (cc[i] >= 1e8f) continue;
    const bool left = i == 0 || cc[i] <= cc[i - 1];
    const bool right = i == span || cc[i] <= cc[i + 1];
    if (left && right) minima.push_back({cc[i], i - 4 * max_c});
  }
  std::sort(minima.begin(), minima.end());
  if (minima.size() > 4) minima.resize(4);
  // Also look near the last real shift: scrolling has momentum.
  minima.push_back({0.f, last_shift_});

  int best_s = 0;
  float best_f = c0;
  for (const auto& m : minima) {
    for (int s = m.second - 2; s <= m.second + 2; ++s) {
      if (s == 0) continue;
      const float c = cost(cur_, prev_, h_, kBands, s, cap);
      if (c < best_f) {
        best_f = c;
        best_s = s;
      }
    }
  }
  r.cost = best_f;
  if (best_s != 0 && best_f < c0 * 0.6f && best_f < energy * 0.45f + 0.5f) {
    r.shift = best_s;
    r.moved = true;
    last_shift_ = best_s;
    learn_static(best_s, cap);
  } else {
    last_shift_ = 0;
    if (c0 > energy * 0.08f + 0.3f) r.changed = true;
  }
  return finish();
}

}  // namespace sp
