#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../aimodels.h"
#include "../apps.h"
#include "../analysis.h"
#include "../devjunk.h"
#include "../diskhealth.h"
#include "../fsops.h"
#include "../games.h"
#include "../i18n.h"
#include "../journal.h"
#include "../phash.h"
#include "../protect.h"
#include "../scanner.h"
#include "../schedule.h"
#include "../startup.h"
#include "../vdisks.h"
#include "../wintools.h"
#include "../tree.h"
#include "../update.h"
#include "cleanup.h"
#include "gfx.h"
#include "history.h"
#include "icons.h"
#include "platform.h"
#include "treemap.h"
#include "uia.h"

enum class View { Home, Browse, Largest, Types, TypeFiles, Duplicates, Old, Empty, DevJunk, Search, Cleanup, History, Compare, Deleted, AiModels, Wsl, WinTools, Games, Startup, Apps, CleanReview, DiskHealth, Count };

enum Cmd : int {
    CMD_NONE = 0,
    CMD_HOME = 100,
    CMD_SCAN_FOLDER,
    CMD_OPEN_SCAN,
    CMD_RESCAN,
    CMD_THEME_TOGGLE,
    CMD_MORE,
    CMD_BACK,
    CMD_FORWARD,
    CMD_UP,
    CMD_TOGGLE_TREEMAP,
    CMD_MODE_DISK,
    CMD_MODE_APPARENT,
    CMD_DELETE,
    CMD_DELETE_PERM,
    CMD_CLEAR_SEL,
    CMD_DUP_FIND,
    CMD_DUP_MARK,
    CMD_CLEAN_SELECTED,
    CMD_CLEAN_MEASURE,
    CMD_ADMIN,
    CMD_CANCEL_TASK,
    CMD_BACK_TO_TYPES,
    CMD_SEARCH_CLEAR,
    CMD_OPEN = 200,
    CMD_OPEN_DEFAULT,
    CMD_REVEAL,
    CMD_COPY_PATH,
    CMD_TERMINAL,
    CMD_PROPERTIES,
    CMD_GOTO,
    CMD_RESCAN_ITEM,
    CMD_SELECT_ALL,
    CMD_SET_THEME_SYS = 300,
    CMD_SET_THEME_LIGHT,
    CMD_SET_THEME_DARK,
    CMD_SET_SI,
    CMD_SET_HIDDEN,
    CMD_SET_HARDLINKS,
    CMD_SET_SHELL,
    CMD_SET_PERMANENT,
    CMD_EXPORT_JSON,
    CMD_EXPORT_CSV,
    CMD_ABOUT,
    CMD_PROTECTED,
    CMD_OPEN_JOURNAL,
    CMD_REVIEW_ALL,
    CMD_REVIEW_NONE,
    CMD_CLEANERS,
    CMD_DEV_MARK_OLD,
    CMD_AI_REFRESH,
    CMD_AI_UNUSED,
    CMD_AI_DELETE,
    CMD_WSL_REFRESH,
    CMD_WSL_FREE,
    CMD_WINAPP2,
    CMD_WINAPP2_REMOVE,
    CMD_WIN_REFRESH,
    CMD_GAMES_REFRESH,
    CMD_GAME_UNINSTALL,
    CMD_GAMES_DELETE,
    CMD_DISK_REFRESH,
    CMD_CLEAN_SCHEDULE,
    CMD_KEEP_SIGNED_IN,
    CMD_STARTUP_REFRESH,
    CMD_APPS_REFRESH,
    CMD_APP_UNINSTALL,
    CMD_APP_LEFTOVERS,
    CMD_APP_STOPWAIT,
    CMD_APPS_DELETE,
    CMD_APP_LEFT_DONE,
    CMD_PHOTO_FIND,
    CMD_PHOTO_MARK,
    CMD_UPDATE_CHECK,    // ⋯ > Check for updates now (also when daily checks are off)
    CMD_UPDATE_AUTO,     // ⋯ > Check for updates daily
    CMD_UPDATE_NOTES,    // the update banner's buttons
    CMD_UPDATE_INSTALL,
    CMD_UPDATE_LATER,
    CMD_UPDATE_SKIP,
    CMD_UPDATE_COPY,
    CMD_WIN_ACT = 600,  // + WinAction
    CMD_LANG = 800,     // + 0 Windows' language, 1 the pseudo-language, 2 + an index into langMenu_
};

