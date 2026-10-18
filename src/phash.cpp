#include "phash.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "analysis.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWork = 64;  // side of the grayscale copy both hashes are made from

// ---- resampling

struct Tap {
    int i;
    float w;
};

// Box filter with fractional coverage (downscaling), linear interpolation (upscaling).
std::vector<std::vector<Tap>> Taps(int n, int m) {
    std::vector<std::vector<Tap>> taps(m);
    double scale = static_cast<double>(n) / m;
    for (int d = 0; d < m; ++d) {
        if (scale <= 1.0) {
            double c = (d + 0.5) * scale - 0.5;
            int i0 = static_cast<int>(std::floor(c));
            float f = static_cast<float>(c - i0);
            int a = std::clamp(i0, 0, n - 1), b = std::clamp(i0 + 1, 0, n - 1);
            taps[d].push_back({a, 1.0f - f});
            taps[d].push_back({b, f});
            continue;
        }
        double lo = d * scale, hi = (d + 1) * scale;
        for (int i = static_cast<int>(std::floor(lo)); i < static_cast<int>(std::ceil(hi)) && i < n; ++i) {
            double a = std::max(lo, static_cast<double>(i)), b = std::min(hi, static_cast<double>(i + 1));
            if (b > a) taps[d].push_back({i, static_cast<float>((b - a) / scale)});
        }
    }
    return taps;
}

template <class T>
void Orient(std::vector<T>& px, int& w, int& h, int ch, int o) {
    if (o < 2 || o > 8 || w <= 0 || h <= 0) return;
    int nw = o >= 5 ? h : w, nh = o >= 5 ? w : h;
    std::vector<T> out(px.size());
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            int sx = x, sy = y;
            switch (o) {
                case 2: sx = w - 1 - x; break;
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                case 4: sy = h - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = h - 1 - x; break;
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                case 8: sx = w - 1 - y; sy = x; break;
            }
            const T* s = &px[(static_cast<size_t>(sy) * w + sx) * ch];
            T* d = &out[(static_cast<size_t>(y) * nw + x) * ch];
            for (int c = 0; c < ch; ++c) d[c] = s[c];
        }
    px.swap(out);
    w = nw;
    h = nh;
}

// ---- WIC

UINT BitsPerPixel(IWICImagingFactory* wic, const WICPixelFormatGUID& fmt) {
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo> pfi;
    UINT bpp = 0;
    if (SUCCEEDED(wic->CreateComponentInfo(fmt, &ci)) && SUCCEEDED(ci.As(&pfi))) pfi->GetBitsPerPixel(&bpp);
    return bpp;
}

int ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> qr;
    if (FAILED(frame->GetMetadataQueryReader(&qr))) return 1;
    for (const wchar_t* name : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        int v = 0;
        if (SUCCEEDED(qr->GetMetadataByName(name, &pv))) {
            if (pv.vt == VT_UI2) v = pv.uiVal;
            else if (pv.vt == VT_UI4) v = static_cast<int>(pv.ulVal);
            else if (pv.vt == VT_I2) v = pv.iVal;
            else if (pv.vt == VT_I4) v = pv.lVal;
            PropVariantClear(&pv);
        }
        if (v >= 1 && v <= 8) return v;
    }
    return 1;
}

struct Source {
    ComPtr<IWICBitmapSource> src;  // first frame, possibly already scaled down by the codec
    UINT width = 0, height = 0;    // the frame's full size (as stored, before orientation)
    int orientation = 1;
};

