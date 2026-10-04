#include "pet/spider.hpp"

namespace sp {
namespace {

// Measured off the reference clip: hips sit in the front half of the body,
// front legs reach forward, back legs trail, every leg bends its knee outward.
constexpr float kAlong[4] = {0.36f, 0.15f, -0.06f, -0.27f};
constexpr float kAngle[4] = {0.55f, 1.22f, 1.95f, 2.62f};
constexpr float kRest[4] = {0.95f, 0.84f, 0.86f, 0.98f};
constexpr float kSeg[3] = {0.44f, 0.41f, 0.37f};

}  // namespace

Spider::Spider() { configure(1.f); }

void Spider::configure(float scale) {
  scale_ = std::clamp(scale, 0.3f, 4.f);
  body_len_ = 50.f * scale_;
  body_wid_ = 11.5f * scale_;
  reach_ = 118.f * scale_;
  for (int s = 0; s < 2; ++s) {
    for (int i = 0; i < 4; ++i) {
      Leg& l = legs_[s * 4 + i];
      l.side = s == 0 ? -1 : 1;
      l.index = i;
      l.along = kAlong[i] * body_len_;
      l.angle = kAngle[i];
      l.rest = kRest[i] * reach_;
      l.pole = kAngle[i] + (kPi * 0.5f - kAngle[i]) * 0.4f;
      for (int k = 0; k < 3; ++k) l.seg[k] = kSeg[k] * reach_;
      if (l.phase == 0) l.phase = rng_.range(0.1f, 6.2f);
    }
  }
}

void Spider::place(Vec2 at, float heading) {
  pos_ = at;
  vel_ = {};
  heading_ = heading;
  for (Leg& l : legs_) {
    l.foot = rest_point(l, airborne_ ? curl_ : 1.f);
    l.foot_vel = {};
    l.stepping = false;
    l.planted = !airborne_;
    l.init = false;
    solve(l);
  }
}

void Spider::shift(Vec2 d) {
  pos_ += d;
  goal_ += d;
  for (Leg& l : legs_) {
    l.foot += d;
    l.from += d;
    l.to += d;
    l.anchor += d;
    for (Vec2& p : l.j) p += d;
  }
}

void Spider::set_goal(Vec2 goal, float max_speed, float arrive_radius) {
  goal_ = goal;
  has_goal_ = true;
  max_speed_ = max_speed;
  arrive_ = std::max(1.f, arrive_radius);
}

void Spider::set_airborne(bool on) {
  if (airborne_ == on) return;
  airborne_ = on;
  for (Leg& l : legs_) {
    l.stepping = false;
    l.lift = 0;
    l.planted = !on;
    l.foot_vel = {};
  }
}

void Spider::anchor(int side, int index, Vec2 p) {
  Leg& l = leg(side, index);
  l.anchored = true;
  l.anchor = p;
}

void Spider::release_anchor(int side, int index) { leg(side, index).anchored = false; }

void Spider::release_anchors() {
  for (Leg& l : legs_) l.anchored = false;
}

bool Spider::arrived(float tol) const {
  if (!has_goal_) return true;
  return distance(pos_, goal_) <= tol && speed() < 40.f * scale_;
}

int Spider::planted_count() const {
  int n = 0;
  for (const Leg& l : legs_) n += (l.planted && !l.stepping) ? 1 : 0;
  return n;
}

Vec2 Spider::hip(const Leg& l) const {
  const Vec2 f = from_angle(heading_);
  return pos_ + f * l.along + perp(f) * (static_cast<float>(l.side) * body_wid_ * 0.5f);
}

Vec2 Spider::rest_point(const Leg& l, float curl) const {
  const float a = heading_ + static_cast<float>(l.side) * l.angle;
  return pos_ + from_angle(a) * (l.rest * curl);
}

bool Spider::neighbor_stepping(const Leg& l) const {
  for (const Leg& o : legs_) {
    if (&o == &l || !o.stepping) continue;
    if (o.side == l.side && std::abs(o.index - l.index) == 1) return true;
    if (o.side != l.side && o.index == l.index) return true;
  }
  return false;
}

void Spider::move(float dt) {
  if (has_goal_) {
    const Vec2 d = goal_ - pos_;
    const float dist = length(d);
    float want = max_speed_ * std::clamp(dist / arrive_, 0.f, 1.f);
    // Real spiders run in bursts. Long trips get short freezes between runs.
    if (burst_ && dist > 150.f * scale_) {
      burst_t_ -= dt;
      if (burst_t_ <= 0) {
        bursting_ = !bursting_;
        burst_t_ = bursting_ ? rng_.range(0.35f, 0.95f) : rng_.range(0.07f, 0.2f);
      }
      if (!bursting_) want *= 0.1f;
    } else {
      bursting_ = true;
    }
    const Vec2 desired = dist > 0.4f ? normalized(d) * want : Vec2{};
    const Vec2 dv = desired - vel_;
    const float accel = 1500.f * scale_ * dt;
    const float l = length(dv);
    vel_ = l > accel ? vel_ + dv * (accel / l) : desired;
  } else {
    vel_ = vel_ * std::exp(-9.f * dt);
  }
  pos_ += vel_ * dt;
}

void Spider::turn(float dt) {
  const float sp = speed();
  float want = heading_;
  if (has_face_) want = face_;
  if (sp > 18.f * scale_ && !(has_face_ && sp < 60.f * scale_)) want = std::atan2(vel_.y, vel_.x);
  if (spin_ != 0) {
    heading_ = wrap_pi(heading_ + spin_ * dt);
    spin_ *= std::exp(-2.5f * dt);
    if (std::fabs(spin_) < 0.05f) spin_ = 0;
    return;
  }
  const float rate = (sp > 18.f * scale_ ? 7.5f : 4.5f) * dt;
  heading_ = wrap_pi(heading_ + std::clamp(wrap_pi(want - heading_), -rate, rate));
}

void Spider::tick(float dt, const SnapFn& snap) {
  dt = std::clamp(dt, 0.f, 0.05f);
  time_ += dt;
  if (!airborne_ && !held_) move(dt);
  turn(dt);
  curl_ = lerp(curl_, curl_goal_, damp(5.f, dt));
  bob_ = bob_ * std::exp(-14.f * dt);
  if (speed() < 20.f * scale_) still_for_ += dt;
  else still_for_ = 0;

  int stepping = 0;
  for (const Leg& l : legs_) stepping += l.stepping ? 1 : 0;
  for (Leg& l : legs_) update_leg(l, dt, stepping, snap);
}

void Spider::update_leg(Leg& l, float dt, int& stepping, const SnapFn& snap) {
  const Vec2 h = hip(l);
  if (airborne_) {
    const float wiggle = std::sin(time_ * 13.f + l.phase * 5.f) * 0.32f * flail_;
    const float a = heading_ + static_cast<float>(l.side) * (l.angle + wiggle);
    const Vec2 target = h + from_angle(a) * (l.rest * curl_);
    l.foot_vel += (target - l.foot) * (170.f * dt);
    l.foot_vel = l.foot_vel * std::exp(-16.f * dt);
    l.foot += l.foot_vel * dt;
    l.lift = 0.6f;
    solve(l);
    return;
  }

  if (l.stepping) {
    l.t += dt / l.dur;
    if (l.anchored) l.to = l.anchor;
    const float k = ease_in_out(std::min(1.f, l.t));
    const Vec2 base = lerp(l.from, l.to, k);
    l.lift = std::sin(kPi * std::min(1.f, l.t));
    const Vec2 out = normalized(base - pos_);
    l.foot = base + out * (l.lift * 7.f * scale_);
    if (l.t >= 1.f) {
      l.foot = l.to;
      l.stepping = false;
      l.planted = true;
      l.lift = 0;
      --stepping;
      bob_ += normalized(l.to - pos_) * (0.9f * scale_);
    }
    solve(l);
    return;
  }

  const float sp = speed();
  const bool moving = sp > 20.f * scale_;
  Vec2 want;
  if (l.anchored) {
    want = l.anchor;
  } else {
    const float lead = moving ? 0.2f : 0.f;
    want = rest_point(l, 1.f) + vel_ * lead;
  }
  const float err = distance(l.foot, want);
  const bool over = distance(l.foot, h) > l.reach() * 0.97f;
  float threshold = moving ? reach_ * 0.34f : reach_ * 0.2f;
  if (l.anchored) threshold = 2.5f * scale_;
  const bool settle = !moving && still_for_ > 0.3f;
  const bool due = err > threshold && (moving || l.anchored || settle);
  if (due || over) {
    if (over || l.anchored || (stepping < 4 && !neighbor_stepping(l))) {
      Vec2 to = want;
      if (!l.anchored) {
        to += Vec2{rng_.range(-5.f, 5.f), rng_.range(-5.f, 5.f)} * scale_;
        Vec2 snapped;
        if (snap && snap(to, 12.f * scale_, snapped)) to = snapped;
      }
      l.stepping = true;
      l.planted = false;
      l.from = l.foot;
      l.to = to;
      l.t = 0;
      l.dur = std::clamp(0.17f - sp / (2600.f * scale_), 0.075f, 0.17f) * (l.anchored ? 0.8f : 1.f);
      ++stepping;
    }
  }
  solve(l);
}

// FABRIK on a three-segment chain. The knee and ankle are pulled toward a pole
// beside the body every frame, so legs bend outward like the reference.
void Spider::solve(Leg& l) {
  auto& j = l.j;
  const Vec2 h = hip(l);
  const Vec2 target = l.foot;
  const Vec2 to_target = target - h;
  const float dt_len = length(to_target);
  const float side = static_cast<float>(l.side);
  const Vec2 pole = h + from_angle(heading_ + side * l.pole) * (reach_ * 0.6f) + normalized(to_target) * (dt_len * 0.25f);

  j[0] = h;
  if (!l.init) {
    j[1] = lerp(h, pole, 0.7f);
    j[2] = lerp(pole, target, 0.5f);
    j[3] = target;
    l.init = true;
  }
  j[1] = lerp(j[1], pole, 0.3f);
  j[2] = lerp(j[2], lerp(pole, target, 0.6f), 0.15f);

  const float total = l.reach();
  if (dt_len >= total) {
    const Vec2 n = normalized(to_target);
    j[1] = h + n * l.seg[0];
    j[2] = j[1] + n * l.seg[1];
    j[3] = j[2] + n * l.seg[2];
    return;
  }
  for (int it = 0; it < 10; ++it) {
    j[3] = target;
    for (int i = 2; i >= 0; --i) j[i] = j[i + 1] + normalized(j[i] - j[i + 1]) * l.seg[i];
    j[0] = h;
    for (int i = 1; i <= 3; ++i) j[i] = j[i - 1] + normalized(j[i] - j[i - 1]) * l.seg[i - 1];
    if (distance(j[3], target) < 0.25f) break;
  }
}

}  // namespace sp
