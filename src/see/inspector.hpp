#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sp {

// UI Automation on its own thread: finds the page area of a window (the
// Document control in a browser), its address, and scrolls it gently. Only
// reads accessibility data a screen reader would; never another process's memory.
class Inspector {
 public:
  struct Result {
    uint64_t token = 0;
    bool found = false;   // a document area was found
    RECT doc{};           // screen pixels
    std::wstring url;
    bool can_scroll = false;
  };

  // One thing the page publishes for screen readers, in screen pixels.
  struct Node {
    enum Type : uint8_t { Text, Link, Heading, Button };
    Type type = Text;
    std::wstring text;
    std::wstring url;
    RECT rect{};
    int level = 0;
  };
  struct Scan {
    uint64_t token = 0;
    std::vector<Node> nodes;
    double took = 0;
  };

  void start();
  void stop();
  // Read the page's accessibility tree (what is on screen) on the worker.
  void scan(HWND root, uint64_t token);
  bool take_scan(Scan& out);
  void inspect(HWND root, POINT hint, uint64_t token);
  bool take(Result& out);
  // positive = down; method 0 = accessibility scroll, 1 = wheel message
  void scroll(HWND root, POINT at, int notches, int method);

 private:
  void loop();
  struct Job {
    int kind = 0;  // 0 inspect, 1 scroll, 2 read the page
    HWND root = nullptr;
    POINT at{};
    uint64_t token = 0;
    int notches = 0;
    int method = 0;
  };
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<Result> results_;
  std::deque<Scan> scans_;
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

}  // namespace sp
