#pragma once

// The stage: a see-through, click-through layer over the window the spider is
// on. It draws the spider (coral legs, mint joints, a thin navy body, a
// magenta head), its thought cloud, the finds it ate re-set on navy tags that
// fit their own words, a check line under each find, verdict labels in the
// margin, and what matches your search, lit up. Direct2D through
// DirectComposition on its own thread.
//
// Never in the wrong place: anything measured before the window or its
// content last moved is hidden until it is measured again (the Wingman
// overlay's rule), and labels never cover text.

#include "eyes/eyes.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sp::crawl {

// A find, where it is on screen.
struct Mark {
  std::string id, kind, text;          // kind: doi isbn id title sentence heading link
  std::vector<eyes::Box> boxes;        // content space: x from the view's left, y from its top + scroll when measured
  bool eaten = false;                  // restyled: the spider has it
  bool match = false;                  // matches what you are looking for
  int verdict = 0;                     // 0 none, 1 checking, 2 verified, 3 wrong, 4 not found, 5 no proof, 6 opinion, 7 ad
  unsigned long long at_ms = 0;        // when the boxes were measured: drawn only if nothing moved since
  unsigned layout = 0;                 // eyes::State::layout then
};

struct Scene {
  bool show = false;
  std::string name = "SpiderPet";      // in its thought cloud
  std::vector<Mark> marks;
  std::string goal;                    // id of the mark the spider walks to ("" = none)
  eyes::Box goal_screen;               // or a thing on screen (a task's button), screen pixels
  bool hold = false;                   // its front legs hold the goal (eating, or a task's button)
  std::string reading;                 // id of the sentence it reads now
  std::vector<eyes::Box> read;         // content space: what it has read (silk in the margin)
  std::vector<eyes::Box> columns;      // content space: every paragraph (verdict labels go in the margin beside them)
  unsigned spans_layout = ~0u;         // eyes::State::layout of `read` and `columns` (other: out of date, not drawn)
  std::string say;                     // a line for the thought cloud
  unsigned say_id = 0;                 // bumps for a new line
  std::string hud;                     // a small status under the spider
  bool hide_in_shares = false;         // screen shares and recordings leave the spider out
};

class Stage {
 public:
  explicit Stage(eyes::Eyes& eyes);
  ~Stage();
  void set(const Scene& s);
  // The spider is at its goal (within reach).
  bool at_goal() const;
  // Drag and drop without a mouse drag: the spider follows the cursor until
  // you click a window (it is dropped there) or press Escape (null).
  void pick(std::function<void(HWND)> done);
  bool picking() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace sp::crawl
