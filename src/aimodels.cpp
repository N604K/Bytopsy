#include "aimodels.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "fsops.h"
#include "journal.h"
#include "tree.h"
#include "util.h"

namespace {

int64_t FtToI(const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::wstring Env(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n && n < 32768 ? std::wstring(buf, n) : L"";
}

std::wstring Home() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    return v.substr(a, v.find_last_not_of(L" \t\r\n") - a + 1);
}

std::wstring StripSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    return p;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToLongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The entries of a folder ("." and ".." left out).
std::vector<WIN32_FIND_DATAW> List(const std::wstring& dir) {
    std::vector<WIN32_FIND_DATAW> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) out.push_back(fd);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool IsSubdir(const WIN32_FIND_DATAW& fd) {
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

uint64_t FileSize(const WIN32_FIND_DATAW& fd) { return (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; }

// Visits every file below `dir` (links are reported, not followed).
void Walk(const std::wstring& dir, const std::function<void(const std::wstring&, const WIN32_FIND_DATAW&)>& f) {
    std::vector<std::wstring> stack{dir};
    while (!stack.empty()) {
        std::wstring d = stack.back();
        stack.pop_back();
        for (auto& fd : List(d)) {
            std::wstring p = d + L"\\" + fd.cFileName;
            if (IsSubdir(fd))
                stack.push_back(p);
            else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                f(p, fd);
        }
    }
}

uint64_t SizeOfPath(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(ToLongPath(p).c_str(), GetFileExInfoStandard, &a)) return 0;
    if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    uint64_t n = 0;
    Walk(p, [&](const std::wstring&, const WIN32_FIND_DATAW& fd) { n += FileSize(fd); });
    return n;
}

int64_t CreationTime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(ToLongPath(p).c_str(), GetFileExInfoStandard, &a) ? FtToI(a.ftCreationTime) : 0;
}

std::string ReadSmall(const std::wstring& path, size_t max = 1 << 20) {
    FILE* f = nullptr;
    std::string data;
    if (_wfopen_s(&f, ToLongPath(path).c_str(), L"rb") != 0 || !f) return data;
    char buf[8192];
    size_t n;
    while (data.size() < max && (n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return data;
}

// The string value of "key" in a JSON text (first occurrence; \" \\ \/ and \uXXXX unescaped).
std::wstring JsonString(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k);
    if (at == std::string::npos) return L"";
    at = json.find_first_not_of(" \t\r\n", at + k.size());
    if (at == std::string::npos || json[at] != ':') return L"";
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || json[at] != '"') return L"";
    std::string out;
    for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            out += json[i];
            continue;
        }
        char e = json[++i];
        if (e == 'u' && i + 4 < json.size()) {
            unsigned cp = std::stoul(json.substr(i + 1, 4), nullptr, 16);
            i += 4;
            out += util::WideToUtf8(std::wstring(1, static_cast<wchar_t>(cp)));
        } else {
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
    }
    return util::Utf8ToWide(out);
}

// "sha256:<64 hex>" digests in the order they appear.
std::vector<std::string> Digests(const std::string& json) {
    std::vector<std::string> out;
    for (size_t at = json.find("sha256:"); at != std::string::npos; at = json.find("sha256:", at + 7)) {
        std::string hex = json.substr(at + 7, 64);
        if (hex.size() == 64 && hex.find_first_not_of("0123456789abcdef") == std::string::npos) out.push_back(hex);
    }
    return out;
}

// Where a link (a symlinked snapshot file) leads, without "\\?\".
std::wstring LinkTarget(const std::wstring& p) {
    HANDLE h = CreateFileW(ToLongPath(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    wchar_t buf[32768];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= 32768) return L"";
    std::wstring s(buf, n);
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + s.substr(8);
    if (s.rfind(L"\\\\?\\", 0) == 0) return s.substr(4);
    return s;
}

std::wstring Lower(const std::wstring& s) { return util::ToLower(s); }

// The absolute path with "." and ".." resolved (so "models\..\chats" can't pass for a model).
std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(StripSlash(p).c_str(), 32768, buf, nullptr);
    return n && n < 32768 ? StripSlash(std::wstring(buf, n)) : L"";
}

