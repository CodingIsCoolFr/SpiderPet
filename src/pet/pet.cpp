#include "pet/pet.hpp"

#include "core/util.hpp"

#include <algorithm>

namespace sp {
namespace {

const wchar_t* kAdjectives[] = {L"cardboard", L"soggy",   L"quantum", L"velvet",  L"feral",   L"haunted", L"tiny",
                                L"rusty",     L"cosmic",  L"sleepy",  L"crunchy", L"polite",  L"sneaky", L"lunar",
                                L"plastic",   L"gloomy",  L"frantic", L"bashful", L"wobbly",  L"neon",   L"dusty",
                                L"grumpy",    L"paper",   L"midnight", L"spicy",  L"hollow",  L"mossy",  L"electric"};
const wchar_t* kNouns[] = {L"priest",   L"harmonica", L"pickle",  L"librarian", L"toaster", L"comet",   L"accountant",
                           L"teapot",   L"wizard",    L"noodle",  L"archivist", L"pigeon",  L"cactus",  L"goblin",
                           L"lantern",  L"bishop",    L"raccoon", L"waffle",    L"oracle",  L"gremlin", L"cassette",
                           L"monk",     L"crouton",   L"kettle",  L"walrus",    L"clerk",   L"sock",    L"banana"};

std::wstring shorten(const std::wstring& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n - 1) + L"…";
}

bool in_rect(const Rect& r, Vec2 p) { return r.contains(p); }

constexpr double kReadable = 3.0;  // seconds a thought stays before the next one

// Chat apps and live pages redraw text all the time; the same words get found
// again as "new". This key is how the spider remembers it already ate them.
std::wstring meal_key(const Entity& e) { return std::wstring(wide(kind_name(e.kind))) + L"|" + lower(e.text); }

}  // namespace

Pet::Pet() { reroll_name(); }

void Pet::reroll_name() {
  Rng r(static_cast<uint64_t>(now_seconds() * 1e6) ^ 0xA5A5A5A5ull);
  name_ = std::wstring(kAdjectives[r.below(std::size(kAdjectives))]) + L" " + kNouns[r.below(std::size(kNouns))];
}

void Pet::set_scale(float s) {
  if (std::fabs(s - scale_) < 0.02f) return;
  scale_ = s;
  spider_.configure(s);
}

const char* Pet::state_name() const {
  switch (state_) {
    case State::Off: return "away";
    case State::Descend: return "dropping in";
    case State::Think: return "looking around";
    case State::Travel: return "heading over";
    case State::Grab: return "grabbing";
    case State::Read: return "reading";
    case State::Roam: return "exploring";
    case State::Idle: return "resting";
    case State::Drag: return "held";
    case State::Thrown: return "flying";
    case State::Ascend: return "climbing out";
    case State::Point: return "showing you";
  }
  return "";
}

// Each thought stays up long enough to read; a newer one waits its turn
// (only the latest waits, older ones are dropped).
void Pet::say(std::wstring text, double now) {
  if (text == thought_) return;
  if (!thought_.empty() && now - thought_at_ < kReadable) {
    pending_ = std::move(text);
    return;
  }
  thought_ = std::move(text);
  thought_at_ = now;
  pending_.clear();
  history_.push_front({now, thought_});
  if (history_.size() > 40) history_.pop_back();
}

void Pet::start_descend(Vec2 at, const View& v, double now) {
  state_ = State::Descend;
  state_at_ = now;
  timer_ = 0;
  from_ = {at.x, v.visible.y - spider_.reach() * 0.6f};
  to_ = at;
  descend_dur_ = std::clamp((to_.y - from_.y) / (900.f * scale_), 0.7f, 1.4f);
  spider_.set_airborne(true);
  spider_.set_curl(0.35f);
  spider_.set_flail(0.15f);
  spider_.release_anchors();
  spider_.clear_goal();
  spider_.place(from_, kPi * 0.5f);
  thread_ = {true, {at.x, v.visible.y}, 1.f};
  target_ = 0;
}

void Pet::summon(Vec2 at, const View& v, double now) {
  summoned_at_ = now;
  start_descend(at, v, now);
}

void Pet::dismiss(double now) {
  if (state_ == State::Off) return;
  state_ = State::Ascend;
  state_at_ = now;
  timer_ = 0;
  from_ = spider_.pos();
  spider_.set_airborne(true);
  spider_.set_curl(0.4f);
  spider_.release_anchors();
  thread_ = {true, {from_.x, from_.y - 2000.f}, 1.f};
  target_ = 0;
}

