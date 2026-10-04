#include "mind/check.hpp"

#include "core/util.hpp"
#include "mind/net.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

namespace sp {

using json = nlohmann::json;

namespace {

// Smartest first. Qwen 3.8 27B judges best; the smaller ones are fallbacks.
const char* kModels[] = {"qwen3.8-27b-uncensored-64k", "qwen3:14b", "qwen3:8b", "qwen2.5:7b", "llama3.1:8b", "qwen3:4b", "qwen2.5:3b"};

std::string lower_ascii(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim_ascii(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Significant words: lowercase, letters and digits, no little words.
std::vector<std::string> words_of(const std::string& s) {
  static const std::set<std::string> stop = {
      "the", "and", "for", "with", "from", "that", "this", "are", "was", "were", "has", "have", "had", "its", "into",
      "their", "than", "then", "also", "but", "not", "can", "may", "been", "being", "which", "who", "whom", "what",
      "when", "where", "how", "all", "any", "some", "such", "these", "those", "there", "here", "over", "under", "about",
      "between", "after", "before", "during", "while", "will", "would", "should", "could", "a", "an", "of", "in", "on",
      "to", "by", "as", "at", "or", "is", "it", "be", "de", "la", "et", "al"};
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (cur.size() >= 3 && !stop.count(cur)) out.push_back(cur);
    cur.clear();
  };
  for (unsigned char c : s) {
    if (std::isalnum(c) || c >= 0x80) cur += static_cast<char>(c >= 0x80 ? c : std::tolower(c));
    else flush();
  }
  flush();
  return out;
}

// Share of `record`'s words that also appear in `context`, 0..1.
float overlap(const std::string& record, const std::string& context) {
  const auto rw = words_of(record);
  if (rw.empty()) return 0.f;
  const auto cw = words_of(context);
  const std::set<std::string> cs(cw.begin(), cw.end());
  int hit = 0;
  std::set<std::string> seen;
  for (const auto& w : rw)
    if (seen.insert(w).second && cs.count(w)) ++hit;
  return static_cast<float>(hit) / static_cast<float>(seen.size());
}

std::string squash(const std::string& s) {
  std::string out;
  bool space = false;
  for (unsigned char c : s) {
    if (std::isspace(c)) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += static_cast<char>(std::tolower(c));
  }
  return out;
}

std::string str(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string first_str(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end()) return {};
  if (it->is_string()) return it->get<std::string>();
  if (it->is_array() && !it->empty() && (*it)[0].is_string()) return (*it)[0].get<std::string>();
  return {};
}

json verdict(const char* status, const std::string& note) { return {{"status", status}, {"note", note}}; }

json source(const std::string& name, const std::string& url, const std::string& title = {}) {
  return {{"name", name}, {"url", url}, {"title", title}};
}

std::string digits_only(const std::string& s, bool keep_x = false) {
  std::string out;
  for (char c : s)
    if (std::isdigit(static_cast<unsigned char>(c)) || (keep_x && (c == 'X' || c == 'x'))) out += static_cast<char>(std::toupper(c));
  return out;
}

bool isbn_ok(const std::string& d) {
  if (d.size() == 10) {
    int sum = 0;
    for (int i = 0; i < 10; ++i) {
      const int v = d[i] == 'X' ? (i == 9 ? 10 : -1000) : d[i] - '0';
      sum += v * (10 - i);
    }
    return sum >= 0 && sum % 11 == 0;
  }
  if (d.size() == 13) {
    int sum = 0;
    for (int i = 0; i < 13; ++i) {
      if (d[i] == 'X') return false;
      sum += (d[i] - '0') * (i % 2 ? 3 : 1);
    }
    return sum % 10 == 0;
  }
  return false;
}

std::string xml_tag(const std::string& xml, const std::string& tag, size_t from = 0) {
  const size_t a = xml.find("<" + tag, from);
  if (a == std::string::npos) return {};
  const size_t open_end = xml.find('>', a);
  const size_t b = xml.find("</" + tag + ">", open_end);
  if (open_end == std::string::npos || b == std::string::npos) return {};
  return trim_ascii(xml.substr(open_end + 1, b - open_end - 1));
}

std::string year_of(const json& issued) {
  try {
    const auto& parts = issued.at("date-parts").at(0);
    if (!parts.empty() && parts[0].is_number()) return std::to_string(parts[0].get<int>());
  } catch (...) {
  }
  return {};
}

}  // namespace

Checker::Checker() { load_cache(); }

Checker::~Checker() { save_cache(); }

void Checker::load_cache() {
  std::ifstream f(app_data_dir() + L"\\checks.json", std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  json j = json::parse(ss.str(), nullptr, false);
  if (j.is_object()) cache_ = std::move(j);
}

void Checker::save_cache() {
  std::lock_guard lock(mu_);
  if (!cache_dirty_) return;
  // Keep it small: the newest answers win.
  while (cache_.size() > 3000) cache_.erase(cache_.begin());
  std::ofstream f(app_data_dir() + L"\\checks.json", std::ios::binary | std::ios::trunc);
  f << cache_.dump(-1, ' ', false, json::error_handler_t::replace);
  cache_dirty_ = false;
}

std::string Checker::pick_model() {
  std::string wanted;
  {
    std::lock_guard lock(mu_);
    wanted = wanted_;
  }
  const double now = now_seconds();
  if (!model_.empty() && now - model_at_ < 30) return model_;
  model_at_ = now;
  std::string body;
  if (local_http("GET", "/api/tags", nullptr, body, 3000) != 200) {
    model_.clear();
    return model_;
  }
  json tags = json::parse(body, nullptr, false);
  std::vector<std::string> have;
  if (tags.is_object() && tags.contains("models"))
    for (const auto& m : tags["models"]) {
      std::string n = str(m, "name");
      // "name:latest" and "name" are the same model.
      if (n.size() > 7 && n.compare(n.size() - 7, 7, ":latest") == 0) n.resize(n.size() - 7);
      have.push_back(n);
      if (!sizes_.count(n)) sizes_[n] = static_cast<long long>(m.value("size", 0.0) * 0.94);  // the file, less what text checks never load
    }
  model_.clear();
  if (!wanted.empty() && std::find(have.begin(), have.end(), wanted) != have.end()) return model_ = wanted;
  for (const char* want : kModels)
    if (std::find(have.begin(), have.end(), want) != have.end()) {
      model_ = want;
      break;
    }
  if (model_.empty() && !have.empty()) model_ = have.front();
  return model_;
}

void Checker::set_model(const std::string& name) {
  std::lock_guard lock(mu_);
  wanted_ = name;
  model_at_ = -100;
}

json Checker::status() {
  json out = {{"ollama", false}, {"model", ""}};
  std::string body;
  if (local_http("GET", "/api/tags", nullptr, body, 2000) == 200) {
    out["ollama"] = true;
    out["model"] = pick_model();
  }
  return out;
}

bool Checker::gpu_busy() const { return now_seconds() < slow_until_.load(); }

namespace {

// Free graphics memory in bytes, from the NVIDIA driver; -1 without one.
long long free_vram() {
  struct Mem {
    unsigned long long total, free, used;
  };
  using Init = int (*)();
  using Handle = int (*)(unsigned, void**);
  using Info = int (*)(void*, Mem*);
  static const HMODULE dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dll) return -1;
  static const auto init = reinterpret_cast<Init>(GetProcAddress(dll, "nvmlInit_v2"));
  static const auto handle = reinterpret_cast<Handle>(GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2"));
  static const auto info = reinterpret_cast<Info>(GetProcAddress(dll, "nvmlDeviceGetMemoryInfo"));
  static const bool ok = init && handle && info && init() == 0;
  void* dev = nullptr;
  Mem m{};
  if (!ok || handle(0, &dev) != 0 || info(dev, &m) != 0) return -1;
  return static_cast<long long>(m.free);
}

}  // namespace

// Whether the model can sit in graphics memory next to everything open now.
// One that does not fit spills into main memory, writes a word every few
// seconds and keeps the card at full load, so it is better not loaded at all.
bool Checker::fits(const std::string& model) {
  std::string body;
  if (local_http("GET", "/api/ps", nullptr, body, 2000) == 200) {
    const json ps = json::parse(body, nullptr, false);
    if (ps.is_object() && ps.contains("models") && ps["models"].is_array())
      for (const json& m : ps["models"]) {
        std::string n = str(m, "name");
        if (n.size() > 7 && n.compare(n.size() - 7, 7, ":latest") == 0) n.resize(n.size() - 7);
        if (n != model) continue;
        sizes_[model] = static_cast<long long>(m.value("size", 0.0));  // measured: weights and working memory
        return true;                                                    // already loaded: the speed check watches it
      }
  }
  const long long free = free_vram();
  const auto it = sizes_.find(model);
  if (free < 0 || it == sizes_.end() || it->second <= 0) return true;  // no way to tell: try it
  return free >= it->second + 800ll * 1024 * 1024;                      // and room for the rest of the PC to breathe
}

json Checker::ask(const std::string& system, const std::string& user, const json& schema, int max_tokens) {
  if (gpu_busy()) return nullptr;
  const std::string model = pick_model();
  if (model.empty()) return nullptr;
  if (!fits(model)) {
    slow_until_ = now_seconds() + 30;  // look again in half a minute: you may have closed something
    return nullptr;
  }
  json req = {{"model", model},
              {"stream", false},
              {"format", schema},
              {"keep_alive", "2m"},  // the graphics card memory comes back soon after the spider stops
              {"messages", json::array({{{"role", "system"}, {"content", system}}, {{"role", "user"}, {"content", user}}})},
              {"options", {{"temperature", 0}, {"num_ctx", 8192}, {"num_predict", max_tokens}}}};
  // Qwen3 thinks out loud by default; a verdict does not need it.
  if (model.rfind("qwen3", 0) == 0) req["think"] = false;
  const std::string body = req.dump(-1, ' ', false, json::error_handler_t::replace);
  std::string out;
  const double t0 = now_seconds();
  const long code = local_http("POST", "/api/chat", &body, out, 60000);
  // The model shares the graphics card with your browser. A short rest after
  // each answer keeps scrolling and video smooth while the spider works.
  const double took = now_seconds() - t0;
  json res = json::parse(out, nullptr, false);
  // How fast it wrote. When another app (a game, VRChat) has taken graphics
  // memory, the model no longer fits: it writes a word every few seconds and
  // holds the card at full load, and everything on screen stutters. Then it
  // leaves the card and rests for a minute and a half.
  double load = 0, tps = -1;
  int written = 0;
  if (res.is_object()) {
    load = res.value("load_duration", 0.0) / 1e9;
    written = res.value("eval_count", 0);
    const double gen = res.value("eval_duration", 0.0) / 1e9;
    if (gen > 0) tps = written / gen;
  }
  const bool timed_out = code != 200 && took > 55;
  if (timed_out || took - load > 40 || (written >= 8 && tps >= 0 && tps < 4)) {
    // Each time in a row it rests longer: 1.5, 3, 6, then 12 minutes.
    strikes_ = std::min(strikes_ + 1, 4);
    slow_until_ = now_seconds() + 90.0 * (1 << (strikes_ - 1));
    const std::string unload = json{{"model", model}, {"keep_alive", 0}}.dump();
    std::string ignored;
    local_http("POST", "/api/generate", &unload, ignored, 10000);
  } else {
    if (code == 200) strikes_ = 0;
    Sleep(static_cast<DWORD>(std::min(1500.0, took * 400.0)));
  }
  if (code != 200) return nullptr;
  if (!res.is_object()) return nullptr;
  std::string content;
  try {
    content = res.at("message").at("content").get<std::string>();
  } catch (...) {
    return nullptr;
  }
  const size_t think = content.find("</think>");
  if (think != std::string::npos) content = content.substr(think + 8);
  json parsed = json::parse(content, nullptr, false);
  return parsed.is_discarded() ? json(nullptr) : parsed;
}

// The record was found. Does the text around the find on the page describe
// the same work? The title decides; year, journal and authors back it up.
// The model only rules on the unclear cases, and only from the two texts.
json Checker::compare_record(const std::string& context, const std::string& found_title, json out) {
  const float ov = overlap(found_title, context);
  const auto ctx_words = words_of(context);
  if (found_title.empty() || ctx_words.size() < 5) {
    out["status"] = "verified";
    out["note"] = "exists: " + found_title;
    return out;
  }
  if (ov >= 0.6f) {
    out["status"] = "verified";
    out["note"] = "matches: " + found_title;
    return out;
  }
  // Libraries and pages often disagree on the subtitle ("American Anthropology,
  // 1946-1970" vs "American Anthropology: Papers from..."). A main title of two
  // or more words, all on the page, is the same work.
  {
    const size_t cut = found_title.find_first_of(":,;(");
    const std::string main_title = found_title.substr(0, cut);
    if (words_of(main_title).size() >= 2 && overlap(main_title, context) >= 0.99f) {
      out["status"] = "verified";
      out["note"] = "matches: " + found_title;
      return out;
    }
  }
  const json found = out.value("found", json::object());
  int support = 0;
  std::string hints;
  const std::string year = str(found, "year").substr(0, 4);
  if (year.size() == 4 && context.find(year) != std::string::npos) ++support;
  if (!str(found, "journal").empty() && overlap(str(found, "journal"), context) >= 0.6f) ++support;
  for (const auto& a : words_of(str(found, "authors")))
    if (lower_ascii(context).find(a) != std::string::npos) {
      ++support;
      break;
    }
  if (!year.empty()) hints += "Year: " + year + "\n";
  if (!str(found, "journal").empty()) hints += "Published in: " + str(found, "journal") + "\n";
  if (!str(found, "authors").empty()) hints += "Authors on record (often incomplete for book chapters): " + str(found, "authors") + "\n";
  if (ov >= 0.3f && support >= 1) {
    out["status"] = "verified";
    out["note"] = "matches: " + found_title;
    return out;
  }
  const json schema = {{"type", "object"},
                       {"properties", {{"same", {{"type", "boolean"}}}, {"reason", {{"type", "string"}}}}},
                       {"required", {"same", "reason"}}};
  const json j = ask(
      "You compare a citation from a web page with a library record. Decide only from these texts. The title is "
      "the main evidence. Small differences in wording, case, punctuation or a subtitle still mean the same work. "
      "Author lists in library records are often wrong or incomplete, so different authors alone are not enough to "
      "say it is a different work.",
      "Citation text on the page:\n" + context.substr(0, 700) + "\n\nLibrary record:\nTitle: " + found_title + "\n" + hints +
          "\nIs the citation about this same work?",
      schema, 120);
  if (j.is_object() && j.contains("same") && j["same"].is_boolean()) {
    const bool same = j["same"].get<bool>();
    out["status"] = same ? "verified" : "mismatch";
    out["note"] = same ? "matches: " + found_title : "the id belongs to a different work: " + found_title;
    if (j.contains("reason") && j["reason"].is_string()) out["reason"] = j["reason"];
    return out;
  }
  // No model: the words decide alone, carefully.
  out["status"] = ov >= 0.3f ? "verified" : "mismatch";
  out["note"] = ov >= 0.3f ? "matches: " + found_title : "the id may belong to a different work: " + found_title;
  return out;
}

json Checker::check_doi(const std::string& raw, const std::string& context) {
  std::string doi = trim_ascii(raw);
  while (!doi.empty() && std::strchr(".,;:)]", doi.back())) doi.pop_back();
  const std::string link = "https://doi.org/" + doi;
  std::string body;
  const long code = web_get("https://api.crossref.org/works/" + url_encode(doi, "/"), body);
  if (code == 200) {
    json j = json::parse(body, nullptr, false);
    if (j.is_object() && j.contains("message")) {
      const json& m = j["message"];
      const std::string title = first_str(m, "title");
      std::string authors;
      if (m.contains("author") && m["author"].is_array())
        for (size_t i = 0; i < m["author"].size() && i < 3; ++i) {
          if (!authors.empty()) authors += ", ";
          authors += str(m["author"][i], "family");
        }
      json out = {{"source", source("Crossref", link, title)},
                  {"found", {{"title", title}, {"authors", authors}, {"year", year_of(m.value("issued", json::object()))},
                             {"journal", first_str(m, "container-title")}, {"doi", doi}}}};
      return compare_record(context, title, std::move(out));
    }
  }
  if (code == 404) {
    // Not every DOI is a Crossref DOI (DataCite, mEDRA...). Ask the DOI system itself.
    std::string h;
    const long hc = web_get("https://doi.org/api/handles/" + url_encode(doi, "/"), h);
    json hj = json::parse(h, nullptr, false);
    if (hc == 200 && hj.is_object() && hj.value("responseCode", 0) == 1) {
      json out = verdict("verified", "exists (registered outside Crossref)");
      out["source"] = source("doi.org", link);
      return out;
    }
    if (hc == 404 || (hj.is_object() && hj.value("responseCode", 0) == 100)) {
      json out = verdict("not_found", "this DOI does not exist");
      out["source"] = source("doi.org", link);
      return out;
    }
  }
  return verdict("error", code == 0 ? "offline: could not reach Crossref" : "Crossref answered " + std::to_string(code));
}

json Checker::check_isbn(const std::string& raw, const std::string& context) {
  const std::string isbn = digits_only(raw, true);
  if (!isbn_ok(isbn)) return verdict("not_found", "the check digit is wrong: this ISBN is mistyped");
  std::string body;
  long code = web_get("https://openlibrary.org/isbn/" + isbn + ".json", body);
  if (code == 200) {
    json j = json::parse(body, nullptr, false);
    const std::string title = j.is_object() ? str(j, "title") : "";
    if (!title.empty()) {
      json out = {{"source", source("OpenLibrary", "https://openlibrary.org/isbn/" + isbn, title)},
                  {"found", {{"title", title}, {"year", str(j, "publish_date")}, {"isbn", isbn}}}};
      return compare_record(context, title, std::move(out));
    }
  }
  body.clear();
  code = web_get("https://www.googleapis.com/books/v1/volumes?q=isbn:" + isbn, body);
  if (code == 200) {
    json j = json::parse(body, nullptr, false);
    if (j.is_object() && j.value("totalItems", 0) > 0 && j.contains("items")) {
      const json& v = j["items"][0].value("volumeInfo", json::object());
      const std::string title = str(v, "title");
      json out = {{"source", source("Google Books", "https://books.google.com/books?vid=ISBN" + isbn, title)},
                  {"found", {{"title", title}, {"year", str(v, "publishedDate")}, {"isbn", isbn}}}};
      return compare_record(context, title, std::move(out));
    }
    return verdict("unverified", "valid ISBN, but no library knows it");
  }
  return verdict("error", "offline: could not reach the book libraries");
}

json Checker::check_pubmed(const std::string& raw, bool pmc, const std::string& context) {
  const std::string id = digits_only(raw);
  if (id.empty()) return verdict("not_found", "no number in this id");
  std::string body;
  const long code = web_get(std::string("https://eutils.ncbi.nlm.nih.gov/entrez/eutils/esummary.fcgi?db=") +
                                (pmc ? "pmc" : "pubmed") + "&retmode=json&id=" + id,
                            body);
  if (code != 200) return verdict("error", "offline: could not reach PubMed");
  json j = json::parse(body, nullptr, false);
  try {
    const json& r = j.at("result").at(id);
    if (r.contains("error")) return verdict("not_found", "PubMed has no record with this id");
    const std::string title = str(r, "title");
    const std::string url = pmc ? "https://pmc.ncbi.nlm.nih.gov/articles/PMC" + id + "/" : "https://pubmed.ncbi.nlm.nih.gov/" + id + "/";
    json out = {{"source", source(pmc ? "PubMed Central" : "PubMed", url, title)},
                {"found", {{"title", title}, {"year", str(r, "pubdate")}, {"journal", str(r, "source")}}}};
    return compare_record(context, title, std::move(out));
  } catch (...) {
    return verdict("not_found", "PubMed has no record with this id");
  }
}

json Checker::check_arxiv(const std::string& raw, const std::string& context) {
  std::string id = trim_ascii(raw);
  if (lower_ascii(id).rfind("arxiv:", 0) == 0) id = id.substr(6);
  std::string body;
  const long code = web_get("https://export.arxiv.org/api/query?id_list=" + url_encode(id, "/."), body);
  if (code != 200) return verdict("error", "offline: could not reach arXiv");
  const size_t entry = body.find("<entry>");
  const std::string title = entry == std::string::npos ? "" : xml_tag(body, "title", entry);
  if (title.empty() || title == "Error") return verdict("not_found", "arXiv has no paper with this id");
  json out = {{"source", source("arXiv", "https://arxiv.org/abs/" + id, title)}, {"found", {{"title", title}}}};
  return compare_record(context, title, std::move(out));
}

json Checker::check_title(const std::string& title, const std::string& context) {
  if (words_of(title).size() < 3) return verdict("skipped", "too short to look up");
  std::string body;
  const long code =
      web_get("https://api.crossref.org/works?rows=5&select=DOI,title,issued,author&query.bibliographic=" + url_encode(title), body);
  if (code != 200) return verdict("error", "offline: could not reach Crossref");
  json j = json::parse(body, nullptr, false);
  try {
    // A preprint and the paper often share a title: the one whose DOI the
    // citation itself names wins.
    json items = j.at("message").at("items");
    const std::string ctx = lower_ascii(context);
    std::stable_sort(items.begin(), items.end(), [&](const json& a, const json& b) {
      const bool ia = !str(a, "DOI").empty() && ctx.find(lower_ascii(str(a, "DOI"))) != std::string::npos;
      const bool ib = !str(b, "DOI").empty() && ctx.find(lower_ascii(str(b, "DOI"))) != std::string::npos;
      return ia && !ib;
    });
    for (const json& it : items) {
      const std::string t = first_str(it, "title");
      // Both ways: the record's words in the title and the title's words in the record.
      if (overlap(t, title) >= 0.8f && overlap(title, t) >= 0.8f) {
        const std::string doi = str(it, "DOI");
        json out = verdict("verified", "published work, DOI " + doi);
        out["source"] = source("Crossref", "https://doi.org/" + doi, t);
        out["found"] = {{"title", t}, {"doi", doi}, {"year", year_of(it.value("issued", json::object()))}};
        return out;
      }
    }
  } catch (...) {
  }
  return verdict("unverified", "no published work with this title in Crossref");
}

json Checker::check_claim(const std::string& sentence, const json& page, bool online) {
  // Wikipedia cannot be checked against itself; its sources (DOIs, ISBNs,
  // titles) are checked instead. No badge, no model time.
  if (lower_ascii(str(page, "url")).find("wikipedia.org/wiki/") != std::string::npos)
    return verdict("skipped", "Wikipedia itself: its cited sources are checked instead");

  // 1. Is it something that can be true or false at all?
  const json triage_schema = {
      {"type", "object"},
      {"properties",
       {{"type", {{"type", "string"}, {"enum", {"fact", "opinion", "promotion", "instruction", "other"}}}},
        {"checkable", {{"type", "boolean"}}},
        {"subject", {{"type", "string"}}},
        {"query", {{"type", "string"}}}}},
      {"required", {"type", "checkable", "subject", "query"}}};
  const json tri = ask(
      "You sort one sentence from a web page. type: fact (a claim about the world that could be true or false), "
      "opinion (a view or judgement), promotion (advertising or selling), instruction (telling the reader to do "
      "something), other. checkable: true only for a specific factual claim. subject: the title of the one "
      "encyclopedia article that would cover this claim (for example \"Spider\" or \"Eiffel Tower\"). query: 2 to 6 "
      "search words for the specific fact.",
      "Page: " + str(page, "title") + "\nSentence: " + sentence.substr(0, 600), triage_schema, 80);
  if (!tri.is_object()) return verdict("error", "the local model is not running");
  const std::string type = str(tri, "type");
  if (type == "opinion") return verdict("opinion", "an opinion, not a fact to check");
  if (type == "promotion") return verdict("promo", "advertising, not information");
  if (type == "instruction") return verdict("skipped", "an instruction, nothing to check");
  if (!tri.value("checkable", false)) return verdict("skipped", "not a specific claim");
  if (!online) return verdict("unverified", "a fact; online lookups are off, so it was not checked");

  // The page itself cannot be its own evidence.
  const std::string lang = [&] {
    std::string l = lower_ascii(str(page, "lang")).substr(0, 2);
    return (l.size() == 2 && std::isalpha(static_cast<unsigned char>(l[0])) && std::isalpha(static_cast<unsigned char>(l[1]))) ? l : std::string("en");
  }();
  const std::string host = lang + ".wikipedia.org";
  const bool self = lower_ascii(str(page, "url")).find("wikipedia.org/") != std::string::npos;

  // 2. Evidence: the best Wikipedia articles for the subject.
  std::string query = str(tri, "query");
  if (words_of(query).empty()) return verdict("skipped", "no subject to look up");
  std::string body;
  if (web_get("https://" + host + "/w/api.php?action=query&list=search&format=json&utf8=1&srlimit=3&srsearch=" + url_encode(query),
              body) != 200)
    return verdict("error", "offline: could not reach Wikipedia");
  json sj = json::parse(body, nullptr, false);
  // The article the model named comes first, then what the search found.
  std::vector<std::string> titles;
  const std::string subject = trim_ascii(str(tri, "subject"));
  if (!subject.empty()) titles.push_back(subject);
  try {
    for (const json& r : sj.at("query").at("search")) {
      const std::string t = str(r, "title");
      if (lower_ascii(t) != lower_ascii(subject)) titles.push_back(t);
    }
  } catch (...) {
  }
  const std::string page_title = lower_ascii(str(page, "title"));
  struct Passage {
    std::string text, article;
    float score;
  };
  std::vector<Passage> passages;
  const auto claim_words = words_of(sentence);
  const std::set<std::string> claim_set(claim_words.begin(), claim_words.end());
  int fetched = 0;
  for (const std::string& t : titles) {
    if (fetched >= 3) break;
    // On a Wikipedia page, its own article is not independent evidence.
    if (self && page_title.find(lower_ascii(t)) != std::string::npos) continue;
    std::string eb;
    if (web_get("https://" + host + "/w/api.php?action=query&prop=extracts&explaintext=1&format=json&utf8=1&redirects=1&titles=" +
                    url_encode(t),
                eb) != 200)
      continue;
    ++fetched;
    json ej = json::parse(eb, nullptr, false);
    std::string extract;
    try {
      for (const auto& [id, p] : ej.at("query").at("pages").items()) extract = str(p, "extract");
    } catch (...) {
    }
    std::stringstream ss(extract);
    std::string para;
    while (std::getline(ss, para)) {
      if (para.size() < 60) continue;
      const auto pw = words_of(para);
      std::set<std::string> hit;
      for (const auto& w : pw)
        if (claim_set.count(w)) hit.insert(w);
      float s = 0;
      for (const auto& w : hit) s += std::isdigit(static_cast<unsigned char>(w[0])) ? 2.f : 1.f;
      if (s >= 2) passages.push_back({para.substr(0, 900), t, s});
    }
  }
  if (passages.empty()) {
    json out = verdict("unverified", self ? "this page is the encyclopedia; no other source found" : "no evidence found");
    return out;
  }
  std::sort(passages.begin(), passages.end(), [](const Passage& a, const Passage& b) { return a.score > b.score; });
  if (passages.size() > 3) passages.resize(3);
  std::string evidence;
  for (const Passage& p : passages) evidence += "[" + p.article + "] " + p.text + "\n\n";

  // 3. The ruling, from the evidence only, with the sentence it relied on.
  const json judge_schema = {
      {"type", "object"},
      {"properties",
       {{"verdict", {{"type", "string"}, {"enum", {"supported", "contradicted", "not_enough_info"}}}},
        {"quote", {{"type", "string"}}},
        {"reason", {{"type", "string"}}}}},
      {"required", {"verdict", "quote", "reason"}}};
  const json rule = ask(
      "You check one claim against evidence. Use ONLY the evidence text. Do not use your own knowledge. "
      "supported: the evidence clearly says the same thing. contradicted: the evidence clearly says something "
      "incompatible (a different number, date, name or fact). not_enough_info: anything else. quote: copy the one "
      "sentence from the evidence that decides it, word for word. reason: at most 20 words.",
      "Claim: " + sentence.substr(0, 600) + "\n\nEvidence:\n" + evidence, judge_schema, 220);
  if (!rule.is_object()) return verdict("error", "the local model is not running");
  std::string v = str(rule, "verdict");
  const std::string quote = str(rule, "quote");
  // A verdict needs its quote to really be in the evidence; a model that
  // made up its quote does not get to decide.
  if (v != "not_enough_info" && (quote.size() < 12 || squash(evidence).find(squash(quote)) == std::string::npos))
    v = "not_enough_info";
  std::string article = passages.front().article;
  for (const Passage& p : passages)
    if (!quote.empty() && squash(p.text).find(squash(quote)) != std::string::npos) article = p.article;
  json out;
  if (v == "supported") out = verdict("verified", str(rule, "reason"));
  else if (v == "contradicted") out = verdict("mismatch", str(rule, "reason"));
  else out = verdict("unverified", "the sources found do not settle it");
  std::string slug = article;
  std::replace(slug.begin(), slug.end(), ' ', '_');
  out["source"] = source("Wikipedia", "https://" + host + "/wiki/" + url_encode(slug, "_()"), article);
  if (v != "not_enough_info") out["quote"] = quote;
  return out;
}

json Checker::check(const json& find, const json& page, bool online) {
  const std::string kind = str(find, "kind");
  const std::string text = trim_ascii(str(find, "text"));
  const std::string label = lower_ascii(str(find, "label"));
  const std::string context = str(find, "context");
  const std::string key = kind + "|" + label + "|" + text + "|" + std::to_string(std::hash<std::string>{}(context));
  {
    std::lock_guard lock(mu_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
      json hit = *it;
      hit["cached"] = true;
      return hit;
    }
  }
  json out;
  if (!online && kind != "sentence" && kind != "fact") {
    if (kind == "isbn")
      out = isbn_ok(digits_only(text, true)) ? verdict("unverified", "the check digit is right; online lookups are off")
                                             : verdict("not_found", "the check digit is wrong: this ISBN is mistyped");
    else
      out = verdict("skipped", "online lookups are off");
    return out;  // not cached: it is worth a real check later
  }
  if (kind == "doi") out = check_doi(text, context);
  else if (kind == "isbn") out = check_isbn(text, context);
  else if (kind == "id" && (label == "pmid" || label == "pubmed")) out = check_pubmed(text, false, context);
  else if (kind == "id" && label == "pmc") out = check_pubmed(text, true, context);
  else if (kind == "id" && label == "arxiv") out = check_arxiv(text, context);
  else if (kind == "title") out = check_title(text, context);
  else if (kind == "sentence" || kind == "fact") out = check_claim(text, page, online);
  else out = verdict("skipped", "nothing to check for this kind");
  out["checked_at"] = static_cast<int64_t>(std::time(nullptr));
  // Errors are worth trying again later; answers are kept.
  if (str(out, "status") != "error" && online) {
    std::lock_guard lock(mu_);
    cache_[key] = out;
    cache_dirty_ = true;
  }
  return out;
}

json Checker::rank(const std::string& goal, const std::string& title, const json& items) {
  if (!items.is_array() || items.empty()) return {{"scores", json::object()}};
  std::string list;
  for (const json& it : items) {
    std::string t = str(it, "text");
    if (t.size() > 240) t = t.substr(0, 240) + "...";
    list += str(it, "id") + ": [" + str(it, "kind") + "] " + t + "\n";
  }
  const json schema = {
      {"type", "object"},
      {"properties",
       {{"scores",
         {{"type", "array"},
          {"items",
           {{"type", "object"},
            {"properties", {{"id", {{"type", "string"}}}, {"score", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10}}}}},
            {"required", {"id", "score"}}}}}}}},
      {"required", {"scores"}}};
  const std::string want = goal.empty() ? "understand the main topic of the page \"" + title + "\"" : goal;
  const json j = ask(
      "You rate how useful each item from a web page is for what the reader wants. 10: exactly what they want. "
      "5: related background. 0: unrelated, navigation, or advertising. Rate every id.",
      "The reader wants to: " + want + "\nPage: " + title + "\n\nItems:\n" + list, schema, 60 + 14 * static_cast<int>(items.size()));
  json scores = json::object();
  if (j.is_object() && j.contains("scores") && j["scores"].is_array())
    for (const json& s : j["scores"])
      if (s.contains("id") && s.contains("score") && s["score"].is_number()) scores[str(s, "id")] = s["score"];
  return {{"scores", scores}};
}

json Checker::expand(const std::string& goal) {
  const json schema = {{"type", "object"},
                       {"properties", {{"terms", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 14}}}}},
                       {"required", {"terms"}}};
  const json j = ask(
      "You help a reader search a web page. Give up to 14 short lowercase search terms (one to three words each) "
      "that text matching their request would contain: the key words themselves, their singular and plural, "
      "synonyms, and closely related names. Most important first.",
      "The reader is looking for: " + goal, schema, 160);
  json terms = json::array();
  if (j.is_object() && j.contains("terms") && j["terms"].is_array())
    for (const json& t : j["terms"])
      if (t.is_string() && t.get<std::string>().size() >= 2 && t.get<std::string>().size() <= 40) terms.push_back(lower_ascii(t.get<std::string>()));
  return terms;
}

json Checker::gist(const std::string& title, const std::string& url, const std::string& text) {
  const json schema = {
      {"type", "object"},
      {"properties",
       {{"summary", {{"type", "string"}}},
        {"points", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 3}}},
        {"type", {{"type", "string"}, {"enum", {"reference", "research", "news", "opinion", "tutorial", "forum", "social", "video", "shop", "other"}}}}}},
      {"required", {"summary", "points", "type"}}};
  const json j = ask(
      "You summarize a web page for a busy reader. summary: one plain sentence, at most 25 words. points: up to 3 "
      "key facts from the page, each at most 18 words, only things the page actually says. type: what kind of page "
      "it is.",
      "Title: " + title + "\nAddress: " + url + "\n\n" + text.substr(0, 7000), schema, 260);
  if (!j.is_object()) return {{"error", "the local model is not running"}};
  return j;
}

}  // namespace sp
