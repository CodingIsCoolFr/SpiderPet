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
  // Answers a question about a page from the page's text only:
  // {answer, quote} (the quote is checked to really be on the page). Null without the model.
  nlohmann::json answer(const std::string& question, const std::string& title, const std::string& text);
  // Things floating over a page ({key, tag, role, classes, cover, text, buttons}):
  // which are popups to tuck away. -> {key: {action: hide|keep, kind, why}}. Null without the model.
  nlohmann::json blockers(const nlohmann::json& items);
  // Which of the page's buttons, links and boxes ({i, kind, label, href, inView})
  // the user means by `want` ("Click the first video"). -> {index, why}; index -1
  // when none fits. Null without the model. The user still confirms before anything happens.
  nlohmann::json pick(const std::string& want, const nlohmann::json& items);
  // The next step toward a goal, from a look at the page ({url, title, text,
  // scroll, items: [{i, kind, label, href, value, inView, search}]}) and what was
  // done so far. -> {action: click|type|scroll|goto|back|done|ask, index, text,
  // enter, url, dir, say}. Null without the model.
  // think: the model reasons before each step (smarter, about 3x slower).
  nlohmann::json next_action(const std::string& goal, const nlohmann::json& intent, const std::string& memory,
                             const nlohmann::json& history, const nlohmann::json& snap, bool think = false);
  // What a goal really asks for, before the first step: {intent, done_when,
  // query, media}. Null without the model.
  nlohmann::json understand(const std::string& goal, bool think = false);
  // One sentence, up to three key points, and what kind of page it is.
  nlohmann::json gist(const std::string& title, const std::string& url, const std::string& text);
  // True for a while after the model got squeezed out of the graphics card
  // (another app took the memory): model work waits, so the PC stays smooth.
  bool gpu_busy() const;
  // Drops every remembered check answer (checks.json), so finds are looked up afresh.
  void forget();

 private:
  nlohmann::json check_doi(const std::string& doi, const std::string& context);
  nlohmann::json check_isbn(const std::string& isbn, const std::string& context);
  nlohmann::json check_pubmed(const std::string& id, bool pmc, const std::string& context);
  nlohmann::json check_arxiv(const std::string& id, const std::string& context);
  nlohmann::json check_title(const std::string& title, const std::string& context);
  nlohmann::json check_claim(const std::string& sentence, const nlohmann::json& page, bool online);
  nlohmann::json compare_record(const std::string& context, const std::string& found_title, nlohmann::json out);
  // Ask the local model for JSON that fits `schema`. Null when it cannot.
  // think: let a thinking model (Qwen3) reason before it answers: smarter, slower.
  nlohmann::json ask(const std::string& system, const std::string& user, const nlohmann::json& schema,
                     int max_tokens = 300, bool think = false);
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