// The Windows view's actions (command CMD_WIN_ACT + action).
enum WinAction {
    WA_ANALYZE,
    WA_CLEAN_STORE,
    WA_DRIVERS,
    WA_SHADOW,
    WA_HIBER_OFF,
    WA_HIBER_REDUCED,
    WA_HIBER_ON,
    WA_COMPACT_ON,
    WA_COMPACT_OFF,
    WA_LOGS,
    WA_COUNT
};

enum HitKind {
    HK_NONE,
    HK_BUTTON,    // a = command
    HK_NAV,       // a = view
    HK_DRIVE,     // a = drive index
    HK_CRUMB,     // b = Node*
    HK_ROW,       // a = row
    HK_EXPANDER,  // a = row
    HK_HEADER,    // a = sort key
    HK_SCROLL,    // scrollbar of the main list
    HK_SPLITTER,
    HK_TREEMAP,
    HK_CARD,      // a = home card index
    HK_RECENT,    // a = recent index
    HK_CHIP,      // a = group, b = value
    HK_CHECK,     // a = cleanup category index
    HK_SNAPSHOT,  // a = snapshot index, b = action (0 compare, 1 delete)
    HK_DIALOG,    // a = button index (-1 = checkbox)
    HK_MODAL_BG,
    HK_BADGE,     // shield badge (tooltip): a = row, -1 = the open folder
    HK_RESTORE,   // a = index into deleted_
    HK_REVIEW,    // a = cleanup category: open its file list
    HK_DIALOG_CHOICE,  // a = choice index, b = option index
    HK_SWITCH,    // a = startup entry index: its on/off switch
};

struct Hit {
    RectF r;
    int kind = HK_NONE;
    int a = 0;
    intptr_t b = 0;
};

enum RowKind { RK_NODE, RK_GROUP, RK_TYPE, RK_DIFF, RK_CLEAN, RK_AI, RK_WSL, RK_GAME, RK_STARTUP, RK_APP, RK_APPLEFT };

struct Row {
    Node* node = nullptr;
    int depth = 0;
    int kind = RK_NODE;
    int idx = 0;
};

struct ListState {
    float scroll = 0;
    int focus = -1;
    int anchor = -1;
};

struct Column {
    std::wstring title;
    float width;  // > 0 fixed DIP, < 0 flexible weight
    Align align;
    int sortKey;  // -1 = not sortable
    float x = 0, w = 0;
};

enum SortKey { SK_NAME, SK_SIZE, SK_ITEMS, SK_MTIME, SK_FOLDER };

struct Task {
    std::wstring title;
    std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::function<std::vector<std::pair<std::wstring, std::wstring>>()> lines;
    std::function<double()> progress;
    std::function<void()> cancel;
    std::function<void()> onDone;
    HANDLE thread = nullptr;
    ULONGLONG start = 0;
    bool cancelRequested = false;
};

// A row of options in a dialog: chips, or "< value >" for long lists (times, days).
struct DialogChoice {
    std::wstring label;
    std::vector<std::wstring> options;
    int value = 0;
    bool stepper = false;
    bool hidden = false;
};

struct Dialog {
    bool active = false;
    std::wstring title, message;
    std::vector<std::wstring> lines;
    wchar_t icon = 0;
    int tone = 0;  // 0 accent, 1 danger, 2 warning
    std::vector<std::wstring> buttons;
    int primary = -1, danger = -1;
    int cancel = -1;  // button chosen by Escape (-1: the first non-primary button)
    bool hasCheck = false, checked = false;
    std::wstring checkText;
    bool checkDanger = true;  // checked = risky (e.g. "delete permanently"): drawn in the danger color
    std::function<void(Dialog&)> onCheck;
    std::function<void(int button, bool checked)> onClose;
    std::vector<DialogChoice> choices;
    std::function<void(Dialog&)> onChoice;                       // a choice changed
    std::function<void(int button, const Dialog&)> onCloseWith;  // like onClose, with the choices
    // Keyboard focus (Tab / arrows): -1 the checkbox, 0.. a button, 1000 + i a choice; anything else
    // means the primary button. kbd: the keyboard moved it, so a focus ring is drawn.
    int focus = -1000;
    bool kbd = false;
};

