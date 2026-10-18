// Self-update in the windowed app: the first-start question, the daily check (only when turned
// on), ⋯ > Check for updates now, the banner that offers a newer version, and installing it
// (core: src/update.cpp).

#include "app.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "../fsops.h"
#include "../util.h"
#include "../version.h"

namespace {

constexpr float kSidebarW = 240.0f;
constexpr float kStatusH = 30.0f;
constexpr wchar_t IC_DOWNLOAD = 0xE896, IC_SYNC = 0xE895, IC_WARN = 0xE7BA;

std::wstring FolderOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// "2026-10-18" in the Windows regional format.
std::wstring ReleasedText(const std::wstring& iso) {
    SYSTEMTIME st{};
    if (iso.size() < 10 || swscanf_s(iso.c_str(), L"%hu-%hu-%hu", &st.wYear, &st.wMonth, &st.wDay) != 3) return iso;
    st.wHour = 12;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return iso;
    return util::FormatDateLocal(static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime));
}

void OpenUrl(const std::wstring& url) {
    if (url.rfind(L"https://", 0) == 0) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

void App::StartupUpdates(bool updated) {
    std::wstring exe = ExePath();
    pkgMgr_ = PackageManagerFor(exe);
    // The programs an update replaced were kept as .old (they were running): delete them now. Right
    // after an update the previous window may still be closing, so it tries for a few seconds.
    std::wstring dir = FolderOf(exe);
    HANDLE t = util::StartThread(
        [dir, updated] {
            for (int i = 0; i < (updated ? 20 : 1); ++i) {
                if (!CleanupOldFiles(dir)) break;
                Sleep(500);
            }
        },
        256 << 10);
    if (t) CloseHandle(t);
    if (updated) {
        toast_ = {TrF(L"Updated to Bytopsy {0}", BYTOPSY_VERSION), 0, GetTickCount64() + 6000};
        EnsureTimer();
    }
}

void App::AskAboutUpdates() {
    Dialog d;
    d.icon = IC_SYNC;
    d.title = Tr(L"Check for new versions?");
    d.message = Tr(L"Bytopsy can look once a day for a new version and tell you about it. It then asks bytopsy.com (or GitHub, "
                   L"if that doesn't answer) for a small file with the latest version number; nothing about you or your PC is "
                   L"sent, and nothing is installed without asking.") +
                L"\n\n" + Tr(L"You can change this later in ⋯ > Check for updates daily.");
    d.buttons = {Tr(L"Don't check"), Tr(L"Check once a day")};
    d.primary = -1;  // neither is preselected: Enter and Escape both mean "don't check"
    d.cancel = 0;
    d.onClose = [this](int b, bool) {
        settings_.updatesAsked = true;
        settings_.updates = b == 1 ? 1 : 0;
        SaveSettings(settings_);
        if (b == 1) MaybeCheckForUpdates();
    };
    ShowDialog(std::move(d));
}

void App::MaybeCheckForUpdates() {
    if (settings_.updates != 1 || updateCheckRunning_) return;
    int64_t now = static_cast<int64_t>(time(nullptr));
    if (settings_.updateLastCheck && now >= settings_.updateLastCheck && now - settings_.updateLastCheck < 86400) return;
    settings_.updateLastCheck = now;
    SaveSettings(settings_);
    StartUpdateCheck(false);
}

void App::StartUpdateCheck(bool manual) {
    UpdateSources src = DefaultUpdateSources();
    if (!manual) {
        // In the background: nothing is shown unless a newer version turns up.
        updateCheckRunning_ = true;
        HWND hwnd = hwnd_;
        HANDLE t = util::StartThread(
            [src, hwnd] {
                auto* r = new UpdateCheck(CheckForUpdate(src, BYTOPSY_VERSION));
                if (!PostMessageW(hwnd, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(r))) delete r;
            },
            1 << 20);
        if (t) CloseHandle(t);
        return;
    }
    auto result = std::make_shared<UpdateCheck>();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Checking for updates…");
    std::wstring where = src.local ? src.urls.front() : L"bytopsy.com · github.com";
    t->lines = [where] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{Tr(L"Installed"), BYTOPSY_VERSION}, {Tr(L"Asking"), where}};
    };
    t->cancel = [cancel] { cancel->store(true); };
    t->onDone = [this, result] { OnUpdateChecked(*result, true); };
    StartTask(std::move(t), [src, result, cancel] { *result = CheckForUpdate(src, BYTOPSY_VERSION, cancel.get()); });
}

