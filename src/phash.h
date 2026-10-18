#pragma once

// Similar photos: perceptual hashes of images decoded with WIC (Windows Imaging Component), grouped
// by Hamming distance. Two hashes per image, both from a small grayscale copy that is turned upright
// first (EXIF orientation):
//   dHash  9x8 brightness gradients (left < right), 64 bits
//   pHash  32x32 DCT, the 8x8 lowest frequencies above / below their median, 64 bits
// Two photos match when both distances are within the chosen threshold, so resized, re-encoded or
// slightly brightened copies match while different shots of the same scene usually don't. Images
// are only read, never written.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "tree.h"

struct IWICImagingFactory;

// jpg jpeg jpe jfif png bmp dib gif tif tiff heic heif webp (lowercase, no dot).
bool IsPhotoExt(const std::wstring& extLower);

inline int Hamming(uint64_t a, uint64_t b) {
    uint64_t x = a ^ b;
    int n = 0;
    while (x) {
        x &= x - 1;
        ++n;
    }
    return n;
}

// Area-average resampling of a grayscale image (row-major floats).
void ResampleGray(const float* src, int sw, int sh, float* dst, int dw, int dh);
// Hashes of a grayscale image of any size (resampled internally).
uint64_t DHashGray(const float* gray, int w, int h);
uint64_t PHashGray(const float* gray, int w, int h);

struct ImageHash {
    uint64_t dhash = 0, phash = 0;
    uint32_t width = 0, height = 0;  // upright pixel size
    bool ok = false;                 // decoded
    bool flat = false;               // (nearly) one color: never grouped
};

// EXIF orientation 1..8 applied to a w x h buffer of `channels` bytes per pixel (or floats when
// `channels` is 0, one per pixel). Returns the upright size in w/h.
void OrientPixels(std::vector<uint8_t>& px, int& w, int& h, int channels, int orientation);
void OrientGray(std::vector<float>& px, int& w, int& h, int orientation);

// Decodes the first frame small (the codec scales down while decoding where it can, e.g. JPEG),
// turns it upright and hashes it. `wic` belongs to the calling thread.
bool HashImageFile(IWICImagingFactory* wic, const std::wstring& path, ImageHash& out);

// A small upright preview: premultiplied BGRA, longest side <= maxSide.
bool DecodeThumbnail(IWICImagingFactory* wic, const std::wstring& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h);

// Finds every hash within a Hamming distance without comparing against all of them (multi-index
// hashing): the 64 bits are split into four 16-bit chunks, each with its own table. Two hashes at
// most r bits apart have at least one chunk at most r/4 bits apart, so a query only looks into the
// buckets near its own four chunks. Exact; much faster than a BK-tree on 64-bit hashes, where a
// radius of 7 already visits a third of the tree. Not thread-safe (Query reuses a scratch list).
class HashIndex {
public:
    explicit HashIndex(std::vector<uint64_t> hashes);  // id = position
    // Appends the ids of every hash within maxDist of `hash` (each once).
    void Query(uint64_t hash, int maxDist, std::vector<int>& out) const;
    size_t Size() const { return hashes_.size(); }
    // Number of hashes compared by the last Query (for tests).
    mutable size_t compared = 0;

private:
    void Visit(int chunk, uint32_t key, int bit, int left, uint64_t hash, int maxDist, std::vector<int>& out) const;
    std::vector<uint64_t> hashes_;
    std::vector<uint32_t> start_[4];  // bucket offsets into ids_ (65537 each)
    std::vector<int> ids_[4];
    mutable std::vector<uint32_t> seen_;
    mutable uint32_t stamp_ = 0;
};

enum class PhotoMatch { Strict, Normal, Loose };
struct PhotoThreshold {
    int dhash, phash;
};
PhotoThreshold ThresholdFor(PhotoMatch m);
const wchar_t* PhotoMatchName(PhotoMatch m);

// Hashes per (path, size, modified time), so a second search only decodes new or changed images.
// Thread-safe. With a file, Load() reads earlier sessions and Save() writes them back.
class PhotoHashCache {
public:
    explicit PhotoHashCache(std::wstring file = L"") : file_(std::move(file)) {}
    // %LOCALAPPDATA%\WinNCDU\photohash.cache
    static std::wstring DefaultFile();
    bool Lookup(const std::wstring& path, uint64_t size, int64_t mtime, ImageHash& out);
    void Store(const std::wstring& path, uint64_t size, int64_t mtime, const ImageHash& h);
    void Load();  // once; later calls do nothing
    bool Save();  // only when something changed
    size_t Size();
    const std::wstring& File() const { return file_; }

private:
    struct Entry {
        uint64_t size = 0;
        int64_t mtime = 0;
        ImageHash hash;
        bool used = false;  // looked up or stored this session
    };
    std::mutex lock_;
    std::wstring file_;
    std::unordered_map<std::wstring, Entry> map_;  // lowercase path
    bool loaded_ = false, dirty_ = false;
};

struct Photo {
    Node* node = nullptr;
    ImageHash hash;
};

struct PhotoGroup {
    std::vector<Photo> photos;  // the suggested keeper first; every other one is within the threshold of it
    int maxDist = 0;            // largest dHash distance from the keeper (0 = the same picture)
    uint64_t Reclaimable() const;
};

struct PhotoProgress {
    std::atomic<int> stage{0};  // 0 collect, 1 decode + hash, 2 compare
    std::atomic<uint64_t> filesDone{0}, filesTotal{0}, cached{0}, failed{0};
    std::atomic<bool> cancel{false};
};

// The order that picks the keeper: most pixels, then largest file, then oldest, then the name that
// looks like an original (no "copy", "(2)", ...), then the shorter name.
bool PhotoBetter(const Photo& a, const Photo& b);

// Groups the images below `scope` (at least `minSize` bytes) that look alike. Cloud placeholders,
// links and hard-link duplicates are skipped (decoding a placeholder would download it).
std::vector<PhotoGroup> FindSimilarPhotos(Node* scope, uint64_t minSize, PhotoMatch match, int threads, PhotoProgress& prog,
                                          PhotoHashCache* cache);
// The grouping step alone (exposed for tests): photos with ok && !flat hashes.
std::vector<PhotoGroup> GroupPhotos(std::vector<Photo> photos, PhotoThreshold t);
