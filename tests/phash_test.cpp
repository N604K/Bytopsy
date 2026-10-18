// Self-check of the similar-photo finder: winncdu-phash-test.exe
// Hamming distance, the hash index (against brute force), resampling and EXIF orientation, then real
// images written with WIC into %TEMP%: a picture, the same picture smaller and re-encoded as JPEG,
// brightened, stored sideways with an EXIF orientation tag, mirrored, an unrelated picture and a
// blank one. Checks near/far distances, grouping, the suggested keeper and the hash cache.
// Everything is created in and removed from a temporary folder.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

#include "phash.h"
#include "scanner.h"
#include "tree.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static std::wstring Num(long long v) { return std::to_wstring(v); }

// ---- synthetic pictures (24bpp BGR)

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGR
};

static Img Pattern(int w, int h, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    Img im{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 3)};
    std::vector<float> f(static_cast<size_t>(w) * h * 3);
    float g0[3] = {U(rng) * 255, U(rng) * 255, U(rng) * 255}, g1[3] = {U(rng) * 255, U(rng) * 255, U(rng) * 255};
    bool vertical = U(rng) < 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float t = vertical ? static_cast<float>(y) / h : static_cast<float>(x) / w;
            for (int c = 0; c < 3; ++c) f[(static_cast<size_t>(y) * w + x) * 3 + c] = g0[c] * (1 - t) + g1[c] * t;
        }
    for (int k = 0; k < 14; ++k) {
        float cx = U(rng) * w, cy = U(rng) * h, r = (0.06f + U(rng) * 0.22f) * std::min(w, h);
        float col[3] = {U(rng) * 255, U(rng) * 255, U(rng) * 255};
        bool rect = U(rng) < 0.4f;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float dx = (x - cx) / r, dy = (y - cy) / r;
                float d = rect ? std::max(std::fabs(dx), std::fabs(dy)) : std::sqrt(dx * dx + dy * dy);
                float a = std::clamp((1.0f - d) * 6.0f, 0.0f, 1.0f) * 0.85f;
                if (a <= 0) continue;
                for (int c = 0; c < 3; ++c) {
                    float& v = f[(static_cast<size_t>(y) * w + x) * 3 + c];
                    v = v * (1 - a) + col[c] * a;
                }
            }
    }
    for (size_t i = 0; i < f.size(); ++i) im.px[i] = static_cast<uint8_t>(std::clamp(f[i], 0.0f, 255.0f));
    return im;
}

static Img Half(const Img& a) {
    Img o{a.w / 2, a.h / 2, {}};
    o.px.resize(static_cast<size_t>(o.w) * o.h * 3);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x)
            for (int c = 0; c < 3; ++c) {
                int s = 0;
                for (int k = 0; k < 4; ++k) s += a.px[(static_cast<size_t>(2 * y + k / 2) * a.w + 2 * x + k % 2) * 3 + c];
                o.px[(static_cast<size_t>(y) * o.w + x) * 3 + c] = static_cast<uint8_t>(s / 4);
            }
    return o;
}

static Img Brighter(const Img& a, int add) {
    Img o = a;
    for (auto& v : o.px) v = static_cast<uint8_t>(std::clamp(v + add, 0, 255));
    return o;
}

// Stored rotated 90 degrees counter-clockwise: EXIF orientation 6 (rotate clockwise to view) makes it upright.
static Img StoredForOrientation6(const Img& a) {
    Img o{a.h, a.w, std::vector<uint8_t>(a.px.size())};
    // Upright (x, y) = stored(y, h_stored - 1 - x) with stored size (a.h x a.w).
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x) {
            int sx = y, sy = o.h - 1 - x;
            for (int c = 0; c < 3; ++c) o.px[(static_cast<size_t>(sy) * o.w + sx) * 3 + c] = a.px[(static_cast<size_t>(y) * a.w + x) * 3 + c];
        }
    return o;
}

static Img Mirror(const Img& a) {
    Img o = a;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 3; ++c) o.px[(static_cast<size_t>(y) * a.w + x) * 3 + c] = a.px[(static_cast<size_t>(y) * a.w + (a.w - 1 - x)) * 3 + c];
    return o;
}

