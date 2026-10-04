#include "core/spell.hpp"

#include <spellcheck.h>
#include <wrl/client.h>

#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace sp {

namespace {

ISpellChecker* checker_here() {
  thread_local ComPtr<ISpellChecker> checker;
  thread_local bool tried = false;
  if (!tried) {
    tried = true;
    ComPtr<ISpellCheckerFactory> factory;
    BOOL ok = FALSE;
    if (SUCCEEDED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->IsSupported(L"en-US", &ok)) && ok)
      factory->CreateSpellChecker(L"en-US", &checker);
  }
  return checker.Get();
}

bool known(ISpellChecker* checker, const std::wstring& w) {
  ComPtr<IEnumSpellingError> errors;
  if (FAILED(checker->Check(w.c_str(), &errors)) || !errors) return false;
  ComPtr<ISpellingError> e;
  return errors->Next(&e) != S_OK;
}

}  // namespace

bool repair_ocr_word(std::wstring& word) {
  ISpellChecker* checker = checker_here();
  if (!checker) return false;
  size_t a = 0, b = word.size();
  while (a < b && !std::iswalnum(word[a]) && word[a] != L'/' && word[a] != L'|') ++a;
  while (b > a && !std::iswalnum(word[b - 1])) --b;
  const std::wstring core = word.substr(a, b - a);
  int lower_letters = 0;
  for (wchar_t c : core) lower_letters += std::iswlower(c) ? 1 : 0;
  if (core.size() < 4 || lower_letters < 3 || known(checker, core)) return false;
  const std::wstring head = word.substr(0, a), tail = word.substr(b);
  // A comma read as a "v" or a "7" glued to the word.
  if ((core.back() == L'v' || core.back() == L'7') && tail.empty() && known(checker, core.substr(0, core.size() - 1))) {
    word = head + core.substr(0, core.size() - 1) + L",";
    return true;
  }
  auto swap_all = [](std::wstring s, const std::wstring& from, const std::wstring& to, size_t from_pos) {
    for (size_t p = s.find(from, from_pos); p != std::wstring::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    return s;
  };
  struct Swap {
    const wchar_t* from;
    const wchar_t* to;
    size_t from_pos;  // capitals at the start are real ("Jackson")
  };
  static const Swap swaps[] = {{L"/", L"l", 0}, {L"|", L"l", 0}, {L"J", L"l", 1}, {L"I", L"l", 1}, {L"1", L"l", 0},
                               {L"0", L"o", 0}, {L"rn", L"m", 0}, {L"cl", L"d", 0}, {L"vv", L"w", 0}, {L"li", L"h", 0}};
  for (const Swap& sw : swaps) {
    if (core.find(sw.from, sw.from_pos) == std::wstring::npos) continue;
    const std::wstring fixed = swap_all(core, sw.from, sw.to, sw.from_pos);
    if (fixed != core && known(checker, fixed)) {
      word = head + fixed + tail;
      return true;
    }
  }
  return false;
}

float misspelled_share(const std::wstring& text) {
  ISpellChecker* checker = checker_here();
  if (!checker) return -1.f;

  // Only plain lowercase words count: names, acronyms and numbers are often
  // missing from the dictionary without being misreads.
  int words = 0, bad = 0;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::iswalpha(text[i])) ++i;
    const size_t a = i;
    bool plain = true;
    while (i < text.size() && (std::iswalpha(text[i]) || text[i] == L'\'')) {
      plain &= !std::iswupper(text[i]) || i == a;
      ++i;
    }
    if (i - a < 2 || !plain) continue;
    const std::wstring w = text.substr(a, i - a);
    if (std::iswupper(w[0])) continue;
    ++words;
    bad += known(checker, w) ? 0 : 1;
  }
  return words ? static_cast<float>(bad) / static_cast<float>(words) : 0.f;
}

}  // namespace sp
