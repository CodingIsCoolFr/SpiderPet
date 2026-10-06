#pragma once

// A picture of a window, for the model's own eyes, and text read from it
// (Windows' built-in OCR) when an app shows text the screen-reader
// interface can't see: games, videos, pictures, canvas apps.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace sp::eyes {

struct Pixels {
  int w = 0, h = 0;
  std::vector<uint8_t> bgra;  // top-down, 4 bytes a pixel
  RECT on_screen{};
  bool ok() const { return w > 0 && h > 0 && bgra.size() == size_t(w) * h * 4; }
};

// The window as it is on screen (or as it draws itself, when it is in the
// back). The spider steps out of the picture first (see set_grab_guard).
Pixels grab(HWND h, const RECT* region = nullptr);
// Called around a screen capture: true just before, false right after. The
// stage uses it to step out of the picture, so the spider never reads itself.
void set_grab_guard(void (*guard)(bool hide));
// JPEG, longest side at most max_side, base64 (what Ollama takes as an image).
std::string jpeg_base64(const Pixels& px, int max_side = 1280, int quality = 82);

struct OcrLine {
  std::string text;
  RECT r{};  // screen pixels
  struct Word {
    std::string text;
    RECT r{};  // screen pixels
  };
  std::vector<Word> words;
};
// Lines of text in the picture, in reading order. Empty when Windows has no
// OCR language installed. Call from a worker thread (it waits for the result).
std::vector<OcrLine> read_pixels(const Pixels& px);

}  // namespace sp::eyes
