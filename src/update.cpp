#include "update.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wintrust.h>

#include <algorithm>
#include <cstring>

#include "i18n.h"
#include "journal.h"
#include "runtool.h"
#include "updatekey.h"
#include "util.h"
#include "version.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

using i18n::Tr;
using i18n::TrF;

namespace {

bool StartsWithI(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
}

bool EndsWithI(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && _wcsnicmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring LeafOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring TempFolder() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::wstring t(buf, n);
    while (!t.empty() && t.back() == L'\\') t.pop_back();
    return t;
}

// Deletes a folder this code made. Links inside are removed, never followed.
void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                RemoveTree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(p.c_str());
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// ---- SHA-256 (bcrypt.dll)

class Sha256 {
public:
    Sha256() {
        ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0));
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Add(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (ok_ && n) {
            ULONG chunk = static_cast<ULONG>(std::min<size_t>(n, 1u << 30));
            ok_ = BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0));
            p += chunk;
            n -= chunk;
        }
    }
    bool Finish(uint8_t out[32]) { return ok_ && BCRYPT_SUCCESS(BCryptFinishHash(hash_, out, 32, 0)); }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    bool ok_ = false;
};

std::string Hex(const uint8_t* d, size_t n) {
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += kDigits[d[i] >> 4];
        s += kDigits[d[i] & 15];
    }
    return s;
}

// ---- A small strict JSON reader (the outer update.json is read before anything is trusted).

struct JVal {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    std::string text;  // a number as written, or a string (UTF-8)
    std::vector<JVal> items;
    std::vector<std::pair<std::string, JVal>> members;
    const JVal* Get(const char* key) const {
        for (auto& m : members)
            if (m.first == key) return &m.second;
        return nullptr;
    }
};

class JParser {
public:
    explicit JParser(std::string_view s) : p_(s.data()), e_(s.data() + s.size()) {}
    bool Document(JVal& v) {
        if (e_ - p_ >= 3 && memcmp(p_, "\xEF\xBB\xBF", 3) == 0) p_ += 3;
        Ws();
        if (!Value(v, 0)) return false;
        Ws();
        return p_ == e_;
    }

private:
    const char* p_;
    const char* e_;