void Pet::shift(Vec2 d) {
  spider_.shift(d);
  from_ += d;
  to_ += d;
  thread_.top += d;
}

void Pet::forget_page() {
  target_ = 0;
  meals_ = 0;
  eaten_.clear();
  frontier_ = -1e9f;
  web_.clear();
  last_done_ = 0;
  gist_.clear();
  gist_asked_ = false;
  spider_.release_anchors();
  if (state_ == State::Travel || state_ == State::Grab || state_ == State::Read || state_ == State::Roam)
    state_ = State::Think;
}

bool Pet::hit(Vec2 p) const {
  if (state_ == State::Off || state_ == State::Ascend) return false;
  const float r = std::max(16.f, spider_.body_len() * 0.6f);
  return distance(p, spider_.draw_pos()) < r;
}

void Pet::grab(Vec2 p, double now) {
  state_ = State::Drag;
  state_at_ = now;
  drag_offset_ = spider_.pos() - p;
  drag_last_ = p;
  drag_last_t_ = now;
  drag_vel_ = {};
  target_ = 0;
  spider_.release_anchors();
  spider_.set_airborne(true);
  spider_.set_held(true);
  spider_.set_curl(0.75f);
  spider_.set_flail(1.f);
  thread_.on = false;
  say(persona_.line(Persona::Moment::Lifted), now);
}

void Pet::drag(Vec2 p, double now) {
  if (state_ != State::Drag) return;
  const float dt = static_cast<float>(now - drag_last_t_);
  if (dt > 0.001f) {
    const Vec2 v = (p - drag_last_) * (1.f / dt);
    drag_vel_ = lerp(drag_vel_, v, std::clamp(dt * 18.f, 0.f, 1.f));
    drag_last_ = p;
    drag_last_t_ = now;
  }
  spider_.set_position(p + drag_offset_);
  spider_.set_velocity(drag_vel_);
}

void Pet::release(double now) {
  if (state_ != State::Drag) return;
  state_ = State::Thrown;
  state_at_ = now;
  spider_.set_held(false);
  Vec2 v = drag_vel_;
  const float sp = length(v);
  const float cap = 1600.f * scale_;
  if (sp > cap) v = v * (cap / sp);
  spider_.set_velocity(v);
  spider_.spin(std::clamp(sp / (300.f * scale_), 0.f, 8.f) * (rng_.unit() < 0.5f ? -1.f : 1.f));
}

Vec2 Pet::approach(const Entity& e, float& face) const {
  const float L = spider_.body_len();
  if (e.kind == Kind::Sentence && !e.lines.empty()) {
    const Rect& l = e.lines.front();
    face = 0;
    return {l.x - L * 0.32f + 2.f, l.bottom() + spider_.body_wid() * 0.5f + 3.f * scale_};
  }
  const Vec2 p = spider_.pos();
  const Vec2 edge = e.box.nearest(p);
  Vec2 out = p - edge;
  if (length(out) < 1.f) out = {0, 1};
  out = normalized(out);
  face = std::atan2(-out.y, -out.x);
  return edge + out * (L * 0.5f + 12.f * scale_);
}

Vec2 Pet::sweep_point(const Entity& e, float p, Rect& line) const {
  float total = 0;
  for (const Rect& r : e.lines) total += r.w;
  float left = std::clamp(p, 0.f, 1.f) * total;
  for (const Rect& r : e.lines) {
    if (left <= r.w || &r == &e.lines.back()) {
      line = r;
      return {r.x + std::min(left, r.w), r.center().y};
    }
    left -= r.w;
  }
  line = e.box;
  return e.box.center();
}

void Pet::lock(Entity& e, double now, const Settings& s) {
  e.mark = Mark::Locked;
  target_ = e.id;
  state_ = State::Travel;
  state_at_ = now;
  timer_ = 0;
  spider_.release_anchors();
  if (e.kind == Kind::Sentence) {
    say(e.summary.empty() ? persona_.line(Persona::Moment::Read)
                          : persona_.line(Persona::Moment::Learned, &e, wide(e.summary)),
        now);
    if (s.brain && e.summary.empty()) asks_.push_back({e.id, "sum", e.text});
  } else {
    say(persona_.line(Persona::Moment::Lock, &e), now);
  }
}

