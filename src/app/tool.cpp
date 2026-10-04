// SpiderTool: console helper for testing without touching the real desktop.
//   SpiderTool ocr <image> [y] [h]    dump what the reader sees in a slice
#include "core/util.hpp"
#include "see/image.hpp"
#include "see/ocr.hpp"
#include "page/page.hpp"
#include "pet/pet.hpp"
#include "render/offscreen.hpp"
#include "render/paint.hpp"
#include "render/scene.hpp"
#include "see/scroll.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>

using namespace sp;

static bool slice(const Frame& page, int y, int h, Frame& f);

// SpiderTool sim <image> <outdir> [seconds] [fps] [height] [throw-at]
// Films the whole pipeline over a page image: fake window, fake wheel
// scrolling, real OCR, real scroll tracking, real behavior and drawing.
// Writes PNG frames; nothing appears on screen.
static int run_sim(int argc, wchar_t** argv) {
  Frame img;
  if (!load_image(argv[2], img)) return 1;
  const std::wstring out = argv[3];
  CreateDirectoryW(out.c_str(), nullptr);
  const double seconds = argc > 4 ? _wtof(argv[4]) : 12.0;
  const int fps = argc > 5 ? _wtoi(argv[5]) : 30;
  const int h = argc > 6 ? _wtoi(argv[6]) : 800;
  const double throw_at = argc > 7 ? _wtof(argv[7]) : -1.0;
  const int w = img.w;

  Offscreen off;
  if (!off.create(w, h)) {
    std::printf("offscreen failed\n");
    return 1;
  }
  Painter painter;
  Scene scene;
  TextReader reader;
  ScrollTracker tracker;
  Page page;
  page.set_title(L"Spider - Wikipedia");
  Pet pet;
  Settings settings;
  settings.brain = false;
  pet.set_scale(1.f);

  double true_scroll = 0, est_scroll = 0, last_ocr = -10, last_motion = -10, wheel_at = -10;
  double wheel_from = 0, wheel_to = 0;
  const float dt = 1.f / static_cast<float>(fps);
  int frame_no = 0, ocr_runs = 0, drift_fixes = 0;
  double max_err = 0;
  bool summoned = false;
  FILE* track = nullptr;
  _wfopen_s(&track, (out + L"\\track.csv").c_str(), L"w");
  for (double t = 0; t < seconds; t += dt, ++frame_no) {
    // Fake browser smooth scroll: each wheel notch glides 100 px over 150 ms.
    if (t - wheel_at < 0.15) true_scroll = lerp(static_cast<float>(wheel_from), static_cast<float>(wheel_to), static_cast<float>((t - wheel_at) / 0.15));
    else true_scroll = wheel_to;
    true_scroll = std::clamp(true_scroll, 0.0, static_cast<double>(img.h - h - 1));

    Frame f;
    slice(img, static_cast<int>(true_scroll), h, f);
    const auto res = tracker.feed(f);
    if (res.moved) {
      est_scroll += res.shift;
      last_motion = t;
    }
    if (res.changed) last_motion = t;
    max_err = std::max(max_err, std::fabs(est_scroll - true_scroll));
    f.scroll = est_scroll;

    if (t - last_ocr > 0.6 && t - last_motion > 0.12) {
      OcrPass pass;
      if (reader.read(f, pass)) {
        const float drift = page.merge(pass, nullptr);
        if (drift != 0) {
          pet.shift({0, drift});
          ++drift_fixes;
        }
        ++ocr_runs;
      }
      last_ocr = t;
    }

    View view;
    view.visible = {0, static_cast<float>(est_scroll), static_cast<float>(w), static_cast<float>(h)};
    view.attached = true;
    view.crawl_ok = true;
    if (!summoned && t > 0.3) {
      pet.summon({w * 0.42f, static_cast<float>(est_scroll) + h * 0.45f}, view, t);
      summoned = true;
    }
    if (throw_at > 0 && t >= throw_at && t < throw_at + 0.7) {
      const Vec2 at = pet.spider().pos();
      if (!pet.dragging()) pet.grab(at, t);
      const float k = static_cast<float>((t - throw_at) / 0.7);
      pet.drag({at.x + 9.f * std::cos(k * 9.f) + 6.f, at.y - 4.f + 7.f * std::sin(k * 7.f)}, t);
    } else if (pet.dragging()) {
      pet.release(t);
    }
    if (!page.empty()) pet.set_scale(std::clamp(page.pitch() / 22.f, 0.7f, 1.6f) * settings.size);
    pet.update(t, dt, page, view, settings);
    if (pet.wants_scroll() > 24.f && t - wheel_at > 0.35) {
      wheel_at = t;
      wheel_from = true_scroll;
      wheel_to = true_scroll + 100.0;
    }
    pet.take_harvest();
    pet.take_asks();

    ID2D1DeviceContext* ctx = off.begin(f);
    painter.attach(ctx, off.d2d(), off.dwrite());
    scene.draw(painter, page, pet, settings, {0, static_cast<float>(-est_scroll)}, {0, 0, static_cast<float>(w), static_cast<float>(h)}, {}, t, 0.f, dt);
    if (track) {
      const Vec2 p = pet.spider().pos();
      std::fprintf(track, "%d,%.1f,%.1f,%s,%.1f\n", frame_no, p.x, p.y - est_scroll, pet.state_name(), est_scroll);
    }
    Frame shot;
    if (off.end(shot)) {
      wchar_t name[64];
      swprintf_s(name, L"\\f%05d.png", frame_no);
      save_png(out + name, shot.w, shot.h, shot.px.data(), shot.w);
    }
  }
  if (track) std::fclose(track);
  std::printf("frames=%d ocr=%d drift_fixes=%d max_scroll_err=%.0f harvested=%d entities=%zu\n", frame_no, ocr_runs,
              drift_fixes, max_err, page.harvested(), page.entities().size());
  return 0;
}