    void Ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool Lit(const char* s) {
        size_t n = strlen(s);
        if (static_cast<size_t>(e_ - p_) < n || memcmp(p_, s, n) != 0) return false;
        p_ += n;
        return true;
    }
    bool Value(JVal& v, int depth) {
        if (depth > 32 || p_ >= e_) return false;
        switch (*p_) {
            case '{': return Obj(v, depth);
            case '[': return Arr(v, depth);
            case '"': v.kind = JVal::String; return Str(v.text);
            case 't': v.kind = JVal::Bool; v.b = true; return Lit("true");
            case 'f': v.kind = JVal::Bool; v.b = false; return Lit("false");
            case 'n': v.kind = JVal::Null; return Lit("null");
            default: return Num(v);
        }
    }
    bool Obj(JVal& v, int depth) {
        v.kind = JVal::Object;
        ++p_;
        Ws();
        if (p_ < e_ && *p_ == '}') return ++p_, true;
        for (;;) {
            Ws();
            std::string key;
            if (p_ >= e_ || *p_ != '"' || !Str(key)) return false;
            if (v.Get(key.c_str())) return false;  // a key twice: refuse rather than guess which counts
            Ws();
            if (p_ >= e_ || *p_ != ':') return false;
            ++p_;
            Ws();
            JVal m;
            if (!Value(m, depth + 1)) return false;
            v.members.emplace_back(std::move(key), std::move(m));
            Ws();
            if (p_ < e_ && *p_ == ',') {
                ++p_;
                continue;
            }
            if (p_ < e_ && *p_ == '}') return ++p_, true;
            return false;
        }
    }
    bool Arr(JVal& v, int depth) {
        v.kind = JVal::Array;
        ++p_;
        Ws();
        if (p_ < e_ && *p_ == ']') return ++p_, true;
        for (;;) {
            Ws();
            JVal item;
            if (!Value(item, depth + 1)) return false;
            v.items.push_back(std::move(item));
            Ws();
            if (p_ < e_ && *p_ == ',') {
                ++p_;
                continue;
            }
            if (p_ < e_ && *p_ == ']') return ++p_, true;
            return false;
        }
    }
    bool Hex4(uint32_t& cp) {
        if (e_ - p_ < 4) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= c - '0';
            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    static void Utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool Str(std::string& out) {
        ++p_;
        while (p_ < e_) {
            unsigned char c = static_cast<unsigned char>(*p_++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (p_ >= e_) return false;
            char x = *p_++;
            switch (x) {
                case '"':
                case '\\':
                case '/': out += x; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo;
                        if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
                        p_ += 2;
                        if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return false;
                    }
                    Utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Digits() {
        const char* s = p_;
        while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
        return p_ > s;
    }
    bool Num(JVal& v) {
        const char* s = p_;
        if (p_ < e_ && *p_ == '-') ++p_;
        if (!Digits()) return false;
        if (p_ < e_ && *p_ == '.') {
            ++p_;
            if (!Digits()) return false;
        }
        if (p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < e_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (!Digits()) return false;
        }
        v.kind = JVal::Number;
        v.text.assign(s, p_);
        return true;
    }
};

bool GetString(const JVal& obj, const char* key, std::wstring& out, bool required) {
    const JVal* v = obj.Get(key);
    if (!v) return !required;
    if (v->kind != JVal::String) return false;
    out = util::Utf8ToWide(v->text);
    return true;
}

bool IsHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// ---- Versions

struct Ver {
    std::vector<uint64_t> nums;
    std::wstring pre;
};

Ver SplitVersion(std::wstring_view v) {
    Ver r;
    size_t i = 0;
    if (i < v.size() && (v[i] == L'v' || v[i] == L'V')) ++i;
    while (i < v.size()) {
        uint64_t n = 0;
        size_t start = i;
        while (i < v.size() && v[i] >= L'0' && v[i] <= L'9') n = n * 10 + (v[i++] - L'0');
        if (i == start) break;
        r.nums.push_back(n);
        if (i < v.size() && v[i] == L'.') {
            ++i;
            continue;
        }
        break;
    }
    if (i < v.size() && v[i] == L'-') r.pre = std::wstring(v.substr(i + 1));
    return r;
}

// Pre-release parts ("beta.10" vs "beta.2"): numbers compare as numbers, words as text.
int ComparePre(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (i >= a.size()) return -1;
        if (j >= b.size()) return 1;
        size_t ie = a.find(L'.', i), je = b.find(L'.', j);
        if (ie == std::wstring::npos) ie = a.size();
        if (je == std::wstring::npos) je = b.size();
        std::wstring x = a.substr(i, ie - i), y = b.substr(j, je - j);
        bool xn = !x.empty() && x.find_first_not_of(L"0123456789") == std::wstring::npos;
        bool yn = !y.empty() && y.find_first_not_of(L"0123456789") == std::wstring::npos;
        int c;
        if (xn && yn) {
            uint64_t xv = _wcstoui64(x.c_str(), nullptr, 10), yv = _wcstoui64(y.c_str(), nullptr, 10);
            c = xv < yv ? -1 : xv > yv ? 1 : 0;
        } else if (xn != yn) {
            c = xn ? -1 : 1;
        } else {
            c = x.compare(y);
            c = c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        if (c) return c;
        i = ie + 1;
        j = je + 1;
    }
    return 0;
}

// ---- Network

const wchar_t* kUserAgent = L"Bytopsy/" BYTOPSY_VERSION L" (update check)";

std::wstring WinHttpError(DWORD e) {
    switch (e) {
        case ERROR_WINHTTP_TIMEOUT: return Tr(L"the server didn't answer in time");
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return Tr(L"the address couldn't be found (offline?)");
        case ERROR_WINHTTP_CANNOT_CONNECT: return Tr(L"couldn't connect to the server");
        case ERROR_WINHTTP_SECURE_FAILURE: return Tr(L"the secure connection failed (certificate problem)");
        case ERROR_WINHTTP_REDIRECT_FAILED: return Tr(L"the server redirected to an address that isn't HTTPS");
        default: break;
    }
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                             GetModuleHandleW(L"winhttp.dll"), e, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring s = n && msg ? std::wstring(msg, n) : util::Win32ErrorMessage(e);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

struct Internet {
    HINTERNET h = nullptr;
    Internet() = default;
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    ~Internet() {
        if (h) WinHttpCloseHandle(h);
    }
};

using Sink = std::function<bool(const char* data, size_t n)>;

// file:///C:/path/file -> C:\path\file (the local test mode)
std::wstring FileUrlPath(std::wstring_view url) {
    std::wstring p;
    std::wstring_view rest = url.substr(8);
    for (size_t i = 0; i < rest.size(); ++i) {
        wchar_t c = rest[i];
        if (c == L'%' && i + 2 < rest.size() && iswxdigit(rest[i + 1]) && iswxdigit(rest[i + 2])) {
            p += static_cast<wchar_t>(wcstoul(std::wstring(rest.substr(i + 1, 2)).c_str(), nullptr, 16));
            i += 2;
        } else {
            p += c == L'/' ? L'\\' : c;
        }
    }
    return p;
}

bool FileGet(std::wstring_view url, uint64_t maxBytes, const Sink& sink, std::wstring& err, std::atomic<bool>* cancel,
             const HttpProgress& progress) {
    std::wstring path = FileUrlPath(url);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = TrF(L"{0}: {1}", path, util::Win32ErrorMessage(GetLastError()));
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t total = static_cast<uint64_t>(size.QuadPart), done = 0;
    bool ok = true;
    if (total > maxBytes) {
        err = TrF(L"the file is larger than expected ({0})", util::FormatSizeShort(total));
        ok = false;
    }
    std::vector<char> buf(64 * 1024);
    while (ok) {
        if (cancel && cancel->load()) {
            err = Tr(L"Cancelled");
            ok = false;
            break;
        }
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            err = util::Win32ErrorMessage(GetLastError());
            ok = false;
            break;
        }
        if (!got) break;
        done += got;
        if (done > maxBytes) {
            err = Tr(L"the file is larger than expected");
            ok = false;
            break;
        }
        if (!sink(buf.data(), got)) {
            ok = false;
            break;
        }
        if (progress) progress(done, total);
    }
    CloseHandle(h);
    return ok;
}

bool HttpGet(const std::wstring& url, uint64_t maxBytes, bool allowLocal, const Sink& sink, std::wstring& err, std::atomic<bool>* cancel,
             const HttpProgress& progress) {
    if (allowLocal && StartsWithI(url, L"file:///")) return FileGet(url, maxBytes, sink, err, cancel, progress);
    wchar_t host[256] = {}, path[4096] = {}, extra[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        err = TrF(L"not a valid address: {0}", url);
        return false;
    }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    bool local = !https && allowLocal && IsLocalTestUrl(url);
    if (!https && !local) {
        err = TrF(L"only HTTPS addresses are used, not {0}", url);
        return false;
    }
    Internet session;
    session.h = WinHttpOpen(kUserAgent, local ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h && !local) {
        // Windows without automatic proxy support in WinHTTP: the proxy set in Internet Options, if any.
        std::wstring proxy, bypass;
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
        if (WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
            if (ie.lpszProxy) proxy = ie.lpszProxy;
            if (ie.lpszProxyBypass) bypass = ie.lpszProxyBypass;
            if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
            if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
            if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
        }
        session.h = proxy.empty() ? WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
                                  : WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(),
                                                bypass.empty() ? WINHTTP_NO_PROXY_BYPASS : bypass.c_str(), 0);
    }
    if (!session.h) {
        err = WinHttpError(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    // GitHub answers release downloads with a redirect to its file storage: followed, but never
    // from HTTPS to plain HTTP, and not endlessly.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    DWORD maxRedirects = 5;
    WinHttpSetOption(session.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));

    Internet conn, req;
    conn.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    std::wstring object = std::wstring(path) + extra;
    if (conn.h)
        req.h = WinHttpOpenRequest(conn.h, L"GET", object.empty() ? L"/" : object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) {
        err = WinHttpError(GetLastError());
        return false;
    }
    DWORD features = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    WinHttpSetOption(req.h, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &maxRedirects, sizeof(maxRedirects));
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        err = WinHttpError(GetLastError());
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = status == 404 ? Tr(L"not found (404)") : TrF(L"the server answered {0}", static_cast<unsigned>(status));
        return false;
    }
    uint64_t total = 0;
    wchar_t len[32] = {};
    DWORD lenSize = sizeof(len);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lenSize, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(len, nullptr, 10);
    if (total > maxBytes) {
        err = TrF(L"the file is larger than expected ({0})", util::FormatSizeShort(total));
        return false;
    }
    std::vector<char> buf(64 * 1024);
    uint64_t done = 0;
    for (;;) {
        if (cancel && cancel->load()) {
            err = Tr(L"Cancelled");
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            err = WinHttpError(GetLastError());
            return false;
        }
        if (!got) break;
        done += got;
        if (done > maxBytes) {
            err = Tr(L"the file is larger than expected");
            return false;
        }
        if (!sink(buf.data(), got)) return false;
        if (progress) progress(done, total);
    }
    return true;
}

std::wstring HostOf(const std::wstring& url) {
    if (StartsWithI(url, L"file:")) return L"file";
    size_t s = url.find(L"://");
    if (s == std::wstring::npos) return url;
    size_t e = url.find_first_of(L"/:", s + 3);
    return url.substr(s + 3, e == std::wstring::npos ? std::wstring::npos : e - s - 3);
}

// A short path for tools that may not cope with every character in a long one (tar.exe).
std::wstring ToolPath(const std::wstring& p) {
    bool ascii = std::all_of(p.begin(), p.end(), [](wchar_t c) { return c < 0x80; });
    if (ascii) return p;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetShortPathNameW(p.c_str(), buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? std::wstring(buf, n) : p;
}

bool MoveWithRetry(const std::wstring& from, const std::wstring& to, DWORD& error) {
    for (int i = 0; i < 6; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED) return false;
        Sleep(250);
    }
    return false;
}

// What an update installs and may leave behind as <name>.old.
const wchar_t* const kReleaseFiles[] = {L"bytopsy.exe", L"bytopsy-cli.exe", L"README.md", L"LICENSE"};

}  // namespace

// =============================================================================================
// Versions

int CompareVersions(std::wstring_view a, std::wstring_view b) {
    Ver x = SplitVersion(a), y = SplitVersion(b);
    size_t n = std::max(x.nums.size(), y.nums.size());
    for (size_t i = 0; i < n; ++i) {
        uint64_t p = i < x.nums.size() ? x.nums[i] : 0, q = i < y.nums.size() ? y.nums[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    if (x.pre.empty() != y.pre.empty()) return x.pre.empty() ? 1 : -1;  // 1.2.0-beta < 1.2.0
    return ComparePre(x.pre, y.pre);
}

bool IsValidVersion(std::wstring_view v) {
    size_t i = 0, parts = 0;
    while (true) {
        size_t start = i;
        while (i < v.size() && v[i] >= L'0' && v[i] <= L'9') ++i;
        if (i == start || i - start > 9) return false;
        ++parts;
        if (i < v.size() && v[i] == L'.') {
            ++i;
            continue;
        }
        break;
    }
    if (parts > 4) return false;
    if (i == v.size()) return true;
    if (v[i] != L'-' || i + 1 == v.size() || v.size() - i > 40) return false;
    for (++i; i < v.size(); ++i)
        if (!iswalnum(v[i]) && v[i] != L'.') return false;
    return true;
}

// =============================================================================================
// Encoding, hashes, signatures

bool Base64Decode(std::string_view in, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    size_t pad = 0, chars = 0;
    for (char c : in) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        ++chars;
        if (c == '=') {
            if (++pad > 2) return false;
            continue;
        }
        if (pad) return false;  // data after the padding
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else return false;
        acc = ((acc << 6) | static_cast<uint32_t>(v)) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return chars % 4 == 0 || pad == 0;
}

std::string Base64Encode(const uint8_t* data, size_t n) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < n) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < n) v |= data[i + 2];
        s += kAlphabet[(v >> 18) & 63];
        s += kAlphabet[(v >> 12) & 63];
        s += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
        s += i + 2 < n ? kAlphabet[v & 63] : '=';
    }
    return s;
}

