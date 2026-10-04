#include "render/overlay.hpp"

#include "core/util.hpp"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace sp {

static UINT pick_interval(const RECT& monitor);

bool Overlay::create(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = &Overlay::proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SpiderPetOverlay";
  RegisterClassExW(&wc);

  POINT origin{0, 0};
  HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  bounds_ = mi.rcMonitor;

  hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                              WS_EX_LAYERED | WS_EX_TRANSPARENT,
                          wc.lpszClassName, L"SpiderPet Overlay", WS_POPUP, bounds_.left, bounds_.top,
                          bounds_.right - bounds_.left, bounds_.bottom - bounds_.top, nullptr, nullptr, inst, this);
  if (!hwnd_) return false;
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);


  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               &d3d_, nullptr, nullptr)))
    return false;
  ComPtr<IDXGIDevice> dxgi;
  d3d_.As(&dxgi);
  ComPtr<IDXGIFactory2> dxgi_factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi_factory)))) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(bounds_.right - bounds_.left);
  desc.Height = static_cast<UINT>(bounds_.bottom - bounds_.top);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  if (FAILED(dxgi_factory->CreateSwapChainForComposition(dxgi.Get(), &desc, nullptr, &swap_))) return false;

  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(factory_.GetAddressOf()))))
    return false;
  if (FAILED(factory_->CreateDevice(dxgi.Get(), &device_))) return false;
  if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_))) return false;
  if (!make_target()) return false;

  if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
  if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dtarget_))) return false;
  if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
  visual_->SetContent(swap_.Get());
  dtarget_->SetRoot(visual_.Get());
  dcomp_->Commit();

  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1),
                      reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));

  interval_ = pick_interval(bounds_);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  return true;
}

bool Overlay::make_target() {
  ctx_->SetTarget(nullptr);
  target_.Reset();
  ComPtr<IDXGISurface> surface;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
  const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
  if (FAILED(ctx_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
  ctx_->SetTarget(target_.Get());
  ctx_->SetDpi(96.f, 96.f);
  return true;
}

void Overlay::destroy() {
  if (ctx_) ctx_->SetTarget(nullptr);
  target_.Reset();
  visual_.Reset();
  dtarget_.Reset();
  dcomp_.Reset();
  ctx_.Reset();
  device_.Reset();
  swap_.Reset();
  d3d_.Reset();
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

// On a 240 Hz screen, every second refresh is plenty for the spider.
static UINT pick_interval(const RECT& monitor) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  POINT c{(monitor.left + monitor.right) / 2, (monitor.top + monitor.bottom) / 2};
  if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return 1;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 1;
  return dm.dmDisplayFrequency >= 200 ? 2 : 1;
}

void Overlay::cover(const RECT& monitor) {
  if (EqualRect(&monitor, &bounds_)) return;
  bounds_ = monitor;
  interval_ = pick_interval(monitor);
  const int w = monitor.right - monitor.left;
  const int h = monitor.bottom - monitor.top;
  SetWindowPos(hwnd_, HWND_TOPMOST, monitor.left, monitor.top, w, h, SWP_NOACTIVATE);
  ctx_->SetTarget(nullptr);
  target_.Reset();
  swap_->ResizeBuffers(2, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
  make_target();
}

void Overlay::set_interactive(bool on) {
  if (dragging_) on = true;
  if (on == interactive_) return;
  interactive_ = on;
  LONG ex = GetWindowLongW(hwnd_, GWL_EXSTYLE);
  if (on) ex &= ~WS_EX_TRANSPARENT;
  else ex |= WS_EX_TRANSPARENT;
  SetWindowLongW(hwnd_, GWL_EXSTYLE, ex);
}

void Overlay::set_capturable(bool on) {
  if (on == capturable_) return;
  capturable_ = on;
  SetWindowDisplayAffinity(hwnd_, on ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}

void Overlay::set_visible(bool on) {
  if (on == visible_) return;
  visible_ = on;
  ShowWindow(hwnd_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
}

ID2D1DeviceContext* Overlay::begin() {
  if (!ctx_ || !target_) return nullptr;
  ctx_->BeginDraw();
  ctx_->SetTransform(D2D1::Matrix3x2F::Identity());
  ctx_->Clear(D2D1::ColorF(0, 0, 0, 0));
  return ctx_.Get();
}

void Overlay::end() {
  if (!ctx_) return;
  const HRESULT hr = ctx_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) make_target();
  swap_->Present(interval_, 0);
}

LRESULT CALLBACK Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->hwnd_ == hwnd) return self->handle(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::handle(UINT msg, WPARAM wp, LPARAM lp) {
  auto screen_point = [&] {
    POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
    ClientToScreen(hwnd_, &p);
    return p;
  };
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, dragging_ ? IDC_SIZEALL : (grab_cursor ? IDC_HAND : IDC_ARROW)));
      return TRUE;
    case WM_LBUTTONDOWN:
      dragging_ = true;
      SetCapture(hwnd_);
      if (on_mouse) on_mouse(MouseEvent::Down, screen_point());
      return 0;
    case WM_LBUTTONDBLCLK:
      if (on_mouse) on_mouse(MouseEvent::Double, screen_point());
      return 0;
    case WM_MOUSEMOVE:
      if (on_mouse) on_mouse(MouseEvent::Move, screen_point());
      return 0;
    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
        if (on_mouse) on_mouse(MouseEvent::Up, screen_point());
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (dragging_) {
        dragging_ = false;
        POINT p;
        GetCursorPos(&p);
        if (on_mouse) on_mouse(MouseEvent::Up, p);
      }
      return 0;
    case WM_HOTKEY:
      if (on_hotkey) on_hotkey();
      return 0;
    case WM_CLOSE:
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace sp