bool Inside(const std::vector<std::wstring>& roots, const std::wstring& path) {
    std::wstring p = Lower(FullPath(path));
    if (p.empty()) return false;
    for (auto& r : roots) {
        std::wstring l = Lower(FullPath(r));
        if (!l.empty() && p.size() > l.size() + 1 && p.compare(0, l.size(), l) == 0 && p[l.size()] == L'\\') return true;
    }
    return false;
}

// Top-level items by size; each repository's revisions right after it, also by size.
void SortItems(AiStore& s) {
    auto bySize = [&](size_t a, size_t b) { return s.items[a].bytes > s.items[b].bytes; };
    std::vector<size_t> tops;
    for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].parent < 0) tops.push_back(i);
    std::stable_sort(tops.begin(), tops.end(), bySize);
    std::vector<AiItem> out;
    for (size_t t : tops) {
        int parent = static_cast<int>(out.size());
        out.push_back(s.items[t]);
        std::vector<size_t> kids;
        for (size_t j = 0; j < s.items.size(); ++j)
            if (s.items[j].parent == static_cast<int>(t)) kids.push_back(j);
        std::stable_sort(kids.begin(), kids.end(), bySize);
        for (size_t k : kids) {
            out.push_back(s.items[k]);
            out.back().parent = parent;
        }
    }
    s.items = std::move(out);
}

}  // namespace

bool LastAccessTracked() {
    DWORD v = 0, n = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"NtfsDisableLastAccessUpdate", RRF_RT_REG_DWORD,
                     nullptr, &v, &n) != ERROR_SUCCESS)
        return false;
    return (v & 1) == 0;  // 0x80000002 "system managed, enabled", 0x80000003 "... disabled", legacy 0 / 1
}

// ---------------------------------------------------------------------------------------------
// Hugging Face: hub\models--org--name\{blobs,refs,snapshots\<commit>}

