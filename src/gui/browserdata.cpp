#include "browserdata.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>

#include "../fsops.h"
#include "../tree.h"
#include "../util.h"
#include "sqlite3.h"

namespace {

int64_t FtToI(const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

uint64_t FileSize(const std::wstring& p, int64_t* mtime = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(ToLongPath(p).c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    if (mtime) *mtime = FtToI(fa.ftLastWriteTime);
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(ToLongPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A database with its journal / WAL files: what it takes on disk.
uint64_t DbSize(const std::wstring& file) {
    return FileSize(file) + FileSize(file + L"-journal") + FileSize(file + L"-wal") + FileSize(file + L"-shm");
}

std::wstring Trim(const std::wstring& v) {
    size_t a = v.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = v.find_last_not_of(L" \t\r\n");
    return v.substr(a, b - a + 1);
}

std::wstring FileName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return util::ToLower(s == std::wstring::npos ? p : p.substr(s + 1));
}

std::wstring ParentOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// ---- SQLite

struct Db {
    sqlite3* h = nullptr;
    Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db() {
        if (h) sqlite3_close_v2(h);
    }
};

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &s, nullptr); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    ~Stmt() { sqlite3_finalize(s); }
    bool Row() { return s && sqlite3_step(s) == SQLITE_ROW; }
    int64_t Int(int i) { return sqlite3_column_int64(s, i); }
    std::wstring Text(int i) {
        const unsigned char* t = sqlite3_column_text(s, i);
        return t ? util::Utf8ToWide(reinterpret_cast<const char*>(t)) : L"";
    }
};

// Opens a database for reading only, as an "immutable" snapshot: SQLite takes no locks and writes
// nothing (no journal, no -shm file), so a running browser is never disturbed. What a browser has
// not written back yet (its WAL) isn't seen, which is fine for counting.
bool OpenSnapshot(const std::wstring& file, Db& db) {
    std::string u = util::WideToUtf8(file), uri = "file:";
    if (u.size() >= 2 && u[1] == ':') uri += "///";
    for (char ch : u) {
        if (ch == '\\') {
            uri += '/';
        } else if (ch == '%' || ch == '?' || ch == '#' || ch == ' ') {
            char b[4];
            snprintf(b, sizeof(b), "%%%02X", static_cast<unsigned char>(ch));
            uri += b;
        } else {
            uri += ch;
        }
    }
    uri += "?mode=ro&immutable=1";
    return sqlite3_open_v2(uri.c_str(), &db.h, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) == SQLITE_OK;
}

// Opens an existing database for changes (never creates one).
int OpenForWriting(const std::wstring& file, Db& db) {
    int rc = sqlite3_open_v2(util::WideToUtf8(file).c_str(), &db.h, SQLITE_OPEN_READWRITE, nullptr);
    if (rc == SQLITE_OK) sqlite3_busy_timeout(db.h, 1500);
    return rc;
}

bool HasColumn(sqlite3* db, const char* table, const char* column) {
    std::string sql = std::string("PRAGMA table_info(\"") + table + "\")";
    Stmt st(db, sql.c_str());
    while (st.Row())
        if (_wcsicmp(st.Text(1).c_str(), util::Utf8ToWide(column).c_str()) == 0) return true;
    return false;
}

bool HasTable(sqlite3* db, const char* table) {
    Stmt st(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
    if (!st.s) return false;
    sqlite3_bind_text(st.s, 1, table, -1, SQLITE_STATIC);
    return st.Row();
}

int64_t Count(sqlite3* db, const char* sql) {
    Stmt st(db, sql);
    return st.Row() ? st.Int(0) : -1;
}

std::wstring LastError(sqlite3* db) {
    return db ? util::Utf8ToWide(sqlite3_errmsg(db)) : L"out of memory";
}

bool Busy(int rc) {
    rc &= 0xFF;
    return rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
}

// Which kind of database a file is, by name; the schema is checked before anything is read or changed.
enum class DbKind { Unknown, ChromiumCookies, FirefoxCookies, ChromiumHistory, FirefoxPlaces };

DbKind KindOf(const std::wstring& file) {
    std::wstring n = FileName(file);
    if (n == L"cookies") return DbKind::ChromiumCookies;
    if (n == L"cookies.sqlite") return DbKind::FirefoxCookies;
    if (n == L"history") return DbKind::ChromiumHistory;
    if (n == L"places.sqlite") return DbKind::FirefoxPlaces;
    return DbKind::Unknown;
}

bool SchemaOk(sqlite3* db, DbKind k) {
    switch (k) {
        case DbKind::ChromiumCookies: return HasTable(db, "cookies") && HasColumn(db, "cookies", "host_key");
        case DbKind::FirefoxCookies: return HasTable(db, "moz_cookies") && HasColumn(db, "moz_cookies", "host");
        case DbKind::ChromiumHistory: return HasTable(db, "urls") && HasTable(db, "visits") && HasColumn(db, "urls", "visit_count");
        case DbKind::FirefoxPlaces:
            return HasTable(db, "moz_places") && HasTable(db, "moz_historyvisits") && HasTable(db, "moz_bookmarks") &&
                   HasColumn(db, "moz_places", "foreign_count") && HasColumn(db, "moz_bookmarks", "fk");
        default: return false;
    }
}

// Chromium keeps more history next to the History database: link colors, the new tab page's most
// visited sites, address bar shortcuts and predictions. All of it is made from history, and
// Chromium's own "Clear browsing history" clears it too.
const wchar_t* const kHistoryCompanions[] = {L"History-journal", L"History-wal", L"History-shm", L"Visited Links",
                                             L"Top Sites", L"Top Sites-journal", L"Shortcuts", L"Shortcuts-journal",
                                             L"Network Action Predictor", L"Network Action Predictor-journal"};

// Firefox: pages that nothing but history refers to (not bookmarked, no keyword).
const char kOrphanPlaces[] =
    "foreign_count = 0 AND id NOT IN (SELECT fk FROM moz_bookmarks WHERE fk NOT NULL)";

std::string OrphanPlacesWhere(sqlite3* db) {
    std::string w = kOrphanPlaces;
    if (HasTable(db, "moz_keywords") && HasColumn(db, "moz_keywords", "place_id"))
        w += " AND id NOT IN (SELECT place_id FROM moz_keywords WHERE place_id NOT NULL)";
    return w;
}

// The host of an http(s) address, or empty.
std::wstring HostOfUrl(const std::wstring& url) {
    std::wstring u = util::ToLower(url);
    size_t start;
    if (u.rfind(L"https://", 0) == 0) start = 8;
    else if (u.rfind(L"http://", 0) == 0) start = 7;
    else return L"";
    size_t end = u.find_first_of(L"/?#", start);
    std::wstring host = u.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host = host.substr(0, colon);
    return host;
}

std::wstring NormalizeSite(const std::wstring& line) {
    std::wstring s = util::ToLower(Trim(line));
    if (s.find_first_of(L" \t") != std::wstring::npos) return L"";  // not one site
    size_t scheme = s.find(L"://");
    if (scheme != std::wstring::npos) s = s.substr(scheme + 3);
    size_t end = s.find_first_of(L"/?#:");
    if (end != std::wstring::npos) s = s.substr(0, end);
    while (!s.empty() && (s[0] == L'*' || s[0] == L'.')) s.erase(0, 1);
    while (!s.empty() && s.back() == L'.') s.pop_back();
    if (s.rfind(L"www.", 0) == 0) s.erase(0, 4);
    for (wchar_t ch : s)
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch > 127)) return L"";
    return s;
}

// Cleaning: SQL's view of the keep list.
void KeepFn(sqlite3_context* ctx, int, sqlite3_value** v) {
    auto* keep = static_cast<const std::vector<std::wstring>*>(sqlite3_user_data(ctx));
    const unsigned char* t = sqlite3_value_text(v[0]);
    sqlite3_result_int(ctx, t && KeepHost(*keep, util::Utf8ToWide(reinterpret_cast<const char*>(t))) ? 1 : 0);
}

bool Exec(sqlite3* db, const char* sql, std::wstring& err) {
    char* msg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        err = msg ? util::Utf8ToWide(msg) : LastError(db);
        sqlite3_free(msg);
        return false;
    }
    return true;
}

