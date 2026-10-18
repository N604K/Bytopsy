#include "schedule.h"

#include <windows.h>
#include <oleauto.h>
#define SECURITY_WIN32
#include <security.h>
#include <taskschd.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "journal.h"
#include "i18n.h"
#include "util.h"

namespace {

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

bool Is(const std::wstring& a, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names)
        if (a == n) return true;
    return false;
}

// Splits a command line the way the C runtime does for simple cases: spaces separate, double
// quotes group (and are removed).
std::vector<std::wstring> SplitArgs(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, any = false;
    for (wchar_t c : s) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if ((c == L' ' || c == L'\t') && !quoted) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int DaysInMonth(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && IsLeap(y) ? 29 : kDays[(m - 1) % 12];
}

void AddDays(int& y, int& m, int& d, int n) {
    d += n;
    while (d > DaysInMonth(y, m)) {
        d -= DaysInMonth(y, m);
        if (++m > 12) {
            m = 1;
            ++y;
        }
    }
}

std::wstring Boundary(int y, int m, int d, int hh, int mm) {
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02dT%02d:%02d:00", y, m, d, hh, mm);
    return b;
}

}  // namespace

std::vector<std::wstring> ParseIdList(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos) j = s.size();
        std::wstring id = util::ToLower(Trim(s.substr(i, j - i)));
        if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
        i = j + 1;
    }
    return out;
}

std::wstring JoinIds(const std::vector<std::wstring>& ids) {
    std::wstring s;
    for (auto& id : ids) s += (s.empty() ? L"" : L",") + id;
    return s;
}

bool ParseCleanCommandLine(const std::vector<std::wstring>& args, CleanCommand& out, std::wstring& err) {
    out = CleanCommand{};
    for (auto& a : args)
        if (a == L"--clean" || a.rfind(L"--clean=", 0) == 0) out.clean = true;
    if (!out.clean) return true;
    bool haveIds = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--clean" || a.rfind(L"--clean=", 0) == 0) {
            if (haveIds) {
                err = L"--clean was given twice";
                return false;
            }
            std::wstring v;
            if (a == L"--clean") {
                if (i + 1 >= args.size() || (args[i + 1].size() > 1 && args[i + 1][0] == L'-')) {
                    err = L"--clean needs category ids, e.g. --clean temp,browser (or all-safe)";
                    return false;
                }
                v = args[++i];
            } else {
                v = a.substr(8);
            }
            out.ids = ParseIdList(v);
            if (out.ids.empty()) {
                err = L"--clean needs category ids, e.g. --clean temp,browser (or all-safe)";
                return false;
            }
            haveIds = true;
        } else if (Is(a, {L"--yes", L"-y"})) {
            out.yes = true;
        } else if (Is(a, {L"--quiet", L"-q"})) {
            out.quiet = true;
        } else if (a == L"--si") {
            out.si = true;
        } else {
            err = L"--clean can't be combined with " + a + L" (only --yes, --quiet and --si)";
            return false;
        }
    }
    return true;
}

