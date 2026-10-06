// spider.hpp - the crawler: body steering, 8 legs with 2-bone IK and a
// tetrapod gait. Feet land on real words of the page (the overlay asks the
// browser what is under each foot).
//
// Coordinates are "page space": x like the screen, y = screen y + scroll, so
// planted feet stay on their words while the page scrolls.
#pragma once
#include <cmath>
#include <functional>
#include <random>
#include <string>

namespace sp::spider {

struct Vec {
    float x = 0, y = 0;
    Vec operator+(Vec o) const { return {x + o.x, y + o.y}; }
    Vec operator-(Vec o) const { return {x - o.x, y - o.y}; }
    Vec operator*(float s) const { return {x * s, y * s}; }
    float len() const { return std::sqrt(x * x + y * y); }
    Vec norm() const { float l = len(); return l > 1e-4f ? Vec{x / l, y / l} : Vec{1, 0}; }
};
inline Vec rot(Vec v, float a) { float c = std::cos(a), s = std::sin(a); return {v.x * c - v.y * s, v.x * s + v.y * c}; }
inline Vec lerp(Vec a, Vec b, float t) { return a + (b - a) * t; }

struct Leg {
    Vec hip_local;        // body frame: x forward, y right
    float rest_angle = 0; // from heading
    float rest_dist = 0;
    float l1 = 50, l2 = 60;
    bool right = true;
    int group = 0;
    Vec foot, from, to;
    float t = 1;          // step progress, 1 = planted
    bool stepping = false;
    unsigned ask = 0;     // id of the pending word lookup
    Vec override_target;  // used while inspecting a claim
    bool has_override = false;
};

// Asks the browser for the word near a page-space point. The answer comes
// back later through Spider::snap().
using AskWord = std::function<void(int leg, unsigned ask, Vec page_point)>;

class Spider {
public:
    Vec pos, vel;
    float heading = 1.5708f;
    float scale = 1;
    Leg legs[8];
    Vec goal;
    bool has_goal = false;
    float max_speed = 260;
    float drop = 0;       // entrance: hanging from a thread, 1 -> 0

    void init(Vec start, float scale_, unsigned seed);
    void update(float dt, const AskWord& ask);
    // A word answer for leg i: aim the foot at this point instead.
    void snap(int leg, unsigned ask, Vec page_point);
    Vec hip(int i) const;
    Vec knee(int i) const;
    Vec head() const;
    Vec tail() const;
    bool arrived(float radius) const { return (goal - pos).len() < radius * scale; }
    // Every foot moves once so it lands on a word again (after a scroll jump).
    void replant();
    // Moves the whole spider (feet too) and lets it drop in on a thread.
    void teleport(Vec to);

private:
    std::mt19937 rng_;
    float twitch_ = 0;
    int replant_ = 0;
    unsigned next_ask_ = 1;
    float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng_); }
    Vec rest_point(const Leg& l) const;
    void start_step(int i, Vec target, const AskWord& ask);
};

} // namespace sp::spider