// One line of the "Recently deleted" view.
struct DeletedRow {
    JournalEntry e;
    int bin = -1;         // index into App::bin_ while it is still in the Recycle Bin
    bool back = false;    // Recycle entries not in the bin: the path exists again (restored)
};

struct ToastInfo {
    std::wstring text;
    int tone = 0;  // 0 success, 1 error, 2 info
    ULONGLONG until = 0;
};

class App : private AccHost {
public:
    // `show`: how the program was started (wWinMain's nCmdShow), e.g. minimized from a shortcut.
    // `updated`: started by the update that just replaced the program (--updated).
    bool Create(HINSTANCE inst, const std::wstring& path, const std::wstring& importFile, int show = SW_SHOWDEFAULT,
                bool updated = false);
    int Run();

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK SearchProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // ---- window & input (app.cpp)
    void OnPaint();
    void OnSize();
    void OnMouseMove(float x, float y);
    void OnLButtonDown(float x, float y, bool dbl);
    void OnLButtonUp(float x, float y);
    void OnRButtonUp(float x, float y);
    void OnWheel(float delta, float x, float y);
    void OnKeyDown(WPARAM vk);
    void OnCommand(int cmd);
    void OnDrop(HDROP drop);
    void OnTimer();
    const Hit* HitAt(float x, float y);
    void ApplyTheme();
    // Loads settings_.language (or BYTOPSY_LANG) and redraws in it.
    void ApplyLanguage(bool redraw);
    std::wstring SearchCue() const;
    void UpdateSearchBox();
    void CreateSearchBox();
    float Px2Dip(int px) const { return px * 96.0f / gfx_.Dpi(); }
    int Dip2Px(float d) const { return static_cast<int>(d * gfx_.Dpi() / 96.0f + 0.5f); }
    void Invalidate() { InvalidateRect(hwnd_, nullptr, FALSE); }
    void EnsureTimer();

    // ---- model (actions.cpp)
    uint64_t Val(const Node* n) const { return settings_.apparent ? n->size : n->disk; }
    void SetRoot(std::unique_ptr<Node> r, bool readOnly);
    void PostMutation();
    void BuildRows();
    std::vector<Node*> SortedChildren(Node* dir);
    std::vector<Node*> SelectedNodes();
    std::vector<Node*> NormalizeTargets(std::vector<Node*> v);
    Node* FindNodeByPath(const std::wstring& path);
    Node* FocusNode();
    void SelectRow(int row, bool ctrl, bool shift);
    void EnsureVisible(int row);
    void RevealInBrowse(Node* n);
    void SortResults();
    void RebuildTypes();
    void RebuildEmpty();
    void RebuildDevJunk();
    void SortDevJunk();
    void ToggleDevProject(int project);
    void LoadAiModels();
    void ToggleAiRow(int row);
    void StartAiDelete();
    uint64_t AiSelectedBytes() const;
    void LoadWsl();
    void ToggleWslRow(int row);
    void StartWslFree();
    uint64_t WslRowBytes(int id) const;
    void LoadWinTools();
    void LoadGames();
    void LoadDiskHealth();
    void SortGames();
    void OnGameRow(int row);
    void StartGameUninstall();
    void StartGameLeftoversDelete();
    void LoadStartup();
    bool StartupVisible(const StartupEntry& e) const;
    void OnStartupSwitch(int index);
    void ApplyStartupChange(int index, bool enable);
    // Apps view (apps_view.cpp)
    void LoadApps();
    void SortApps();
    bool AppVisible(const InstalledApp& a) const;
    void OnAppRow(int row);
    void StartAppUninstall();
    void OnAppUninstallProgress();
    void LookForAppLeftovers(bool askIfInstalled);
    void StartAppLeftoversDelete();
    void SyncSearchBox();
    void OnSearchChanged();
    void WinToolAction(int action);
    void RunWinTool(const std::wstring& title, const std::wstring& journalName, const std::wstring& done,
                    std::function<AdminRun()> work, const std::wstring& doneShown = L"");
    void RebuildOld();

