#include "render/scene.hpp"

#include "core/util.hpp"

using Microsoft::WRL::ComPtr;

namespace sp {
namespace {

D2D1_COLOR_F hex(uint32_t c, float a = 1.f) { return rgb(c, a); }

Rect off(const Rect& r, Vec2 o) { return r.moved(o.x, o.y); }

}  // namespace

// Every kind of find gets its own look, in the spirit of the reference: DOIs
// go green monospace, ISBNs big blue monospace, titles salmon serif, links
// get a colored box. The variant picks one of a few looks per kind.
Scene::Style Scene::style_for(const Entity& e, const Palette& pal) const {
  Style s;
  const int v = e.variant;
  switch (e.kind) {
    case Kind::Title:
      if (v == 0) {
        s.family = 0, s.size = 1.06f, s.italic = DWRITE_FONT_STYLE_ITALIC, s.color = pal.ink;
        s.fill = true, s.fill_color = pal.salmon;
      } else if (v == 1) {
        s.family = 0, s.size = 1.04f, s.color = pal.blue, s.outline = true, s.outline_color = pal.blue;
      } else {
        s.family = 2, s.size = 1.25f, s.weight = DWRITE_FONT_WEIGHT_SEMI_BOLD, s.color = pal.magenta;
      }
      break;
    case Kind::Doi:
      if (v == 0) s.family = 1, s.size = 1.08f, s.spacing = 1.f, s.color = pal.green;
      else if (v == 1) s.family = 1, s.size = 1.f, s.color = pal.grey, s.outline = true, s.outline_color = pal.magenta;
      else s.family = 1, s.size = 1.35f, s.spacing = 2.f, s.color = pal.green;
      break;
    case Kind::Isbn:
      if (v == 0) s.family = 1, s.size = 1.45f, s.spacing = 1.f, s.color = pal.blue;
      else if (v == 1) s.family = 1, s.size = 1.2f, s.spacing = 1.f, s.color = pal.blue, s.outline = true, s.outline_color = pal.blue;
      else s.family = 1, s.size = 1.3f, s.color = pal.cyan;
      break;
    case Kind::Id:
      if (v == 0) s.family = 1, s.size = 1.05f, s.color = hex(0x0D1430), s.fill = true, s.fill_color = pal.chip;
      else if (v == 1) s.family = 1, s.size = 1.f, s.color = pal.magenta, s.outline = true, s.outline_color = pal.magenta;
      else s.family = 0, s.size = 1.3f, s.color = pal.green;
      break;
    case Kind::Link:
      s.family = 0, s.size = 1.02f, s.outline = true;
      if (v == 0) s.color = pal.blue, s.outline_color = pal.blue;
      else if (v == 1) s.color = pal.magenta, s.outline_color = pal.green;
      else s.color = pal.cyan, s.outline_color = pal.magenta;
      break;
    case Kind::Url:
    case Kind::Email:
      s.family = 1, s.color = pal.cyan, s.outline = true, s.outline_color = pal.cyan;
      break;
    case Kind::Heading:
      s.family = 2, s.size = 1.06f, s.weight = DWRITE_FONT_WEIGHT_BOLD, s.color = pal.magenta;
      break;
    case Kind::Date:
      s.family = 1, s.color = pal.amber;
      break;
    case Kind::Number:
      s.family = 1, s.size = 1.15f, s.spacing = 2.f, s.color = pal.red;
      break;
    case Kind::Handle:
      s.family = 2, s.weight = DWRITE_FONT_WEIGHT_BOLD, s.color = pal.cyan;
      break;
    case Kind::Item:
      s.family = 2, s.size = 1.02f, s.outline = true;
      if (v == 0) s.color = pal.blue, s.outline_color = pal.blue;
      else if (v == 1) s.color = pal.green, s.outline_color = pal.green;
      else s.color = pal.name, s.outline_color = pal.magenta;
      break;
    case Kind::Sentence:
      break;
  }
  return s;
}

// The new look may grow only into empty space. Where other text is close
// (a row of footer links, a dense reference list), it is shrunk to fit the
// original spot so restyled words never pile onto their neighbors.
IDWriteTextLayout* Scene::layout_for(Painter& p, const Page& page, const Entity& e, const Style& st) {
  const Rect& r = e.lines.front();
  const float size = e.font_px * st.size;
  const uint64_t key = (static_cast<uint64_t>(fnv1a(e.text)) << 24) ^ (static_cast<uint64_t>(size * 8.f) << 8) ^
                       (static_cast<uint64_t>(r.w) << 40) ^ static_cast<uint64_t>(st.family * 3 + e.variant);
  Cached& c = layouts_[e.id];
  if (c.layout && c.key == key) return c.layout.Get();
  const Fonts& f = p.fonts();
  const std::wstring& family = st.family == 0 ? f.serif : st.family == 1 ? f.mono : f.sans;
  c.layout = p.layout(e.text, p.format(family, size, st.weight, st.italic), st.spacing);
  c.key = key;
  if (!c.layout) return nullptr;
  DWRITE_TEXT_METRICS m{};
  c.layout->GetMetrics(&m);
  const float w = m.widthIncludingTrailingWhitespace, h = m.height;
  const Rect grown{r.x - 3.f, r.center().y - h * 0.5f, w + 6.f, h};
  bool crowded = false;
  for (const TextLine& l : page.lines()) {
    if (l.box.bottom() < grown.y - 2.f) continue;
    if (l.box.y > grown.bottom() + 2.f) break;
    const bool own = l.box.contains(r.center());
    for (const OcrWord& wd : l.words) {
      if (r.inflated(1.f).contains(wd.box.center())) continue;  // our own words
      const float ov = overlap_area(grown, wd.box);
      if (ov > (own ? 1.f : 0.15f * wd.box.w * wd.box.h)) {
        crowded = true;
        break;
      }
    }
    if (crowded) break;
  }
  if (crowded) {
    const float k = std::clamp(std::min(r.w * 1.04f / std::max(1.f, w), (r.h + 4.f) * 1.15f / std::max(1.f, h)), 0.55f, 1.f);
    if (k < 0.99f) c.layout = p.layout(e.text, p.format(family, size * k, st.weight, st.italic), st.spacing * k);
  }
  return c.layout.Get();
}

void Scene::sentence(Painter& p, const Palette& pal, const Entity& e, Vec2 o, float progress, bool live) {
  float total = 0;
  for (const Rect& r : e.lines) total += r.w;
  float left = std::clamp(progress, 0.f, 1.f) * total;
  const D2D1_COLOR_F c = e.key ? pal.magenta : pal.blue;
  for (const Rect& raw : e.lines) {
    if (left <= 0) break;
    const Rect r = off(raw, o);
    const float w = std::min(left, r.w);
    left -= r.w;
    p.fill({r.x - 1, r.y - 1, w + 2, r.h + 2}, with_alpha(c, live ? 0.18f : 0.09f));
    p.fill({r.x - 1, r.bottom() + 1.5f, w + 2, 1.6f}, with_alpha(c, live ? 0.95f : 0.55f));
  }
}

void Scene::decal(Painter& p, const Palette& pal, const Page& page, const Entity& e, Vec2 o, double now) {
  const Style st = style_for(e, pal);
  const float age = static_cast<float>(now - e.applied_at);
  ID2D1DeviceContext* ctx = p.ctx();

  if (e.lines.size() != 1) {
    // Wrapped across lines: mark it in place, keep the original text.
    for (const Rect& raw : e.lines) {
      const Rect r = off(raw, o).inflated(1.5f);
      p.fill(r, with_alpha(st.fill ? st.fill_color : st.color, 0.2f));
      p.stroke(r, st.outline ? st.outline_color : st.color, 1.4f);
    }
    return;
  }

  const Rect r = off(e.lines.front(), o);
  IDWriteTextLayout* layout = layout_for(p, page, e, st);
  if (!layout) return;
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  const float cy = r.center().y;
  const float pad = (st.fill || st.outline) ? 3.f : 0.f;
  const float bh = std::max(r.h + 4.f, m.height);
  const Rect box{r.x - pad, cy - bh * 0.5f, m.widthIncludingTrailingWhitespace + pad * 2, bh};

  // Paper over the original first, in place, so a tilted copy leaves a gap.
  p.fill({r.x - 2.5f, r.y - 3.f, r.w + 5.f, r.h + 6.f}, hex(e.bg));

  D2D1::Matrix3x2F xf = D2D1::Matrix3x2F::Identity();
  if (e.tilt != 0) {
    xf = D2D1::Matrix3x2F::Rotation(e.tilt * 180.f / kPi, {r.x, cy}) *
         D2D1::Matrix3x2F::Translation(e.nudge.x, e.nudge.y);
  }
  float jitter = 0;
  if (age < 0.35f) {
    const float k = 1.f - age / 0.35f;
    jitter = rng_.range(-3.f, 3.f) * k;
    ctx->SetTransform(xf);
    // Chromatic ghosts and a few torn scanlines, like the reference's glitch.
    ctx->DrawTextLayout({r.x + rng_.range(-7.f, -2.f) * k, cy - m.height * 0.5f}, layout,
                        p.brush(with_alpha(hex(0x00F0FF), 0.6f * k)));
    ctx->DrawTextLayout({r.x + rng_.range(2.f, 7.f) * k, cy - m.height * 0.5f}, layout,
                        p.brush(with_alpha(hex(0xFF2A6D), 0.6f * k)));
    const D2D1_COLOR_F bars[] = {pal.magenta, pal.green, pal.blue, pal.salmon};
    for (int i = 0; i < 4; ++i) {
      const float bw = rng_.range(12.f, 140.f);
      p.fill({r.x + rng_.range(-20.f, r.w), r.y + rng_.range(0.f, r.h), bw, rng_.range(1.5f, 4.f)},
             with_alpha(bars[rng_.below(4)], 0.55f * k));
    }
  }
  ctx->SetTransform(xf * D2D1::Matrix3x2F::Translation(jitter, 0));
  if (st.fill) p.fill(box, st.fill_color);
  else p.fill(box.inflated(-pad + 0.5f), hex(e.bg));
  if (st.outline) p.stroke(box, st.outline_color, 1.5f);
  ctx->DrawTextLayout({r.x, cy - m.height * 0.5f}, layout, p.brush(st.color));
  ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

// A comic thought cloud: a rounded box with bumps on every side. Built once
// per size, in its own coordinates (0,0)..(w,h).
ID2D1Geometry* Scene::cloud(ID2D1Factory1* f, float w, float h, float k) {
  const uint64_t key = (static_cast<uint64_t>(w * 0.5f) << 32) | (static_cast<uint64_t>(h * 0.5f) << 12) |
                       static_cast<uint64_t>(k * 20.f);
  auto it = clouds_.find(key);
  if (it != clouds_.end()) return it->second.Get();
  ComPtr<ID2D1RoundedRectangleGeometry> core;
  const float r = std::min(h * 0.5f, 14.f * k);
  f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), r, r), &core);
  ComPtr<ID2D1Geometry> cur = core;
  auto bump = [&](float cx, float cy, float rad) {
    ComPtr<ID2D1EllipseGeometry> e;
    f->CreateEllipseGeometry(D2D1::Ellipse({cx, cy}, rad, rad), &e);
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    cur->CombineWithGeometry(e.Get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.Get());
    sink->Close();
    cur = path;
  };
  const float rb = std::clamp(h * 0.34f, 7.f * k, 13.f * k);
  const int n = std::max(2, static_cast<int>((w - rb) / (rb * 1.5f)));
  for (int i = 0; i < n; ++i) {
    const float x = rb * 0.9f + (w - rb * 1.8f) * static_cast<float>(i) / static_cast<float>(n - 1);
    bump(x, rb * 0.3f, rb * (i % 2 ? 0.9f : 1.f));
    bump(std::min(w - rb * 0.6f, x + rb * 0.4f), h - rb * 0.3f, rb * (i % 2 ? 1.f : 0.85f));
  }
  bump(rb * 0.25f, h * 0.5f, h * 0.42f);
  bump(w - rb * 0.25f, h * 0.5f, h * 0.42f);
  if (clouds_.size() > 64) clouds_.clear();
  clouds_[key] = cur;
  return cur.Get();
}