void Pet::apply(Entity& e, double now) {
  e.mark = Mark::Done;
  e.applied_at = now;
  e.read = 1.f;
  if (e.kind != Kind::Sentence && e.lines.size() == 1 && rng_.unit() < 0.14f) {
    e.tilt = rng_.range(0.05f, 0.16f) * (rng_.unit() < 0.5f ? -1.f : 1.f);
    e.nudge = Vec2{rng_.range(-6.f, 6.f), rng_.range(-4.f, 4.f)} * scale_;
  }
  if (eaten_.insert(meal_key(e)).second) harvest_.push_back({e.id, e.kind, e.text, e.label, e.summary, e.href});
  frontier_ = std::max(frontier_, e.box.center().y);
  if (last_done_ > 0 && last_done_ != e.id) web_.push_back({last_done_, e.id, now});
  if (web_.size() > 160) web_.erase(web_.begin());
  last_done_ = e.id;
  target_ = 0;
  spider_.release_anchors();
  ++meals_;
  if (meals_ % 10 == 0) say(persona_.line(Persona::Moment::Meals, nullptr, {}, meals_), now);
  else if (e.kind == Kind::Sentence && !e.summary.empty()) say(persona_.line(Persona::Moment::Learned, &e, wide(e.summary)), now);
  else if (rng_.unit() < 0.3f) say(persona_.line(Persona::Moment::Absorbed, &e), now);
}

void Pet::point(const Entity& e, double now) {
  if (state_ == State::Off || state_ == State::Drag || state_ == State::Descend || state_ == State::Ascend) return;
  target_ = e.id;
  state_ = State::Point;
  state_at_ = now;
  timer_ = 0;
  spider_.release_anchors();
  thought_.clear();  // say it right away
  say(persona_.line(Persona::Moment::Here, &e), now);
}

void Pet::abandon(Page& page) {
  if (Entity* e = page.find(target_)) {
    if (e->mark == Mark::Locked) {
      e->mark = ++e->fails >= 2 ? Mark::Skip : Mark::New;
    }
  }
  target_ = 0;
  spider_.release_anchors();
  state_ = State::Think;
}

void Pet::think(double now, Page& page, const View& v, const Settings& s) {
  const Rect& vis = v.visible;
  const Vec2 p = spider_.pos();
  Entity* best = nullptr;
  float best_cost = 1e9f, best_value = 0.f;
  // Where harvests already cluster, more of the same is worth less.
  std::vector<float> done_y;
  for (const Entity& e : page.entities())
    if (e.mark == Mark::Done) done_y.push_back(e.box.center().y);
  const float band = vis.h * 0.3f;
  for (Entity& e : page.entities()) {
    if (e.mark != Mark::New || e.fails >= 2) continue;
    if (eaten_.count(meal_key(e))) {  // seen it before: keep its look, do not eat it twice
      e.mark = Mark::Done;
      e.applied_at = now - 10.0;
      e.read = 1.f;
      continue;
    }
    if (e.kind == Kind::Sentence && !s.read) continue;
    const bool in_view = e.box.y >= vis.y + 2 && e.box.bottom() <= vis.bottom() - 2 && e.box.x >= vis.x - 2 &&
                         e.box.right() <= vis.right() + 2;
    if (!in_view) {
      if (!v.crawl_ok || e.box.y < vis.bottom() - 2 || e.box.y > vis.bottom() + vis.h * 1.2f) continue;
    }
    const float cy = e.box.center().y;
    const float d = distance(p, e.box.nearest(p));
    float cost = d / (140.f * scale_) - kind_priority(e.kind) - (e.key ? 1.5f : 0.f);
    if (e.kind == Kind::Sentence) cost -= e.score * 0.3f;
    if (!in_view) cost += 1.5f + (e.box.y - vis.bottom()) / vis.h * 2.f;
    // Read like a person: top to bottom. Going back up costs extra.
    if (cy < frontier_ - 40.f * scale_) cost += 1.f;
    else if (cy < p.y - 220.f * scale_) cost += 0.6f;
    int crowd = 0;
    for (float y : done_y) crowd += std::fabs(y - cy) < band ? 1 : 0;
    cost += 0.22f * crowd;
    if (cost < best_cost) {
      best_cost = cost;
      best = &e;
      // What the find is worth by itself, travel aside.
      best_value = kind_priority(e.kind) + (e.key ? 1.5f : 0.f) - 0.22f * crowd +
                   (e.kind == Kind::Sentence ? e.score * 0.3f : 0.f);
    }
  }
  // When the page goes on and only low-value leftovers are near, move on.
  // Lines cut by the bottom edge are not in the model yet, so the page always
  // counts as going on until a scroll stops moving it.
  const bool more_below_now = !v.page_end;
  if (best && v.attached && v.crawl_ok && more_below_now && best_value < 1.0f) best = nullptr;
  if (best) {
    lock(*best, now, s);
    return;
  }
  const bool more_below = !v.page_end;
  if (v.attached && v.crawl_ok && more_below) {
    if (state_ != State::Roam) say(persona_.line(Persona::Moment::Crawl), now);
    state_ = State::Roam;
    state_at_ = now;
    return;
  }
  if (state_ != State::Idle) {
    bool below = false;
    for (const Entity& e : page.entities())
      below |= e.mark == Mark::New && e.box.y > vis.bottom() - 2;
    if (v.attached && page.empty()) say(persona_.line(Persona::Moment::Barren), now);
    else if (v.attached && v.page_end) say(persona_.line(Persona::Moment::Drained), now);
    else if (v.attached && (below || more_below_now)) say(persona_.line(Persona::Moment::More), now);
    chatter_ = rng_.range(7.f, 12.f);
    state_ = State::Idle;
    state_at_ = now;
    idle_t_ = 0;
    wander_t_ = rng_.range(1.f, 3.f);
    groom_t_ = rng_.range(2.f, 5.f);
  }
}

