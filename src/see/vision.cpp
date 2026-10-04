#include "see/vision.hpp"

#include "core/util.hpp"
#include "see/ocr.hpp"
#include "see/scroll.hpp"
#include "see/window_capture.hpp"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace sp {

void Vision::start() {
  quit_ = false;
  capture_ = std::thread([this] { capture_loop(); });
  reader_ = std::thread([this] { read_loop(); });
}

void Vision::stop() {
  quit_ = true;
  cv_.notify_all();
  if (capture_.joinable()) capture_.join();
  if (reader_.joinable()) reader_.join();
}

uint64_t Vision::watch(HWND window, const RECT& r) {
  std::lock_guard lock(mu_);
  window_ = window;
  rect_ = window ? r : RECT{};
  ++gen_;
  st_ = {};
  st_.generation = gen_;
  st_.window_capture = true;  // until the capture thread finds out otherwise
  passes_.clear();
  read_requested_ = true;
  last_read_ = -100;
  return gen_;
}

void Vision::move(const RECT& r) {
  std::lock_guard lock(mu_);
  rect_ = r;
}

void Vision::request_read() {
  std::lock_guard lock(mu_);
  read_requested_ = true;
}

Vision::State Vision::state() const {
  std::lock_guard lock(mu_);
  return st_;
}

bool Vision::take(OcrPass& out) {
  std::lock_guard lock(mu_);
  if (passes_.empty()) return false;
  out = std::move(passes_.front());
  passes_.pop_front();
  return out.generation == gen_;
}

namespace {

// Fallback when window capture is not available: copy the screen. The overlay
// must then be hidden from capture, or the spider would read itself.
class ScreenCopy {
 public:
  ~ScreenCopy() {
    if (dib_) {
      SelectObject(mem_, old_);
      DeleteObject(dib_);
    }
    if (mem_) DeleteDC(mem_);
    if (screen_) ReleaseDC(nullptr, screen_);
  }
  const uint32_t* grab(const RECT& r) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (!screen_) {
      screen_ = GetDC(nullptr);
      mem_ = CreateCompatibleDC(screen_);
    }
    if (w != w_ || h != h_) {
      if (dib_) {
        SelectObject(mem_, old_);
        DeleteObject(dib_);
      }
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      void* p = nullptr;
      dib_ = CreateDIBSection(screen_, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
      bits_ = static_cast<uint32_t*>(p);
      old_ = SelectObject(mem_, dib_);
      w_ = w;
      h_ = h;
    }
    if (!dib_ || !bits_) return nullptr;
    BitBlt(mem_, 0, 0, w, h, screen_, r.left, r.top, SRCCOPY);
    GdiFlush();
    return bits_;
  }

 private:
  HDC screen_ = nullptr, mem_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ old_ = nullptr;
  uint32_t* bits_ = nullptr;
  int w_ = 0, h_ = 0;
};

}  // namespace

