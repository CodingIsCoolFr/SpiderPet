#pragma once

#include "core/geom.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sp {

// One captured image of the watched area, BGRA, top-down.
struct Frame {
  int w = 0;
  int h = 0;
  std::vector<uint32_t> px;
  double time = 0;
  double scroll = 0;  // content offset when this frame was taken
  std::vector<uint8_t> fixed;  // 1 for rows that do not scroll (toolbars, sticky headers)
  uint32_t at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

inline float luma(uint32_t bgra) {
  const float b = (bgra & 0xFF) / 255.f, g = ((bgra >> 8) & 0xFF) / 255.f, r = ((bgra >> 16) & 0xFF) / 255.f;
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

struct OcrWord {
  std::wstring text;
  Rect box;
  uint32_t fg = 0xFFFFFF;  // 0xRRGGBB
  uint32_t bg = 0x000000;
};

struct OcrLine {
  std::wstring text;
  Rect box;
  std::vector<OcrWord> words;
  uint32_t fg = 0xFFFFFF;
  uint32_t bg = 0x000000;
};

// Everything the reader saw in one frame. Boxes are in frame pixels; add
// `scroll` to y to get content coordinates.
struct OcrPass {
  std::vector<OcrLine> lines;
  int w = 0;
  int h = 0;
  double scroll = 0;
  double time = 0;
  double took = 0;
  uint64_t generation = 0;  // target generation the frame belongs to
  bool exact = false;       // the app's own text, not OCR guesses
  std::vector<uint8_t> fixed;
  std::shared_ptr<const Frame> image;  // the pixels that were read
};

}  // namespace sp
