#pragma once

// Bytopsy updates itself (opt-in). The release workflow attaches update.json to every GitHub
// release: {"format":1,"manifest":"<base64 of the inner JSON>","signature":"<base64 of r||s>"},
// an ECDSA P-256 signature over SHA-256 of the inner JSON's exact bytes, made with a key kept in a
// GitHub secret (tools/update-key.ps1). The app holds the public key (updatekey.h) and checks the
// signature with bcrypt.dll before it believes anything inside. The inner JSON names the version,
// its release date and notes, the minimum version still advised, and the x64 zip (URL, SHA-256,
// size). The zip must come from https://github.com/N604K/Bytopsy/releases/download/; it is checked
// against the signed hash (and, once releases are code-signed, against the running exe's
// publisher) before anything next to the exe changes.
//
// Nothing here runs by itself: the windowed app checks once a day only when the user turned it
// on, or when asked (Check now, bytopsy-cli --check-update).

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// ---- Versions

// "1.2.3", "1.2.3-beta.1": numbers compared part by part ("1.2" = "1.2.0"); a pre-release
// suffix sorts before the release itself, suffixes compare as text. <0, 0, >0 like strcmp.
int CompareVersions(std::wstring_view a, std::wstring_view b);
// Digits and dots, at most four numbers, optionally "-" and [A-Za-z0-9.] after them.
bool IsValidVersion(std::wstring_view v);

// ---- Encoding and hashes

bool Base64Decode(std::string_view in, std::vector<uint8_t>& out);
std::string Base64Encode(const uint8_t* data, size_t n);
std::string Sha256Hex(const void* data, size_t n);  // lowercase
bool Sha256File(const std::wstring& path, std::string& hex);

// ECDSA P-256 over SHA-256(data): sig = r||s (64 bytes, IEEE P1363), key = X||Y (64 bytes).
bool VerifyEcdsaP256(const uint8_t* data, size_t n, const uint8_t* sig, size_t sigLen, const uint8_t* key, size_t keyLen);
// All zeros: the placeholder in updatekey.h until the project's key is generated.
bool IsPlaceholderKey(const uint8_t* key, size_t keyLen);

// ---- The manifest

struct UpdateAsset {
    std::wstring name;   // bytopsy-1.2.0-x64.zip
    std::wstring url;
    std::string sha256;  // 64 lowercase hex digits
    uint64_t size = 0;
};

struct UpdateManifest {
    std::wstring version;
    std::wstring released;        // "2026-10-18"
    std::wstring notesUrl;        // the release page
    std::wstring minimumVersion;  // optional: older versions are urged to update
    std::vector<UpdateAsset> assets;
};

constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr uint64_t kMaxZipBytes = 200ull * 1024 * 1024;
// Where release zips may come from (and nowhere else, outside the local test mode).
inline constexpr wchar_t kReleaseDownloadPrefix[] = L"https://github.com/N604K/Bytopsy/releases/download/";

// Reads update.json: the signature is checked with key (X||Y) before the inner JSON is parsed.
bool ParseSignedManifest(std::string_view file, const uint8_t* key, size_t keyLen, UpdateManifest& m, std::wstring& err);
// The inner JSON alone (after the signature was checked).
bool ParseManifestJson(std::string_view json, UpdateManifest& m, std::wstring& err);
// https://github.com/N604K/Bytopsy/releases/download/<tag>/<file> with nothing odd in the rest.
bool IsReleaseAssetUrl(std::wstring_view url);
// The local test mode's URLs: file:///..., http://localhost[:port]/..., http://127.0.0.1[:port]/...
bool IsLocalTestUrl(std::wstring_view url);

enum class UpdateStatus { UpToDate, Available, NoKey, Failed };

struct UpdateCheck {
    UpdateStatus status = UpdateStatus::Failed;
    UpdateManifest manifest;
    UpdateAsset asset;      // the x64 zip
    bool required = false;  // the running version is older than minimum_version
    bool local = false;     // from the local test mode
    std::wstring source;    // the address that answered
    std::wstring error;
};

// A verified manifest against the running version: same or older -> UpToDate (never a downgrade);
// no usable x64 zip, or one from anywhere but the release downloads -> Failed.
UpdateCheck EvaluateManifest(const UpdateManifest& m, std::wstring_view running, bool local);