void Vision::capture_loop() {
  join_mta();
  WindowCapture::allow_borderless();
  WindowCapture cap;
  ScreenCopy screen;
  HWND cap_window = nullptr;
  bool cap_ok = false;
  uint64_t seen_gen = 0;
  ScrollTracker tracker;
  std::vector<uint32_t> buf, last;
  int last_w = 0, last_h = 0;
  double last_t = 0;

  while (!quit_) {
    const double t0 = now_seconds();
    RECT r;
    HWND window;
    uint64_t gen;
    {
      std::lock_guard lock(mu_);
      r = rect_;
      window = window_;
      gen = gen_;
    }
    if (!window || r.right - r.left < 64 || r.bottom - r.top < 64) {
      if (cap_window) {
        cap.stop();
        cap_window = nullptr;
        cap_ok = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      continue;
    }
    if (gen != seen_gen) {
      tracker.reset();
      seen_gen = gen;
      last.clear();
    }
    if (window != cap_window) {
      cap_ok = cap.start(window);
      cap_window = window;
      debug_log(std::string("capture: ") + (cap_ok ? "window capture" : "screen copy fallback"));
    }

    const uint32_t* px = nullptr;
    int w = 0, h = 0;
    if (cap_ok) {
      // Window capture frames cover the window's visible frame.
      RECT frame{};
      if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        GetWindowRect(window, &frame);
      const RECT region{r.left - frame.left, r.top - frame.top, r.right - frame.left, r.bottom - frame.top};
      if (cap.grab(region, buf, w, h)) px = buf.data();
    } else {
      px = screen.grab(r);
      w = r.right - r.left;
      h = r.bottom - r.top;
    }

    const double t = now_seconds();
    ScrollTracker::Result res;
    if (px) {
      res = tracker.feed(px, w, h, w);
      last.assign(px, px + static_cast<size_t>(w) * h);
      last_w = w;
      last_h = h;
      last_t = t;
    }
    {
      std::lock_guard lock(mu_);
      if (gen == gen_) {
        st_.window_capture = cap_ok;
        if (res.moved) {
          st_.scroll += res.shift;
          st_.last_motion = t;
        }
        if (res.changed) st_.last_change = t;
        if (px) st_.capture_ms = lerp(static_cast<float>(st_.capture_ms), static_cast<float>((t - t0) * 1000.0), 0.1f);
        // A static window sends no new frames: answer from the last one.
        if (want_snapshot_ && !last.empty()) {
          snapshot_.w = last_w;
          snapshot_.h = last_h;
          snapshot_.px = last;
          snapshot_.scroll = st_.scroll;
          snapshot_.time = last_t;
          const auto& st = tracker.statics();
          snapshot_.fixed.assign(static_cast<size_t>(last_h), 0);
          for (size_t y = 0; y < st.size() && y < snapshot_.fixed.size(); ++y) snapshot_.fixed[y] = st[y] > 0.6f ? 1 : 0;
          snapshot_gen_ = gen;
          want_snapshot_ = false;
          have_snapshot_ = true;
          cv_.notify_all();
        }
      }
    }
    // About 90 looks a second is plenty to follow a smooth scroll; with
    // window capture, a quiet window costs almost nothing.
    const double spent = now_seconds() - t0;
    const double pace = (cap_ok && !px) ? 0.004 : 0.011;
    if (spent < pace) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int>((pace - spent) * 1e6)));
  }
  cap.stop();
}

void Vision::read_loop() {
  join_mta();
  TextReader reader;
  reader_ready_ = reader.ready();
  if (!reader.ready()) debug_log("ocr engine unavailable");
  while (!quit_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const double t = now_seconds();
    {
      std::unique_lock lock(mu_);
      if (!window_ || rect_.right - rect_.left < 64 || rect_.bottom - rect_.top < 64) continue;
      const bool stale = st_.last_motion > last_read_ || st_.last_change > last_read_;
      const bool due = read_requested_ || (stale && t - last_read_ > 0.6) || t - last_read_ > 3.0;
      const bool settled = t - st_.last_motion > 0.15;
      if (!due || !settled) continue;
      have_snapshot_ = false;
      want_snapshot_ = true;
      cv_.wait_for(lock, std::chrono::milliseconds(250), [&] { return have_snapshot_ || quit_.load(); });
      if (!have_snapshot_) continue;
      read_requested_ = false;
      last_read_ = t;
    }
    Frame f;
    uint64_t gen;
    {
      std::lock_guard lock(mu_);
      f = std::move(snapshot_);
      gen = snapshot_gen_;
      have_snapshot_ = false;
    }
    OcrPass pass;
    if (!reader.read(f, pass)) continue;
    pass.generation = gen;
    pass.fixed = std::move(f.fixed);
    std::lock_guard lock(mu_);
    st_.ocr_ms = pass.took * 1000.0;
    if (gen != gen_) continue;
    passes_.push_back(std::move(pass));
    while (passes_.size() > 2) passes_.pop_front();
  }
}

}  // namespace sp