void App::OnUpdateChecked(const UpdateCheck& c, bool manual) {
    switch (c.status) {
        case UpdateStatus::Available:
            // A version the user skipped comes back only when asked for, or when it is important.
            if (!manual && !c.required && _wcsicmp(c.manifest.version.c_str(), settings_.updateSkip.c_str()) == 0) return;
            update_ = c;
            updateBanner_ = true;
            if (acc_) acc_->Notify(TrF(L"Bytopsy {0} is available", c.manifest.version));
            break;
        case UpdateStatus::UpToDate:
            if (manual) Toast(TrF(L"Bytopsy {0} is the latest version", BYTOPSY_VERSION), 0);
            break;
        case UpdateStatus::NoKey:
            if (manual) Toast(Tr(L"This build of Bytopsy can't check for updates: it has no update key"), 2);
            break;
        case UpdateStatus::Failed:
            if (manual) Toast(TrF(L"Couldn't check for updates: {0}", c.error), 1);
            break;
    }
    Invalidate();
}

void App::OnUpdateCommand(int cmd) {
    const std::wstring& v = update_.manifest.version;
    switch (cmd) {
        case CMD_UPDATE_CHECK: StartUpdateCheck(true); break;
        case CMD_UPDATE_AUTO:
            settings_.updates = settings_.updates == 1 ? 0 : 1;
            settings_.updatesAsked = true;
            if (settings_.updates == 1) {
                Toast(Tr(L"Bytopsy looks for a new version once a day"), 2);
                MaybeCheckForUpdates();
            } else {
                Toast(Tr(L"Bytopsy no longer looks for new versions by itself"), 2);
            }
            break;
        case CMD_UPDATE_NOTES: OpenUrl(update_.manifest.notesUrl); break;
        case CMD_UPDATE_LATER: updateBanner_ = false; break;
        case CMD_UPDATE_SKIP:
            settings_.updateSkip = v;
            updateBanner_ = false;
            Toast(TrF(L"Bytopsy {0} won't be offered again; ⋯ > Check for updates now still finds it", v), 2);
            break;
        case CMD_UPDATE_COPY: {
            std::wstring command = PackageManagerCommand(pkgMgr_);
            if (CopyTextToClipboard(command)) Toast(TrF(L"Copied: {0}", command), 2);
            break;
        }
        case CMD_UPDATE_INSTALL: ShowUpdateInstall(); break;
    }
}

