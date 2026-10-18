#pragma once

// WSL and Docker Desktop virtual disks (ext4.vhdx, docker_data.vhdx). They grow as Linux writes
// files but never shrink by themselves: deleting files inside frees space for Linux, not for
// Windows. This module shows each disk's size on the drive against the space used inside, compacts
// disks (WSL shut down, then diskpart), and runs Docker's own prune commands for the parts of
// Docker's data that come back by themselves.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct VDisk {
    std::wstring name;    // "Ubuntu", "Docker Desktop data"
    std::wstring distro;  // the WSL distro that mounts it ("docker-desktop" for Docker's data disk)
    std::wstring mount;   // where that distro mounts it ("/", "/mnt/docker-desktop-disk")
    std::wstring path;    // ...\ext4.vhdx
    bool docker = false;  // belongs to Docker Desktop
    bool running = false;  // its distro is running right now
    uint64_t fileBytes = 0;  // space the file takes on the drive
    uint64_t used = 0;       // used inside (valid if usedKnown)
    bool usedKnown = false;
    std::wstring usedFrom;  // "df" (asked the running distro) or "superblock" (read from the file)
    uint64_t Reclaimable() const { return usedKnown && fileBytes > used ? fileBytes - used : 0; }
};

// Registered WSL 2 distros (HKCU\...\Lxss) and Docker Desktop's data disk. Asks running distros
// for their used space (`df`); stopped ones are never started: their used space is read from the
// ext4 superblock inside the .vhdx.
std::vector<VDisk> FindVDisks();

// Reads the ext4 superblock at the start of a .vhdx: used and total bytes of its file system.
bool ReadVhdxExt4(const std::wstring& vhdx, uint64_t& used, uint64_t& total, std::wstring& err);

struct DockerRow {
    std::wstring type;  // "Images", "Containers", "Local Volumes", "Build Cache"
    uint64_t size = 0, reclaimable = 0;
    int total = 0, active = 0;
};
struct DockerInfo {
    bool cli = false;      // docker.exe found
    bool running = false;  // the engine answered
    bool desktopRunning = false;  // Docker Desktop.exe is running
    std::wstring error;    // why `docker system df` failed
    std::vector<DockerRow> rows;
    const DockerRow* Find(const wchar_t* type) const;
};
DockerInfo QueryDocker();

// Docker's own cleanup for what comes back by itself: the build cache (`docker builder prune -a`,
// rebuilt by the next build) and images no container uses (`docker image prune -a`, pulled again
// when needed). Containers and volumes are never pruned: they can hold data.
struct DockerPruneResult {
    uint64_t freed = 0;
    std::vector<std::wstring> errors;
};
DockerPruneResult DockerPrune(bool buildCache, bool images, std::atomic<bool>& cancel);

struct CompactResult {
    uint64_t before = 0, after = 0;
    size_t compacted = 0;
    std::vector<std::wstring> errors;
    bool dockerRestarted = false;
};
// Trims free space inside the running distros, stops Docker Desktop if it runs (and starts it again
// afterwards), shuts WSL down, then compacts each disk with diskpart (asks for administrator rights
// if needed). Every running Linux distro stops.
CompactResult CompactVDisks(const std::vector<VDisk>& disks, std::atomic<bool>& cancel,
                            const std::function<void(const std::wstring&)>& status);

// Exposed for tests.
uint64_t ParseDockerSize(const std::wstring& s);                   // "38.69GB" -> 38690000000
std::vector<DockerRow> ParseDockerDf(const std::string& jsonLines);  // `docker system df --format "{{json .}}"`
bool ParseDfUsed(const std::string& dfOutput, uint64_t& used);     // `df -P -B1 <mount>`
std::string DiskpartScript(const std::vector<std::wstring>& vhdx);
