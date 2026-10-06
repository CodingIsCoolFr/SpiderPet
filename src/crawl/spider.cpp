#include "crawl/spider.hpp"

#include <algorithm>

namespace sp::spider {

namespace {
constexpr float kPi = 3.14159265f;

float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
} // namespace

void Spider::init(Vec start, float scale_, unsigned seed) {
    rng_.seed(seed);
    scale = scale_;
    pos = start;
    vel = {0, 0};
    goal = start;
    heading = kPi / 2;
    drop = 1;
    // Right legs front to back, then left legs. Front and back pairs reach
    // furthest, like the long legs in the reference video.
    const float angles[4] = {0.55f, 1.25f, 1.95f, 2.6f};
    const float hips[4] = {10, 4, -2, -8};
    const float reach[4] = {1.25f, 0.95f, 0.95f, 1.15f};
    for (int i = 0; i < 8; ++i) {
        Leg& l = legs[i];
        int k = i % 4;
        l.right = i < 4;
        float side = l.right ? 1.f : -1.f;
        l.hip_local = {hips[k], 6 * side};
        l.rest_angle = angles[k] * side;
        float r = reach[k] * rnd(0.85f, 1.2f);
        l.l1 = 52 * r;
        l.l2 = 64 * r;
        l.rest_dist = (l.l1 + l.l2) * 0.72f;
        // Tetrapod gait: R0 L1 R2 L3 move together, then the other four.
        l.group = (k + (l.right ? 0 : 1)) % 2;
        l.foot = rest_point(l);
        l.from = l.to = l.foot;
    }
}

Vec Spider::hip(int i) const {
    return pos + rot(legs[i].hip_local * scale, heading);
}

Vec Spider::head() const { return pos + rot(Vec{16, 0} * scale, heading); }
Vec Spider::tail() const { return pos + rot(Vec{-16, 0} * scale, heading); }

Vec Spider::rest_point(const Leg& l) const {
    Vec h = pos + rot(l.hip_local * scale, heading);
    return h + rot(Vec{1, 0}, heading + l.rest_angle) * (l.rest_dist * scale);
}

Vec Spider::knee(int i) const {
    const Leg& l = legs[i];
    Vec h = hip(i);
    Vec d = l.foot - h;
    float l1 = l.l1 * scale, l2 = l.l2 * scale;
    float dist = std::clamp(d.len(), std::fabs(l1 - l2) + 0.01f, l1 + l2 - 0.01f);
    float a = std::acos(std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1.f, 1.f));
    float base = std::atan2(d.y, d.x);
    Vec k1 = h + rot(Vec{1, 0}, base + a) * l1;
    Vec k2 = h + rot(Vec{1, 0}, base - a) * l1;
    // Knees bend away from the body's center line.
    Vec right = rot(Vec{0, 1}, heading);
    float s1 = (k1.x - pos.x) * right.x + (k1.y - pos.y) * right.y;
    float s2 = (k2.x - pos.x) * right.x + (k2.y - pos.y) * right.y;
    bool pick1 = l.right ? s1 > s2 : s1 < s2;
    return pick1 ? k1 : k2;
}

void Spider::start_step(int i, Vec target, const AskWord& ask) {
    Leg& l = legs[i];
    l.from = l.foot;
    l.to = target;
    l.t = 0;
    l.stepping = true;
    l.ask = next_ask_++;
    if (ask) ask(i, l.ask, target);
}

void Spider::snap(int i, unsigned ask, Vec p) {
    if (i < 0 || i >= 8) return;
    Leg& l = legs[i];
    if (l.ask != ask) return; // a newer step already started
    float reach = (l.l1 + l.l2) * scale * 0.98f;
    if ((p - hip(i)).len() > reach) return;
    if (l.stepping) l.to = p;
    else l.foot = l.from = l.to = p;
}

void Spider::teleport(Vec to) {
    Vec d = to - pos;
    pos = to;
    goal = to;
    vel = {0, 0};
    for (auto& l : legs) {
        l.foot = l.foot + d;
        l.from = l.from + d;
        l.to = l.to + d;
        l.stepping = false;
        l.t = 1;
    }
    drop = 1;
    replant_ = 8;
}

