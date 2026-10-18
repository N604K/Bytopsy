// Self-check of the WSL and Docker virtual disk code: winncdu-vdisks-test.exe
// Reads ext4 usage from small hand-built VHDX files in %TEMP% and parses sample output of df,
// docker and wsl.exe. Never runs WSL, Docker or diskpart.
// Set WINNCDU_VHDX to a real .vhdx to print what is read from it (read-only).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "runtool.h"
#include "util.h"
#include "vdisks.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

template <class T>
static void Set(std::string& b, size_t at, T v) {
    memcpy(&b[at], &v, sizeof(T));
}

// A minimal dynamic VHDX: region table, metadata (1 MiB blocks, 512-byte sectors), a block table
// whose first entry points at a payload block holding an ext4 superblock.
static std::wstring MakeVhdx(const std::wstring& name, uint64_t batEntry, uint32_t blocksLo, uint32_t freeLo, uint32_t logBlock,
                             bool is64 = false, uint32_t blocksHi = 0, uint32_t freeHi = 0) {
    const size_t MB = 1 << 20;
    std::string f(4 * MB + 4096, '\0');
    memcpy(&f[0], "vhdxfile", 8);
    // Region table at 192 KiB: BAT at 2 MiB, metadata at 1 MiB.
    size_t rt = 0x30000;
    memcpy(&f[rt], "regi", 4);
    Set<uint32_t>(f, rt + 8, 2);
    const GUID bat = {0x2DC27766, 0xF623, 0x4200, {0x9D, 0x64, 0x11, 0x5E, 0x9B, 0xFD, 0x4A, 0x08}};
    const GUID meta = {0x8B7CA206, 0x4790, 0x4B9A, {0xB8, 0xFE, 0x57, 0x5F, 0x05, 0x0F, 0x88, 0x6E}};
    memcpy(&f[rt + 16], &bat, 16);
    Set<uint64_t>(f, rt + 32, 2 * MB);
    Set<uint32_t>(f, rt + 40, static_cast<uint32_t>(MB));
    memcpy(&f[rt + 48], &meta, 16);
    Set<uint64_t>(f, rt + 64, 1 * MB);
    Set<uint32_t>(f, rt + 72, static_cast<uint32_t>(MB));
    // Metadata: file parameters (block size) and logical sector size.
    size_t md = MB;
    memcpy(&f[md], "metadata", 8);
    Set<uint16_t>(f, md + 10, 2);
    const GUID params = {0xCAA16737, 0xFA36, 0x4D43, {0xB3, 0xB6, 0x33, 0xF0, 0xAA, 0x44, 0xE7, 0x6B}};
    const GUID sector = {0x8141BF1D, 0xA96F, 0x4709, {0xBA, 0x47, 0xF2, 0x33, 0xA8, 0xFA, 0xAB, 0x5F}};
    memcpy(&f[md + 32], &params, 16);
    Set<uint32_t>(f, md + 48, 0x10000);
    Set<uint32_t>(f, md + 52, 8);
    memcpy(&f[md + 64], &sector, 16);
    Set<uint32_t>(f, md + 80, 0x10008);
    Set<uint32_t>(f, md + 84, 4);
    Set<uint32_t>(f, md + 0x10000, static_cast<uint32_t>(MB));  // block size
    Set<uint32_t>(f, md + 0x10008, 512);
    // Block table: entry 0 -> payload at 3 MiB.
    Set<uint64_t>(f, 2 * MB, batEntry);
    // ext4 superblock 1024 bytes into the payload block.
    size_t sb = 3 * MB + 1024;
    Set<uint32_t>(f, sb + 0x04, blocksLo);
    Set<uint32_t>(f, sb + 0x0C, freeLo);
    Set<uint32_t>(f, sb + 0x18, logBlock);
    Set<uint16_t>(f, sb + 0x38, 0xEF53);
    if (is64) {
        Set<uint32_t>(f, sb + 0x60, 0x80);
        Set<uint32_t>(f, sb + 0x150, blocksHi);
        Set<uint32_t>(f, sb + 0x158, freeHi);
    }
    wchar_t tmp[MAX_PATH];
    std::wstring path = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + name;
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") == 0 && out) {
        fwrite(f.data(), 1, f.size(), out);
        fclose(out);
    }
    return path;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const uint64_t present = (3ull << 20) | 6;  // payload block fully present at 3 MiB

    // ---- VHDX + ext4 superblock
    uint64_t used = 0, total = 0;
    std::wstring err;
    std::wstring a = MakeVhdx(L"winncdu-test-a.vhdx", present, 262144, 200000, 2);
    bool ok = ReadVhdxExt4(a, used, total, err);
    Check(ok && used == (262144ull - 200000) * 4096 && total == 262144ull * 4096,
          L"ext4 usage read from the first block of a VHDX: " + util::FormatSize(used) + L" of " + util::FormatSize(total) + L" " + err);
    std::wstring b = MakeVhdx(L"winncdu-test-b.vhdx", present, 5, 1, 2, true, 1, 0);
    err.clear();
    ok = ReadVhdxExt4(b, used, total, err);
    Check(ok && total == ((1ull << 32) + 5) * 4096 && used == ((1ull << 32) + 4) * 4096,
          L"64-bit ext4 block counts " + err);
    std::wstring c = MakeVhdx(L"winncdu-test-c.vhdx", 0, 262144, 200000, 2);
    err.clear();
    ok = ReadVhdxExt4(c, used, total, err);
    Check(!ok && !err.empty(), L"a never-written first block is no file system: " + err);
    std::wstring d = MakeVhdx(L"winncdu-test-d.vhdx", present, 10, 20, 2);
    err.clear();
    ok = ReadVhdxExt4(d, used, total, err);
    Check(!ok, L"more free blocks than blocks is refused: " + err);
    wchar_t tmp[MAX_PATH];
    std::wstring notVhdx = std::wstring(tmp, GetTempPathW(MAX_PATH, tmp)) + L"winncdu-test-e.vhdx";
    FILE* out = nullptr;
    if (_wfopen_s(&out, notVhdx.c_str(), L"wb") == 0 && out) {
        fputs("not a disk", out);
        fclose(out);
    }
    err.clear();
    ok = ReadVhdxExt4(notVhdx, used, total, err);
    Check(!ok && !err.empty(), L"other files are refused: " + err);
    for (auto& p : {a, b, c, d, notVhdx}) DeleteFileW(p.c_str());

    // ---- docker system df
    Check(ParseDockerSize(L"0B") == 0 && ParseDockerSize(L"749.6kB") == 749600 && ParseDockerSize(L"38.69GB (89%)") == 38690000000ull &&
              ParseDockerSize(L"1.5TB") == 1500000000000ull && ParseDockerSize(L"512B") == 512,
          L"Docker sizes (decimal units)");
    std::string df =
        "{\"Active\":\"5\",\"Reclaimable\":\"38.69GB (89%)\",\"Size\":\"43.28GB\",\"TotalCount\":\"18\",\"Type\":\"Images\"}\n"
        "{\"Active\":\"3\",\"Reclaimable\":\"2.691GB (10%)\",\"Size\":\"24.97GB\",\"TotalCount\":\"9\",\"Type\":\"Containers\"}\n"
        "{\"Active\":\"6\",\"Reclaimable\":\"6.003GB (72%)\",\"Size\":\"8.228GB\",\"TotalCount\":\"57\",\"Type\":\"Local Volumes\"}\r\n"
        "{\"Active\":\"0\",\"Reclaimable\":\"7.711GB\",\"Size\":\"11.2GB\",\"TotalCount\":\"219\",\"Type\":\"Build Cache\"}\n";
    auto rows = ParseDockerDf(df);
    Check(rows.size() == 4 && rows[0].type == L"Images" && rows[0].size == 43280000000ull && rows[0].reclaimable == 38690000000ull &&
              rows[0].total == 18 && rows[0].active == 5 && rows[3].type == L"Build Cache" && rows[3].reclaimable == 7711000000ull,
          L"docker system df rows");
    Check(ParseDockerDf("error during connect: this error may indicate that the docker daemon is not running").empty(),
          L"docker errors give no rows");

    // ---- df inside the distro
    uint64_t u = 0;
    Check(ParseDfUsed("Filesystem        1-blocks        Used    Available Capacity Mounted on\n"
                      "/dev/sdd    1081101176832 23707013120 1002417127424       3% /\n",
                      u) &&
              u == 23707013120ull,
          L"df -P (GNU)");
    Check(ParseDfUsed("Filesystem           1024-blocks    Used Available Capacity Mounted on\n"
                      "/dev/sde             1081101176832 53828435968 972280385536   5% /mnt/docker-desktop-disk\n",
                      u) &&
              u == 53828435968ull,
          L"df -P (busybox, Docker's distro)");
    Check(ParseDfUsed("Filesystem 1-blocks Used Available Capacity Mounted on\nC:\\ my disk 100 40 60 40% /mnt/c\n", u) && u == 40,
          L"df: a file system name with spaces");
    Check(!ParseDfUsed("df: /nope: No such file or directory\n", u), L"df errors are not a size");

    // ---- wsl.exe output (UTF-16LE, with or without a byte order mark)
    std::wstring names = L"Ubuntu\r\ndocker-desktop\r\n";
    std::string raw(reinterpret_cast<const char*>(names.data()), names.size() * 2);
    Check(DecodeToolOutput(raw) == names && DecodeToolOutput("\xFF\xFE" + raw) == names, L"wsl.exe UTF-16 output");
    Check(DecodeToolOutput("23707013120 /\n") == L"23707013120 /\n", L"UTF-8 output stays as is");

    // ---- diskpart
    std::string script = DiskpartScript({L"C:\\Users\\me\\AppData\\Local\\wsl\\{83250052}\\ext4.vhdx"});
    Check(script.find("select vdisk file=\"C:\\Users\\me\\AppData\\Local\\wsl\\{83250052}\\ext4.vhdx\"\r\nattach vdisk readonly\r\n"
                      "compact vdisk\r\ndetach vdisk\r\n") == 0 &&
              script.substr(script.size() - 6) == "exit\r\n",
          L"diskpart script: select, attach read-only, compact, detach");

    // ---- optional: a real disk (read-only)
    wchar_t real[MAX_PATH * 2];
    if (DWORD n = GetEnvironmentVariableW(L"WINNCDU_VHDX", real, MAX_PATH * 2); n && n < MAX_PATH * 2) {
        err.clear();
        ok = ReadVhdxExt4(real, used, total, err);
        std::string line = util::WideToUtf8(std::wstring(L"     ") + real + L": " +
                                            (ok ? util::FormatSize(used) + L" used of " + util::FormatSize(total) : L"error: " + err) + L"\n");
        fputs(line.c_str(), stdout);
    }

    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
