#pragma once

#include "page/page.hpp"
#include "pet/persona.hpp"
#include "pet/spider.hpp"

#include <deque>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sp {

struct Settings {
  bool crawl = true;  // scroll the page by itself when you are away
  bool read = true;   // read key sentences, not only harvest data
  bool web = true;    // leave silk between harvested things
  bool brain = true;  // ask the local model for short summaries
  bool share = true;  // let screen sharing and recordings see the spider
  float size = 1.f;
};

// What the spider can see right now, in content coordinates.
struct View {
  Rect visible;
  bool attached = false;  // on a page, or loose on the desktop
  bool crawl_ok = false;  // allowed to ask for a scroll this frame
  bool page_end = false;  // scrolling stopped moving the page: nothing more below
};

struct Harvest {
  int entity = 0;
  Kind kind = Kind::Sentence;
  std::wstring text;
  std::wstring label;
  std::string summary;
  std::wstring href;
};

struct Ask {
  int entity = 0;  // 0 = the page itself
  std::string task;
  std::wstring text;
};

class Pet {
 public:
  enum class State { Off, Descend, Think, Travel, Grab, Read, Roam, Idle, Drag, Thrown, Ascend, Point };

  Pet();
  void set_scale(float s);
  void reroll_name();
  const std::wstring& name() const { return name_; }

  void summon(Vec2 at, const View& v, double now);
  void dismiss(double now);
  void shift(Vec2 d);
  void forget_page();
  void clear_silk() {
    web_.clear();
    last_done_ = 0;
  }

  bool hit(Vec2 p) const;
  void grab(Vec2 p, double now);
  void drag(Vec2 p, double now);
  void release(double now);
  bool dragging() const { return state_ == State::Drag; }

  void update(double now, float dt, Page& page, const View& v, const Settings& s);
  // Run over to a find and hold the line on it, so you can see where it is.
  void point(const Entity& e, double now);
  void answer(const Ask& ask, const std::string& text, Page& page, double now);

  float wants_scroll() const { return wants_scroll_; }
  std::vector<Harvest> take_harvest() { return std::exchange(harvest_, {}); }
  std::vector<Ask> take_asks() { return std::exchange(asks_, {}); }

  State state() const { return state_; }
  const char* state_name() const;
  bool active() const { return state_ != State::Off; }
  const Spider& spider() const { return spider_; }
  int target() const { return target_; }
  bool tethered() const {
    return target_ > 0 &&
           (state_ == State::Travel || state_ == State::Grab || state_ == State::Read || state_ == State::Point);
  }
  // How far the line is out (0..1) and, while reading, where its tip is.
  float tether_out() const { return state_ == State::Travel || state_ == State::Point ? 1.f : tether_; }
  Vec2 tether_tip() const { return tip_; }
  const std::deque<std::pair<double, std::wstring>>& history() const { return history_; }

  const std::wstring& thought() const { return thought_; }
  bool busy() const { return state_ == State::Travel || state_ == State::Grab || state_ == State::Read; }
  double thought_at() const { return thought_at_; }
  const std::string& gist() const { return gist_; }

  struct Thread {
    bool on = false;
    Vec2 top{};
    float alpha = 0;
  };
  const Thread& thread() const { return thread_; }
  struct Silk {
    int a = 0, b = 0;
    double at = 0;
  };
  const std::vector<Silk>& web() const { return web_; }
  int last_done() const { return last_done_; }
  double summoned_at() const { return summoned_at_; }

 private:
  void say(std::wstring text, double now);
  void think(double now, Page& page, const View& v, const Settings& s);
  void lock(Entity& e, double now, const Settings& s);
  void apply(Entity& e, double now);
  void abandon(Page& page);
  void start_descend(Vec2 at, const View& v, double now);
  Vec2 approach(const Entity& e, float& face) const;
  Vec2 sweep_point(const Entity& e, float p, Rect& line) const;

  Spider spider_;
  Rng rng_{0xC0FFEE};
  State state_ = State::Off;
  std::wstring name_;
  std::wstring thought_;
  std::wstring pending_;  // next thought, shown once the current one was readable
  std::deque<std::pair<double, std::wstring>> history_;
  float tether_ = 0;
  Vec2 tip_{};
  double thought_at_ = 0;
  std::string gist_;
  bool gist_asked_ = false;
  int target_ = 0;
  double state_at_ = 0;
  float timer_ = 0;
  float idle_t_ = 0, groom_t_ = 0, wander_t_ = 0;
  bool grooming_ = false;
  float away_ = 0;
  Vec2 from_{}, to_{};
  float descend_dur_ = 1.1f;
  Thread thread_;
  std::vector<Silk> web_;
  int last_done_ = 0;
  double summoned_at_ = -10;
  float wants_scroll_ = 0;
  Vec2 drag_offset_{};
  Vec2 drag_last_{};
  double drag_last_t_ = 0;
  Vec2 drag_vel_{};
  float scale_ = 1;
  float frontier_ = -1e9f;  // deepest point harvested on this page
  Persona persona_;
  std::unordered_set<std::wstring> eaten_;  // kind|text already harvested on this page
  int meals_ = 0;
  float chatter_ = 6.f;
  std::vector<Harvest> harvest_;
  std::vector<Ask> asks_;
};

}  // namespace sp
