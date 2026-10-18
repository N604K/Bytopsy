// Drivers view (works without a scan): driver updates Windows Update offers this PC, installed by
// Windows Update itself after a System Restore point, and the PC maker's and GPU vendors' own
// driver tools, which are only opened, never driven.
#include "app.h"

#include <shellapi.h>

#include <algorithm>

#include "../runtool.h"
#include "../util.h"

namespace {

constexpr wchar_t IC_DRIVER = 0xE964, IC_REFRESH = 0xE72C, IC_PC = 0xE7F8, IC_GPU = 0xE7F4, IC_UPDATE = 0xE895, IC_WARN = 0xE7BA,
                  IC_OK = 0xE73E;

TextStyle Small(float size = 12.0f, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, Align a = Align::Left) { return TS(size, w, a); }

std::wstring Shorten(std::wstring s, size_t max) {
    if (s.size() > max) s = s.substr(0, max - 1) + L"…";
    return s;
}

std::wstring Dots(const std::vector<std::wstring>& parts) {
    std::wstring s;
    for (auto& p : parts)
        if (!p.empty()) s += (s.empty() ? L"" : L"  ·  ") + p;
    return s;
}

// "24.40.0.4 (4/13/2026)"
std::wstring VersionDate(const std::wstring& version, int64_t date) {
    std::wstring v = version.empty() ? Tr(L"unknown version") : version;
    return date ? TrF(L"{0} ({1})", v, util::FormatDateLocal(date)) : v;
}

constexpr float kCardH = 100, kUpdH = 100, kUpdStep = 108, kSectionH = 34;

}  // namespace

// =============================================================================================
// Model

void App::LoadDrivers() {
    if (task_) return;
    auto st = std::make_shared<DriverToolsReport>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Looking at this PC's drivers");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{{L"", Tr(L"Devices, the PC maker, and the vendors' driver tools that are installed")}};
    };
    t->onDone = [this, st] {
        drvTools_ = std::move(*st);
        drvLinks_.clear();
        for (auto& tool : drvTools_.makerTools) drvLinks_.push_back(tool);
        for (auto& g : drvTools_.gpus)
            for (auto& tool : g.tools) drvLinks_.push_back(tool);
        drvLoaded_ = true;
        Invalidate();
    };
    StartTask(std::move(t), [st] { *st = ReadDriverTools(); });
}

void App::CheckDriverUpdates() {
    if (task_) return;
    struct State {
        DriverSearch s;
        std::atomic<bool> cancel{false};
    };
    auto st = std::make_shared<State>();
    auto t = std::make_unique<Task>();
    t->title = Tr(L"Asking Windows Update for driver updates");
    t->lines = [] {
        return std::vector<std::pair<std::wstring, std::wstring>>{
            {L"", Tr(L"Windows contacts Windows Update for this, as Settings does; it can take a minute or two")}};
    };
    t->cancel = [st] { st->cancel = true; };
    t->onDone = [this, st] {
        if (st->cancel) return Toast(Tr(L"Cancelled"), 2);
        drvSearch_ = std::move(st->s);
        drvSearched_ = true;
        drvSel_.clear();
        for (size_t i = 0; i < drvSearch_.updates.size(); ++i)
            if (drvSearch_.updates[i].Preselect(drvSearch_.devices)) drvSel_.insert(i);
        if (!drvSearch_.ok) Toast(drvSearch_.error, 1);
        Invalidate();
    };
    StartTask(std::move(t), [st] { st->s = SearchDriverUpdates(&st->cancel); });
}