std::string Sha256Hex(const void* data, size_t n) {
    Sha256 h;
    h.Add(data, n);
    uint8_t d[32];
    return h.Finish(d) ? Hex(d, 32) : "";
}

bool Sha256File(const std::wstring& path, std::string& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    Sha256 h;
    std::vector<char> buf(256 * 1024);
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) != FALSE) && got) h.Add(buf.data(), got);
    CloseHandle(f);
    uint8_t d[32];
    if (!ok || !h.Finish(d)) return false;
    hex = Hex(d, 32);
    return true;
}

bool IsPlaceholderKey(const uint8_t* key, size_t keyLen) {
    if (keyLen != 64) return true;
    for (size_t i = 0; i < keyLen; ++i)
        if (key[i]) return false;
    return true;
}

bool VerifyEcdsaP256(const uint8_t* data, size_t n, const uint8_t* sig, size_t sigLen, const uint8_t* key, size_t keyLen) {
    if (sigLen != 64 || keyLen != 64 || IsPlaceholderKey(key, keyLen)) return false;
    uint8_t digest[32];
    {
        Sha256 h;
        h.Add(data, n);
        if (!h.Finish(digest)) return false;
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
    std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), key, 64);
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, blob.data(), static_cast<ULONG>(blob.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptVerifySignature(k, nullptr, digest, 32, const_cast<PUCHAR>(sig), 64, 0));
    if (k) BCryptDestroyKey(k);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// =============================================================================================