bool ResolveCleanIds(const std::vector<std::wstring>& requested, const std::vector<CleanerInfo>& known,
                     std::vector<std::wstring>& out, std::wstring& err) {
    out.clear();
    std::vector<std::wstring> unknown;
    auto add = [&](const std::wstring& id) {
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    for (auto& r : requested) {
        std::wstring id = util::ToLower(r);
        if (id == L"all-safe") {
            for (auto& k : known)
                if (k.safe && !k.external && !k.imported) add(k.id);
            continue;
        }
        auto it = std::find_if(known.begin(), known.end(), [&](const CleanerInfo& k) { return k.id == id; });
        if (it == known.end())
            unknown.push_back(r);
        else
            add(it->id);
    }
    if (unknown.empty()) return true;
    std::wstring valid;
    size_t imported = 0;
    for (auto& k : known) {
        if (k.imported) {
            ++imported;
            continue;
        }
        valid += (valid.empty() ? L"" : L", ") + k.id;
    }
    err = std::wstring(unknown.size() == 1 ? L"unknown cleanup category " : L"unknown cleanup categories ") + JoinIds(unknown) +
          L". Valid: " + valid + L", or all-safe (the ones preselected in the windowed app)";
    if (imported) err += L"; also " + util::FormatCount(imported) + L" imported winapp2.ini ids (w2:...)";
    out.clear();
    return false;
}

int CleanExitCode(bool anyRan, uint64_t failedItems, size_t toolErrors) {
    if (!anyRan) return kCleanSkipped;
    return failedItems || toolErrors ? kCleanPartial : kCleanOk;
}

std::wstring CleanArguments(const std::vector<std::wstring>& ids) {
    std::wstring list = JoinIds(ids);
    if (list.find_first_of(L" \t") != std::wstring::npos) list = L"\"" + list + L"\"";
    return L"--clean " + list + L" --yes --quiet";
}

bool ParseCleanArguments(const std::wstring& args, std::vector<std::wstring>& ids) {
    std::vector<std::wstring> v = SplitArgs(args);
    // Whatever comes before --clean (e.g. a wrapper's own arguments) isn't ours.
    auto at = std::find_if(v.begin(), v.end(), [](const std::wstring& a) { return a == L"--clean" || a.rfind(L"--clean=", 0) == 0; });
    if (at == v.end()) return false;
    v.erase(v.begin(), at);
    CleanCommand cmd;
    std::wstring err;
    if (!ParseCleanCommandLine(v, cmd, err) || !cmd.clean) return false;
    ids = cmd.ids;
    return true;
}

const wchar_t* WeekdayName(int d) {
    static const wchar_t* const kNames[] = {N_(L"Sunday"),   N_(L"Monday"), N_(L"Tuesday"), N_(L"Wednesday"),
                                            N_(L"Thursday"), N_(L"Friday"), N_(L"Saturday")};
    return d >= 0 && d < 7 ? kNames[d] : L"?";
}

std::wstring DescribeSchedule(const ScheduleSpec& s) {
    wchar_t t[16];
    swprintf(t, 16, L"%02d:%02d", s.hour, s.minute);
    switch (s.freq) {
        case ScheduleFreq::Daily: return TrF(L"Daily at {0}", std::wstring(t));
        case ScheduleFreq::Weekly: return TrF(L"Weekly on {0} at {1}", Tr(WeekdayName(s.weekday)), std::wstring(t));
        case ScheduleFreq::Monthly: return TrF(L"Monthly on day {0} at {1}", s.monthDay, std::wstring(t));
        case ScheduleFreq::Logon: return Tr(L"At logon");
    }
    return L"";
}

std::wstring ScheduleStartBoundary(const ScheduleSpec& s, int year, int month, int day, int dayOfWeek, int hour, int minute) {
    int y = year, m = month, d = day;
    bool laterToday = s.hour * 60 + s.minute > hour * 60 + minute;
    switch (s.freq) {
        case ScheduleFreq::Logon: return Boundary(y, m, d, hour, minute);
        case ScheduleFreq::Daily:
            if (!laterToday) AddDays(y, m, d, 1);
            break;
        case ScheduleFreq::Weekly: {
            int ahead = ((s.weekday - dayOfWeek) % 7 + 7) % 7;
            if (ahead == 0 && !laterToday) ahead = 7;
            AddDays(y, m, d, ahead);
            break;
        }
        case ScheduleFreq::Monthly: {
            int md = std::clamp(s.monthDay, 1, 28);
            if (d > md || (d == md && !laterToday)) {
                if (++m > 12) {
                    m = 1;
                    ++y;
                }
            }
            d = md;
            break;
        }
    }
    return Boundary(y, m, d, s.hour, s.minute);
}

bool ParseBoundaryTime(const std::wstring& boundary, int& hour, int& minute) {
    size_t t = boundary.find(L'T');
    if (t == std::wstring::npos) return false;
    int h = -1, mi = -1;
    if (swscanf_s(boundary.c_str() + t + 1, L"%d:%d", &h, &mi) != 2 || h < 0 || h > 23 || mi < 0 || mi > 59) return false;
    hour = h;
    minute = mi;
    return true;
}

int WeekdayFromMask(unsigned mask) {
    for (int i = 0; i < 7; ++i)
        if (mask & (1u << i)) return i;
    return -1;
}

int MonthDayFromMask(unsigned mask) {
    for (int i = 0; i < 31; ++i)
        if (mask & (1u << i)) return i + 1;
    return -1;
}

std::wstring DescribeTaskResult(int64_t code) {
    switch (static_cast<uint32_t>(code)) {
        case kCleanOk: return Tr(L"OK");
        case kCleanPartial: return Tr(L"some items were in use, or a tool's command failed");
        case kCleanUsage: return Tr(L"failed: bad arguments or unknown category ids");
        case kCleanSkipped: return Tr(L"nothing ran: the categories need administrator rights");
        case 0x00041303: return Tr(L"not run yet");                      // SCHED_S_TASK_HAS_NOT_RUN
        case 0x00041301: return Tr(L"running now");                      // SCHED_S_TASK_RUNNING
        case 0x00041306: return Tr(L"stopped before it finished");       // SCHED_S_TASK_TERMINATED
        case 0x800710E0: return Tr(L"didn't start (on battery, or not signed in)");
        case 0xC000013A: return Tr(L"stopped before it finished");
        case 0x80070002: return Tr(L"failed: bytopsy-cli.exe was not found");
    }
    wchar_t b[16];
    swprintf(b, 16, L"%08X", static_cast<uint32_t>(code));
    return TrF(L"failed (0x{0})", std::wstring(b));
}

// =============================================================================================
// Task Scheduler

namespace {

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b; }
};

