#include "export.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "i18n.h"
#include "util.h"
#include "version.h"

namespace {

class Out {
public:
    explicit Out(FILE* f) : f_(f) { buf_.reserve(1 << 20); }
    ~Out() { Flush(); }
    void Raw(std::string_view s) {
        buf_.append(s);
        if (buf_.size() > (1 << 20)) Flush();
    }
    void Ch(char c) { buf_.push_back(c); }
    void Num(uint64_t v) { Raw(std::to_string(v)); }
    void SNum(int64_t v) { Raw(std::to_string(v)); }
    void JsonStr(const std::wstring& s) {
        std::string u = util::WideToUtf8(s);
        Ch('"');
        for (unsigned char c : u) {
            switch (c) {
                case '"': Raw("\\\""); break;
                case '\\': Raw("\\\\"); break;
                case '\n': Raw("\\n"); break;
                case '\r': Raw("\\r"); break;
                case '\t': Raw("\\t"); break;
                default:
                    if (c < 0x20) {
                        char tmp[8];
                        snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                        Raw(tmp);
                    } else {
                        Ch(static_cast<char>(c));
                    }
            }
        }
        Ch('"');
    }
    void CsvStr(const std::wstring& s) {
        std::string u = util::WideToUtf8(s);
        Ch('"');
        for (char c : u) {
            if (c == '"') Ch('"');
            Ch(c);
        }
        Ch('"');
    }
    bool Flush() {
        if (!buf_.empty()) {
            if (fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) failed_ = true;
            buf_.clear();
        }
        return !failed_;
    }
    bool Failed() const { return failed_; }

private:
    FILE* f_;
    std::string buf_;
    bool failed_ = false;
};

void WriteInfo(Out& o, const Node* n, bool top) {
    o.Raw("{\"name\":");
    o.JsonStr(top ? NodePath(n) : n->name);  // ncdu names the top folder by its full path
    if (!n->IsDir() || n->Has(NF_LINK | NF_EXCLUDED)) {
        uint64_t as = n->Has(NF_HARDLINK) ? 0 : n->size;
        uint64_t ds = n->Has(NF_HARDLINK) ? 0 : n->disk;
        if (as) {
            o.Raw(",\"asize\":");
            o.Num(as);
        }
        if (ds) {
            o.Raw(",\"dsize\":");
            o.Num(ds);
        }
    }
    if (n->mtime > 0) {
        o.Raw(",\"mtime\":");
        o.SNum(util::FileTimeToUnix(n->mtime));
    }
    if (n->Has(NF_ERROR)) o.Raw(",\"read_error\":true");
    if (n->Has(NF_EXCLUDED)) o.Raw(",\"excluded\":\"pattern\"");
    if (n->Has(NF_LINK)) o.Raw(",\"notreg\":true");
    o.Ch('}');
}

// The top folder is always written as a folder: an export that doesn't start with one can't be read.
void WriteNode(Out& o, const Node* n, std::atomic<uint64_t>& count, bool top = false) {
    ++count;
    if (n->IsDir() && (top || !n->Has(NF_LINK | NF_EXCLUDED))) {
        o.Ch('[');
        WriteInfo(o, n, top);
        for (const auto& c : n->children) {
            o.Raw(",\n");
            WriteNode(o, c.get(), count);
        }
        o.Ch(']');
    } else {
        WriteInfo(o, n, top);
    }
}

FILE* OpenOut(const std::wstring& file, std::wstring& err) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) {
        err = TrF(L"Cannot create {0}: {1}", file, util::Win32ErrorMessage(GetLastError()));
        return nullptr;
    }
    return f;
}

std::wstring FlagString(const Node* n) {
    std::wstring s;
    auto add = [&](const wchar_t* t) {
        if (!s.empty()) s += L' ';
        s += t;
    };
    if (n->Has(NF_ERROR)) add(L"read_error");
    if (n->Has(NF_LINK)) add(L"link");
    if (n->Has(NF_EXCLUDED)) add(L"excluded");
    if (n->Has(NF_HARDLINK)) add(L"hardlink");
    if (n->Has(NF_CLOUD)) add(L"cloud");
    return s;
}

}  // namespace

