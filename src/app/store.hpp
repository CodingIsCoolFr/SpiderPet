#pragma once

#include "pet/pet.hpp"

#include <string>
#include <vector>

namespace sp {

struct Record {
  uint64_t page = 0;  // which visit to which window
  int entity = 0;
  std::string kind;
  std::string text;
  std::string label;
  std::string summary;
  std::string url;     // where a link points
  std::string source;  // window title
  std::string when;    // local time, ISO 8601
};

// Everything the spider has harvested this session. Each record is also
// appended to %APPDATA%\SpiderPet\harvest.jsonl so nothing is lost on exit.
class Store {
 public:
  void add(const Harvest& h, uint64_t page, const std::wstring& source);
  void summarize(uint64_t page, int entity, const std::string& summary);
  const std::vector<Record>& all() const { return records_; }
  void clear() { records_.clear(); }
  std::wstring save_json() const;  // returns the file written, or empty
  std::wstring save_csv() const;
  std::string as_text() const;

 private:
  std::vector<Record> records_;
};

Settings load_settings();
void save_settings(const Settings& s);

}  // namespace sp