// Hands the freed pages back to the disk: the WAL is folded in and the file rewritten. Failing here
// changes nothing (the deletion is already committed).
void Compact(sqlite3* db) {
    std::wstring ignored;
    Exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", ignored);
    Exec(db, "VACUUM", ignored);
    Exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", ignored);
}

const char kKeepTemplate[] =
    "# Sites WinNCDU keeps you signed in to when it deletes browser cookies (Cleanup \xE2\x86\x92 Browser cookies).\r\n"
    "# One site per line. A site covers its subdomains: google.com keeps mail.google.com too.\r\n"
    "# Signing in often takes more than one site: YouTube needs youtube.com and google.com, a Microsoft\r\n"
    "# account live.com, microsoft.com and microsoftonline.com.\r\n"
    "# Save this file, then click Measure in Cleanup.\r\n"
    "#\r\n";

}  // namespace

std::wstring KeepSignedInFile() {
    wchar_t over[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"WINNCDU_KEEP_SIGNED_IN", over, MAX_PATH);  // tests
    if (n && n < MAX_PATH) return over;
    PWSTR p = nullptr;
    std::wstring file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) file = std::wstring(p) + L"\\WinNCDU\\keep-signed-in.txt";
    CoTaskMemFree(p);
    return file;
}

std::vector<std::wstring> LoadKeepList(const std::wstring& file) {
    std::vector<std::wstring> out;
    FILE* f = nullptr;
    if (file.empty() || _wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return out;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) data.erase(0, 3);
    std::wstring text = util::Utf8ToWide(data);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        std::wstring site = NormalizeSite(line);
        if (!site.empty() && std::find(out.begin(), out.end(), site) == out.end()) out.push_back(site);
    }
    return out;
}