// The manifest

bool ParseManifestJson(std::string_view json, UpdateManifest& m, std::wstring& err) {
    m = UpdateManifest{};
    JVal root;
    if (!JParser(json).Document(root) || root.kind != JVal::Object) {
        err = Tr(L"the update information is not valid JSON");
        return false;
    }
    if (!GetString(root, "version", m.version, true) || !IsValidVersion(m.version)) {
        err = Tr(L"the update information has no valid version");
        return false;
    }
    if (!GetString(root, "released", m.released, false) || !GetString(root, "notes_url", m.notesUrl, false) ||
        !GetString(root, "minimum_version", m.minimumVersion, false)) {
        err = Tr(L"the update information has a field of the wrong kind");
        return false;
    }
    if (!m.minimumVersion.empty() && !IsValidVersion(m.minimumVersion)) {
        err = Tr(L"the update information has no valid minimum version");
        return false;
    }
    if (!m.notesUrl.empty() && !StartsWithI(m.notesUrl, L"https://")) m.notesUrl.clear();
    const JVal* assets = root.Get("assets");
    if (!assets || assets->kind != JVal::Array) {
        err = Tr(L"the update information lists no downloads");
        return false;
    }
    for (const JVal& a : assets->items) {
        UpdateAsset as;
        const JVal* sha = a.kind == JVal::Object ? a.Get("sha256") : nullptr;
        const JVal* size = a.kind == JVal::Object ? a.Get("size") : nullptr;
        if (a.kind != JVal::Object || !GetString(a, "name", as.name, true) || !GetString(a, "url", as.url, true) || !sha ||
            sha->kind != JVal::String || !IsHex64(sha->text) || !size || size->kind != JVal::Number ||
            size->text.find_first_not_of("0123456789") != std::string::npos || size->text.size() > 15) {
            err = Tr(L"the update information has a download without name, address, SHA-256 or size");
            return false;
        }
        as.sha256 = sha->text;
        for (auto& c : as.sha256) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        as.size = _strtoui64(size->text.c_str(), nullptr, 10);
        m.assets.push_back(std::move(as));
    }
    return true;
}

