#include "see/ocr.hpp"

#include "core/util.hpp"

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace sp {

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;

void join_mta() {
  try {
    init_apartment(apartment_type::multi_threaded);
  } catch (...) {
    // Already joined, or the thread is STA: OCR still works through the proxy.
  }
}

struct TextReader::Impl {
  OcrEngine engine{nullptr};
  float word_h = 0;  // median word height last time, picks the upscale
};

TextReader::TextReader() : impl_(std::make_unique<Impl>()) {
  try {
    impl_->engine = OcrEngine::TryCreateFromUserProfileLanguages();
    if (!impl_->engine)
      impl_->engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(L"en-US"));
  } catch (...) {
    impl_->engine = nullptr;
  }
}

TextReader::~TextReader() = default;

bool TextReader::ready() const { return impl_->engine != nullptr; }

std::wstring TextReader::language() const {
  if (!impl_->engine) return {};
  return std::wstring(impl_->engine.RecognizerLanguage().LanguageTag());
}

namespace {

uint32_t to_rgb(uint32_t bgra) { return bgra & 0xFFFFFF; }

int color_dist(uint32_t a, uint32_t b) {
  const int dr = int((a >> 16) & 0xFF) - int((b >> 16) & 0xFF);
  const int dg = int((a >> 8) & 0xFF) - int((b >> 8) & 0xFF);
  const int db = int(a & 0xFF) - int(b & 0xFF);
  return std::abs(dr) + std::abs(dg) + std::abs(db);
}

// Background = the most common color in a thin ring around the box.
// Foreground = the average of the pixels inside that differ most from it.
void sample_colors(const Frame& f, const Rect& box, uint32_t& fg, uint32_t& bg) {
  const int x0 = std::clamp(static_cast<int>(box.x) - 2, 0, f.w - 1);
  const int y0 = std::clamp(static_cast<int>(box.y) - 2, 0, f.h - 1);
  const int x1 = std::clamp(static_cast<int>(box.right()) + 2, 0, f.w - 1);
  const int y1 = std::clamp(static_cast<int>(box.bottom()) + 2, 0, f.h - 1);
  static thread_local std::array<uint16_t, 4096> bins;
  bins.fill(0);
  uint32_t best = 0;
  uint16_t best_n = 0;
  auto vote = [&](uint32_t c) {
    const uint32_t q = ((c >> 20) & 0xF) << 8 | ((c >> 12) & 0xF) << 4 | ((c >> 4) & 0xF);
    if (++bins[q] > best_n) {
      best_n = bins[q];
      best = c;
    }
  };
  for (int x = x0; x <= x1; ++x) {
    vote(to_rgb(f.at(x, y0)));
    vote(to_rgb(f.at(x, y1)));
  }
  for (int y = y0; y <= y1; ++y) {
    vote(to_rgb(f.at(x0, y)));
    vote(to_rgb(f.at(x1, y)));
  }
  bg = best;
  int max_d = 0;
  for (int y = y0 + 2; y <= y1 - 2; ++y)
    for (int x = x0 + 2; x <= x1 - 2; ++x) max_d = std::max(max_d, color_dist(to_rgb(f.at(x, y)), bg));
  if (max_d < 40) {
    fg = luma(bg) < 0.5f ? 0xE8E8E8 : 0x202020;
    return;
  }
  uint64_t r = 0, g = 0, b = 0, n = 0;
  const int cut = max_d * 3 / 5;
  for (int y = y0 + 2; y <= y1 - 2; ++y)
    for (int x = x0 + 2; x <= x1 - 2; ++x) {
      const uint32_t c = to_rgb(f.at(x, y));
      if (color_dist(c, bg) < cut) continue;
      r += (c >> 16) & 0xFF;
      g += (c >> 8) & 0xFF;
      b += c & 0xFF;
      ++n;
    }
  fg = n ? static_cast<uint32_t>((r / n) << 16 | (g / n) << 8 | (b / n)) : 0xE8E8E8;
}

}  // namespace