// Opens the first frame. When the codec can scale while decoding (JPEG: 1/2, 1/4, 1/8), decodes
// a copy whose sides stay about `minW` x `minH` or larger, which is several times faster.
bool OpenImage(IWICImagingFactory* wic, const std::wstring& path, UINT minW, UINT minH, Source& out) {
    if (!wic) return false;
    ComPtr<IStream> stream;
    HRESULT hr = SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(hr) && path.size() >= MAX_PATH)
        hr = SHCreateStreamOnFileEx(ToLongPath(path).c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(hr)) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;
    out.width = w;
    out.height = h;
    out.orientation = ReadOrientation(frame.Get());
    out.src = frame;

    ComPtr<IWICBitmapSourceTransform> xf;
    double s = std::max(static_cast<double>(minW) / w, static_cast<double>(minH) / h);
    if (s < 0.5 && SUCCEEDED(frame.As(&xf))) {
        UINT tw = std::max(1u, static_cast<UINT>(std::ceil(w * s))), th = std::max(1u, static_cast<UINT>(std::ceil(h * s)));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(xf->GetClosestSize(&tw, &th)) && tw && th && tw < w && th < h && SUCCEEDED(xf->GetClosestPixelFormat(&fmt))) {
            UINT bpp = BitsPerPixel(wic, fmt);
            uint64_t stride = (static_cast<uint64_t>(tw) * bpp + 31) / 32 * 4;
            uint64_t bytes = stride * th;
            if (bpp && bytes < (256ull << 20)) {
                std::vector<BYTE> buf(static_cast<size_t>(bytes));
                ComPtr<IWICBitmap> mem;
                if (SUCCEEDED(xf->CopyPixels(nullptr, tw, th, &fmt, WICBitmapTransformRotate0, static_cast<UINT>(stride),
                                             static_cast<UINT>(bytes), buf.data())) &&
                    SUCCEEDED(wic->CreateBitmapFromMemory(tw, th, fmt, static_cast<UINT>(stride), static_cast<UINT>(bytes), buf.data(), &mem)))
                    out.src = mem;
            }
        }
    }
    return true;
}

// Converts + scales the source to exactly w x h pixels of `fmt` (4 bytes per pixel).
bool ReadScaled(IWICImagingFactory* wic, IWICBitmapSource* src, const WICPixelFormatGUID& fmt, UINT w, UINT h, std::vector<uint8_t>& px) {
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return false;
    if (FAILED(wic->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(conv.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
    px.assign(static_cast<size_t>(w) * h * 4, 0);
    return SUCCEEDED(scaler->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()));
}

// ---- names that look like copies

bool LooksLikeCopy(const std::wstring& name) {
    std::wstring s = util::ToLower(name);
    size_t dot = s.rfind(L'.');
    if (dot != std::wstring::npos) s.resize(dot);
    for (const wchar_t* w : {L"copy", L"kopie", L"copie", L"kopia", L"copia"})
        if (s.find(w) != std::wstring::npos) return true;
    // "name (2)"
    if (s.size() >= 3 && s.back() == L')') {
        size_t open = s.rfind(L'(');
        if (open != std::wstring::npos && open + 1 < s.size() - 1) {
            bool digits = true;
            for (size_t i = open + 1; i < s.size() - 1; ++i) digits = digits && iswdigit(s[i]);
            if (digits) return true;
        }
    }
    return !s.empty() && s.back() == L'~';
}

}  // namespace

// =============================================================================================

bool IsPhotoExt(const std::wstring& e) {
    static const wchar_t* exts[] = {L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"bmp", L"dib", L"gif",
                                    L"tif", L"tiff", L"heic", L"heif", L"webp"};
    for (const wchar_t* x : exts)
        if (e == x) return true;
    return false;
}

void ResampleGray(const float* src, int sw, int sh, float* dst, int dw, int dh) {
    auto tx = Taps(sw, dw), ty = Taps(sh, dh);
    std::vector<float> tmp(static_cast<size_t>(sh) * dw);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < dw; ++x) {
            float v = 0;
            for (auto& t : tx[x]) v += src[static_cast<size_t>(y) * sw + t.i] * t.w;
            tmp[static_cast<size_t>(y) * dw + x] = v;
        }
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float v = 0;
            for (auto& t : ty[y]) v += tmp[static_cast<size_t>(t.i) * dw + x] * t.w;
            dst[static_cast<size_t>(y) * dw + x] = v;
        }
}

uint64_t DHashGray(const float* gray, int w, int h) {
    float p[8][9];
    ResampleGray(gray, w, h, &p[0][0], 9, 8);
    uint64_t bits = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (p[y][x] < p[y][x + 1]) bits |= 1ull << (y * 8 + x);
    return bits;
}