bool ExportNcduJson(const Node* root, const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err) {
    FILE* f = OpenOut(file, err);
    if (!f) return false;
    {
        Out o(f);
        o.Raw("[1,2,{\"progname\":\"bytopsy\",\"progver\":\"" BYTOPSY_VERSION_A "\",\"timestamp\":");
        o.SNum(static_cast<int64_t>(time(nullptr)));
        o.Raw("},\n");
        WriteNode(o, root, count, true);
        o.Raw("]\n");
        if (!o.Flush()) err = Tr(L"Write error (disk full?)");
    }
    bool ok = fclose(f) == 0 && err.empty();
    if (!ok && err.empty()) err = Tr(L"Write error");
    return ok;
}

bool ExportCsv(const Node* root, const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err) {
    FILE* f = OpenOut(file, err);
    if (!f) return false;
    {
        Out o(f);
        o.Raw("\xEF\xBB\xBF");
        o.Raw("Path,Type,Size,DiskUsage,Items,Modified,Flags\r\n");
        std::vector<const Node*> stack{root};
        while (!stack.empty()) {
            const Node* n = stack.back();
            stack.pop_back();
            ++count;
            o.CsvStr(NodePath(n));
            o.Raw(n->IsDir() ? ",dir," : ",file,");
            o.Num(n->size);
            o.Ch(',');
            o.Num(n->disk);
            o.Ch(',');
            o.Num(n->items);
            o.Ch(',');
            o.CsvStr(n->mtime > 0 ? util::FormatTime(n->mtime) : L"");
            o.Ch(',');
            o.CsvStr(FlagString(n));
            o.Raw("\r\n");
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(it->get());
        }
        if (!o.Flush()) err = Tr(L"Write error (disk full?)");
    }
    bool ok = fclose(f) == 0 && err.empty();
    if (!ok && err.empty()) err = Tr(L"Write error");
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Import

namespace {

class Parser {
public:
    Parser(const char* p, const char* end, std::atomic<uint64_t>& count) : p_(p), begin_(p), end_(end), count_(count) {}

    std::unique_ptr<Node> ParseRoot() {
        Ws();
        if (!Expect('[')) return nullptr;
        double major = 0;
        if (!Number(major)) return nullptr;
        if (major != 1) return Fail(L"Unsupported ncdu export major version");
        if (!Comma()) return nullptr;
        double minor;
        if (!Number(minor)) return nullptr;
        if (!Comma()) return nullptr;
        if (!SkipValue()) return nullptr;  // metadata
        if (!Comma()) return nullptr;
        Ws();
        auto root = std::make_unique<Node>();
        if (Peek() != '[') return Fail(L"Expected root directory");
        if (!ParseDir(root.get())) return nullptr;
        return root;
    }

    std::wstring Error() const {
        size_t off = static_cast<size_t>(p_ - begin_);
        return err_ + L" (at byte " + std::to_wstring(off) + L")";
    }

private:
    struct Info {
        bool excluded = false, notreg = false, readError = false, hlnk = false;
        uint64_t ino = 0, dev = 0;
    };

    // Parsing recurses per level; this bounds the stack (a Windows path has at most ~16K levels).
    static constexpr int kMaxDepth = 16384;
    struct Nest {
        explicit Nest(int& d) : d_(d) { ++d_; }
        ~Nest() { --d_; }
        int& d_;
    };

    std::nullptr_t Fail(const std::wstring& msg) {
        if (err_.empty()) err_ = msg;
        return nullptr;
    }
    bool FailB(const std::wstring& msg) {
        Fail(msg);
        return false;
    }
    void Ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    char Peek() {
        Ws();
        return p_ < end_ ? *p_ : '\0';
    }
    bool Expect(char c) {
        Ws();
        if (p_ < end_ && *p_ == c) {
            ++p_;
            return true;
        }
        return FailB(std::wstring(L"Expected '") + static_cast<wchar_t>(c) + L"'");
    }
    bool Comma() { return Expect(','); }

    bool Number(double& v) {
        Ws();
        const char* s = p_;
        while (p_ < end_ && (isdigit(static_cast<unsigned char>(*p_)) || *p_ == '-' || *p_ == '+' || *p_ == '.' ||
                             *p_ == 'e' || *p_ == 'E'))
            ++p_;
        if (s == p_) return FailB(L"Expected number");
        v = strtod(std::string(s, p_).c_str(), nullptr);
        return true;
    }
    bool Uint(uint64_t& v) {
        double d;
        Ws();
        const char* s = p_;
        bool simple = true;
        while (p_ < end_ && isdigit(static_cast<unsigned char>(*p_))) ++p_;
        if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-')) simple = false;
        if (simple && p_ != s) {
            v = strtoull(std::string(s, p_).c_str(), nullptr, 10);
            return true;
        }
        p_ = s;
        if (!Number(d)) return false;
        v = !(d > 0) ? 0 : d >= 18446744073709551616.0 ? UINT64_MAX : static_cast<uint64_t>(d);
        return true;
    }

    bool String(std::string& out) {
        out.clear();
        if (!Expect('"')) return false;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= end_) break;
            char e = *p_++;
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (end_ - p_ < 4) return FailB(L"Bad escape");
                    unsigned cp = static_cast<unsigned>(strtoul(std::string(p_, p_ + 4).c_str(), nullptr, 16));
                    p_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        unsigned lo = static_cast<unsigned>(strtoul(std::string(p_ + 2, p_ + 6).c_str(), nullptr, 16));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p_ += 6;
                        }
                    }
                    // Encode as UTF-8.
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: out.push_back(e); break;
            }
        }
        if (p_ >= end_) return FailB(L"Unterminated string");
        ++p_;
        return true;
    }

    bool Literal(bool& v) {
        Ws();
        auto match = [&](const char* lit) {
            size_t n = strlen(lit);
            if (static_cast<size_t>(end_ - p_) >= n && memcmp(p_, lit, n) == 0) {
                p_ += n;
                return true;
            }
            return false;
        };
        if (match("true")) {
            v = true;
            return true;
        }
        if (match("false") || match("null")) {
            v = false;
            return true;
        }
        return FailB(L"Expected literal");
    }

    bool SkipValue() {
        char c = Peek();
        if (c == '"') {
            std::string s;
            return String(s);
        }
        if (c == '{' || c == '[') {
            if (depth_ >= kMaxDepth) return FailB(L"Nested too deeply");
            Nest nest(depth_);
            char close = c == '{' ? '}' : ']';
            ++p_;
            if (Peek() == close) {
                ++p_;
                return true;
            }
            for (;;) {
                if (c == '{') {
                    std::string k;
                    if (!String(k) || !Expect(':')) return false;
                }
                if (!SkipValue()) return false;
                char n = Peek();
                if (n == ',') {
                    ++p_;
                    continue;
                }
                return Expect(close);
            }
        }
        if (c == 't' || c == 'f' || c == 'n') {
            bool b;
            return Literal(b);
        }
        double d;
        return Number(d);
    }

    bool ParseInfo(Node* n, Info& info) {
        if (!Expect('{')) return false;
        std::string key, sval;
        if (Peek() == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            if (!String(key) || !Expect(':')) return false;
            if (key == "name") {
                if (!String(sval)) return false;
                n->name = util::Utf8ToWide(sval);
            } else if (key == "asize") {
                if (!Uint(n->size)) return false;
            } else if (key == "dsize") {
                if (!Uint(n->disk)) return false;
            } else if (key == "mtime") {
                uint64_t t;
                if (!Uint(t)) return false;
                // Times past what a FILETIME can hold are left out.
                if (t <= static_cast<uint64_t>((INT64_MAX - util::UnixToFileTime(0)) / 10000000))
                    n->mtime = util::UnixToFileTime(static_cast<int64_t>(t));
            } else if (key == "ino") {
                if (!Uint(info.ino)) return false;
            } else if (key == "dev") {
                if (!Uint(info.dev)) return false;
            } else if (key == "read_error" || key == "notreg" || key == "hlnkc") {
                bool b;
                if (!Literal(b)) return false;
                if (key == "read_error") info.readError = b;
                else if (key == "notreg") info.notreg = b;
                else info.hlnk = b;
            } else if (key == "excluded") {
                if (Peek() == '"') {
                    if (!String(sval)) return false;
                    info.excluded = true;
                } else {
                    bool b;
                    if (!Literal(b)) return false;
                    info.excluded = b;
                }
            } else {
                if (!SkipValue()) return false;
            }
            char c = Peek();
            if (c == ',') {
                ++p_;
                continue;
            }
            return Expect('}');
        }
    }

    void ApplyInfo(Node* n, const Info& info) {
        if (info.readError) n->flags |= NF_ERROR;
        if (info.excluded) n->flags |= NF_EXCLUDED;
        if (info.notreg) n->flags |= NF_LINK;
        if (info.hlnk && info.ino) {
            if (!hardlinks_.insert({info.dev, info.ino}).second) n->flags |= NF_HARDLINK;
        }
    }

    bool ParseDir(Node* dir) {
        if (depth_ >= kMaxDepth) return FailB(L"Folders nested too deeply");
        Nest nest(depth_);
        if (!Expect('[')) return false;
        Info info;
        if (!ParseInfo(dir, info)) return false;
        dir->flags |= NF_DIR;
        ApplyInfo(dir, info);
        // Directory own sizes are not tracked; totals come from children.
        dir->size = dir->disk = 0;
        ++count_;
        for (;;) {
            char c = Peek();
            if (c == ']') {
                ++p_;
                return true;
            }
            if (c != ',') return FailB(L"Expected ',' or ']'");
            ++p_;
            auto child = std::make_unique<Node>();
            child->parent = dir;
            char k = Peek();
            if (k == '[') {
                if (!ParseDir(child.get())) return false;
            } else if (k == '{') {
                Info ci;
                if (!ParseInfo(child.get(), ci)) return false;
                ApplyInfo(child.get(), ci);
                ++count_;
            } else {
                return FailB(L"Expected item");
            }
            dir->children.push_back(std::move(child));
        }
    }

    const char* p_;
    const char* begin_;
    const char* end_;
    std::atomic<uint64_t>& count_;
    std::wstring err_;
    int depth_ = 0;
    std::set<std::pair<uint64_t, uint64_t>> hardlinks_;
};

}  // namespace

std::unique_ptr<Node> ImportNcduJson(const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = TrF(L"Cannot open {0}: {1}", file, util::Win32ErrorMessage(GetLastError()));
        return nullptr;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string data;
    try {
        data.resize(static_cast<size_t>(sz.QuadPart));
    } catch (...) {
        CloseHandle(h);
        err = Tr(L"File too large to load");
        return nullptr;
    }
    size_t got = 0;
    while (got < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - got, 1u << 24));
        DWORD read = 0;
        if (!ReadFile(h, data.data() + got, chunk, &read, nullptr) || read == 0) break;
        got += read;
    }
    CloseHandle(h);
    data.resize(got);
    if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0x1F && static_cast<unsigned char>(data[1]) == 0x8B) {
        err = Tr(L"Compressed exports are not supported; decompress the file first");
        return nullptr;
    }
    const char* p = data.data();
    if (data.size() >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;
    Parser parser(p, data.data() + data.size(), count);
    auto root = parser.ParseRoot();
    if (!root) {
        err = TrF(L"Not a valid ncdu JSON export: {0}", parser.Error());
        return nullptr;
    }
    RecomputeTotals(root.get());
    return root;
}
