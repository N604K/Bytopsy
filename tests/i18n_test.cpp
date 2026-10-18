// Self-check of translations: bytopsy-i18n-test.exe
// Language file parsing (escapes, comments, malformed lines, placeholder checks), {n} substitution
// and reordering, plural rules, the fallback to English, the pseudo-language, regional number
// formats, and that res/lang/template.lang and every shipped translation parse cleanly.
#include <windows.h>

#include <cstdio>
#include <set>
#include <string>

#include "i18n.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static bool HasProblem(const std::vector<std::wstring>& p, const std::wstring& part) {
    for (auto& s : p)
        if (s.find(part) != std::wstring::npos) return true;
    return false;
}

static void Use(const std::wstring& text, const wchar_t* code = L"xx") {
    auto c = std::make_unique<i18n::Catalog>(i18n::ParseCatalog(text));
    c->code = code;
    i18n::SetCatalog(std::move(c));
}

int main() {
    using namespace i18n;

    // ---- Substitution
    Check(Format(L"Freed {0} in {1}", {L"1 GB", L"3 categories"}) == L"Freed 1 GB in 3 categories", L"Format: {0} {1}");
    Check(Format(L"{1} then {0}", {L"a", L"b"}) == L"b then a", L"Format: reordered");
    Check(Format(L"{0} and {2}", {L"a"}) == L"a and {2}", L"Format: a missing argument stays visible");
    Check(Format(L"{{0}} is {0}", {L"x"}) == L"{0} is x", L"Format: {{ and }} are braces");
    Check(Format(L"{x} {", {L"a"}) == L"{x} {", L"Format: other braces are left alone");
    Check(TrF(L"{0} of {1}", 3, std::wstring(L"five")) == L"3 of five", L"TrF without a language: English with values");

    // ---- English (no language loaded)
    SetCatalog(nullptr);
    Check(Tr(L"Clean up") == L"Clean up", L"no language: Tr is the English text");
    Check(TrN(1, L"{0} item", L"{0} items") == L"1 item" && TrN(0, L"{0} item", L"{0} items") == L"0 items" &&
              TrN(1234, L"{0} item", L"{0} items") == L"1,234 items",
          L"no language: English plural, digit grouping");
    Check(CurrentCode() == L"en", L"no language: code en");

    // ---- Parsing
    std::vector<std::wstring> problems;
    std::wstring file =
        L"\xFEFF# A comment\r\n"
        L"@name = Testisch\r\n"
        L"@plural = east-slavic\r\n"
        L"\r\n"
        L"   # indented comment\n"
        L"Clean up = Aufräumen\n"
        L"Freed {0} in {1} = In {1}: {0} frei\n"
        L"Two\\nlines = Zwei\\nZeilen\n"
        L"a \\= b = a ist b\n"
        L"turned off \\= Manual start = aus \\= manuell\n"
        L"Pipe \\| here = Rohr \\| hier\n"
        L"\\sLeading and trailing\\s = \\sVorne und hinten\\s\n"
        L"Back\\\\slash = Rück\\\\strich\n"
        L"Unknown \\q escape = Unbekannt \\q\n"
        L"Not translated yet =\n"
        L"no separator on this line\n"
        L" = nothing before\n"
        L"Bad {0} = Schlecht {0} {3}\n"
        L"Dropped {0} = Ohne Zahl\n"
        L"Clean up = Aufräumen 2\n"
        L"@colour = blue\n"
        L"{0} file|{0} files = {0} Datei|{0} Dateien|{0} Dateien!\n"
        L"{0} game|{0} games = {0} Spiel|{0} Spiele {1}\n"
        L"{0} cat|{0} cats|{0} more = x\n"
        L"\\#hash = Raute\n"
        L"\\@at = Klammeraffe";  // no newline at the end
    Catalog c = ParseCatalog(file, &problems);
    Check(c.name == L"Testisch", L"parse: @name (after a BOM and a comment)");
    Check(c.plural && std::wstring(c.plural->name) == L"east-slavic", L"parse: @plural");
    Check(c.strings.count(L"Clean up") && c.strings[L"Clean up"] == L"Aufräumen 2", L"parse: a duplicate replaces the first");
    Check(HasProblem(problems, L"line 20: translated twice"), L"parse: the duplicate is reported with its line");
    Check(c.strings[L"Freed {0} in {1}"] == L"In {1}: {0} frei", L"parse: placeholders kept");
    Check(c.strings[L"Two\nlines"] == L"Zwei\nZeilen", L"parse: \\n is a newline (CRLF and LF lines)");
    Check(c.strings[L"a = b"] == L"a ist b", L"parse: \\= in the English text");
    Check(c.strings[L"turned off = Manual start"] == L"aus = manuell", L"parse: \\= on both sides");
    Check(c.strings[L"Pipe | here"] == L"Rohr | hier", L"parse: \\| outside plurals");
    Check(c.strings[L" Leading and trailing "] == L" Vorne und hinten ", L"parse: \\s keeps spaces at the ends");
    Check(c.strings[L"Back\\slash"] == L"Rück\\strich", L"parse: \\\\ is a backslash");
    Check(c.strings[L"Unknown \\q escape"] == L"Unbekannt \\q", L"parse: an unknown escape stays as written");
    Check(!c.strings.count(L"Not translated yet"), L"parse: an empty translation is no translation");
    Check(HasProblem(problems, L"line 16: no \"=\""), L"parse: a line without = is reported");
    Check(HasProblem(problems, L"line 17: nothing before"), L"parse: a line with nothing before = is reported");
    Check(!c.strings.count(L"Bad {0}") && HasProblem(problems, L"line 18: {3} is not in the English text"),
          L"parse: a translation with a placeholder the English lacks is rejected");
    Check(c.strings[L"Dropped {0}"] == L"Ohne Zahl", L"parse: leaving a placeholder out is allowed");
    Check(HasProblem(problems, L"line 21: unknown setting @colour"), L"parse: unknown settings are reported");
    std::wstring fileKey = std::wstring(L"{0} file") + L'\x1F' + L"{0} files";
    Check(c.plurals.count(fileKey) && c.plurals[fileKey].size() == 3 && c.plurals[fileKey][2] == L"{0} Dateien!", L"parse: plural forms");
    Check(HasProblem(problems, L"line 23: {1} is not in the English text"), L"parse: plural forms are checked too");
    Check(HasProblem(problems, L"line 24: more than one \"|\""), L"parse: three English forms are reported");
    Check(c.strings[L"#hash"] == L"Raute" && c.strings[L"@at"] == L"Klammeraffe", L"parse: \\# and \\@ start an entry; last line without newline");
    Check(problems.size() == 7, L"parse: 7 problems (" + std::to_wstring(problems.size()) + L")");
    std::vector<std::wstring> p2;
    ParseCatalog(L"@plural = martian\n", &p2);
    Check(HasProblem(p2, L"unknown plural rule"), L"parse: an unknown plural rule is reported");

    // EscapeEntry is what ParseCatalog reads back.
    const wchar_t* tricky[] = {L" a = b | c \\ d \n e\t#", L"#start", L"@start", L"x", L"{0}% \u00B7 \u201Cquoted\u201D"};
    std::wstring round;
    for (auto t : tricky) round += EscapeEntry(t) + L" = " + EscapeEntry(std::wstring(L"T:") + t) + L"\n";
    Catalog rc = ParseCatalog(round);
    bool rt = rc.strings.size() == 5;
    for (auto t : tricky) rt = rt && rc.strings[t] == std::wstring(L"T:") + t;
    Check(rt, L"EscapeEntry round trip");

    // ---- A loaded language
    Use(file);
    Check(Tr(L"Clean up") == L"Aufräumen 2", L"loaded: Tr translates");
    Check(Tr(L"Never heard of") == L"Never heard of", L"loaded: missing entries fall back to English");
    Check(Tr(L"Not translated yet") == L"Not translated yet", L"loaded: empty translations fall back to English");
    Check(TrF(L"Freed {0} in {1}", L"2 GB", L"Temp") == L"In Temp: 2 GB frei", L"loaded: TrF reorders values");
    Check(TrF(L"Dropped {0}", 5) == L"Ohne Zahl", L"loaded: TrF with a dropped placeholder");
    Check(TrN(1, L"{0} file", L"{0} files") == L"1 Datei" && TrN(2, L"{0} file", L"{0} files") == L"2 Dateien" &&
              TrN(5, L"{0} file", L"{0} files") == L"5 Dateien!" && TrN(21, L"{0} file", L"{0} files") == L"21 Datei" &&
              TrN(11, L"{0} file", L"{0} files") == L"11 Dateien!" && TrN(22, L"{0} file", L"{0} files") == L"22 Dateien",
          L"loaded: plural forms follow the east-slavic rule (1, 2, 5, 21, 11, 22)");
    Check(TrN(3, L"{0} dog", L"{0} dogs") == L"3 dogs", L"loaded: an untranslated plural falls back to English");
    Check(CurrentCode() == L"xx", L"loaded: code");
    {
        EnglishScope en;
        Check(Tr(L"Clean up") == L"Clean up" && TrN(2, L"{0} file", L"{0} files") == L"2 files", L"EnglishScope: English on this thread");
    }
    Check(Tr(L"Clean up") == L"Aufräumen 2", L"EnglishScope: translations again afterwards");
    // Fewer forms than the rule has: the last one is used.
    Use(L"@plural = east-slavic\n{0} file|{0} files = {0} Datei|{0} Dateien\n");
    Check(TrN(5, L"{0} file", L"{0} files") == L"5 Dateien", L"loaded: missing plural forms use the last one");
    // No @plural: the rule comes from the code.
    Use(L"{0} file|{0} files = {0} fichier|{0} fichiers\n", L"fr");
    Check(TrN(0, L"{0} file", L"{0} files") == L"0 fichier" && TrN(2, L"{0} file", L"{0} files") == L"2 fichiers",
          L"loaded: French counts 0 as singular (rule from the code)");
    Use(L"{0} file|{0} files = {0} ファイル\n", L"ja");
    Check(TrN(7, L"{0} file", L"{0} files") == L"7 ファイル", L"loaded: Japanese has one form");

    // ---- Plural rules
    auto pick = [](const wchar_t* rule, uint64_t n) { return FindPluralRule(rule)->pick(n); };
    Check(pick(L"one", 1) == 0 && pick(L"one", 0) == 1 && pick(L"one", 2) == 1, L"rule one");
    Check(pick(L"polish", 1) == 0 && pick(L"polish", 3) == 1 && pick(L"polish", 5) == 2 && pick(L"polish", 22) == 1 &&
              pick(L"polish", 12) == 2 && pick(L"polish", 21) == 2,
          L"rule polish");
    Check(pick(L"czech", 1) == 0 && pick(L"czech", 4) == 1 && pick(L"czech", 5) == 2 && pick(L"czech", 22) == 2, L"rule czech");
    Check(pick(L"arabic", 0) == 0 && pick(L"arabic", 2) == 2 && pick(L"arabic", 7) == 3 && pick(L"arabic", 50) == 4 &&
              pick(L"arabic", 100) == 5,
          L"rule arabic");
    Check(pick(L"slovenian", 101) == 0 && pick(L"slovenian", 2) == 1 && pick(L"slovenian", 4) == 2 && pick(L"slovenian", 5) == 3,
          L"rule slovenian");
    bool formsOk = true;
    for (auto& r : PluralRules())
        for (uint64_t n = 0; n < 300; ++n) formsOk = formsOk && r.pick(n) >= 0 && r.pick(n) < r.forms;
    Check(formsOk, L"every rule picks a form it has");
    Check(std::wstring(PluralRuleFor(L"pt-BR").name) == L"zero-one" && std::wstring(PluralRuleFor(L"pt-PT").name) == L"one" &&
              std::wstring(PluralRuleFor(L"ru").name) == L"east-slavic" && std::wstring(PluralRuleFor(L"de-AT").name) == L"one",
          L"rules by language code");

    // ---- Pseudo-language
    std::wstring ps = Pseudo(L"Freed {0} in {1} categories");
    Check(ps.front() == L'[' && ps.back() == L']', L"pseudo: bracketed");
    Check(ps.find(L"{0}") != std::wstring::npos && ps.find(L"{1}") != std::wstring::npos, L"pseudo: placeholders intact");
    Check(ps.find(L"Freed") == std::wstring::npos && ps.size() >= 28 * 13 / 10, L"pseudo: accented and at least 30% longer");
    Check(Pseudo(L"").empty(), L"pseudo: empty stays empty");
    auto pc = std::make_unique<Catalog>();
    pc->pseudo = true;
    pc->code = kPseudoCode;
    SetCatalog(std::move(pc));
    std::wstring pf = TrF(L"Moved {0}", L"README.md");
    Check(pf.find(L"README.md") != std::wstring::npos && pf.find(L"Moved") == std::wstring::npos, L"pseudo: values are not changed");
    Check(TrN(2, L"{0} item", L"{0} items").find(L"2 ") == 1, L"pseudo: plural forms");
    SetCatalog(nullptr);

    // ---- Matching the Windows language
    std::vector<LanguageInfo> avail = {{L"en", L"English", L""}, {L"de", L"Deutsch", L""}, {L"pt-BR", L"Português", L""}};
    Check(WindowsLanguageFor({L"de-AT", L"en-US"}, avail) == L"de", L"Windows language: de-AT takes de");
    Check(WindowsLanguageFor({L"pt-BR"}, avail) == L"pt-BR", L"Windows language: exact region");
    Check(WindowsLanguageFor({L"pt-PT"}, avail) == L"en", L"Windows language: pt-PT doesn't take pt-BR");
    Check(WindowsLanguageFor({L"fr-FR", L"de-DE"}, avail) == L"de", L"Windows language: the first available preference");
    Check(WindowsLanguageFor({L"en-GB", L"de-DE"}, avail) == L"en", L"Windows language: English before a later preference");
    Check(WindowsLanguageFor({}, avail) == L"en", L"Windows language: nothing: English");

    // ---- Regional number formats
    Check(util::FormatSizeShort(1536) == L"1.5 KiB" && util::FormatCount(1234567) == L"1,234,567", L"numbers: defaults");
    util::SetNumberFormat(L",", L".");
    Check(util::FormatSizeShort(1536) == L"1,5 KiB" && util::FormatCount(1234567) == L"1.234.567" && util::FormatDecimal(2.26, 1) == L"2,3",
          L"numbers: decimal comma and dot grouping");
    {
        EnglishScope en;
        Check(util::FormatSizeShort(1536) == L"1.5 KiB" && util::FormatCount(1234567) == L"1,234,567", L"numbers: English in an EnglishScope");
    }
    util::SetNumberFormat(L".", L"\u202F");
    Check(util::FormatCount(1234567) == L"1\u202F234\u202F567", L"numbers: a narrow no-break space as group separator");
    util::SetNumberFormat(L".", L",");
    Check(!util::FormatDateLocal(util::NowFileTime()).empty() && util::FormatDateLocal(0).empty(), L"dates: regional format, empty for 0");

    // ---- A language file on disk (UTF-8 with BOM)
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"bytopsy-i18n-test.lang";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::string utf8 = "\xEF\xBB\xBF@name = Fran\xC3\xA7" "ais\r\nClean up = Nettoyer\r\n";
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
    std::wstring text;
    Check(ReadLanguageFile(path, text), L"file: read");
    Catalog fc = ParseCatalog(text);
    Check(fc.name == L"Français" && fc.strings[L"Clean up"] == L"Nettoyer", L"file: UTF-8 decoded");
    DeleteFileW(path.c_str());

    // ---- The template and shipped translations
#ifdef BYTOPSY_SOURCE_DIR
    std::wstring dir = std::wstring(BYTOPSY_SOURCE_DIR) + L"\\res\\lang";
    std::wstring tt;
    Check(ReadLanguageFile(dir + L"\\template.lang", tt), L"template.lang: read");
    std::vector<std::wstring> tp;
    Catalog tc = ParseCatalog(tt, &tp);
    Check(tp.empty(), L"template.lang: parses without problems" + (tp.empty() ? std::wstring() : L" (" + tp[0] + L")"));
    // Every entry is empty in the template; count its lines with " =".
    size_t entries = 0, filled = tc.strings.size() + tc.plurals.size();
    for (size_t pos = 0; (pos = tt.find(L" =", pos)) != std::wstring::npos; ++pos) ++entries;
    Check(entries > 500 && filled == 0, L"template.lang: " + std::to_wstring(entries) + L" entries, none translated");
    Check(tt.find(L"\nClean up =") != std::wstring::npos && tt.find(L"\n{0} item|{0} items =") != std::wstring::npos,
          L"template.lang: has known strings");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.lang").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (_wcsicmp(name.c_str(), L"template.lang") == 0) continue;
            std::wstring lt;
            std::vector<std::wstring> lp;
            Check(ReadLanguageFile(dir + L"\\" + name, lt), name + L": read");
            ParseCatalog(lt, &lp);
            Check(lp.empty(), name + L": parses without problems" + (lp.empty() ? std::wstring() : L" (" + lp[0] + L")"));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#endif

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
