// Self-check of AI model listing and removal: winncdu-aimodels-test.exe
// Builds fake Hugging Face, Ollama and LM Studio stores in %TEMP% and checks what is listed, what
// each item frees, and that deleting touches exactly the model's files (never chats, keys, tokens).
#include <windows.h>

#include <cstdio>
#include <string>

#include "aimodels.h"
#include "fsops.h"
#include "util.h"

static int g_fail = 0;

static void Check(bool ok, const std::wstring& what) {
    if (!ok) ++g_fail;
    std::string u = util::WideToUtf8(std::wstring(ok ? L"ok   " : L"FAIL ") + what + L"\n");
    fputs(u.c_str(), stdout);
}

static void Put(const std::wstring& path, size_t bytes, const std::string& text = "") {
    for (size_t i = path.find(L'\\', 3); i != std::wstring::npos; i = path.find(L'\\', i + 1)) CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        std::string data = text.empty() ? std::string(bytes, 'x') : text;
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }
}

static const AiItem* Item(const AiStore& s, const std::wstring& name, size_t* index = nullptr) {
    for (size_t i = 0; i < s.items.size(); ++i)
        if (s.items[i].name == name) {
            if (index) *index = i;
            return &s.items[i];
        }
    return nullptr;
}

static std::wstring Names(const AiStore& s) {
    std::wstring out;
    for (auto& it : s.items) out += (it.parent >= 0 ? L"  " : L"") + it.name + L"=" + std::to_wstring(it.bytes) + L" ";
    return out;
}

static std::string Hex(char c) { return std::string(64, c); }