uint64_t PHashGray(const float* gray, int w, int h) {
    static float cosT[8][32];
    static std::once_flag once;
    std::call_once(once, [] {
        const double pi = 3.14159265358979323846;
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 32; ++x) cosT[u][x] = static_cast<float>(std::cos((2 * x + 1) * u * pi / 64.0));
    });
    std::vector<float> g(32 * 32);
    ResampleGray(gray, w, h, g.data(), 32, 32);
    // Separable DCT-II, only the 8x8 lowest frequencies.
    float rows[32][8];
    for (int y = 0; y < 32; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 32; ++x) s += g[y * 32 + x] * cosT[u][x];
            rows[y][u] = s;
        }
    float c[64];
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int y = 0; y < 32; ++y) s += rows[y][u] * cosT[v][y];
            c[v * 8 + u] = s;
        }
    // Median of the 63 AC coefficients (the DC term is overall brightness).
    float ac[63];
    std::copy(c + 1, c + 64, ac);
    std::nth_element(ac, ac + 31, ac + 63);
    float med = ac[31];
    uint64_t bits = 0;
    for (int i = 1; i < 64; ++i)
        if (c[i] > med) bits |= 1ull << i;
    return bits;
}

void OrientPixels(std::vector<uint8_t>& px, int& w, int& h, int channels, int orientation) { Orient(px, w, h, channels, orientation); }
void OrientGray(std::vector<float>& px, int& w, int& h, int orientation) { Orient(px, w, h, 1, orientation); }

bool HashImageFile(IWICImagingFactory* wic, const std::wstring& path, ImageHash& out) {
    out = ImageHash{};
    Source s;
    if (!OpenImage(wic, path, kWork, kWork, s)) return false;
    std::vector<uint8_t> px;
    if (!ReadScaled(wic, s.src.Get(), GUID_WICPixelFormat32bppBGRA, kWork, kWork, px)) return false;
    // Luma, transparent parts over white.
    std::vector<float> gray(kWork * kWork);
    double sum = 0, sum2 = 0;
    for (size_t i = 0; i < gray.size(); ++i) {
        const uint8_t* p = &px[i * 4];
        float a = p[3] / 255.0f;
        float l = 0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2];
        float v = l * a + 255.0f * (1.0f - a);
        gray[i] = v;
        sum += v;
        sum2 += static_cast<double>(v) * v;
    }
    int w = kWork, h = kWork;
    OrientGray(gray, w, h, s.orientation);
    double mean = sum / gray.size();
    double var = sum2 / gray.size() - mean * mean;
    out.flat = var < 2.0 * 2.0;
    out.dhash = DHashGray(gray.data(), w, h);
    out.phash = PHashGray(gray.data(), w, h);
    bool swap = s.orientation >= 5;
    out.width = swap ? s.height : s.width;
    out.height = swap ? s.width : s.height;
    out.ok = true;
    return true;
}

bool DecodeThumbnail(IWICImagingFactory* wic, const std::wstring& path, int maxSide, std::vector<uint8_t>& bgra, int& w, int& h) {
    Source s;
    if (maxSide < 1) return false;
    // The wanted size depends on the image's: open it without scaling to learn it (only the header
    // is read), then open again with the codec scaling down to about the preview size.
    // Sizes come from the stored (not yet upright) dimensions; turning upright keeps the aspect.
    if (!OpenImage(wic, path, UINT_MAX, UINT_MAX, s)) return false;
    double scale = std::min(1.0, static_cast<double>(maxSide) / std::max(s.width, s.height));
    UINT tw = std::max(1u, static_cast<UINT>(std::lround(s.width * scale)));
    UINT th = std::max(1u, static_cast<UINT>(std::lround(s.height * scale)));
    if (scale < 0.5 && !OpenImage(wic, path, tw, th, s)) return false;
    if (!ReadScaled(wic, s.src.Get(), GUID_WICPixelFormat32bppPBGRA, tw, th, bgra)) return false;
    w = static_cast<int>(tw);
    h = static_cast<int>(th);
    OrientPixels(bgra, w, h, 4, s.orientation);
    return true;
}

// =============================================================================================
// Multi-index hashing