// The name floats over the spider, as in the reference clip. When the spider
// has something to say, the name moves into a thought cloud with the thought.
void Scene::labels(Painter& p, const Palette& pal, const Page&, const Pet& pet, Vec2 o, const Rect& clip,
                   double now, float dt) {
  const Spider& s = pet.spider();
  const float k = s.scale();
  ID2D1DeviceContext* ctx = p.ctx();
  if (!name_layout_ || name_text_ != pet.name()) {
    name_text_ = pet.name();
    name_layout_ = p.layout(name_text_, p.format(p.fonts().sans, 15.f * k, DWRITE_FONT_WEIGHT_BOLD));
    small_name_ = p.layout(name_text_, p.format(p.fonts().sans, 12.f * k, DWRITE_FONT_WEIGHT_BOLD));
  }
  const Vec2 head = s.head() + o;
  clip_origin_ = {clip.x, clip.y};
  bubble_shown_ = false;
  const std::wstring& t = pet.thought();
  const float age = static_cast<float>(now - pet.thought_at());
  float alpha = 0.f;
  if (!t.empty()) alpha = 1.f;  // stays up; the next thought replaces it

  // Plain name label, fading out while the cloud is up.
  DWRITE_TEXT_METRICS m{};
  name_layout_->GetMetrics(&m);
  const Vec2 body = s.draw_pos() + o;
  Vec2 want{body.x, body.y - s.reach() * 0.95f};
  want.x = std::clamp(want.x, clip.x + m.width * 0.5f + 6, clip.right() - m.width * 0.5f - 6);
  want.y = std::clamp(want.y, clip.y + m.height + 4, clip.bottom() - 4);
  if (!name_init_ || distance(want, name_pos_) > 600.f) {
    name_pos_ = want;
    name_init_ = true;
  }
  name_pos_ = lerp(name_pos_, want, damp(5.f, dt));
  if (alpha < 0.99f)
    p.outlined(name_layout_.Get(), {name_pos_.x - m.width * 0.5f, name_pos_.y - m.height},
               with_alpha(pal.name, 1.f - alpha), with_alpha(pal.name_stroke, 1.f - alpha), 1.6f);
  if (alpha <= 0.01f) return;

  // Size the cloud for the whole thought, then type it out quickly.
  const float max_w = 260.f * k;
  auto make = [&](const std::wstring& text) {
    auto l = p.layout(text, p.format(p.fonts().sans, 15.5f * k, DWRITE_FONT_WEIGHT_NORMAL));
    if (l) {
      l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
      l->SetMaxWidth(max_w);
    }
    return l;
  };
  if (!full_layout_ || full_text_ != t) {
    full_text_ = t;
    full_layout_ = make(t);
  }
  const std::wstring part = t.substr(0, std::min(t.size(), static_cast<size_t>(age * 55.f) + 1));
  if (!thought_layout_ || thought_shown_ != part) {
    thought_shown_ = part;
    thought_layout_ = make(part);
  }
  if (!full_layout_ || !thought_layout_ || !small_name_) return;
  DWRITE_TEXT_METRICS fm{}, nm{};
  full_layout_->GetMetrics(&fm);
  small_name_->GetMetrics(&nm);
  const float padx = 13.f * k, pady = 9.f * k, gap = 1.f * k;
  const float w = std::max(fm.width, nm.width) + padx * 2;
  const float h = nm.height + gap + fm.height + pady * 2;
  const float edge = 12.f * k;  // bumps stick out this far

  // Well clear of the legs, up and to the right. It holds still while the
  // spider works nearby and only drifts over when the spider has moved far,
  // so it can be read. Pinned bubbles stay where they were dropped.
  auto fit = [&](Vec2 q) {
    q.x = std::clamp(q.x, clip.x + w * 0.5f + edge, clip.right() - w * 0.5f - edge);
    q.y = std::clamp(q.y, clip.y + h + edge, clip.bottom() - edge);
    return q;
  };
  if (pinned_) {
    bubble_pos_ = fit(clip_origin_ + pin_);
    bubble_goal_ = bubble_pos_;
    bubble_init_ = true;
  } else {
    Vec2 spot{head.x + s.reach() * 1.1f, head.y - s.reach() * 1.35f};
    if (spot.x + w * 0.5f + edge > clip.right()) spot.x = head.x - s.reach() * 1.1f;  // no room: go left
    if (spot.y - h - edge < clip.y) spot.y = head.y + s.reach() * 1.35f + h;          // no room: go below
    spot = fit(spot);
    if (!bubble_init_ || distance(spot, bubble_pos_) > 1600.f) {
      bubble_pos_ = bubble_goal_ = spot;
      bubble_init_ = true;
    }
    if (distance(spot, bubble_goal_) > s.reach() * 1.6f) bubble_goal_ = spot;
    bubble_pos_ = lerp(bubble_pos_, fit(bubble_goal_), damp(2.2f, dt));
  }
  const Vec2 tl{bubble_pos_.x - w * 0.5f, bubble_pos_.y - h};
  bubble_box_ = {tl.x - edge, tl.y - edge, w + 2 * edge, h + 2 * edge};
  bubble_shown_ = true;

  const D2D1_COLOR_F fill = pal.dark ? rgb(0x0C0E1C, 0.93f * alpha) : rgb(0xFFFFFF, 0.96f * alpha);
  const D2D1_COLOR_F line = with_alpha(pal.body, alpha);

  // Thought trail: three shrinking bubbles leading off toward the spider.
  const bool below = bubble_pos_.y < head.y ? false : true;
  const Vec2 from{std::clamp(head.x, tl.x + 18.f * k, tl.x + w - 18.f * k), below ? tl.y - 5.f * k : tl.y + h + 5.f * k};
  const Vec2 to = head + Vec2{0, below ? 10.f * k : -10.f * k};
  const Vec2 dir = normalized(to - from);
  const float reach = std::min(distance(from, to), 70.f * k);
  const float rads[3] = {4.8f * k, 3.4f * k, 2.2f * k};
  const float at[3] = {0.15f, 0.5f, 0.85f};
  for (int i = 0; i < 3; ++i) {
    const Vec2 c = from + dir * (reach * at[i]);
    p.dot(c, rads[i] + 1.4f * k, line);
    p.dot(c, rads[i], fill);
  }

  // A small pop when a new thought arrives.
  const float pop = 0.82f + 0.18f * ease_out_back(std::min(1.f, age / 0.22f));
  ctx->SetTransform(D2D1::Matrix3x2F::Scale(pop, pop, {w * 0.5f, h}) * D2D1::Matrix3x2F::Translation(tl.x, tl.y));
  if (ID2D1Geometry* g = cloud(p.factory(), w, h, k)) {
    ctx->FillGeometry(g, p.brush(fill));
    ctx->DrawGeometry(g, p.brush(line), 1.6f * k);
  }
  ctx->DrawTextLayout({padx, pady}, small_name_.Get(), p.brush(with_alpha(pal.dark ? pal.joint : pal.body, alpha)));
  ctx->DrawTextLayout({padx, pady + nm.height + gap}, thought_layout_.Get(), p.brush(with_alpha(pal.name, alpha)));
  ctx->SetTransform(D2D1::Matrix3x2F::Identity());
}

