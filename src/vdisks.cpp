#include "vdisks.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "fsops.h"
#include "journal.h"
#include "runtool.h"
#include "tree.h"
#include "i18n.h"
#include "util.h"

namespace {

std::wstring TempDir() {
    wchar_t tmp[MAX_PATH];
    return std::wstring(tmp, GetTempPathW(MAX_PATH, tmp));
}

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find(L'\n', pos);
        std::wstring l = s.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        size_t b = l.find_first_not_of(L" \t");
        if (b != std::wstring::npos) out.push_back(l.substr(b));
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    return out;
}

uint64_t AllocatedSize(const std::wstring& path) {
    DWORD hi = 0;
    DWORD lo = GetCompressedFileSizeW(ToLongPath(path).c_str(), &hi);
    if (lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return 0;
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exe) == 0;
    CloseHandle(snap);
    return found;
}

// A disk is free to compact once nothing has it open (WSL detaches it when it shuts down).
bool InUse(const std::wstring& path) {
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(h);
    return false;
}

std::wstring StripLongPrefix(std::wstring p) {
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

std::wstring RegString(HKEY k, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return L"";
    size_t n = size / sizeof(wchar_t);
    while (n && buf[n - 1] == 0) --n;
    return std::wstring(buf, n);
}

DWORD RegDword(HKEY k, const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) != ERROR_SUCCESS || type != REG_DWORD) return def;
    return v;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : L"";
}

std::string ReadSmall(const std::wstring& path, size_t max = 1 << 20) {
    std::string out;
    HANDLE h = CreateFileW(ToLongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char b[8192];
    DWORD n = 0;
    while (out.size() < max && ReadFile(h, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
    CloseHandle(h);
    return out;
}

// The string value of "key" in a flat JSON object (no nesting needed here).
std::wstring JsonString(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t at = json.find(k);
    if (at == std::string::npos) return L"";
    at = json.find(':', at + k.size());
    if (at == std::string::npos) return L"";
    at = json.find('"', at);
    if (at == std::string::npos) return L"";
    std::string v;
    for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        v += json[i];
    }
    return util::Utf8ToWide(v);
}

std::vector<std::wstring> RunningDistros() {
    std::string out;
    if (RunTool(SystemTool(L"wsl.exe"), L"--list --running --quiet", out, 20000) != 0) return {};
    return Lines(DecodeToolOutput(out));
}

bool AskDf(VDisk& d) {
    std::string out;
    // Distro names have no spaces, and wsl.exe passes quotes through literally.
    if (RunTool(SystemTool(L"wsl.exe"), L"-d " + d.distro + L" -u root -- df -P -B1 " + d.mount, out, 30000) != 0) return false;
    uint64_t used = 0;
    if (!ParseDfUsed(out, used)) return false;
    d.used = used;
    d.usedKnown = true;
    d.usedFrom = L"df";
    return true;
}

// ---------------------------------------------------------------------------------------------
// VHDX: just enough of the format to find the first block of the virtual disk.

const GUID kBatRegion = {0x2DC27766, 0xF623, 0x4200, {0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08}};
const GUID kMetadataRegion = {0x8B7CA206, 0x4790, 0x4B9A, {0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E}};
const GUID kFileParameters = {0xCAA16737, 0xFA36, 0x4D43, {0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B}};
const GUID kLogicalSectorSize = {0x8141BF1D, 0xA96F, 0x4709, {0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F}};

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD n) {
    OVERLAPPED o{};
    o.Offset = static_cast<DWORD>(off);
    o.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, &o) && got == n;
}

