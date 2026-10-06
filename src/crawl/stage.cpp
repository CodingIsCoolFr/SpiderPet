#include "crawl/stage.hpp"

#include "core/util.hpp"
#include "crawl/spider.hpp"
#include "eyes/shot.hpp"

#include <d2d1_2.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using Microsoft::WRL::ComPtr;
using sp::spider::Vec;

namespace sp::crawl {

namespace {

constexpr float kPi = 3.14159265f;

struct Rgb {
  float r, g, b;
};
Rgb hex(unsigned v) { return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f}; }
D2D1_COLOR_F col(Rgb c, float a) { return D2D1::ColorF(c.r, c.g, c.b, a); }

// The look of the GIF the spider was modelled on.
const Rgb kLeg = hex(0xF27A63), kJoint = hex(0x7DF0A3), kBody = hex(0x4B55D8), kHead = hex(0xE33DE0), kSilk = hex(0xE33DE0);

struct Verdict {
  const wchar_t* text;
  Rgb c;
};
Verdict verdict_style(int v) {
  switch (v) {
    case 1: return {L"…", hex(0x8B93A1)};
    case 2: return {L"✓ verified", hex(0x3DDC84)};
    case 3: return {L"⚠ wrong", hex(0xFFB340)};
    case 4: return {L"✗ not found", hex(0xFF4D5E)};
    case 5: return {L"? no proof", hex(0x8B93A1)};
    case 6: return {L"opinion", hex(0xB48CFF)};
    case 7: return {L"ad", hex(0xFF8A3D)};
    default: return {L"", hex(0)};
  }
}

struct WordReply {
  int leg;
  unsigned ask;
  eyes::WordHit hit;
};

std::atomic<void*> g_picker{nullptr};
std::atomic<HWND> g_stage_hwnd{nullptr};
std::atomic<void*> g_stage_self{nullptr};
constexpr UINT kGuardMsg = WM_APP + 41;

}  // namespace

struct Stage::Impl {
  eyes::Eyes& eyes;
  std::thread th;
  std::atomic<bool> quit{false};
  std::mutex mu;
  Scene scene;
  std::mutex reply_mu;
  std::vector<WordReply> replies;

  HWND hwnd = nullptr;
  bool shown = false;
  RECT placed{};
  ComPtr<ID3D11Device> d3d;
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID2D1Factory2> d2f;
  ComPtr<ID2D1Device1> d2dev;
  ComPtr<ID2D1DeviceContext> dc;
  ComPtr<ID2D1Bitmap1> target;
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IDCompositionTarget> dtarget;
  ComPtr<IDCompositionVisual> visual;
  ComPtr<IDWriteFactory> dw;
  ComPtr<IDWriteTextFormat> f_name, f_text, f_hud, f_badge, f_icon;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<ID2D1StrokeStyle> round, dotted, dashed;
  UINT width = 0, height = 0;

  // How much of each find shows (0..1): it fades in once measured where it
  // is now, and is gone at once when the window or its content moves.
  std::map<std::string, float> shown_a;
  float silk_a = 0;
  // A find re-set on its tag: the text laid out to fit its own box.
  struct Tag {
    float w = 0, h = 0, size = 0;
    ComPtr<IDWriteTextLayout> lay;
    float tw = 0, th = 0;
  };
  std::map<std::string, Tag> tags;
  float chip_w[8] = {};

  spider::Spider sp;
  bool ready = false;
  HWND last_hwnd = nullptr;
  unsigned last_gen = 0, last_jump = 0, last_say = 0;
  std::atomic<bool> arrived{false};
  float time = 0;
  float scan_y = 0, scan_dir = 1;
  bool zig = false;
  std::mt19937 rng{4242};
  // The thought cloud: typed out, then left up to be read.
  std::wstring bubble;
  float bubble_age = 99, bubble_life = 0;

  // Picking a window: the spider follows the cursor.
  std::atomic<bool> picking{false};
  std::function<void(HWND)> pick_done;
  HHOOK hook = nullptr;
  std::atomic<bool> clicked{false};
  POINT click_at{};
  bool swallow_up = false;
  bool topmost = false;

  explicit Impl(eyes::Eyes& e) : eyes(e) {
    g_stage_self = this;
    th = std::thread([this] { run(); });
  }
  ~Impl() {
    quit = true;
    if (hwnd) PostMessageW(hwnd, WM_NULL, 0, 0);
    if (th.joinable()) th.join();
  }

  float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

  // ------------------------------------------------ window