    // ---- actions (actions.cpp)
    void StartTask(std::unique_ptr<Task> t, std::function<void()> work);
    void OnTaskDone();
    void StartScan(const std::wstring& path);
    void StartRescan(std::vector<Node*> dirs);
    void StartDelete(std::vector<Node*> targets, bool permanent);
    void StartImport(const std::wstring& file);
    void StartExport(bool csv);
    void StartDuplicates();
    void StartSimilarPhotos();
    void StartCleanup();
    void MeasureCleanup();
    void OnCleanupMeasured(std::vector<CleanCategory>* result);
    CleanEntry* ReviewEntry(int row);
    void ToggleKeep(int row);
    void LoadSchedule();
    void ShowScheduleDialog();
    void NavigateTo(Node* dir, bool remember = true);
    void ShowView(View v);
    void RunSearchQuery(const std::wstring& q);
    void OpenRow(int row);
    void OpenNode(Node* n);
    void RequestDelete(bool permanent);
    void ShowDeleteDialog(std::vector<Node*> targets, bool permanent, Protect worst, std::vector<std::wstring> notes,
                          std::wstring heads);
    void ShowProtectedLocations();
    void ShowContextMenu(int screenX, int screenY);
    void ShowMoreMenu();
    void CompareSnapshot(size_t index);
    void LoadHistory();
    void LoadDeleted();
    void RestoreDeleted(int index);
    void Toast(const std::wstring& text, int tone = 0);
    void ShowDialog(Dialog d);
    void ShowAbout();
    void ConfirmNewScan(const std::wstring& path);

    // ---- self-update (update_gui.cpp)
    void StartupUpdates(bool updated);  // .old files of the last update, the "updated" message
    void AskAboutUpdates();             // once, on the first start
    void MaybeCheckForUpdates();        // at most once a day, when turned on
    void StartUpdateCheck(bool manual);
    void OnUpdateChecked(const UpdateCheck& c, bool manual);
    void OnUpdateCommand(int cmd);
    void ShowUpdateInstall();
    void StartUpdateInstall();
    void DrawUpdateBanner();

    // ---- accessibility (uia_app.cpp): the UI Automation tree, rebuilt from each frame
    HWND AccWindow() const override { return hwnd_; }
    const AccTree* AccCurrent() override;
    HRESULT AccDo(uint64_t key, AccAct act) override;
    RECT AccScreenRect(const RectF& r) const override;
    bool AccClientPoint(double screenX, double screenY, float* x, float* y) const override;
    void AccFrameBegin();
    void AccBuild();
    void AccEvents();
    void AccRun(uint64_t key, AccAct act);
    void AccMark(const std::wstring& name, int toggle = -1);  // the last click region's name / toggle state
    void AccDisabled(const RectF& r, int kind, int a, const std::wstring& name = L"");  // a control drawn disabled
    void AccNameSearchBox(bool set);
    std::vector<int> DialogSlots() const;
    int DialogFocus() const;
    bool DialogKey(WPARAM vk, bool shift);
    // What a click on a region does (OnLButtonDown, and UI Automation's patterns).
    void ActivateHit(const Hit& hit, bool dbl, bool ctrl, bool shift);

