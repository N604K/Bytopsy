#include "app.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>

#include "../util.h"

#pragma comment(lib, "comctl32.lib")

namespace {
const wchar_t* kClass = L"Bytopsy.MainWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr float kSidebarW = 240.0f;
constexpr float kTopH = 56.0f;
}  // namespace

// =============================================================================================
// Creation

bool App::Create(HINSTANCE inst, const std::wstring& path, const std::wstring& importFile, int show, bool updated) {
    inst_ = inst;
    acc_ = std::make_unique<AccBridge>(static_cast<AccHost*>(this));
    settings_ = LoadSettings();
    util::g_si = settings_.si;
    elevated_ = IsElevated();
    MigrateShellIntegration();
    // Numbers and dates in the Windows regional format; the language from the settings.
    wchar_t dec[8] = L".", grp[8] = L",";
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8)) wcscpy_s(dec, L".");
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, grp, 8)) wcscpy_s(grp, L",");
    util::SetNumberFormat(dec, grp);
    ApplyLanguage(false);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    UINT sysDpi = GetDpiForSystem();
    int w = MulDiv(1320, sysDpi, 96), h = MulDiv(840, sysDpi, 96);
    std::wstring title = elevated_ ? Tr(L"Bytopsy (Administrator)") : L"Bytopsy";
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                            inst, this);
    if (!hwnd_) return false;
    if (settings_.hasWindow) {
        RECT r = settings_.window;
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            GetWindowPlacement(hwnd_, &wp);
            wp.rcNormalPosition = r;
            wp.showCmd = SW_HIDE;
            SetWindowPlacement(hwnd_, &wp);
        }
    }
    gfx_.SetDpi(static_cast<float>(GetDpiForWindow(hwnd_)));
    if (!gfx_.Init(hwnd_)) {
        MessageBoxW(hwnd_, Tr(L"Direct2D could not be initialized.").c_str(), L"Bytopsy", MB_ICONERROR);
        return false;
    }
    gfx_.SetLocale(i18n::CurrentCode() == i18n::kPseudoCode ? L"en-US" : i18n::CurrentCode());
    ApplyTheme();
    CreateSearchBox();
    DragAcceptFiles(hwnd_, TRUE);
    // Allow drag & drop from a non-elevated Explorer into an elevated window.
    ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);

    drives_ = ListDrives();
    LoadHistory();
    // As started (a shortcut set to "Minimized", a launcher that doesn't want focus taken), except
    // that a plain start restores the last maximized state.
    bool plain = show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT;
    ShowWindow(hwnd_, plain ? (settings_.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL) : show);
    UpdateWindow(hwnd_);
    if (!toast_.text.empty()) EnsureTimer();  // a problem with the language file
    StartupUpdates(updated);

    if (!importFile.empty())
        StartImport(importFile);
    else if (!path.empty())
        StartScan(path);
    // Update checks: asked once (not over a scan that is starting: then on a later start).
    if (!settings_.updatesAsked) {
        if (path.empty() && importFile.empty()) AskAboutUpdates();
    } else {
        MaybeCheckForUpdates();
    }
    return true;
}

int App::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void App::CreateSearchBox() {
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_, nullptr, inst_,
                              nullptr);
    SetWindowSubclass(search_, SearchProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(SearchCue().c_str()));
    AccNameSearchBox(true);
    UpdateSearchBox();
}

std::wstring App::SearchCue() const {
    return searchForApps_ ? Tr(L"Filter apps by name or publisher") : Tr(L"Search files & folders   e.g.  *.iso   >1G   older:1y   type:d");
}

