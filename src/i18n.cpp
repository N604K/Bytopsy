#include "i18n.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <set>

#include "util.h"

namespace i18n {

const wchar_t* const kPseudoCode = L"qps-ploc";

namespace {

// The catalog in use. Replaced ones are kept (a few per run at most), so a string another thread
// is still reading never goes away.
std::atomic<const Catalog*> g_catalog{nullptr};
std::mutex g_retiredLock;
std::vector<std::unique_ptr<Catalog>>& Retired() {
    static auto* v = new std::vector<std::unique_ptr<Catalog>>();  // intentionally leaked
    return *v;
}
thread_local int t_english = 0;

const Catalog* Active() { return t_english ? nullptr : g_catalog.load(std::memory_order_acquire); }

constexpr wchar_t kPluralSep = L'\x1F';

std::wstring PluralKey(std::wstring_view one, std::wstring_view other) {
    std::wstring k(one);
    k += kPluralSep;
    k += other;
    return k;
}

// ---- Plural rules (gettext / CLDR, integers only)
int RuleOne(uint64_t n) { return n == 1 ? 0 : 1; }
int RuleZeroOne(uint64_t n) { return n <= 1 ? 0 : 1; }
int RuleNone(uint64_t) { return 0; }
int RuleEastSlavic(uint64_t n) {
    return n % 10 == 1 && n % 100 != 11 ? 0 : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2;
}
int RulePolish(uint64_t n) { return n == 1 ? 0 : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2; }
int RuleCzech(uint64_t n) { return n == 1 ? 0 : n >= 2 && n <= 4 ? 1 : 2; }
int RuleRomanian(uint64_t n) { return n == 1 ? 0 : n == 0 || (n % 100 > 0 && n % 100 < 20) ? 1 : 2; }
int RuleLithuanian(uint64_t n) {
    return n % 10 == 1 && n % 100 != 11 ? 0 : n % 10 >= 2 && (n % 100 < 10 || n % 100 >= 20) ? 1 : 2;
}
int RuleSlovenian(uint64_t n) { return n % 100 == 1 ? 0 : n % 100 == 2 ? 1 : n % 100 == 3 || n % 100 == 4 ? 2 : 3; }
int RuleArabic(uint64_t n) {
    return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n % 100 >= 3 && n % 100 <= 10 ? 3 : n % 100 >= 11 ? 4 : 5;
}

std::wstring LowerAscii(std::wstring_view s) {
    std::wstring o(s);
    for (auto& c : o)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    return o;
}

std::wstring Trim(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) --b;
    return std::wstring(s.substr(a, b - a));
}

// Position of the first `c` not preceded by an escaping backslash, or npos.
size_t FindUnescaped(std::wstring_view s, wchar_t c, size_t from = 0) {
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] == L'\\') {
            ++i;
            continue;
        }
        if (s[i] == c) return i;
    }
    return std::wstring_view::npos;
}

std::vector<std::wstring_view> SplitUnescaped(std::wstring_view s, wchar_t c) {
    std::vector<std::wstring_view> parts;
    size_t start = 0;
    for (;;) {
        size_t p = FindUnescaped(s, c, start);
        if (p == std::wstring_view::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 1;
    }
}

std::wstring Unescape(std::wstring_view s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 >= s.size()) {
            o += s[i];
            continue;
        }
        wchar_t n = s[++i];
        switch (n) {
            case L'n': o += L'\n'; break;
            case L't': o += L'\t'; break;
            case L's': o += L' '; break;
            case L'\\':
            case L'=':
            case L'|':
            case L'#':
            case L'@': o += n; break;
            default:  // not an escape: keep it as written
                o += L'\\';
                o += n;
        }
    }
    return o;
}

// The {n} placeholders a pattern uses.
std::set<int> Placeholders(std::wstring_view s) {
    std::set<int> out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'{' && i + 1 < s.size() && s[i + 1] == L'{') {
            ++i;
            continue;
        }
        if (s[i] == L'{' && i + 2 < s.size() && iswdigit(s[i + 1]) && s[i + 2] == L'}') out.insert(s[i + 1] - L'0');
    }
    return out;
}

// Pseudo-language letters: accented look-alikes of a-z and A-Z (all in Segoe UI).
const wchar_t kPseudoLower[] = L"åƀçďéƒĝĥîĵķĺḿñöþǫŕšţûṽŵẋýž";
const wchar_t kPseudoUpper[] = L"ÅƁÇĎÉƑĜĤÎĴĶĹḾÑÖÞǪŔŠŢÛṼŴẊÝŽ";

}  // namespace