int main() {
    wchar_t b[MAX_PATH];
    std::wstring tmp(b, GetTempPathW(MAX_PATH, b));
    std::wstring dir = tmp + L"winncdu-aimodels-test-" + std::to_wstring(GetCurrentProcessId());
    std::wstring journal = dir + L"-journal.log";
    SetEnvironmentVariableW(L"WINNCDU_JOURNAL", journal.c_str());
    std::atomic<bool> cancel{false};

    // ---- Hugging Face: one model with main, a PR and an unreferenced revision; a dataset
    std::wstring hf = dir + L"\\huggingface", hub = hf + L"\\hub", repo = hub + L"\\models--org--model";
    Put(hf + L"\\token", 0, "hf_secret");
    Put(repo + L"\\refs\\main", 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    Put(repo + L"\\refs\\refs\\pr\\1", 0, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    Put(repo + L"\\snapshots\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\model.bin", 100);
    Put(repo + L"\\snapshots\\bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\model.safetensors", 200);
    Put(repo + L"\\snapshots\\cccccccccccccccccccccccccccccccccccccccc\\old.bin", 50);
    Put(repo + L"\\.no_exist\\cccccccccccccccccccccccccccccccccccccccc\\x.json", 0);
    // Symlinked snapshots (as on Linux, or Windows with Developer Mode): a blob shared by two
    // revisions, and one used by the unreferenced revision only.
    Put(repo + L"\\blobs\\h1shared", 300);
    Put(repo + L"\\blobs\\h2only", 70);
    bool links = CreateSymbolicLinkW((repo + L"\\snapshots\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\shared.bin").c_str(),
                                     (repo + L"\\blobs\\h1shared").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) &&
                 CreateSymbolicLinkW((repo + L"\\snapshots\\cccccccccccccccccccccccccccccccccccccccc\\shared.bin").c_str(),
                                     (repo + L"\\blobs\\h1shared").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) &&
                 CreateSymbolicLinkW((repo + L"\\snapshots\\cccccccccccccccccccccccccccccccccccccccc\\only.bin").c_str(),
                                     (repo + L"\\blobs\\h2only").c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    Put(hub + L"\\datasets--user--data\\refs\\main", 0, "dddddddddddddddddddddddddddddddddddddddd");
    Put(hub + L"\\datasets--user--data\\snapshots\\dddddddddddddddddddddddddddddddddddddddd\\train.parquet", 40);
    Put(hf + L"\\xet\\chunk-cache\\c1", 30);

    AiStore h = ScanHuggingFace(hub, hf + L"\\xet");
    size_t iA = 0, iB = 0, iC = 0, iRepo = 0;
    const AiItem* r = Item(h, L"org/model", &iRepo);
    const AiItem* ra = Item(h, L"revision aaaaaaa", &iA);
    const AiItem* rb = Item(h, L"revision bbbbbbb", &iB);
    const AiItem* rc = Item(h, L"revision ccccccc", &iC);
    Check(r && ra && rb && rc && Item(h, L"user/data") && Item(h, L"Xet download cache"), L"HF: repos, revisions, xet: " + Names(h));
    Check(r && r->detail == L"model  ·  3 revisions" && ra && ra->detail == L"main" && rb && rb->detail == L"pr/1" && rc && rc->unused &&
              !ra->unused,
          L"HF: refs named, the unreferenced revision marked unused");
    Check(ra && rb && ra->parent == static_cast<int>(iRepo) && rb->parent == static_cast<int>(iRepo), L"HF: revisions follow their repository");
    Check(rb && rb->restore.find(L" --revision refs/pr/1") != std::wstring::npos &&
              rc && rc->restore.find(L"--revision cccccccc") != std::wstring::npos,
          L"HF: a PR revision downloads again with --revision refs/pr/N: " + (rb ? rb->restore : L""));
    if (links)
        Check(rc && rc->bytes == 50 + 70 && ra && ra->bytes == 100, L"HF: a revision frees its own blobs, not shared ones (" +
                                                                         std::to_wstring(rc ? rc->bytes : 0) + L")");
    else
        std::fputs("skip symlinked snapshots (no symlink rights)\n", stdout);
    Check(Item(h, L"user/data")->restore == L"huggingface-cli download user/data --repo-type dataset", L"HF: how to get a dataset back");

    AiDeleteResult d = DeleteAiItems(h, {iC}, cancel);
    Check(d.errors.empty() && !PathExists(repo + L"\\snapshots\\cccccccccccccccccccccccccccccccccccccccc") &&
              PathExists(repo + L"\\snapshots\\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") && PathExists(repo + L"\\refs\\main") &&
              !PathExists(repo + L"\\.no_exist\\cccccccccccccccccccccccccccccccccccccccc"),
          L"HF: deleting an unused revision removes only it");
    if (links)
        Check(!PathExists(repo + L"\\blobs\\h2only") && PathExists(repo + L"\\blobs\\h1shared"), L"HF: its own blob goes, the shared one stays");
    h = ScanHuggingFace(hub, hf + L"\\xet");
    Item(h, L"revision bbbbbbb", &iB);
    d = DeleteAiItems(h, {iB}, cancel);
    Check(!PathExists(repo + L"\\snapshots\\bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") && !PathExists(repo + L"\\refs\\refs\\pr\\1") &&
              PathExists(repo + L"\\refs\\main"),
          L"HF: deleting a PR revision removes the ref pointing to it");
    h = ScanHuggingFace(hub, hf + L"\\xet");
    std::vector<size_t> all;
    for (size_t i = 0; i < h.items.size(); ++i) all.push_back(i);
    d = DeleteAiItems(h, all, cancel);
    Check(!PathExists(repo) && !PathExists(hub + L"\\datasets--user--data") && PathExists(hf + L"\\xet") && !PathExists(hf + L"\\xet\\chunk-cache"),
          L"HF: everything selected empties the cache folders");
    Check(PathExists(hf + L"\\token"), L"HF: the sign-in token next to the cache is never touched");

    // ---- Ollama: two tags of one model (same layers), and a model sharing one layer with them
    std::wstring ol = dir + L"\\.ollama", models = ol + L"\\models", man = models + L"\\manifests\\registry.ollama.ai\\library";
    Put(ol + L"\\id_ed25519", 0, "key");
    Put(ol + L"\\history", 0, "prompts");
    auto blob = [&](char c, size_t n, const std::string& text = "") { Put(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex(c)), n, text); };
    auto manifest = [&](const std::wstring& path, char config, std::initializer_list<char> layers) {
        std::string j = "{\"schemaVersion\":2,\"config\":{\"mediaType\":\"application/vnd.docker.container.image.v1+json\",\"digest\":\"sha256:" +
                        Hex(config) + "\",\"size\":10},\"layers\":[";
        for (char l : layers) j += std::string(j.back() == '[' ? "" : ",") + "{\"digest\":\"sha256:" + Hex(l) + "\",\"size\":1}";
        Put(path, 0, j + "]}");
    };
    blob('c', 0, "{\"model_format\":\"gguf\",\"model_family\":\"llama\",\"model_type\":\"8.0B\",\"file_type\":\"Q4_K_M\"}");
    blob('a', 1000);  // llama weights
    blob('e', 20);    // a license layer both models share
    blob('d', 0, "{\"model_family\":\"phi3\",\"model_type\":\"3.8B\",\"file_type\":\"Q4_0\"}");
    blob('f', 400);  // phi weights
    blob('9', 5);    // not used by any model (a leftover): never touched
    manifest(man + L"\\llama3\\8b", 'c', {'a', 'e'});
    manifest(man + L"\\llama3\\latest", 'c', {'a', 'e'});
    manifest(man + L"\\phi3\\mini", 'd', {'f', 'e'});
    manifest(models + L"\\manifests\\hf.co\\user\\repo-GGUF\\Q4", 'd', {'f'});
    AiStore o = ScanOllama(models);
    size_t i8b = 0, iLatest = 0, iPhi = 0;
    const AiItem* l8 = Item(o, L"llama3:8b", &i8b);
    Check(l8 && Item(o, L"llama3:latest", &iLatest) && Item(o, L"phi3:mini", &iPhi) && Item(o, L"hf.co/user/repo-GGUF:Q4"),
          L"Ollama: names as `ollama list` shows them: " + Names(o));
    Check(l8 && l8->detail.rfind(L"llama  ·  8.0B  ·  Q4_K_M", 0) == 0 && l8->bytes == 0, L"Ollama: details from the config; two tags of one model free nothing alone: " +
                                                                                           (l8 ? l8->detail : L""));
    Check(l8 && l8->restore == L"ollama pull llama3:8b", L"Ollama: how to get it back");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    uint64_t expected = 1000;  // the weights, the config and both manifests
    for (const std::wstring& p : {models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('c')), man + L"\\llama3\\8b", man + L"\\llama3\\latest"})
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) expected += fa.nFileSizeLow;
    d = DeleteAiItems(o, {i8b, iLatest}, cancel);
    Check(d.errors.empty() && !PathExists(man + L"\\llama3") && !PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('a'))) &&
              !PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('c'))) &&
              PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('e'))),
          L"Ollama: both tags gone with their layers; the layer phi3 still uses stays");
    Check(d.freed == expected, L"Ollama: freed what was removed (" + std::to_wstring(d.freed) + L" of " + std::to_wstring(expected) + L")");
    o = ScanOllama(models);
    Item(o, L"phi3:mini", &iPhi);
    DeleteAiItems(o, {iPhi}, cancel);
    Check(PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('f'))) && !PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('e'))),
          L"Ollama: a layer another model (hf.co) uses stays; the now unused one goes");
    Check(PathExists(models + L"\\blobs\\sha256-" + util::Utf8ToWide(Hex('9'))) && PathExists(ol + L"\\id_ed25519") && PathExists(ol + L"\\history"),
          L"Ollama: unrelated blobs, the identity key and the prompt history are never touched");

    // ---- LM Studio
    std::wstring lm = dir + L"\\.lmstudio", lmm = lm + L"\\models";
    Put(lm + L"\\conversations\\chat.json", 0, "{}");
    Put(lmm + L"\\lmstudio-community\\Qwen3-8B-GGUF\\Qwen3-8B-Q4_K_M.gguf", 500);
    Put(lmm + L"\\lmstudio-community\\Qwen3-8B-GGUF\\Qwen3-8B-Q8_0.gguf", 100);
    Put(lmm + L"\\bartowski\\phi-4-GGUF\\phi-4-Q4.gguf", 50);
    AiStore l = ScanLMStudio(lmm);
    size_t iQwen = 0;
    const AiItem* q = Item(l, L"lmstudio-community/Qwen3-8B-GGUF", &iQwen);
    Check(l.items.size() == 2 && q && q->bytes == 600 && q->detail == L"Qwen3-8B-Q4_K_M.gguf and 1 more" && iQwen == 0,
          L"LM Studio: one item per downloaded model, largest first: " + Names(l));
    DeleteAiItems(l, {iQwen}, cancel);
    Check(!PathExists(lmm + L"\\lmstudio-community\\Qwen3-8B-GGUF") && PathExists(lmm + L"\\bartowski\\phi-4-GGUF") &&
              PathExists(lm + L"\\conversations\\chat.json"),
          L"LM Studio: only that model goes; chats stay");

    // ---- The guard: nothing outside a store's folders, whatever an item says
    AiStore evil = ScanLMStudio(lmm);
    if (!evil.items.empty()) {
        evil.items[0].remove = {lm + L"\\conversations", lmm + L"\\..\\conversations\\chat.json"};
        AiDeleteResult e = DeleteAiItems(evil, {0}, cancel);
        Check(e.errors.size() == 2 && PathExists(lm + L"\\conversations\\chat.json"), L"paths outside the model folders are refused");
    }

    // ---- Every deletion is journaled with how to get the model back
    std::string log;
    FILE* jf = nullptr;
    if (_wfopen_s(&jf, journal.c_str(), L"rb") == 0 && jf) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), jf)) > 0) log.append(buf, n);
        fclose(jf);
    }
    Check(log.find("get it back: ollama pull llama3:8b") != std::string::npos && log.find("Hugging Face cache: revision ccccccc") != std::string::npos,
          L"journal entries name the model and how to get it back");

    std::atomic<uint64_t> removed{0};
    std::wstring err;
    DeletePermanently(dir, cancel, removed, err);
    DeleteFileW(journal.c_str());
    printf("\n%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
