#pragma once

#include "pet/pet.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sp {

// Talks to a small local model (Ollama on 127.0.0.1:11434) on its own
// thread. The spider never waits for it; answers show up when they show up.
class Llm {
 public:
  struct Answer {
    Ask ask;
    uint64_t page = 0;
    std::string text;
  };

  void start();
  void stop();
  void ask(const Ask& a, uint64_t page, const std::wstring& title);
  std::vector<Answer> take();
  std::string status() const;
  std::string model() const;
  void set_model(const std::string& m);

 private:
  struct Job {
    Ask ask;
    uint64_t page = 0;
    std::wstring title;
  };
  void loop();
  bool ensure_model();

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::vector<Answer> done_;
  std::string status_ = "starting";
  std::string model_;
  std::string wanted_;
  double model_checked_ = -100;
  std::atomic<bool> quit_{false};
  std::thread thread_;
};

}  // namespace sp