HashIndex::HashIndex(std::vector<uint64_t> hashes) : hashes_(std::move(hashes)) {
    seen_.assign(hashes_.size(), 0);
    for (int c = 0; c < 4; ++c) {
        std::vector<uint32_t>& st = start_[c];
        st.assign(65537, 0);
        for (uint64_t h : hashes_) ++st[((h >> (16 * c)) & 0xFFFF) + 1];
        for (size_t k = 1; k < st.size(); ++k) st[k] += st[k - 1];
        std::vector<uint32_t> fill(st.begin(), st.end() - 1);
        ids_[c].resize(hashes_.size());
        for (size_t i = 0; i < hashes_.size(); ++i) ids_[c][fill[(hashes_[i] >> (16 * c)) & 0xFFFF]++] = static_cast<int>(i);
    }
}

void HashIndex::Visit(int chunk, uint32_t key, int bit, int left, uint64_t hash, int maxDist, std::vector<int>& out) const {
    for (uint32_t k = start_[chunk][key]; k < start_[chunk][key + 1]; ++k) {
        int id = ids_[chunk][k];
        if (seen_[id] == stamp_) continue;
        seen_[id] = stamp_;
        ++compared;
        if (Hamming(hashes_[id], hash) <= maxDist) out.push_back(id);
    }
    // Every key that differs in up to `left` more bits, each set of bits enumerated once.
    if (left == 0) return;
    for (int b = bit; b < 16; ++b) Visit(chunk, key ^ (1u << b), b + 1, left - 1, hash, maxDist, out);
}

void HashIndex::Query(uint64_t hash, int maxDist, std::vector<int>& out) const {
    compared = 0;
    if (hashes_.empty() || maxDist < 0) return;
    if (++stamp_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        stamp_ = 1;
    }
    int sub = std::min(maxDist / 4, 16);
    for (int c = 0; c < 4; ++c) Visit(c, static_cast<uint32_t>((hash >> (16 * c)) & 0xFFFF), 0, sub, hash, maxDist, out);
}

// =============================================================================================

PhotoThreshold ThresholdFor(PhotoMatch m) {
    switch (m) {
        case PhotoMatch::Strict: return {2, 4};
        case PhotoMatch::Loose: return {10, 14};
        default: return {5, 8};
    }
}

const wchar_t* PhotoMatchName(PhotoMatch m) {
    switch (m) {
        case PhotoMatch::Strict: return L"strict";
        case PhotoMatch::Loose: return L"loose";
        default: return L"normal";
    }
}

// ---- cache

std::wstring PhotoHashCache::DefaultFile() {
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\photohash.cache";
}

bool PhotoHashCache::Lookup(const std::wstring& path, uint64_t size, int64_t mtime, ImageHash& out) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = map_.find(util::ToLower(path));
    if (it == map_.end() || it->second.size != size || it->second.mtime != mtime) return false;
    it->second.used = true;
    out = it->second.hash;
    return true;
}

void PhotoHashCache::Store(const std::wstring& path, uint64_t size, int64_t mtime, const ImageHash& h) {
    std::lock_guard<std::mutex> g(lock_);
    Entry& e = map_[util::ToLower(path)];
    e.size = size;
    e.mtime = mtime;
    e.hash = h;
    e.used = true;
    dirty_ = true;
}

size_t PhotoHashCache::Size() {
    std::lock_guard<std::mutex> g(lock_);
    return map_.size();
}

static const char kCacheHeader[] = "Bytopsy photo hashes 1\n";

void PhotoHashCache::Load() {
    std::lock_guard<std::mutex> g(lock_);
    if (loaded_) return;
    loaded_ = true;
    if (file_.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file_.c_str(), L"rb") != 0 || !f) return;
    std::string data;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.compare(0, sizeof(kCacheHeader) - 1, kCacheHeader) != 0) return;
    size_t pos = sizeof(kCacheHeader) - 1;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        // size  mtime  width  height  flags  dhash  phash  path
        const char* p = line.c_str();
        char* end = nullptr;
        Entry e;
        e.size = _strtoui64(p, &end, 10);
        if (*end != '\t') continue;
        e.mtime = _strtoi64(end + 1, &end, 10);
        if (*end != '\t') continue;
        e.hash.width = static_cast<uint32_t>(strtoul(end + 1, &end, 10));
        if (*end != '\t') continue;
        e.hash.height = static_cast<uint32_t>(strtoul(end + 1, &end, 10));
        if (*end != '\t') continue;
        unsigned long flags = strtoul(end + 1, &end, 10);
        if (*end != '\t') continue;
        e.hash.dhash = _strtoui64(end + 1, &end, 16);
        if (*end != '\t') continue;
        e.hash.phash = _strtoui64(end + 1, &end, 16);
        if (*end != '\t' || !end[1]) continue;
        e.hash.ok = (flags & 1) != 0;
        e.hash.flat = (flags & 2) != 0;
        std::wstring path = util::Utf8ToWide(end + 1);
        map_[util::ToLower(path)] = e;
    }
}