template <class T>
T Get(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

// Reads n bytes at a virtual-disk offset (within one block). Blocks that were never written read
// as zeros.
bool ReadVirtual(HANDLE h, uint64_t virtOffset, uint8_t* out, DWORD n, std::wstring& err) {
    char sig[8];
    if (!ReadAt(h, 0, sig, 8) || memcmp(sig, "vhdxfile", 8) != 0) return err = Tr(L"not a VHDX file"), false;
    uint64_t batOff = 0, metaOff = 0;
    for (uint64_t at : {0x30000ull, 0x40000ull}) {  // the region table and its copy
        std::vector<uint8_t> rt(0x10000);
        if (!ReadAt(h, at, rt.data(), static_cast<DWORD>(rt.size())) || memcmp(rt.data(), "regi", 4) != 0) continue;
        uint32_t count = std::min<uint32_t>(Get<uint32_t>(&rt[8]), 2047);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = &rt[16 + i * 32];
            if (memcmp(e, &kBatRegion, 16) == 0) batOff = Get<uint64_t>(e + 16);
            if (memcmp(e, &kMetadataRegion, 16) == 0) metaOff = Get<uint64_t>(e + 16);
        }
        if (batOff && metaOff) break;
    }
    if (!batOff || !metaOff) return err = Tr(L"no block table in the VHDX"), false;
    std::vector<uint8_t> md(0x10000);
    if (!ReadAt(h, metaOff, md.data(), static_cast<DWORD>(md.size())) || memcmp(md.data(), "metadata", 8) != 0)
        return err = Tr(L"no metadata in the VHDX"), false;
    uint32_t blockSize = 0, sector = 0;
    uint16_t count = std::min<uint16_t>(Get<uint16_t>(&md[10]), 2047);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = &md[32 + i * 32];
        uint32_t off = Get<uint32_t>(e + 16);
        uint8_t v[4];
        if (!ReadAt(h, metaOff + off, v, 4)) continue;
        if (memcmp(e, &kFileParameters, 16) == 0) blockSize = Get<uint32_t>(v);
        if (memcmp(e, &kLogicalSectorSize, 16) == 0) sector = Get<uint32_t>(v);
    }
    if (blockSize < (1u << 20) || (blockSize & (blockSize - 1)) || (sector != 512 && sector != 4096))
        return err = Tr(L"unexpected VHDX block size"), false;
    uint64_t chunkRatio = (1ull << 23) * sector / blockSize;  // payload entries between sector bitmap entries
    uint64_t block = virtOffset / blockSize, within = virtOffset % blockSize;
    if (within + n > blockSize) return err = Tr(L"read across blocks"), false;
    uint64_t entry = 0;
    if (!ReadAt(h, batOff + (block + block / chunkRatio) * 8, &entry, 8)) return err = Tr(L"can't read the block table"), false;
    uint64_t state = entry & 7;
    if (state != 6 && state != 7) {  // not fully/partially present: never written
        memset(out, 0, n);
        return true;
    }
    if (!ReadAt(h, (entry >> 20) * (1ull << 20) + within, out, n)) return err = Tr(L"can't read the VHDX"), false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