bool ParseSignedManifest(std::string_view file, const uint8_t* key, size_t keyLen, UpdateManifest& m, std::wstring& err) {
    if (IsPlaceholderKey(key, keyLen)) {
        err = Tr(L"this build of Bytopsy has no update key");
        return false;
    }
    if (file.size() > kMaxManifestBytes) {
        err = Tr(L"the update information is too large");
        return false;
    }
    JVal root;
    if (!JParser(file).Document(root) || root.kind != JVal::Object) {
        err = Tr(L"the update information is not valid JSON");
        return false;
    }
    const JVal* format = root.Get("format");
    const JVal* manifest = root.Get("manifest");
    const JVal* signature = root.Get("signature");
    if (!format || format->kind != JVal::Number || format->text != "1") {
        err = Tr(L"the update information is in a format this version of Bytopsy doesn't read");
        return false;
    }
    std::vector<uint8_t> inner, sig;
    if (!manifest || manifest->kind != JVal::String || !signature || signature->kind != JVal::String ||
        !Base64Decode(manifest->text, inner) || !Base64Decode(signature->text, sig) || sig.size() != 64 || inner.empty()) {
        err = Tr(L"the update information is incomplete");
        return false;
    }
    // Nothing inside is looked at before the signature is known to be good.
    if (!VerifyEcdsaP256(inner.data(), inner.size(), sig.data(), sig.size(), key, keyLen)) {
        err = Tr(L"the update information's signature is not valid");
        return false;
    }
    return ParseManifestJson(std::string_view(reinterpret_cast<const char*>(inner.data()), inner.size()), m, err);
}

bool IsReleaseAssetUrl(std::wstring_view url) {
    std::wstring_view prefix = kReleaseDownloadPrefix;
    if (url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix) return false;
    std::wstring_view rest = url.substr(prefix.size());  // <tag>/<file>
    size_t slash = rest.find(L'/');
    if (slash == std::wstring_view::npos || slash == 0 || slash + 1 == rest.size() || rest.find(L'/', slash + 1) != std::wstring_view::npos)
        return false;
    for (wchar_t c : rest)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_' ||
              c == L'+' || c == L'/'))
            return false;
    std::wstring_view tag = rest.substr(0, slash), file = rest.substr(slash + 1);
    return tag != L"." && tag != L".." && file != L"." && file != L"..";
}

bool IsLocalTestUrl(std::wstring_view url) {
    if (StartsWithI(url, L"file:///")) return url.size() > 8;
    for (std::wstring_view host : {L"http://localhost", L"http://127.0.0.1"})
        if (StartsWithI(url, host) && url.size() > host.size() && (url[host.size()] == L':' || url[host.size()] == L'/')) return true;
    return false;
}

UpdateCheck EvaluateManifest(const UpdateManifest& m, std::wstring_view running, bool local) {
    UpdateCheck c;
    c.manifest = m;
    c.local = local;
    if (CompareVersions(m.version, running) <= 0) {
        c.status = UpdateStatus::UpToDate;
        return c;
    }
    const UpdateAsset* zip = nullptr;
    for (auto& a : m.assets)
        if (EndsWithI(a.name, L"-x64.zip")) {
            zip = &a;
            break;
        }
    if (!zip) {
        c.error = Tr(L"the update information names no download for 64-bit Windows");
        return c;
    }
    if (local ? !IsLocalTestUrl(zip->url) : !IsReleaseAssetUrl(zip->url)) {
        c.error = TrF(L"the download isn't one of Bytopsy's GitHub releases: {0}", zip->url);
        return c;
    }
    if (!zip->size || zip->size > kMaxZipBytes) {
        c.error = TrF(L"the download's size is out of range ({0})", util::FormatSizeShort(zip->size));
        return c;
    }
    c.asset = *zip;
    c.required = !m.minimumVersion.empty() && CompareVersions(running, m.minimumVersion) < 0;
    c.status = UpdateStatus::Available;
    return c;
}

UpdateSources DefaultUpdateSources() {
    UpdateSources s;
    s.urls = {kUpdateUrl, kUpdateFallbackUrl};
    s.key.assign(kUpdatePublicKey, kUpdatePublicKey + sizeof(kUpdatePublicKey));
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(L"BYTOPSY_UPDATE_URL", buf, static_cast<DWORD>(std::size(buf)));
    if (n && n < std::size(buf) && IsLocalTestUrl(std::wstring_view(buf, n))) {
        s.urls = {std::wstring(buf, n)};
        s.local = true;
        char key[256];
        DWORD k = GetEnvironmentVariableA("BYTOPSY_UPDATE_TEST_KEY", key, static_cast<DWORD>(std::size(key)));
        std::vector<uint8_t> bytes;
        if (k && k < std::size(key) && Base64Decode(std::string_view(key, k), bytes) && bytes.size() == 64) s.key = bytes;
    }
    return s;
}