static int run_ocr(int argc, wchar_t** argv) {
  Frame page;
  if (!load_image(argv[2], page)) {
    std::printf("cannot load image\n");
    return 1;
  }
  const int y = argc > 3 ? _wtoi(argv[3]) : 0;
  const int h = argc > 4 ? _wtoi(argv[4]) : std::min(page.h, 900);
  Frame f;
  f.w = page.w;
  f.h = std::min(h, page.h - y);
  f.px.assign(page.px.begin() + static_cast<size_t>(y) * page.w, page.px.begin() + static_cast<size_t>(y + f.h) * page.w);
  TextReader reader;
  std::printf("ocr ready=%d lang=%s\n", reader.ready(), utf8(reader.language()).c_str());
  OcrPass pass;
  if (!reader.read(f, pass)) {
    std::printf("read failed\n");
    return 1;
  }
  std::printf("lines=%zu took=%.0f ms\n", pass.lines.size(), pass.took * 1000);
  for (const auto& l : pass.lines) {
    std::printf("[%4.0f,%4.0f %4.0fx%3.0f] fg=%06X bg=%06X | %s\n", l.box.x, l.box.y, l.box.w, l.box.h, l.fg, l.bg,
                utf8(l.text).c_str());
    for (const auto& w : l.words)
      if (w.fg != l.fg) std::printf("      word '%s' h=%.0f fg=%06X\n", utf8(w.text).c_str(), w.box.h, w.fg);
  }
  return 0;
}

static bool slice(const Frame& page, int y, int h, Frame& f) {
  f.w = page.w;
  f.h = std::min(h, page.h - y);
  if (f.h <= 16) return false;
  f.px.assign(page.px.begin() + static_cast<size_t>(y) * page.w, page.px.begin() + static_cast<size_t>(y + f.h) * page.w);
  f.scroll = y;
  return true;
}

// SpiderTool page <image> <title> [h] [step]: scroll a viewport down the
// image, merge every pass, and list what the spider would harvest.
static int run_page(int argc, wchar_t** argv) {
  Frame img;
  if (!load_image(argv[2], img)) return 1;
  const std::wstring title = argc > 3 ? argv[3] : L"";
  const int h = argc > 4 ? _wtoi(argv[4]) : 900;
  const int step = argc > 5 ? _wtoi(argv[5]) : 600;
  TextReader reader;
  Page page;
  page.set_title(title);
  double took = 0;
  int passes = 0;
  for (int y = 0; y + 100 < img.h; y += step) {
    Frame f;
    if (!slice(img, y, h, f)) break;
    OcrPass pass;
    if (!reader.read(f, pass)) continue;
    took += pass.took;
    ++passes;
    page.merge(pass, nullptr);
  }
  std::printf("passes=%d avg=%.0f ms lines=%zu line_h=%.1f dark=%d column_x=%.0f\n", passes,
              took * 1000 / std::max(1, passes), page.lines().size(), page.line_height(), page.dark(), page.column_x());
  std::map<std::string, int> counts;
  for (const Entity& e : page.entities()) counts[kind_name(e.kind)]++;
  for (auto& [k, n] : counts) std::printf("  %-9s %d\n", k.c_str(), n);
  for (const Entity& e : page.entities()) {
    if (e.kind == Kind::Link) continue;
    const std::string label = e.label.empty() ? "" : utf8(e.label) + " ";
    const std::string extra = e.lines.size() > 1 ? " (" + std::to_string(e.lines.size()) + " lines)" : "";
    std::printf("%-8s %5.0f %5.2f%s %s%s%s\n", kind_name(e.kind), e.box.y, e.score, e.key ? "*" : " ", label.c_str(),
                utf8(e.text).substr(0, 150).c_str(), extra.c_str());
  }
  if (const char* dbg = std::getenv("PAGE_LINES")) {  // "y0,y1": dump model lines in that band
    const float y0 = static_cast<float>(std::atof(dbg));
    const char* comma = std::strchr(dbg, ',');
    const float y1 = comma ? static_cast<float>(std::atof(comma + 1)) : y0 + 100;
    for (const TextLine& l : page.lines())
      if (l.box.y >= y0 && l.box.y <= y1)
        std::printf("line [%4.0f,%4.0f %4.0fx%3.0f] %s\n", l.box.x, l.box.y, l.box.w, l.box.h, utf8(l.text).c_str());
  }
  std::printf("--- sample ---\n%s\n", utf8(page.sample(500)).c_str());
  return 0;
}

