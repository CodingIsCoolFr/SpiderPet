#include "render/paint.hpp"

using Microsoft::WRL::ComPtr;

namespace sp {

D2D1_COLOR_F rgb(uint32_t hex, float a) {
  return D2D1::ColorF(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) {
  c.a *= a;
  return c;
}

// Colors sampled from the reference clip: salmon legs, mint joints, a
// periwinkle body box, a magenta head and tether.
Palette make_palette(bool dark) {
  Palette p;
  p.dark = dark;
  if (dark) {
    p.leg = rgb(0xEE7A68);
    p.joint = rgb(0x8BF5A6);
    p.body = rgb(0x6173F2);
    p.body_fill = rgb(0x0A0C1A, 0.92f);
    p.head = rgb(0xE33CD2);
    p.tether = rgb(0xDA3CEC);
    p.name = rgb(0xF4F6FF);
    p.name_stroke = rgb(0x000000, 0.72f);
    p.caption = rgb(0x8FD0FF);
    p.silk = rgb(0xE8EEFF, 0.17f);
    p.green = rgb(0x93F5AE);
    p.blue = rgb(0x7D8BFF);
    p.magenta = rgb(0xE64CF2);
    p.red = rgb(0xFF5C5C);
    p.amber = rgb(0xFFC46B);
    p.cyan = rgb(0x7FD6FF);
    p.salmon = rgb(0xF2836B);
    p.chip = rgb(0x4E8FF0);
    p.grey = rgb(0x9AA0B4);
    p.ink = rgb(0x1E120C);
  } else {
    p.leg = rgb(0xE0604C);
    p.joint = rgb(0x22C463);
    p.body = rgb(0x4152E0);
    p.body_fill = rgb(0x10122A, 0.9f);
    p.head = rgb(0xD02CC0);
    p.tether = rgb(0xC42AD8);
    p.name = rgb(0x14151C);
    p.name_stroke = rgb(0xFFFFFF, 0.85f);
    p.caption = rgb(0x1468C8);
    p.silk = rgb(0x141828, 0.16f);
    p.green = rgb(0x0E9A4C);
    p.blue = rgb(0x3346E0);
    p.magenta = rgb(0xB414C8);
    p.red = rgb(0xD42A2A);
    p.amber = rgb(0xB86A00);
    p.cyan = rgb(0x0B7FB8);
    p.salmon = rgb(0xF2836B);
    p.chip = rgb(0x2F6FE0);
    p.grey = rgb(0x5C6275);
    p.ink = rgb(0x1E120C);
  }
  return p;
}

static bool has_family(IDWriteFactory1* dw, const wchar_t* name) {
  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
  UINT32 index = 0;
  BOOL exists = FALSE;
  fonts->FindFamilyName(name, &index, &exists);
  return exists != FALSE;
}

void Painter::attach(ID2D1DeviceContext* ctx, ID2D1Factory1* factory, IDWriteFactory1* dwrite) {
  if (ctx_ == ctx) return;
  ctx_ = ctx;
  factory_ = factory;
  dwrite_ = dwrite;
  ctx_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
  const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
      D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
  factory_->CreateStrokeStyle(props, nullptr, 0, &round_);
  if (has_family(dwrite_, L"Cascadia Mono")) fonts_.mono = L"Cascadia Mono";
  if (has_family(dwrite_, L"Segoe UI Variable Text")) fonts_.sans = L"Segoe UI Variable Text";
}

ID2D1SolidColorBrush* Painter::brush(D2D1_COLOR_F c) {
  brush_->SetColor(c);
  return brush_.Get();
}

void Painter::line(Vec2 a, Vec2 b, D2D1_COLOR_F c, float width) {
  ctx_->DrawLine({a.x, a.y}, {b.x, b.y}, brush(c), width, round_.Get());
}

void Painter::dot(Vec2 p, float r, D2D1_COLOR_F c) {
  ctx_->FillEllipse(D2D1::Ellipse({p.x, p.y}, r, r), brush(c));
}

void Painter::fill(const Rect& r, D2D1_COLOR_F c) {
  ctx_->FillRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush(c));
}

void Painter::stroke(const Rect& r, D2D1_COLOR_F c, float width) {
  ctx_->DrawRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush(c), width);
}

