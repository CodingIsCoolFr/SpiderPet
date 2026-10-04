#pragma once

#include "see/frame.hpp"

#include <memory>
#include <string>

namespace sp {

// Windows.Media.Ocr behind a plain interface. Call from a thread that has
// joined the multithreaded apartment.
class TextReader {
 public:
  TextReader();
  ~TextReader();
  bool ready() const;
  std::wstring language() const;
  bool read(const Frame& frame, OcrPass& out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

void join_mta();  // winrt::init_apartment(multi_threaded), safe to call twice

}  // namespace sp