bool PhotoHashCache::Save() {
    std::lock_guard<std::mutex> g(lock_);
    if (file_.empty() || !dirty_) return true;
    // Keep it bounded: when it grows large, entries nobody asked for this session go.
    if (map_.size() > 300000)
        for (auto it = map_.begin(); it != map_.end();) it = it->second.used ? std::next(it) : map_.erase(it);
    size_t slash = file_.find_last_of(L'\\');
    if (slash != std::wstring::npos) SHCreateDirectoryExW(nullptr, file_.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = file_ + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    std::string out = kCacheHeader;
    char head[160];
    for (auto& kv : map_) {
        const Entry& e = kv.second;
        snprintf(head, sizeof(head), "%llu\t%lld\t%u\t%u\t%d\t%016llx\t%016llx\t", static_cast<unsigned long long>(e.size),
                 static_cast<long long>(e.mtime), e.hash.width, e.hash.height, (e.hash.ok ? 1 : 0) | (e.hash.flat ? 2 : 0),
                 static_cast<unsigned long long>(e.hash.dhash), static_cast<unsigned long long>(e.hash.phash));
        out += head;
        out += util::WideToUtf8(kv.first);
        out += '\n';
        if (out.size() > (1 << 20)) {
            fwrite(out.data(), 1, out.size(), f);
            out.clear();
        }
    }
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (ok) ok = MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    if (ok) dirty_ = false;
    return ok;
}

// =============================================================================================
// Grouping

uint64_t PhotoGroup::Reclaimable() const {
    uint64_t b = 0;
    for (size_t i = 1; i < photos.size(); ++i) b += photos[i].node->size;
    return b;
}

bool PhotoBetter(const Photo& a, const Photo& b) {
    uint64_t pa = static_cast<uint64_t>(a.hash.width) * a.hash.height, pb = static_cast<uint64_t>(b.hash.width) * b.hash.height;
    if (pa != pb) return pa > pb;
    if (a.node->size != b.node->size) return a.node->size > b.node->size;
    if (a.node->mtime != b.node->mtime) return a.node->mtime < b.node->mtime;
    bool ca = LooksLikeCopy(a.node->name), cb = LooksLikeCopy(b.node->name);
    if (ca != cb) return !ca;
    if (a.node->name.size() != b.node->name.size()) return a.node->name.size() < b.node->name.size();
    return NodePath(a.node) < NodePath(b.node);
}

std::vector<PhotoGroup> GroupPhotos(std::vector<Photo> photos, PhotoThreshold t) {
    photos.erase(std::remove_if(photos.begin(), photos.end(), [](const Photo& p) { return !p.hash.ok || p.hash.flat; }), photos.end());
    int n = static_cast<int>(photos.size());
    std::vector<uint64_t> dh(n);
    for (int i = 0; i < n; ++i) dh[i] = photos[i].hash.dhash;
    HashIndex index(std::move(dh));
    std::vector<int> parent(n);
    for (int i = 0; i < n; ++i) parent[i] = i;
    auto find = [&](int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    std::vector<int> cand;
    for (int i = 0; i < n; ++i) {
        cand.clear();
        index.Query(photos[i].hash.dhash, t.dhash, cand);
        for (int j : cand) {
            if (j <= i || Hamming(photos[i].hash.phash, photos[j].hash.phash) > t.phash) continue;
            int a = find(i), b = find(j);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    }
    std::unordered_map<int, std::vector<Photo>> byRoot;
    for (int i = 0; i < n; ++i) byRoot[find(i)].push_back(photos[i]);
    // Similarity chains (frame 1 ~ frame 2 ~ ... ~ frame 600), so a connected set is split around
    // keepers: the best photo takes everything close to *it*, then the best one left, and so on.
    // Whatever a group suggests deleting is then always close to the photo it keeps.
    auto close = [&](const Photo& a, const Photo& b) {
        return Hamming(a.hash.dhash, b.hash.dhash) <= t.dhash && Hamming(a.hash.phash, b.hash.phash) <= t.phash;
    };
    std::vector<PhotoGroup> out;
    for (auto& kv : byRoot) {
        std::vector<Photo>& set = kv.second;
        if (set.size() < 2) continue;
        std::sort(set.begin(), set.end(), PhotoBetter);
        std::vector<char> used(set.size(), 0);
        for (size_t i = 0; i < set.size(); ++i) {
            if (used[i]) continue;
            used[i] = 1;
            PhotoGroup g;
            g.photos.push_back(set[i]);
            for (size_t j = i + 1; j < set.size(); ++j)
                if (!used[j] && close(set[i], set[j])) {
                    used[j] = 1;
                    g.photos.push_back(set[j]);
                    g.maxDist = std::max(g.maxDist, Hamming(set[i].hash.dhash, set[j].hash.dhash));
                }
            if (g.photos.size() >= 2) out.push_back(std::move(g));
        }
    }
    std::sort(out.begin(), out.end(), [](const PhotoGroup& a, const PhotoGroup& b) {
        uint64_t ra = a.Reclaimable(), rb = b.Reclaimable();
        if (ra != rb) return ra > rb;
        return NodePath(a.photos[0].node) < NodePath(b.photos[0].node);
    });
    return out;
}

std::vector<PhotoGroup> FindSimilarPhotos(Node* scope, uint64_t minSize, PhotoMatch match, int threads, PhotoProgress& prog,
                                          PhotoHashCache* cache) {
    prog.stage = 0;
    std::vector<Node*> files;
    std::vector<Node*> stack{scope};
    while (!stack.empty()) {
        Node* d = stack.back();
        stack.pop_back();
        for (auto& c : d->children) {
            Node* n = c.get();
            if (n->IsDir()) {
                if (n->CanEnter()) stack.push_back(n);
                continue;
            }
            if (n->Has(NF_LINK | NF_HARDLINK | NF_EXCLUDED | NF_CLOUD) || n->size == 0 || n->size < minSize) continue;
            if (n->attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) continue;
            if (IsPhotoExt(FileExt(n->name))) files.push_back(n);
        }
    }
    std::vector<std::wstring> paths(files.size());
    for (size_t i = 0; i < files.size(); ++i) paths[i] = NodePath(files[i]);
    prog.filesTotal = files.size();
    prog.filesDone = 0;
    if (prog.cancel) return {};
    if (cache) cache->Load();

    prog.stage = 1;
    std::vector<ImageHash> hashes(files.size());
    std::atomic<size_t> next{0};
    auto worker = [&] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICImagingFactory> wic;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= files.size() || prog.cancel.load()) break;
            Node* n = files[i];
            ImageHash h;
            if (cache && cache->Lookup(paths[i], n->size, n->mtime, h)) {
                ++prog.cached;
            } else {
                if (!wic || !HashImageFile(wic.Get(), paths[i], h)) h = ImageHash{};
                if (cache && !prog.cancel.load()) cache->Store(paths[i], n->size, n->mtime, h);
            }
            if (!h.ok) ++prog.failed;
            hashes[i] = h;
            ++prog.filesDone;
        }
        wic.Reset();
        if (SUCCEEDED(co)) CoUninitialize();
    };
    std::vector<std::thread> pool;
    int nt = std::clamp(threads, 1, 16);
    for (int t = 0; t < nt; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (cache) cache->Save();  // what was decoded is kept even when cancelled
    if (prog.cancel) return {};

    prog.stage = 2;
    std::vector<Photo> photos;
    for (size_t i = 0; i < files.size(); ++i) photos.push_back({files[i], hashes[i]});
    return GroupPhotos(std::move(photos), ThresholdFor(match));
}