UpdateCheck CheckForUpdate(const UpdateSources& src, std::wstring_view running, std::atomic<bool>* cancel) {
    UpdateCheck c;
    if (src.key.size() != 64 || IsPlaceholderKey(src.key.data(), src.key.size())) {
        c.status = UpdateStatus::NoKey;
        c.error = Tr(L"this build of Bytopsy has no update key, so it can't check for updates");
        return c;
    }
    std::wstring errors;
    for (const std::wstring& url : src.urls) {
        if (cancel && cancel->load()) {
            errors = Tr(L"Cancelled");
            break;
        }
        std::string body;
        std::wstring err;
        UpdateManifest m;
        if (HttpGetString(url, kMaxManifestBytes, src.local, body, err, cancel) && ParseSignedManifest(body, src.key.data(), src.key.size(), m, err)) {
            c = EvaluateManifest(m, running, src.local);
            c.source = url;
            return c;
        }
        errors += (errors.empty() ? L"" : L" · ") + TrF(L"{0}: {1}", HostOf(url), err);
    }
    c.status = UpdateStatus::Failed;
    c.local = src.local;
    c.error = errors;
    return c;
}

bool HttpGetString(const std::wstring& url, size_t maxBytes, bool allowLocal, std::string& out, std::wstring& err, std::atomic<bool>* cancel) {
    out.clear();
    return HttpGet(
        url, maxBytes, allowLocal,
        [&](const char* d, size_t n) {
            out.append(d, n);
            return true;
        },
        err, cancel, nullptr);
}

bool HttpGetFile(const std::wstring& url, const std::wstring& path, uint64_t maxBytes, bool allowLocal, std::wstring& err, std::atomic<bool>* cancel,
                 const HttpProgress& progress) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        err = TrF(L"{0}: {1}", path, util::Win32ErrorMessage(GetLastError()));
        return false;
    }
    std::wstring writeErr;
    bool ok = HttpGet(
        url, maxBytes, allowLocal,
        [&](const char* d, size_t n) {
            DWORD written = 0;
            if (WriteFile(f, d, static_cast<DWORD>(n), &written, nullptr) && written == n) return true;
            writeErr = TrF(L"{0}: {1}", path, util::Win32ErrorMessage(GetLastError()));
            return false;
        },
        err, cancel, progress);
    CloseHandle(f);
    if (!ok) {
        if (!writeErr.empty()) err = writeErr;
        DeleteFileW(path.c_str());
    }
    return ok;
}

// =============================================================================================
// Package managers

PackageManager PackageManagerFor(std::wstring_view exePath) {
    std::wstring p = util::ToLower(exePath);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (p.find(L"\\scoop\\apps\\") != std::wstring::npos || p.find(L"\\apps\\bytopsy\\current\\") != std::wstring::npos) return PackageManager::Scoop;
    if (p.find(L"\\winget\\packages\\") != std::wstring::npos || p.find(L"\\winget\\links\\") != std::wstring::npos) return PackageManager::Winget;
    if (p.find(L"\\chocolatey\\lib\\") != std::wstring::npos) return PackageManager::Chocolatey;
    return PackageManager::None;
}

const wchar_t* PackageManagerName(PackageManager pm) {
    switch (pm) {
        case PackageManager::Winget: return L"winget";
        case PackageManager::Scoop: return L"Scoop";
        case PackageManager::Chocolatey: return L"Chocolatey";
        default: return L"";
    }
}

const wchar_t* PackageManagerCommand(PackageManager pm) {
    switch (pm) {
        case PackageManager::Winget: return L"winget upgrade N604K.Bytopsy";
        case PackageManager::Scoop: return L"scoop update bytopsy";
        case PackageManager::Chocolatey: return L"choco upgrade bytopsy";
        default: return L"";
    }
}

// =============================================================================================
// Installing

bool ExtractZip(const std::wstring& zip, const std::wstring& dir, std::wstring& err) {
    std::wstring tar = SystemTool(L"tar.exe");
    if (!Exists(tar)) {
        err = Tr(L"Windows' tar.exe is missing (it comes with Windows 10 version 1803 and later)");
        return false;
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        err = TrF(L"{0}: {1}", dir, util::Win32ErrorMessage(GetLastError()));
        return false;
    }
    std::string out;
    int code = RunTool(tar, L"-x -f \"" + ToolPath(zip) + L"\" -C \"" + ToolPath(dir) + L"\"", out, 180000);
    if (code != 0) {
        std::wstring last = LastLine(out);
        err = TrF(L"unpacking failed: {0}", last.empty() ? TrF(L"tar.exe ended with code {0}", code) : last);
        return false;
    }
    return true;
}

