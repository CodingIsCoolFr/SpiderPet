#pragma once

#include "see/frame.hpp"

#include <cstdint>
#include <vector>

namespace sp {

// The letters of one piece of text, lifted out of a captured frame: how much
// of each pixel is ink, and how much clean paper lies around them. The
// overlay repaints these exact shapes in a new look, so a restyled word keeps
// its place and size and never spills onto what sits next to it.
struct Glyphs {
  Rect at;                  // the cut, whole pixels (frame or content coordinates)
  int w = 0;
  int h = 0;
  std::vector<uint8_t> a;   // ink coverage 0..255, row by row
  uint32_t fg = 0;          // 0xRRGGBB
  uint32_t bg = 0;
  float room_l = 0, room_r = 0, room_t = 0, room_b = 0;  // clean paper on each side, px
  uint32_t stamp = 0;       // changes whenever the coverage does
  bool chip = false;        // the letters sit on their own patch (a button, a tag)
  float stray = 0;          // share of the ink outside the reader's words (icons)
  bool literal = false;     // the reader's words here spell the find's own text
  bool ok() const { return w > 0 && h > 0 && a.size() == static_cast<size_t>(w) * h; }
};

// box is in frame pixels. Fails on busy backgrounds (photos, gradients) and
// where ink and paper are too close to tell apart.
bool cut_glyphs(const Frame& f, const Rect& box, Glyphs& out);

}  // namespace sp