    // ---- drawing (draw.cpp)
    void Paint();
    void AddHit(const RectF& r, int kind, int a = 0, intptr_t b = 0) { hits_.push_back({r, kind, a, b}); }
    bool Hovered(int kind, int a = 0, intptr_t b = 0) const;
    void DrawTopBar();
    void DrawSidebar();
    void DrawStatusBar();
    void DrawHome(const RectF& area);
    void DrawBrowse(const RectF& area);
    void DrawResults(const RectF& area);
    void DrawTypes(const RectF& area);
    void DrawDuplicates(const RectF& area);
    void DrawSimilarPhotos(const RectF& area, float top);
    ID2D1Bitmap* Thumb(Node* n);  // photo preview for a row, decoded on demand (null: not yet / not possible)
    void ClearThumbs();
    void DrawDevJunk(const RectF& area);
    void DrawAiModels(const RectF& area);
    void DrawWsl(const RectF& area);
    void DrawWinTools(const RectF& area);
    void DrawGames(const RectF& area);
    void DrawDiskHealth(const RectF& area);
    void DrawStartup(const RectF& area);
    void DrawApps(const RectF& area);
    void DrawCleanup(const RectF& area);
    void DrawCleanReview(const RectF& area);
    void DrawHistory(const RectF& area);
    void DrawCompare(const RectF& area);
    void DrawDeleted(const RectF& area);
    void DrawTable(const RectF& area, std::vector<Column>& cols, int sortKey, bool sortDesc,
                   const std::function<void(int row, const Column& col, int ci, const RectF& cell, bool sel)>& cell,
                   const std::function<void(int row, const RectF& r)>& fullRow = nullptr);
    void DrawTreemap(const RectF& area);
    void DrawViewHeader(const RectF& r, const std::wstring& title, const std::wstring& subtitle, wchar_t icon);
    void DrawNameCell(const RectF& cell, Node* n, int depth, bool expander, bool expanded, int row, bool sel,
                      ID2D1Bitmap* thumb = nullptr);
    const ProtectHit* Shield(Node* n);  // badge to show for a row, or null
    D2D1_COLOR_F ShieldColor(Protect p) const;
    void DrawUsageBar(const RectF& r, double frac, bool pct, double pctValue);
    void DrawDialog();
    void DrawBusy();
    void DrawToast();
    void DrawTooltip();
    void DrawEmptyState(const RectF& area, wchar_t icon, const std::wstring& title, const std::wstring& text);
    float Button(const RectF& r, wchar_t icon, const std::wstring& text, int cmd, int style, bool enabled = true);
    // What a Button with this text needs (at least minW): translations are often longer.
    float ButtonWidth(const std::wstring& text, wchar_t icon, float minW = 0);
    // A cleanup category's name and description as shown: built-in ones translated.
    static std::wstring CatName(const CleanCategory& c);
    static std::wstring CatDesc(const CleanCategory& c);
    float IconButton(float x, float y, wchar_t icon, int cmd, const std::wstring& tip, bool active = false);
    float Chip(float x, float y, const std::wstring& text, bool active, int group, intptr_t value);
    void Checkbox(const RectF& r, bool checked, bool hover, bool mixed = false);

    // ---- state
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr;
    HFONT searchFont_ = nullptr;
    HBRUSH searchBrush_ = nullptr;
    HINSTANCE inst_ = nullptr;
    Gfx gfx_;
    IconCache icons_;
    Palette P_;
    Settings settings_;
    bool elevated_ = false;
    std::vector<i18n::LanguageInfo> langMenu_;  // the Language menu's entries, as last shown

    std::unique_ptr<Node> root_;
    std::vector<std::unique_ptr<Node>> graveyard_;
    bool readOnly_ = false, incomplete_ = false;
    uint64_t scanErrors_ = 0;
    double scanSeconds_ = 0;
    uint64_t volTotal_ = 0, volFree_ = 0;
    bool haveVol_ = false;
    ProtectBadges badges_;

    View view_ = View::Home;
    Node* browseDir_ = nullptr;
    std::vector<Node*> backStack_, fwdStack_;
    std::unordered_set<Node*> expanded_;
    std::unordered_set<Node*> sel_;
    std::vector<Row> rows_;
    bool rowsDirty_ = true;
    ListState lists_[static_cast<int>(View::Count)];
    int sortKey_ = SK_SIZE;
    bool sortDesc_ = true;
    int resSortKey_ = SK_SIZE;
    bool resSortDesc_ = true;