// =============================================================================================
// Lookup

std::wstring Tr(std::wstring_view english) {
    const Catalog* c = Active();
    if (!c || english.empty()) return std::wstring(english);
    // A text that is already pseudo ("[...~]", passed through Tr twice) stays as it is.
    if (c->pseudo) {
        bool already = english.size() > 2 && english.front() == L'[' && english.substr(english.size() - 2) == L"~]";
        return already ? std::wstring(english) : Pseudo(english);
    }
    auto it = c->strings.find(english);
    return it == c->strings.end() || it->second.empty() ? std::wstring(english) : it->second;
}

std::wstring Format(std::wstring_view p, const std::vector<std::wstring>& args) {
    std::wstring o;
    o.reserve(p.size() + 16);
    for (size_t i = 0; i < p.size(); ++i) {
        wchar_t c = p[i];
        if ((c == L'{' || c == L'}') && i + 1 < p.size() && p[i + 1] == c) {
            o += c;
            ++i;
            continue;
        }
        if (c == L'{' && i + 2 < p.size() && iswdigit(p[i + 1]) && p[i + 2] == L'}') {
            size_t k = static_cast<size_t>(p[i + 1] - L'0');
            if (k < args.size()) {
                o += args[k];
                i += 2;
                continue;
            }
        }
        o += c;
    }
    return o;
}

namespace detail {

std::wstring Count(uint64_t n) { return util::FormatCount(n); }

std::wstring PluralPattern(uint64_t n, std::wstring_view one, std::wstring_view other) {
    std::wstring_view english = n == 1 ? one : other;
    const Catalog* c = Active();
    if (!c) return std::wstring(english);
    if (c->pseudo) return Pseudo(english);
    auto it = c->plurals.find(PluralKey(one, other));
    if (it == c->plurals.end() || it->second.empty()) return std::wstring(english);
    const PluralRule& rule = c->plural ? *c->plural : PluralRuleFor(c->code);
    size_t k = static_cast<size_t>(std::clamp(rule.pick(n), 0, rule.forms - 1));
    k = std::min(k, it->second.size() - 1);
    return it->second[k].empty() ? std::wstring(english) : it->second[k];
}

}  // namespace detail

// =============================================================================================
// Plural rules

const std::vector<PluralRule>& PluralRules() {
    static const std::vector<PluralRule> rules = {
        {L"one", 2, RuleOne, L"1 | 0, 2, 3, ..."},                        // English, German, Dutch, Swedish, Italian, Spanish, ...
        {L"zero-one", 2, RuleZeroOne, L"0, 1 | 2, 3, ..."},               // French, Brazilian Portuguese, Hindi
        {L"none", 1, RuleNone, L"every number"},                         // Japanese, Chinese, Korean, Vietnamese, Thai, Indonesian
        {L"east-slavic", 3, RuleEastSlavic, L"1, 21 | 2-4, 22-24 | 0, 5-20, 25"},  // Russian, Ukrainian, Belarusian
        {L"polish", 3, RulePolish, L"1 | 2-4, 22-24 | 0, 5-21, 25"},
        {L"czech", 3, RuleCzech, L"1 | 2-4 | 0, 5, ..."},                  // Czech, Slovak
        {L"romanian", 3, RuleRomanian, L"1 | 0, 2-19, 101 | 20, 21, ..."},
        {L"lithuanian", 3, RuleLithuanian, L"1, 21 | 2-9, 22 | 0, 10-20"},
        {L"slovenian", 4, RuleSlovenian, L"1, 101 | 2, 102 | 3, 4 | 0, 5, ..."},
        {L"arabic", 6, RuleArabic, L"0 | 1 | 2 | 3-10 | 11-99 | 100-102"},
    };
    return rules;
}

const PluralRule* FindPluralRule(std::wstring_view name) {
    std::wstring n = LowerAscii(Trim(name));
    for (auto& r : PluralRules())
        if (n == r.name) return &r;
    return nullptr;
}