bool FindReleaseFiles(const std::wstring& dir, std::wstring& folder, std::vector<std::wstring>& names, std::wstring& err) {
    names.clear();
    folder = dir;
    if (!Exists(dir + L"\\bytopsy.exe")) {
        // A zip with one folder at the top (bytopsy-1.2.0-x64\...).
        std::vector<std::wstring> subs;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n != L"." && n != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    subs.push_back(n);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (subs.size() == 1 && Exists(dir + L"\\" + subs[0] + L"\\bytopsy.exe")) folder = dir + L"\\" + subs[0];
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        err = Tr(L"the update's zip is empty");
        return false;
    }
    bool exe = false, bad = false;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            bad = true;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // Never replaced: what puts a copy in portable mode.
        if (_wcsicmp(n.c_str(), util::kPortableMarker) == 0 || _wcsicmp(n.c_str(), L"winncdu.portable") == 0) continue;
        if (EndsWithI(n, L".old") || EndsWithI(n, L".new")) continue;
        if (_wcsicmp(n.c_str(), L"bytopsy.exe") == 0) exe = true;
        names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (bad) {
        err = Tr(L"the update's zip contains a link, which a release never does");
        return false;
    }
    if (!exe) {
        err = Tr(L"the update's zip has no bytopsy.exe");
        return false;
    }
    // The programs first: if one can't be replaced, nothing else has changed yet.
    std::stable_sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return EndsWithI(a, L".exe") && !EndsWithI(b, L".exe"); });
    return true;
}

bool AuthenticodeSigner(const std::wstring& file, std::wstring& subject) {
    subject.clear();
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    // Offline: no revocation lists or certificates are fetched.
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    HWND noUi = static_cast<HWND>(INVALID_HANDLE_VALUE);
    bool ok = WinVerifyTrust(noUi, &action, &wd) == ERROR_SUCCESS;
    if (ok) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        PCCERT_CONTEXT cert = sg && sg->csCertChain && sg->pasCertChain ? sg->pasCertChain[0].pCert : nullptr;
        if (cert) {
            DWORD n = CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, nullptr, 0);
            if (n > 1) {
                std::wstring s(n, L'\0');
                CertNameToStrW(X509_ASN_ENCODING, &cert->pCertInfo->Subject, CERT_X500_NAME_STR, s.data(), n);
                s.resize(n - 1);
                subject = s;
            }
        }
        ok = !subject.empty();
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(noUi, &action, &wd);
    return ok;
}

bool CheckSamePublisher(const std::wstring& runningExe, const std::vector<std::wstring>& newExes, std::wstring& err) {
    std::wstring mine;
    if (!AuthenticodeSigner(runningExe, mine)) return true;  // unsigned: the signed manifest and its hash vouch for the files
    for (const std::wstring& exe : newExes) {
        std::wstring theirs;
        if (!AuthenticodeSigner(exe, theirs)) {
            err = TrF(L"{0} in the update isn't validly signed, but this copy of Bytopsy is", LeafOf(exe));
            return false;
        }
        if (theirs != mine) {
            err = TrF(L"{0} in the update is signed by someone else ({1})", LeafOf(exe), theirs);
            return false;
        }
    }
    return true;
}

bool FolderWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\bytopsy-write-test-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool InstallFiles(const std::wstring& exeDir, const std::wstring& newDir, const std::vector<std::wstring>& names, std::wstring& err) {
    auto at = [&](const std::wstring& n, const wchar_t* suffix) { return exeDir + L"\\" + n + suffix; };
    for (const std::wstring& n : names) {
        if (n.empty() || n.find_first_of(L"\\/:") != std::wstring::npos) {
            err = TrF(L"not a file name: {0}", n);
            return false;
        }
        DeleteFileW(at(n, L".new").c_str());
        if (Exists(at(n, L".old")) && !DeleteFileW(at(n, L".old").c_str())) {
            err = TrF(L"{0} from the last update is still in use: close every Bytopsy window and try again", n + L".old");
            return false;
        }
    }
    // 1. Copy every new file in next to the old one; nothing has changed if this fails.
    std::vector<std::wstring> copied;
    auto removeCopies = [&] {
        for (auto& n : copied) DeleteFileW(at(n, L".new").c_str());
    };
    for (const std::wstring& n : names) {
        if (!CopyFileW((newDir + L"\\" + n).c_str(), at(n, L".new").c_str(), TRUE)) {
            err = TrF(L"couldn't write {0}: {1}", at(n, L".new"), util::Win32ErrorMessage(GetLastError()));
            removeCopies();
            return false;
        }
        copied.push_back(n);
    }
    // 2. Swap them one by one: the old file becomes <name>.old, the new one takes its name.
    struct Swapped {
        std::wstring name;
        bool hadOld;
    };
    std::vector<Swapped> swapped;
    auto rollback = [&] {
        for (auto it = swapped.rbegin(); it != swapped.rend(); ++it) {
            std::wstring cur = at(it->name, L"");
            if (MoveFileExW(cur.c_str(), at(it->name, L".new").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) && it->hadOld)
                MoveFileExW(at(it->name, L".old").c_str(), cur.c_str(), MOVEFILE_WRITE_THROUGH);
        }
        removeCopies();
    };
    for (const std::wstring& n : names) {
        std::wstring cur = at(n, L"");
        bool had = Exists(cur);
        DWORD e = 0;
        if (had && !MoveWithRetry(cur, at(n, L".old"), e)) {
            err = e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION
                      ? TrF(L"{0} is in use: close every Bytopsy window and bytopsy-cli, then try again", cur)
                      : TrF(L"couldn't replace {0}: {1}", cur, util::Win32ErrorMessage(e));
            rollback();
            return false;
        }
        if (!MoveWithRetry(at(n, L".new"), cur, e)) {
            if (had) MoveFileExW(at(n, L".old").c_str(), cur.c_str(), MOVEFILE_WRITE_THROUGH);
            err = TrF(L"couldn't replace {0}: {1}", cur, util::Win32ErrorMessage(e));
            rollback();
            return false;
        }
        swapped.push_back({n, had});
    }
    return true;
}