void App::StartDriverInstall() {
    std::vector<DriverUpdate> chosen;
    for (size_t i : drvSel_)
        if (i < drvSearch_.updates.size()) chosen.push_back(drvSearch_.updates[i]);
    if (chosen.empty()) return Toast(Tr(L"Check the driver updates to install first"), 2);
    std::wstring exe = ExePath();
    std::wstring cli = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"bytopsy-cli.exe";
    if (!PathExists(cli)) return Toast(Tr(L"bytopsy-cli.exe was not found next to bytopsy.exe"), 1);

    Dialog d;
    d.title = TrN(chosen.size(), L"Install {0} driver update through Windows Update?", L"Install {0} driver updates through Windows Update?");
    d.message = Tr(L"Windows Update downloads and installs exactly these drivers, as Settings > Windows Update would: signed by Microsoft "
                   L"and matched to this PC's devices. Bytopsy installs nothing itself. A device may stop for a moment while its driver "
                   L"changes.");
    if (!elevated_) d.message += L" " + Tr(L"Windows asks for administrator permission.");
    bool reboot = false;
    for (auto& u : chosen) {
        const DeviceDriver* dev = u.device >= 0 && u.device < static_cast<int>(drvSearch_.devices.size()) ? &drvSearch_.devices[u.device] : nullptr;
        d.lines.push_back(util::FormatSizeShort(u.bytes) + L"\t" + Shorten(u.title, 80));
        d.lines.push_back(L"\t" + TrF(L"{0} → {1}", dev ? VersionDate(dev->version, dev->date) : Tr(L"no device found"), VersionDate(u.version, u.date)));
        if (u.display) d.lines.push_back(L"\t⚠ " + Tr(L"A graphics driver: the GPU vendor's own tool is usually the better source"));
        if (u.age == OfferAge::Older) d.lines.push_back(L"\t⚠ " + Tr(L"Older than the driver the device has now"));
        reboot = reboot || u.reboot == 1;
    }
    if (reboot) d.message += L" " + Tr(L"Windows has to restart afterwards.");
    d.hasCheck = true;
    d.checked = true;
    d.checkText = Tr(L"Create a System Restore point first (if it can't be made, nothing is installed)");
    d.checkDanger = false;
    d.icon = IC_DRIVER;
    d.tone = 2;
    d.buttons = {Tr(L"Cancel"), Tr(L"Install")};
    d.primary = 1;
    d.onClose = [this, chosen, cli](int b, bool restorePoint) {
        if (b != 1) return;
        std::wstring ids;
        for (auto& u : chosen) ids += (ids.empty() ? L"" : L",") + u.updateId;
        std::vector<std::wstring> check;
        if (!ParseUpdateIdList(ids, check)) return Toast(Tr(L"Windows Update gave an update id Bytopsy doesn't recognize"), 1);
        // The elevated copy journals into this journal, also when another account approves the prompt.
        std::wstring journal = JournalFile();
        std::wstring cmd = (journal.empty() ? L"" : L"set \"BYTOPSY_JOURNAL=" + journal + L"\" & ") + L"\"" + cli + L"\" --install-driver-updates " +
                           ids + (restorePoint ? L"" : L" --no-restore-point");
        struct State {
            AdminRun run;
        };
        auto st = std::make_shared<State>();
        auto t = std::make_unique<Task>();
        t->title = Tr(L"Installing driver updates through Windows Update");
        t->lines = [restorePoint] {
            std::vector<std::pair<std::wstring, std::wstring>> l;
            if (restorePoint) l.push_back({L"", Tr(L"A System Restore point first, then Windows Update downloads and installs")});
            l.push_back({L"", Tr(L"This can take several minutes. Don't turn the PC off.")});
            return l;
        };
        t->onDone = [this, st, chosen] {
            const AdminRun& r = st->run;
            if (!r.error.empty()) return Toast(r.error, r.declined ? 2 : 1);
            std::wstring text = DecodeToolOutput(r.out);
            if (r.code == 4) {
                std::wstring why;
                size_t at = text.find(L"RESTOREPOINT\tfailed\t");
                if (at != std::wstring::npos) {
                    size_t s = at + 20, e = text.find_first_of(L"\r\n", s);
                    why = text.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
                }
                Dialog e;
                e.title = Tr(L"No restore point, so nothing was installed");
                e.message = (why.empty() ? LastLine(r.out) : why) + L"\n\n" +
                            Tr(L"Install again without the restore point option, or turn System Protection on first.");
                e.icon = IC_WARN;
                e.tone = 2;
                e.buttons = {Tr(L"OK")};
                e.primary = 0;
                ShowDialog(std::move(e));
                return;
            }
            std::vector<DriverInstallResult> res = ParseInstallOutput(r.out);
            if (res.empty()) return Toast(TrF(L"Installing failed: {0}", LastLine(r.out)), 1);
            drvResults_ = res;
            drvReboot_ = text.find(L"Restart Windows") != std::wstring::npos;
            size_t ok = 0;
            for (auto& x : res)
                if (x.status == InstallStatus::Installed) {
                    ++ok;
                    drvReboot_ = drvReboot_ || x.reboot;
                    auto& ups = drvSearch_.updates;
                    ups.erase(std::remove_if(ups.begin(), ups.end(), [&](const DriverUpdate& u) { return u.updateId == x.updateId; }), ups.end());
                }
            drvSel_.clear();
            std::wstring msg = TrN(ok, L"{0} of {1} driver update installed", L"{0} of {1} driver updates installed", util::FormatCount(res.size()));
            if (drvReboot_) msg += L"  ·  " + Tr(L"restart Windows to finish");
            Toast(msg, ok == res.size() ? 0 : 1);
            lists_[static_cast<int>(View::Drivers)].scroll = 0;
            Invalidate();
        };
        StartTask(std::move(t), [st, cmd] { st->run = RunAsAdmin(cmd); });
    };
    ShowDialog(std::move(d));
}