AiStore ScanHuggingFace(const std::wstring& hubDir, const std::wstring& xetDir) {
    AiStore s;
    s.kind = AiStoreKind::HuggingFace;
    s.name = L"Hugging Face cache";
    std::wstring hub = StripSlash(hubDir);
    s.folder = hub;
    s.roots = {hub};
    if (!xetDir.empty()) s.roots.push_back(StripSlash(xetDir));
    bool atime = LastAccessTracked();
    for (auto& rd : List(hub)) {
        if (!IsSubdir(rd)) continue;
        std::wstring dirName = rd.cFileName;
        size_t sep = dirName.find(L"--");
        if (sep == std::wstring::npos) continue;
        std::wstring type = dirName.substr(0, sep);  // models, datasets, spaces
        if (type != L"models" && type != L"datasets" && type != L"spaces") continue;
        std::wstring id = dirName.substr(sep + 2);
        for (size_t at = id.find(L"--"); at != std::wstring::npos; at = id.find(L"--", at + 1)) id.replace(at, 2, L"/");
        std::wstring repo = hub + L"\\" + dirName;

        // refs\main, refs\refs\pr\10, ... -> commit
        std::map<std::wstring, std::vector<std::wstring>> refsOf;      // commit -> ref names
        std::map<std::wstring, std::vector<std::wstring>> refFilesOf;  // commit -> ref files
        std::map<std::wstring, std::wstring> revisionOf;                // commit -> what --revision takes ("refs/pr/10")
        std::wstring refs = repo + L"\\refs";
        Walk(refs, [&](const std::wstring& p, const WIN32_FIND_DATAW&) {
            std::wstring commit = Lower(Trim(util::Utf8ToWide(ReadSmall(p, 256))));
            std::wstring name = p.substr(refs.size() + 1);
            for (auto& c : name)
                if (c == L'\\') c = L'/';
            revisionOf.emplace(commit, name);
            if (name.rfind(L"refs/", 0) == 0) name = name.substr(5);
            refsOf[commit].push_back(name);
            refFilesOf[commit].push_back(p);
        });
        // Blob sizes, and which revisions use each blob.
        std::unordered_map<std::wstring, uint64_t> blobSize;
        for (auto& b : List(repo + L"\\blobs"))
            if (!(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) blobSize[Lower(repo + L"\\blobs\\" + b.cFileName)] = FileSize(b);
        struct Rev {
            std::wstring commit, dir;
            uint64_t own = 0;  // real files (Windows without symlinks keeps copies in the snapshot)
            std::set<std::wstring> blobs;
            int64_t used = 0;
        };
        std::vector<Rev> revs;
        std::unordered_map<std::wstring, int> blobUsers;
        for (auto& sd : List(repo + L"\\snapshots")) {
            if (!IsSubdir(sd)) continue;
            Rev r;
            r.commit = Lower(sd.cFileName);
            r.dir = repo + L"\\snapshots\\" + sd.cFileName;
            Walk(r.dir, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    std::wstring t = Lower(LinkTarget(p));
                    if (!t.empty() && blobSize.count(t)) r.blobs.insert(t);
                } else {
                    r.own += FileSize(fd);
                    if (atime) r.used = std::max(r.used, FtToI(fd.ftLastAccessTime));
                }
            });
            for (auto& b : r.blobs) ++blobUsers[b];
            revs.push_back(std::move(r));
        }
        AiItem repoItem;
        repoItem.name = id;
        repoItem.location = repo;
        repoItem.remove = {repo};
        repoItem.bytes = SizeOfPath(repo);
        std::wstring typeArg = type == L"datasets" ? L" --repo-type dataset" : type == L"spaces" ? L" --repo-type space" : L"";
        repoItem.restore = L"huggingface-cli download " + id + typeArg;
        std::wstring kind = type == L"models" ? L"model" : type == L"datasets" ? L"dataset" : L"space";
        if (revs.size() == 1) {
            auto it = refsOf.find(revs[0].commit);
            repoItem.detail = kind + (it != refsOf.end() ? L"  \u00B7  " + it->second[0] : L"");
        } else {
            repoItem.detail = kind + L"  \u00B7  " + std::to_wstring(revs.size()) + L" revisions";
        }
        int repoIndex = static_cast<int>(s.items.size());
        std::vector<AiItem> revItems;
        for (auto& r : revs) {
            int64_t added = CreationTime(r.dir);
            repoItem.added = repoItem.added ? std::min(repoItem.added, added) : added;
            uint64_t blobsUsed = 0;
            for (auto& b : r.blobs) blobsUsed += blobSize[b];
            int64_t used = r.used;
            if (atime)
                for (auto& b : r.blobs) {
                    WIN32_FILE_ATTRIBUTE_DATA a;
                    if (GetFileAttributesExW(ToLongPath(b).c_str(), GetFileExInfoStandard, &a)) used = std::max(used, FtToI(a.ftLastAccessTime));
                }
            repoItem.used = std::max(repoItem.used, used);
            if (revs.size() < 2) continue;
            AiItem ri;
            ri.parent = repoIndex;
            ri.name = L"revision " + r.commit.substr(0, 7);
            auto refIt = refsOf.find(r.commit);
            ri.unused = refIt == refsOf.end();
            if (ri.unused) {
                ri.detail = L"not referenced by any branch, tag or PR";
            } else {
                for (auto& n : refIt->second) ri.detail += (ri.detail.empty() ? L"" : L", ") + n;
            }
            ri.location = r.dir;
            ri.added = added;
            ri.used = used;
            ri.bytes = r.own;
            ri.remove = {r.dir, repo + L"\\.no_exist\\" + r.commit};
            for (auto& b : r.blobs)
                if (blobUsers[b] == 1) {
                    ri.bytes += blobSize[b];
                    ri.remove.push_back(b);
                }
            if (!ri.unused)
                for (auto& f : refFilesOf[r.commit]) ri.remove.push_back(f);
            ri.restore = repoItem.restore + L" --revision " + (ri.unused ? r.commit : revisionOf[r.commit]);
            revItems.push_back(std::move(ri));
        }
        s.items.push_back(std::move(repoItem));
        for (auto& r : revItems) s.items.push_back(std::move(r));
    }
    if (!xetDir.empty() && IsDir(xetDir)) {
        AiItem x;
        x.name = L"Xet download cache";
        x.detail = L"chunks kept to speed up later downloads";
        x.restore = L"filled again by the next download";
        x.location = StripSlash(xetDir);
        for (auto& c : List(x.location)) x.remove.push_back(x.location + L"\\" + c.cFileName);
        x.bytes = SizeOfPath(x.location);
        x.added = CreationTime(x.location);
        if (x.bytes) s.items.push_back(std::move(x));
    }
    for (auto& it : s.items)
        if (it.parent < 0) s.bytes += it.bytes;
    SortItems(s);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Ollama: models\manifests\<registry>\<namespace>\<model>\<tag> (JSON) + models\blobs\sha256-<hex>

