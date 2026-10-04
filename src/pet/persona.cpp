#include "pet/persona.hpp"

#include "core/util.hpp"

#include <iterator>

namespace sp {
namespace {

std::wstring shorten(const std::wstring& s, size_t n) {
  if (s.size() <= n) return s;
  return s.substr(0, n - 1) + L"…";
}

std::wstring fill(const wchar_t* pattern, const std::wstring& x) {
  std::wstring out = pattern;
  const size_t at = out.find(L"{}");
  if (at != std::wstring::npos) out.replace(at, 2, x);
  return out;
}

}  // namespace

const wchar_t* Persona::pick(const wchar_t* const* options, int n) {
  const wchar_t* p = options[rng_.below(n)];
  for (int tries = 0; p == last_ && n > 1 && tries < 4; ++tries) p = options[rng_.below(n)];
  last_ = p;
  return p;
}

#define PICK(...)                                     \
  [&] {                                               \
    static const wchar_t* const opts[] = {__VA_ARGS__}; \
    return pick(opts, static_cast<int>(std::size(opts))); \
  }()

std::wstring Persona::line(Moment m, const Entity* e, const std::wstring& extra, int count) {
  switch (m) {
    case Moment::Land: return PICK(L"new habitat.", L"touching down.", L"host acquired.", L"tasting the surface…");
    case Moment::Loose: return PICK(L"no host. drop me on a page.", L"no web here.", L"waiting for a host.");
    case Moment::Sense: return PICK(L"sensing…", L"probing.", L"scanning substrate.", L"signals…");
    case Moment::Absorbed: return PICK(L"absorbed.", L"digested.", L"stored.", L"assimilated.", L"mine now.");
    case Moment::Read: return PICK(L"parsing…", L"ingesting text…", L"reading…");
    case Moment::Learned:
      return fill(PICK(L"absorbing: {}", L"digesting: {}", L"learned: {}", L"tastes like: {}"), shorten(extra, 46));
    case Moment::Gist: return L"habitat: " + shorten(extra, 40);
    case Moment::Crawl: return PICK(L"crawling deeper.", L"more substrate below.", L"descending the page.");
    case Moment::Rest: return PICK(L"resting.", L"cleaning legs.", L"listening…", L"digesting…", L"zzz");
    case Moment::Lifted: return PICK(L"!! displaced", L"lifted. hostile?", L"put me down.");
    case Moment::Landed: return PICK(L"relocated.", L"recalibrating…", L"new coordinates.");
    case Moment::Barren: return PICK(L"barren surface.", L"nothing to eat here.");
    case Moment::Drained: return PICK(L"habitat drained.", L"nothing left in reach.");
    case Moment::Meals: return std::to_wstring(count) + L" meals stored.";
    case Moment::More: return PICK(L"more below. scroll me.", L"page goes on. scroll?", L"hungry. more below.");
    case Moment::Here:
      return (e ? fill(PICK(L"here: {}", L"found it: {}", L"right here: {}"), shorten(e->text, 30)) : L"here.");
    case Moment::Lock:
      break;
  }
  if (!e) return L"sensing\u2026";
  // Name the actual thing, so the bubble says what it is doing.
  const std::wstring t = shorten(e->text, 30);
  switch (e->kind) {
    case Kind::Doi: return fill(PICK(L"doi: {}", L"signal doi: {}"), t);
    case Kind::Isbn: return fill(PICK(L"isbn: {}", L"book spore: {}"), t);
    case Kind::Id: return lower(e->label.empty() ? L"id" : e->label) + L": " + t;
    case Kind::Title: return fill(PICK(L"tasting: {}", L"title: {}", L"ooh: {}"), t);
    case Kind::Heading: return fill(PICK(L"new region: {}", L"territory: {}"), lower(t));
    case Kind::Link: {
      std::wstring host = e->href;
      const size_t s = host.find(L"://");
      if (s != std::wstring::npos) host = host.substr(s + 3);
      host = host.substr(0, host.find(L'/'));
      if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
      const std::wstring what = fill(PICK(L"tasting: {}", L"eating: {}", L"sampling: {}"), shorten(e->text, 24));
      return host.empty() ? what : what + L" \u2192 " + host;
    }
    case Kind::Url: return fill(L"address: {}", t);
    case Kind::Email: return fill(L"contact: {}", t);
    case Kind::Date: return fill(L"timestamp: {}", t);
    case Kind::Number: return fill(L"quantity: {}", t);
    case Kind::Handle: return fill(L"organism: {}", t);
    case Kind::Sentence: return L"reading: " + shorten(e->text, 26);
    case Kind::Item: return fill(PICK(L"tasting: {}", L"logging: {}", L"noted: {}"), t);
  }
  return L"sensing\u2026";
}

#undef PICK

}  // namespace sp