// SpiderTool scroll <image>: fake scrolls (with a fixed header) and check the tracker.
static int run_scroll(int, wchar_t** argv) {
  Frame img;
  if (!load_image(argv[2], img)) return 1;
  ScrollTracker tracker;
  Rng rng(7);
  int y = 400, ok = 0, bad = 0, total = 0;
  double spent = 0;
  const int h = 900, header = 60;
  for (int i = 0; i < 120; ++i) {
    int d = 0;
    const int kind = i % 4;
    if (kind == 1) d = static_cast<int>(rng.range(-120, 120));
    if (kind == 2) d = static_cast<int>(rng.range(-400, 400));
    if (kind == 3) d = static_cast<int>(rng.range(1, 30));
    y = std::clamp(y + d, 0, img.h - h - 1);
    Frame f;
    slice(img, y, h, f);
    for (int r = 0; r < header; ++r)  // sticky header that never scrolls
      std::copy(img.px.begin() + static_cast<size_t>(r) * img.w, img.px.begin() + static_cast<size_t>(r + 1) * img.w,
                f.px.begin() + static_cast<size_t>(r) * f.w);
    static int last_y = y;
    const int truth = y - last_y;
    last_y = y;
    const double t0 = now_seconds();
    const auto res = tracker.feed(f);
    spent += now_seconds() - t0;
    if (i == 0) continue;
    ++total;
    if (res.shift == truth) ++ok;
    else {
      ++bad;
      std::printf("miss: truth=%d got=%d moved=%d changed=%d cost=%.2f\n", truth, res.shift, res.moved, res.changed,
                  res.cost);
    }
  }
  std::printf("ok=%d bad=%d of %d, %.2f ms per frame\n", ok, bad, total, spent * 1000 / 120);
  return 0;
}

// SpiderTool view <image> <x> <y> <w> <h> <seconds>
// A plain window that shows a page image and scrolls smoothly on the mouse
// wheel, like a browser. A safe target for live tests: no accounts, no sync.
namespace viewer {
Frame page;
double scroll = 0, target = 0;
HWND hwnd = nullptr;

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEWHEEL: {
      RECT rc;
      GetClientRect(h, &rc);
      target -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0 * 100.0;
      target = std::clamp(target, 0.0, static_cast<double>(std::max(0L, page.h - rc.bottom)));
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = page.w;
      bi.bmiHeader.biHeight = -page.h;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      const int top = static_cast<int>(scroll);
      const int rows = std::min<int>(rc.bottom, page.h - top);
      // Draw only the visible slice, starting at the scrolled row.
      bi.bmiHeader.biHeight = -rows;
      SetDIBitsToDevice(dc, 0, 0, std::min<int>(rc.right, page.w), rows, 0, 0, 0, rows,
                        &page.px[static_cast<size_t>(top) * page.w], &bi, DIB_RGB_COLORS);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace viewer

static int run_view(int argc, wchar_t** argv) {
  if (argc < 8 || !load_image(argv[2], viewer::page)) return 1;
  const int x = _wtoi(argv[3]), y = _wtoi(argv[4]), w = _wtoi(argv[5]), h = _wtoi(argv[6]);
  const double seconds = _wtof(argv[7]);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = viewer::proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SpiderTestPage";
  RegisterClassExW(&wc);
  viewer::hwnd = CreateWindowExW(0, wc.lpszClassName, L"SpiderTest - page", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr,
                                 nullptr, wc.hInstance, nullptr);
  ShowWindow(viewer::hwnd, SW_SHOWNOACTIVATE);
  const double start = now_seconds();
  double last = start;
  MSG msg;
  while (now_seconds() - start < seconds) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    const double t = now_seconds();
    const double dt = t - last;
    last = t;
    // Smooth scroll like a browser: glide toward the target.
    const double before = viewer::scroll;
    viewer::scroll += (viewer::target - viewer::scroll) * std::min(1.0, dt * 14.0);
    if (std::fabs(viewer::target - viewer::scroll) < 0.5) viewer::scroll = viewer::target;
    if (static_cast<int>(before) != static_cast<int>(viewer::scroll)) InvalidateRect(viewer::hwnd, nullptr, FALSE);
    Sleep(8);
  }
  DestroyWindow(viewer::hwnd);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  join_mta();
  if (argc >= 3 && std::wstring(argv[1]) == L"ocr") return run_ocr(argc, argv);
  if (argc >= 3 && std::wstring(argv[1]) == L"page") return run_page(argc, argv);
  if (argc >= 3 && std::wstring(argv[1]) == L"scroll") return run_scroll(argc, argv);
  if (argc >= 4 && std::wstring(argv[1]) == L"sim") return run_sim(argc, argv);
  if (argc >= 8 && std::wstring(argv[1]) == L"view") return run_view(argc, argv);
  std::printf("usage: SpiderTool ocr <image> [y] [h]\n");
  return 2;
}
