#pragma once

#include "see/frame.hpp"

#include <windows.h>

#include <memory>
#include <vector>

namespace sp {

// The exact words a window shows, with the exact box of every word, read
// from the text the app publishes for screen readers (UI Automation's text
// pattern). Browsers, Electron apps, Word and most editors publish it; when
// an app does not, read() fails and the caller falls back to OCR.
// Call from one thread that has joined the multithreaded apartment.
class PageText {
 public:
  struct Stats {
    int words = 0;
    int calls = 0;
    double took = 0;
    float cover = 0;  // how much of the asked-for area the text document covers
  };

  PageText();
  ~PageText();
  // Lines and words in screen pixels, top to bottom, only the parts inside
  // `area`. fg/bg are left for the caller to measure from pixels.
  bool read(HWND window, const RECT& area, std::vector<OcrLine>& out, Stats* stats = nullptr);
  void forget();  // the window changed what it shows (new page, new tab)

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Turns screen-pixel lines from PageText into a reader pass over frame `f`
// (which shows `area`): frame coordinates, colors measured from the pixels,
// and only words that are actually drawn there.
bool exact_pass(const std::vector<OcrLine>& lines, const RECT& area, const Frame& f, OcrPass& out, int* dropped = nullptr);

}  // namespace sp
