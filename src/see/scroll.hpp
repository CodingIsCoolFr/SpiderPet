#pragma once

#include "see/frame.hpp"

#include <vector>

namespace sp {

// Finds how far the content moved vertically between two captures, by
// matching per-row ink profiles in a few vertical bands. This is what keeps
// feet and restyled words glued to text while the page scrolls.
class ScrollTracker {
 public:
  struct Result {
    int shift = 0;        // content moved up by this many pixels (scrolling down)
    bool moved = false;   // a clean shift was found
    bool changed = false; // pixels changed but not as a scroll
    float cost = 0;
  };

  Result feed(const Frame& f) { return feed(f.px.data(), f.w, f.h, f.w); }
  Result feed(const uint32_t* px, int w, int h, int stride);
  void reset() {
    has_prev_ = false;
    static_.clear();
  }
  // Per row, 0..1: how sure we are the row stays put while the page scrolls
  // (toolbars, sticky headers). Learned from real scrolls.
  const std::vector<float>& statics() const { return static_; }

 private:
  static constexpr int kBands = 64;   // fine profile
  static constexpr int kCoarse = 16;  // 4 fine bands and 4 rows per coarse cell
  void profile(const uint32_t* px, int w, int h, int stride, std::vector<float>& out) const;
  static float cost(const std::vector<float>& a, const std::vector<float>& b, int rows, int bands, int shift, float cap);

  std::vector<float> prev_, cur_, cur_c_;
  std::vector<float> prev_phase_[4];  // old coarse profile at row offsets 0..3
  int w_ = 0, h_ = 0;
  bool has_prev_ = false;
  int last_shift_ = 0;
  std::vector<float> static_;
  void learn_static(int shift, float cap);
};

}  // namespace sp
