#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace sp {

// Windows.Graphics.Capture of one window (what OBS and Discord use for window
// capture). It returns only that window's own pixels, even where other windows
// cover it, and never our overlay. That lets the spider stay visible to screen
// sharing without the spider ever reading itself. Use from one MTA thread.
class WindowCapture {
 public:
  WindowCapture();
  ~WindowCapture();

  bool start(HWND hwnd);
  void stop();
  bool active() const;
  // Newest frame cropped to `region` (pixels relative to the window's visible
  // frame, see DWMWA_EXTENDED_FRAME_BOUNDS). False when nothing new arrived.
  bool grab(const RECT& region, std::vector<uint32_t>& out, int& w, int& h);
  // Ask Windows once to skip the yellow capture border. True when allowed.
  static bool allow_borderless();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sp
