#pragma once

// The crawler does everything the browser extension used to do, for any
// window, from inside SpiderPet: it reads what the window shows (the eyes),
// harvests the DOIs, ISBNs, ids, titles, links and key sentences, walks the
// spider over them and restyles what it ate (the stage), and carries out a
// task's steps with that window's own buttons and boxes.
//
// It speaks the same messages the extension spoke, so the app's brain (the
// checks, the library, answers and tasks) did not have to change.

#include <nlohmann/json.hpp>

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace sp::crawl {

class Crawler {
 public:
  using Handler = std::function<void(int client, const nlohmann::json& msg)>;
  using Gone = std::function<void(int client)>;

  Crawler();
  ~Crawler();
  bool start(Handler on_message, Gone on_gone);
  void stop();
  // From the app, as it sent them to the extension.
  void send(int client, const nlohmann::json& msg);
  void broadcast(const nlohmann::json& msg);
  int clients() const;  // 1 while the spider is on a window

  void drop_on(HWND h);  // put the spider on this window
  void pick();           // the spider follows the cursor until you click a window
  void lift();           // take it off
  // For the app's window: what the spider is on and how far it got.
  nlohmann::json status();
  // A picture of the window the spider is on, JPEG base64 (for the model's eyes). Empty when none.
  std::string picture(int max_side = 1280);

 private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

}  // namespace sp::crawl
