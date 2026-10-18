#include "devjunk.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "tree.h"
#include "i18n.h"
#include "util.h"

namespace {

// One kind of artifact folder.
struct Rule {
    const wchar_t* name;     // folder name, lowercase ('*' allowed)
    const wchar_t* kind;
    const wchar_t* how;
    // Next to the folder (in the project folder), any one of these; "a+b" needs both; a trailing
    // '\' means a folder. Empty: only `inside` decides, or (with `anywhere`) nothing does.
    std::vector<const wchar_t*> beside;
    const wchar_t* inside = nullptr;  // a file the folder itself must contain
    bool anywhere = false;            // caches that are always safe: grouped by the nearest project
};

const std::vector<Rule>& Rules() {
    static const std::vector<Rule> r = {
        {L"node_modules", L"Node.js", L"npm install", {L"package.json"}},
        {L".next", L"Next.js", L"next build", {L"package.json"}},
        {L".nuxt", L"Nuxt", L"nuxt build", {L"package.json"}},
        {L".svelte-kit", L"SvelteKit", L"vite build", {L"package.json"}},
        {L".angular", L"Angular", L"ng build", {L"angular.json"}},
        {L".parcel-cache", L"Parcel", L"parcel build", {L"package.json"}},
        {L".turbo", L"Turborepo", L"turbo run", {L"package.json", L"turbo.json"}},
        {L"target", L"Rust", L"cargo build", {L"cargo.toml"}},
        {L"target", L"Maven", L"mvn package", {L"pom.xml"}},
        {L"bin", L".NET", L"dotnet build", {L"*.csproj", L"*.fsproj", L"*.vbproj"}},
        {L"obj", L".NET", L"dotnet build", {L"*.csproj", L"*.fsproj", L"*.vbproj"}},
        {L".gradle", L"Gradle", L"gradle build", {L"build.gradle", L"build.gradle.kts", L"settings.gradle", L"settings.gradle.kts"}},
        {L"build", L"Gradle", L"gradle build", {L"build.gradle", L"build.gradle.kts"}},
        {L"build", L"CMake", L"cmake --build", {}, L"cmakecache.txt"},
        {L"cmake-build-*", L"CMake", L"cmake --build", {L"cmakelists.txt"}},
        {L"out", L"CMake", L"cmake --build", {L"cmakepresets.json"}},
        {L"build", L"Flutter", L"flutter build", {L"pubspec.yaml"}},
        {L".dart_tool", L"Dart", L"dart pub get", {L"pubspec.yaml"}},
        {L"library", L"Unity", N_(L"opening the project in Unity"), {L"assets\\+projectsettings\\"}},
        {L"temp", L"Unity", N_(L"opening the project in Unity"), {L"assets\\+projectsettings\\"}},
        {L"obj", L"Unity", N_(L"opening the project in Unity"), {L"assets\\+projectsettings\\"}},
        {L"intermediate", L"Unreal", N_(L"building in Unreal Editor"), {L"*.uproject"}},
        {L"deriveddatacache", L"Unreal", N_(L"opening the project in Unreal Editor"), {L"*.uproject"}},
        {L".venv", L"Python", L"python -m venv + pip install", {}, L"pyvenv.cfg"},
        {L"venv", L"Python", L"python -m venv + pip install", {}, L"pyvenv.cfg"},
        {L".tox", L"Python", L"tox", {L"tox.ini", L"pyproject.toml", L"setup.py"}},
        {L"__pycache__", L"Python", N_(L"running the code"), {}, nullptr, true},
        {L".pytest_cache", L"Python", L"pytest", {}, nullptr, true},
        {L".mypy_cache", L"Python", L"mypy", {}, nullptr, true},
        {L".ruff_cache", L"Python", L"ruff", {}, nullptr, true},
        {L".terraform", L"Terraform", L"terraform init", {L"*.tf"}},
        {L"zig-cache", L"Zig", L"zig build", {L"build.zig"}},
        {L".zig-cache", L"Zig", L"zig build", {L"build.zig"}},
        {L"zig-out", L"Zig", L"zig build", {L"build.zig"}},
        {L"_build", L"Elixir", L"mix compile", {L"mix.exs"}},
        {L"deps", L"Elixir", L"mix deps.get", {L"mix.exs"}},
        {L".stack-work", L"Haskell", L"stack build", {L"stack.yaml"}},
    };
    return r;
}

bool NameIs(const std::wstring& name, const wchar_t* lowerPat) {
    return util::HasWildcards(lowerPat) ? util::GlobMatchI(lowerPat, name) : util::ToLower(name) == lowerPat;
}

const Node* Child(const Node* dir, const std::wstring& lowerPat, bool wantDir) {
    for (auto& c : dir->children)
        if (c->IsDir() == wantDir && NameIs(c->name, lowerPat.c_str())) return c.get();
    return nullptr;
}

// One marker spec: "a+b" (all of them), "name\" for a folder.
bool HasMarker(const Node* dir, const wchar_t* spec) {
    std::wstring s = spec;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(L'+', i);
        if (j == std::wstring::npos) j = s.size();
        std::wstring part = s.substr(i, j - i);
        bool wantDir = !part.empty() && part.back() == L'\\';
        if (wantDir) part.pop_back();
        if (!Child(dir, part, wantDir)) return false;
        i = j + 1;
    }
    return true;
}

