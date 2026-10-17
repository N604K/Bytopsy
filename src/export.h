#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "tree.h"

// ncdu-compatible JSON export ("ncdu -f file" can read it, and vice versa).
bool ExportNcduJson(const Node* root, const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err);
// CSV with one row per item: path, type, sizes, items, modified, flags. UTF-8 with BOM (Excel friendly).
bool ExportCsv(const Node* root, const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err);
// Loads an ncdu JSON export. Totals are computed.
std::unique_ptr<Node> ImportNcduJson(const std::wstring& file, std::atomic<uint64_t>& count, std::wstring& err);