const PluralRule& PluralRuleFor(std::wstring_view code) {
    std::wstring c = LowerAscii(code);
    std::wstring lang = c.substr(0, c.find_first_of(L"-_"));
    const wchar_t* name = L"one";
    if (c == L"pt-br" || lang == L"fr" || lang == L"hi" || lang == L"fil") name = L"zero-one";
    else if (lang == L"ja" || lang == L"zh" || lang == L"ko" || lang == L"vi" || lang == L"th" || lang == L"id" || lang == L"ms")
        name = L"none";
    else if (lang == L"ru" || lang == L"uk" || lang == L"be") name = L"east-slavic";
    else if (lang == L"pl") name = L"polish";
    else if (lang == L"cs" || lang == L"sk") name = L"czech";
    else if (lang == L"ro") name = L"romanian";
    else if (lang == L"lt") name = L"lithuanian";
    else if (lang == L"sl") name = L"slovenian";
    else if (lang == L"ar") name = L"arabic";
    return *FindPluralRule(name);
}

// =============================================================================================
// Parsing

std::wstring EscapeEntry(std::wstring_view s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        switch (c) {
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n"; break;
            case L'\t': o += L"\\t"; break;
            case L'=': o += L"\\="; break;
            case L'|': o += L"\\|"; break;
            case L' ':
                o += i == 0 || i + 1 == s.size() ? L"\\s" : L" ";
                break;
            case L'#':
            case L'@':
                if (i == 0) o += L'\\';
                o += c;
                break;
            default: o += c;
        }
    }
    return o;
}

Catalog ParseCatalog(std::wstring_view text, std::vector<std::wstring>* problems) {
    Catalog cat;
    auto problem = [&](size_t line, const std::wstring& msg) {
        if (problems) problems->push_back(L"line " + std::to_wstring(line) + L": " + msg);
    };
    if (!text.empty() && text[0] == 0xFEFF) text.remove_prefix(1);
    size_t lineNo = 0, pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring_view raw = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#') {
            if (end == text.size()) break;
            continue;
        }
        size_t eq = FindUnescaped(line, L'=');
        if (eq == std::wstring::npos) {
            problem(lineNo, L"no \"=\" between the English text and the translation");
            if (end == text.size()) break;
            continue;
        }
        std::wstring keyRaw = Trim(std::wstring_view(line).substr(0, eq));
        std::wstring valRaw = Trim(std::wstring_view(line).substr(eq + 1));
        if (keyRaw.empty()) {
            problem(lineNo, L"nothing before \"=\"");
        } else if (keyRaw[0] == L'@') {
            std::wstring meta = LowerAscii(keyRaw.substr(1)), v = Unescape(valRaw);
            if (meta == L"name") cat.name = v;
            else if (meta == L"code") cat.code = v;
            else if (meta == L"plural") {
                cat.plural = v.empty() ? nullptr : FindPluralRule(v);  // empty: from the code
                if (!cat.plural && !v.empty()) problem(lineNo, L"unknown plural rule \"" + v + L"\"");
            } else if (meta != L"translators" && meta != L"language") {
                problem(lineNo, L"unknown setting @" + meta);
            }
        } else {
            auto keyParts = SplitUnescaped(keyRaw, L'|');
            if (keyParts.size() > 2) {
                problem(lineNo, L"more than one \"|\" in the English text");
            } else if (keyParts.size() == 2) {
                std::wstring one = Unescape(keyParts[0]), other = Unescape(keyParts[1]);
                std::set<int> english = Placeholders(one), o2 = Placeholders(other);
                english.insert(o2.begin(), o2.end());
                std::vector<std::wstring> forms;
                bool ok = true;
                if (!valRaw.empty())
                    for (auto f : SplitUnescaped(valRaw, L'|')) {
                        forms.push_back(Unescape(Trim(f)));
                        for (int p : Placeholders(forms.back()))
                            if (!english.count(p)) {
                                problem(lineNo, L"{" + std::to_wstring(p) + L"} is not in the English text");
                                ok = false;
                            }
                    }
                std::wstring key = PluralKey(one, other);
                if (ok && !forms.empty()) {
                    if (cat.plurals.count(key)) problem(lineNo, L"translated twice; the last one counts");
                    cat.plurals[key] = forms;
                }
            } else {
                std::wstring key = Unescape(keyRaw), val = Unescape(valRaw);
                std::set<int> english = Placeholders(key);
                bool ok = true;
                for (int p : Placeholders(val))
                    if (!english.count(p)) {
                        problem(lineNo, L"{" + std::to_wstring(p) + L"} is not in the English text");
                        ok = false;
                    }
                if (ok && !val.empty()) {
                    if (cat.strings.count(key)) problem(lineNo, L"translated twice; the last one counts");
                    cat.strings[key] = val;
                }
            }
        }
        if (end == text.size()) break;
    }
    return cat;
}

