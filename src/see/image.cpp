#include "see/image.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace sp {

static ComPtr<IWICImagingFactory> wic() {
  ComPtr<IWICImagingFactory> f;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  return f;
}

bool load_image(const std::wstring& path, Frame& out) {
  auto f = wic();
  if (!f) return false;
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)))
    return false;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) return false;
  ComPtr<IWICFormatConverter> conv;
  f->CreateFormatConverter(&conv);
  if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom)))
    return false;
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  out.w = static_cast<int>(w);
  out.h = static_cast<int>(h);
  out.px.resize(static_cast<size_t>(w) * h);
  return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(out.px.data())));
}

bool save_png(const std::wstring& path, int w, int h, const uint32_t* bgra, int stride_px) {
  auto f = wic();
  if (!f) return false;
  ComPtr<IWICStream> stream;
  f->CreateStream(&stream);
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> enc;
  f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
  enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> frame;
  enc->CreateNewFrame(&frame, nullptr);
  frame->Initialize(nullptr);
  frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&fmt);
  frame->WritePixels(static_cast<UINT>(h), static_cast<UINT>(stride_px * 4), static_cast<UINT>(stride_px * 4 * h),
                     reinterpret_cast<BYTE*>(const_cast<uint32_t*>(bgra)));
  frame->Commit();
  return SUCCEEDED(enc->Commit());
}

}  // namespace sp
