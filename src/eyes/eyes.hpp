#pragma once

// The spider's eyes: it reads any window on the PC, with no browser extension.
//
// Windows' own screen-reader interface (UI Automation, the one NVDA and
// Narrator use) gives the real text of a browser page, a chat app, a document
// or a settings window, where every piece sits, and every button, link and
// box. Ideas and rules come from open projects that do the same for AI
// agents: Windows-MCP (what counts as clickable, cycle guard, element budget),
// Cua's Windows driver (a numbered list of what is on screen) and Hermes
// Agent (screen + numbered elements for the model). All MIT.
//
// Everything that talks to UI Automation runs on one worker thread; the
// public methods queue work there and wait.

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sp::eyes {

struct Box {
  float x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// Which app a window belongs to.
struct AppInfo {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  std::string exe;      // "firefox.exe"
  std::string name;     // "Firefox", "Discord", "Notepad" (from the program's own description)
  std::string title;    // the window title
  std::string cls;      // window class
  std::string engine;   // "firefox" / "chromium" for browsers, else ""
  bool browser = false;
};
AppInfo identify(HWND h);
// The app window under a screen point (not SpiderPet's own windows).
HWND window_at(POINT pt);
bool is_ours(HWND h);

// One read of a window.
struct Look {
  bool ok = false;
  std::string error;
  AppInfo app;
  std::string how;                   // "page" (a web page or document), "app" (an app's own controls), "picture" (read from pixels)
  std::string url;                   // web address; apps get app://<exe>/<title>
  std::string title, lang, desc;
  std::string text;                  // the readable text, paragraphs on their own lines (up to ~12000 chars)
  std::vector<std::string> blocks;   // paragraphs in reading order
  std::vector<Box> spans;            // where each block is: content space (x from the view's left, y from its top + scroll)
  std::vector<int> heading;          // heading level per block (0 = body text)
  struct Link {
    std::string href, text;
  };
  std::vector<Link> links;
  std::vector<std::string> controls; // an app's buttons, menus and tabs, by name (for understanding what it is)
  Box view;                          // the content area on screen
  size_t elements = 0;               // how much of the window the screen-reader interface showed
  unsigned long long at_ms = 0;      // when it was read (GetTickCount64)
  bool steady = false;               // nothing moved while it was read: `spans` can be trusted
  unsigned layout = 0;               // State::layout when it was read
};

// Where finds are, by their text. `para` (the paragraph a find came from)
// picks the right one when the same text is on the page twice.
struct Spot {
  std::string id, text, para;
};
struct Spots {
  std::map<std::string, std::vector<Box>> boxes;  // content space
  unsigned long long at_ms = 0;                   // when they were measured
  bool steady = false;                            // nothing moved while they were measured
  unsigned layout = 0;
};

// Something you can click or type into, for tasks.
struct Item {
  int i = 0;
  std::string kind;   // button, link, field, checkbox, tab, menu item, list
  std::string label, value, href, row;  // row: the words of the row or card it sits in
  std::vector<std::string> options;
  bool in_view = false, search = false, secret = false;
  Box r;              // screen pixels
};

// A word under a point, for the spider's feet.
struct WordHit {
  bool ok = false;
  Box word;
  std::string text;
  bool link = false;
  Box link_rect;
  std::string link_text;
};

// What the overlay needs every frame. A tracking thread of its own keeps the
// geometry fresh (~120 times a second), even while a long read runs.
struct State {
  bool attached = false;
  bool visible = false;     // the window is on screen: shown, not minimized, not on another desktop
  HWND hwnd = nullptr;
  Box window, view;         // screen pixels
  double scroll = 0;        // how far the content scrolled since it was read (estimate)
  unsigned jump = 0;        // bumps when the scroll estimate lost track
  unsigned gen = 0;         // bumps when the page or view changed
  std::string url;
  double dpi_scale = 1;
  // Motion. Anything measured before moved_ms may be in the wrong place now:
  // the overlay hides it until it is measured again (as the Wingman overlay
  // hides its labels while the window under it moves or scrolls).
  unsigned long long moved_ms = 0;  // when the window, the view or the content last moved (GetTickCount64)
  unsigned layout = 0;              // bumps when the content was laid out again (resized, zoomed) or tracking was lost
};

class Eyes {
 public:
  Eyes();
  ~Eyes();
  Eyes(const Eyes&) = delete;
  Eyes& operator=(const Eyes&) = delete;

  bool attach(HWND h, int timeout_ms, std::string* error);
  void detach();
  State state();
  Look look();

  // Where text is on screen right now (screen pixels; empty when not found).
  std::vector<Box> find_text(const std::string& text, const std::string& near_block = "");
  // Finds' boxes in one go, in content space, with when they were measured.
  // Boxes that can't be right (a line far taller than the text, a short word
  // as wide as the page) are dropped.
  Spots locate(const std::vector<Spot>& want);
  void word_at(POINT pt, std::function<void(const WordHit&)> cb);
  bool scroll_to_text(const std::string& text);

  // Tasks. items() numbers what can be clicked or typed into; the others act
  // on those numbers. Read only toward secrets: a password or card box is
  // marked secret and never typed into.
  std::vector<Item> items(int max);
  bool show_item(int i);                         // scrolls it into view
  Box item_box(int i);
  bool click_item(int i, std::string* note);
  bool type_item(int i, const std::string& text, bool enter, std::string* note);
  bool scroll(const std::string& dir);           // up, down, top, bottom
  bool press(const std::string& key);            // Escape, Tab, arrows, PageUp/Down, Home, End, Space

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace sp::eyes
