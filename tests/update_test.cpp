// Self-check of the self-update: bytopsy-update-test.exe
// Signs manifests with a throwaway ECDSA P-256 key made here (never the project's key), checks
// versions, hashes and address pinning, and swaps fake program files in a %TEMP% fixture. Never
// goes online: every download is a file:// URL, and the one real-looking URL check is refused
// before any connection. Never touches the build's own exe.
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <string>
#include <vector>

#include "journal.h"
#include "runtool.h"
#include "update.h"
#include "updatekey.h"
#include "util.h"
#include "version.h"

static int g_fail = 0;

static void CheckResult(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}
// The condition first, so that a message can show the error it left behind.
#define Check(cond, what)         \
    do {                          \
        bool ok_ = (cond);        \
        CheckResult(ok_, (what)); \
    } while (0)

static void Put(const std::wstring& path, const std::string& text) {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

static std::string Get(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return "<missing>";
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(dir + L"\\" + n);
            else
                DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::wstring FileUrl(const std::wstring& path) {
    std::wstring u = L"file:///";
    for (wchar_t c : path) u += c == L'\\' ? L'/' : c;
    return u;
}

// ---- A throwaway signing key (bcrypt.dll), like the one tools/update-key.ps1 makes.
struct TestKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    uint8_t pub[64] = {};
    bool Make() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
        if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(alg, &key, 256, 0)) || !BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) return false;
        uint8_t blob[256];
        ULONG n = 0;
        if (!BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &n, 0)) || n != sizeof(BCRYPT_ECCKEY_BLOB) + 64)
            return false;
        memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
        return true;
    }
    std::vector<uint8_t> Sign(const std::string& data) {
        std::string hex = Sha256Hex(data.data(), data.size());
        uint8_t digest[32];
        for (int i = 0; i < 32; ++i) digest[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
        std::vector<uint8_t> sig(64);
        ULONG n = 0;
        BCryptSignHash(key, nullptr, digest, 32, sig.data(), 64, &n, 0);
        sig.resize(n);
        return sig;
    }
    ~TestKey() {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

static std::string Outer(const std::string& inner, const std::vector<uint8_t>& sig, const char* format = "1") {
    return std::string("{\"format\":") + format + ",\"manifest\":\"" + Base64Encode(reinterpret_cast<const uint8_t*>(inner.data()), inner.size()) +
           "\",\"signature\":\"" + Base64Encode(sig.data(), sig.size()) + "\"}";
}

static std::string Inner(const std::string& version, const std::string& url, const std::string& sha, uint64_t size,
                         const std::string& minimum = "", const std::string& name = "") {
    std::string n = name.empty() ? "bytopsy-" + version + "-x64.zip" : name;
    return "{\"version\":\"" + version + "\",\"released\":\"2026-10-18\",\"notes_url\":\"https://github.com/N604K/Bytopsy/releases/tag/v" + version +
           "\"" + (minimum.empty() ? "" : ",\"minimum_version\":\"" + minimum + "\"") + ",\"assets\":[{\"name\":\"" + n + "\",\"url\":\"" + url +
           "\",\"sha256\":\"" + sha + "\",\"size\":" + std::to_string(size) + "}]}";
}

static std::string Narrow(const std::wstring& w) { return util::WideToUtf8(w); }

static UpdateCheck EvaluateInner(const std::string& inner, bool local) {
    UpdateManifest m;
    std::wstring err;
    if (!ParseManifestJson(inner, m, err)) {
        UpdateCheck c;
        c.error = err;
        return c;
    }
    return EvaluateManifest(m, BYTOPSY_VERSION, local);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH];
    std::wstring base = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-update-test";
    RemoveTree(base);
    CreateDirectoryW(base.c_str(), nullptr);
    SetEnvironmentVariableW(L"BYTOPSY_JOURNAL", (base + L"\\journal.log").c_str());
    const std::string kSha(64, 'a');
    const std::string kGitHub = "https://github.com/N604K/Bytopsy/releases/download/v99.0.0/bytopsy-99.0.0-x64.zip";

    // ---- Versions
    Check(CompareVersions(L"1.0.0", L"1.0.0") == 0, L"1.0.0 = 1.0.0");
    Check(CompareVersions(L"1.0", L"1.0.0") == 0, L"1.0 = 1.0.0");
    Check(CompareVersions(L"1.0.1", L"1.0.0") > 0, L"1.0.1 > 1.0.0");
    Check(CompareVersions(L"1.10.0", L"1.9.9") > 0, L"1.10.0 > 1.9.9 (numbers, not text)");
    Check(CompareVersions(L"2.0.0", L"1.99.99") > 0, L"2.0.0 > 1.99.99");
    Check(CompareVersions(L"1.2.0-beta.1", L"1.2.0") < 0, L"a pre-release comes before its release");
    Check(CompareVersions(L"1.2.0-beta.2", L"1.2.0-beta.10") < 0, L"beta.2 < beta.10");
    Check(CompareVersions(L"1.2.0-beta", L"1.1.9") > 0, L"1.2.0-beta > 1.1.9");
    Check(CompareVersions(L"v1.2.0", L"1.2.0") == 0, L"a leading v is ignored");
    Check(IsValidVersion(L"1.2.3") && IsValidVersion(L"1.2.3-rc.1") && IsValidVersion(L"10"), L"valid versions");
    Check(!IsValidVersion(L"") && !IsValidVersion(L"1..2") && !IsValidVersion(L"1.2.3.4.5") && !IsValidVersion(L"1.2 ") &&
              !IsValidVersion(L"abc") && !IsValidVersion(L"1.2-") && !IsValidVersion(L"1.2-a/b"),
          L"invalid versions are refused");

    // ---- Encoding and hashes
    std::vector<uint8_t> bytes;
    Check(Base64Decode("YWJj", bytes) && std::string(bytes.begin(), bytes.end()) == "abc", L"base64 decode");
    Check(Base64Decode("YQ==", bytes) && bytes.size() == 1 && bytes[0] == 'a', L"base64 with padding");
    Check(!Base64Decode("YW$j", bytes) && !Base64Decode("YQ==YQ==", bytes), L"bad base64 is refused");
    {
        std::string s = "any\x01\xFF bytes";
        std::vector<uint8_t> back;
        Check(Base64Decode(Base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size()), back) && std::string(back.begin(), back.end()) == s,
              L"base64 round trip");
    }
    Check(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256 of abc");
    Check(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", L"SHA-256 of nothing");
    Put(base + L"\\abc.txt", "abc");
    std::string fileHash;
    Check(Sha256File(base + L"\\abc.txt", fileHash) && fileHash == Sha256Hex("abc", 3), L"SHA-256 of a file");
    Check(IsPlaceholderKey(kUpdatePublicKey, sizeof(kUpdatePublicKey)) || !IsPlaceholderKey(kUpdatePublicKey, sizeof(kUpdatePublicKey)),
          L"updatekey.h holds 64 bytes");

    // ---- Signed manifests
    TestKey key, other;
    Check(key.Make() && other.Make(), L"throwaway P-256 keys made");
    std::string inner = Inner("99.0.0", kGitHub, kSha, 1234, "98.0.0");
    std::vector<uint8_t> sig = key.Sign(inner);
    Check(sig.size() == 64, L"signature is r||s, 64 bytes");
    UpdateManifest m;
    std::wstring err;
    Check(ParseSignedManifest(Outer(inner, sig), key.pub, 64, m, err), L"valid signature accepted: " + err);
    Check(m.version == L"99.0.0" && m.released == L"2026-10-18" && m.minimumVersion == L"98.0.0" && m.assets.size() == 1 &&
              m.assets[0].url == util::Utf8ToWide(kGitHub) && m.assets[0].sha256 == kSha && m.assets[0].size == 1234 &&
              m.notesUrl == L"https://github.com/N604K/Bytopsy/releases/tag/v99.0.0",
          L"manifest fields read");
    {
        std::string tampered = inner;
        tampered[tampered.find("99.0.0")] = '8';  // 89.0.0, same signature
        Check(!ParseSignedManifest(Outer(tampered, sig), key.pub, 64, m, err), L"a changed byte in the manifest is refused: " + err);
        std::vector<uint8_t> badSig = sig;
        badSig[10] ^= 1;
        Check(!ParseSignedManifest(Outer(inner, badSig), key.pub, 64, m, err), L"a changed byte in the signature is refused");
        Check(!ParseSignedManifest(Outer(inner, sig), other.pub, 64, m, err), L"a signature by another key is refused");
        uint8_t zeros[64] = {};
        Check(!ParseSignedManifest(Outer(inner, sig), zeros, 64, m, err), L"the placeholder key accepts nothing");
        Check(!ParseSignedManifest(Outer(inner, sig, "2"), key.pub, 64, m, err), L"another format is refused");
        Check(!ParseSignedManifest("{\"format\":1,\"manifest\":\"" + Base64Encode(reinterpret_cast<const uint8_t*>(inner.data()), inner.size()) + "\"}",
                                   key.pub, 64, m, err),
              L"no signature: refused");
        Check(!ParseSignedManifest("{\"format\":1,\"manifest\":\"@@@\",\"signature\":\"" + Base64Encode(sig.data(), sig.size()) + "\"}", key.pub, 64, m, err),
              L"bad base64: refused");
        std::string dup = Outer(inner, sig);
        dup.insert(dup.size() - 1, ",\"format\":1");
        Check(!ParseSignedManifest(dup, key.pub, 64, m, err), L"a key twice: refused");
        Check(!ParseSignedManifest(Outer(inner, sig) + std::string(kMaxManifestBytes, ' '), key.pub, 64, m, err), L"larger than 64 KB: refused");
        Check(!ParseSignedManifest("not json", key.pub, 64, m, err), L"not JSON: refused");
        std::string badInner = "{\"version\":\"99.0.0\"";
        Check(!ParseSignedManifest(Outer(badInner, key.Sign(badInner)), key.pub, 64, m, err), L"a signed but broken manifest: refused");
        std::string unicode = Inner("99.0.0", kGitHub, kSha, 1);
        unicode.insert(1, "\"x\":\"\\u00e9\\ud83d\\ude00\",");
        Check(ParseSignedManifest(Outer(unicode, key.Sign(unicode)), key.pub, 64, m, err), L"\\u escapes and surrogate pairs read");
    }

    // ---- What a verified manifest means for the running version
    {
        UpdateCheck c = EvaluateInner(inner, false);
        Check(c.status == UpdateStatus::Available && c.asset.size == 1234 && c.required, L"newer version: available, required below minimum_version");
        c = EvaluateInner(Inner("99.0.0", kGitHub, kSha, 1234, "0.1"), false);
        Check(c.status == UpdateStatus::Available && !c.required, L"above minimum_version: not required");
        c = EvaluateInner(Inner(Narrow(BYTOPSY_VERSION), kGitHub, kSha, 1234), false);
        Check(c.status == UpdateStatus::UpToDate, L"same version: up to date");
        c = EvaluateInner(Inner("0.9.0", kGitHub, kSha, 1234), false);
        Check(c.status == UpdateStatus::UpToDate, L"older version: up to date (never a downgrade)");
        for (const char* url : {"https://evil.example.com/bytopsy-99.0.0-x64.zip", "http://github.com/N604K/Bytopsy/releases/download/v99.0.0/bytopsy-99.0.0-x64.zip",
                                "https://github.com/N604K/Bytopsy/releases/download/v99.0.0/../x-x64.zip",
                                "https://github.com/N604K/Bytopsy/releases/download/../evil/bytopsy-x64.zip",
                                "https://github.com/N604K/Bytopsyx/releases/download/v99.0.0/bytopsy-99.0.0-x64.zip",
                                "https://github.com/someone/Bytopsy/releases/download/v99.0.0/bytopsy-99.0.0-x64.zip",
                                "https://github.com/N604K/Bytopsy/releases/download/v99.0.0/bytopsy-99.0.0-x64.zip?x=1",
                                "https://github.com/N604K/Bytopsy/releases/download/v99.0.0/a/bytopsy-99.0.0-x64.zip",
                                "https://github.com.evil.com/N604K/Bytopsy/releases/download/v99.0.0/bytopsy-x64.zip",
                                "file:///C:/bytopsy-99.0.0-x64.zip"}) {
            c = EvaluateInner(Inner("99.0.0", url, kSha, 1234), false);
            Check(c.status == UpdateStatus::Failed, L"foreign download refused: " + util::Utf8ToWide(url));
        }
        c = EvaluateInner(Inner("99.0.0", kGitHub, kSha, 1234), true);
        Check(c.status == UpdateStatus::Failed, L"the local test mode never downloads from GitHub");
        c = EvaluateInner(Inner("99.0.0", kGitHub, kSha, 1234, "", "bytopsy-99.0.0-arm64.zip"), false);
        Check(c.status == UpdateStatus::Failed, L"no x64 zip: refused");
        c = EvaluateInner(Inner("99.0.0", kGitHub, kSha, 0), false);
        Check(c.status == UpdateStatus::Failed, L"size 0: refused");
        c = EvaluateInner(Inner("99.0.0", kGitHub, kSha, kMaxZipBytes + 1), false);
        Check(c.status == UpdateStatus::Failed, L"larger than 200 MB: refused");
        c = EvaluateInner(Inner("99.0.0", kGitHub, "abc", 1234), false);
        Check(c.status == UpdateStatus::Failed, L"a SHA-256 that isn't 64 hex digits: refused");
        Check(IsReleaseAssetUrl(L"https://github.com/N604K/Bytopsy/releases/download/v1.2.0/bytopsy-1.2.0-x64.zip"), L"release download accepted");
    }

    // ---- Local test URLs and where the app looks
    Check(IsLocalTestUrl(L"file:///C:/x/update.json") && IsLocalTestUrl(L"http://localhost:8080/update.json") &&
              IsLocalTestUrl(L"http://127.0.0.1/update.json"),
          L"local test URLs");
    Check(!IsLocalTestUrl(L"http://localhost.evil.com/u.json") && !IsLocalTestUrl(L"http://localhostx/u") && !IsLocalTestUrl(L"https://bytopsy.com/update.json") &&
              !IsLocalTestUrl(L"http://example.com/") && !IsLocalTestUrl(L"file:///"),
          L"anything else is not local");
    {
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_URL", nullptr);
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_TEST_KEY", nullptr);
        UpdateSources s = DefaultUpdateSources();
        Check(!s.local && s.urls.size() == 2 && s.urls[0] == kUpdateUrl && s.urls[1] == kUpdateFallbackUrl && s.key.size() == 64 &&
                  memcmp(s.key.data(), kUpdatePublicKey, 64) == 0,
              L"normally: bytopsy.com, then GitHub's latest release, with the built-in key");
        std::wstring testKey = util::Utf8ToWide(Base64Encode(key.pub, 64));
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_URL", L"https://evil.example.com/update.json");
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_TEST_KEY", testKey.c_str());
        s = DefaultUpdateSources();
        Check(!s.local && s.urls.size() == 2 && memcmp(s.key.data(), kUpdatePublicKey, 64) == 0, L"a non-local BYTOPSY_UPDATE_URL and its key are ignored");
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_URL", L"file:///C:/fixture/update.json");
        s = DefaultUpdateSources();
        Check(s.local && s.urls.size() == 1 && s.key.size() == 64 && memcmp(s.key.data(), key.pub, 64) == 0, L"a local BYTOPSY_UPDATE_URL takes the test key");
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_URL", nullptr);
        SetEnvironmentVariableW(L"BYTOPSY_UPDATE_TEST_KEY", nullptr);
        UpdateSources none;
        none.urls = {L"https://bytopsy.com/update.json"};
        none.key.assign(64, 0);
        Check(CheckForUpdate(none, BYTOPSY_VERSION).status == UpdateStatus::NoKey, L"the placeholder key: no check at all (nothing contacted)");
        std::string body;
        Check(!HttpGetString(L"http://example.com/update.json", 1000, false, body, err), L"plain HTTP is refused before connecting: " + err);
        Check(!HttpGetString(L"ftp://example.com/update.json", 1000, true, body, err), L"other schemes are refused");
    }

    // ---- Package managers
    Check(PackageManagerFor(L"C:\\Users\\x\\scoop\\apps\\bytopsy\\current\\bytopsy.exe") == PackageManager::Scoop, L"Scoop");
    Check(PackageManagerFor(L"C:\\ProgramData\\scoop\\apps\\bytopsy\\1.0.0\\bytopsy.exe") == PackageManager::Scoop, L"Scoop (global)");
    Check(PackageManagerFor(L"D:\\pkgs\\apps\\bytopsy\\current\\bytopsy.exe") == PackageManager::Scoop, L"Scoop in another folder");
    Check(PackageManagerFor(L"C:\\Users\\x\\AppData\\Local\\Microsoft\\WinGet\\Packages\\N604K.Bytopsy_Microsoft.Winget.Source_8wekyb3d8bbwe\\bytopsy.exe") ==
              PackageManager::Winget,
          L"winget package folder");
    Check(PackageManagerFor(L"C:\\Users\\x\\AppData\\Local\\Microsoft\\WinGet\\Links\\bytopsy.exe") == PackageManager::Winget, L"winget link");
    Check(PackageManagerFor(L"C:\\ProgramData\\chocolatey\\lib\\bytopsy\\tools\\bytopsy.exe") == PackageManager::Chocolatey, L"Chocolatey");
    Check(PackageManagerFor(L"D:\\Tools\\Bytopsy\\bytopsy.exe") == PackageManager::None && PackageManagerFor(L"D:\\scoopy\\apps\\x\\bytopsy.exe") == PackageManager::None,
          L"anywhere else: no package manager");
    Check(std::wstring(PackageManagerCommand(PackageManager::Winget)) == L"winget upgrade N604K.Bytopsy" &&
              std::wstring(PackageManagerCommand(PackageManager::Scoop)) == L"scoop update bytopsy" &&
              std::wstring(PackageManagerCommand(PackageManager::Chocolatey)) == L"choco upgrade bytopsy",
          L"package manager commands");

    // ---- Publisher check
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring subject;
        Check(!AuthenticodeSigner(self, subject), L"this unsigned test program has no signer");
        Check(CheckSamePublisher(self, {base + L"\\abc.txt"}, err), L"unsigned running exe: the manifest vouches for the files");
        wchar_t pf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH);
        std::wstring signedExe = std::wstring(pf, n) + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        if (n && Exists(signedExe)) {
            Check(AuthenticodeSigner(signedExe, subject) && subject.find(L"Microsoft") != std::wstring::npos, L"a signed exe's signer: " + subject);
            Check(!CheckSamePublisher(signedExe, {self}, err), L"signed running exe, unsigned new exe: refused (" + err + L")");
            Check(CheckSamePublisher(signedExe, {signedExe}, err), L"same publisher: accepted");
        } else {
            fputs("skip signed-exe checks (vswhere.exe not found)\n", stdout);
        }
    }

    // ---- Swapping files in a fixture folder
    std::wstring app = base + L"\\app", rel = base + L"\\release";
    auto makeApp = [&] {
        RemoveTree(app);
        Put(app + L"\\bytopsy.exe", "OLD-GUI");
        Put(app + L"\\bytopsy-cli.exe", "OLD-CLI");
        Put(app + L"\\bytopsy.portable", "");
        Put(app + L"\\Bytopsy-data\\settings.ini", "theme=2\n");
        Put(app + L"\\lang\\de.lang", "@name = Deutsch\n");
        Put(app + L"\\notes.txt", "mine");
    };
    RemoveTree(rel);
    Put(rel + L"\\bytopsy.exe", "NEW-GUI");
    Put(rel + L"\\bytopsy-cli.exe", "NEW-CLI");
    Put(rel + L"\\README.md", "new readme");
    Put(rel + L"\\bytopsy.portable", "from the zip");
    Put(rel + L"\\lang\\x.lang", "@name = X\n");
    Check(FolderWritable(base), L"the fixture folder is writable");
    Check(!FolderWritable(base + L"\\does-not-exist"), L"a missing folder is not writable");
    if (!RunningElevated()) {
        wchar_t sys[MAX_PATH];
        UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
        Check(!FolderWritable(std::wstring(sys, sn)), L"System32 is not writable without administrator rights");
    }
    std::wstring folder;
    std::vector<std::wstring> names;
    Check(FindReleaseFiles(rel, folder, names, err) && folder == rel && names.size() == 3 && names[0].find(L".exe") != std::wstring::npos,
          L"release files: the programs first, no portable marker, no subfolders");
    {
        // A zip with one folder at the top.
        Put(base + L"\\nested\\bytopsy-99.0.0-x64\\bytopsy.exe", "X");
        std::vector<std::wstring> nn;
        Check(FindReleaseFiles(base + L"\\nested", folder, nn, err) && folder == base + L"\\nested\\bytopsy-99.0.0-x64" && nn.size() == 1,
              L"release files inside one top folder");
        Check(!FindReleaseFiles(base + L"\\app-missing", folder, nn, err), L"no bytopsy.exe: refused");
    }

    makeApp();
    {
        // The running program: it can be renamed (FILE_SHARE_DELETE), not overwritten.
        HANDLE running = CreateFileW((app + L"\\bytopsy.exe").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(running != INVALID_HANDLE_VALUE, L"fixture exe held open like a running program");
        // bytopsy-cli.exe held open without delete sharing: it can't be renamed, so all is put back.
        HANDLE locked = CreateFileW((app + L"\\bytopsy-cli.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(!InstallFiles(app, rel, {L"bytopsy.exe", L"bytopsy-cli.exe", L"README.md"}, err), L"a file that can't be replaced stops the swap: " + err);
        CloseHandle(locked);
        Check(Get(app + L"\\bytopsy.exe") == "OLD-GUI" && Get(app + L"\\bytopsy-cli.exe") == "OLD-CLI" && !Exists(app + L"\\bytopsy.exe.old") &&
                  !Exists(app + L"\\bytopsy.exe.new") && !Exists(app + L"\\bytopsy-cli.exe.new") && !Exists(app + L"\\README.md"),
              L"... and everything was put back");
        // A .old from the last update that is still in use: nothing is changed.
        Put(app + L"\\bytopsy-cli.exe.old", "OLDER");
        HANDLE oldInUse = CreateFileW((app + L"\\bytopsy-cli.exe.old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(!InstallFiles(app, rel, {L"bytopsy.exe", L"bytopsy-cli.exe"}, err) && err.find(L"in use") != std::wstring::npos,
              L"a .old still in use: refused before anything changes");
        CloseHandle(oldInUse);
        Check(Get(app + L"\\bytopsy.exe") == "OLD-GUI" && !Exists(app + L"\\bytopsy.exe.new"), L"... nothing changed");
        Check(InstallFiles(app, rel, {L"bytopsy.exe", L"bytopsy-cli.exe", L"README.md"}, err), L"swap with the program running: " + err);
        CloseHandle(running);
        Check(Get(app + L"\\bytopsy.exe") == "NEW-GUI" && Get(app + L"\\bytopsy-cli.exe") == "NEW-CLI" && Get(app + L"\\README.md") == "new readme",
              L"new files in place");
        Check(Get(app + L"\\bytopsy.exe.old") == "OLD-GUI" && Get(app + L"\\bytopsy-cli.exe.old") == "OLD-CLI", L"old files kept as .old");
        Check(Get(app + L"\\bytopsy.portable") == "" && Get(app + L"\\Bytopsy-data\\settings.ini") == "theme=2\n" &&
                  Get(app + L"\\lang\\de.lang") == "@name = Deutsch\n" && Get(app + L"\\notes.txt") == "mine",
              L"portable marker, Bytopsy-data, lang and other files untouched");
        // Cleanup on the next start; a .old still in use stays.
        Put(app + L"\\mine.txt.old", "not ours");
        HANDLE busy = CreateFileW((app + L"\\bytopsy-cli.exe.old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(CleanupOldFiles(app) == 1 && !Exists(app + L"\\bytopsy.exe.old"), L"cleanup: .old files deleted, one in use stays");
        CloseHandle(busy);
        Check(CleanupOldFiles(app) == 0 && !Exists(app + L"\\bytopsy-cli.exe.old") && Exists(app + L"\\mine.txt.old"),
              L"cleanup: the rest goes; other .old files are not ours");
    }

    // ---- The whole update from a local, signed manifest (file:// only)
    {
        std::wstring zip = base + L"\\bytopsy-99.0.0-x64.zip";
        std::string out;
        int code = RunTool(SystemTool(L"tar.exe"), L"--format zip -c -f \"" + zip + L"\" -C \"" + rel + L"\" bytopsy.exe bytopsy-cli.exe README.md bytopsy.portable lang",
                           out, 60000);
        Check(code == 0 && Exists(zip), L"fixture zip made with tar.exe");
        std::string zipSha;
        Sha256File(zip, zipSha);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &fa);
        uint64_t zipSize = fa.nFileSizeLow;
        std::wstring extracted = base + L"\\extracted";
        Check(ExtractZip(zip, extracted, err) && Get(extracted + L"\\bytopsy.exe") == "NEW-GUI", L"zip unpacked with System32\\tar.exe: " + err);
        Put(base + L"\\broken.zip", "PK this is not a zip");
        Check(!ExtractZip(base + L"\\broken.zip", base + L"\\broken", err), L"a broken zip: refused (" + err + L")");

        std::string zipUrl = Narrow(FileUrl(zip));
        std::string manifest = Inner("99.0.0", zipUrl, zipSha, zipSize);
        Put(base + L"\\update.json", Outer(manifest, key.Sign(manifest)));
        UpdateSources src;
        src.urls = {FileUrl(base + L"\\update.json")};
        src.key.assign(key.pub, key.pub + 64);
        src.local = true;
        UpdateCheck c = CheckForUpdate(src, BYTOPSY_VERSION);
        Check(c.status == UpdateStatus::Available && c.asset.sha256 == zipSha, L"check against the local manifest: " + c.error);

        // A download that doesn't match the signed hash changes nothing.
        makeApp();
        UpdateCheck wrong = c;
        wrong.asset.sha256 = kSha;
        Check(!InstallUpdate(wrong, app + L"\\bytopsy.exe", nullptr, nullptr, err) && Get(app + L"\\bytopsy.exe") == "OLD-GUI",
              L"wrong SHA-256: nothing installed (" + err + L")");
        UpdateCheck shorter = c;
        shorter.asset.size = zipSize - 1;
        Check(!InstallUpdate(shorter, app + L"\\bytopsy.exe", nullptr, nullptr, err) && Get(app + L"\\bytopsy.exe") == "OLD-GUI",
              L"wrong size: nothing installed (" + err + L")");
        Check(!InstallUpdate(c, L"C:\\Users\\x\\scoop\\apps\\bytopsy\\current\\bytopsy.exe", nullptr, nullptr, err) && err.find(L"scoop update") != std::wstring::npos,
              L"a package manager's copy is left to the package manager");
        UpdateCheck foreign = c;
        foreign.local = false;
        Check(!InstallUpdate(foreign, app + L"\\bytopsy.exe", nullptr, nullptr, err), L"a file:// download outside the test mode: refused");

        UpdateProgress progress;
        HANDLE running = CreateFileW((app + L"\\bytopsy.exe").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(InstallUpdate(c, app + L"\\bytopsy.exe", &progress, nullptr, err), L"update installed: " + err);
        CloseHandle(running);
        Check(progress.step == static_cast<int>(UpdateStep::Done) && progress.done == zipSize, L"progress reported");
        Check(Get(app + L"\\bytopsy.exe") == "NEW-GUI" && Get(app + L"\\bytopsy-cli.exe") == "NEW-CLI" && Get(app + L"\\README.md") == "new readme" &&
                  Get(app + L"\\bytopsy.exe.old") == "OLD-GUI",
              L"new programs in place, the old ones as .old");
        Check(Get(app + L"\\bytopsy.portable") == "" && Get(app + L"\\Bytopsy-data\\settings.ini") == "theme=2\n" && !Exists(app + L"\\lang\\x.lang") &&
                  Get(app + L"\\lang\\de.lang") == "@name = Deutsch\n",
              L"the zip's portable marker and folders are not installed; the user's are kept");
        bool leftover = false;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"bytopsy-update-" + std::to_wstring(GetCurrentProcessId()) + L"-*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            leftover = true;
            FindClose(h);
        }
        Check(!leftover, L"the download folder in %TEMP% is removed");
        auto journal = JournalRead(10);
        Check(!journal.empty() && journal[0].path.find(L"Bytopsy update: ") == 0 && journal[0].path.find(L"99.0.0") != std::wstring::npos,
              L"journaled: " + (journal.empty() ? std::wstring(L"nothing") : journal[0].path + L" (" + journal[0].note + L")"));

        // Through a downloaded file, with a size limit.
        uint64_t seen = 0;
        Check(HttpGetFile(FileUrl(zip), base + L"\\copy.zip", zipSize, true, err, nullptr, [&](uint64_t d, uint64_t) { seen = d; }) && seen == zipSize,
              L"file download with progress");
        Check(!HttpGetFile(FileUrl(zip), base + L"\\copy2.zip", zipSize - 1, true, err) && !Exists(base + L"\\copy2.zip"),
              L"larger than allowed: refused, partial file removed");
    }

    RemoveTree(base);
    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
