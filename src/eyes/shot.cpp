#include "eyes/shot.hpp"

#include "core/util.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace sp::eyes {

namespace {
std::atomic<void (*)(bool)> g_guard{nullptr};
}

void set_grab_guard(void (*guard)(bool)) { g_guard = guard; }

Pixels grab(HWND h, const RECT* region) {
  Pixels px;
  RECT r{};
  if (region) r = *region;
  else if (!GetWindowRect(h, &r)) return px;
  int w = r.right - r.left, ht = r.bottom - r.top;
  if (w <= 0 || ht <= 0 || w > 16384 || ht > 16384) return px;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -ht;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return px;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  // In front: what you see, exactly. In the back: ask the window to draw itself.
  HWND fg = GetForegroundWindow();
  bool front = !region && h && (fg == h || GetAncestor(fg, GA_ROOTOWNER) == h);
  bool ok;
  if (region || front) {
    // A copy of the screen would hold the spider too: it steps out for a moment.
    auto guard = g_guard.load();
    if (guard) {
      guard(true);
      Sleep(45);  // the compositor needs a frame
    }
    ok = BitBlt(mem, 0, 0, w, ht, screen, r.left, r.top, SRCCOPY) != 0;
    if (guard) guard(false);
  } else {
    ok = PrintWindow(h, mem, PW_RENDERFULLCONTENT) != 0;
  }
  if (ok) {
    px.w = w;
    px.h = ht;
    px.bgra.assign(static_cast<uint8_t*>(bits), static_cast<uint8_t*>(bits) + size_t(w) * ht * 4);
    for (size_t i = 3; i < px.bgra.size(); i += 4) px.bgra[i] = 255;
    px.on_screen = r;
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return px;
}

std::string jpeg_base64(const Pixels& px, int max_side, int quality) {
  if (!px.ok()) return {};
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return {};
  ComPtr<IWICBitmap> src;
  if (FAILED(f->CreateBitmapFromMemory(UINT(px.w), UINT(px.h), GUID_WICPixelFormat32bppBGRA, UINT(px.w * 4), UINT(px.bgra.size()),
                                       const_cast<BYTE*>(px.bgra.data()), &src)))
    return {};
  ComPtr<IWICBitmapSource> img = src;
  double k = std::min(1.0, double(max_side) / std::max(px.w, px.h));
  UINT tw = UINT(std::max(1.0, px.w * k)), th = UINT(std::max(1.0, px.h * k));
  if (k < 1.0) {
    ComPtr<IWICBitmapScaler> sc;
    if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeHighQualityCubic))) img = sc;
  }
  ComPtr<IWICFormatConverter> conv;
  if (FAILED(f->CreateFormatConverter(&conv)) ||
      FAILED(conv->Initialize(img.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
    return {};
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
  ComPtr<IWICBitmapEncoder> enc;
  if (FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return {};
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> props;
  if (FAILED(enc->CreateNewFrame(&frame, &props))) return {};
  PROPBAG2 opt{};
  opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
  VARIANT q;
  VariantInit(&q);
  q.vt = VT_R4;
  q.fltVal = std::clamp(quality, 30, 100) / 100.0f;
  props->Write(1, &opt, &q);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
  if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(tw, th)) || FAILED(frame->SetPixelFormat(&fmt)) ||
      FAILED(frame->WriteSource(conv.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
    return {};
  HGLOBAL hg = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &hg))) return {};
  STATSTG stat{};
  stream->Stat(&stat, STATFLAG_NONAME);
  size_t n = size_t(stat.cbSize.QuadPart);
  const uint8_t* data = static_cast<const uint8_t*>(GlobalLock(hg));
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(data[i]) << 16 | (i + 1 < n ? uint32_t(data[i + 1]) << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < n ? tbl[(v >> 6) & 63] : '=';
    out += i + 2 < n ? tbl[v & 63] : '=';
  }
  GlobalUnlock(hg);
  return out;
}

std::vector<OcrLine> read_pixels(const Pixels& px) {
  std::vector<OcrLine> out;
  if (!px.ok()) return out;
  try {
    namespace img = winrt::Windows::Graphics::Imaging;
    namespace ocr = winrt::Windows::Media::Ocr;
    ocr::OcrEngine engine = ocr::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) return out;
    uint32_t maxd = ocr::OcrEngine::MaxImageDimension();
    // Too big for the OCR: read the top-left part that fits (the start of the text).
    int w = std::min<int>(px.w, int(maxd)), h = std::min<int>(px.h, int(maxd));
    img::SoftwareBitmap bmp(img::BitmapPixelFormat::Bgra8, w, h, img::BitmapAlphaMode::Premultiplied);
    winrt::Windows::Storage::Streams::Buffer buf(uint32_t(w) * h * 4);
    uint8_t* dst = buf.data();
    for (int y = 0; y < h; ++y) memcpy(dst + size_t(y) * w * 4, px.bgra.data() + size_t(y) * px.w * 4, size_t(w) * 4);
    buf.Length(uint32_t(w) * h * 4);
    bmp.CopyFromBuffer(buf);
    ocr::OcrResult res = engine.RecognizeAsync(bmp).get();
    for (auto line : res.Lines()) {
      sp::eyes::OcrLine l;
      l.text = winrt::to_string(line.Text());
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (auto word : line.Words()) {
        auto b = word.BoundingRect();
        x0 = std::min(x0, b.X);
        y0 = std::min(y0, b.Y);
        x1 = std::max(x1, b.X + b.Width);
        y1 = std::max(y1, b.Y + b.Height);
        l.words.push_back({winrt::to_string(word.Text()), RECT{px.on_screen.left + LONG(b.X), px.on_screen.top + LONG(b.Y),
                                                            px.on_screen.left + LONG(b.X + b.Width), px.on_screen.top + LONG(b.Y + b.Height)}});
      }
      if (x1 < x0) continue;
      l.r = RECT{px.on_screen.left + LONG(x0), px.on_screen.top + LONG(y0), px.on_screen.left + LONG(x1), px.on_screen.top + LONG(y1)};
      out.push_back(std::move(l));
    }
  } catch (...) {
    debug_log("ocr: Windows OCR failed");
  }
  return out;
}

}  // namespace sp::eyes