bool TextReader::read(const Frame& frame, OcrPass& out) {
  if (!impl_->engine || frame.w < 16 || frame.h < 16) return false;
  const double t0 = now_seconds();

  // Windows OCR reads dark text on light paper best. Flip dark pages.
  double sum = 0;
  int n = 0;
  for (size_t i = 0; i < frame.px.size(); i += 97) {
    sum += luma(frame.px[i]);
    ++n;
  }
  const bool dark = n > 0 && sum / n < 0.42;

  // Small screen text (under ~16 px) reads much better at twice the size.
  const bool fine_print = impl_->word_h <= 0 || impl_->word_h < 20.f;
  const int k = (fine_print && frame.w * 2 <= 9000 && frame.h * 2 <= 9000) ? 2 : 1;
  const int W = frame.w * k, H = frame.h * k;
  const uint32_t bytes = static_cast<uint32_t>(W) * H * 4;
  winrt::Windows::Storage::Streams::Buffer buffer(bytes);
  auto* dst = reinterpret_cast<uint32_t*>(buffer.data());
  auto prep = [dark](uint32_t c) { return (dark ? (~c & 0x00FFFFFF) : (c & 0x00FFFFFF)); };
  auto avg2 = [](uint32_t a, uint32_t b) { return ((a & 0xFEFEFE) >> 1) + ((b & 0xFEFEFE) >> 1); };
  for (int y = 0; y < frame.h; ++y) {
    const uint32_t* row = &frame.px[static_cast<size_t>(y) * frame.w];
    const uint32_t* next = &frame.px[static_cast<size_t>(std::min(y + 1, frame.h - 1)) * frame.w];
    for (int x = 0; x < frame.w; ++x) {
      const uint32_t a = prep(row[x]);
      if (k == 1) {
        dst[static_cast<size_t>(y) * W + x] = a | 0xFF000000u;
        continue;
      }
      const int x1 = std::min(x + 1, frame.w - 1);
      const uint32_t b = prep(row[x1]), c = prep(next[x]), d = prep(next[x1]);
      const size_t o = static_cast<size_t>(y * 2) * W + x * 2;
      dst[o] = a | 0xFF000000u;
      dst[o + 1] = avg2(a, b) | 0xFF000000u;
      dst[o + W] = avg2(a, c) | 0xFF000000u;
      dst[o + W + 1] = avg2(avg2(a, b), avg2(c, d)) | 0xFF000000u;
    }
  }
  buffer.Length(bytes);
  const float inv = 1.f / static_cast<float>(k);

  try {
    SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(buffer, BitmapPixelFormat::Bgra8, W, H,
                                                                 BitmapAlphaMode::Premultiplied);
    OcrResult result = impl_->engine.RecognizeAsync(bitmap).get();
    out.lines.clear();
    for (const winrt::Windows::Media::Ocr::OcrLine& src : result.Lines()) {
      sp::OcrLine line;
      for (const winrt::Windows::Media::Ocr::OcrWord& w : src.Words()) {
        const auto r = w.BoundingRect();
        sp::OcrWord word;
        word.text = std::wstring(w.Text());
        word.box = {r.X * inv, r.Y * inv, r.Width * inv, r.Height * inv};
        sample_colors(frame, word.box, word.fg, word.bg);
        line.box = unite(line.box, word.box);
        if (!line.text.empty()) line.text += L' ';
        line.text += word.text;
        line.words.push_back(std::move(word));
      }
      if (line.words.empty()) continue;
      sample_colors(frame, line.box, line.fg, line.bg);
      out.lines.push_back(std::move(line));
    }
  } catch (const winrt::hresult_error& e) {
    debug_log("ocr failed: " + utf8(std::wstring(e.message())));
    return false;
  }
  std::vector<float> hs;
  for (const auto& l : out.lines)
    for (const auto& w : l.words) hs.push_back(w.box.h);
  if (!hs.empty()) {
    std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
    impl_->word_h = hs[hs.size() / 2];
  }
  out.w = frame.w;
  out.h = frame.h;
  out.scroll = frame.scroll;
  out.time = frame.time;
  out.took = now_seconds() - t0;
  return true;
}

}  // namespace sp
