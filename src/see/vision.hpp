#pragma once

#include "see/frame.hpp"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace sp {

// Watches one rectangle of one window. A capture thread grabs it ~90 times a
// second and tracks scrolling; a reader thread runs OCR when the view settles.
// Frames come from Windows.Graphics.Capture of that window when possible, so
// they never contain our overlay or windows lying on top.
class Vision {
 public:
  struct State {
    double scroll = 0;       // accumulated content offset, pixels
    double last_motion = 0;  // last time a scroll was seen
    double last_change = 0;  // last time pixels changed without scrolling
    uint64_t generation = 0;
    bool window_capture = false;  // frames are the window's own pixels
    double capture_ms = 0;
    double ocr_ms = 0;
  };

  void start();
  void stop();
  uint64_t watch(HWND window, const RECT& screen_rect);  // new generation; null window stops
  void move(const RECT& screen_rect);                    // same content, window moved
  void request_read();
  State state() const;
  bool take(OcrPass& out);
  bool reader_ready() const { return reader_ready_; }

 private:
  void capture_loop();
  void read_loop();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  HWND window_ = nullptr;
  RECT rect_{};
  uint64_t gen_ = 0;
  State st_;
  bool want_snapshot_ = false;
  bool have_snapshot_ = false;
  Frame snapshot_;
  uint64_t snapshot_gen_ = 0;
  bool read_requested_ = false;
  double last_read_ = -100;
  std::deque<OcrPass> passes_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> reader_ready_{false};
  std::thread capture_, reader_;
};

}  // namespace sp