void App::ShowUpdateInstall() {
    if (update_.status != UpdateStatus::Available) return;
    if (pkgMgr_ != PackageManager::None) {  // the package manager updates it, not Bytopsy
        Toast(TrF(L"Installed with {0}; update it there:  {1}", PackageManagerName(pkgMgr_), PackageManagerCommand(pkgMgr_)), 2);
        return;
    }
    std::wstring dir = FolderOf(ExePath());
    const std::wstring& v = update_.manifest.version;
    if (!FolderWritable(dir)) {
        // Program Files and the like: Bytopsy doesn't ask for administrator rights by itself.
        Dialog d;
        d.icon = IC_WARN;
        d.tone = 2;
        d.title = Tr(L"Bytopsy can't update itself here");
        d.message = TrF(L"Windows doesn't let Bytopsy change files in {0} without administrator rights.", dir) + L" " +
                    (elevated_ ? Tr(L"Download the new version from its release page and unpack it there.")
                               : Tr(L"Restart Bytopsy as administrator and choose ⋯ > Check for updates now, or download the new version from its release page."));
        d.buttons = {Tr(L"Release page")};
        if (!elevated_) d.buttons.push_back(Tr(L"Restart as administrator"));
        d.buttons.push_back(Tr(L"Close"));
        d.primary = static_cast<int>(d.buttons.size()) - 1;
        d.cancel = d.primary;
        bool elevated = elevated_;
        d.onClose = [this, elevated](int b, bool) {
            if (b == 0) OpenUrl(update_.manifest.notesUrl);
            if (b == 1 && !elevated) OnCommand(CMD_ADMIN);
        };
        ShowDialog(std::move(d));
        return;
    }
    Dialog d;
    d.icon = IC_DOWNLOAD;
    d.title = TrF(L"Update to Bytopsy {0}?", v);
    d.message = TrF(L"Bytopsy downloads the release zip ({0}) from GitHub, checks it against the signed update information, replaces "
                    L"its programs in {1} and restarts.",
                    util::FormatSizeShort(update_.asset.size), dir) +
                L"\n\n" +
                Tr(L"Settings, history, your rules and a portable copy's Bytopsy-data stay as they are. The programs you have now are "
                   L"kept as .old files until the next start.");
    d.lines = {Tr(L"Installed") + L"\t" + BYTOPSY_VERSION, Tr(L"New") + L"\t" + v, Tr(L"Download") + L"\t" + update_.asset.url};
    d.buttons = {Tr(L"Cancel"), Tr(L"Update and restart")};
    d.primary = 1;
    d.cancel = 0;
    d.onClose = [this](int b, bool) {
        if (b == 1) StartUpdateInstall();
    };
    ShowDialog(std::move(d));
}

void App::StartUpdateInstall() {
    struct State {
        UpdateProgress progress;
        std::atomic<bool> cancel{false};
        bool ok = false;
        std::wstring err;
    };
    auto st = std::make_shared<State>();
    UpdateCheck c = update_;
    std::wstring exe = ExePath();
    auto t = std::make_unique<Task>();
    t->title = TrF(L"Updating to Bytopsy {0}…", c.manifest.version);
    t->lines = [st] {
        std::wstring step;
        switch (static_cast<UpdateStep>(st->progress.step.load())) {
            case UpdateStep::Download: step = Tr(L"Downloading"); break;
            case UpdateStep::Verify: step = Tr(L"Checking the download"); break;
            case UpdateStep::Unpack: step = Tr(L"Unpacking"); break;
            case UpdateStep::Install:
            case UpdateStep::Done: step = Tr(L"Replacing the programs"); break;
        }
        return std::vector<std::pair<std::wstring, std::wstring>>{
            {Tr(L"Step"), step},
            {Tr(L"Downloaded"), TrF(L"{0} of {1}", util::FormatSizeShort(st->progress.done), util::FormatSizeShort(st->progress.total))}};
    };
    t->progress = [st] {
        uint64_t total = st->progress.total;
        if (st->progress.step.load() != static_cast<int>(UpdateStep::Download)) return 1.0;
        return total ? static_cast<double>(st->progress.done) / static_cast<double>(total) : -1.0;
    };
    t->cancel = [st] { st->cancel = true; };  // honored until the files are swapped
    t->onDone = [this, st, c, exe] {
        if (!st->ok) {
            if (st->cancel) {
                Toast(Tr(L"Update cancelled; nothing was changed"), 2);
                return;
            }
            Dialog d;
            d.icon = IC_WARN;
            d.tone = 1;
            d.title = Tr(L"The update didn't work");
            d.message = st->err + L"\n\n" + Tr(L"Nothing was changed: you still have the version you had.");
            d.buttons = {Tr(L"Release page"), Tr(L"OK")};
            d.primary = 1;
            d.cancel = 1;
            d.onClose = [url = c.manifest.notesUrl](int b, bool) {
                if (b == 0) OpenUrl(url);
            };
            ShowDialog(std::move(d));
            return;
        }
        updateBanner_ = false;
        // Start the new version and close this one (its exe is now bytopsy.exe.old).
        std::wstring dir = FolderOf(exe), next = dir + L"\\bytopsy.exe";
        std::wstring cmd = L"\"" + next + L"\" --updated";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(next.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        } else {
            Toast(TrF(L"Updated to Bytopsy {0}: start it again to use the new version", c.manifest.version), 0);
        }
    };
    StartTask(std::move(t), [st, c, exe] { st->ok = InstallUpdate(c, exe, &st->progress, &st->cancel, st->err); });
}

