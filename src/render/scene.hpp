#pragma once

#include "page/page.hpp"
#include "pet/pet.hpp"
#include "render/paint.hpp"

#include <unordered_map>
#include <vector>

namespace sp {

// Draws one frame: harvested text restyled in place, silk between finds, the
// tether to the current target, the spider, and its labels.
class Scene {
 public:
  // offset maps content coordinates to target-local pixels; clip is the
  // watched area in target-local pixels.
  // holes: target-local boxes of other windows over the page; nothing of
  // ours is painted inside them.
  void draw(Painter& p, const Page& page, const Pet& pet, const Settings& s, Vec2 offset, const Rect& clip,
            const std::vector<Rect>& holes, double now, float dim, float dt);
  void forget() { layouts_.clear(); }

  // The thought bubble can be dragged and pinned (target-local pixels).
  bool bubble_hit(Vec2 local) const { return bubble_shown_ && bubble_box_.inflated(4.f).contains(local); }
  Vec2 bubble_anchor() const { return bubble_pos_; }
  void pin_bubble(Vec2 local_anchor) {
    pinned_ = true;
    pin_ = local_anchor - clip_origin_;
  }
  void unpin_bubble() { pinned_ = false; }

 private:
  struct Style {
    int family = 0;  // 0 serif, 1 mono, 2 sans
    float size = 1.f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE italic = DWRITE_FONT_STYLE_NORMAL;
    float spacing = 0;
    D2D1_COLOR_F color{};
    bool fill = false;
    D2D1_COLOR_F fill_color{};
    bool outline = false;
    D2D1_COLOR_F outline_color{};
  };
  Style style_for(const Entity& e, const Palette& pal) const;
  IDWriteTextLayout* layout_for(Painter& p, const Page& page, const Entity& e, const Style& st);
  void decal(Painter& p, const Palette& pal, const Page& page, const Entity& e, Vec2 off, double now);
  void sentence(Painter& p, const Palette& pal, const Entity& e, Vec2 off, float progress, bool live);
  void labels(Painter& p, const Palette& pal, const Page& page, const Pet& pet, Vec2 off, const Rect& clip,
              double now, float dt);

  struct Cached {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    uint64_t key = 0;
  };
  std::unordered_map<int, Cached> layouts_;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> name_layout_, small_name_;
  std::wstring name_text_;
  Vec2 name_pos_{};
  bool name_init_ = false;
  // Thought bubble.
  ID2D1Geometry* cloud(ID2D1Factory1* f, float w, float h, float k);
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID2D1Geometry>> clouds_;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> full_layout_, thought_layout_;
  std::wstring full_text_, thought_shown_;
  Vec2 bubble_pos_{};   // bottom-center of the cloud
  Vec2 bubble_goal_{};
  bool bubble_init_ = false;
  bool bubble_shown_ = false;
  Rect bubble_box_{};
  bool pinned_ = false;
  Vec2 pin_{};          // relative to the watched area's top-left
  Vec2 clip_origin_{};
  Rng rng_{0xBADC0DE};
};

}  // namespace sp