  static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == kGuardMsg) {
      auto* self = static_cast<Impl*>(g_stage_self.load());
      bool hide = w != 0 || (self && self->hidden_in_shares);
      SetWindowDisplayAffinity(h, hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
      return 0;
    }
    return DefWindowProcW(h, m, w, l);
  }

  // Screen shares and recordings: in or out, as the setting says.
  bool hidden_in_shares = false;
  bool affinity_set = false;
  void apply_shares(bool hide) {
    if (affinity_set && hide == hidden_in_shares) return;
    hidden_in_shares = hide;
    affinity_set = true;
    SetWindowDisplayAffinity(hwnd, hide ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
  }

  // Right above the window it is on: other windows that cover that window
  // cover the spider too, and it stays when you click somewhere else.
  void place_above(const RECT& want, HWND target) {
    HWND prev = target ? GetWindow(target, GW_HWNDPREV) : nullptr;
    bool right_above = prev == hwnd;
    if (EqualRect(&want, &placed) && shown && right_above) return;
    HWND after = prev && prev != hwnd ? prev : HWND_TOP;
    bool resized = !EqualRect(&want, &placed) || !shown;
    placed = want;
    SetWindowPos(hwnd, after, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (resized) resize(UINT(want.right - want.left), UINT(want.bottom - want.top));
    shown = true;
  }

  static LRESULT CALLBACK mouse_hook(int code, WPARAM w, LPARAM l) {
    auto* self = static_cast<Impl*>(g_picker.load());
    if (code == HC_ACTION && self && self->picking) {
      auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
      if (w == WM_LBUTTONDOWN) {
        self->click_at = m->pt;
        self->clicked = true;
        self->swallow_up = true;
        return 1;  // the click drops the spider; it does not reach the app
      }
      if (w == WM_LBUTTONUP && self->swallow_up) {
        self->swallow_up = false;
        return 1;
      }
    }
    return CallNextHookEx(nullptr, code, w, l);
  }

  bool create_window() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SpiderPetStage";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                           wc.lpszClassName, L"SpiderPet", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) return false;
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    g_stage_hwnd = hwnd;
    // The spider's own pictures of a window leave it out (screen shares see it, when you want).
    eyes::set_grab_guard([](bool hide) {
      if (HWND h = g_stage_hwnd.load()) {
        DWORD_PTR r = 0;
        SendMessageTimeoutW(h, kGuardMsg, hide ? 1 : 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 300, &r);
      }
    });
    return true;
  }

  bool init_graphics() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
      return false;
    d3d.As(&dxgi);
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory2), &fo, reinterpret_cast<void**>(d2f.GetAddressOf()))))
      return false;
    if (FAILED(d2f->CreateDevice(dxgi.Get(), &d2dev))) return false;
    if (FAILED(d2dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) return false;
    if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) return false;
    if (FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &dtarget))) return false;
    dcomp->CreateVisual(&visual);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    auto fmt = [&](const wchar_t* face, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE s, float size, ComPtr<IDWriteTextFormat>& out) {
      dw->CreateTextFormat(face, nullptr, w, s, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
      if (out) out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, 10.5f, f_name);
    fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 13.f, f_text);
    fmt(L"Cascadia Mono", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, 11.f, f_hud);
    fmt(L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, 10.5f, f_badge);
    fmt(L"Segoe UI Symbol", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, 9.5f, f_icon);
    if (f_icon) {
      f_icon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      f_icon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    D2D1_STROKE_STYLE_PROPERTIES sp2{};
    sp2.startCap = sp2.endCap = D2D1_CAP_STYLE_ROUND;
    sp2.lineJoin = D2D1_LINE_JOIN_ROUND;
    d2f->CreateStrokeStyle(sp2, nullptr, 0, &round);
    D2D1_STROKE_STYLE_PROPERTIES sd = sp2;
    sd.dashCap = D2D1_CAP_STYLE_ROUND;
    sd.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    const float dots[] = {0.f, 2.6f}, dashes[] = {3.f, 2.5f};
    d2f->CreateStrokeStyle(sd, dots, 2, &dotted);
    d2f->CreateStrokeStyle(sd, dashes, 2, &dashed);
    return true;
  }

  bool resize(UINT w, UINT h) {
    w = std::max(16u, w);
    h = std::max(16u, h);
    if (w == width && h == height && swap) return true;
    dc->SetTarget(nullptr);
    target.Reset();
    if (!swap) {
      DXGI_SWAP_CHAIN_DESC1 desc{};
      desc.Width = w;
      desc.Height = h;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      ComPtr<IDXGIAdapter> adapter;
      dxgi->GetAdapter(&adapter);
      ComPtr<IDXGIFactory2> factory;
      adapter->GetParent(IID_PPV_ARGS(&factory));
      if (FAILED(factory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr, &swap))) return false;
      visual->SetContent(swap.Get());
      dtarget->SetRoot(visual.Get());
      dcomp->Commit();
    } else if (FAILED(swap->ResizeBuffers(2, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
      return false;
    }
    ComPtr<IDXGISurface> surface;
    swap->GetBuffer(0, IID_PPV_ARGS(&surface));
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) return false;
    dc->SetTarget(target.Get());
    width = w;
    height = h;
    return true;
  }

  void place(const RECT& want) {
    if (!EqualRect(&want, &placed) || !shown) {
      placed = want;
      SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_TOP, want.left, want.top, want.right - want.left, want.bottom - want.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      resize(UINT(want.right - want.left), UINT(want.bottom - want.top));
      shown = true;
    }
  }

  void hide() {
    if (shown) {
      ShowWindow(hwnd, SW_HIDE);
      shown = false;
    }
  }

  // ------------------------------------------------ main loop

  void run() {
    SetThreadDescription(GetCurrentThread(), L"spiderpet-stage");
    if (!create_window() || !init_graphics()) {
      debug_log("stage: graphics init failed");
      return;
    }
    auto last = std::chrono::steady_clock::now();
    while (!quit) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      auto now = std::chrono::steady_clock::now();
      float dt = std::min(0.05f, std::chrono::duration<float>(now - last).count());
      last = now;
      time += dt;
      if (picking) {
        pick_frame(dt);
        continue;
      }
      Scene sc;
      {
        std::lock_guard<std::mutex> lock(mu);
        sc = scene;
      }
      eyes::State st = eyes.state();
      if (!sc.show || !st.attached || !st.visible || st.window.empty()) {
        hide();
        if (!st.attached) ready = false;
        arrived = false;
        Sleep(50);
        continue;
      }
      RECT want{LONG(st.window.x), LONG(st.window.y), LONG(st.window.x + st.window.w), LONG(st.window.y + st.window.h)};
      apply_shares(sc.hide_in_shares);
      if (topmost) {
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        topmost = false;
      }
      place_above(want, st.hwnd);
      step(dt, sc, st);
      update_alphas(sc, st, dt);
      draw(sc, st, Vec{st.window.x, st.window.y});
      if (FAILED(swap->Present(1, 0))) Sleep(16);
    }
    if (hook) UnhookWindowsHookEx(hook);
    if (hwnd) DestroyWindow(hwnd);
  }

  // A find shows only when it was measured after the last move, on the same
  // layout: never where it used to be.
  static bool fresh(const Mark& m, const eyes::State& st) { return m.at_ms > st.moved_ms && m.layout == st.layout; }

  void update_alphas(const Scene& sc, const eyes::State& st, float dt) {
    std::map<std::string, float> next;
    for (auto& m : sc.marks) {
      auto it = shown_a.find(m.id);
      float a = it == shown_a.end() ? 0.f : it->second;
      next[m.id] = fresh(m, st) ? std::min(1.f, a + dt / 0.15f) : 0.f;
    }
    shown_a.swap(next);
    bool still = GetTickCount64() - st.moved_ms > 120;
    silk_a = still && sc.spans_layout == st.layout ? std::min(1.f, silk_a + dt / 0.3f) : 0.f;
    if (tags.size() > 400) tags.clear();
  }
  float alpha_of(const std::string& id) const {
    auto it = shown_a.find(id);
    return it == shown_a.end() ? 0.f : it->second;
  }

  // ------------------------------------------------ picking a window

  void pick_frame(float dt) {
    if (!hook) hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, GetModuleHandleW(nullptr), 0);
    POINT cur;
    GetCursorPos(&cur);
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!topmost) {
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      topmost = true;
    }
    place(vs);
    Vec origin{float(vs.left), float(vs.top)};
    float s = float(GetDpiForSystem()) / 96.f;
    if (!ready) {
      sp.init(Vec{float(cur.x) - origin.x, float(cur.y) - origin.y}, s, unsigned(GetTickCount()));
      sp.drop = 0;
      ready = true;
      say(L"drop me on any window.");
    }
    sp.scale = s;
    sp.goal = {float(cur.x) - origin.x, float(cur.y) - origin.y + 30 * s};
    sp.has_goal = true;
    sp.max_speed = 900;
    sp.update(dt, nullptr);
    bubble_age += dt;
    HWND under = eyes::window_at(cur);
    std::string what = under ? eyes::identify(under).name : std::string();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    if (under) {
      RECT r{};
      GetWindowRect(under, &r);
      D2D1_RECT_F rr = D2D1::RectF(float(r.left) - origin.x, float(r.top) - origin.y, float(r.right) - origin.x, float(r.bottom) - origin.y);
      brush->SetColor(col(kJoint, 0.9f));
      dc->DrawRectangle(rr, brush.Get(), 3.f * s);
    }
    draw_spider(Vec{0, 0}, 0, s);
    std::wstring hint = under ? L"click to drop me on " + wide(what) : std::wstring(L"move over a window · Esc to cancel");
    draw_hud(hint, Vec{sp.pos.x, sp.pos.y}, s, float(vs.right - vs.left));
    if (bubble_age < bubble_life) draw_bubble(Vec{sp.pos.x, sp.pos.y}, s, -1e9f, float(vs.right - vs.left), "SpiderPet");
    dc->EndDraw();
    swap->Present(1, 0);
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (clicked || esc) {
      HWND got = nullptr;
      if (clicked) got = eyes::window_at(click_at);
      clicked = false;
      picking = false;
      g_picker = nullptr;
      if (hook) {
        UnhookWindowsHookEx(hook);
        hook = nullptr;
      }
      ready = false;  // drops in on the window it was given
      hide();
      auto done = std::move(pick_done);
      if (done) done(esc ? nullptr : got);
    }
  }

  // ------------------------------------------------ simulation

  Vec content_of_screen(float x, float y, const eyes::State& st) const {
    return {x - st.view.x, y - st.view.y + float(st.scroll)};
  }
  // Content space -> the stage window.
  Vec local(Vec c, const eyes::State& st, Vec origin) const {
    return {c.x + st.view.x - origin.x, c.y - float(st.scroll) + st.view.y - origin.y};
  }

  const Mark* mark_of(const Scene& sc, const std::string& id) const {
    for (auto& m : sc.marks)
      if (m.id == id) return &m;
    return nullptr;
  }

  // The sentence it reads now: all its lines in one box (content space).
  eyes::Box reading_box(const Scene& sc) const {
    const Mark* m = sc.reading.empty() ? nullptr : mark_of(sc, sc.reading);
    if (!m || m->boxes.empty()) return {};
    eyes::Box u = m->boxes[0];
    for (auto& b : m->boxes) {
      float x1 = std::max(u.x + u.w, b.x + b.w), y1 = std::max(u.y + u.h, b.y + b.h);
      u.x = std::min(u.x, b.x);
      u.y = std::min(u.y, b.y);
      u.w = x1 - u.x;
      u.h = y1 - u.y;
    }
    return u;
  }

  void say(const std::wstring& t) {
    bubble = t;
    bubble_age = 0;
    bubble_life = float(t.size()) / 32.f + 2.6f;
  }

  void step(float dt, const Scene& sc, const eyes::State& st) {
    float s = float(st.dpi_scale);
    float top = float(st.scroll), bottom = top + st.view.h;
    if (!ready || st.hwnd != last_hwnd || st.gen != last_gen) {
      if (!ready || st.hwnd != last_hwnd) {
        // It drops in on a thread from the top of the window.
        sp.init(Vec{st.view.w * 0.45f, top + st.view.h * 0.3f}, s, unsigned(GetTickCount()));
        scan_y = sp.pos.y;
        ready = true;
      }
      last_hwnd = st.hwnd;
      last_gen = st.gen;
      last_jump = st.jump;
    }
    if (st.jump != last_jump) {
      last_jump = st.jump;
      sp.replant();
    }
    if (sc.say_id != last_say) {
      last_say = sc.say_id;
      if (!sc.say.empty()) say(wide(sc.say));
    }
    bubble_age += dt;
    sp.scale = s;
    for (auto& l : sp.legs) l.has_override = false;
    // Where it is going: a find, a thing on screen, or along the text.
    eyes::Box goal_box;
    bool have_goal = false;
    if (!sc.goal.empty())
      if (const Mark* m = mark_of(sc, sc.goal); m && !m->boxes.empty()) {
        goal_box = m->boxes[0];
        have_goal = true;
      }
    if (!have_goal && !sc.goal_screen.empty()) {
      Vec c = content_of_screen(sc.goal_screen.x, sc.goal_screen.y, st);
      goal_box = eyes::Box{c.x, c.y, sc.goal_screen.w, sc.goal_screen.h};
      have_goal = true;
    }
    if (have_goal) {
      // Stand just left of it, or below it if there is no room.
      float x = goal_box.x - 30 * s;
      float y = goal_box.y + goal_box.h * 0.5f + 40 * s;
      if (x < 60 * s) x = goal_box.x + std::min(goal_box.w, 60.f * s);
      x = std::clamp(x, 40 * s, std::max(50 * s, st.window.w - 40 * s));
      sp.goal = {x, y};
      sp.has_goal = true;
      sp.max_speed = 380;
      bool close_by = sp.arrived(45);
      arrived = close_by;
      if (close_by && sc.hold) {
        sp.legs[0].override_target = {goal_box.x, goal_box.y};
        sp.legs[0].has_override = true;
        sp.legs[4].override_target = {goal_box.x + std::min(goal_box.w, 220.f * s), goal_box.y + goal_box.h};
        sp.legs[4].has_override = true;
      }
    } else if (eyes::Box rr = reading_box(sc); !rr.empty()) {
      arrived = false;
      // Reading: line by line over the sentence, like eyes over text.
      float x0 = std::max(50 * s, rr.x + 20 * s), x1 = std::max(x0 + 1, rr.x + rr.w - 20 * s);
      if (!sp.has_goal || sp.arrived(30) || sp.goal.y < rr.y - 30 * s || sp.goal.y > rr.y + rr.h + 40 * s) {
        if (scan_y < rr.y || scan_y > rr.y + rr.h) scan_y = rr.y;
        scan_y += 22 * s;
        if (scan_y > rr.y + rr.h) scan_y = rr.y;
        scan_dir = -scan_dir;
        sp.goal = {scan_dir > 0 ? x1 : x0, scan_y + 30 * s};
      }
      sp.has_goal = true;
      sp.max_speed = 210;
    } else {
      arrived = false;
      // Roaming down the text in a zig-zag, like the GIF.
      if (!sp.has_goal || sp.arrived(30)) {
        scan_y += scan_dir * rnd(26, 58) * s;
        if (scan_y > bottom - 80 * s) {
          scan_y = bottom - 80 * s;
          scan_dir = -1;
        }
        if (scan_y < top + 80 * s) {
          scan_y = top + 80 * s;
          scan_dir = 1;
        }
        zig = !zig;
        float lo = 80 * s, hi = std::max(90 * s, st.view.w - 80 * s), mid = (lo + hi) / 2;
        sp.goal = {zig ? rnd(lo, mid) : rnd(mid, hi), scan_y};
        sp.has_goal = true;
      }
      if (sp.goal.y < top || sp.goal.y > bottom) {
        scan_y = (top + bottom) / 2;
        sp.goal.y = scan_y;
      }
      sp.max_speed = 130;
    }
    // Left far behind by a big scroll: it rappels back in.
    if (sp.drop <= 0 && (sp.pos.y < top - 200 * s || sp.pos.y > bottom + 200 * s)) {
      float x = std::clamp(sp.goal.x, 80 * s, std::max(90 * s, st.view.w - 80 * s));
      float y = std::clamp(sp.goal.y, top + 120 * s, std::max(top + 130 * s, bottom - 120 * s));
      sp.teleport({x, y});
    }
    eyes::Box view = st.view;
    float scroll_now = float(st.scroll);
    sp.update(dt, [this, view, scroll_now](int leg, unsigned ask, Vec p) {
      POINT pt{LONG(p.x + view.x), LONG(p.y - scroll_now + view.y)};
      if (!view.contains(float(pt.x), float(pt.y))) return;
      eyes.word_at(pt, [this, leg, ask](const eyes::WordHit& w) {
        std::lock_guard<std::mutex> lock(reply_mu);
        replies.push_back({leg, ask, w});
      });
    });
    // Feet that found a word grab a corner of it.
    std::vector<WordReply> got;
    {
      std::lock_guard<std::mutex> lock(reply_mu);
      got.swap(replies);
    }
    for (auto& r : got) {
      if (!r.hit.ok) continue;
      Vec c = content_of_screen(r.hit.word.x, r.hit.word.y, st);
      Vec aim = sp.legs[r.leg].to;
      Vec corners[4] = {{c.x, c.y}, {c.x + r.hit.word.w, c.y}, {c.x, c.y + r.hit.word.h}, {c.x + r.hit.word.w, c.y + r.hit.word.h}};
      Vec best = corners[0];
      for (auto& k : corners)
        if ((k - aim).len() < (best - aim).len()) best = k;
      sp.snap(r.leg, r.ask, best);
    }
  }

  // ------------------------------------------------ drawing

  void line(Vec a, Vec b, D2D1_COLOR_F c, float w) {
    brush->SetColor(c);
    dc->DrawLine({a.x, a.y}, {b.x, b.y}, brush.Get(), w, round.Get());
  }
  void dot(Vec p, float r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    dc->FillEllipse(D2D1::Ellipse({p.x, p.y}, r, r), brush.Get());
  }
  void fill(const D2D1_RECT_F& r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    dc->FillRectangle(r, brush.Get());
  }
  void frame(const D2D1_RECT_F& r, D2D1_COLOR_F c, float w) {
    brush->SetColor(c);
    dc->DrawRectangle(r, brush.Get(), w);
  }
  float text_width(const std::wstring& t, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> l;
    dw->CreateTextLayout(t.c_str(), UINT32(t.size()), f, 4000, 200, &l);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
  }
  void text(const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    brush->SetColor(c);
    dc->DrawText(t.c_str(), UINT32(t.size()), f, r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  void draw_spider(Vec off, float drop_off, float s) {
    auto P = [&](Vec v) { return Vec{v.x + off.x, v.y + off.y - drop_off}; };
    Vec body = P(sp.pos);
    if (sp.drop > 0) line({body.x, 0}, body, col(kSilk, 0.8f), 1.f * s);
    for (int i = 0; i < 8; ++i) {
      Vec h = P(sp.hip(i)), k = P(sp.knee(i)), f = P(sp.legs[i].foot);
      line(h, k, col(kLeg, 0.97f), 1.6f * s);
      line(k, f, col(kLeg, 0.97f), 1.5f * s);
      dot(k, 2.8f * s, col(kJoint, 1));
      dot(f, 2.8f * s, col(kJoint, 1));
    }
    float deg = sp.heading * 180.f / kPi;
    dc->SetTransform(D2D1::Matrix3x2F::Rotation(deg, {body.x, body.y}));
    D2D1_RECT_F br = D2D1::RectF(body.x - 19 * s, body.y - 5 * s, body.x + 19 * s, body.y + 5 * s);
    fill(br, D2D1::ColorF(0.07f, 0.09f, 0.26f, 0.93f));
    frame(br, col(kBody, 1), 1.6f * s);
    dot({body.x + 13 * s, body.y}, 3.2f * s, col(kHead, 1));
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
  }

  void draw_hud(const std::wstring& t, Vec body, float s, float clip_r) {
    if (t.empty()) return;
    float w = text_width(t, f_hud.Get()) + 10;
    float x = std::clamp(body.x - w / 2, 4.f, std::max(4.f, clip_r - w - 4));
    float y = body.y + 34 * s;
    fill(D2D1::RectF(x, y, x + w, y + 18), D2D1::ColorF(0.03f, 0.04f, 0.06f, 0.85f));
    text(t, f_hud.Get(), D2D1::RectF(x + 5, y + 1, x + w, y + 18), col(kJoint, 0.95f));
  }

  // A scalloped cloud with the name in mint and the thought typed out.
  void draw_bubble(Vec body, float s, float top_limit, float clip_r, const std::string& name_u8) {
    float a = std::min(1.f, bubble_age * 8) * std::clamp((bubble_life - bubble_age) * 3, 0.f, 1.f);
    if (a <= 0) return;
    size_t n = std::min(bubble.size(), size_t(bubble_age * 32.f) + 1);
    std::wstring shown = bubble.substr(0, n);
    std::wstring name = wide(name_u8);
    float tw = std::min(460 * s, std::max(text_width(bubble, f_text.Get()), text_width(name, f_name.Get())));
    float w = tw + 40 * s, h = 46 * s;
    float x = std::clamp(body.x + 10 * s, 4.f, std::max(4.f, clip_r - w - 6));
    float y = std::max(top_limit + 10 * s, body.y - 76 * s - h);
    if (top_limit < -1e8f) y = body.y - 76 * s - h;
    D2D1_COLOR_F edge = D2D1::ColorF(0.42f, 0.45f, 0.95f, 0.95f * a);
    D2D1_COLOR_F fillc = D2D1::ColorF(0.035f, 0.045f, 0.13f, 0.95f * a);
    struct Blob {
      float cx, cy, r;
    };
    std::vector<Blob> blobs;
    int bumps = std::max(3, int((w - 8 * s) / (15 * s)));
    for (int i = 0; i < bumps; ++i) {
      float cx = x + 7 * s + (w - 14 * s) * float(i) / float(bumps - 1);
      blobs.push_back({cx, y + 3 * s, 8.5f * s});
      blobs.push_back({cx, y + h - 3 * s, 8.5f * s});
    }
    for (float f : {0.36f, 0.66f}) {
      blobs.push_back({x + 3 * s, y + h * f, 9 * s});
      blobs.push_back({x + w - 3 * s, y + h * f, 9 * s});
    }
    float o = 1.4f * s;
    brush->SetColor(edge);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - o, y - o, x + w + o, y + h + o), 14 * s, 14 * s), brush.Get());
    for (auto& b : blobs) dc->FillEllipse(D2D1::Ellipse({b.cx, b.cy}, b.r + o, b.r + o), brush.Get());
    brush->SetColor(fillc);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), 14 * s, 14 * s), brush.Get());
    for (auto& b : blobs) dc->FillEllipse(D2D1::Ellipse({b.cx, b.cy}, b.r, b.r), brush.Get());
    Vec p0{x + 34 * s, y + h + 12 * s};
    Vec dd = body - p0;
    float len = std::max(1.f, dd.len());
    Vec dir = dd * (1.f / len);
    float stp = std::min(16 * s, len / 3);
    float radii[3] = {5.5f, 4.f, 2.8f};
    for (int i = 0; i < 3; ++i) {
      Vec p = p0 + dir * (stp * float(i));
      brush->SetColor(fillc);
      dc->FillEllipse(D2D1::Ellipse({p.x, p.y}, radii[i] * s, radii[i] * s), brush.Get());
      brush->SetColor(edge);
      dc->DrawEllipse(D2D1::Ellipse({p.x, p.y}, radii[i] * s, radii[i] * s), brush.Get(), 1.2f * s);
    }
    text(name, f_name.Get(), D2D1::RectF(x + 18 * s, y + 6 * s, x + w - 6 * s, y + 22 * s), col(kJoint, a));
    text(shown, f_text.Get(), D2D1::RectF(x + 18 * s, y + 21 * s, x + w - 6 * s, y + h), D2D1::ColorF(1, 1, 1, a));
  }

  static bool tagged(const Mark& m) {
    return m.eaten && m.boxes.size() == 1 && (m.kind == "doi" || m.kind == "isbn" || m.kind == "id" || m.kind == "title");
  }

  // A find the spider ate, re-set on a navy tag of exactly its own size, so it
  // never covers the words around it: DOIs in green code, ISBNs in bold blue
  // code, ids on a blue chip, titles in salmon serif. The text shrinks to fit.
  void draw_tag(const Mark& m, const D2D1_RECT_F& r, float s, float a) {
    struct Look {
      const wchar_t* face;
      DWRITE_FONT_WEIGHT weight;
      DWRITE_FONT_STYLE style;
      Rgb ink, fillc, edge;
      float edge_a;
    };
    const Rgb navy{0.035f, 0.045f, 0.13f};
    Look lk = m.kind == "doi"    ? Look{L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, hex(0x8CF0B0), navy, kJoint, 0.5f}
              : m.kind == "isbn" ? Look{L"Consolas", DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, hex(0x9CC2FF), navy, hex(0x5B63E0), 0.95f}
              : m.kind == "id"   ? Look{L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, hex(0x0E1A3A), hex(0x6A9CF0), hex(0x6A9CF0), 0.f}
                                 : Look{L"Georgia", DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, hex(0xFF9A82), navy, kLeg, 0.5f};
    float bw = r.right - r.left, bh = r.bottom - r.top;
    Tag& tg = tags[m.id];
    if (!tg.lay || std::fabs(tg.w - bw) > 0.5f || std::fabs(tg.h - bh) > 0.5f) {
      tg = Tag{};
      tg.w = bw;
      tg.h = bh;
      std::wstring t = wide(m.text.size() > 200 ? m.text.substr(0, 200) : m.text);
      float avail = std::max(4.f, bw - 4);
      float size = std::min(bh * 0.74f, 30.f * s);
      for (int pass = 0; pass < 2; ++pass) {
        ComPtr<IDWriteTextFormat> f;
        dw->CreateTextFormat(lk.face, nullptr, lk.weight, lk.style, DWRITE_FONT_STRETCH_NORMAL, std::max(7.f, size), L"en-us", &f);
        if (!f) return;
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> lay;
        dw->CreateTextLayout(t.c_str(), UINT32(t.size()), f.Get(), avail, bh, &lay);
        if (!lay) return;
        DWRITE_TEXT_METRICS mt{};
        lay->GetMetrics(&mt);
        if (pass == 0 && mt.widthIncludingTrailingWhitespace > avail && size > 7.f) {
          size = std::max(7.f, size * avail / mt.widthIncludingTrailingWhitespace * 0.98f);
          continue;
        }
        // Still too long at the smallest size: it ends in an ellipsis.
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> dots;
        dw->CreateEllipsisTrimmingSign(f.Get(), &dots);
        lay->SetTrimming(&trim, dots.Get());
        lay->GetMetrics(&mt);
        tg.lay = lay;
        tg.size = size;
        tg.tw = std::min(avail, mt.widthIncludingTrailingWhitespace);
        tg.th = mt.height;
        break;
      }
    }
    if (!tg.lay) return;
    D2D1_RECT_F bx = D2D1::RectF(r.left - 1, r.top, r.right + 1, r.bottom);
    brush->SetColor(col(lk.fillc, a));  // solid: the words under it don't show through
    dc->FillRoundedRectangle(D2D1::RoundedRect(bx, 3 * s, 3 * s), brush.Get());
    if (lk.edge_a > 0) {
      brush->SetColor(col(lk.edge, lk.edge_a * a));
      dc->DrawRoundedRectangle(D2D1::RoundedRect(bx, 3 * s, 3 * s), brush.Get(), 1.f * s);
    }
    brush->SetColor(col(lk.ink, a));
    dc->DrawTextLayout({r.left + (bw - tg.tw) / 2, r.top + (bh - tg.th) / 2}, tg.lay.Get(), brush.Get());
  }

  // A find it ate that is not re-set (it runs over lines, or it is a link or
  // a sentence): its kind's colour, on its own words only.
  void draw_kind_mark(const Mark& m, const D2D1_RECT_F& r, float s, float a) {
    if (m.kind == "link") {
      frame(D2D1::RectF(r.left - 2, r.top - 1, r.right + 2, r.bottom + 1), col(kSilk, 0.85f * a), 1.3f * s);
      return;
    }
    if (m.kind == "sentence") {
      if (!m.verdict) line({r.left, r.bottom + 1}, {r.right, r.bottom + 1}, col(kJoint, 0.55f * a), 1.4f * s);
      return;
    }
    Rgb kc = m.kind == "doi" ? hex(0x3DDC84) : m.kind == "isbn" ? hex(0x5B8CFF) : m.kind == "id" ? hex(0x6A9CF0) : kLeg;
    fill(D2D1::RectF(r.left - 1, r.top, r.right + 1, r.bottom), col(kc, 0.16f * a));
    if (!m.verdict) line({r.left, r.bottom + 1}, {r.right, r.bottom + 1}, col(kc, 0.8f * a), 1.4f * s);
  }

  // The check, right under the find's words, like a spell checker's line:
  // straight green when verified, wavy amber or red when wrong or not found,
  // dotted grey for no proof (and while checking), dashed for opinion or ad.
  void draw_check_line(int v, const D2D1_RECT_F& r, float s, float a) {
    Verdict vs = verdict_style(v);
    if (!vs.text[0]) return;
    float y = r.bottom + 1.8f * s, x0 = r.left, x1 = r.right;
    float w = 1.5f * s;
    if (v == 1) a *= 0.45f + 0.35f * std::sin(time * 6);
    brush->SetColor(col(vs.c, 0.95f * a));
    if (v == 2) {
      dc->DrawLine({x0, y}, {x1, y}, brush.Get(), w, round.Get());
    } else if (v == 3 || v == 4) {
      float amp = 1.5f * s, step = 2.4f * s;
      bool up = true;
      for (float x = x0; x < x1; x += step, up = !up)
        dc->DrawLine({x, y + (up ? -amp : amp)}, {std::min(x1, x + step), y + (up ? amp : -amp)}, brush.Get(), 1.2f * s, round.Get());
    } else {
      dc->DrawLine({x0, y}, {x1, y}, brush.Get(), w, (v == 6 || v == 7) ? dashed.Get() : dotted.Get());
    }
  }

  // Verdict labels: in the margin beside the find's paragraph, never on text.
  // Greedy placement, as map and chart labelers do: try the right margin,
  // then the left, sliding up or down past labels already placed. With no
  // margin to use, a small round badge sits just above the end of the find.
  void draw_chips(const Scene& sc, const eyes::State& st, Vec origin, float s) {
    std::vector<D2D1_RECT_F> taken;
    auto hits = [&](const D2D1_RECT_F& r) {
      for (auto& t : taken)
        if (r.left < t.right && r.right > t.left && r.top < t.bottom && r.bottom > t.top) return true;
      return false;
    };
    std::vector<const Mark*> order;
    for (auto& m : sc.marks) {
      if (alpha_of(m.id) <= 0 || m.boxes.empty()) continue;
      for (auto& b : m.boxes) {
        Vec p = local({b.x, b.y}, st, origin);
        taken.push_back(D2D1::RectF(p.x - 2, p.y - 1, p.x + b.w + 2, p.y + b.h + 3 * s));
      }
      if (m.verdict >= 2) order.push_back(&m);
    }
    std::sort(order.begin(), order.end(), [](const Mark* a, const Mark* b) {
      const eyes::Box &x = a->boxes.back(), &y = b->boxes.back();
      return x.y != y.y ? x.y < y.y : x.x < y.x;
    });
    float vl = st.view.x - origin.x, vr = vl + st.view.w, vt = st.view.y - origin.y, vb = vt + st.view.h;
    // Paragraphs (where they are now) are text: labels stay off them.
    std::vector<D2D1_RECT_F> paras;
    if (sc.spans_layout == st.layout)
      for (auto& c : sc.columns) {
        Vec cp = local({c.x, c.y}, st, origin);
        if (cp.y + c.h < vt - 40 || cp.y > vb + 40) continue;
        paras.push_back(D2D1::RectF(cp.x - 6 * s, cp.y - 2, cp.x + c.w + 6 * s, cp.y + c.h + 2));
      }
    // The free gap on this band nearest to the find: x of the label, or none.
    auto gap_for = [&](float top, float bot, float w, const D2D1_RECT_F& fr, float* dist) -> float {
      std::vector<std::pair<float, float>> busy;
      for (auto* list : {&paras, &taken})
        for (auto& r : *list)
          if (r.top < bot + 2 && r.bottom > top - 2) busy.push_back({r.left - 3, r.right + 3});
      std::sort(busy.begin(), busy.end());
      float best_x = -1e9f, best_d = 1e9f, at = vl + 4;
      auto consider = [&](float g0, float g1) {
        if (g1 - g0 < w) return;
        float x, d;
        if (g0 >= fr.right) x = g0, d = g0 - fr.right;
        else if (g1 <= fr.left) x = g1 - w, d = fr.left - g1;
        else return;  // a gap across the find itself means no paragraph is known here
        if (d < best_d) best_d = d, best_x = x;
      };
      for (auto& [b0, b1] : busy) {
        if (b0 > at) consider(at, b0);
        at = std::max(at, b1);
      }
      consider(at, vr - 4);
      *dist = best_d;
      return best_x;
    };
    float h = 16 * s;
    for (const Mark* m : order) {
      float a = alpha_of(m->id);
      Verdict v = verdict_style(m->verdict);
      if (chip_w[m->verdict] <= 0) chip_w[m->verdict] = text_width(v.text, f_badge.Get()) + 12;
      float w = chip_w[m->verdict];
      const eyes::Box& b = m->boxes.back();
      Vec p = local({b.x, b.y}, st, origin);
      D2D1_RECT_F fr = D2D1::RectF(p.x, p.y, p.x + b.w, p.y + b.h);
      float cy = (fr.top + fr.bottom) / 2;
      bool placed = false;
      for (int k = 0; k < 9 && !placed && !paras.empty(); ++k) {
        float off = float((k + 1) / 2) * (h + 3) * (k % 2 ? 1.f : -1.f);
        float top = cy - h / 2 + off, bot = top + h;
        if (top < vt + 2 || bot > vb - 2) continue;
        float dist = 0, x = gap_for(top, bot, w, fr, &dist);
        if (x < -1e8f) continue;
        D2D1_RECT_F r = D2D1::RectF(x, top, x + w, bot);
        taken.push_back(r);
        placed = true;
        // Far from its find, or off its line: a thin thread back to it.
        if (std::fabs(off) > h * 0.6f || dist > 90 * s) {
          bool right = x > fr.right;
          brush->SetColor(col(v.c, 0.6f * a));
          dc->DrawLine({right ? r.left : r.right, (r.top + r.bottom) / 2}, {right ? fr.right + 2 : fr.left - 2, cy}, brush.Get(), 1.f * s, dotted.Get());
        }
        brush->SetColor(col(v.c, 0.95f * a));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 7 * s, 7 * s), brush.Get());
        text(v.text, f_badge.Get(), D2D1::RectF(r.left + 6, r.top + 1, r.right, r.bottom), D2D1::ColorF(0.04f, 0.05f, 0.06f, a));
      }
      if (placed) continue;
      // No margin: a small round badge just above the end of the find.
      float rad = 6.5f * s;
      D2D1_POINT_2F c{fr.right, fr.top - rad * 0.55f};
      D2D1_RECT_F rr = D2D1::RectF(c.x - rad, c.y - rad, c.x + rad, c.y + rad);
      bool clash = false;
      for (size_t i = 0; i < taken.size() && !clash; ++i) {
        const D2D1_RECT_F& t = taken[i];
        bool own = t.left <= fr.left + 1 && t.right >= fr.right - 1 && t.top <= fr.top + 1 && t.bottom >= fr.bottom - 1;
        if (!own && rr.left < t.right && rr.right > t.left && rr.top < t.bottom && rr.bottom > t.top) clash = true;
      }
      if (clash || rr.top < vt) continue;
      taken.push_back(rr);
      brush->SetColor(col(v.c, 0.97f * a));
      dc->FillEllipse(D2D1::Ellipse(c, rad, rad), brush.Get());
      std::wstring g(1, v.text[0]);
      text(g, f_icon.Get(), rr, D2D1::ColorF(0.04f, 0.05f, 0.06f, a));
    }
  }

  void draw(const Scene& sc, const eyes::State& st, Vec origin) {
    float s = float(st.dpi_scale);
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // Only over the window's content (not its title bar), except the spider itself.
    D2D1_RECT_F clip = D2D1::RectF(st.view.x - origin.x, st.view.y - origin.y, st.view.x - origin.x + st.view.w, st.view.y - origin.y + st.view.h);
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
    // Silk down the margin of what it has read (hidden while things move).
    if (silk_a > 0)
      for (auto& r : sc.read) {
        Vec a = local({r.x - 9 * s, r.y}, st, origin), b = local({r.x - 9 * s, r.y + r.h}, st, origin);
        if (b.y < 0 || a.y > float(height)) continue;
        line(a, b, col(kLeg, 0.35f * silk_a), 1.f * s);
      }
    // The sentence it reads: a soft band over its lines.
    if (const Mark* rm = sc.reading.empty() ? nullptr : mark_of(sc, sc.reading))
      if (float a = alpha_of(rm->id); a > 0)
        for (auto& b : rm->boxes) {
          Vec p = local({b.x, b.y}, st, origin);
          fill(D2D1::RectF(p.x - 2, p.y - 1, p.x + b.w + 2, p.y + b.h + 1), col(kJoint, (0.07f + 0.04f * std::sin(time * 5)) * a));
        }
    // What matches your search lights up; what it ate is re-set on its tag;
    // a check line sits under each checked find. Only finds measured where
    // they are now are drawn.
    for (auto& m : sc.marks) {
      float a = alpha_of(m.id);
      if (a <= 0) continue;
      for (auto& b : m.boxes) {
        Vec lp = local({b.x, b.y}, st, origin);
        if (lp.y + b.h < 0 || lp.y > float(height)) continue;
        D2D1_RECT_F r = D2D1::RectF(lp.x, lp.y, lp.x + b.w, lp.y + b.h);
        if (m.match && !m.eaten) fill(D2D1::RectF(r.left - 2, r.top - 1, r.right + 2, r.bottom + 1), D2D1::ColorF(1.f, 0.85f, 0.2f, 0.28f * a));
        if (tagged(m)) draw_tag(m, r, s, a);
        else if (m.eaten) draw_kind_mark(m, r, s, a);
        if (m.verdict) draw_check_line(m.verdict, r, s, a);
      }
    }
    draw_chips(sc, st, origin, s);
    dc->PopAxisAlignedClip();
    // The lasso: a magenta thread from its head to what it holds (only where
    // that thing is now).
    Vec body = local(sp.pos, st, origin);
    float drop_off = sp.drop * sp.drop * 380 * s;
    eyes::Box hold_box;
    if (sc.hold) {
      if (const Mark* m = mark_of(sc, sc.goal); m && !m->boxes.empty()) {
        if (alpha_of(m->id) > 0) hold_box = m->boxes[0];
      } else if (!sc.goal_screen.empty() && GetTickCount64() - st.moved_ms > 120) {
        Vec c = content_of_screen(sc.goal_screen.x, sc.goal_screen.y, st);
        hold_box = {c.x, c.y, sc.goal_screen.w, sc.goal_screen.h};
      }
    }
    if (!hold_box.empty()) {
      Vec a = local({hold_box.x, hold_box.y}, st, origin);
      D2D1_RECT_F r = D2D1::RectF(a.x - 2, a.y - 2, a.x + hold_box.w + 2, a.y + hold_box.h + 2);
      frame(r, col(kSilk, 0.9f), 1.4f * s);
      Vec hd = local(sp.head(), st, origin);
      line(hd, {std::clamp(hd.x, r.left, r.right), hd.y < r.top ? r.top : r.bottom}, col(kSilk, 0.9f), 1.3f * s);
    }
    Vec off{st.view.x - origin.x, st.view.y - origin.y - float(st.scroll)};
    draw_spider(off, drop_off, s);
    body.y -= drop_off;
    draw_hud(wide(sc.hud), body, s, float(width));
    if (bubble_age < bubble_life) draw_bubble(body, s, 0, float(width), sc.name);
    if (dc->EndDraw() == D2DERR_RECREATE_TARGET) {
      debug_log("stage: device lost, rebuilding");
      swap.Reset();
      width = height = 0;
    }
  }
};

Stage::Stage(eyes::Eyes& eyes) : d(std::make_unique<Impl>(eyes)) {}
Stage::~Stage() = default;

void Stage::set(const Scene& s) {
  std::lock_guard<std::mutex> lock(d->mu);
  d->scene = s;
}

bool Stage::at_goal() const { return d->arrived; }

void Stage::pick(std::function<void(HWND)> done) {
  d->pick_done = std::move(done);
  d->clicked = false;
  d->ready = false;
  g_picker = d.get();
  d->picking = true;
}

bool Stage::picking() const { return d->picking; }

}  // namespace sp::crawl
