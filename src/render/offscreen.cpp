#include "render/offscreen.hpp"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sp {

bool Offscreen::create(int w, int h) {
  w_ = w;
  h_ = h;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &d3d_, nullptr, nullptr)))
    return false;
  ComPtr<IDXGIDevice> dxgi;
  d3d_.As(&dxgi);
  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(factory_.GetAddressOf()))))
    return false;
  if (FAILED(factory_->CreateDevice(dxgi.Get(), &device_))) return false;
  if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_))) return false;
  const auto fmt = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, fmt, 96.f, 96.f);
  if (FAILED(ctx_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &target_))) return false;
  auto rb = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, fmt, 96.f, 96.f);
  if (FAILED(ctx_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &rb, &readback_))) return false;
  ctx_->SetTarget(target_.Get());
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1),
                      reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
  return true;
}

ID2D1DeviceContext* Offscreen::begin(const Frame& bg) {
  ctx_->BeginDraw();
  ctx_->SetTransform(D2D1::Matrix3x2F::Identity());
  ctx_->Clear(D2D1::ColorF(0, 0, 0, 1));
  if (bg.w > 0 && bg.h > 0) {
    ComPtr<ID2D1Bitmap> bmp;
    const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (SUCCEEDED(ctx_->CreateBitmap(D2D1::SizeU(bg.w, bg.h), bg.px.data(), bg.w * 4, &props, &bmp)))
      ctx_->DrawBitmap(bmp.Get(), D2D1::RectF(0, 0, static_cast<float>(bg.w), static_cast<float>(bg.h)));
  }
  return ctx_.Get();
}

bool Offscreen::end(Frame& out) {
  if (FAILED(ctx_->EndDraw())) return false;
  D2D1_POINT_2U origin{0, 0};
  D2D1_RECT_U src{0, 0, static_cast<UINT32>(w_), static_cast<UINT32>(h_)};
  if (FAILED(readback_->CopyFromBitmap(&origin, target_.Get(), &src))) return false;
  D2D1_MAPPED_RECT m{};
  if (FAILED(readback_->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
  out.w = w_;
  out.h = h_;
  out.px.resize(static_cast<size_t>(w_) * h_);
  for (int y = 0; y < h_; ++y) std::memcpy(&out.px[static_cast<size_t>(y) * w_], m.bits + y * m.pitch, w_ * 4);
  readback_->Unmap();
  return true;
}

}  // namespace sp