std::wstring Take(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : L"";
    SysFreeString(b);
    return s;
}

VARIANT Empty() {
    VARIANT v;
    VariantInit(&v);
    return v;
}

VARIANT Str(const Bstr& s) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = s;  // borrowed: the Bstr frees it
    return v;
}

bool NotFound(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

std::wstring HrText(HRESULT hr) {
    if (hr == E_ACCESSDENIED) return Tr(L"access denied");
    std::wstring m = util::Win32ErrorMessage(static_cast<unsigned long>(hr));
    return m;
}

bool Fail(const wchar_t* what, HRESULT hr, std::wstring& err) {
    err = Tr(what) + L": " + HrText(hr);  // `what` is marked N_ at the callers
    return false;
}

bool Connect(Com<ITaskService>& svc, std::wstring& err) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&svc));
    if (FAILED(hr)) return Fail(N_(L"Task Scheduler isn't available"), hr, err);
    hr = svc->Connect(Empty(), Empty(), Empty(), Empty());
    if (FAILED(hr)) return Fail(N_(L"Couldn't connect to Task Scheduler"), hr, err);
    return true;
}

// "DOMAIN\user" of the current user.
std::wstring CurrentUser() {
    wchar_t buf[512];
    ULONG n = 512;
    if (GetUserNameExW(NameSamCompatible, buf, &n)) return buf;
    DWORD m = 512;
    return GetUserNameW(buf, &m) ? buf : L"";
}

std::wstring FolderLeaf(const std::wstring& folder) {
    std::wstring f = folder;
    while (!f.empty() && f.front() == L'\\') f.erase(f.begin());
    return f;
}