AiStore ScanOllama(const std::wstring& modelsDir) {
    AiStore s;
    s.kind = AiStoreKind::Ollama;
    s.name = L"Ollama models";
    std::wstring models = StripSlash(modelsDir);
    s.folder = models;
    s.roots = {models + L"\\manifests", models + L"\\blobs"};
    bool atime = LastAccessTracked();
    std::wstring manifests = models + L"\\manifests";
    struct M {
        std::wstring path, name;
        std::vector<std::wstring> blobs;
        std::string config;
        int64_t added;
    };
    std::vector<M> ms;
    std::unordered_map<std::wstring, int> users;
    Walk(manifests, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
        std::string json = ReadSmall(p, 256 * 1024);
        std::vector<std::string> d = Digests(json);
        if (d.empty()) return;
        M m;
        m.path = p;
        m.added = FtToI(fd.ftLastWriteTime);
        std::vector<std::wstring> parts;
        std::wstring rel = p.substr(manifests.size() + 1);
        for (size_t i = 0; i <= rel.size();) {
            size_t j = rel.find(L'\\', i);
            if (j == std::wstring::npos) j = rel.size();
            parts.push_back(rel.substr(i, j - i));
            i = j + 1;
        }
        if (parts.size() == 4 && Lower(parts[0]) == L"registry.ollama.ai")
            m.name = (Lower(parts[1]) == L"library" ? L"" : parts[1] + L"/") + parts[2] + L":" + parts[3];
        else if (parts.size() >= 2)
            m.name = rel.substr(0, rel.rfind(L'\\')) + L":" + parts.back();
        else
            m.name = rel;
        for (auto& c : m.name)
            if (c == L'\\') c = L'/';
        std::set<std::wstring> mine;
        for (auto& h : d) mine.insert(models + L"\\blobs\\sha256-" + util::Utf8ToWide(h));
        m.blobs.assign(mine.begin(), mine.end());
        for (auto& b : mine) ++users[Lower(b)];
        // The config blob (named right after "config") says family, size and quantization.
        size_t cfg = json.find("\"config\"");
        if (cfg != std::string::npos) {
            std::vector<std::string> cd = Digests(json.substr(cfg, 400));
            if (!cd.empty()) m.config = ReadSmall(models + L"\\blobs\\sha256-" + util::Utf8ToWide(cd[0]), 64 * 1024);
        }
        ms.push_back(std::move(m));
    });
    for (auto& m : ms) {
        AiItem it;
        it.name = m.name;
        it.location = m.path;
        it.remove = {m.path};
        it.blobs = m.blobs;
        it.added = m.added;
        it.restore = L"ollama pull " + m.name;
        uint64_t largest = 0;
        for (auto& b : m.blobs) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (!GetFileAttributesExW(ToLongPath(b).c_str(), GetFileExInfoStandard, &a)) continue;
            uint64_t size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
            if (users[Lower(b)] == 1) it.bytes += size;
            if (size > largest) {
                largest = size;
                if (atime) it.used = FtToI(a.ftLastAccessTime);
            }
        }
        for (const char* k : {"model_family", "model_type", "file_type"}) {
            std::wstring v = JsonString(m.config, k);
            if (!v.empty()) it.detail += (it.detail.empty() ? L"" : L"  \u00B7  ") + v;
        }
        if (it.detail.empty()) it.detail = std::to_wstring(m.blobs.size()) + L" layers";
        if (it.bytes < largest) it.detail += L"  \u00B7  shares layers with other models";
        s.items.push_back(std::move(it));
    }
    s.bytes = SizeOfPath(models + L"\\blobs") + SizeOfPath(manifests);
    SortItems(s);
    return s;
}