// =============================================================================================
// Pseudo-language

std::wstring Pseudo(std::wstring_view s) {
    if (s.empty()) return std::wstring();
    std::wstring o = L"[";
    size_t letters = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'{' && i + 2 < s.size() && iswdigit(s[i + 1]) && s[i + 2] == L'}') {  // a placeholder stays
            o.append(s.substr(i, 3));
            i += 2;
            continue;
        }
        if (c >= L'a' && c <= L'z') c = kPseudoLower[c - L'a'];
        else if (c >= L'A' && c <= L'Z') c = kPseudoUpper[c - L'A'];
        if (iswalpha(s[i])) ++letters;
        o += c;
    }
    // German and French run about 30% longer than English; short labels more.
    size_t extra = std::max<size_t>(2, (letters * 40 + 99) / 100);
    o += L' ';
    o.append(extra, L'~');
    o += L']';
    return o;
}

// =============================================================================================
// The language in use

void SetCatalog(std::unique_ptr<Catalog> c) {
    std::lock_guard<std::mutex> lk(g_retiredLock);
    const Catalog* old = g_catalog.exchange(c.get(), std::memory_order_acq_rel);
    if (c) Retired().push_back(std::move(c));
    (void)old;  // still owned by Retired()
}

std::wstring CurrentCode() {
    const Catalog* c = g_catalog.load(std::memory_order_acquire);
    return c ? c->code : L"en";
}

EnglishScope::EnglishScope() { ++t_english; }
EnglishScope::~EnglishScope() { --t_english; }

// =============================================================================================
// Where languages come from

namespace {

std::wstring ExeDir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return L"";
    std::wstring p(buf.data(), n);
    size_t slash = p.rfind(L'\\');
    return slash == std::wstring::npos ? L"" : p.substr(0, slash);
}

// "PT_BR" (a resource name) -> "pt-BR"; "zh_hans" -> "zh-Hans".
std::wstring CodeFromResourceName(std::wstring n) {
    std::wstring out;
    size_t part = 0, start = 0;
    while (start <= n.size()) {
        size_t sep = n.find_first_of(L"_-", start);
        std::wstring p = n.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        for (auto& c : p) c = static_cast<wchar_t>(part == 0 ? towlower(c) : p.size() == 2 ? towupper(c) : towlower(c));
        if (part > 0 && p.size() == 4 && !p.empty()) p[0] = static_cast<wchar_t>(towupper(p[0]));
        out += (part ? L"-" : L"") + p;
        ++part;
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    return out;
}

bool SameCode(std::wstring_view a, std::wstring_view b) { return LowerAscii(a) == LowerAscii(b); }

struct Source {
    LanguageInfo info;
    std::wstring resName;  // built in: the resource's name
};

bool ResourceText(const std::wstring& name, std::wstring& text) {
    HRSRC r = FindResourceW(nullptr, name.c_str(), L"LANG");
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    const char* p = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    if (!p) return false;
    text = util::Utf8ToWide(std::string_view(p, SizeofResource(nullptr, r)));
    return true;
}

// The @name of a language file, without parsing the rest.
std::wstring NameIn(std::wstring_view text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.size() > 5 && LowerAscii(line.substr(0, 5)) == L"@name") {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) return Unescape(Trim(std::wstring_view(line).substr(eq + 1)));
        }
    }
    return L"";
}