    // Result data
    std::vector<Node*> largest_, searchRes_, oldRes_, emptyRes_, typeFiles_;
    std::wstring searchQuery_, typeFilesExt_;
    std::vector<ExtStat> types_;
    uint64_t catTotals_[CAT_COUNT] = {};
    std::vector<DupGroup> dups_;
    bool dupsDone_ = false;
    float sidebarScroll_ = 0;  // DIPs; only when the window is too short for every sidebar item
    // Similar photos (the Duplicates view's second mode)
    bool dupPhotos_ = false;
    std::vector<PhotoGroup> photos_;
    bool photosDone_ = false;
    uint64_t photoImages_ = 0, photoFailed_ = 0;  // images looked at / not decodable in the last search
    PhotoMatch photoMatch_ = PhotoMatch::Normal;
    uint64_t photoMin_ = 100 << 10;
    std::shared_ptr<PhotoHashCache> photoCache_;
    std::unordered_map<Node*, ID2D1Bitmap*> thumbs_;
    unsigned thumbGen_ = 0;
    ULONGLONG thumbDeadline_ = 0;  // stop decoding previews in this frame after this tick
    bool thumbsPending_ = false;   // some visible previews still to decode: paint again
    std::vector<DevProject> devJunk_;
    bool devJunkDone_ = false;
    bool devJunkByAge_ = false;  // sort projects by last change (oldest first) instead of size
    std::vector<AiStore> ai_;
    bool aiLoaded_ = false;
    bool aiAtime_ = false;  // Windows records last-access times, so "Last used" means something
    std::set<std::pair<size_t, size_t>> aiSel_;  // (store, item)
    // RK_AI rows: idx = store << 20 | item.
    static int AiRowIdx(size_t store, size_t item) { return static_cast<int>(store << 20 | item); }
    std::vector<VDisk> vdisks_;
    DockerInfo docker_;
    bool wslLoaded_ = false;
    // RK_WSL rows: idx = a disk index, or kWslDocker + 0 build cache, 1 unused images, 2 stopped
    // containers, 3 unused volumes (the last two are shown, never offered).
    static constexpr int kWslDocker = 1000;
    std::set<int> wslSel_;
    struct WinToolsInfo {
        std::vector<DriverPackage> drivers;
        Hibernation hiber;
        bool compactOs = false;
        EventLogs logs;
        std::vector<ShadowStorage> shadows;
        std::wstring shadowErr;
        ComponentStore store;
        bool storeAnalyzed = false;
    };
    WinToolsInfo win_;
    bool winLoaded_ = false;
    std::vector<Game> games_;
    bool gamesLoaded_ = false;
    bool gamesByAge_ = false;       // least recently played first instead of largest
    int gameChosen_ = -1;           // the installed game the Uninstall button acts on
    std::set<size_t> gameLeftSel_;  // checked leftovers
    DiskHealthReport disk_;
    std::vector<DriveTrend> diskTrends_;
    bool diskLoaded_ = false;
    StartupList startup_;
    bool startupLoaded_ = false;
    bool startupShowMs_ = false;  // also list Microsoft's own services and tasks
    std::vector<InstalledApp> apps_;
    bool appsLoaded_ = false;
    int appSortKey_ = SK_SIZE;
    bool appSortDesc_ = true;
    std::wstring appFilter_;     // what the search box holds while the Apps view is shown
    bool searchForApps_ = false;  // the search box currently filters apps
    int appChosen_ = -1;         // index into apps_: the app the Uninstall button acts on
    // An uninstaller Bytopsy started: a worker waits for it to exit, then for its entry to go.
    struct AppWait {
        UninstallEntry e;
        std::atomic<int> phase{0};  // 0 running, 1 exited (entry still there), 2 entry gone, 3 still installed
        std::atomic<bool> stop{false};
    };
    std::shared_ptr<AppWait> appWait_;
    std::wstring appLeftFor_;          // the uninstalled app whose leftovers are listed
    UninstallEntry appLeftEntry_;      // its (former) entry: deleting stops if it is back
    std::vector<AppLeftover> appLeft_;
    std::set<size_t> appLeftSel_;      // checked leftovers
    uint64_t dupMin_ = 1 << 20;
    int oldAgeDays_ = 365;
    uint64_t oldMinSize_ = 0;
    std::vector<CleanCategory> cleanup_;
    bool cleanupMeasuring_ = false;
    int reviewCat_ = -1;  // category shown in View::CleanReview
    TaskInfo schedule_;          // the scheduled cleanup task (\Bytopsy\Scheduled cleanup)
    std::wstring scheduleErr_;   // Task Scheduler couldn't be read
    std::vector<SnapshotInfo> snapshots_;
    std::vector<DiffEntry> diff_;
    SnapshotInfo diffInfo_;
    std::vector<DeletedRow> deleted_;
    std::vector<RecycledItem> bin_;

