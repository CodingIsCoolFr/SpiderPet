#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sp {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
  float x = 0;
  float y = 0;
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
inline Vec2& operator+=(Vec2& a, Vec2 b) { a.x += b.x; a.y += b.y; return a; }
inline Vec2& operator-=(Vec2& a, Vec2 b) { a.x -= b.x; a.y -= b.y; return a; }

inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline float distance(Vec2 a, Vec2 b) { return length(a - b); }
inline Vec2 normalized(Vec2 v) {
  const float l = length(v);
  return l < 1e-5f ? Vec2{1, 0} : Vec2{v.x / l, v.y / l};
}
inline Vec2 from_angle(float a) { return {std::cos(a), std::sin(a)}; }
inline Vec2 perp(Vec2 v) { return {-v.y, v.x}; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline Vec2 lerp(Vec2 a, Vec2 b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)}; }
inline float wrap_pi(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}
inline float ease_in_out(float t) { return t < 0.5f ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2.f) / 2; }
inline float ease_out_cubic(float t) { return 1 - std::pow(1 - t, 3.f); }
inline float ease_in_cubic(float t) { return t * t * t; }
inline float ease_out_back(float t) {
  const float c1 = 1.4f, c3 = c1 + 1;
  return 1 + c3 * std::pow(t - 1, 3.f) + c1 * std::pow(t - 1, 2.f);
}
// Frame-rate independent smoothing toward a goal.
inline float damp(float rate, float dt) { return 1.f - std::exp(-rate * dt); }

struct Rect {
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(Vec2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
  Vec2 nearest(Vec2 p) const { return {std::clamp(p.x, x, x + w), std::clamp(p.y, y, y + h)}; }
  Rect moved(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  Rect inflated(float d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
};

inline Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  const float x1 = std::max(a.right(), b.right()), y1 = std::max(a.bottom(), b.bottom());
  return {x0, y0, x1 - x0, y1 - y0};
}

inline float overlap_area(const Rect& a, const Rect& b) {
  const float w = std::min(a.right(), b.right()) - std::max(a.x, b.x);
  const float h = std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y);
  return (w > 0 && h > 0) ? w * h : 0.f;
}

inline float iou(const Rect& a, const Rect& b) {
  const float i = overlap_area(a, b);
  const float u = a.w * a.h + b.w * b.h - i;
  return u > 0 ? i / u : 0.f;
}

// Small fast RNG so the sim does not depend on <random> state machines.
class Rng {
 public:
  explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s_(seed ? seed : 1) {}
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return static_cast<uint32_t>(s_ >> 11);
  }
  float unit() { return (next() & 0xFFFFFF) / float(0x1000000); }
  float range(float a, float b) { return a + (b - a) * unit(); }
  int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }

 private:
  uint64_t s_;
};

}  // namespace sp
