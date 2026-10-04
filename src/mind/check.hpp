#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace sp {

// Checks what the spider harvested against real sources, with the local
// model as the judge. The model never decides from its own memory: it only
// compares a find with a record or a passage that was looked up, and has to
// quote the passage it relied on.
//
//   DOI    -> Crossref (and the DOI handle server for non-Crossref DOIs)
//   ISBN   -> check digit, OpenLibrary, Google Books
//   PMID   -> PubMed, PMC -> PubMed Central, arXiv id -> arXiv
//   title  -> Crossref bibliographic search
//   claim  -> the model sorts fact / opinion / promotion, Wikipedia finds
//             evidence, the model rules supported / contradicted / not enough
//
// Only the find itself (an id, a title, a few search words) leaves the PC,
// never the page. Answers are cached in %APPDATA%\SpiderPet\checks.json.
class Checker {
 public:
  Checker();
  ~Checker();

  // {ollama: bool, model: "...", models: [...]}
  nlohmann::json status();
  // find: {kind, text, label, href, context}; page: {url, title, lang}
  // -> {status, note, source: {name, url, title}, quote, found: {...}}
  // online = false: nothing leaves the PC; only local checks (ISBN check
  // digit, fact/opinion sorting) are done.
  nlohmann::json check(const nlohmann::json& find, const nlohmann::json& page, bool online = true);
  void set_model(const std::string& name);  // empty = pick the best installed
  // How well each item answers what the user is looking for, 0..10.
  nlohmann::json rank(const std::string& goal, const std::string& title, const nlohmann::json& items);
  // Search terms (synonyms, plurals, related names) for what the user typed.
  nlohmann::json expand(const std::string& goal);
  // One sentence, up to three key points, and what kind of page it is.
  nlohmann::json gist(const std::string& title, const std::string& url, const std::string& text);
  // True for a while after the model got squeezed out of the graphics card
  // (another app took the memory): model work waits, so the PC stays smooth.
  bool gpu_busy() const;

 private:
  nlohmann::json check_doi(const std::string& doi, const std::string& context);
  nlohmann::json check_isbn(const std::string& isbn, const std::string& context);
  nlohmann::json check_pubmed(const std::string& id, bool pmc, const std::string& context);
  nlohmann::json check_arxiv(const std::string& id, const std::string& context);
  nlohmann::json check_title(const std::string& title, const std::string& context);
  nlohmann::json check_claim(const std::string& sentence, const nlohmann::json& page, bool online);
  nlohmann::json compare_record(const std::string& context, const std::string& found_title, nlohmann::json out);
  // Ask the local model for JSON that fits `schema`. Null when it cannot.
  nlohmann::json ask(const std::string& system, const std::string& user, const nlohmann::json& schema,
                     int max_tokens = 300);
  std::string pick_model();
  bool fits(const std::string& model);
  void load_cache();
  void save_cache();

  std::mutex mu_;
  nlohmann::json cache_ = nlohmann::json::object();
  bool cache_dirty_ = false;
  std::string model_;
  std::string wanted_;
  double model_at_ = -100;
  std::atomic<double> slow_until_{0};
  int strikes_ = 0;                          // slow answers in a row
  std::map<std::string, long long> sizes_;  // model -> bytes it takes in graphics memory
};

}  // namespace sp
