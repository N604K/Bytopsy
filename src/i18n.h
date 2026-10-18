#pragma once

// Translations of the windowed app's text (gettext style: the English text is the key).
//
//   Tr(L"Clean up")                                   a fixed string
//   TrF(L"Freed {0} in {1}", size, where)             with values; translators may reorder {0} {1}
//   TrN(n, L"{0} item", L"{0} items")                 a count: {0} is n, with digit grouping
//   N_(L"Hibernation file")                           only marks text that is translated later,
//                                                     where it is shown (tables, journal notes)
//
// Language files are UTF-8 text, "English = translation" per line (see ParseCatalog, and the
// header of res/lang/template.lang). Without a loaded language (bytopsy-cli, the self-tests)
// every function returns the English text.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "util.h"

#define N_(s) s

namespace i18n {

std::wstring Tr(std::wstring_view english);

// Replaces {0}..{9} with args (missing ones stay as they are); "{{" and "}}" are literal braces.
std::wstring Format(std::wstring_view pattern, const std::vector<std::wstring>& args);

namespace detail {
inline std::wstring Arg(const std::wstring& s) { return s; }
inline std::wstring Arg(std::wstring_view s) { return std::wstring(s); }
inline std::wstring Arg(const wchar_t* s) { return s ? s : L""; }
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, wchar_t> && !std::is_same_v<T, bool>, int> = 0>
std::wstring Arg(T v) {
    return std::to_wstring(v);
}
// The translated pattern for n: the language's plural rule picks the form.
std::wstring PluralPattern(uint64_t n, std::wstring_view one, std::wstring_view other);
std::wstring Count(uint64_t n);  // util::FormatCount
}  // namespace detail

template <class... A>
std::wstring TrF(std::wstring_view english, const A&... args) {
    return Format(Tr(english), {detail::Arg(args)...});
}

// {0} is n; further arguments are {1}, {2}, ...
template <class... A>
std::wstring TrN(uint64_t n, std::wstring_view one, std::wstring_view other, const A&... args) {
    return Format(detail::PluralPattern(n, one, other), {detail::Count(n), detail::Arg(args)...});
}

// ---- Plural rules: which form of a TrN entry a count uses. A language file names its rule with
// "@plural = <name>"; otherwise it comes from the language code. Adding a rule is one line in i18n.cpp.
struct PluralRule {
    const wchar_t* name;
    int forms;                 // how many forms a translation gives, separated by "|"
    int (*pick)(uint64_t n);   // 0 .. forms-1
    const wchar_t* example;    // "1 | 2", for translators
};
const PluralRule* FindPluralRule(std::wstring_view name);
const PluralRule& PluralRuleFor(std::wstring_view languageCode);
const std::vector<PluralRule>& PluralRules();

// ---- A parsed language file.
struct TransparentHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view s) const { return std::hash<std::wstring_view>()(s); }
};
struct Catalog {
    std::wstring code;  // "de", "pt-BR"; "qps-ploc" for the pseudo-language
    std::wstring name;  // "Deutsch" (@name)
    const PluralRule* plural = nullptr;
    bool pseudo = false;
    std::unordered_map<std::wstring, std::wstring, TransparentHash, std::equal_to<>> strings;
    // TrN entries, keyed by one + U+001F + other.
    std::unordered_map<std::wstring, std::vector<std::wstring>, TransparentHash, std::equal_to<>> plurals;
};

// The file format, line by line (UTF-8, a BOM is fine):
//   # comment                     (whole lines only)
//   @name = Deutsch               metadata: @name, @code, @plural, @translators
//   English text = Übersetzung    the first "=" that isn't escaped separates the two; spaces
//                                 around both sides are trimmed; an empty translation means
//                                 "not translated yet" (English is shown)
//   {0} item|{0} items = {0} Element|{0} Elemente     plural entry: forms separated by "|"
// Escapes on both sides: \n newline, \t tab, \\ backslash, \= \| \# \@ the character itself,
// \s a space (to keep leading or trailing spaces). A translation that uses a {n} the English
// text doesn't have is rejected. Problems ("line 12: ...") go to `problems`; such lines are skipped.
Catalog ParseCatalog(std::wstring_view text, std::vector<std::wstring>* problems = nullptr);
// Escapes text for the left or right side of a line (the inverse of what ParseCatalog reads).
std::wstring EscapeEntry(std::wstring_view s);

// The pseudo-language: every string bracketed, accented and about 40% longer, made from the
// English text, so untranslated or cut-off text shows in screenshots. "{0}" etc. stay intact.
std::wstring Pseudo(std::wstring_view english);
extern const wchar_t* const kPseudoCode;  // "qps-ploc"

// ---- The language in use (process-wide; switching is safe while other threads translate).
void SetCatalog(std::unique_ptr<Catalog> c);  // null: English
std::wstring CurrentCode();                   // "en" when English
// Text in English on this thread while it lives (journal and log text), including the number
// format (util::SetNumberFormat).
class EnglishScope {
public:
    EnglishScope();
    ~EnglishScope();
    EnglishScope(const EnglishScope&) = delete;
    EnglishScope& operator=(const EnglishScope&) = delete;

private:
    util::InvariantNumbers numbers_;
};

// ---- Where languages come from (bytopsy.exe): built into the exe (res/lang/<code>.lang, as
// resources of type LANG), then "lang\<code>.lang" next to the exe and in util::DataDir()\lang,
// which replace a built-in one of the same code: a translation can be tried without rebuilding.
struct LanguageInfo {
    std::wstring code, name;  // name: @name, or the code
    std::wstring file;        // empty: built in
};
std::vector<LanguageInfo> AvailableLanguages();  // English first, then by name; no pseudo-language
// The Windows display language(s) (GetUserPreferredUILanguages) matched against `available`:
// "de-AT" takes "de-AT", else "de". "en" when nothing matches.
std::wstring WindowsLanguage(const std::vector<LanguageInfo>& available);
std::wstring WindowsLanguageFor(const std::vector<std::wstring>& preferred, const std::vector<LanguageInfo>& available);
// Loads a language by code ("en", "qps-ploc" or "pseudo", or one of AvailableLanguages). An empty
// code means the Windows language. The BYTOPSY_LANG environment variable overrides `code`.
// Returns the code in use; problems in the file go to `problems`.
std::wstring LoadLanguage(const std::wstring& code, std::vector<std::wstring>* problems = nullptr);
// Reads a language file (UTF-8) into text.
bool ReadLanguageFile(const std::wstring& file, std::wstring& text);

}  // namespace i18n

using i18n::Tr;
using i18n::TrF;
using i18n::TrN;