void App::ApplyLanguage(bool redraw) {
    std::vector<std::wstring> problems;
    std::wstring code = i18n::LoadLanguage(settings_.language, &problems);
    if (!redraw) {
        // At start: problems in a translation someone is trying out are shown once the window is up.
        if (!problems.empty())
            toast_ = {TrN(problems.size(), L"The language file has {0} problem: {1}", L"The language file has {0} problems, the first: {1}",
                          problems[0]),
                      1, GetTickCount64() + 12000};
        return;
    }
    gfx_.SetLocale(code == i18n::kPseudoCode ? L"en-US" : code);
    typeNames_.clear();
    fitCache_.clear();
    SetWindowTextW(hwnd_, elevated_ ? Tr(L"Bytopsy (Administrator)").c_str() : L"Bytopsy");
    if (search_) SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(SearchCue().c_str()));
    if (!problems.empty())
        Toast(TrN(problems.size(), L"The language file has {0} problem: {1}", L"The language file has {0} problems, the first: {1}", problems[0]),
              1);
    rowsDirty_ = true;
    tmDirty_ = true;
    Invalidate();
}

void App::UpdateSearchBox() {
    if (!search_) return;
    if (searchFont_) DeleteObject(searchFont_);
    searchFont_ = CreateFontW(-Dip2Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont_), TRUE);
    SendMessageW(search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    OnSize();
}

void App::ApplyTheme() {
    bool dark = settings_.theme == 2 || (settings_.theme == 0 && SystemUsesDarkTheme());
    P_ = dark ? DarkPalette() : LightPalette();
    ApplyWindowTheme(hwnd_, dark, P_.surfaceRgb);
    SetMenuTheme(dark);
    if (searchBrush_) DeleteObject(searchBrush_);
    uint32_t inputRgb = dark ? 0x272933 : 0xEEF0F6;
    searchBrush_ = CreateSolidBrush(RGB((inputRgb >> 16) & 0xFF, (inputRgb >> 8) & 0xFF, inputRgb & 0xFF));
    tmDirty_ = true;
    if (search_) InvalidateRect(search_, nullptr, TRUE);
    Invalidate();
}

void App::OnSize() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float w = Px2Dip(rc.right);
    float left = kSidebarW + 20;
    float boxW = std::clamp(w - left - 230, 200.0f, 620.0f);
    // The edit control sits inside the rounded search box drawn by DrawTopBar.
    float textH = 20;
    float y = (kTopH - textH) / 2 + 1;
    if (search_) MoveWindow(search_, Dip2Px(left + 38), Dip2Px(y), Dip2Px(boxW - 38 - 34), Dip2Px(textH), TRUE);
    tmDirty_ = true;
}

void App::EnsureTimer() {
    if (!timerOn_) {
        SetTimer(hwnd_, kTimerId, 33, nullptr);
        timerOn_ = true;
    }
}

void App::OnTimer() {
    bool animating = task_ != nullptr || cleanupMeasuring_ || !toast_.text.empty();
    if (!toast_.text.empty() && GetTickCount64() > toast_.until + 400) toast_.text.clear();
    if (!animating) {
        KillTimer(hwnd_, kTimerId);
        timerOn_ = false;
    }
    Invalidate();
}