void Spider::replant() {
    replant_ = 8;
}

void Spider::update(float dt, const AskWord& ask) {
    dt = std::min(dt, 0.05f);

    // Entrance: slide down a silk thread, then start walking.
    if (drop > 0) drop = std::max(0.f, drop - dt * 1.6f);

    // Steering with a soft arrival.
    Vec to_goal = goal - pos;
    float dist = to_goal.len();
    float slow = std::clamp(dist / (90 * scale), 0.f, 1.f);
    Vec desired = has_goal ? to_goal.norm() * (max_speed * scale * slow) : Vec{0, 0};
    float accel = std::min(1.f, dt * 3.5f);
    vel = vel + (desired - vel) * accel;
    if (drop > 0.05f) vel = {0, 0};
    pos = pos + vel * dt;

    float speed = vel.len();
    if (speed > 12 * scale) {
        float want = std::atan2(vel.y, vel.x);
        heading += wrap(want - heading) * std::min(1.f, dt * 5.f);
    }

    // Gait: a foot that lags too far behind its rest spot steps, but only
    // when no leg of the other group is in the air.
    bool group_busy[2] = {false, false};
    for (auto& l : legs)
        if (l.stepping) group_busy[l.group] = true;
    float step_time = std::clamp(0.2f - speed / (scale * 4000.f), 0.09f, 0.2f);

    int worst = -1;
    float worst_err = 0;
    for (int i = 0; i < 8; ++i) {
        Leg& l = legs[i];
        if (l.stepping) continue;
        Vec target = l.has_override ? l.override_target : rest_point(l);
        float err = (l.foot - target).len();
        float threshold = (l.l1 + l.l2) * scale * (l.has_override ? 0.08f : 0.32f);
        float reach = (l.l1 + l.l2) * scale;
        bool forced = (l.foot - hip(i)).len() > reach * 0.98f;
        if ((err > threshold && !group_busy[1 - l.group]) || forced) {
            if (err > worst_err || forced) { worst_err = forced ? 1e9f : err; worst = i; }
        }
    }
    // After a scroll jump the feet point at old words: move them one by one.
    if (worst < 0 && replant_ > 0) {
        int i = 8 - replant_;
        if (!legs[i].stepping && !group_busy[1 - legs[i].group]) { worst = i; --replant_; }
    }
    if (worst >= 0) {
        Leg& l = legs[worst];
        Vec target = l.has_override ? l.override_target : rest_point(l);
        // Reach ahead in the walking direction so the stride looks natural.
        if (!l.has_override) target = target + vel * (step_time * 1.2f) + Vec{rnd(-8, 8), rnd(-8, 8)} * scale;
        start_step(worst, target, l.has_override ? AskWord() : ask);
        group_busy[l.group] = true;
    }

    // While standing, a random leg feels around now and then: it keeps the
    // spider alive and touches new words.
    twitch_ -= dt;
    if (speed < 20 * scale && twitch_ <= 0) {
        twitch_ = rnd(0.25f, 0.9f);
        int i = int(rnd(0, 7.99f));
        Leg& l = legs[i];
        if (!l.stepping && !l.has_override && !group_busy[1 - l.group]) {
            Vec h = hip(i);
            float ang = heading + l.rest_angle + rnd(-0.5f, 0.5f);
            float r = (l.l1 + l.l2) * scale * rnd(0.45f, 0.95f);
            start_step(i, h + rot(Vec{1, 0}, ang) * r, ask);
        }
    }

    for (auto& l : legs) {
        if (!l.stepping) continue;
        l.t = std::min(1.f, l.t + dt / step_time);
        float e = l.t * l.t * (3 - 2 * l.t);
        Vec p = lerp(l.from, l.to, e);
        // Lift: push the foot outward mid-step so the arc reads in 2D.
        Vec out = (p - pos).norm();
        float lift = std::sin(l.t * kPi) * 6 * scale;
        l.foot = p + out * lift;
        if (l.t >= 1) { l.stepping = false; l.foot = l.to; }
    }
}

} // namespace sp::spider
