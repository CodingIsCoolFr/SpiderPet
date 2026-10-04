#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <dwrite_1.h>
#include <wrl/client.h>

#include <functional>

namespace sp {

// A borderless, topmost, click-through window drawn with Direct2D through
// DirectComposition, so it has real per-pixel alpha.
class Overlay {
 public:
  enum class MouseEvent { Down, Move, Up, Double };

  bool create(HINSTANCE inst);
  void destroy();
  HWND hwnd() const { return hwnd_; }

  void cover(const RECT& monitor);
  RECT bounds() const { return bounds_; }
  void set_interactive(bool on);
  void set_visible(bool on);
  bool visible() const { return visible_; }
  bool interactive() const { return interactive_; }
  void set_capturable(bool on);  // false hides it from screen capture

  ID2D1DeviceContext* begin();
  void end();
  ID2D1Factory1* d2d() const { return factory_.Get(); }
  IDWriteFactory1* dwrite() const { return dwrite_.Get(); }

  std::function<void(MouseEvent, POINT)> on_mouse;
  std::function<void()> on_hotkey;
  bool grab_cursor = false;

 private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  bool make_target();

  HWND hwnd_ = nullptr;
  RECT bounds_{};
  bool interactive_ = false;
  bool visible_ = true;
  bool capturable_ = true;
  bool dragging_ = false;
  UINT interval_ = 1;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
  Microsoft::WRL::ComPtr<ID2D1Device> device_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
  Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
  Microsoft::WRL::ComPtr<IDCompositionTarget> dtarget_;
  Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
  Microsoft::WRL::ComPtr<IDWriteFactory1> dwrite_;
};

}  // namespace sp