std::vector<Source> Sources() {
    std::vector<Source> out;
    auto add = [&](Source s) {
        for (auto& o : out)
            if (SameCode(o.info.code, s.info.code)) {
                o = std::move(s);  // a file replaces the built-in one
                return;
            }
        out.push_back(std::move(s));
    };
    std::vector<std::wstring> names;
    EnumResourceNamesW(
        nullptr, L"LANG",
        [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) -> BOOL {
            if (!IS_INTRESOURCE(name)) reinterpret_cast<std::vector<std::wstring>*>(param)->push_back(name);
            return TRUE;
        },
        reinterpret_cast<LONG_PTR>(&names));
    for (auto& n : names) {
        std::wstring text;
        if (!ResourceText(n, text)) continue;
        Source s;
        s.resName = n;
        s.info.code = CodeFromResourceName(n);
        s.info.name = NameIn(text);
        add(std::move(s));
    }
    std::vector<std::wstring> dirs;
    std::wstring exe = ExeDir();
    if (!exe.empty()) dirs.push_back(exe + L"\\lang");
    std::wstring data = util::DataDir();
    if (!data.empty() && !SameCode(data + L"\\lang", exe + L"\\lang")) dirs.push_back(data + L"\\lang");
    for (auto& d : dirs) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*.lang").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring file = d + L"\\" + fd.cFileName;
            std::wstring code(fd.cFileName);
            code = code.substr(0, code.size() - 5);
            if (SameCode(code, L"template") || SameCode(code, L"en") || code.empty()) continue;
            std::wstring text;
            if (!ReadLanguageFile(file, text)) continue;
            Source s;
            s.info.code = CodeFromResourceName(code);
            s.info.name = NameIn(text);
            s.info.file = file;
            add(std::move(s));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& s : out)
        if (s.info.name.empty()) s.info.name = s.info.code;
    return out;
}

}  // namespace

bool ReadLanguageFile(const std::wstring& file, std::wstring& text) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    std::string bytes;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = bytes.empty() || (ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size());
    }
    CloseHandle(h);
    if (ok) text = util::Utf8ToWide(bytes);
    return ok;
}

std::vector<LanguageInfo> AvailableLanguages() {
    std::vector<LanguageInfo> out{{L"en", L"English", L""}};
    std::vector<Source> s = Sources();
    std::sort(s.begin(), s.end(), [](const Source& a, const Source& b) { return _wcsicmp(a.info.name.c_str(), b.info.name.c_str()) < 0; });
    for (auto& x : s) out.push_back(x.info);
    return out;
}

std::wstring WindowsLanguageFor(const std::vector<std::wstring>& preferred, const std::vector<LanguageInfo>& available) {
    for (auto& p : preferred) {
        for (auto& a : available)
            if (SameCode(a.code, p)) return a.code;
        std::wstring base = p.substr(0, p.find(L'-'));
        for (auto& a : available)
            if (SameCode(a.code, base)) return a.code;
        if (SameCode(base, L"en")) return L"en";  // English is preferred over a later language
    }
    return L"en";
}

std::wstring WindowsLanguage(const std::vector<LanguageInfo>& available) {
    ULONG count = 0, size = 0;
    std::vector<std::wstring> preferred;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
        std::vector<wchar_t> buf(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size))
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) preferred.push_back(p);
    }
    return WindowsLanguageFor(preferred, available);
}

std::wstring LoadLanguage(const std::wstring& requested, std::vector<std::wstring>* problems) {
    std::wstring code = requested;
    wchar_t env[64];
    DWORD n = GetEnvironmentVariableW(L"BYTOPSY_LANG", env, 64);
    if (n > 0 && n < 64) code = env;
    if (SameCode(code, L"pseudo") || SameCode(code, kPseudoCode)) {
        auto c = std::make_unique<Catalog>();
        c->code = kPseudoCode;
        c->name = L"Pseudo";
        c->pseudo = true;
        SetCatalog(std::move(c));
        return kPseudoCode;
    }
    std::vector<Source> sources = Sources();
    if (code.empty()) {
        std::vector<LanguageInfo> avail{{L"en", L"English", L""}};
        for (auto& s : sources) avail.push_back(s.info);
        code = WindowsLanguage(avail);
    }
    for (auto& s : sources) {
        if (!SameCode(s.info.code, code)) continue;
        std::wstring text;
        bool ok = s.info.file.empty() ? ResourceText(s.resName, text) : ReadLanguageFile(s.info.file, text);
        if (!ok) {
            if (problems) problems->push_back(L"can't read " + (s.info.file.empty() ? s.info.code : s.info.file));
            break;
        }
        std::vector<std::wstring> issues;
        auto c = std::make_unique<Catalog>(ParseCatalog(text, &issues));
        c->code = s.info.code;
        if (c->name.empty()) c->name = s.info.name;
        if (problems)
            for (auto& i : issues) problems->push_back((s.info.file.empty() ? s.info.code + L".lang" : s.info.file) + L", " + i);
        SetCatalog(std::move(c));
        return s.info.code;
    }
    SetCatalog(nullptr);
    return L"en";
}

}  // namespace i18n