void Pet::update(double now, float dt, Page& page, const View& v, const Settings& s) {
  wants_scroll_ = 0;
  if (state_ == State::Off) return;
  if (!pending_.empty() && now - thought_at_ >= kReadable) {
    std::wstring next = std::move(pending_);
    pending_.clear();
    say(std::move(next), now);
  }
  const Rect& vis = v.visible;
  const float L = spider_.body_len();
  if (s.brain && v.attached && !gist_asked_) {
    const std::wstring sample = page.sample(1400);
    if (sample.size() >= 300) {
      gist_asked_ = true;
      asks_.push_back({0, "gist", sample});
    }
  }
  auto snap = [&page](Vec2 q, float r, Vec2& out) { return page.snap(q, r, out); };

  // Fell off the visible area (you scrolled away): drop back in on a thread.
  const bool grounded = state_ != State::Descend && state_ != State::Ascend && state_ != State::Drag &&
                        state_ != State::Thrown;
  if (grounded && !in_rect(vis.inflated(30.f * scale_), spider_.pos())) {
    away_ += dt;
    if (away_ > 0.8f) {
      away_ = 0;
      abandon(page);
      const float x = std::clamp(spider_.pos().x, vis.x + spider_.reach(), vis.right() - spider_.reach());
      start_descend({x, vis.y + vis.h * 0.42f}, v, now);
    }
  } else {
    away_ = 0;
  }

  if (thread_.on && state_ != State::Descend && state_ != State::Ascend) {
    thread_.alpha -= dt * 3.f;
    thread_.top.y -= dt * 900.f * scale_;
    if (thread_.alpha <= 0) thread_.on = false;
  }

  switch (state_) {
    case State::Off:
      break;
    case State::Descend: {
      timer_ += dt / descend_dur_;
      const float k = std::min(1.f, timer_);
      const float y = lerp(from_.y, to_.y, ease_out_back(k));
      spider_.set_position({to_.x + std::sin(timer_ * 7.f) * 2.f * (1 - k), y});
      spider_.set_face(kPi * 0.5f);
      if (k > 0.65f) spider_.set_curl(1.f);
      thread_.top.x = to_.x;
      if (timer_ >= 1.f) {
        spider_.set_airborne(false);
        spider_.set_flail(0);
        spider_.clear_face();
        state_ = State::Think;
        state_at_ = now;
        timer_ = 0;
        say(persona_.line(v.attached ? Persona::Moment::Land : Persona::Moment::Loose), now);
      }
      break;
    }
    case State::Ascend: {
      timer_ += dt / 0.9f;
      const float k = std::min(1.f, timer_);
      spider_.set_position({from_.x, lerp(from_.y, vis.y - spider_.reach() * 1.5f, ease_in_cubic(k))});
      thread_.top = {from_.x, vis.y - 10.f};
      if (timer_ >= 1.f) {
        state_ = State::Off;
        thread_.on = false;
      }
      break;
    }
    case State::Drag:
      break;
    case State::Thrown: {
      Vec2 vel = spider_.vel() * std::exp(-3.2f * dt);
      Vec2 p = spider_.pos() + vel * dt;
      const Rect box = vis.inflated(-L);
      if (p.x < box.x || p.x > box.right()) {
        vel.x = -vel.x * 0.5f;
        p.x = std::clamp(p.x, box.x, box.right());
      }
      if (p.y < box.y || p.y > box.bottom()) {
        vel.y = -vel.y * 0.5f;
        p.y = std::clamp(p.y, box.y, box.bottom());
      }
      spider_.set_position(p);
      spider_.set_velocity(vel);
      spider_.set_flail(std::clamp(length(vel) / (400.f * scale_), 0.2f, 1.f));
      if (length(vel) < 70.f * scale_ || now - state_at_ > 1.4) {
        say(persona_.line(Persona::Moment::Landed), now);
        spider_.set_velocity(vel * 0.3f);
        spider_.set_airborne(false);
        spider_.set_flail(0);
        state_ = State::Think;
        state_at_ = now;
      }
      break;
    }
    case State::Think:
      if (now - state_at_ > 0.12) think(now, page, v, s);
      break;
    case State::Idle: {
      idle_t_ += dt;
      chatter_ -= dt;
      if (chatter_ <= 0) {
        chatter_ = rng_.range(8.f, 14.f);
        say(persona_.line(v.attached || !page.empty() ? Persona::Moment::Rest : Persona::Moment::Loose), now);
      }
      wander_t_ -= dt;
      groom_t_ -= dt;
      if (grooming_ && groom_t_ < -1.4f) {
        grooming_ = false;
        spider_.release_anchors();
        groom_t_ = rng_.range(4.f, 8.f);
      } else if (!grooming_ && groom_t_ <= 0) {
        grooming_ = true;
      }
      if (grooming_) {
        const Vec2 f = spider_.forward();
        const Vec2 h = spider_.head();
        for (int side : {-1, 1}) {
          const float w = std::sin(static_cast<float>(now) * 13.f + side) * 3.f * scale_;
          spider_.anchor(side, 0, h + f * (spider_.reach() * 0.28f) + perp(f) * (side * spider_.reach() * 0.1f + w));
        }
      }
      if (wander_t_ <= 0 && !grooming_) {
        wander_t_ = rng_.range(3.f, 6.f);
        const float a = rng_.range(-kPi, kPi);
        Vec2 g = spider_.pos() + from_angle(a) * (rng_.range(30.f, 80.f) * scale_);
        g.x = std::clamp(g.x, vis.x + L, vis.right() - L);
        g.y = std::clamp(g.y, vis.y + L, vis.bottom() - L);
        spider_.set_goal(g, 70.f * scale_, 40.f * scale_);
      }
      if (idle_t_ > 0.8f) {
        idle_t_ = 0;
        const State before = state_;
        think(now, page, v, s);
        if (state_ != before) {
          grooming_ = false;
          spider_.release_anchors();
          spider_.clear_goal();
        }
      }
      break;
    }
    case State::Roam: {
      const float x = page.column_x() > 0 ? page.column_x() + spider_.reach() : vis.center().x;
      spider_.set_goal({std::clamp(x, vis.x + L, vis.right() - L), vis.bottom() - L}, 150.f * scale_, 60.f * scale_);
      wants_scroll_ = std::max(0.f, spider_.pos().y - (vis.y + vis.h * 0.62f));
      timer_ += dt;
      if (!v.crawl_ok) {
        state_ = State::Think;
      } else if (timer_ > 0.5f) {
        timer_ = 0;
        think(now, page, v, s);
      }
      break;
    }
    case State::Travel: {
      Entity* e = page.find(target_);
      if (!e || e->mark != Mark::Locked) {
        abandon(page);
        break;
      }
      float face = 0;
      const Vec2 goal = approach(*e, face);
      spider_.set_goal(goal, 300.f * scale_, 70.f * scale_);
      timer_ += dt;
      if (v.crawl_ok) wants_scroll_ = std::max(0.f, std::max(e->box.bottom(), spider_.pos().y) - (vis.y + vis.h * 0.7f));
      // Close enough to throw a line at it: no need to walk the whole way.
      const Vec2 h = spider_.head();
      const float gap = distance(h, e->box.nearest(h));
      const bool in_view = e->box.y >= vis.y && e->box.bottom() <= vis.bottom();
      if ((gap <= spider_.reach() * 3.f && in_view) || spider_.arrived(6.f * scale_)) {
        state_ = e->kind == Kind::Sentence ? State::Read : State::Grab;
        state_at_ = now;
        timer_ = 0;
        tether_ = 0;
        e->read = 0;
      } else if (timer_ > 7.f) {
        abandon(page);
      } else if (!v.crawl_ok && (e->box.bottom() < vis.y || e->box.y > vis.bottom())) {
        abandon(page);
      }
      break;
    }
    case State::Point: {
      Entity* e = page.find(target_);
      timer_ += dt;
      if (!e || timer_ > 4.f) {
        target_ = 0;
        spider_.release_anchors();
        spider_.clear_face();
        state_ = State::Think;
        state_at_ = now;
        break;
      }
      float face = 0;
      spider_.set_goal(approach(*e, face), 340.f * scale_, 70.f * scale_);
      tip_ = e->box.nearest(spider_.head());
      if (spider_.arrived(8.f * scale_)) spider_.set_face(face);
      break;
    }
    case State::Grab: {
      Entity* e = page.find(target_);
      if (!e) {
        abandon(page);
        break;
      }
      timer_ += dt;
      // The line shoots out, holds, the find changes, the line snaps back.
      // Meanwhile the spider keeps creeping closer so its legs stay busy.
      const Vec2 h = spider_.head();
      const Vec2 near = e->box.nearest(h);
      const Vec2 to = near - spider_.pos();
      const float dist = length(to);
      const Vec2 creep = spider_.pos() + normalized(to) * std::max(0.f, dist - spider_.reach() * 1.1f) * 0.6f;
      spider_.set_goal(creep, 110.f * scale_, 40.f * scale_);
      spider_.set_face(std::atan2(to.y, to.x));
      tether_ = std::min(1.f, timer_ / 0.14f);
      tip_ = near;
      if (distance(h, near) < spider_.reach() * 0.9f) {
        const Vec2 side = perp(spider_.forward()) * (spider_.reach() * 0.3f);
        spider_.anchor(-1, 0, e->box.inflated(-1.f).nearest(h - side));
        spider_.anchor(1, 0, e->box.inflated(-1.f).nearest(h + side));
      } else {
        spider_.release_anchor(-1, 0);
        spider_.release_anchor(1, 0);
      }
      if (timer_ > 0.4f && timer_ - dt <= 0.4f) apply(*e, now);
      if (timer_ > 0.52f) {
        spider_.clear_face();
        tether_ = 0;
        state_ = State::Think;
        state_at_ = now;
      }
      break;
    }
    case State::Read: {
      Entity* e = page.find(target_);
      if (!e) {
        abandon(page);
        break;
      }
      // Scanning: the line's tip runs along the sentence and the highlight
      // follows it. The spider drifts along below instead of walking each line.
      const float dur = std::clamp(static_cast<float>(e->text.size()) / 60.f, 0.8f, 3.f);
      e->read = std::min(1.f, e->read + dt / dur);
      tether_ = std::min(1.f, static_cast<float>(now - state_at_) / 0.14f);
      Rect line;
      const Vec2 at = sweep_point(*e, e->read, line);
      tip_ = at;
      Vec2 goal{at.x - spider_.reach() * 0.5f, e->box.bottom() + spider_.reach() * 0.8f};
      goal.x = std::clamp(goal.x, vis.x + L, vis.right() - L);
      goal.y = std::clamp(goal.y, vis.y + L, vis.bottom() - L);
      spider_.set_goal(goal, 160.f * scale_, 80.f * scale_);
      if (distance(spider_.head(), at) < spider_.reach() * 0.95f) spider_.anchor(-1, 0, {at.x, line.center().y});
      else spider_.release_anchor(-1, 0);
      if (v.crawl_ok) wants_scroll_ = std::max(0.f, line.bottom() - (vis.y + vis.h * 0.72f));
      if (e->read >= 1.f) {
        apply(*e, now);
        tether_ = 0;
        state_ = State::Think;
        state_at_ = now;
      }
      break;
    }
  }

  if (state_ == State::Idle || state_ == State::Think || state_ == State::Descend || state_ == State::Ascend)
    spider_.set_burst(true);
  spider_.tick(dt, snap);
}

void Pet::answer(const Ask& ask, const std::string& text, Page& page, double now) {
  if (text.empty()) return;
  if (ask.task == "gist") {
    gist_ = text;
    say(persona_.line(Persona::Moment::Gist, nullptr, wide(text)), now);
    return;
  }
  if (Entity* e = page.find(ask.entity)) {
    e->summary = text;
    if (target_ == e->id) say(persona_.line(Persona::Moment::Learned, e, wide(text)), now);
  }
}

}  // namespace sp