// DATE (local time) -> FILETIME ticks (UTC); 0 for "none".
int64_t DateToTicks(DATE d) {
    SYSTEMTIME local{}, utc{};
    if (d <= 0 || !VariantTimeToSystemTime(d, &local) || local.wYear < 2000) return 0;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return 0;
    FILETIME ft;
    if (!SystemTimeToFileTime(&utc, &ft)) return 0;
    return (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

bool TaskSave(const std::wstring& folderPath, const std::wstring& name, const ScheduleSpec& when, const std::wstring& exe,
              const std::wstring& args, const std::wstring& description, std::wstring& err) {
    Com<ITaskService> svc;
    if (!Connect(svc, err)) return false;
    Com<ITaskFolder> folder;
    HRESULT hr = svc->GetFolder(Bstr(folderPath), &folder);
    if (NotFound(hr)) {
        Com<ITaskFolder> root;
        hr = svc->GetFolder(Bstr(L"\\"), &root);
        if (FAILED(hr)) return Fail(N_(L"Couldn't open Task Scheduler"), hr, err);
        hr = root->CreateFolder(Bstr(FolderLeaf(folderPath)), Empty(), &folder);
    }
    if (FAILED(hr)) return Fail(N_(L"Couldn't open the task folder"), hr, err);

    Com<ITaskDefinition> def;
    hr = svc->NewTask(0, &def);
    if (FAILED(hr)) return Fail(N_(L"Couldn't create the task"), hr, err);
    std::wstring user = CurrentUser();
    {
        Com<IRegistrationInfo> ri;
        if (SUCCEEDED(def->get_RegistrationInfo(&ri))) {
            ri->put_Author(Bstr(L"Bytopsy"));
            ri->put_Description(Bstr(description));
        }
    }
    {
        Com<IPrincipal> pr;
        hr = def->get_Principal(&pr);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the task"), hr, err);
        pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);  // as you, only while you're signed in
        pr->put_RunLevel(TASK_RUNLEVEL_LUA);              // never elevated
        if (!user.empty()) pr->put_UserId(Bstr(user));
    }
    {
        Com<ITaskSettings> st;
        hr = def->get_Settings(&st);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the task"), hr, err);
        st->put_StartWhenAvailable(VARIANT_TRUE);  // a missed start runs as soon as possible
        st->put_DisallowStartIfOnBatteries(VARIANT_TRUE);
        st->put_StopIfGoingOnBatteries(VARIANT_TRUE);
        st->put_RunOnlyIfIdle(VARIANT_FALSE);
        st->put_WakeToRun(VARIANT_FALSE);
        st->put_AllowDemandStart(VARIANT_TRUE);
        st->put_Enabled(VARIANT_TRUE);
        st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        st->put_ExecutionTimeLimit(Bstr(L"PT2H"));
        st->put_Priority(7);  // below normal
    }
    {
        Com<ITriggerCollection> tc;
        hr = def->get_Triggers(&tc);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the task"), hr, err);
        TASK_TRIGGER_TYPE2 type = when.freq == ScheduleFreq::Daily    ? TASK_TRIGGER_DAILY
                                  : when.freq == ScheduleFreq::Weekly ? TASK_TRIGGER_WEEKLY
                                  : when.freq == ScheduleFreq::Monthly ? TASK_TRIGGER_MONTHLY
                                                                       : TASK_TRIGGER_LOGON;
        Com<ITrigger> tr;
        hr = tc->Create(type, &tr);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the trigger"), hr, err);
        SYSTEMTIME now;
        GetLocalTime(&now);
        tr->put_StartBoundary(Bstr(ScheduleStartBoundary(when, now.wYear, now.wMonth, now.wDay, now.wDayOfWeek, now.wHour, now.wMinute)));
        tr->put_Enabled(VARIANT_TRUE);
        if (when.freq == ScheduleFreq::Daily) {
            Com<IDailyTrigger> t;
            if (SUCCEEDED(tr->QueryInterface(IID_IDailyTrigger, reinterpret_cast<void**>(&t)))) t->put_DaysInterval(1);
        } else if (when.freq == ScheduleFreq::Weekly) {
            Com<IWeeklyTrigger> t;
            if (SUCCEEDED(tr->QueryInterface(IID_IWeeklyTrigger, reinterpret_cast<void**>(&t)))) {
                t->put_DaysOfWeek(static_cast<short>(1 << std::clamp(when.weekday, 0, 6)));
                t->put_WeeksInterval(1);
            }
        } else if (when.freq == ScheduleFreq::Monthly) {
            Com<IMonthlyTrigger> t;
            if (SUCCEEDED(tr->QueryInterface(IID_IMonthlyTrigger, reinterpret_cast<void**>(&t)))) {
                t->put_DaysOfMonth(static_cast<long>(1u << (std::clamp(when.monthDay, 1, 28) - 1)));
                t->put_MonthsOfYear(0xFFF);
            }
        } else {
            Com<ILogonTrigger> t;
            if (SUCCEEDED(tr->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&t)))) {
                if (!user.empty()) t->put_UserId(Bstr(user));  // your logon only (any user's needs admin)
                t->put_Delay(Bstr(L"PT5M"));                   // not in the way of signing in
            }
        }
    }
    {
        Com<IActionCollection> ac;
        hr = def->get_Actions(&ac);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the task"), hr, err);
        Com<IAction> a;
        hr = ac->Create(TASK_ACTION_EXEC, &a);
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the action"), hr, err);
        Com<IExecAction> ex;
        hr = a->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&ex));
        if (FAILED(hr)) return Fail(N_(L"Couldn't set up the action"), hr, err);
        ex->put_Path(Bstr(exe));
        ex->put_Arguments(Bstr(args));
        size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) ex->put_WorkingDirectory(Bstr(exe.substr(0, slash)));
    }
    Com<IRegisteredTask> reg;
    hr = folder->RegisterTaskDefinition(Bstr(name), def.p, TASK_CREATE_OR_UPDATE, Empty(), Empty(), TASK_LOGON_INTERACTIVE_TOKEN,
                                        Empty(), &reg);
    if (FAILED(hr)) return Fail(N_(L"Couldn't save the task"), hr, err);
    return true;
}