// base minus every hole, as a geometry mask for a layer.
static ComPtr<ID2D1Geometry> cut_out(ID2D1Factory1* f, const Rect& base, const std::vector<Rect>& holes) {
  ComPtr<ID2D1RectangleGeometry> rect;
  f->CreateRectangleGeometry(D2D1::RectF(base.x, base.y, base.right(), base.bottom()), &rect);
  ComPtr<ID2D1Geometry> cur = rect;
  for (const Rect& h : holes) {
    ComPtr<ID2D1RectangleGeometry> hole;
    f->CreateRectangleGeometry(D2D1::RectF(h.x, h.y, h.right(), h.bottom()), &hole);
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) break;
    cur->CombineWithGeometry(hole.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
    sink->Close();
    cur = path;
  }
  return cur;
}

void Scene::draw(Painter& p, const Page& page, const Pet& pet, const Settings& s, Vec2 o, const Rect& clip,
                 const std::vector<Rect>& holes, double now, float dim, float dt) {
  if (!pet.active()) return;
  const Palette pal = make_palette(page.empty() ? true : page.dark());
  ID2D1DeviceContext* ctx = p.ctx();
  const Spider& sp = pet.spider();
  const float k = sp.scale();

  ComPtr<ID2D1Geometry> page_mask;
  if (!holes.empty()) {
    page_mask = cut_out(p.factory(), clip, holes);
    ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), page_mask.Get()), nullptr);
  }
  ctx->PushAxisAlignedClip(D2D1::RectF(clip.x, clip.y, clip.right(), clip.bottom()), D2D1_ANTIALIAS_MODE_ALIASED);
  const Rect seen = clip.inflated(40.f);

  // Harvested text and the sentence being read.
  for (const Entity& e : page.entities()) {
    const bool live = e.id == pet.target() && pet.state() == Pet::State::Read;
    if (e.mark != Mark::Done && !live) continue;
    if (!seen.contains(off(e.box, o).center()) && overlap_area(seen, off(e.box, o)) <= 0) continue;
    if (e.kind == Kind::Sentence) sentence(p, pal, e, o, e.read, live);
    else decal(p, pal, page, e, o, now);
  }

  // Silk between finds, and the dragline back to the last one.
  if (s.web) {
    auto anchor = [&](int id, Vec2& out) {
      for (const Entity& e : page.entities())
        if (e.id == id) {
          out = (e.lines.empty() ? e.box : e.lines.front()).center() + o;
          out.x = e.lines.empty() ? out.x : e.lines.front().x + o.x;
          return true;
        }
      return false;
    };
    for (const Pet::Silk& w : pet.web()) {
      Vec2 a, b;
      if (!anchor(w.a, a) || !anchor(w.b, b)) continue;
      if (!seen.contains(a) && !seen.contains(b)) continue;
      const float d = distance(a, b);
      const Vec2 ctrl = lerp(a, b, 0.5f) + Vec2{0, d * 0.08f};
      p.curve(a, ctrl, b, pal.silk, 0.9f, static_cast<float>((now - w.at) / 0.35));
    }
    Vec2 last;
    if (pet.last_done() > 0 && anchor(pet.last_done(), last) && pet.state() != Pet::State::Drag) {
      const Vec2 rear = sp.rear() + o;
      const float d = distance(rear, last);
      p.curve(rear, lerp(rear, last, 0.5f) + Vec2{0, d * 0.1f}, last, with_alpha(pal.silk, 1.8f), 0.9f);
    }
  }

  // Lock-on: the target box and the line from the head. While moving in, the
  // line is out at full length, like the reference; a grab shoots it out; a
  // read runs its tip along the sentence.
  if (pet.tethered()) {
    for (const Entity& e : page.entities()) {
      if (e.id != pet.target()) continue;
      const Vec2 head = sp.head() + o;
      const bool reading = pet.state() == Pet::State::Read;
      if (!reading) {
        const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(now) * 9.f);
        for (const Rect& l : e.lines) p.stroke(off(l, o).inflated(2.5f), with_alpha(pal.tether, pulse), 1.6f);
      }
      const Vec2 end = reading || pet.state() == Pet::State::Grab ? pet.tether_tip() + o
                                                                 : off(e.box, o).inflated(2.5f).nearest(head);
      const Vec2 tip = lerp(head, end, pet.tether_out());
      p.line(head, tip, pal.tether, std::max(1.2f, 1.7f * k));
      p.dot(tip, std::max(1.6f, 2.4f * k), pal.tether);
      break;
    }
  }
  ctx->PopAxisAlignedClip();
  if (page_mask) ctx->PopLayer();

  // The spider itself may poke past the page edge, but never into a window
  // that sits on top of the page.
  ComPtr<ID2D1Geometry> body_mask;
  if (!holes.empty() && !pet.dragging()) {
    body_mask = cut_out(p.factory(), clip.inflated(4000.f), holes);
    ctx->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), body_mask.Get()), nullptr);
  }

  const Pet::Thread& th = pet.thread();
  if (th.on) {
    const Vec2 top = th.top + o;
    p.line({top.x, std::max(top.y, clip.y - 4)}, sp.rear() + o, with_alpha(pal.name, 0.6f * th.alpha), 1.1f);
  }

  ctx->SetTransform(D2D1::Matrix3x2F::Translation(o.x, o.y));
  if (dim < 0.99f) p.spider(sp, pal, 1.f - dim);
  ctx->SetTransform(D2D1::Matrix3x2F::Identity());

  if (dim < 0.5f) labels(p, pal, page, pet, o, clip, now, dt);
  if (body_mask) ctx->PopLayer();

  // Arrival glitch: torn color bars down the left edge for a moment.
  const float intro = static_cast<float>(now - pet.summoned_at());
  if (intro >= 0 && intro < 0.7f) {
    const float a = 1.f - intro / 0.7f;
    const D2D1_COLOR_F bars[] = {pal.magenta, pal.green, pal.blue, pal.salmon, pal.cyan};
    for (int i = 0; i < 18; ++i) {
      const float y = clip.y + rng_.range(0.f, clip.h);
      p.fill({clip.x + rng_.range(0.f, 30.f), y, rng_.range(10.f, 160.f), rng_.range(2.f, 6.f)},
             with_alpha(bars[rng_.below(5)], 0.5f * a));
    }
  }
  if (layouts_.size() > 800) layouts_.clear();
}

}  // namespace sp
