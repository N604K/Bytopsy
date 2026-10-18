#pragma once

// Browser cookies and history (Cleanup's "Browser cookies" and "Browser history" categories). Both
// live in SQLite databases, so they are read and changed with SQLite itself: cookies are deleted
// row by row, keeping the sites on the "keep me signed in to" list; Firefox's history is deleted
// without touching its bookmarks, which share the same database.

#include <cstdint>
#include <string>
#include <vector>

enum class BrowserData { None, Cookies, History };

// %LOCALAPPDATA%\Bytopsy\keep-signed-in.txt (BYTOPSY_KEEP_SIGNED_IN overrides it, for tests).
std::wstring KeepSignedInFile();
// The sites in the list, normalized ("https://www.GitHub.com/login" -> "github.com"). Missing file: empty.
std::vector<std::wstring> LoadKeepList(const std::wstring& file);
// Writes the list with its explanation and `sites` if the file doesn't exist yet.
bool CreateKeepList(const std::wstring& file, const std::vector<std::wstring>& sites);
// A cookie's host (".google.com", "accounts.google.com") belongs to a kept site, or a subdomain of one.
bool KeepHost(const std::vector<std::wstring>& keep, const std::wstring& host);
// "mail.google.com" -> "google.com", "www.bbc.co.uk" -> "bbc.co.uk" (a heuristic, no suffix list).
std::wstring SiteOf(const std::wstring& host);

// One cookie or history database of a browser profile, and what cleaning it would remove.
struct BrowserDb {
    std::wstring file;      // Network\Cookies, cookies.sqlite, History or places.sqlite
    bool firefox = false;
    uint64_t rows = 0;      // cookies or visits that cleaning removes
    uint64_t keptRows = 0;  // cookies of kept sites
    size_t keptSites = 0;   // sites of the keep list that have cookies here
    uint64_t bytes = 0;     // about what cleaning frees on disk
    int64_t mtime = 0;
};

// The databases of one profile folder (Chromium: "Default", "Profile 1", Opera's folder; Firefox:
// a folder under Profiles), counted read-only: the files are never locked or written, so this works
// while the browser runs. A database that isn't what its name says is skipped (`err` says why).
std::vector<BrowserDb> InspectProfile(const std::wstring& dir, BrowserData kind, const std::vector<std::wstring>& keep,
                                      std::wstring* err = nullptr);
// The most visited sites in these profiles' histories (read-only), most visits first.
std::vector<std::wstring> MostVisitedSites(const std::vector<std::wstring>& profileDirs, size_t max);

struct BrowserCleanResult {
    uint64_t rows = 0;   // cookies or visits removed
    uint64_t freed = 0;  // bytes
    bool busy = false;   // the browser has the database open
};
// Cookies: every cookie except those of kept sites. History: Chromium's History file and the files
// made from it (Visited Links, Top Sites, Shortcuts, Network Action Predictor); Firefox's visits and
// the pages nothing else refers to (bookmarked pages and keywords stay). The browser must be closed.
bool CleanBrowserDb(const std::wstring& file, BrowserData kind, const std::vector<std::wstring>& keep,
                    BrowserCleanResult& result, std::wstring& err);
