#pragma once

// Developer junk: build output and dependency folders that a project recreates on its next build
// or install (node_modules, target, bin/obj, .gradle, Unity Library, __pycache__, .venv, ...),
// found in a scan tree and grouped by project.

#include <cstdint>
#include <string>
#include <vector>

struct Node;

struct DevArtifact {
    Node* node = nullptr;
    const wchar_t* kind = L"";  // "Node.js", "Rust", ".NET", ...
    const wchar_t* how = L"";   // how it comes back: "npm install", "cargo build", ...
};

struct DevProject {
    Node* dir = nullptr;               // the project folder
    std::vector<DevArtifact> artifacts;  // largest first (apparent size)
    int64_t lastChange = 0;            // newest file outside the artifacts and .git (FILETIME ticks)
    std::wstring kinds;                // "Node.js, Python"
};

// Every artifact folder must be confirmed by its project (node_modules next to a package.json,
// target next to Cargo.toml or pom.xml, build with a CMakeCache.txt inside, ...). App and system
// locations (AppData, Program Files, Windows, ProgramData, dot-folders such as .vscode) are never
// searched: dependencies there belong to installed programs, not to your projects.
// Sorted by artifact size, largest first.
std::vector<DevProject> FindDevJunk(Node* root);

// Total apparent (`disk` false) or allocated size of a project's artifacts.
uint64_t DevProjectSize(const DevProject& p, bool disk);