// ---------------------------------------------------------------------------------------------
// LM Studio: models\<publisher>\<repository>\<files>

AiStore ScanLMStudio(const std::wstring& modelsDir) {
    AiStore s;
    s.kind = AiStoreKind::LMStudio;
    s.name = L"LM Studio models";
    std::wstring models = StripSlash(modelsDir);
    s.folder = models;
    s.roots = {models};
    bool atime = LastAccessTracked();
    for (auto& pub : List(models)) {
        if (!IsSubdir(pub)) continue;
        std::wstring pubDir = models + L"\\" + pub.cFileName;
        for (auto& rep : List(pubDir)) {
            if (!IsSubdir(rep)) continue;
            AiItem it;
            it.name = std::wstring(pub.cFileName) + L"/" + rep.cFileName;
            it.location = pubDir + L"\\" + rep.cFileName;
            it.remove = {it.location};
            it.added = FtToI(rep.ftCreationTime);
            it.restore = L"download it again in LM Studio (Discover)";
            size_t files = 0;
            uint64_t largest = 0;
            std::wstring largestName;
            Walk(it.location, [&](const std::wstring& p, const WIN32_FIND_DATAW& fd) {
                ++files;
                it.bytes += FileSize(fd);
                if (FileSize(fd) > largest) {
                    largest = FileSize(fd);
                    largestName = p.substr(p.rfind(L'\\') + 1);
                    if (atime) it.used = FtToI(fd.ftLastAccessTime);
                }
            });
            if (!files) continue;
            it.detail = files == 1 ? largestName : largestName + L" and " + std::to_wstring(files - 1) + L" more";
            s.bytes += it.bytes;
            s.items.push_back(std::move(it));
        }
    }
    SortItems(s);
    return s;
}

