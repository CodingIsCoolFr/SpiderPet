#pragma once

#include "core/geom.hpp"

#include <array>
#include <functional>

namespace sp {

// Moves a foot onto something solid near p (a letter on the page). Returns
// false when there is nothing within radius.
using SnapFn = std::function<bool(Vec2 p, float radius, Vec2& out)>;

struct Leg {
  int side = 1;    // -1 left of the heading, +1 right
  int index = 0;   // 0 front .. 3 back
  float along = 0; // hip offset along the body axis
  float angle = 0; // rest direction, radians off the heading
  float rest = 0;  // rest distance from the body center
  float pole = 0;  // direction the knee bends toward
  std::array<float, 3> seg{};
  std::array<Vec2, 4> j{};  // hip, knee, ankle, foot
  Vec2 foot{};
  Vec2 foot_vel{};
  bool planted = false;
  bool init = false;
  bool stepping = false;
  Vec2 from{}, to{};
  float t = 0, dur = 0.15f, lift = 0;
  bool anchored = false;
  Vec2 anchor{};
  float phase = 0;
  float reach() const { return seg[0] + seg[1] + seg[2]; }
};

// The node-and-line spider: a thin body box, eight three-segment legs, and a
// gait that plants feet on the page and steps when they fall behind.
class Spider {
 public:
  Spider();

  void configure(float scale);
  float scale() const { return scale_; }
  void place(Vec2 at, float heading);
  void shift(Vec2 d);

  void set_goal(Vec2 goal, float max_speed, float arrive_radius);
  void clear_goal() { has_goal_ = false; }
  bool has_goal() const { return has_goal_; }
  Vec2 goal() const { return goal_; }
  void set_face(float angle) { face_ = angle; has_face_ = true; }
  void clear_face() { has_face_ = false; }
  void set_burst(bool on) { burst_ = on; }

  // Airborne: hanging on a thread, held by the mouse, or thrown. Feet dangle.
  void set_airborne(bool on);
  bool airborne() const { return airborne_; }
  void set_curl(float c) { curl_goal_ = c; }
  void set_flail(float f) { flail_ = f; }
  void set_held(bool held) { held_ = held; }
  void set_position(Vec2 p) { pos_ = p; }
  void set_velocity(Vec2 v) { vel_ = v; }
  void spin(float rate) { spin_ = rate; }

  void anchor(int side, int index, Vec2 p);
  void release_anchor(int side, int index);
  void release_anchors();

  void tick(float dt, const SnapFn& snap);

  Vec2 pos() const { return pos_; }
  Vec2 draw_pos() const { return pos_ + bob_; }
  Vec2 vel() const { return vel_; }
  float speed() const { return length(vel_); }
  float heading() const { return heading_; }
  Vec2 forward() const { return from_angle(heading_); }
  Vec2 head() const { return draw_pos() + forward() * (body_len_ * 0.32f); }
  Vec2 rear() const { return draw_pos() - forward() * (body_len_ * 0.5f); }
  float body_len() const { return body_len_; }
  float body_wid() const { return body_wid_; }
  float reach() const { return reach_; }
  const std::array<Leg, 8>& legs() const { return legs_; }
  bool arrived(float tol) const;
  int planted_count() const;

 private:
  Leg& leg(int side, int index) { return legs_[(side < 0 ? 0 : 4) + index]; }
  Vec2 hip(const Leg& l) const;
  Vec2 rest_point(const Leg& l, float curl) const;
  bool neighbor_stepping(const Leg& l) const;
  void move(float dt);
  void turn(float dt);
  void update_leg(Leg& l, float dt, int& stepping, const SnapFn& snap);
  void solve(Leg& l);

  std::array<Leg, 8> legs_{};
  Rng rng_{0x51D3A2F1ull};
  float scale_ = 1;
  float body_len_ = 48, body_wid_ = 13, reach_ = 105;
  Vec2 pos_{}, vel_{}, bob_{};
  float heading_ = kPi * 0.5f;
  float time_ = 0;
  float still_for_ = 0;
  bool has_goal_ = false;
  Vec2 goal_{};
  float max_speed_ = 240, arrive_ = 60;
  bool has_face_ = false;
  float face_ = 0;
  bool burst_ = true;
  bool bursting_ = true;
  float burst_t_ = 0.5f;
  bool airborne_ = true;
  bool held_ = false;
  float curl_ = 0.4f, curl_goal_ = 0.4f;
  float flail_ = 0;
  float spin_ = 0;
};

}  // namespace sp