const Rule* Match(const Node* n) {
    const Node* parent = n->parent;
    std::wstring lower = util::ToLower(n->name);
    for (const Rule& r : Rules()) {
        if (util::HasWildcards(r.name) ? !util::GlobMatchI(r.name, lower) : lower != r.name) continue;
        if (r.anywhere) return &r;
        if (r.inside && !Child(n, r.inside, false)) continue;
        if (!r.beside.empty() &&
            (!parent || std::none_of(r.beside.begin(), r.beside.end(), [&](const wchar_t* m) { return HasMarker(parent, m); })))
            continue;
        if (r.beside.empty() && !r.inside) continue;
        return &r;
    }
    return nullptr;
}

// Folders that belong to installed programs or Windows, never to your projects.
bool SystemFolder(const std::wstring& name) {
    static const wchar_t* const kNames[] = {L"appdata", L"program files", L"program files (x86)", L"programdata", L"windows",
                                            L"$recycle.bin", L"system volume information", L"$windows.~bt", L"$windows.~ws",
                                            L"windows.old", L"msocache", L"recovery"};
    std::wstring l = util::ToLower(name);
    if (!l.empty() && (l[0] == L'.' || l[0] == L'$')) return true;  // .vscode, .nuget, .cargo, ...
    return std::find(std::begin(kNames), std::end(kNames), l) != std::end(kNames);
}

// The project an "anywhere" cache belongs to: the nearest folder above it that looks like a
// project root (or, failing that, the folder it sits in).
Node* ProjectOf(Node* cacheDir) {
    static const wchar_t* const kRoots[] = {L".git", L"pyproject.toml", L"setup.py", L"setup.cfg", L"requirements.txt",
                                            L"package.json", L"cargo.toml", L"go.mod", L"*.sln", L"pipfile"};
    int depth = 0;
    for (Node* p = cacheDir->parent; p && depth < 12; p = p->parent, ++depth)
        for (const wchar_t* m : kRoots)
            if (Child(p, m, false) || Child(p, m, true)) return p;
    return cacheDir->parent;
}

}  // namespace

std::vector<DevProject> FindDevJunk(Node* root) {
    std::vector<DevProject> out;
    if (!root || !root->CanEnter()) return out;
    // Scanning inside a system or app folder (C:\Users\me\AppData\...): nothing to find there.
    std::wstring rootPath = NodePath(root);
    for (size_t i = 0; i < rootPath.size();) {
        size_t j = rootPath.find(L'\\', i);
        if (j == std::wstring::npos) j = rootPath.size();
        std::wstring seg = rootPath.substr(i, j - i);
        if (!seg.empty() && seg.back() != L':' && seg != L"?" && SystemFolder(seg)) return out;
        i = j + 1;
    }

    std::unordered_map<Node*, size_t> index;  // project folder -> out[]
    std::unordered_set<const Node*> artifactSet;
    std::vector<Node*> stack{root};
    while (!stack.empty()) {
        Node* d = stack.back();
        stack.pop_back();
        for (auto& c : d->children) {
            Node* n = c.get();
            if (!n->CanEnter()) continue;
            if (const Rule* r = Match(n)) {
                Node* proj = r->anywhere ? ProjectOf(n) : n->parent;
                if (!proj) continue;
                auto [it, fresh] = index.try_emplace(proj, out.size());
                if (fresh) {
                    out.emplace_back();
                    out.back().dir = proj;
                }
                out[it->second].artifacts.push_back({n, r->kind, r->how});
                artifactSet.insert(n);
                continue;  // never look inside an artifact (node_modules in node_modules)
            }
            if (SystemFolder(n->name)) continue;
            stack.push_back(n);
        }
    }

    for (DevProject& p : out) {
        std::stable_sort(p.artifacts.begin(), p.artifacts.end(),
                         [](const DevArtifact& a, const DevArtifact& b) { return a.node->size > b.node->size; });
        for (auto& a : p.artifacts)
            if (p.kinds.find(a.kind) == std::wstring::npos) p.kinds += (p.kinds.empty() ? L"" : L", ") + std::wstring(a.kind);
        // Last change: the newest file of the project itself, not its build output or Git data.
        std::vector<const Node*> st{p.dir};
        while (!st.empty()) {
            const Node* d = st.back();
            st.pop_back();
            for (auto& c : d->children) {
                const Node* n = c.get();
                if (n->IsDir()) {
                    if (n->CanEnter() && !artifactSet.count(n) && util::ToLower(n->name) != L".git") st.push_back(n);
                } else if (n->mtime > p.lastChange) {
                    p.lastChange = n->mtime;
                }
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const DevProject& a, const DevProject& b) { return DevProjectSize(a, false) > DevProjectSize(b, false); });
    return out;
}

uint64_t DevProjectSize(const DevProject& p, bool disk) {
    uint64_t n = 0;
    for (auto& a : p.artifacts) n += disk ? a.node->disk : a.node->size;
    return n;
}
