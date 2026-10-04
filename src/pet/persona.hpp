#pragma once

#include "core/geom.hpp"
#include "page/page.hpp"

#include <string>

namespace sp {

// The spider's voice: a small digital organism crawling the web. It feeds on
// information, senses signals, digests what it finds. Lowercase fragments,
// a few words at a time, so the bubble says what it is doing at a glance.
class Persona {
 public:
  enum class Moment {
    Land,       // dropped onto a page
    Loose,      // on the desktop, no page
    Sense,      // looking for something
    Lock,       // picked a target (uses the entity's kind)
    Absorbed,   // finished grabbing a find
    Read,       // started reading a sentence
    Learned,    // a sentence summary arrived (uses `extra`)
    Gist,       // the page's gist arrived (uses `extra`)
    Crawl,      // moving down the page, scrolling
    Rest,       // idle
    Lifted,     // grabbed by the mouse
    Landed,     // came down after a throw
    Barren,     // page with nothing to eat
    Drained,    // ate everything in reach
    Meals,      // milestone count (uses `count`)
    More,       // more page below, but not allowed to scroll
    Here,       // showing the user where a find is
  };

  std::wstring line(Moment m, const Entity* e = nullptr, const std::wstring& extra = {}, int count = 0);

 private:
  const wchar_t* pick(const wchar_t* const* options, int n);
  Rng rng_{0x0DA7A5EEDull};
  const wchar_t* last_ = nullptr;
};

}  // namespace sp
