#pragma once

// Deletion journal: every delete, cleanup and restore is appended to
// journal.log in util::DataDir() (UTF-8, one tab-separated line per action), so people can see
// later what Bytopsy removed, and bring back what is still in the Recycle Bin.

#include <cstdint>
#include <string>
#include <vector>

enum class JournalAction : uint8_t {
    Recycle,  // moved to the Recycle Bin
    Delete,   // deleted permanently
    Clean,    // a Cleanup category (path = the category's name)
    Restore,  // brought back from the Recycle Bin
};

struct JournalEntry {
    int64_t time = 0;  // FILETIME ticks (UTC)
    JournalAction action = JournalAction::Delete;
    uint64_t bytes = 0;
    uint64_t items = 0;  // files and folders, the item itself included
    std::wstring path;
    std::wstring note;  // e.g. "partly deleted", "12 items in use were skipped"
};

std::wstring JournalFile();
const wchar_t* JournalActionName(JournalAction a);
// Thread-safe. Failures are ignored: the journal must never stand in the way of a delete.
void JournalAppend(const JournalEntry& e);
// Newest first.
std::vector<JournalEntry> JournalRead(size_t maxEntries = 1000);

// ---- Recycle Bin (the current user's bins on local drives)

struct RecycledItem {
    std::wstring original;  // where it was deleted from
    std::wstring dataFile;  // X:\$Recycle.Bin\<SID>\$R... (the item itself)
    std::wstring infoFile;  // X:\$Recycle.Bin\<SID>\$I... (original path, size and time)
    int64_t deleted = 0;    // FILETIME ticks (UTC)
    uint64_t size = 0;
};

std::vector<RecycledItem> ListRecycleBin();
// The bin item that a Recycle entry of the journal refers to, or null (emptied or restored).
const RecycledItem* FindRecycled(const std::vector<RecycledItem>& bin, const JournalEntry& e);
// Moves the item back to where it was deleted from; never overwrites. Logs a Restore entry.
bool RestoreRecycled(const RecycledItem& item, std::wstring& err);
