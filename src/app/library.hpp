#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace sp {

struct Settings {
  std::string goal;            // what the user is looking for; steers relevance
  bool online = true;          // look finds up in Crossref, PubMed, Wikipedia...
  bool auto_check = true;      // check every find as it is harvested
  bool check_sentences = true; // also check claims (slower: two model calls each)
  bool crawl = true;           // work through the whole page, out of sight too (it never scrolls)
  bool popups = true;          // tuck away popups, cookie banners and sign-up walls that block the page
  bool ask_every_step = false; // tasks: wait for "Do it" on every step, not only the risky ones
  bool think_tasks = true;     // tasks: the AI thinks before each step (smarter, slower)
  bool show_in_shares = true;  // screen shares and recordings show the spider
  std::string model;           // empty = best installed

  nlohmann::json to_json() const;
  void from_json(const nlohmann::json& j);
  void load();
  void save() const;
};

// Everything the spider has harvested, in every browser, with the checks.
// Not thread-safe: the app guards it with its own lock.
//
// A find: {id, kind, text, label, href, context, url, title, at,
//          verdict: {status, note, source{name,url,title}, quote, found} | null,
//          score: 0..10 | -1}
class Library {
 public:
  void load();
  void save();
  bool dirty() const { return dirty_; }

  // Adds what is new and returns the ids that were not known before.
  std::vector<std::string> add(const std::string& url, const std::string& title, const std::string& lang,
                               const nlohmann::json& finds);
  void touch_page(const std::string& url, const std::string& title, const std::string& lang);
  nlohmann::json* find(const std::string& id);
  nlohmann::json* page(const std::string& url);
  void set_verdict(const std::string& id, const nlohmann::json& v);
  void set_score(const std::string& id, int score);
  void set_gist(const std::string& url, const nlohmann::json& gist);
  void remove_page(const std::string& url);
  void clear();

  const nlohmann::json& pages() const { return root_["pages"]; }
  const nlohmann::json& finds() const { return root_["finds"]; }
  const std::vector<std::string>& page_order() const { return page_order_; }  // newest first
  std::vector<std::string> finds_of(const std::string& url) const;

  bool export_json(const std::wstring& path) const;
  bool export_csv(const std::wstring& path) const;

 private:
  void rebuild_order();
  nlohmann::json root_ = {{"pages", nlohmann::json::object()}, {"finds", nlohmann::json::object()}};
  std::vector<std::string> page_order_;
  bool dirty_ = false;
};

// Kinds the checker knows how to verify.
bool checkable(const nlohmann::json& find, const Settings& s);

}  // namespace sp