bool CreateKeepList(const std::wstring& file, const std::vector<std::wstring>& sites) {
    if (file.empty() || GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    CreateDirectoryW(ParentOf(file).c_str(), nullptr);
    std::string text = kKeepTemplate;
    if (sites.empty()) {
        text += "# No browsing history was found when this list was made. Add the sites you want to stay\r\n"
                "# signed in to, for example:\r\n# github.com\r\n";
    } else {
        text += "# Your most visited sites when this list was made. Remove any you don't need to stay signed in to.\r\n";
        for (auto& s : sites) text += util::WideToUtf8(s) + "\r\n";
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

bool KeepHost(const std::vector<std::wstring>& keep, const std::wstring& host) {
    std::wstring h = util::ToLower(host);
    while (!h.empty() && h[0] == L'.') h.erase(0, 1);
    for (auto& k : keep) {
        if (h == k) return true;
        // A subdomain, on a label boundary: "notgoogle.com" is not "google.com".
        if (h.size() > k.size() && h.compare(h.size() - k.size(), k.size(), k) == 0 && h[h.size() - k.size() - 1] == L'.') return true;
    }
    return false;
}

std::wstring SiteOf(const std::wstring& host) {
    std::wstring h = util::ToLower(host);
    while (!h.empty() && h[0] == L'.') h.erase(0, 1);
    if (h.find_first_not_of(L"0123456789.") == std::wstring::npos) return h;  // an IP address
    std::vector<std::wstring> labels;
    for (size_t i = 0; i <= h.size();) {
        size_t j = h.find(L'.', i);
        if (j == std::wstring::npos) j = h.size();
        labels.push_back(h.substr(i, j - i));
        i = j + 1;
    }
    if (labels.size() <= 2) return h;
    size_t take = 2;
    // Country domains with a second level ("co.uk", "com.au", "co.il"): one label more.
    static const wchar_t* const kSecond[] = {L"co", L"com", L"org", L"net", L"ac", L"gov", L"edu", L"ne", L"or", L"go", L"gob", L"mil", L"ltd", L"plc", L"sch", L"nic"};
    const std::wstring& sld = labels[labels.size() - 2];
    if (labels.back().size() == 2 && std::any_of(std::begin(kSecond), std::end(kSecond), [&](const wchar_t* s) { return sld == s; }))
        take = 3;
    std::wstring out;
    for (size_t i = labels.size() - take; i < labels.size(); ++i) out += (out.empty() ? L"" : L".") + labels[i];
    return out;
}

std::vector<BrowserDb> InspectProfile(const std::wstring& dir, BrowserData kind, const std::vector<std::wstring>& keep, std::wstring* err) {
    std::vector<BrowserDb> out;
    std::vector<std::wstring> candidates;
    if (kind == BrowserData::Cookies)
        candidates = {dir + L"\\Network\\Cookies", dir + L"\\Cookies", dir + L"\\cookies.sqlite"};
    else if (kind == BrowserData::History)
        candidates = {dir + L"\\History", dir + L"\\places.sqlite"};
    for (auto& file : candidates) {
        if (!IsFile(file)) continue;
        DbKind k = KindOf(file);
        Db db;
        if (!OpenSnapshot(file, db) || !SchemaOk(db.h, k)) {
            if (err) *err = file + L": not a browser database this version of WinNCDU knows (" + LastError(db.h) + L")";
            continue;
        }
        BrowserDb b;
        b.file = file;
        b.firefox = k == DbKind::FirefoxCookies || k == DbKind::FirefoxPlaces;
        uint64_t size = FileSize(file, &b.mtime);
        if (kind == BrowserData::Cookies) {
            Stmt st(db.h, k == DbKind::ChromiumCookies ? "SELECT host_key, COUNT(*) FROM cookies GROUP BY host_key"
                                                         : "SELECT host, COUNT(*) FROM moz_cookies GROUP BY host");
            std::vector<bool> siteUsed(keep.size(), false);
            while (st.Row()) {
                std::wstring host = st.Text(0);
                uint64_t n = static_cast<uint64_t>(st.Int(1));
                if (!KeepHost(keep, host)) {
                    b.rows += n;
                    continue;
                }
                b.keptRows += n;
                for (size_t i = 0; i < keep.size(); ++i)
                    if (!siteUsed[i] && KeepHost({keep[i]}, host)) siteUsed[i] = true;
            }
            b.keptSites = static_cast<size_t>(std::count(siteUsed.begin(), siteUsed.end(), true));
            uint64_t all = b.rows + b.keptRows;
            b.bytes = all ? size * b.rows / all : 0;
        } else if (k == DbKind::ChromiumHistory) {
            int64_t n = Count(db.h, "SELECT COUNT(*) FROM visits");
            b.rows = n > 0 ? static_cast<uint64_t>(n) : 0;
            b.bytes = size;
            for (const wchar_t* c : kHistoryCompanions) b.bytes += FileSize(dir + L"\\" + c);
        } else {
            int64_t n = Count(db.h, "SELECT COUNT(*) FROM moz_historyvisits");
            b.rows = n > 0 ? static_cast<uint64_t>(n) : 0;
            int64_t places = Count(db.h, "SELECT COUNT(*) FROM moz_places");
            int64_t orphans = Count(db.h, ("SELECT COUNT(*) FROM moz_places WHERE " + OrphanPlacesWhere(db.h)).c_str());
            b.bytes = places > 0 && orphans > 0 ? size * static_cast<uint64_t>(orphans) / static_cast<uint64_t>(places) : 0;
        }
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<std::wstring> MostVisitedSites(const std::vector<std::wstring>& profileDirs, size_t max) {
    std::unordered_map<std::wstring, uint64_t> visits;
    for (auto& dir : profileDirs) {
        for (const wchar_t* name : {L"\\History", L"\\places.sqlite"}) {
            std::wstring file = dir + name;
            if (!IsFile(file)) continue;
            DbKind k = KindOf(file);
            Db db;
            if (!OpenSnapshot(file, db) || !SchemaOk(db.h, k)) continue;
            Stmt st(db.h, k == DbKind::ChromiumHistory
                              ? "SELECT url, visit_count FROM urls WHERE visit_count > 0 ORDER BY visit_count DESC LIMIT 5000"
                              : "SELECT url, visit_count FROM moz_places WHERE visit_count > 0 ORDER BY visit_count DESC LIMIT 5000");
            while (st.Row()) {
                std::wstring host = HostOfUrl(st.Text(0));
                if (host.find(L'.') == std::wstring::npos || host.find_first_not_of(L"0123456789.") == std::wstring::npos) continue;
                visits[SiteOf(host)] += static_cast<uint64_t>(st.Int(1));
            }
        }
    }
    std::vector<std::pair<std::wstring, uint64_t>> sorted(visits.begin(), visits.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::vector<std::wstring> out;
    for (auto& [site, n] : sorted) {
        if (out.size() >= max || n < 3) break;  // a site visited once or twice isn't one you sign in to
        out.push_back(site);
    }
    return out;
}

bool CleanBrowserDb(const std::wstring& file, BrowserData kind, const std::vector<std::wstring>& keep, BrowserCleanResult& r,
                    std::wstring& err) {
    DbKind k = KindOf(file);
    bool cookies = k == DbKind::ChromiumCookies || k == DbKind::FirefoxCookies;
    if (k == DbKind::Unknown || cookies != (kind == BrowserData::Cookies) || kind == BrowserData::None) {
        err = L"not a cookie or history database";
        return false;
    }
    if (k == DbKind::ChromiumHistory) {
        // The whole file holds history (and the downloads list) only: it goes, with the files made from it.
        {
            Db db;
            if (!OpenSnapshot(file, db) || !SchemaOk(db.h, k)) {
                err = L"not a Chromium history database";
                return false;
            }
            int64_t n = Count(db.h, "SELECT COUNT(*) FROM visits");
            r.rows = n > 0 ? static_cast<uint64_t>(n) : 0;
        }
        std::atomic<bool> cancel{false};
        std::atomic<uint64_t> removed{0};
        uint64_t size = FileSize(file);
        if (!DeletePermanently(file, cancel, removed, err)) {
            r.rows = 0;
            r.busy = PathExists(file);
            return false;
        }
        r.freed += size;
        std::wstring dir = ParentOf(file);
        for (const wchar_t* c : kHistoryCompanions) {
            std::wstring p = dir + L"\\" + c;
            uint64_t s = FileSize(p);
            std::wstring e2;
            if (IsFile(p) && DeletePermanently(p, cancel, removed, e2)) r.freed += s;
        }
        return true;
    }

    uint64_t before = DbSize(file);
    Db db;
    int rc = OpenForWriting(file, db);
    if (rc != SQLITE_OK) {
        r.busy = Busy(rc) || (rc & 0xFF) == SQLITE_CANTOPEN;
        err = L"couldn't open it: " + LastError(db.h);
        return false;
    }
    if (!SchemaOk(db.h, k)) {
        r.busy = Busy(sqlite3_errcode(db.h));
        err = r.busy ? L"the browser has it open" : L"not the database WinNCDU expected; left alone";
        return false;
    }
    rc = sqlite3_exec(db.h, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        r.busy = Busy(rc);
        err = r.busy ? L"the browser has it open" : LastError(db.h);
        return false;
    }
    bool ok = true;
    if (cookies) {
        sqlite3_create_function(db.h, "winncdu_keep", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, const_cast<std::vector<std::wstring>*>(&keep),
                                KeepFn, nullptr, nullptr);
        ok = Exec(db.h, k == DbKind::ChromiumCookies ? "DELETE FROM cookies WHERE NOT winncdu_keep(host_key)"
                                                       : "DELETE FROM moz_cookies WHERE NOT winncdu_keep(host)",
                  err);
        if (ok) r.rows = static_cast<uint64_t>(sqlite3_changes64(db.h));
    } else {
        // Firefox keeps bookmarks in the same database, pointing at moz_places: only visits and the
        // pages nothing else refers to are deleted. Firefox's own bookkeeping triggers are temporary
        // (created by Firefox when it runs), so what they would do is done here: rows of other tables
        // that belonged to deleted pages go too, and kept pages lose their visit counts.
        ok = Exec(db.h, "DELETE FROM moz_historyvisits", err);
        if (ok) r.rows = static_cast<uint64_t>(sqlite3_changes64(db.h));
        if (ok) ok = Exec(db.h, ("DELETE FROM moz_places WHERE " + OrphanPlacesWhere(db.h)).c_str(), err);
        std::string reset = "UPDATE moz_places SET visit_count = 0, last_visit_date = NULL";
        if (HasColumn(db.h, "moz_places", "typed")) reset += ", typed = 0";
        if (HasColumn(db.h, "moz_places", "recalc_frecency")) reset += ", recalc_frecency = 1";
        if (ok) ok = Exec(db.h, reset.c_str(), err);
        for (const char* t : {"moz_annos", "moz_inputhistory", "moz_places_metadata", "moz_places_extra"})
            if (ok && HasTable(db.h, t) && HasColumn(db.h, t, "place_id"))
                ok = Exec(db.h, (std::string("DELETE FROM ") + t + " WHERE place_id NOT IN (SELECT id FROM moz_places)").c_str(), err);
        if (ok && HasTable(db.h, "moz_historyvisits_extra")) ok = Exec(db.h, "DELETE FROM moz_historyvisits_extra", err);
        if (ok && HasTable(db.h, "moz_places_metadata_search_queries") && HasColumn(db.h, "moz_places_metadata", "search_query_id"))
            ok = Exec(db.h,
                      "DELETE FROM moz_places_metadata_search_queries WHERE id NOT IN "
                      "(SELECT search_query_id FROM moz_places_metadata WHERE search_query_id NOT NULL)",
                      err);
        if (ok && HasTable(db.h, "moz_origins") && HasColumn(db.h, "moz_places", "origin_id"))
            ok = Exec(db.h, "DELETE FROM moz_origins WHERE id NOT IN (SELECT origin_id FROM moz_places WHERE origin_id NOT NULL)", err);
    }
    if (!ok || !Exec(db.h, "COMMIT", err)) {
        std::wstring ignored;
        Exec(db.h, "ROLLBACK", ignored);
        r.rows = 0;
        return false;
    }
    Compact(db.h);
    sqlite3_close_v2(db.h);
    db.h = nullptr;
    uint64_t after = DbSize(file);
    r.freed = before > after ? before - after : 0;
    return true;
}
