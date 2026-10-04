#pragma once

#include "core/geom.hpp"
#include "see/frame.hpp"
#include "see/glyphs.hpp"

#include <functional>
#include <string>
#include <vector>

namespace sp {

// What a piece of text is. Each kind gets its own look when the spider
// harvests it, the way the reference clip restyles DOIs, ISBNs and titles.
enum class Kind : uint8_t { Sentence, Heading, Title, Doi, Isbn, Id, Link, Url, Email, Date, Number, Handle, Item };
const char* kind_name(Kind k);
float kind_priority(Kind k);

enum class Mark : uint8_t { New, Locked, Done, Skip };

struct Entity {
  int id = 0;
  Kind kind = Kind::Sentence;
  Mark mark = Mark::New;
  std::wstring text;
  std::wstring label;          // "PMID", "ISBN" and so on for ids
  std::wstring href;           // where a link goes, when the page told us
  std::vector<Rect> lines;     // one box per text line, content coordinates
  std::vector<Glyphs> glyphs;  // the real letters of each line, when they could be lifted
  Rect box;
  float font_px = 16;
  uint32_t fg = 0xE0E0E0;
  uint32_t bg = 0x101010;
  float score = 0;
  bool key = false;
  int variant = 0;
  float tilt = 0;
  Vec2 nudge{};
  double applied_at = 0;
  float read = 0;              // sentence sweep, 0..1
  int misses = 0;
  int fails = 0;
  std::string summary;         // the local model's short version, UTF-8
};

// What the page itself says about a spot, read through accessibility (the
// structure browsers publish for screen readers): exact text, where links go,
// which lines are headings. Content coordinates.
struct Fact {
  enum Type : uint8_t { Text, Link, Heading, Button };
  Type type = Text;
  std::wstring text;
  std::wstring url;
  Rect box;
  int level = 0;
};

struct TextLine {
  std::wstring text;
  Rect box;
  std::vector<OcrWord> words;  // content coordinates
  uint32_t fg = 0;
  uint32_t bg = 0;
};

// Everything the spider knows about the watched area, in content coordinates
// (x from the left edge of the area, y down the page including scroll).
class Page {
 public:
  void clear();
  void set_title(const std::wstring& title);

  // Folds one reader pass in. `visible` says whether a content-space box is
  // actually on screen (no other window over it). Returns the drift that was
  // corrected; the caller moves its own anchors by the same amount.
  float merge(const OcrPass& pass, const std::function<bool(const Rect&)>& visible);
  void shift(float dy);
  // Replace what accessibility says about the band [y0, y1] and re-derive the
  // finds there, so links get their real text and targets.
  void merge_facts(std::vector<Fact> facts, float y0, float y1);
  bool has_facts() const { return !facts_.empty(); }

  std::vector<Entity>& entities() { return entities_; }
  const std::vector<Entity>& entities() const { return entities_; }
  Entity* find(int id);
  const std::vector<TextLine>& lines() const { return lines_; }

  bool snap(Vec2 p, float radius, Vec2& out) const;
  float line_height() const { return line_h_; }
  float pitch() const { return pitch_; }  // baseline-to-baseline distance in paragraphs
  bool dark() const { return dark_; }
  uint32_t paper() const { return paper_; }
  float column_x() const { return column_x_; }
  std::wstring sample(size_t max_chars) const;
  int harvested() const;
  bool empty() const { return lines_.empty(); }
  float bottom() const { return lines_.empty() ? 0.f : lines_.back().box.bottom(); }

 private:
  struct Block {
    std::vector<int> lines;  // indices into lines_
    Rect box;
    bool prose = false;
  };
  void rebuild_stats();
  void build_blocks();
  void extract(float y0, float y1, std::vector<Entity>& out) const;
  void extract_line(const TextLine& line, bool prose, std::vector<Entity>& out) const;
  void extract_items(float y0, float y1, std::vector<Entity>& out) const;
  std::wstring facts_text(const Rect& area) const;
  void extract_sentences(const Block& b, std::vector<Entity>& out) const;
  void reconcile(std::vector<Entity>& found, float y0, float y1, const std::function<bool(const Rect&)>& visible);
  void lift_glyphs(const Frame& f, float scroll, float y0, float y1, const std::function<bool(const Rect&)>& visible);
  bool is_link_color(uint32_t fg) const;

  std::vector<TextLine> lines_;
  std::vector<Block> blocks_;
  std::vector<int> line_block_;  // block index of each line
  std::vector<Fact> facts_;
  std::vector<Entity> entities_;
  std::vector<std::wstring> title_words_;
  std::vector<std::wstring> topic_words_;
  int next_id_ = 1;
  float line_h_ = 16;
  float word_h_ = 12;
  float pitch_ = 22;
  bool dark_ = true;
  uint32_t paper_ = 0x101010;
  uint32_t ink_ = 0xE0E0E0;
  float column_x_ = 0;
};

}  // namespace sp