static bool Save(IWICImagingFactory* wic, const std::wstring& path, const Img& im, bool jpeg, float quality = 0.9f, int orientation = 0) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &props)))
        return false;
    if (jpeg) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(im.w, im.h))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) return false;
    if (orientation) {
        ComPtr<IWICMetadataQueryWriter> qw;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_UI2;
        pv.uiVal = static_cast<USHORT>(orientation);
        if (FAILED(frame->GetMetadataQueryWriter(&qw)) || FAILED(qw->SetMetadataByName(L"/app1/ifd/{ushort=274}", &pv))) return false;
    }
    UINT stride = im.w * 3;
    if (FAILED(frame->WritePixels(im.h, stride, stride * im.h, const_cast<BYTE*>(im.px.data())))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

static void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(p);
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static std::unique_ptr<Node> Scan(const std::wstring& path) {
    auto root = std::make_unique<Node>();
    root->name = path;
    root->flags = NF_DIR;
    ReadNodeMeta(root.get(), path);
    ScanOptions opt;
    ScanStats stats;
    ScanDirectory(root.get(), path, opt, stats);
    RecomputeTotals(root.get());
    return root;
}

// ---- unit parts

static void TestBasics() {
    Check(Hamming(0, ~0ull) == 64 && Hamming(0xB, 0x1) == 2 && Hamming(42, 42) == 0, L"Hamming distance");

    // Resampling: a constant stays constant, 2x2 blocks are averaged.
    std::vector<float> c(37 * 23, 100.0f), out(9 * 8);
    ResampleGray(c.data(), 37, 23, out.data(), 9, 8);
    bool same = true;
    for (float v : out) same = same && std::fabs(v - 100.0f) < 0.01f;
    float q[16] = {0, 2, 10, 10, 4, 6, 10, 10, 1, 1, 3, 3, 1, 1, 3, 3}, q2[4];
    ResampleGray(q, 4, 4, q2, 2, 2);
    Check(same && std::fabs(q2[0] - 3) < 1e-4f && std::fabs(q2[1] - 10) < 1e-4f && std::fabs(q2[2] - 1) < 1e-4f && std::fabs(q2[3] - 3) < 1e-4f,
          L"area resampling");

    // Orientation on a 3 x 2 image:  a b c / d e f
    auto orient = [](int o, int& w, int& h) {
        std::vector<uint8_t> px = {'a', 'b', 'c', 'd', 'e', 'f'};
        w = 3;
        h = 2;
        OrientPixels(px, w, h, 1, o);
        return std::string(px.begin(), px.end());
    };
    int w, h;
    std::string r6 = orient(6, w, h);  // rotate clockwise: d a / e b / f c
    Check(r6 == "daebfc" && w == 2 && h == 3, L"EXIF orientation 6 rotates clockwise");
    std::string r8 = orient(8, w, h);  // counter-clockwise: c f / b e / a d
    Check(r8 == "cfbead" && w == 2 && h == 3, L"EXIF orientation 8 rotates counter-clockwise");
    Check(orient(3, w, h) == "fedcba" && orient(2, w, h) == "cbafed" && orient(4, w, h) == "defabc" && orient(1, w, h) == "abcdef",
          L"EXIF orientations 1-4");
    Check(orient(5, w, h) == "adbecf" && orient(7, w, h) == "fcebda", L"EXIF orientations 5 and 7 (transpose, transverse)");
}

static void TestHashIndex() {
    std::mt19937_64 rng(12345);
    const int n = 30000;
    std::vector<uint64_t> hs;
    for (int i = 0; i < n; ++i) {
        uint64_t v = rng();
        // A third of them are near copies of an earlier hash (a few bits flipped), as photos are.
        if (i > 10 && i % 3 == 0) {
            v = hs[rng() % hs.size()];
            int flips = static_cast<int>(rng() % 9);
            for (int k = 0; k < flips; ++k) v ^= 1ull << (rng() % 64);
        }
        hs.push_back(v);
    }
    HashIndex index(hs);
    Check(index.Size() == static_cast<size_t>(n), L"hash index holds every hash (duplicates included)");
    bool match = true;
    for (int qi = 0; qi < 400 && match; ++qi) {
        uint64_t q = hs[rng() % hs.size()];
        if (qi % 2) q ^= 1ull << (rng() % 64);
        int r = qi % 20;
        std::vector<int> got, want;
        index.Query(q, r, got);
        for (int i = 0; i < n; ++i)
            if (Hamming(hs[i], q) <= r) want.push_back(i);
        std::sort(got.begin(), got.end());
        match = got == want;
        if (!match) wprintf(L"     radius %d: %zu found, %zu expected\n", r, got.size(), want.size());
    }
    Check(match, L"hash index finds exactly what brute force finds (radius 0-19), each id once");

    for (PhotoMatch m : {PhotoMatch::Strict, PhotoMatch::Normal, PhotoMatch::Loose}) {
        int r = ThresholdFor(m).dhash;
        auto t0 = std::chrono::steady_clock::now();
        std::vector<int> tmp;
        size_t compared = 0;
        for (int i = 0; i < n; ++i) {
            tmp.clear();
            index.Query(hs[i], r, tmp);
            compared += index.compared;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        wprintf(L"     %d queries at radius %d: %.0f ms, %.2f%% of the hashes compared on average\n", n, r, ms, 100.0 * compared / n / n);
        Check(compared < static_cast<size_t>(n) * n / 20, std::wstring(L"hash index prunes at ") + PhotoMatchName(m) + L" (under 5% compared)");
    }
}

static void TestImages(const std::wstring& dir) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
        Check(false, L"WIC imaging factory");
        return;
    }
    Img a = Pattern(640, 480, 7), b = Pattern(640, 480, 99), other = Pattern(640, 480, 2024);
    Img flat{320, 240, std::vector<uint8_t>(320 * 240 * 3, 128)};
    CreateDirectoryW((dir + L"\\album").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\backup").c_str(), nullptr);
    struct F {
        std::wstring name;
        bool ok;
    } files[] = {
        {L"album\\beach.png", Save(wic.Get(), dir + L"\\album\\beach.png", a, false)},
        {L"backup\\beach small.jpg", Save(wic.Get(), dir + L"\\backup\\beach small.jpg", Half(a), true, 0.55f)},
        {L"backup\\beach - Copy.jpg", Save(wic.Get(), dir + L"\\backup\\beach - Copy.jpg", Brighter(a, 22), true, 0.92f)},
        {L"album\\beach sideways.jpg", Save(wic.Get(), dir + L"\\album\\beach sideways.jpg", StoredForOrientation6(a), true, 0.9f, 6)},
        {L"album\\beach mirrored.png", Save(wic.Get(), dir + L"\\album\\beach mirrored.png", Mirror(a), false)},
        {L"album\\forest.png", Save(wic.Get(), dir + L"\\album\\forest.png", b, false)},
        {L"album\\city.jpg", Save(wic.Get(), dir + L"\\album\\city.jpg", other, true, 0.8f)},
        {L"album\\blank.png", Save(wic.Get(), dir + L"\\album\\blank.png", flat, false)},
        {L"album\\blank2.jpg", Save(wic.Get(), dir + L"\\album\\blank2.jpg", Brighter(flat, 60), true)},
    };
    bool allSaved = true;
    for (auto& f : files) allSaved = allSaved && f.ok;
    Check(allSaved, L"fixture images written with WIC (PNG, JPEG, JPEG with EXIF orientation)");
    {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, (dir + L"\\album\\broken.jpg").c_str(), L"wb") == 0 && fp) {
            fputs("not really a jpeg", fp);
            fclose(fp);
        }
    }

    ImageHash h[9];
    for (int i = 0; i < 9; ++i) HashImageFile(wic.Get(), dir + L"\\" + files[i].name, h[i]);
    bool decoded = true;
    for (auto& x : h) decoded = decoded && x.ok;
    Check(decoded, L"every fixture decodes");
    ImageHash bad;
    Check(!HashImageFile(wic.Get(), dir + L"\\album\\broken.jpg", bad) && !bad.ok, L"a broken file is reported, not hashed");
    Check(h[0].width == 640 && h[0].height == 480 && h[1].width == 320 && h[1].height == 240, L"sizes are read");
    Check(h[3].width == 640 && h[3].height == 480, L"sideways JPEG is reported upright (" + Num(h[3].width) + L" x " + Num(h[3].height) + L")");
    Check(h[7].flat && h[8].flat && !h[0].flat && !h[5].flat, L"blank images are recognized as flat");

    PhotoThreshold t = ThresholdFor(PhotoMatch::Normal);
    const wchar_t* names[] = {L"smaller + JPEG 55%", L"brightened + JPEG", L"sideways + EXIF tag", L"mirrored", L"unrelated 1", L"unrelated 2"};
    for (int i = 1; i <= 6; ++i) {
        int dd = Hamming(h[0].dhash, h[i].dhash), dp = Hamming(h[0].phash, h[i].phash);
        bool nearExpected = i <= 3;
        bool isNear = dd <= t.dhash && dp <= t.phash;
        Check(isNear == nearExpected, std::wstring(names[i - 1]) + L": dHash " + Num(dd) + L", pHash " + Num(dp) + L" -> " +
                                          (isNear ? L"similar" : L"different"));
    }
    int dd = Hamming(h[0].dhash, h[1].dhash);
    Check(dd <= ThresholdFor(PhotoMatch::Strict).dhash + 2, L"a plain resize/re-encode is close even for Strict-ish (dHash " + Num(dd) + L")");

    std::vector<uint8_t> px;
    int tw = 0, th = 0;
    bool thumb = DecodeThumbnail(wic.Get(), dir + L"\\album\\beach sideways.jpg", 64, px, tw, th);
    Check(thumb && tw == 64 && th == 48 && px.size() == 64u * 48 * 4, L"thumbnail is small and upright (" + Num(tw) + L" x " + Num(th) + L")");

    // ---- whole pipeline over a scan
    auto root = Scan(dir);
    std::wstring cacheFile = dir + L"\\cache\\photohash.cache";
    PhotoHashCache cache(cacheFile);
    PhotoProgress prog;
    auto groups = FindSimilarPhotos(root.get(), 1, PhotoMatch::Normal, 4, prog, &cache);
    Check(prog.filesTotal == 10 && prog.failed == 1, L"10 images considered, 1 unreadable (" + Num(static_cast<long long>(prog.filesTotal)) + L", " +
                                                          Num(static_cast<long long>(prog.failed)) + L")");
    Check(groups.size() == 1, L"one group of similar photos (" + Num(static_cast<long long>(groups.size())) + L")");
    if (groups.size() == 1) {
        auto& g = groups[0];
        std::vector<std::wstring> got;
        for (auto& p : g.photos) got.push_back(p.node->name);
        std::sort(got.begin(), got.end());
        std::vector<std::wstring> want = {L"beach - Copy.jpg", L"beach sideways.jpg", L"beach small.jpg", L"beach.png"};
        Check(got == want, L"the group holds the original, the small, the bright and the sideways copy");
        const Photo& keep = g.photos[0];
        uint64_t biggest640 = 0;
        for (auto& p : g.photos)
            if (p.hash.width == 640) biggest640 = std::max(biggest640, p.node->size);
        Check(keep.hash.width == 640 && keep.node->size == biggest640, L"suggested keeper: full resolution, then largest file (" + keep.node->name + L")");
        Check(g.maxDist <= t.dhash && g.Reclaimable() > 0, L"group distance and reclaimable bytes");
    }
    Check(cache.Size() == 10, L"hash cache holds every image, unreadable ones too (" + Num(static_cast<long long>(cache.Size())) + L")");

    PhotoProgress again;
    auto groups2 = FindSimilarPhotos(root.get(), 1, PhotoMatch::Normal, 4, again, &cache);
    Check(again.cached == 10 && groups2.size() == 1, L"second search uses the cache (" + Num(static_cast<long long>(again.cached)) + L" cached)");
    PhotoHashCache fromDisk(cacheFile);
    PhotoProgress third;
    auto groups3 = FindSimilarPhotos(root.get(), 1, PhotoMatch::Normal, 2, third, &fromDisk);
    Check(third.cached == 10 && groups3.size() == 1 && groups3[0].photos.size() == 4, L"the cache file is read back in a new session");

    // A changed file is decoded again.
    {
        Img changed = Pattern(640, 480, 555);
        Save(wic.Get(), dir + L"\\album\\forest.png", changed, false);
        auto root2 = Scan(dir);
        PhotoProgress p4;
        FindSimilarPhotos(root2.get(), 1, PhotoMatch::Normal, 2, p4, &fromDisk);
        Check(p4.cached == 9, L"a changed image is hashed again (" + Num(static_cast<long long>(p4.cached)) + L" of 10 cached)");
    }

    PhotoProgress strict;
    auto gs = FindSimilarPhotos(root.get(), 1, PhotoMatch::Strict, 2, strict, nullptr);
    size_t strictCount = gs.empty() ? 0 : gs[0].photos.size();
    PhotoProgress loose;
    auto gl = FindSimilarPhotos(root.get(), 1, PhotoMatch::Loose, 2, loose, nullptr);
    size_t looseCount = gl.empty() ? 0 : gl[0].photos.size();
    Check(strictCount <= 4 && looseCount >= 4 && gl.size() == 1, L"Strict groups no more, Loose no fewer (" + Num(static_cast<long long>(strictCount)) + L" / " +
                                                                     Num(static_cast<long long>(looseCount)) + L")");

    PhotoProgress big;
    auto none = FindSimilarPhotos(root.get(), 10ull << 20, PhotoMatch::Normal, 2, big, nullptr);
    Check(big.filesTotal == 0 && none.empty(), L"minimum size leaves out small files");
    PhotoProgress cancelled;
    cancelled.cancel = true;
    Check(FindSimilarPhotos(root.get(), 1, PhotoMatch::Normal, 2, cancelled, nullptr).empty(), L"cancelled search returns nothing");
}