    // Treemap cache
    std::vector<TmRect> tm_;
    Node* tmRoot_ = nullptr;
    RectF tmArea_{};
    bool tmDirty_ = true;
    ID2D1Bitmap* tmBitmap_ = nullptr;
    unsigned tmGen_ = 0;
    int tmHover_ = -1;
    std::vector<int> tmShields_;  // treemap rectangles that get a shield badge
    float mouseX_ = -1, mouseY_ = -1;

    // Layout + interaction
    std::vector<Hit> hits_;
    size_t modalHitStart_ = SIZE_MAX;
    Hit hover_;
    RectF listArea_{}, contentArea_{};
    float listContentH_ = 0;
    int dragKind_ = HK_NONE;
    float dragStart_ = 0, dragValue_ = 0;
    bool tracking_ = false;
    std::vector<DriveInfo> drives_;

    std::wstring tipText_;
    RectF tipAnchor_{};
    std::unordered_map<Node*, int> tmIndex_;
    std::unordered_map<std::wstring, std::wstring> typeNames_;
    const std::wstring& TypeName(const std::wstring& ext);
    std::wstring FitPath(const std::wstring& s, float width, float fontSize, bool bold = false);
    std::wstring RelFolder(Node* n);
    std::unordered_map<std::wstring, std::wstring> fitCache_;  // parent folder, relative to the scan root

    std::unique_ptr<Task> task_;
    // Self-update: the last check that found a newer version, and whether its banner shows.
    UpdateCheck update_;
    bool updateBanner_ = false;
    bool updateCheckRunning_ = false;  // the daily check, in the background
    float bannerTop_ = 0;              // where the banner starts (the toast goes above it)
    PackageManager pkgMgr_ = PackageManager::None;
    Dialog dialog_;
    ToastInfo toast_;
    bool timerOn_ = false;

    // Accessibility
    std::unique_ptr<AccBridge> acc_;
    AccTree accTree_;
    std::vector<Hit> accActs_;  // per element (AccNode::action): the click it stands for
    struct AccMarkInfo {
        size_t hit;  // index into hits_, SIZE_MAX: a disabled control (no click region)
        Hit h;
        std::wstring name;
        int toggle;
        size_t at;  // hits_.size() when it was drawn
    };
    std::vector<AccMarkInfo> accMarks_;
    struct AccCheck {
        RectF r;
        bool on, mixed;
    };
    std::vector<AccCheck> accChecks_;
    size_t accTipFrom_ = SIZE_MAX, accTipTo_ = SIZE_MAX, accModalText_ = SIZE_MAX;
    // Where the sidebar, top bar, status bar and overlays start in hits_ / the recorded text runs.
    size_t accHitSeg_[4] = {}, accRunSeg_[4] = {};
    RectF accModalRect_{};
    bool inPaint_ = false, accEventPosted_ = false;
    uint64_t accSig_ = 0, accModalKey_ = 0, accFocusKey_ = 0;
    uint32_t accFocusFlags_ = 0;
    uint64_t dialogGen_ = 0;
    std::wstring accFocusName_;
};

constexpr UINT WM_APP_TASKDONE = WM_APP + 1;
constexpr UINT WM_APP_CLEANUP = WM_APP + 2;
constexpr UINT WM_APP_UNINSTALL = WM_APP + 3;  // an uninstaller Bytopsy waits for made progress
constexpr UINT WM_APP_ACC = WM_APP + 4;        // raise UI Automation events for the last frame
constexpr UINT WM_APP_ACCDO = WM_APP + 5;      // run a UI Automation action: wParam AccAct, lParam element key
constexpr UINT WM_APP_UPDATE = WM_APP + 6;     // the daily update check is done: lParam a new UpdateCheck*
