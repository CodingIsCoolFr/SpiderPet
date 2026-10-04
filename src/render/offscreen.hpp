#pragma once

#include "see/frame.hpp"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_1.h>
#include <wrl/client.h>

namespace sp {

// Direct2D into a bitmap instead of a window. Used by the simulator to film
// the spider over a page image without putting anything on screen.
class Offscreen {
 public:
  bool create(int w, int h);
  ID2D1DeviceContext* begin(const Frame& background);
  bool end(Frame& out);
  ID2D1Factory1* d2d() const { return factory_.Get(); }
  IDWriteFactory1* dwrite() const { return dwrite_.Get(); }

 private:
  int w_ = 0, h_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
  Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
  Microsoft::WRL::ComPtr<ID2D1Device> device_;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_, readback_;
  Microsoft::WRL::ComPtr<IDWriteFactory1> dwrite_;
};

}  // namespace sp