void Painter::curve(Vec2 a, Vec2 ctrl, Vec2 b, D2D1_COLOR_F c, float width, float upto) {
  // Short polyline is plenty for a hair-thin silk line.
  constexpr int kSteps = 14;
  const int steps = std::max(1, static_cast<int>(kSteps * std::clamp(upto, 0.f, 1.f)));
  Vec2 prev = a;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / kSteps;
    const Vec2 p = a * ((1 - t) * (1 - t)) + ctrl * (2 * (1 - t) * t) + b * (t * t);
    line(prev, p, c, width);
    prev = p;
  }
}

IDWriteTextFormat* Painter::format(const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight,
                                   DWRITE_FONT_STYLE style) {
  const int key_size = static_cast<int>(size * 4.f + 0.5f);
  auto key = std::make_tuple(family, key_size, static_cast<int>(weight), static_cast<int>(style));
  auto it = formats_.find(key);
  if (it != formats_.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> fmt;
  dwrite_->CreateTextFormat(family.c_str(), nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL,
                            std::max(4.f, key_size / 4.f), L"en-us", &fmt);
  if (fmt) {
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  }
  formats_[key] = fmt;
  return fmt.Get();
}

ComPtr<IDWriteTextLayout> Painter::layout(const std::wstring& text, IDWriteTextFormat* fmt, float letter_spacing) {
  ComPtr<IDWriteTextLayout> out;
  if (!fmt) return out;
  dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 8192.f, 512.f, &out);
  if (out && letter_spacing != 0) {
    ComPtr<IDWriteTextLayout1> l1;
    if (SUCCEEDED(out.As(&l1)))
      l1->SetCharacterSpacing(0, letter_spacing, 0, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
  }
  return out;
}

void Painter::outlined(IDWriteTextLayout* layout, Vec2 tl, D2D1_COLOR_F fill, D2D1_COLOR_F halo, float halo_px) {
  if (!layout) return;
  constexpr float kDirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
  auto* b = brush(halo);
  for (const auto& d : kDirs) ctx_->DrawTextLayout({tl.x + d[0] * halo_px, tl.y + d[1] * halo_px}, layout, b);
  ctx_->DrawTextLayout({tl.x, tl.y}, layout, brush(fill));
}

void Painter::spider(const Spider& s, const Palette& pal, float alpha) {
  const float k = s.scale();
  const Vec2 c = s.draw_pos();
  const float deg = s.heading() * 180.f / kPi;
  D2D1_MATRIX_3X2_F base;
  ctx_->GetTransform(&base);
  ctx_->SetTransform(D2D1::Matrix3x2F::Rotation(deg, {c.x, c.y}) * D2D1::Matrix3x2F::ReinterpretBaseType(&base)[0]);
  const float L = s.body_len(), W = s.body_wid();
  const Rect body{c.x - L * 0.5f, c.y - W * 0.5f, L, W};
  fill(body, with_alpha(pal.body_fill, alpha));
  stroke(body, with_alpha(pal.body, alpha), std::max(1.2f, 1.7f * k));
  ctx_->SetTransform(base);

  const float leg_w = std::max(1.1f, 1.9f * k);
  for (const Leg& l : s.legs()) {
    for (int i = 0; i < 3; ++i) line(l.j[i], l.j[i + 1], with_alpha(pal.leg, alpha), leg_w);
  }
  const float r = std::max(1.8f, 3.1f * k);
  for (const Leg& l : s.legs()) {
    dot(l.j[1], r, with_alpha(pal.joint, alpha));
    dot(l.j[2], r, with_alpha(pal.joint, alpha));
    dot(l.j[3], r * (1.f + 0.35f * l.lift), with_alpha(pal.joint, alpha));
  }
  dot(s.head(), std::max(2.2f, 3.7f * k), with_alpha(pal.head, alpha));
}

}  // namespace sp