void App::DrawUpdateBanner() {
    D2D1_SIZE_F sz = gfx_.Size();
    const UpdateManifest& m = update_.manifest;
    bool pm = pkgMgr_ != PackageManager::None;
    std::wstring title = TrF(L"Bytopsy {0} is available", m.version);
    std::wstring line = update_.required ? TrF(L"Important: versions before {0} should be updated. You have {1}.", m.minimumVersion, BYTOPSY_VERSION)
                        : m.released.empty() ? TrF(L"You have {0}.", BYTOPSY_VERSION)
                                             : TrF(L"You have {0}. Released {1}.", BYTOPSY_VERSION, ReleasedText(m.released));
    std::wstring pmLine = pm ? TrF(L"Installed with {0}; update it there:  {1}", PackageManagerName(pkgMgr_), PackageManagerCommand(pkgMgr_)) : L"";
    struct Btn {
        std::wstring text;
        int cmd, style;
        bool enabled;
    };
    std::vector<Btn> btns = {
        {pm ? Tr(L"Copy command") : Tr(L"Update"), pm ? CMD_UPDATE_COPY : CMD_UPDATE_INSTALL, 1, true},
        {Tr(L"What's new"), CMD_UPDATE_NOTES, 0, !m.notesUrl.empty()},
        {Tr(L"Not now"), CMD_UPDATE_LATER, 3, true},
        {Tr(L"Skip this version"), CMD_UPDATE_SKIP, 3, true},
    };
    // As wide as the texts and buttons need (translations run longer), within the content area.
    float pad = 18, gap = 8, lead = pad + 36 + 14, rowW = 0;
    for (auto& b : btns) rowW += ButtonWidth(b.text, 0, 84) + gap;
    TextStyle ts = TS(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float textW = std::max({gfx_.Measure(title, ts), gfx_.Measure(line, TS(12.5f)), pm ? gfx_.Measure(pmLine, TS(12.5f)) : 0.0f});
    float maxW = std::max(320.0f, sz.width - kSidebarW - 48);
    float w = std::min(maxW, std::ceil(std::max({440.0f, lead + rowW - gap + pad, lead + textW + pad})));
    float h = pad + 22 + 20 + (pm ? 20 : 0) + 14 + 34 + pad;
    RectF r = RWH(sz.width - w - 24, sz.height - kStatusH - h - 18, w, h);
    bannerTop_ = r.top;
    D2D1_COLOR_F tone = update_.required ? P_.warning : P_.accent;
    gfx_.Shadow(r, 14, 18);
    gfx_.FillRound(r, 14, P_.surface);
    gfx_.StrokeRound(r, 14, update_.required ? P_.warning : P_.border);
    RectF ic = RWH(r.left + pad, r.top + pad, 36, 36);
    gfx_.FillRound(ic, 10, WithAlpha(tone, 0.16f));
    gfx_.Icon(IC_DOWNLOAD, ic, tone, 16);
    float tx = ic.right + 14, y = r.top + pad - 2;
    gfx_.Text(title, R(tx, y, r.right - pad, y + 24), P_.text, ts);
    y += 24;
    gfx_.Text(line, R(tx, y, r.right - pad, y + 20), update_.required ? P_.warning : P_.textDim, TS(12.5f));
    y += 20;
    if (pm) {
        gfx_.Text(pmLine, R(tx, y, r.right - pad, y + 20), P_.textDim, TS(12.5f));
        y += 20;
    }
    float bx = tx, by = r.bottom - pad - 34;
    for (auto& b : btns) {
        float bw = std::min(ButtonWidth(b.text, 0, 84), std::max(40.0f, r.right - pad - bx));
        if (bx >= r.right - pad - 20) break;
        Button(RWH(bx, by, bw, 34), 0, b.text, b.cmd, b.style, b.enabled);
        bx += bw + gap;
    }
}
