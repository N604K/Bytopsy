#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include "export.h"
#include "fsops.h"
#include "aimodels.h"
#include "journal.h"
#include "protect.h"
#include "scanner.h"
#include "ui.h"
#include "util.h"
#include "version.h"

namespace {

void PrintUsage() {
    printf(
        "WinNCDU " WINNCDU_VERSION_A " - NCurses Disk Usage style analyzer for Windows\n"
        "\n"
        "Usage: winncdu-cli [options] [path]\n"
        "\n"
        "  path                  Folder or drive to scan (no path: choose from a list)\n"
        "\n"
        "Options:\n"
        "  -o FILE               Scan, write an ncdu-compatible JSON export to FILE and exit\n"
        "  --csv FILE            Scan, write a CSV report to FILE and exit\n"
        "  -f FILE               Browse a previously exported JSON file (read-only)\n"
        "  --exclude PATTERN     Skip files/folders matching PATTERN (wildcards; repeatable).\n"
        "                        Patterns containing '\\' are matched against the full path.\n"
        "  -X, --exclude-from F  Read exclude patterns from file F (one per line)\n"
        "  -t, --threads N       Number of scanner threads (default: %u)\n"
        "  -H, --hardlinks       Detect NTFS hard links and count them once (slower)\n"
        "  --apparent            Show apparent sizes instead of disk usage\n"
        "  --si                  Use base-10 units (kB, MB) instead of KiB, MiB\n"
        "  --ascii               Use only ASCII characters for bars and borders\n"
        "  --no-mouse            Disable mouse support\n"
        "  --no-delete           Disable deleting files (safe browsing mode)\n"
        "  --protected           List the protected locations found on this PC and exit\n"
        "  --journal             List what WinNCDU deleted recently (and what is still in the\n"
        "                        Recycle Bin) and exit\n"
        "  --ai-models           List local AI models (Hugging Face, Ollama, LM Studio) with\n"
        "                        what each frees, and exit\n"
        "  -h, --help            Show this help\n"
        "  -V, --version         Show version\n"
        "\n"
        "Press ? inside the program for all key bindings.\n",
        std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
}

std::wstring NormalizePattern(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    return util::ToLower(p);
}

int ListProtected() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    RefreshProtection();
    auto all = ProtectedLocationsOnThisPC();
    std::wstring group;
    std::wstring out = L"Protected locations on this PC (system folders, AI assistant data, keys, wallets, synced folders).\n"
                       L"WinNCDU never deletes Blocked items and asks twice for Important ones.\n";
    for (auto& h : all) {
        if (h.group != group) {
            group = h.group;
            out += L"\n[" + group + L"]\n";
        }
        wchar_t lvl[16];
        swprintf(lvl, 16, L"%-10ls", ProtectLevelName(h.level));
        out += L"  " + std::wstring(lvl) + L" " + h.path + L"  (" + h.name + L")\n";
    }
    out += L"\nAlso anywhere: Git/SVN/Mercurial repositories, KeePass databases, wallet.dat, .pst, private keys.\n";
    out += L"Your own rules: " + ProtectRulesFile() + L"\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListJournal() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto entries = JournalRead(50);
    auto bin = ListRecycleBin();
    std::wstring out = L"Recently deleted by WinNCDU (newest first). Journal: " + JournalFile() + L"\n\n";
    if (entries.empty()) out += L"  Nothing yet.\n";
    for (auto& e : entries) {
        const wchar_t* state = L"";
        if (e.action == JournalAction::Recycle)
            state = FindRecycled(bin, e) ? L"in Recycle Bin" : (PathExists(e.path) ? L"restored" : L"bin emptied");
        wchar_t head[96];
        swprintf(head, 96, L"  %ls  %-8ls %10ls  %-15ls ", util::FormatTime(e.time).c_str(), JournalActionName(e.action),
                 util::FormatSizeShort(e.bytes).c_str(), state);
        out += head + e.path + (e.note.empty() ? L"" : L"  (" + e.note + L")") + L"\n";
    }
    out += L"\nRestore items that are still in the Recycle Bin in the windowed app: Recently deleted.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int ListAiModels() {
    UINT oldCp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    auto stores = FindAiModels();
    bool atime = LastAccessTracked();
    std::wstring out = L"Local AI models (Hugging Face, Ollama, LM Studio). \"Frees\" is what deleting that model alone frees.\n";
    if (stores.empty()) out += L"\n  None found.\n";
    for (auto& s : stores) {
        out += L"\n" + s.name + L"  " + s.folder + L"  (" + util::FormatSizeShort(s.bytes) + L")\n";
        for (auto& it : s.items) {
            wchar_t head[64];
            swprintf(head, 64, L"  %10ls  %ls  ", util::FormatSizeShort(it.bytes).c_str(), util::FormatTime(it.added).substr(0, 10).c_str());
            out += head + std::wstring(it.parent >= 0 ? L"  " : L"") + it.name + L"  (" + it.detail + L")" +
                   (atime && it.used ? L"  last used " + util::FormatTime(it.used).substr(0, 10) : L"") + L"\n";
        }
    }
    out += L"\nDelete single models in the windowed app: AI models.\n";
    std::string u = util::WideToUtf8(out);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
    SetConsoleOutputCP(oldCp);
    return 0;
}

int Fail(const std::wstring& msg) {
    fwprintf(stderr, L"winncdu-cli: %s\n", msg.c_str());
    return 2;
}

int HeadlessExport(const AppConfig& cfg, const std::wstring& outJson, const std::wstring& outCsv) {
    std::wstring err;
    std::wstring path = NormalizeDirPath(cfg.path.empty() ? L"." : cfg.path, err);
    if (path.empty()) return Fail(L"cannot scan " + cfg.path + L": " + err);
    auto root = std::make_unique<Node>();
    root->name = path;
    root->flags = NF_DIR;
    ReadNodeMeta(root.get(), path);
    ScanStats stats;
    std::atomic<bool> done{false};
    std::thread th([&] {
        ScanDirectory(root.get(), path, cfg.scan, stats);
        RecomputeTotals(root.get());
        done = true;
    });
    ULONGLONG start = GetTickCount64();
    while (!done) {
        Sleep(250);
        fwprintf(stderr, L"\rScanning: %ls files, %ls dirs, %ls   ", util::FormatCount(stats.files).c_str(),
                 util::FormatCount(stats.dirs).c_str(), util::FormatSizeShort(stats.bytes).c_str());
    }
    th.join();
    fwprintf(stderr, L"\rScanned %ls items (%ls) in %ls, %ls errors\n", util::FormatCount(root->items).c_str(),
             util::FormatSizeShort(root->disk).c_str(), util::FormatDuration((GetTickCount64() - start) / 1000.0).c_str(),
             util::FormatCount(stats.errors).c_str());
    std::atomic<uint64_t> count{0};
    if (!outJson.empty()) {
        if (!ExportNcduJson(root.get(), outJson, count, err)) return Fail(err);
        fwprintf(stderr, L"Wrote %ls\n", outJson.c_str());
    }
    if (!outCsv.empty()) {
        if (!ExportCsv(root.get(), outCsv, count, err)) return Fail(err);
        fwprintf(stderr, L"Wrote %ls\n", outCsv.c_str());
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    AppConfig cfg;
    cfg.scan.threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
    std::wstring outJson, outCsv;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&](std::wstring& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"-h" || a == L"--help" || a == L"/?") {
            PrintUsage();
            return 0;
        } else if (a == L"-V" || a == L"--version") {
            printf("WinNCDU " WINNCDU_VERSION_A "\n");
            return 0;
        } else if (a == L"-o") {
            if (!next(outJson)) return Fail(L"-o needs a file name");
        } else if (a == L"--csv") {
            if (!next(outCsv)) return Fail(L"--csv needs a file name");
        } else if (a == L"-f") {
            if (!next(cfg.importFile)) return Fail(L"-f needs a file name");
        } else if (a == L"--exclude") {
            if (!next(v)) return Fail(L"--exclude needs a pattern");
            cfg.scan.excludes.push_back(NormalizePattern(v));
        } else if (a.rfind(L"--exclude=", 0) == 0) {
            cfg.scan.excludes.push_back(NormalizePattern(a.substr(10)));
        } else if (a == L"-X" || a == L"--exclude-from") {
            if (!next(v)) return Fail(L"--exclude-from needs a file name");
            FILE* f = nullptr;
            if (_wfopen_s(&f, v.c_str(), L"rb") != 0 || !f) return Fail(L"cannot read " + v);
            std::string content;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
            fclose(f);
            std::wstring w = util::Utf8ToWide(content);
            size_t pos = 0;
            while (pos < w.size()) {
                size_t nl = w.find(L'\n', pos);
                std::wstring line = w.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (!line.empty() && line[0] == 0xFEFF) line.erase(line.begin());
                if (!line.empty() && line[0] != L'#') cfg.scan.excludes.push_back(NormalizePattern(line));
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
        } else if (a == L"-t" || a == L"--threads") {
            if (!next(v)) return Fail(L"--threads needs a number");
            cfg.scan.threads = std::clamp(_wtoi(v.c_str()), 1, 64);
        } else if (a == L"-H" || a == L"--hardlinks") {
            cfg.scan.hardlinks = true;
        } else if (a == L"--apparent") {
            cfg.apparent = true;
        } else if (a == L"--si") {
            util::g_si = true;
        } else if (a == L"--ascii") {
            cfg.ascii = true;
        } else if (a == L"--no-mouse") {
            cfg.mouse = false;
        } else if (a == L"--no-delete") {
            cfg.noDelete = true;
        } else if (a == L"--protected") {
            return ListProtected();
        } else if (a == L"--journal") {
            return ListJournal();
        } else if (a == L"--ai-models") {
            return ListAiModels();
        } else if (a == L"-x") {
            // ncdu compatibility: WinNCDU never crosses into other volumes (mount points are links).
        } else if (a.size() > 1 && a[0] == L'-') {
            return Fail(L"unknown option " + a + L" (see --help)");
        } else {
            cfg.path = a;
        }
    }

    if (!outJson.empty() || !outCsv.empty()) return HeadlessExport(cfg, outJson, outCsv);
    return RunApp(cfg);
}