bool ParseDfUsed(const std::string& dfOutput, uint64_t& used) {
    // "Filesystem 1-blocks Used Available Capacity Mounted on" / "/dev/sdd 1081101176832 23707013120 ... /"
    auto lines = Lines(DecodeToolOutput(dfOutput));
    if (lines.size() < 2) return false;
    std::vector<std::wstring> tok;
    std::wstring cur;
    for (wchar_t c : lines.back() + L" ") {
        if (c == L' ' || c == L'\t') {
            if (!cur.empty()) tok.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (tok.size() < 6) return false;
    const std::wstring& u = tok[tok.size() - 4];  // counted from the end: the file system name may contain spaces
    if (u.empty() || u.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    used = _wcstoui64(u.c_str(), nullptr, 10);
    return true;
}

bool ReadVhdxExt4(const std::wstring& vhdx, uint64_t& used, uint64_t& total, std::wstring& err) {
    HANDLE h = CreateFileW(ToLongPath(vhdx).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = util::Win32ErrorMessage(GetLastError());
        return false;
    }
    uint8_t sb[1024];
    bool ok = ReadVirtual(h, 1024, sb, sizeof(sb), err);
    CloseHandle(h);
    if (!ok) return false;
    if (Get<uint16_t>(sb + 0x38) != 0xEF53) return err = Tr(L"no ext4 file system (or never formatted)"), false;
    uint32_t logBlock = Get<uint32_t>(sb + 0x18);
    if (logBlock > 6) return err = Tr(L"unexpected ext4 block size"), false;
    uint64_t bs = 1024ull << logBlock;
    uint64_t blocks = Get<uint32_t>(sb + 0x04), freeBlocks = Get<uint32_t>(sb + 0x0C);
    if (Get<uint32_t>(sb + 0x60) & 0x80) {  // INCOMPAT_64BIT: high halves
        blocks |= static_cast<uint64_t>(Get<uint32_t>(sb + 0x150)) << 32;
        freeBlocks |= static_cast<uint64_t>(Get<uint32_t>(sb + 0x158)) << 32;
    }
    if (freeBlocks > blocks) return err = Tr(L"inconsistent ext4 superblock"), false;
    used = (blocks - freeBlocks) * bs;
    total = blocks * bs;
    return true;
}

std::vector<VDisk> FindVDisks() {
    std::vector<VDisk> out;
    HKEY lxss = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &lxss) == ERROR_SUCCESS) {
        wchar_t sub[256];
        for (DWORD i = 0;; ++i) {
            DWORD n = 256;
            if (RegEnumKeyExW(lxss, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY k = nullptr;
            if (RegOpenKeyExW(lxss, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
            VDisk d;
            d.distro = RegString(k, L"DistributionName");
            std::wstring base = StripLongPrefix(RegString(k, L"BasePath"));
            std::wstring file = RegString(k, L"VhdFileName");
            DWORD version = RegDword(k, L"Version", 2);
            RegCloseKey(k);
            if (d.distro.empty() || base.empty() || version != 2) continue;  // WSL 1 keeps plain files, no disk
            while (!base.empty() && base.back() == L'\\') base.pop_back();
            d.path = base + L"\\" + (file.empty() ? L"ext4.vhdx" : file);
            if (!PathExists(d.path)) continue;
            d.name = d.distro;
            d.mount = L"/";
            d.docker = d.distro.rfind(L"docker-desktop", 0) == 0;
            if (d.distro == L"docker-desktop") d.name = N_(L"Docker Desktop (system)");
            if (d.distro == L"docker-desktop-data") d.name = N_(L"Docker Desktop data");
            out.push_back(std::move(d));
        }
        RegCloseKey(lxss);
    }
    // Docker Desktop 4.30+: one data disk attached to the docker-desktop distro. Its folder can be
    // moved in Settings > Resources > Disk image location.
    std::wstring local = Env(L"LOCALAPPDATA");
    std::vector<std::wstring> dockerDirs;
    std::wstring custom = JsonString(ReadSmall(Env(L"APPDATA") + L"\\Docker\\settings-store.json"), "CustomWslDistroDir");
    if (custom.empty()) custom = JsonString(ReadSmall(Env(L"APPDATA") + L"\\Docker\\settings.json"), "customWslDistroDir");
    if (!custom.empty()) dockerDirs.push_back(StripLongPrefix(custom));
    if (!local.empty()) dockerDirs.push_back(local + L"\\Docker\\wsl");
    for (auto& dir : dockerDirs)
        for (const wchar_t* rel : {L"\\disk\\docker_data.vhdx", L"\\docker_data.vhdx"}) {
            std::wstring p = dir + rel;
            if (!PathExists(p)) continue;
            bool known = false;
            for (auto& d : out) known = known || _wcsicmp(d.path.c_str(), p.c_str()) == 0;
            if (known) continue;
            VDisk d;
            d.name = N_(L"Docker Desktop data");
            d.distro = L"docker-desktop";
            d.mount = L"/mnt/docker-desktop-disk";
            d.path = p;
            d.docker = true;
            out.push_back(std::move(d));
        }
    if (out.empty()) return out;

    auto running = RunningDistros();
    for (auto& d : out) {
        d.fileBytes = AllocatedSize(d.path);
        d.running = std::find(running.begin(), running.end(), d.distro) != running.end();
        // Never start a stopped distro just to measure it: read its superblock instead (exact once
        // the distro was shut down cleanly).
        if (d.running && AskDf(d)) continue;
        uint64_t used = 0, total = 0;
        std::wstring err;
        if (ReadVhdxExt4(d.path, used, total, err)) {
            d.used = used;
            d.usedKnown = true;
            d.usedFrom = L"superblock";
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const VDisk& a, const VDisk& b) { return a.fileBytes > b.fileBytes; });
    return out;
}

// ---------------------------------------------------------------------------------------------
// Docker

const DockerRow* DockerInfo::Find(const wchar_t* type) const {
    for (auto& r : rows)
        if (_wcsicmp(r.type.c_str(), type) == 0) return &r;
    return nullptr;
}

uint64_t ParseDockerSize(const std::wstring& s) {
    // Docker prints decimal units: "0B", "749.6kB", "38.69GB".
    size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    size_t numStart = i;
    while (i < s.size() && (iswdigit(s[i]) || s[i] == L'.')) ++i;
    if (i == numStart) return 0;
    double v = _wtof(s.substr(numStart, i - numStart).c_str());
    std::wstring unit = util::ToLower(s.substr(i, 2));
    double mul = 1;
    if (!unit.empty()) switch (unit[0]) {
            case L'k': mul = 1e3; break;
            case L'm': mul = 1e6; break;
            case L'g': mul = 1e9; break;
            case L't': mul = 1e12; break;
            case L'p': mul = 1e15; break;
            default: break;
        }
    return static_cast<uint64_t>(v * mul + 0.5);
}

std::vector<DockerRow> ParseDockerDf(const std::string& jsonLines) {
    std::vector<DockerRow> rows;
    size_t pos = 0;
    while (pos < jsonLines.size()) {
        size_t nl = jsonLines.find('\n', pos);
        std::string line = jsonLines.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? jsonLines.size() : nl + 1;
        if (line.find('{') == std::string::npos) continue;
        DockerRow r;
        r.type = JsonString(line, "Type");
        if (r.type.empty()) continue;
        r.size = ParseDockerSize(JsonString(line, "Size"));
        r.reclaimable = ParseDockerSize(JsonString(line, "Reclaimable"));  // "38.69GB (89%)"
        r.total = _wtoi(JsonString(line, "TotalCount").c_str());
        r.active = _wtoi(JsonString(line, "Active").c_str());
        rows.push_back(std::move(r));
    }
    return rows;
}

DockerInfo QueryDocker() {
    DockerInfo info;
    info.desktopRunning = ProcessRunning(L"Docker Desktop.exe");
    std::wstring docker = FindOnPath(L"docker.exe");
    info.cli = !docker.empty();
    if (!info.cli) return info;
    std::string out;
    int code = RunTool(docker, L"system df --format \"{{json .}}\"", out, 60000);
    if (code == 0) info.rows = ParseDockerDf(out);
    info.running = code == 0 && !info.rows.empty();
    if (!info.running) info.error = code == -2 ? Tr(L"Docker didn't answer in time") : LastLine(out);
    return info;
}

DockerPruneResult DockerPrune(bool buildCache, bool images, std::atomic<bool>& cancel) {
    DockerPruneResult res;
    std::wstring docker = FindOnPath(L"docker.exe");
    if (docker.empty()) {
        res.errors.push_back(L"docker was not found on PATH");
        return res;
    }
    struct Job {
        bool on;
        const wchar_t* type;
        const wchar_t* args;
        const wchar_t* label;
    };
    const Job jobs[] = {{buildCache, L"Build Cache", L"builder prune -a -f", N_(L"Docker build cache")},
                        {images, L"Images", L"image prune -a -f", N_(L"Docker images no container uses")}};
    for (auto& j : jobs) {
        if (!j.on || cancel) continue;
        // What Docker itself reports before and after: the data lives inside its virtual disk.
        DockerInfo before = QueryDocker();
        const DockerRow* b = before.Find(j.type);
        std::string out;
        int code = RunTool(docker, j.args, out, 30 * 60000, &cancel);
        DockerInfo after = QueryDocker();
        const DockerRow* a = after.Find(j.type);
        uint64_t freed = b && a && b->size > a->size ? b->size - a->size : 0;
        res.freed += freed;
        std::wstring cmd = std::wstring(L"docker ") + j.args;
        if (code != 0) res.errors.push_back(cmd + L": " + (code == -2 ? Tr(L"cancelled") : LastLine(out)));
        JournalEntry je;
        je.action = JournalAction::Clean;
        je.bytes = freed;
        je.path = j.label;
        je.note = L"ran " + cmd + L" (inside Docker's virtual disk: compact it to give the space to Windows)" +
                  (code != 0 ? L"; failed: " + LastLine(out) : L"");
        JournalAppend(je);
    }
    return res;
}

// ---------------------------------------------------------------------------------------------
// Compacting

std::string DiskpartScript(const std::vector<std::wstring>& vhdx) {
    std::string s;
    for (auto& p : vhdx) {
        s += "select vdisk file=\"" + util::WideToUtf8(p) + "\"\r\n";
        s += "attach vdisk readonly\r\n";
        s += "compact vdisk\r\n";
        s += "detach vdisk\r\n";
    }
    s += "exit\r\n";
    return s;
}

CompactResult CompactVDisks(const std::vector<VDisk>& disks, std::atomic<bool>& cancel,
                            const std::function<void(const std::wstring&)>& status) {
    CompactResult res;
    if (disks.empty()) return res;
    std::wstring wsl = SystemTool(L"wsl.exe");
    std::string out;
    // 1. Hand free space back to the virtual disk (Docker's data disk isn't mounted with discard).
    for (auto& d : disks) {
        if (cancel) return res;
        if (!d.running) continue;
        status(TrF(L"Trimming free space in {0}", Tr(d.name)));
        out.clear();
        RunTool(wsl, L"-d " + d.distro + L" -u root -- fstrim " + d.mount, out, 10 * 60000, &cancel);
    }
    if (cancel) return res;
    // 2. Docker Desktop starts WSL again by itself: stop it first, start it again at the end.
    bool dockerStopped = false;
    if (ProcessRunning(L"Docker Desktop.exe")) {
        std::wstring docker = FindOnPath(L"docker.exe");
        status(Tr(L"Stopping Docker Desktop"));
        out.clear();
        if (docker.empty() || RunTool(docker, L"desktop stop", out, 5 * 60000, &cancel) != 0) {
            res.errors.push_back(TrF(L"Couldn't stop Docker Desktop ({0}). Quit it, then compact again.",
                                     docker.empty() ? Tr(L"docker not found") : LastLine(out)));
            return res;
        }
        dockerStopped = true;
    }
    // 3. Shut WSL down; the disks are detached once it has stopped.
    status(Tr(L"Shutting down WSL"));
    out.clear();
    RunTool(wsl, L"--shutdown", out, 3 * 60000);
    std::vector<std::wstring> paths;
    std::vector<const VDisk*> chosen;
    ULONGLONG start = GetTickCount64();
    for (auto& d : disks) {
        while (InUse(d.path) && GetTickCount64() - start < 60000 && !cancel) Sleep(500);
        if (InUse(d.path)) {
            res.errors.push_back(TrF(L"{0}: still in use after WSL shut down", Tr(d.name)));
            continue;
        }
        paths.push_back(d.path);
        chosen.push_back(&d);
    }
    std::vector<uint64_t> before;
    for (auto* d : chosen) {
        before.push_back(AllocatedSize(d->path));
        res.before += before.back();
    }
    // 4. diskpart, as administrator.
    if (!paths.empty() && !cancel) {
        status(TrN(paths.size(), L"Compacting {0} virtual disk (this can take a while)", L"Compacting {0} virtual disks (this can take a while)"));
        std::wstring script = TempDir() + L"winncdu-compact-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
        std::string text = DiskpartScript(paths);
        HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD n = 0;
        bool written = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr) && n == text.size();
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        // Held open read-only while diskpart (as administrator) runs it: nobody can change or swap it,
        // and what it says is checked once more.
        f = written ? CreateFileW(script.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
        std::string check(text.size() + 1, '\0');
        written = f != INVALID_HANDLE_VALUE && ReadFile(f, check.data(), static_cast<DWORD>(check.size()), &n, nullptr) &&
                  n == text.size() && check.compare(0, n, text) == 0;
        if (!written) {
            res.errors.push_back(TrF(L"Couldn't write {0}", script));
        } else {
            AdminRun r = RunAsAdmin(Quote(SystemTool(L"diskpart.exe")) + L" /s " + Quote(script));  // never interrupted
            if (!r.error.empty())
                res.errors.push_back(r.declined ? Tr(L"Administrator permission was declined: nothing was compacted") : r.error);
            else if (r.code != 0)
                res.errors.push_back(L"diskpart: " + LastLine(r.out));
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(script.c_str());
    }
    for (size_t i = 0; i < chosen.size(); ++i) {
        uint64_t after = AllocatedSize(chosen[i]->path);
        res.after += after;
        uint64_t freed = before[i] > after ? before[i] - after : 0;
        if (freed) ++res.compacted;
        JournalEntry je;
        je.action = JournalAction::Clean;
        je.bytes = freed;
        je.path = chosen[i]->path;
        je.note = L"compacted the virtual disk of " + chosen[i]->name;
        JournalAppend(je);
    }
    // 5. Docker Desktop back as it was.
    if (dockerStopped) {
        status(Tr(L"Starting Docker Desktop again"));
        std::wstring docker = FindOnPath(L"docker.exe");
        out.clear();
        res.dockerRestarted = RunTool(docker, L"desktop start", out, 5 * 60000) == 0;
        if (!res.dockerRestarted) res.errors.push_back(Tr(L"Docker Desktop didn't start again: start it yourself"));
    }
    return res;
}