// Where to look and with which key. Normally https://bytopsy.com/update.json, then the GitHub
// latest release's update.json, with the key from updatekey.h. For tests only, BYTOPSY_UPDATE_URL
// set to a local URL (IsLocalTestUrl) replaces both; then, and only then, BYTOPSY_UPDATE_TEST_KEY
// (base64 of X||Y) replaces the key, and assets must be local too. Any other BYTOPSY_UPDATE_URL is
// ignored.
struct UpdateSources {
    std::vector<std::wstring> urls;
    std::vector<uint8_t> key;
    bool local = false;
};
inline constexpr wchar_t kUpdateUrl[] = L"https://bytopsy.com/update.json";
inline constexpr wchar_t kUpdateFallbackUrl[] = L"https://github.com/N604K/Bytopsy/releases/latest/download/update.json";
UpdateSources DefaultUpdateSources();
// Tries each URL in turn until one gives a manifest with a valid signature.
UpdateCheck CheckForUpdate(const UpdateSources& src, std::wstring_view running, std::atomic<bool>* cancel = nullptr);

// ---- Network (WinHTTP; HTTPS only, except local test URLs when allowLocal)

using HttpProgress = std::function<void(uint64_t done, uint64_t total)>;
bool HttpGetString(const std::wstring& url, size_t maxBytes, bool allowLocal, std::string& out, std::wstring& err,
                   std::atomic<bool>* cancel = nullptr);
bool HttpGetFile(const std::wstring& url, const std::wstring& path, uint64_t maxBytes, bool allowLocal, std::wstring& err,
                 std::atomic<bool>* cancel = nullptr, const HttpProgress& progress = nullptr);

// ---- Package managers: installed by one of them, it updates Bytopsy, not Bytopsy itself.

enum class PackageManager { None, Winget, Scoop, Chocolatey };
// From where the exe lives: ...\scoop\apps\..., ...\WinGet\Packages\... or ...\WinGet\Links\...,
// ...\chocolatey\lib\...
PackageManager PackageManagerFor(std::wstring_view exePath);
const wchar_t* PackageManagerName(PackageManager pm);     // "winget"
const wchar_t* PackageManagerCommand(PackageManager pm);  // "winget upgrade N604K.Bytopsy"

// ---- Installing

// Steps of InstallUpdate, for progress.
enum class UpdateStep { Download, Verify, Unpack, Install, Done };

struct UpdateProgress {
    std::atomic<int> step{0};  // UpdateStep
    std::atomic<uint64_t> done{0}, total{0};
};

// Unpacks a zip with Windows' own tar.exe (System32, Windows 10 1803 and later).
bool ExtractZip(const std::wstring& zip, const std::wstring& dir, std::wstring& err);
// The unpacked release: the folder holding bytopsy.exe (dir itself, or its one subfolder) and its
// plain files (no links, no portable marker). Subfolders are left out.
bool FindReleaseFiles(const std::wstring& dir, std::wstring& folder, std::vector<std::wstring>& names, std::wstring& err);
// The subject of the certificate a file is validly Authenticode-signed with (embedded signature;
// no revocation check, nothing fetched). False for unsigned or invalid files.
bool AuthenticodeSigner(const std::wstring& file, std::wstring& subject);
// When the running exe is signed, every new exe must be signed by the same subject. When it isn't,
// the manifest's signature and hash are what vouch for the files.
bool CheckSamePublisher(const std::wstring& runningExe, const std::vector<std::wstring>& newExes, std::wstring& err);
// Whether files can be created in dir (not, for example, in Program Files without administrator rights).
bool FolderWritable(const std::wstring& dir);
// Puts the new files (names, in newDir) into exeDir. Each is copied in as <name>.new first; then
// the existing file is renamed to <name>.old (a running exe can be renamed, not overwritten) and
// the new one takes its name. On any failure everything is put back as it was. Other files and
// folders in exeDir (Bytopsy-data, lang, bytopsy.portable) are never touched.
bool InstallFiles(const std::wstring& exeDir, const std::wstring& newDir, const std::vector<std::wstring>& names, std::wstring& err);
// The release's file names (bytopsy.exe, bytopsy-cli.exe, README.md, LICENSE): <name>.old left in
// exeDir by an update are deleted. Returns how many are still there (in use).
int CleanupOldFiles(const std::wstring& exeDir);
// Everything: download into a new folder in %TEMP%, check size and SHA-256, unpack, check the
// files and their publisher, install them next to exePath, journal it. The temp folder is removed.
bool InstallUpdate(const UpdateCheck& c, const std::wstring& exePath, UpdateProgress* progress, std::atomic<bool>* cancel,
                   std::wstring& err);