bool TaskQuery(const std::wstring& folderPath, const std::wstring& name, TaskInfo& info, std::wstring& err) {
    info = TaskInfo{};
    Com<ITaskService> svc;
    if (!Connect(svc, err)) return false;
    Com<ITaskFolder> folder;
    HRESULT hr = svc->GetFolder(Bstr(folderPath), &folder);
    if (NotFound(hr)) return true;
    if (FAILED(hr)) return Fail(N_(L"Couldn't open the task folder"), hr, err);
    Com<IRegisteredTask> task;
    hr = folder->GetTask(Bstr(name), &task);
    if (NotFound(hr)) return true;
    if (FAILED(hr)) return Fail(N_(L"Couldn't read the task"), hr, err);
    info.exists = true;
    VARIANT_BOOL en = VARIANT_FALSE;
    if (SUCCEEDED(task->get_Enabled(&en))) info.enabled = en != VARIANT_FALSE;
    DATE d = 0;
    if (SUCCEEDED(task->get_NextRunTime(&d))) info.nextRun = DateToTicks(d);
    d = 0;
    if (SUCCEEDED(task->get_LastRunTime(&d))) info.lastRun = DateToTicks(d);
    LONG res = 0;
    if (SUCCEEDED(task->get_LastTaskResult(&res))) info.lastResult = static_cast<uint32_t>(res);
    Com<ITaskDefinition> def;
    if (FAILED(task->get_Definition(&def))) return true;
    {
        Com<IActionCollection> ac;
        Com<IAction> a;
        Com<IExecAction> ex;
        if (SUCCEEDED(def->get_Actions(&ac)) && SUCCEEDED(ac->get_Item(1, &a)) &&
            SUCCEEDED(a->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&ex)))) {
            BSTR b = nullptr;
            if (SUCCEEDED(ex->get_Path(&b))) info.path = Take(b);
            b = nullptr;
            if (SUCCEEDED(ex->get_Arguments(&b))) info.arguments = Take(b);
        }
    }
    ParseCleanArguments(info.arguments, info.spec.ids);
    Com<ITriggerCollection> tc;
    Com<ITrigger> tr;
    LONG count = 0;
    if (FAILED(def->get_Triggers(&tc)) || FAILED(tc->get_Count(&count)) || count != 1 || FAILED(tc->get_Item(1, &tr))) return true;
    TASK_TRIGGER_TYPE2 type;
    if (FAILED(tr->get_Type(&type))) return true;
    BSTR b = nullptr;
    std::wstring boundary = SUCCEEDED(tr->get_StartBoundary(&b)) ? Take(b) : L"";
    ScheduleSpec& s = info.spec;
    bool timed = ParseBoundaryTime(boundary, s.hour, s.minute);
    if (type == TASK_TRIGGER_LOGON) {
        s.freq = ScheduleFreq::Logon;
        info.specKnown = true;
    } else if (type == TASK_TRIGGER_DAILY) {
        s.freq = ScheduleFreq::Daily;
        info.specKnown = timed;
    } else if (type == TASK_TRIGGER_WEEKLY) {
        Com<IWeeklyTrigger> t;
        short mask = 0;
        if (SUCCEEDED(tr->QueryInterface(IID_IWeeklyTrigger, reinterpret_cast<void**>(&t))) && SUCCEEDED(t->get_DaysOfWeek(&mask))) {
            s.freq = ScheduleFreq::Weekly;
            s.weekday = WeekdayFromMask(static_cast<unsigned short>(mask));
            info.specKnown = timed && s.weekday >= 0;
            if (s.weekday < 0) s.weekday = 0;
        }
    } else if (type == TASK_TRIGGER_MONTHLY) {
        Com<IMonthlyTrigger> t;
        long mask = 0;
        if (SUCCEEDED(tr->QueryInterface(IID_IMonthlyTrigger, reinterpret_cast<void**>(&t))) && SUCCEEDED(t->get_DaysOfMonth(&mask))) {
            s.freq = ScheduleFreq::Monthly;
            s.monthDay = MonthDayFromMask(static_cast<unsigned long>(mask));
            info.specKnown = timed && s.monthDay >= 1 && s.monthDay <= 28;
            s.monthDay = std::clamp(s.monthDay, 1, 28);
        }
    }
    return true;
}