static void TestKeeper() {
    Node a, b, c, d;
    a.name = L"IMG_0001.jpg";
    b.name = L"IMG_0001 (2).jpg";
    c.name = L"IMG_0001.jpg";
    d.name = L"x.jpg";
    a.size = b.size = 1000;
    a.mtime = b.mtime = 50;
    c.size = 5000;
    c.mtime = 10;
    d.size = 900;
    Photo pa{&a, {}}, pb{&b, {}}, pc{&c, {}}, pd{&d, {}};
    pa.hash.width = pb.hash.width = 4000;
    pa.hash.height = pb.hash.height = 3000;
    pc.hash.width = 2000;
    pc.hash.height = 1500;
    pd.hash.width = 4000;
    pd.hash.height = 3000;
    Check(PhotoBetter(pa, pc) && !PhotoBetter(pc, pa), L"keeper: more pixels beat a larger file");
    Check(PhotoBetter(pa, pd), L"keeper: same pixels, larger file wins");
    Check(PhotoBetter(pa, pb) && !PhotoBetter(pb, pa), L"keeper: same everything, the name without \"(2)\" wins");
    b.mtime = 40;
    Check(PhotoBetter(pb, pa), L"keeper: same pixels and size, the older file wins");
}

// A ~ B ~ C with A and C far apart (a chain, like video frames): the group is built around the
// keeper A, so C is not suggested for deletion as a "copy" of A.
static void TestChain() {
    Node a, b, c, d;
    a.name = L"a.jpg";
    b.name = L"b.jpg";
    c.name = L"c.jpg";
    d.name = L"d.jpg";
    a.size = b.size = c.size = d.size = 1000;
    std::vector<Photo> ps(4);
    Node* nodes[] = {&a, &b, &c, &d};
    uint64_t dh[] = {0, 0xFull, 0xFFull, 0xFFFFull << 40};  // b is 4 bits from a and c; c is 8 from a
    for (int i = 0; i < 4; ++i) {
        ps[i].node = nodes[i];
        ps[i].hash.ok = true;
        ps[i].hash.dhash = dh[i];
        ps[i].hash.phash = 0x123456789ull;
        ps[i].hash.width = i == 0 ? 4000 : 1000;  // a is the keeper
        ps[i].hash.height = 1000;
    }
    PhotoThreshold t = ThresholdFor(PhotoMatch::Normal);
    auto groups = GroupPhotos(ps, t);
    bool ok = groups.size() == 1 && groups[0].photos.size() == 2 && groups[0].photos[0].node == &a && groups[0].photos[1].node == &b &&
              groups[0].maxDist == 4;
    Check(ok, L"a chain of similar photos is split around the keeper (nothing far from it is suggested)");
    ps[0].hash.flat = ps[1].hash.flat = true;  // a and b would match, but are blank
    Check(GroupPhotos(ps, t).empty(), L"flat images are never grouped");
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestBasics();
    TestHashIndex();
    TestKeeper();
    TestChain();
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"winncdu-phash-test-" + std::to_wstring(GetCurrentProcessId());
    DeleteTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);
    TestImages(dir);
    DeleteTree(dir);
    Check(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES, L"fixtures removed");
    CoUninitialize();
    puts(g_fail ? "SOME CHECKS FAILED" : "ALL PASSED");
    return g_fail ? 1 : 0;
}