std::vector<AiStore> FindAiModels() {
    std::vector<AiStore> out;
    std::wstring home = Home();
    // Hugging Face: HF_HUB_CACHE, else HF_HOME\hub, else ~\.cache\huggingface\hub.
    std::wstring hfHome = Env(L"HF_HOME");
    if (hfHome.empty()) {
        std::wstring xdg = Env(L"XDG_CACHE_HOME");
        hfHome = (xdg.empty() ? home + L"\\.cache" : xdg) + L"\\huggingface";
    }
    std::wstring hub = Env(L"HF_HUB_CACHE");
    if (hub.empty()) hub = Env(L"HUGGINGFACE_HUB_CACHE");
    if (hub.empty()) hub = hfHome + L"\\hub";
    std::wstring xet = Env(L"HF_XET_CACHE");
    if (xet.empty()) xet = hfHome + L"\\xet";
    if (IsDir(hub)) out.push_back(ScanHuggingFace(hub, xet));
    // Ollama: OLLAMA_MODELS, else ~\.ollama\models.
    std::wstring ollama = Env(L"OLLAMA_MODELS");
    if (ollama.empty()) ollama = home + L"\\.ollama\\models";
    if (IsDir(ollama)) out.push_back(ScanOllama(ollama));
    // LM Studio: its home (~\.lmstudio, or where ~\.lmstudio-home-pointer says), the downloads
    // folder from its settings, and the older ~\.cache\lm-studio.
    std::wstring lmHome = StripSlash(Trim(util::Utf8ToWide(ReadSmall(home + L"\\.lmstudio-home-pointer", 4096))));
    if (lmHome.empty() || !IsDir(lmHome)) lmHome = home + L"\\.lmstudio";
    std::wstring lmModels = JsonString(ReadSmall(lmHome + L"\\settings.json"), "downloadsFolder");
    if (lmModels.empty() || !IsDir(lmModels)) lmModels = lmHome + L"\\models";
    std::vector<std::wstring> lm = {StripSlash(lmModels), home + L"\\.cache\\lm-studio\\models"};
    if (Lower(lm[0]) == Lower(lm[1])) lm.pop_back();
    for (auto& d : lm)
        if (IsDir(d)) {
            out.push_back(ScanLMStudio(d));
            if (out.size() > 1 && out[out.size() - 2].kind == AiStoreKind::LMStudio) out.back().name = L"LM Studio models (older folder)";
        }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Deleting

AiDeleteResult DeleteAiItems(const AiStore& store, const std::vector<size_t>& chosenIn, std::atomic<bool>& cancel) {
    AiDeleteResult res;
    std::set<size_t> chosen;
    for (size_t i : chosenIn)
        if (i < store.items.size()) chosen.insert(i);
    // A repository with every revision chosen goes as a whole; revisions of a chosen repository
    // are part of it.
    std::map<int, std::pair<size_t, size_t>> revs;  // repository -> (revisions, chosen revisions)
    for (size_t i = 0; i < store.items.size(); ++i)
        if (store.items[i].parent >= 0) {
            auto& r = revs[store.items[i].parent];
            ++r.first;
            if (chosen.count(i)) ++r.second;
        }
    for (auto& [repo, n] : revs)
        if (n.first && n.first == n.second) chosen.insert(static_cast<size_t>(repo));
    std::vector<size_t> list;
    for (size_t i : chosen)
        if (store.items[i].parent < 0 || !chosen.count(static_cast<size_t>(store.items[i].parent))) list.push_back(i);

    std::vector<uint64_t> freedBy(store.items.size(), 0);
    std::vector<std::wstring> notes(store.items.size());
    auto remove = [&](std::wstring p, size_t item) {
        if (!Inside(store.roots, p)) {  // never anything outside the model folders
            res.errors.push_back(L"Refused (outside the model folders): " + p);
            notes[item] = L"partly deleted";
            return;
        }
        p = FullPath(p);
        if (!PathExists(p)) return;
        uint64_t before = SizeOfPath(p);
        std::atomic<uint64_t> removed{0};
        std::wstring err;
        bool ok = DeletePermanently(p, cancel, removed, err);
        uint64_t after = ok ? 0 : SizeOfPath(p);
        freedBy[item] += before > after ? before - after : 0;
        if (!ok) {
            notes[item] = L"partly deleted: " + err;
            res.errors.push_back(p + L": " + err);
            for (auto& a : AppsUsing(p, 200))
                if (std::find(res.lockedBy.begin(), res.lockedBy.end(), a.name) == res.lockedBy.end()) res.lockedBy.push_back(a.name);
        }
    };
    for (size_t i : list) {
        if (cancel) break;
        for (auto& p : store.items[i].remove) remove(p, i);
        if (store.kind == AiStoreKind::Ollama) {
            // Empty model folders under manifests go too, as `ollama rm` leaves none.
            std::wstring d = store.items[i].location.substr(0, store.items[i].location.rfind(L'\\'));
            while (Inside(store.roots, d) && RemoveDirectoryW(ToLongPath(d).c_str())) d = d.substr(0, d.rfind(L'\\'));
        }
    }
    if (store.kind == AiStoreKind::Ollama && !cancel) {
        // Layers no remaining model uses (never ones that weren't used by a deleted model).
        AiStore now = ScanOllama(store.folder);
        std::set<std::wstring> still;
        for (auto& it : now.items)
            for (auto& b : it.blobs) still.insert(Lower(b));
        std::set<std::wstring> done;
        for (size_t i : list)
            for (auto& b : store.items[i].blobs)
                if (!still.count(Lower(b)) && done.insert(Lower(b)).second) remove(b, i);
    }
    for (size_t i : list) {
        res.freed += freedBy[i];
        if (notes[i].empty() || freedBy[i]) ++res.deleted;
        const AiItem& it = store.items[i];
        JournalEntry je;
        je.action = JournalAction::Delete;
        je.bytes = freedBy[i];
        je.items = 1;
        je.path = it.location;
        je.note = store.name + L": " + it.name + L"; get it back: " + it.restore + (notes[i].empty() ? L"" : L"; " + notes[i]);
        JournalAppend(je);
    }
    return res;
}