bool TaskRemove(const std::wstring& folderPath, const std::wstring& name, std::wstring& err) {
    Com<ITaskService> svc;
    if (!Connect(svc, err)) return false;
    Com<ITaskFolder> folder;
    HRESULT hr = svc->GetFolder(Bstr(folderPath), &folder);
    if (NotFound(hr)) return true;
    if (FAILED(hr)) return Fail(N_(L"Couldn't open the task folder"), hr, err);
    hr = folder->DeleteTask(Bstr(name), 0);
    if (FAILED(hr) && !NotFound(hr)) return Fail(N_(L"Couldn't remove the task"), hr, err);
    // The folder goes too once nothing else is in it (hidden tasks counted).
    Com<IRegisteredTaskCollection> tasks;
    Com<ITaskFolderCollection> sub;
    LONG nt = 1, nf = 1;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && SUCCEEDED(tasks->get_Count(&nt)) && SUCCEEDED(folder->GetFolders(0, &sub)) &&
        SUCCEEDED(sub->get_Count(&nf)) && nt == 0 && nf == 0) {
        Com<ITaskFolder> root;
        if (SUCCEEDED(svc->GetFolder(Bstr(L"\\"), &root))) root->DeleteFolder(Bstr(FolderLeaf(folderPath)), 0);
    }
    return true;
}

std::wstring ScheduleFolder() {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"BYTOPSY_TASK_FOLDER", over, MAX_PATH);
    if (n && n < MAX_PATH && _wcsnicmp(over, L"\\Bytopsy-test", 13) == 0) return over;
    return kScheduleFolder;
}

// =============================================================================================
// Log

namespace {
std::mutex g_logLock;
}

std::wstring CleanLogFile() {
    std::wstring j = JournalFile();
    size_t slash = j.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : j.substr(0, slash) + L"\\cleanup.log";
}

void CleanLogAppend(const std::wstring& text) {
    std::wstring file = CleanLogFile();
    if (file.empty()) return;
    std::lock_guard<std::mutex> lk(g_logLock);
    CreateDirectoryW(file.substr(0, file.find_last_of(L"\\/")).c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) &&
        ((static_cast<int64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) > (1 << 20))
        MoveFileExW(file.c_str(), (file.substr(0, file.size() - 4) + L".1.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"ab") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t head[32];
    swprintf(head, 32, L"%04u-%02u-%02u %02u:%02u:%02u  ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = head + text;
    for (auto& c : line)
        if (c == L'\r' || c == L'\n') c = L' ';
    std::string u = util::WideToUtf8(line + L"\r\n");
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}
