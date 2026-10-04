#pragma once

#include <string>

namespace sp {

// Share of the real words in `text` that the Windows spell checker does not
// know (0..1), or -1 when no checker is available. Used to throw out OCR
// misreads like "smarter than vou thir". Call from a COM-initialized thread.
float misspelled_share(const std::wstring& text);

// Fixes one OCR word with the usual misreads ("predatorsv" -> "predators,",
// "rnale" -> "male", "sma/l" -> "small"), but only when the fix is a
// dictionary word and the original is not, so names are left alone.
// Returns true when it changed the word.
bool repair_ocr_word(std::wstring& word);

}  // namespace sp
