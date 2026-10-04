#include "app/library.hpp"

#include "core/util.hpp"

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace sp {

using json = nlohmann::json;

namespace {

json read_json(const std::wstring& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return nullptr;
  std::stringstream ss;
  ss << f.rdbuf();
  json j = json::parse(ss.str(), nullptr, false);
  return j.is_discarded() ? json(nullptr) : j;
}

bool write_json(const std::wstring& path, const json& j, int indent = -1) {
  // Write next to it first, so a crash never leaves half a library.
  const std::wstring tmp = path + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(indent, ' ', false, json::error_handler_t::replace);
  }
  return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string s(const json& j, const char* k) {
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string csv(const std::string& v) {
  std::string out = "\"";
  for (char c : v) out += c == '"' ? std::string("\"\"") : std::string(1, c == '\n' ? ' ' : c);
  return out + "\"";
}

}  // namespace

json Settings::to_json() const {
  return {{"goal", goal},          {"online", online},       {"auto_check", auto_check},
          {"check_sentences", check_sentences}, {"crawl", crawl}, {"model", model}};
}

void Settings::from_json(const json& j) {
  if (!j.is_object()) return;
  goal = j.value("goal", goal);
  online = j.value("online", online);
  auto_check = j.value("auto_check", auto_check);
  check_sentences = j.value("check_sentences", check_sentences);
  crawl = j.value("crawl", crawl);
  model = j.value("model", model);
}

void Settings::load() { from_json(read_json(app_data_dir() + L"\\settings.json")); }

void Settings::save() const { write_json(app_data_dir() + L"\\settings.json", to_json(), 2); }

void Library::load() {
  json j = read_json(documents_dir() + L"\\library.json");
  if (j.is_object() && j.contains("pages") && j.contains("finds")) root_ = std::move(j);
  rebuild_order();
}

void Library::save() {
  if (!dirty_) return;
  if (write_json(documents_dir() + L"\\library.json", root_)) dirty_ = false;
}

void Library::rebuild_order() {
  page_order_.clear();
  for (auto& [url, p] : root_["pages"].items()) page_order_.push_back(url);
  std::sort(page_order_.begin(), page_order_.end(), [this](const std::string& a, const std::string& b) {
    return root_["pages"][a].value("at", 0LL) > root_["pages"][b].value("at", 0LL);
  });
}

void Library::touch_page(const std::string& url, const std::string& title, const std::string& lang) {
  json& p = root_["pages"][url];
  if (!p.is_object()) p = {{"finds", json::array()}};
  if (!title.empty()) p["title"] = title;
  if (!lang.empty()) p["lang"] = lang;
  p["at"] = static_cast<int64_t>(std::time(nullptr));
  dirty_ = true;
  rebuild_order();
}

std::vector<std::string> Library::add(const std::string& url, const std::string& title, const std::string& lang,
                                      const json& finds) {
  touch_page(url, title, lang);
  json& p = root_["pages"][url];
  std::vector<std::string> fresh;
  if (!finds.is_array()) return fresh;
  for (const json& f : finds) {
    const std::string id = s(f, "id");
    if (id.empty()) continue;
    json& slot = root_["finds"][id];
    if (slot.is_object()) continue;  // already harvested on an earlier visit
    slot = {{"id", id},
            {"kind", s(f, "kind")},
            {"text", s(f, "text")},
            {"label", s(f, "label")},
            {"href", s(f, "href")},
            {"context", s(f, "context")},
            {"url", url},
            {"title", title},
            {"at", static_cast<int64_t>(std::time(nullptr))},
            {"verdict", nullptr},
            {"score", -1}};
    p["finds"].push_back(id);
    fresh.push_back(id);
  }
  dirty_ = true;
  return fresh;
}

json* Library::find(const std::string& id) {
  auto it = root_["finds"].find(id);
  return it == root_["finds"].end() ? nullptr : &*it;
}

json* Library::page(const std::string& url) {
  auto it = root_["pages"].find(url);
  return it == root_["pages"].end() ? nullptr : &*it;
}

void Library::set_verdict(const std::string& id, const json& v) {
  if (json* f = find(id)) {
    (*f)["verdict"] = v;
    dirty_ = true;
  }
}

void Library::set_score(const std::string& id, int score) {
  if (json* f = find(id)) {
    (*f)["score"] = score;
    dirty_ = true;
  }
}

void Library::set_gist(const std::string& url, const json& gist) {
  if (json* p = page(url)) {
    (*p)["gist"] = gist;
    dirty_ = true;
  }
}

std::vector<std::string> Library::finds_of(const std::string& url) const {
  std::vector<std::string> out;
  auto it = root_["pages"].find(url);
  if (it == root_["pages"].end() || !it->contains("finds")) return out;
  for (const json& id : (*it)["finds"])
    if (id.is_string()) out.push_back(id.get<std::string>());
  return out;
}

void Library::remove_page(const std::string& url) {
  for (const std::string& id : finds_of(url)) root_["finds"].erase(id);
  root_["pages"].erase(url);
  dirty_ = true;
  rebuild_order();
}

void Library::clear() {
  root_ = {{"pages", json::object()}, {"finds", json::object()}};
  dirty_ = true;
  rebuild_order();
}

bool Library::export_json(const std::wstring& path) const { return write_json(path, root_, 2); }

bool Library::export_csv(const std::wstring& path) const {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << "\xEF\xBB\xBF" << "kind,label,text,status,note,source,score,page_title,page_url\n";
  for (const std::string& url : page_order_)
    for (const std::string& id : finds_of(url)) {
      auto it = root_["finds"].find(id);
      if (it == root_["finds"].end()) continue;
      const json& x = *it;
      const json v = x.value("verdict", json());
      const std::string src = v.is_object() && v.contains("source") ? s(v["source"], "url") : "";
      f << csv(s(x, "kind")) << ',' << csv(s(x, "label")) << ',' << csv(s(x, "text")) << ','
        << csv(v.is_object() ? s(v, "status") : "") << ',' << csv(v.is_object() ? s(v, "note") : "") << ','
        << csv(src) << ',' << x.value("score", -1) << ',' << csv(s(x, "title")) << ',' << csv(url) << '\n';
    }
  return true;
}

bool checkable(const json& find, const Settings& st) {
  const std::string k = s(find, "kind");
  const std::string l = s(find, "label");
  if (k == "doi" || k == "isbn" || k == "title") return true;
  if (k == "id") return l == "PMID" || l == "PMC" || l == "arXiv" || l == "pmid" || l == "pmc" || l == "arxiv";
  if (k == "sentence") return st.check_sentences;
  return false;
}

}  // namespace sp