void App::OnDriverCommand(int cmd) {
    if (cmd == CMD_DRV_CHECK) return CheckDriverUpdates();
    if (cmd == CMD_DRV_INSTALL) return StartDriverInstall();
    if (cmd == CMD_DRV_RESULTS_DONE) {
        drvResults_.clear();
        return Invalidate();
    }
    if (cmd >= CMD_DRV_TOOL) {
        size_t i = static_cast<size_t>(cmd - CMD_DRV_TOOL);
        if (i >= drvLinks_.size()) return;
        const DriverTool& t = drvLinks_[i];
        if (t.launch.empty()) return Toast(TrF(L"Start {0} from the Start menu", t.name), 2);
        HINSTANCE h = ShellExecuteW(hwnd_, L"open", t.launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(h) <= 32) Toast(TrF(L"Couldn't open {0}", t.launch), 1);
        return;
    }
    size_t i = static_cast<size_t>(cmd - CMD_DRV_ROW);
    if (i >= drvSearch_.updates.size()) return;
    if (!drvSel_.erase(i)) drvSel_.insert(i);
    Invalidate();
}

// =============================================================================================
// Drawing

void App::DrawDrivers(const RectF& area) {
    float top = area.top + 18;
    {
        float bx = area.right - 24;
        auto button = [&](const std::wstring& t, wchar_t icon, int cmd, int style) {
            float w = ButtonWidth(t, icon, 100);
            bx -= w;
            Button(RWH(bx, top + 6, w, 34), icon, t, cmd, style);
            bx -= 8;
        };
        if (!drvSel_.empty()) button(TrN(drvSel_.size(), L"Install {0} update", L"Install {0} updates"), IC_DRIVER, CMD_DRV_INSTALL, 1);
        button(drvSearched_ ? Tr(L"Check again") : Tr(L"Check Windows Update"), IC_REFRESH, CMD_DRV_CHECK, drvSearched_ ? 0 : 1);
        DrawViewHeader(R(area.left + 24, top, bx - 10, top + 46), Tr(L"Drivers"),
                       Dots({Tr(L"Driver updates from Windows Update, and your PC maker's and graphics vendor's own tools"),
                             Tr(L"works without a scan")}),
                       IC_DRIVER);
    }
    if (!drvLoaded_) return;

    // ---- What to draw, and how tall it is
    struct Act {
        std::wstring text;
        int cmd;
        int style;
    };
    struct Card {
        wchar_t icon;
        std::wstring title, desc, status;
        std::vector<Act> acts;
    };
    size_t link = 0;
    auto toolActs = [&](const std::vector<DriverTool>& tools, std::vector<Act>& acts) {
        bool first = true;
        for (auto& t : tools) {
            std::wstring text = t.installed ? TrF(L"Open {0}", t.name) : t.name.empty() ? Tr(L"Support page") : TrF(L"Get {0}", t.name);
            acts.push_back({text, CMD_DRV_TOOL + static_cast<int>(link++), first && t.installed ? 1 : 0});
            first = false;
        }
    };
    auto toolStatus = [&](const std::vector<DriverTool>& tools, const std::wstring& vendor) {
        std::vector<std::wstring> names;
        for (auto& t : tools)
            if (t.installed) names.push_back(t.name);
        std::wstring s;
        for (auto& n : names) s += (s.empty() ? L"" : L", ") + n;
        if (!s.empty()) return TrF(L"Installed: {0}", s);
        if (tools.empty() || tools[0].name.empty()) return TrF(L"{0} has no driver tool: its support page has the drivers", VendorName(vendor));
        return TrF(L"{0} isn't installed: get it from {1}'s site", tools[0].name, VendorName(vendor));
    };
    Card maker{IC_PC, L"", L"", L"", {}};
    bool haveMaker = !drvTools_.maker.name.empty();
    if (haveMaker) {
        const PcMaker& m = drvTools_.maker;
        std::wstring who = m.vendor.empty() ? m.name : VendorName(m.vendor);
        maker.title = m.model.empty() ? who : m.selfBuilt ? TrF(L"{0} motherboard {1}", who, m.model) : TrF(L"{0} {1}", who, m.model);
        if (m.vendor.empty()) {
            maker.desc = Tr(L"Bytopsy doesn't know this maker's driver tool: drivers Windows Update doesn't have are on the maker's support site.");
            maker.status = m.name;
        } else {
            std::wstring note = !drvTools_.makerTools.empty() ? drvTools_.makerTools[0].note : L"";
            maker.desc = !note.empty() ? note
                         : m.selfBuilt ? Tr(L"A self-built PC: chipset, audio, network and other drivers Windows Update doesn't have come from the "
                                            L"motherboard maker's tool or support page.")
                                       : Tr(L"Drivers Windows Update doesn't have (chipset, audio, utilities, firmware) usually come through "
                                            L"the PC maker's own tool.");
            maker.status = toolStatus(drvTools_.makerTools, m.vendor);
            toolActs(drvTools_.makerTools, maker.acts);
        }
    }
    std::vector<Card> gpuCards;
    for (auto& g : drvTools_.gpus) {
        Card c{IC_GPU, L"", L"", L"", {}};
        for (int a : g.adapters)
            if (a >= 0 && a < static_cast<int>(drvTools_.devices.size()))
                c.title += (c.title.empty() ? L"" : L", ") + drvTools_.devices[a].name;
        const DeviceDriver& first = drvTools_.devices[g.adapters[0]];
        c.desc = TrF(L"Graphics drivers come from {0}'s own tool, which also keeps them up to date. Bytopsy doesn't install them another way.",
                     VendorName(g.vendor));
        c.status = Dots({TrF(L"Driver {0}", VersionDate(first.version, first.date)), toolStatus(g.tools, g.vendor)});
        toolActs(g.tools, c.acts);
        gpuCards.push_back(c);
    }
    const auto& ups = drvSearch_.updates;
    bool listUpdates = drvSearched_ && drvSearch_.ok && !ups.empty();
    float resultsH = drvResults_.empty() ? 0 : 52 + drvResults_.size() * 26.0f + 12;
    // Cards grow with their description (translations are often longer).
    TextStyle wrap = Small(12.5f);
    wrap.wrap = true;
    wrap.center = false;
    auto textRightOf = [&](const Card& c) {
        float actsW = 0;
        for (auto& a : c.acts) actsW += gfx_.Measure(a.text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 44;
        return area.right - 24 - std::max(40.0f, actsW + 30);
    };
    auto descH = [&](const Card& c) { return std::max(20.0f, gfx_.MeasureHeight(c.desc, textRightOf(c) - (area.left + 96), wrap)); };
    auto cardH = [&](const Card& c) { return std::max(kCardH, 34 + descH(c) + 32); };
    float cardsH = haveMaker ? kSectionH + cardH(maker) + 10 : 0;
    if (!gpuCards.empty()) cardsH += kSectionH;
    for (auto& c : gpuCards) cardsH += cardH(c) + 10;
    float contentH = cardsH + kSectionH + resultsH + (listUpdates ? ups.size() * kUpdStep : 90.0f) + 20;

    float y = top + 62;
    RectF body = R(area.left, y, area.right, area.bottom);
    ListState& ls = lists_[static_cast<int>(View::Drivers)];
    ls.scroll = std::clamp(ls.scroll, 0.0f, std::max(0.0f, contentH - H(body)));
    gfx_.PushClip(body);
    y -= ls.scroll;

    auto section = [&](const std::wstring& title, const std::wstring& hint) {
        TextStyle ts = TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float tw = gfx_.Measure(title, ts) + 4;
        gfx_.Text(title, R(area.left + 26, y, area.left + 26 + tw, y + 26), P_.textDim, ts);
        if (!hint.empty()) gfx_.Text(hint, R(area.left + 40 + tw, y, area.right - 26, y + 26), P_.textFaint, Small(12, DWRITE_FONT_WEIGHT_NORMAL, Align::Right));
        y += kSectionH;
    };
    auto drawCard = [&](const Card& c) {
        float h = cardH(c), dh = descH(c);
        RectF card = R(area.left + 24, y, area.right - 24, y + h);
        if (card.bottom >= body.top && card.top <= body.bottom) {
            gfx_.FillRound(card, 12, P_.surface);
            gfx_.StrokeRound(card, 12, P_.border);
            float x = card.left + 18;
            gfx_.FillRound(RWH(x, card.top + 18, 40, 40), 10, WithAlpha(P_.accent, 0.14f));
            gfx_.Icon(c.icon, RWH(x, card.top + 18, 40, 40), P_.accent, 16);
            x += 54;
            float textRight = textRightOf(c);
            gfx_.Text(c.title, R(x, card.top + 10, textRight, card.top + 32), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
            gfx_.Text(c.desc, R(x, card.top + 34, textRight, card.top + 34 + dh + 2), P_.textDim, wrap);
            gfx_.Text(c.status, R(x, card.top + 38 + dh, textRight, card.top + 60 + dh), P_.textFaint, Small(12));
            float bx = card.right - 18;
            for (auto it = c.acts.rbegin(); it != c.acts.rend(); ++it) {
                float w = gfx_.Measure(it->text, TS(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 36;
                bx -= w;
                Button(RWH(bx, card.top + (h - 32) / 2, w, 32), 0, it->text, it->cmd, it->style);
                bx -= 8;
            }
        }
        y += h + 10;
    };

    if (haveMaker) {
        section(Tr(L"Your PC maker"), L"");
        drawCard(maker);
    }
    if (!gpuCards.empty()) {
        section(Tr(L"Graphics"), L"");
        for (auto& c : gpuCards) drawCard(c);
    }

    section(Tr(L"Windows Update"),
            listUpdates ? Dots({TrN(ups.size(), L"{0} driver update offered", L"{0} driver updates offered"),
                                Tr(L"newer drivers for connected devices are checked"), Tr(L"click one to check or uncheck it")})
                        : L"");

    // The last install's outcome.
    if (!drvResults_.empty()) {
        RectF b = R(area.left + 24, y, area.right - 24, y + resultsH - 12);
        gfx_.FillRound(b, 10, WithAlpha(drvReboot_ ? P_.warning : P_.success, 0.12f));
        std::wstring done = Tr(L"Done");
        float dw = ButtonWidth(done, 0, 70);
        Button(RWH(b.right - 12 - dw, b.top + 9, dw, 32), 0, done, CMD_DRV_RESULTS_DONE, 0);
        gfx_.Text(drvReboot_ ? Tr(L"Installed through Windows Update: restart Windows to finish") : Tr(L"Installed through Windows Update"),
                  R(b.left + 16, b.top + 8, b.right - 24 - dw, b.top + 42), P_.text, TS(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        float ry = b.top + 48;
        for (auto& r : drvResults_) {
            bool ok = r.status == InstallStatus::Installed;
            D2D1_COLOR_F col = ok ? P_.success : r.status == InstallStatus::Skipped ? P_.textFaint : P_.danger;
            gfx_.Icon(ok ? IC_OK : IC_WARN, RWH(b.left + 14, ry, 20, 24), col, 12);
            std::wstring st = ok ? (r.reboot ? Tr(L"Installed, restart needed") : Tr(L"Installed"))
                                 : r.status == InstallStatus::Skipped ? TrF(L"Skipped: {0}", r.message)
                                                                      : TrF(L"Failed: {0}", r.message);
            float half = (b.right - b.left - 60) * 0.5f;
            gfx_.Text(r.title.empty() ? r.updateId : r.title, R(b.left + 40, ry, b.left + 40 + half, ry + 24), P_.text, Small(12.5f));
            gfx_.Text(st, R(b.left + 50 + half, ry, b.right - 16, ry + 24), col, Small(12.5f));
            ry += 26;
        }
        y += resultsH;
    }

    if (!listUpdates) {
        RectF card = R(area.left + 24, y, area.right - 24, y + 76);
        gfx_.FillRound(card, 12, P_.surface);
        gfx_.StrokeRound(card, 12, P_.border);
        std::wstring title, text;
        D2D1_COLOR_F tone = P_.accent;
        if (!drvSearched_) {
            title = Tr(L"Windows Update hasn't been asked yet");
            text = Tr(L"“Check Windows Update” asks it which driver updates it has for this PC's devices. Windows does the asking, as "
                      L"Settings > Windows Update does; nothing is downloaded until you choose to install.");
        } else if (!drvSearch_.ok) {
            title = Tr(L"Windows Update couldn't be asked");
            text = drvSearch_.error;
            tone = P_.danger;
        } else {
            title = Tr(L"Windows Update offers no driver updates for this PC");
            text = Tr(L"Drivers it doesn't offer come from the PC maker's and graphics vendor's tools above.");
            tone = P_.success;
        }
        gfx_.FillRound(RWH(card.left + 18, card.top + 18, 40, 40), 10, WithAlpha(tone, 0.14f));
        gfx_.Icon(drvSearched_ && drvSearch_.ok ? IC_OK : IC_UPDATE, RWH(card.left + 18, card.top + 18, 40, 40), tone, 16);
        gfx_.Text(title, R(card.left + 72, card.top + 10, card.right - 18, card.top + 32), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        gfx_.Text(text, R(card.left + 72, card.top + 34, card.right - 18, card.bottom - 6), P_.textDim, wrap);
        gfx_.PopClip();
        return;
    }

    for (size_t i = 0; i < ups.size(); ++i, y += kUpdStep) {
        RectF card = R(area.left + 24, y, area.right - 24, y + kUpdH);
        if (card.bottom < body.top || card.top > body.bottom) continue;
        const DriverUpdate& u = ups[i];
        const DeviceDriver* dev = u.device >= 0 && u.device < static_cast<int>(drvSearch_.devices.size()) ? &drvSearch_.devices[u.device] : nullptr;
        bool on = drvSel_.count(i) > 0;
        bool hover = Hovered(HK_BUTTON, CMD_DRV_ROW + static_cast<int>(i));
        gfx_.FillRound(card, 12, on ? Mix(P_.surface, P_.accent, 0.06f) : hover ? P_.hover : P_.surface);
        gfx_.StrokeRound(card, 12, on ? WithAlpha(P_.accent, 0.5f) : P_.border);
        AddHit(card, HK_BUTTON, CMD_DRV_ROW + static_cast<int>(i));
        Checkbox(RWH(card.left + 18, card.top + 16, 18, 18), on, hover);

        // Right: size and how the offer compares.
        std::wstring size = util::FormatSizeShort(u.bytes);
        std::wstring age;
        D2D1_COLOR_F ageCol = P_.textFaint;
        switch (u.age) {
            case OfferAge::Newer: age = Tr(L"Newer"), ageCol = P_.success; break;
            case OfferAge::Same: age = Tr(L"Same version"); break;
            case OfferAge::Older: age = Tr(L"Older than installed"), ageCol = P_.warning; break;
            default: age = Tr(L"Can't compare"); break;
        }
        float rw = std::max(gfx_.Measure(age, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD)), gfx_.Measure(size, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD))) + 8;
        rw = std::min(rw, 220.0f);
        gfx_.Text(size, R(card.right - 18 - rw, card.top + 10, card.right - 18, card.top + 32), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));
        gfx_.Text(age, R(card.right - 18 - rw, card.top + 34, card.right - 18, card.top + 54), ageCol, Small(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Right));

        float x = card.left + 50, right = card.right - 30 - rw;
        gfx_.Text(u.title, R(x, card.top + 8, right, card.top + 30), P_.text, TS(14, DWRITE_FONT_WEIGHT_SEMI_BOLD));
        std::wstring devText = dev ? dev->name + (dev->present ? L"" : L" " + Tr(L"(not connected)")) : Tr(L"No device with this hardware id found");
        gfx_.Text(Dots({devText, u.driverClass, u.provider}), R(x, card.top + 31, right, card.top + 51), P_.textDim, Small(12.5f));
        gfx_.Text(TrF(L"Installed {0}  →  offered {1}", dev ? VersionDate(dev->version, dev->date) : Tr(L"nothing"), VersionDate(u.version, u.date)),
                  R(x, card.top + 52, right, card.top + 72), P_.text, Small(12.5f));
        std::wstring vendorTool;
        if (u.display) {
            std::wstring v = GpuVendorFromHardwareId(u.hardwareId);
            for (auto& g : drvTools_.gpus)
                if (g.vendor == v && !g.tools.empty() && !g.tools[0].name.empty()) vendorTool = g.tools[0].name;
            if (vendorTool.empty()) vendorTool = v.empty() ? Tr(L"the GPU vendor's tool") : VendorName(v);
        }
        std::wstring notes = Dots({u.display ? TrF(L"Graphics driver: {0} is recommended instead", vendorTool) : L"",
                                   u.reboot == 1 ? Tr(L"needs a restart") : u.reboot == 2 ? Tr(L"may need a restart") : L"",
                                   u.optional ? Tr(L"optional") : L"", u.hidden ? Tr(L"hidden in Windows Update") : L"",
                                   !u.eulaAccepted ? Tr(L"license terms not accepted yet") : L"",
                                   u.devices > 1 ? TrN(u.devices - 1, L"and {0} more device", L"and {0} more devices") : L""});
        gfx_.Text(notes, R(x, card.top + 73, card.right - 18, card.top + 93), u.display || u.age == OfferAge::Older ? P_.warning : P_.textFaint,
                  Small(12));
    }
    gfx_.PopClip();
}
