#pragma once

#include "core/geom.hpp"
#include "pet/spider.hpp"

#include <d2d1_1.h>
#include <dwrite_1.h>
#include <wrl/client.h>

#include <map>
#include <string>
#include <tuple>

namespace sp {

struct Palette {
  bool dark = true;
  D2D1_COLOR_F leg, joint, body, body_fill, head, tether, name, name_stroke, caption, silk;
  D2D1_COLOR_F green, blue, magenta, red, amber, cyan, salmon, chip, grey, ink;
};

Palette make_palette(bool dark);
D2D1_COLOR_F rgb(uint32_t hex, float a = 1.f);
D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a);

// Fonts the restyled text can use. Families are checked once against the
// system collection, so a missing Cascadia Mono falls back to Consolas.
struct Fonts {
  std::wstring serif = L"Georgia";
  std::wstring mono = L"Consolas";
  std::wstring sans = L"Segoe UI";
};

class Painter {
 public:
  void attach(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, IDWriteFactory1* dwrite);
  ID2D1DeviceContext* ctx() const { return ctx_; }
  ID2D1Factory1* factory() const { return factory_; }
  IDWriteFactory1* dwrite() const { return dwrite_; }
  const Fonts& fonts() const { return fonts_; }

  ID2D1SolidColorBrush* brush(D2D1_COLOR_F c);
  void line(Vec2 a, Vec2 b, D2D1_COLOR_F c, float width);
  void dot(Vec2 p, float r, D2D1_COLOR_F c);
  void fill(const Rect& r, D2D1_COLOR_F c);
  void stroke(const Rect& r, D2D1_COLOR_F c, float width);
  void curve(Vec2 a, Vec2 ctrl, Vec2 b, D2D1_COLOR_F c, float width, float upto = 1.f);

  IDWriteTextFormat* format(const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight,
                            DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL);
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout(const std::wstring& text, IDWriteTextFormat* fmt,
                                                   float letter_spacing = 0);
  // Text with a dark halo so it reads on any page.
  void outlined(IDWriteTextLayout* layout, Vec2 top_left, D2D1_COLOR_F fill, D2D1_COLOR_F halo, float halo_px);

  void spider(const Spider& s, const Palette& pal, float alpha);

 private:
  ID2D1DeviceContext* ctx_ = nullptr;
  ID2D1Factory1* factory_ = nullptr;
  IDWriteFactory1* dwrite_ = nullptr;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_;
  std::map<std::tuple<std::wstring, int, int, int>, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
  Fonts fonts_;
};

}  // namespace sp
