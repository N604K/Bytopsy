#pragma once

// Local AI models: the Hugging Face hub cache, Ollama and LM Studio. Lists every model (and every
// Hugging Face revision) with the space it alone frees, when it was downloaded and last opened,
// and deletes single models the way the tools themselves do (`huggingface-cli delete-cache`,
// `ollama rm`). Only model files are ever touched: chats, history, settings, sign-in tokens and
// Ollama's identity key live outside the folders this module may delete from.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

enum class AiStoreKind { HuggingFace, Ollama, LMStudio };

struct AiItem {
    std::wstring name;      // "facebook/musicgen-medium", "revision d3bd7b0", "llama3.1:8b", "lmstudio-community/Qwen3-8B-GGUF"
    std::wstring detail;    // "model · 2 revisions", "main", "llama · 8.0B · Q4_K_M", "2 files"
    std::wstring restore;   // how to get it back: "ollama pull llama3.1:8b", ...
    std::wstring location;  // the folder (or manifest) to show in Explorer
    uint64_t bytes = 0;     // what deleting this item alone frees
    int64_t added = 0;      // downloaded (FILETIME ticks, UTC)
    int64_t used = 0;       // last opened, if Windows records it (0: unknown)
    int parent = -1;        // Hugging Face revisions: index of their repository item
    bool unused = false;    // Hugging Face revision that no ref (main, a branch, a PR) points to
    std::vector<std::wstring> remove;  // files and folders to delete, in order
    std::vector<std::wstring> blobs;   // Ollama: layers this model uses (go when no other model needs them)
};

struct AiStore {
    AiStoreKind kind = AiStoreKind::HuggingFace;
    std::wstring name;                // "Hugging Face cache"
    std::wstring folder;              // shown to the user
    std::vector<std::wstring> roots;  // every path deleted must be inside one of these
    uint64_t bytes = 0;
    std::vector<AiItem> items;        // largest first; revisions right after their repository
};

// The stores found on this PC (environment variables such as HF_HOME, HF_HUB_CACHE, OLLAMA_MODELS
// and LM Studio's settings are honored).
std::vector<AiStore> FindAiModels();
AiStore ScanHuggingFace(const std::wstring& hubDir, const std::wstring& xetDir = L"");
AiStore ScanOllama(const std::wstring& modelsDir);
AiStore ScanLMStudio(const std::wstring& modelsDir);
// Whether Windows updates last-access times on this PC (else "last used" is unknown).
bool LastAccessTracked();

struct AiDeleteResult {
    uint64_t freed = 0;
    size_t deleted = 0;  // items
    std::vector<std::wstring> errors;
    std::vector<std::wstring> lockedBy;  // apps holding a model open (a running Ollama, LM Studio, Python)
};

// Deletes the chosen items of one store permanently (models download again), refusing any path
// outside the store's roots. A repository with all its revisions chosen goes as a whole; for a
// revision, the refs pointing to it go too; for Ollama, layers no remaining model uses go after the
// manifests. Every item is written to the deletion journal.
AiDeleteResult DeleteAiItems(const AiStore& store, const std::vector<size_t>& chosen, std::atomic<bool>& cancel);