int CleanupOldFiles(const std::wstring& exeDir) {
    int left = 0;
    for (const wchar_t* f : kReleaseFiles)
        for (const wchar_t* suffix : {L".old", L".new"}) {
            std::wstring p = exeDir + L"\\" + f + suffix;
            if (Exists(p) && !DeleteFileW(p.c_str())) ++left;
        }
    return left;
}

bool InstallUpdate(const UpdateCheck& c, const std::wstring& exePath, UpdateProgress* progress, std::atomic<bool>* cancel, std::wstring& err) {
    auto step = [&](UpdateStep s) {
        if (progress) progress->step = static_cast<int>(s);
    };
    if (c.status != UpdateStatus::Available) {
        err = Tr(L"there is no update to install");
        return false;
    }
    PackageManager pm = PackageManagerFor(exePath);
    if (pm != PackageManager::None) {
        err = TrF(L"Bytopsy was installed with {0}: update it with  {1}", PackageManagerName(pm), PackageManagerCommand(pm));
        return false;
    }
    // Checked again here: nothing but a release download (or, in the test mode, a local file) is installed.
    if (c.local ? !IsLocalTestUrl(c.asset.url) : !IsReleaseAssetUrl(c.asset.url)) {
        err = TrF(L"the download isn't one of Bytopsy's GitHub releases: {0}", c.asset.url);
        return false;
    }
    if (!c.asset.size || c.asset.size > kMaxZipBytes || !IsHex64(c.asset.sha256)) {
        err = Tr(L"the update information is incomplete");
        return false;
    }
    std::wstring exeDir = DirOf(exePath);
    if (!FolderWritable(exeDir)) {
        err = TrF(L"Bytopsy can't write to {0} (administrator rights needed?)", exeDir);
        return false;
    }
    std::wstring work = TempFolder() + L"\\bytopsy-update-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(work.c_str(), nullptr)) {
        err = TrF(L"{0}: {1}", work, util::Win32ErrorMessage(GetLastError()));
        return false;
    }
    struct Cleanup {
        std::wstring dir;
        ~Cleanup() { RemoveTree(dir); }
    } cleanup{work};

    // Download, then check it against the signed size and hash.
    step(UpdateStep::Download);
    if (progress) progress->total = c.asset.size;
    std::wstring zip = work + L"\\update.zip";
    if (!HttpGetFile(c.asset.url, zip, c.asset.size, c.local, err, cancel, [&](uint64_t done, uint64_t) {
            if (progress) progress->done = done;
        }))
        return false;
    step(UpdateStep::Verify);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    std::string hash;
    if (!GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fa) ||
        ((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) != c.asset.size) {
        err = Tr(L"the download doesn't have the size the update information gives");
        return false;
    }
    if (!Sha256File(zip, hash) || hash != c.asset.sha256) {
        err = Tr(L"the download doesn't match the signed update information (SHA-256)");
        return false;
    }
    if (cancel && cancel->load()) {
        err = Tr(L"Cancelled");
        return false;
    }
    // Unpack and look at what came out.
    step(UpdateStep::Unpack);
    std::wstring files = work + L"\\files", folder;
    std::vector<std::wstring> names;
    if (!ExtractZip(zip, files, err) || !FindReleaseFiles(files, folder, names, err)) return false;
    std::vector<std::wstring> exes;
    for (auto& n : names)
        if (EndsWithI(n, L".exe")) exes.push_back(folder + L"\\" + n);
    if (!CheckSamePublisher(exePath, exes, err)) return false;
    // From here on it can't be cancelled: InstallFiles either completes or puts everything back.
    step(UpdateStep::Install);
    if (!InstallFiles(exeDir, folder, names, err)) return false;
    std::wstring list;
    for (auto& n : names) list += (list.empty() ? L"" : L", ") + n;
    JournalEntry je;
    je.action = JournalAction::Clean;
    je.path = L"Bytopsy update: " BYTOPSY_VERSION L" -> " + c.manifest.version;
    je.bytes = c.asset.size;
    je.items = names.size();
    je.note = L"replaced " + list + L" in " + exeDir + L"; the previous files stay as .old until the next start";
    JournalAppend(je);
    step(UpdateStep::Done);
    return true;
}
