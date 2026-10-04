#include "app/store.hpp"

#include "core/util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace sp {
namespace {

std::string local_time(const char* fmt) {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[64];
  std::strftime(buf, sizeof(buf), fmt, &tm);
  return buf;
}

nlohmann::json to_json(const Record& r) {
  nlohmann::json j = {{"kind", r.kind}, {"text", r.text}, {"source", r.source}, {"when", r.when}};
  if (!r.label.empty()) j["label"] = r.label;
  if (!r.summary.empty()) j["summary"] = r.summary;
  if (!r.url.empty()) j["url"] = r.url;
  return j;
}

std::string csv_cell(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

}  // namespace

void Store::add(const Harvest& h, uint64_t page, const std::wstring& source) {
  const std::string kind = kind_name(h.kind), text = utf8(h.text);
  for (const Record& old : records_)
    if (old.page == page && old.kind == kind && old.text == text) return;
  Record r;
  r.page = page;
  r.entity = h.entity;
  r.kind = kind_name(h.kind);
  r.text = utf8(h.text);
  r.label = utf8(h.label);
  r.summary = h.summary;
  r.url = utf8(h.href);
  r.source = utf8(source);
  r.when = local_time("%Y-%m-%dT%H:%M:%S");
  records_.push_back(r);
  std::ofstream log(app_data_dir() + L"\\harvest.jsonl", std::ios::app | std::ios::binary);
  if (log) log << to_json(r).dump() << "\n";
}

void Store::summarize(uint64_t page, int entity, const std::string& summary) {
  for (auto it = records_.rbegin(); it != records_.rend(); ++it)
    if (it->page == page && it->entity == entity) {
      it->summary = summary;
      return;
    }
}

std::wstring Store::save_json() const {
  const std::wstring path = documents_dir() + L"\\harvest-" + wide(local_time("%Y%m%d-%H%M%S")) + L".json";
  nlohmann::json all = nlohmann::json::array();
  for (const Record& r : records_) all.push_back(to_json(r));
  std::ofstream f(path, std::ios::binary);
  if (!f) return {};
  f << all.dump(2);
  return path;
}

std::wstring Store::save_csv() const {
  const std::wstring path = documents_dir() + L"\\harvest-" + wide(local_time("%Y%m%d-%H%M%S")) + L".csv";
  std::ofstream f(path, std::ios::binary);
  if (!f) return {};
  f << "\xEF\xBB\xBF" << "kind,label,text,url,summary,source,when\r\n";
  for (const Record& r : records_)
    f << csv_cell(r.kind) << ',' << csv_cell(r.label) << ',' << csv_cell(r.text) << ',' << csv_cell(r.url) << ','
      << csv_cell(r.summary) << ',' << csv_cell(r.source) << ',' << csv_cell(r.when) << "\r\n";
  return path;
}

std::string Store::as_text() const {
  std::ostringstream o;
  for (const Record& r : records_) {
    o << r.kind << "\t" << (r.label.empty() ? "" : r.label + " ") << r.text;
    if (!r.url.empty()) o << "\t" << r.url;
    if (!r.summary.empty()) o << "\t(" << r.summary << ")";
    o << "\n";
  }
  return o.str();
}

Settings load_settings() {
  Settings s;
  std::ifstream f(app_data_dir() + L"\\settings.json", std::ios::binary);
  if (!f) return s;
  try {
    const auto j = nlohmann::json::parse(f);
    s.crawl = j.value("crawl", s.crawl);
    s.read = j.value("read", s.read);
    s.web = j.value("web", s.web);
    s.brain = j.value("brain", s.brain);
    s.share = j.value("share", s.share);
    s.size = std::clamp(j.value("size", s.size), 0.5f, 2.f);
  } catch (...) {
  }
  return s;
}

void save_settings(const Settings& s) {
  const nlohmann::json j = {{"crawl", s.crawl}, {"read", s.read}, {"web", s.web}, {"brain", s.brain}, {"share", s.share},
                               {"size", s.size}};
  std::ofstream f(app_data_dir() + L"\\settings.json", std::ios::binary);
  if (f) f << j.dump(2);
}

}  // namespace sp