// =============================================================================================
// Window procedure

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app;
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd_ = hwnd;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::SearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    App* app = reinterpret_cast<App*>(ref);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN && app->searchForApps_) {  // the Apps view filters as you type
            SetFocus(app->hwnd_);
            return 0;
        }
        if (wp == VK_RETURN) {
            int n = GetWindowTextLengthW(hwnd);
            std::wstring text(n, L'\0');
            GetWindowTextW(hwnd, text.data(), n + 1);
            app->RunSearchQuery(text);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            SetFocus(app->hwnd_);
            app->Invalidate();
            return 0;
        }
        if (wp == VK_DOWN) {
            SetFocus(app->hwnd_);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 0x1B)) return 0;
    if (msg == WM_CHAR || msg == WM_KEYUP) app->Invalidate();  // clear button visibility
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            // Photo previews are decoded a few per frame: paint again until the visible ones are done.
            if (thumbsPending_) {
                thumbsPending_ = false;
                Invalidate();
            }
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            gfx_.Resize();
            OnSize();
            Invalidate();
            return 0;
        case WM_DPICHANGED: {
            gfx_.SetDpi(static_cast<float>(HIWORD(wp)));
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            icons_.Clear();
            UpdateSearchBox();
            tmDirty_ = true;
            Invalidate();
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && lp == 0 && LOWORD(wp) >= CMD_HOME) {  // menu / accelerator commands
                OnCommand(LOWORD(wp));
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE && reinterpret_cast<HWND>(lp) == search_) {
                OnSearchChanged();
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = GetDpiForWindow(hwnd_);
            mmi->ptMinTrackSize.x = MulDiv(940, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(600, dpi, 96);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            OnMouseMove(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp)));
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (dragKind_ == HK_NONE) {
                hover_ = Hit{};
                tmHover_ = -1;
                mouseX_ = mouseY_ = -1;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd_);
            OnLButtonDown(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp)), msg == WM_LBUTTONDBLCLK);
            return 0;
        case WM_LBUTTONUP: OnLButtonUp(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_RBUTTONUP: OnRButtonUp(Px2Dip(GET_X_LPARAM(lp)), Px2Dip(GET_Y_LPARAM(lp))); return 0;
        case WM_XBUTTONUP:
            OnCommand(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? CMD_BACK : CMD_FORWARD);
            return TRUE;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            OnWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)), Px2Dip(pt.x), Px2Dip(pt.y));
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp != VK_LEFT && wp != VK_RIGHT && wp != VK_UP) break;
            OnKeyDown(wp);
            return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wp) == hwnd_ && GET_X_LPARAM(lp) == -1) {
                // Keyboard invoked: open at the focused row.
                POINT pt{Dip2Px(listArea_.left + 60), Dip2Px(listArea_.top + 40)};
                ClientToScreen(hwnd_, &pt);
                ShowContextMenu(pt.x, pt.y);
                return 0;
            }
            break;
        case WM_TIMER: OnTimer(); return 0;
        case WM_APP_TASKDONE: OnTaskDone(); return 0;
        case WM_APP_CLEANUP: OnCleanupMeasured(reinterpret_cast<std::vector<CleanCategory>*>(lp)); return 0;
        case WM_APP_UNINSTALL: OnAppUninstallProgress(); return 0;
        case WM_APP_UPDATE: {
            std::unique_ptr<UpdateCheck> r(reinterpret_cast<UpdateCheck*>(lp));
            updateCheckRunning_ = false;
            if (r) OnUpdateChecked(*r, false);
            return 0;
        }
        case WM_GETOBJECT: {
            LRESULT r = 0;
            bool first = acc_ && !acc_->Active();
            if (acc_ && acc_->OnGetObject(wp, lp, &r)) {
                if (first) Invalidate();  // start recording what is drawn
                return r;
            }
            break;
        }
        case WM_APP_ACC: AccEvents(); return 0;
        case WM_APP_ACCDO: AccRun(static_cast<uint64_t>(lp), static_cast<AccAct>(wp)); return 0;
        case WM_DROPFILES: OnDrop(reinterpret_cast<HDROP>(wp)); return 0;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lp) == search_) {
                HDC dc = reinterpret_cast<HDC>(wp);
                auto toRef = [](D2D1_COLOR_F c) {
                    return RGB(static_cast<int>(c.r * 255), static_cast<int>(c.g * 255), static_cast<int>(c.b * 255));
                };
                SetTextColor(dc, toRef(P_.text));
                SetBkColor(dc, P_.dark ? RGB(0x27, 0x29, 0x33) : RGB(0xEE, 0xF0, 0xF6));
                return reinterpret_cast<LRESULT>(searchBrush_);
            }
            break;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && settings_.theme == 0)
                ApplyTheme();
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (hover_.kind == HK_SPLITTER || dragKind_ == HK_SPLITTER)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd_) {
                bool link = hover_.kind == HK_BUTTON || hover_.kind == HK_NAV || hover_.kind == HK_DRIVE ||
                            hover_.kind == HK_CARD || hover_.kind == HK_RECENT || hover_.kind == HK_CHIP ||
                            hover_.kind == HK_CRUMB || hover_.kind == HK_CHECK || hover_.kind == HK_SNAPSHOT ||
                            hover_.kind == HK_DIALOG || hover_.kind == HK_RESTORE || hover_.kind == HK_REVIEW ||
                            hover_.kind == HK_DIALOG_CHOICE || hover_.kind == HK_SWITCH;
                SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_CLOSE: {
            WINDOWPLACEMENT wp2{sizeof(wp2)};
            if (GetWindowPlacement(hwnd_, &wp2)) {
                settings_.window = wp2.rcNormalPosition;
                settings_.hasWindow = true;
                settings_.maximized = wp2.showCmd == SW_SHOWMAXIMIZED;
            }
            SaveSettings(settings_);
            if (appWait_) appWait_->stop = true;  // its worker only waits; the uninstaller itself keeps running
            if (task_) {
                if (task_->cancel) task_->cancel();
                WaitForSingleObject(task_->thread, 2000);
                ExitProcess(0);  // never free the tree under a running worker
            }
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_DESTROY:
            AccNameSearchBox(false);
            if (acc_) acc_->Detach();
            SafeRelease(tmBitmap_);
            icons_.Clear();
            ClearThumbs();
            gfx_.Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// =============================================================================================
// Mouse

const Hit* App::HitAt(float x, float y) {
    size_t lo = dialog_.active || task_ ? std::min(modalHitStart_, hits_.size()) : 0;
    for (size_t i = hits_.size(); i-- > lo;)
        if (Inside(hits_[i].r, x, y)) return &hits_[i];
    return nullptr;
}

void App::OnMouseMove(float x, float y) {
    mouseX_ = x;
    mouseY_ = y;
    if (dragKind_ == HK_SPLITTER) {
        float top = contentArea_.top + 120, bottom = contentArea_.bottom - 80;
        float yy = std::clamp(y, top, bottom);
        settings_.split = std::clamp((yy - contentArea_.top) / std::max(1.0f, H(contentArea_)), 0.2f, 0.9f);
        tmDirty_ = true;
        Invalidate();
        return;
    }
    if (dragKind_ == HK_SCROLL) {
        float viewH = H(listArea_);
        float ratio = listContentH_ / std::max(1.0f, viewH);
        lists_[static_cast<int>(view_)].scroll = dragValue_ + (y - dragStart_) * ratio;
        Invalidate();
        return;
    }
    const Hit* h = HitAt(x, y);
    Hit nh = h ? *h : Hit{};
    bool changed = nh.kind != hover_.kind || nh.a != hover_.a || nh.b != hover_.b;
    hover_ = nh;
    int tmi = nh.kind == HK_TREEMAP ? HitTreemap(tm_, x, y) : -1;
    if (tmi != tmHover_ || tmi >= 0) {
        tmHover_ = tmi;
        changed = true;
    }
    if (changed) Invalidate();
}

void App::OnLButtonDown(float x, float y, bool dbl) {
    const Hit* h = HitAt(x, y);
    if (!h) return;
    Hit hit = *h;
    if (hit.kind == HK_BADGE) {  // a row's shield badge clicks like the row
        if (hit.a < 0) return;
        hit.kind = HK_ROW;
    }
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (hit.kind) {
        case HK_SCROLL: {
            dragKind_ = HK_SCROLL;
            dragStart_ = y;
            ListState& ls = lists_[static_cast<int>(view_)];
            float viewH = H(listArea_);
            float thumbH = std::max(28.0f, viewH * viewH / std::max(viewH, listContentH_));
            float thumbY = listArea_.top + (viewH - thumbH) * (ls.scroll / std::max(1.0f, listContentH_ - viewH));
            if (y < thumbY || y > thumbY + thumbH) {
                // Jump so the thumb centers on the click.
                ls.scroll = (y - listArea_.top - thumbH / 2) / std::max(1.0f, viewH - thumbH) * (listContentH_ - viewH);
            }
            dragValue_ = ls.scroll;
            SetCapture(hwnd_);
            break;
        }
        case HK_SPLITTER:
            dragKind_ = HK_SPLITTER;
            SetCapture(hwnd_);
            break;
        case HK_TREEMAP: {
            int i = HitTreemap(tm_, x, y);
            if (i < 0) break;
            Node* n = tm_[i].node;
            if (dbl) {
                // Zoom into the folder (the item's own folder for files).
                Node* target = n->CanEnter() ? n : n->parent;
                if (target && target != tmRoot_) NavigateTo(target);
            } else {
                RevealInBrowse(n);
            }
            break;
        }
        default: ActivateHit(hit, dbl, ctrl, shift); break;
    }
    Invalidate();
}

// A click on a region (also what UI Automation's Invoke / Toggle / Select run).
void App::ActivateHit(const Hit& hit, bool dbl, bool ctrl, bool shift) {
    switch (hit.kind) {
        case HK_BUTTON: OnCommand(hit.a); break;
        case HK_NAV: ShowView(static_cast<View>(hit.a)); break;
        case HK_DRIVE:
            if (hit.a >= 0 && hit.a < static_cast<int>(drives_.size())) ConfirmNewScan(drives_[hit.a].root);
            break;
        case HK_CARD:
            if (hit.a < static_cast<int>(drives_.size()))
                ConfirmNewScan(drives_[hit.a].root);
            else if (hit.a == 1000)
                OnCommand(CMD_SCAN_FOLDER);
            else if (hit.a == 1001)
                OnCommand(CMD_OPEN_SCAN);
            else if (hit.a == 1002)
                ShowView(View::Cleanup);
            else if (hit.a == 1003)
                OnCommand(CMD_ADMIN);
            break;
        case HK_RECENT:
            if (hit.a >= 0 && hit.a < static_cast<int>(snapshots_.size())) ConfirmNewScan(snapshots_[hit.a].root);
            break;
        case HK_CRUMB: NavigateTo(reinterpret_cast<Node*>(hit.b)); break;
        case HK_ROW:
            if (view_ == View::CleanReview) {
                // A click toggles the check; a double-click shows the item (and undoes its first click).
                ToggleKeep(hit.a);
                if (dbl) OpenRow(hit.a);
                break;
            }
            if (view_ == View::Startup) {
                // A click focuses the row; its switch turns it off/on; a double-click shows the program.
                if (hit.a >= 0 && hit.a < static_cast<int>(rows_.size()) && rows_[hit.a].kind == RK_STARTUP)
                    lists_[static_cast<int>(view_)].focus = hit.a;
                if (dbl) OpenRow(hit.a);
                break;
            }
            if (view_ == View::Games) {
                // The first click of a double-click already chose the row.
                if (dbl)
                    OpenRow(hit.a);
                else
                    OnGameRow(hit.a);
                break;
            }
            if (view_ == View::Apps) {
                if (dbl)
                    OpenRow(hit.a);
                else
                    OnAppRow(hit.a);
                break;
            }
            if (view_ == View::Wsl) {
                ToggleWslRow(hit.a);
                if (dbl) OpenRow(hit.a);
                break;
            }
            if (view_ == View::AiModels) {
                // As in Review: a click toggles, a double-click shows the files (undoing its first click).
                ToggleAiRow(hit.a);
                if (dbl) OpenRow(hit.a);
                break;
            }
            if (view_ == View::DevJunk && hit.a >= 0 && hit.a < static_cast<int>(rows_.size()) && rows_[hit.a].kind == RK_GROUP) {
                if (dbl)
                    RevealInBrowse(devJunk_[rows_[hit.a].idx].dir);
                else
                    ToggleDevProject(rows_[hit.a].idx);
                lists_[static_cast<int>(view_)].focus = hit.a;
                break;
            }
            if (dbl)
                OpenRow(hit.a);
            else
                SelectRow(hit.a, ctrl, shift);
            break;
        case HK_EXPANDER:
            if (hit.a < static_cast<int>(rows_.size())) {
                Node* n = rows_[hit.a].node;
                if (!expanded_.erase(n)) expanded_.insert(n);
                rowsDirty_ = true;
            }
            break;
        case HK_HEADER:
            if (view_ == View::Apps) {
                if (appSortKey_ == hit.a)
                    appSortDesc_ = !appSortDesc_;
                else {
                    appSortKey_ = hit.a;
                    appSortDesc_ = hit.a == SK_SIZE || hit.a == SK_MTIME;
                }
                SortApps();
            } else if (view_ == View::Browse) {
                if (sortKey_ == hit.a)
                    sortDesc_ = !sortDesc_;
                else {
                    sortKey_ = hit.a;
                    sortDesc_ = hit.a != SK_NAME;
                }
                rowsDirty_ = true;
            } else {
                if (resSortKey_ == hit.a)
                    resSortDesc_ = !resSortDesc_;
                else {
                    resSortKey_ = hit.a;
                    resSortDesc_ = hit.a != SK_NAME && hit.a != SK_FOLDER;
                }
                SortResults();
            }
            break;
        case HK_CHIP:
            if (hit.a == 1) dupMin_ = static_cast<uint64_t>(hit.b);
            if (hit.a == 2) {
                oldAgeDays_ = static_cast<int>(hit.b);
                RebuildOld();
            }
            if (hit.a == 3) {
                oldMinSize_ = static_cast<uint64_t>(hit.b);
                RebuildOld();
            }
            if (hit.a == 4) {
                devJunkByAge_ = hit.b != 0;
                SortDevJunk();
            }
            if (hit.a == 5) {
                gamesByAge_ = hit.b != 0;
                SortGames();
            }
            if (hit.a == 41 && appShowUpdates_ != (hit.b != 0)) {  // Apps: all apps / updates
                appShowUpdates_ = hit.b != 0;
                lists_[static_cast<int>(View::Apps)] = ListState{};
                rowsDirty_ = true;
                if (appShowUpdates_ && !appUpdChecked_) CheckAppUpdates();
            }
            if (hit.a == 40) {  // Startup: "Show Microsoft's"
                startupShowMs_ = !startupShowMs_;
                lists_[static_cast<int>(View::Startup)].focus = -1;
                rowsDirty_ = true;
            }
            if (hit.a == 6 && dupPhotos_ != (hit.b != 0)) {  // Duplicates view: identical files / similar photos
                dupPhotos_ = hit.b != 0;
                sel_.clear();  // a selection made in the other mode would be invisible here
                rowsDirty_ = true;
                lists_[static_cast<int>(View::Duplicates)] = ListState{};
            }
            if (hit.a == 7 && photoMatch_ != static_cast<PhotoMatch>(hit.b)) {
                photoMatch_ = static_cast<PhotoMatch>(hit.b);
                // Hashes are remembered, so grouping again with the new threshold is quick.
                if (photosDone_) StartSimilarPhotos();
            }
            if (hit.a == 8) photoMin_ = static_cast<uint64_t>(hit.b);
            break;
        case HK_CHECK:
            if (hit.a >= 0 && hit.a < static_cast<int>(cleanup_.size())) cleanup_[hit.a].selected = !cleanup_[hit.a].selected;
            break;
        case HK_RESTORE: RestoreDeleted(hit.a); break;
        case HK_SWITCH:
            if (!dbl) OnStartupSwitch(hit.a);
            break;
        case HK_REVIEW:
            reviewCat_ = hit.a;
            lists_[static_cast<int>(View::CleanReview)] = ListState{};
            ShowView(View::CleanReview);
            break;
        case HK_SNAPSHOT:
            if (hit.b == 0)
                CompareSnapshot(hit.a);
            else if (hit.a >= 0 && hit.a < static_cast<int>(snapshots_.size())) {
                DeleteSnapshot(snapshots_[hit.a].file);
                LoadHistory();
            }
            break;
        case HK_DIALOG:
            if (hit.a == -1) {
                dialog_.checked = !dialog_.checked;
                if (dialog_.onCheck) dialog_.onCheck(dialog_);
            } else {
                Dialog d = std::move(dialog_);
                dialog_ = Dialog{};
                if (d.onClose) d.onClose(hit.a, d.checked);
                if (d.onCloseWith) d.onCloseWith(hit.a, d);
            }
            break;
        case HK_DIALOG_CHOICE:
            if (hit.a >= 0 && hit.a < static_cast<int>(dialog_.choices.size())) {
                DialogChoice& ch = dialog_.choices[hit.a];
                int n = static_cast<int>(ch.options.size());
                if (n) ch.value = static_cast<int>((hit.b % n + n) % n);  // steppers wrap around
                if (dialog_.onChoice) dialog_.onChoice(dialog_);
            }
            break;
        default: break;
    }
    Invalidate();
}

void App::OnLButtonUp(float, float) {
    if (dragKind_ != HK_NONE) {
        ReleaseCapture();
        dragKind_ = HK_NONE;
        Invalidate();
    }
}

void App::OnRButtonUp(float x, float y) {
    const Hit* h = HitAt(x, y);
    if (!h) return;
    Hit hit = *h;
    if (hit.kind == HK_BADGE && hit.a >= 0) hit.kind = HK_ROW;
    if (view_ == View::CleanReview) return;
    if (hit.kind == HK_ROW) {
        if (hit.a < static_cast<int>(rows_.size())) {
            Node* n = rows_[hit.a].node;
            if (!n || !sel_.count(n)) SelectRow(hit.a, false, false);
        }
    } else if (hit.kind == HK_TREEMAP) {
        int i = HitTreemap(tm_, x, y);
        if (i < 0) return;
        RevealInBrowse(tm_[i].node);
    } else {
        return;
    }
    Invalidate();
    POINT pt{Dip2Px(x), Dip2Px(y)};
    ClientToScreen(hwnd_, &pt);
    ShowContextMenu(pt.x, pt.y);
}

void App::OnWheel(float delta, float x, float y) {
    if (dialog_.active || task_) return;
    if (x < kSidebarW && y > kTopH) {  // the sidebar scrolls when the window is too short for it
        sidebarScroll_ -= delta / 120.0f * 38.0f * 2;
        Invalidate();
        return;
    }
    lists_[static_cast<int>(view_)].scroll -= delta / 120.0f * 32.0f * 3;
    Invalidate();
}

// =============================================================================================
// Keyboard

void App::OnKeyDown(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (task_) {
        if (vk == VK_ESCAPE && task_->cancel && !task_->cancelRequested) {
            task_->cancelRequested = true;
            task_->cancel();
        }
        return;
    }
    if (dialog_.active) {
        if (DialogKey(vk, shift)) return;
        int focus = DialogFocus();
        bool onButton = focus >= 0 && focus < static_cast<int>(dialog_.buttons.size());
        int btn = -1;
        if (vk == VK_ESCAPE) {
            // Escape = the non-primary (cancel) button, or just close.
            btn = -2;
            if (dialog_.cancel >= 0) btn = dialog_.cancel;
            for (int i = 0; btn == -2 && i < static_cast<int>(dialog_.buttons.size()); ++i)
                if (i != dialog_.primary) {
                    btn = i;
                    break;
                }
        }
        // Enter presses the focused button: the primary one unless Tab moved on.
        if (vk == VK_RETURN) btn = onButton ? focus : (dialog_.primary >= 0 ? dialog_.primary : 0);
        if (vk == VK_SPACE && dialog_.kbd && onButton) btn = focus;
        if (vk == VK_SPACE && btn < 0 && dialog_.hasCheck && (!dialog_.kbd || focus == -1)) {
            dialog_.checked = !dialog_.checked;
            if (dialog_.onCheck) dialog_.onCheck(dialog_);
            Invalidate();
            return;
        }
        if (btn != -1) {
            Dialog d = std::move(dialog_);
            dialog_ = Dialog{};
            if (d.onClose && btn >= 0) d.onClose(btn, d.checked);
            if (d.onCloseWith && btn >= 0) d.onCloseWith(btn, d);
            Invalidate();
        }
        return;
    }
    if (alt && vk == VK_LEFT) return OnCommand(CMD_BACK);
    if (alt && vk == VK_RIGHT) return OnCommand(CMD_FORWARD);
    if (alt && vk == VK_UP) return OnCommand(CMD_UP);
    if (ctrl) {
        switch (vk) {
            case 'F':
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
                return;
            case 'O': return OnCommand(CMD_SCAN_FOLDER);
            case 'A': return OnCommand(CMD_SELECT_ALL);
            case 'C': return OnCommand(CMD_COPY_PATH);
            case 'E': return OnCommand(CMD_REVEAL);
            case 'S': return OnCommand(CMD_EXPORT_JSON);
        }
    }
    int count = static_cast<int>(rows_.size());
    ListState& ls = lists_[static_cast<int>(view_)];
    int pageRows = std::max(1, static_cast<int>(H(listArea_) / 32.0f) - 1);
    int target = -2;
    switch (vk) {
        case VK_UP: target = std::max(0, ls.focus - 1); break;
        case VK_DOWN: target = std::min(count - 1, ls.focus + 1); break;
        case VK_PRIOR: target = std::max(0, ls.focus - pageRows); break;
        case VK_NEXT: target = std::min(count - 1, ls.focus + pageRows); break;
        case VK_HOME: target = 0; break;
        case VK_END: target = count - 1; break;
        case VK_RIGHT:
            if (view_ == View::Browse && ls.focus >= 0 && ls.focus < count) {
                Node* n = rows_[ls.focus].node;
                if (n && n->CanEnter() && !n->children.empty()) {
                    if (!expanded_.count(n)) {
                        expanded_.insert(n);
                        rowsDirty_ = true;
                    } else {
                        target = std::min(count - 1, ls.focus + 1);
                    }
                }
            }
            break;
        case VK_LEFT:
            if (view_ == View::Browse && ls.focus >= 0 && ls.focus < count) {
                Node* n = rows_[ls.focus].node;
                if (n && expanded_.count(n)) {
                    expanded_.erase(n);
                    rowsDirty_ = true;
                } else if (rows_[ls.focus].depth > 0) {
                    for (int i = ls.focus - 1; i >= 0; --i)
                        if (rows_[i].node == n->parent) {
                            target = i;
                            break;
                        }
                }
            }
            break;
        case VK_RETURN:
            if (ls.focus >= 0) OpenRow(ls.focus);
            break;
        case VK_SPACE:
            if (view_ == View::CleanReview)
                ToggleKeep(ls.focus);
            else if (view_ == View::AiModels)
                ToggleAiRow(ls.focus);
            else if (view_ == View::Wsl)
                ToggleWslRow(ls.focus);
            else if (view_ == View::Games)
                OnGameRow(ls.focus);
            else if (view_ == View::Startup) {
                if (ls.focus >= 0 && ls.focus < static_cast<int>(rows_.size()) && rows_[ls.focus].kind == RK_STARTUP)
                    OnStartupSwitch(rows_[ls.focus].idx);
            }
            else if (view_ == View::Apps)
                OnAppRow(ls.focus);
            else if (ls.focus >= 0)
                SelectRow(ls.focus, true, false);
            break;
        case VK_DELETE:
            if (view_ == View::AiModels)
                StartAiDelete();
            else if (view_ == View::Apps)
                StartAppLeftoversDelete();
            else
                RequestDelete(shift);
            break;
        case VK_BACK:
            if (view_ == View::CleanReview)
                ShowView(View::Cleanup);
            else if (view_ == View::Browse)
                OnCommand(CMD_UP);
            else if (view_ == View::TypeFiles)
                ShowView(View::Types);
            else if (root_)
                ShowView(View::Browse);
            break;
        case VK_F5: OnCommand(CMD_RESCAN); break;
        case VK_ESCAPE:
            if (view_ == View::CleanReview)
                ShowView(View::Cleanup);
            else if (!sel_.empty())
                sel_.clear();
            else if (view_ != View::Browse && root_)
                ShowView(View::Browse);
            break;
        case VK_F1: ShowAbout(); break;
    }
    if (target >= 0 && target < count) {
        if (shift) {
            if (ls.anchor < 0) ls.anchor = ls.focus >= 0 ? ls.focus : target;
            SelectRow(target, false, true);
        } else {
            SelectRow(target, false, false);
        }
        EnsureVisible(target);
    }
    Invalidate();
}

void App::OnDrop(HDROP drop) {
    wchar_t buf[32768];
    UINT n = DragQueryFileW(drop, 0, buf, 32768);
    DragFinish(drop);
    if (!n) return;
    std::wstring p(buf, n);
    std::wstring lower = util::ToLower(p);
    if (lower.size() > 5 && lower.substr(lower.size() - 5) == L".json") {
        StartImport(p);
        return;
    }
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t slash = p.find_last_of(L'\\');
        if (slash != std::wstring::npos) p = p.substr(0, slash);
    }
    StartScan(p);
}
