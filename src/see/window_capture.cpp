#include "see/window_capture.hpp"

#include "core/util.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <unknwn.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>
#include <cstring>

namespace sp {

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

struct WindowCapture::Impl {
  winrt::com_ptr<ID3D11Device> d3d;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  wdx::Direct3D11::IDirect3DDevice device{nullptr};
  wgc::GraphicsCaptureItem item{nullptr};
  wgc::Direct3D11CaptureFramePool pool{nullptr};
  wgc::GraphicsCaptureSession session{nullptr};
  winrt::Windows::Graphics::SizeInt32 size{};
  winrt::com_ptr<ID3D11Texture2D> staging;
  int sw = 0, sh = 0;
};

WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() { stop(); }

bool WindowCapture::active() const { return impl_->session != nullptr; }

bool WindowCapture::allow_borderless() {
  try {
    const auto status = wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
    return status == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed;
  } catch (...) {
    return false;
  }
}

bool WindowCapture::start(HWND hwnd) {
  stop();
  Impl& m = *impl_;
  try {
    if (!m.d3d) {
      if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                   0, D3D11_SDK_VERSION, m.d3d.put(), nullptr, m.ctx.put())))
        return false;
      auto dxgi = m.d3d.as<IDXGIDevice>();
      winrt::com_ptr<::IInspectable> inspectable;
      if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()))) return false;
      m.device = inspectable.as<wdx::Direct3D11::IDirect3DDevice>();
    }
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m.item))) ||
        !m.item)
      return false;
    m.size = m.item.Size();
    m.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(m.device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                                 2, m.size);
    m.session = m.pool.CreateCaptureSession(m.item);
    try {
      m.session.IsCursorCaptureEnabled(false);
    } catch (...) {
    }
    try {
      m.session.IsBorderRequired(false);
    } catch (...) {
      // Older Windows: the yellow border stays. Capture still works.
    }
    m.session.StartCapture();
    return true;
  } catch (const winrt::hresult_error& e) {
    debug_log("window capture failed: " + utf8(std::wstring(e.message())));
    stop();
    return false;
  }
}

void WindowCapture::stop() {
  Impl& m = *impl_;
  try {
    if (m.session) m.session.Close();
    if (m.pool) m.pool.Close();
  } catch (...) {
  }
  m.session = nullptr;
  m.pool = nullptr;
  m.item = nullptr;
}

bool WindowCapture::grab(const RECT& region, std::vector<uint32_t>& out, int& w, int& h) {
  Impl& m = *impl_;
  if (!m.pool) return false;
  try {
    // Only the newest frame matters.
    wgc::Direct3D11CaptureFrame frame{nullptr};
    for (;;) {
      auto next = m.pool.TryGetNextFrame();
      if (!next) break;
      if (frame) frame.Close();
      frame = next;
    }
    if (!frame) return false;
    const auto content = frame.ContentSize();
    if (content.Width != m.size.Width || content.Height != m.size.Height) {
      m.size = content;
      m.pool.Recreate(m.device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m.size);
    }
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), tex.put_void()))) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const LONG max_w = std::min<LONG>(static_cast<LONG>(td.Width), content.Width);
    const LONG max_h = std::min<LONG>(static_cast<LONG>(td.Height), content.Height);
    RECT r{std::max<LONG>(0, region.left), std::max<LONG>(0, region.top), std::min(max_w, region.right),
           std::min(max_h, region.bottom)};
    const int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw < 16 || rh < 16) {
      frame.Close();
      return false;
    }
    if (!m.staging || m.sw != rw || m.sh != rh) {
      D3D11_TEXTURE2D_DESC sd{};
      sd.Width = rw;
      sd.Height = rh;
      sd.MipLevels = 1;
      sd.ArraySize = 1;
      sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      sd.SampleDesc.Count = 1;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      m.staging = nullptr;
      if (FAILED(m.d3d->CreateTexture2D(&sd, nullptr, m.staging.put()))) return false;
      m.sw = rw;
      m.sh = rh;
    }
    const D3D11_BOX box{static_cast<UINT>(r.left), static_cast<UINT>(r.top), 0, static_cast<UINT>(r.right),
                        static_cast<UINT>(r.bottom), 1};
    m.ctx->CopySubresourceRegion(m.staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
    frame.Close();
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m.ctx->Map(m.staging.get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out.resize(static_cast<size_t>(rw) * rh);
    for (int y = 0; y < rh; ++y)
      std::memcpy(&out[static_cast<size_t>(y) * rw], static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                  static_cast<size_t>(rw) * 4);
    m.ctx->Unmap(m.staging.get(), 0);
    w = rw;
    h = rh;
    return true;
  } catch (const winrt::hresult_error& e) {
    debug_log("window capture frame failed: " + utf8(std::wstring(e.message())));
    return false;
  }
}

}  // namespace sp
