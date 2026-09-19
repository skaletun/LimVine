/**
 * @file    AssetManager.cpp
 * @brief   Реализация конвейера ассетов и горячей перезагрузки.
 */
#include "AssetManager.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lv::asset {

const char* assetTypeName(AssetType t) noexcept {
    switch (t) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::Shader: return "Shader";
        case AssetType::Audio: return "Audio";
        case AssetType::Scene: return "Scene";
        case AssetType::Script: return "Script";
        case AssetType::Font: return "Font";
        case AssetType::Palette: return "Palette";
        case AssetType::Animation: return "Animation";
        default: return "Unknown";
    }
}

AssetType assetTypeFromExtension(std::string_view ext) noexcept {
    if (ext == ".lvmesh" || ext == ".obj" || ext == ".gltf" || ext == ".glb" || ext == ".fbx") return AssetType::Mesh;
    if (ext == ".png" || ext == ".tga" || ext == ".jpg" || ext == ".ktx" || ext == ".dds")     return AssetType::Texture;
    if (ext == ".lvmat" || ext == ".mat")                                                       return AssetType::Material;
    if (ext == ".glsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".spv")  return AssetType::Shader;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")                      return AssetType::Audio;
    if (ext == ".lvscene" || ext == ".scn")                                                     return AssetType::Scene;
    if (ext == ".lvs")                                                                          return AssetType::Script;
    if (ext == ".ttf" || ext == ".otf" || ext == ".lvfont")                                     return AssetType::Font;
    if (ext == ".lvpal" || ext == ".pal")                                                       return AssetType::Palette;
    if (ext == ".lvanim" || ext == ".anim")                                                     return AssetType::Animation;
    return AssetType::Unknown;
}

std::uint64_t AssetManager::fileMtimeMs(const std::filesystem::path& p) noexcept {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(t.time_since_epoch()).count());
}

std::uint64_t AssetManager::hashBytes(const void* data, std::size_t n) noexcept {
    // FNV-1a по байтам: дёшево и достаточно для детекции изменений.
    std::uint64_t h = 1469598103934665603ULL;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

bool AssetManager::readTextFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// ---------------------------------------------------------------------------
//  FileWatcher
// ---------------------------------------------------------------------------
void FileWatcher::watch(const std::filesystem::path& root, std::vector<std::string> extensions,
                        std::chrono::milliseconds interval) {
    root_ = root;
    exts_ = std::move(extensions);
    interval_ = interval;
    active_ = true;
    rescan();
}

void FileWatcher::stop() { active_ = false; snapshot_.clear(); }

void FileWatcher::rescan() {
    snapshot_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) return;
    for (auto it = std::filesystem::recursive_directory_iterator(root_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (!exts_.empty() && std::find(exts_.begin(), exts_.end(), ext) == exts_.end()) continue;
        snapshot_[it->path().string()] = AssetManager::fileMtimeMs(it->path());
    }
}

bool FileWatcher::poll(std::vector<std::filesystem::path>& changed) {
    changed.clear();
    if (!active_) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPoll_ < interval_) return false;
    lastPoll_ = now;

    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) return false;
    for (auto it = std::filesystem::recursive_directory_iterator(root_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (!exts_.empty() && std::find(exts_.begin(), exts_.end(), ext) == exts_.end()) continue;
        const std::string key = it->path().string();
        const std::uint64_t mtime = AssetManager::fileMtimeMs(it->path());
        auto found = snapshot_.find(key);
        if (found == snapshot_.end() || found->second != mtime) {
            snapshot_[key] = mtime;
            if (found != snapshot_.end()) changed.push_back(it->path());
        }
    }
    return !changed.empty();
}

// ---------------------------------------------------------------------------
//  AssetManager
// ---------------------------------------------------------------------------
AssetManager::AssetManager() = default;

AssetManager::~AssetManager() { releaseAll(); }

void AssetManager::setRoot(const std::filesystem::path& root) { root_ = root; }

void AssetManager::registerLoader(AssetType type, Loader loader, Disposer disposer) {
    loaders_[type] = std::move(loader);
    disposers_[type] = std::move(disposer);
}

AssetRecord* AssetManager::find(const std::string& path) {
    auto it = records_.find(path);
    return it == records_.end() ? nullptr : it->second.get();
}
const AssetRecord* AssetManager::find(const std::string& path) const {
    auto it = records_.find(path);
    return it == records_.end() ? nullptr : it->second.get();
}

AssetRecord* AssetManager::load(const std::string& path) {
    if (AssetRecord* existing = find(path)) {
        if (existing->state == LoadState::Ready) { ++existing->refCount; return existing; }
        if (existing->state == LoadState::Failed) return nullptr;
    }

    auto rec = std::make_unique<AssetRecord>();
    rec->path = path;
    rec->type = assetTypeFromExtension(std::filesystem::path(path).extension().string());
    const std::filesystem::path full = root_ / path;
    rec->mtime = fileMtimeMs(full);
    std::error_code ec;
    rec->bytes = std::filesystem::exists(full, ec) ? static_cast<std::size_t>(std::filesystem::file_size(full, ec)) : 0;

    auto it = loaders_.find(rec->type);
    if (it == loaders_.end()) {
        rec->state = LoadState::Failed;
        rec->lastError = std::string("no loader registered for ") + assetTypeName(rec->type);
        records_[path] = std::move(rec);
        return nullptr;
    }
    rec->state = LoadState::Loading;
    AssetRecord* raw = rec.get();
    records_[path] = std::move(rec);

    std::string err;
    raw->payload = it->second(full.string(), *raw, err);
    if (!raw->payload) {
        raw->state = LoadState::Failed;
        raw->lastError = err.empty() ? "loader returned null" : err;
        return nullptr;
    }
    auto disp = disposers_.find(raw->type);
    if (disp != disposers_.end()) raw->disposer = disp->second;
    raw->state = LoadState::Ready;
    raw->refCount = 1;
    return raw;
}

void AssetManager::loadAsync(const std::string& path, std::function<void(AssetRecord*)> onReady) {
    asyncQueue_.push_back(AsyncJob{path, std::move(onReady)});
}

void AssetManager::pumpAsync() {
    // В полной версии задачи уходят в JobSystem, а здесь обрабатываются
    // в главном потоке: контракт вызова (колбэк всегда в главном потоке)
    // сохраняется, поэтому переключение на настоящий параллелизм прозрачно.
    for (AsyncJob& job : asyncQueue_) {
        AssetRecord* rec = load(job.path);
        if (job.onReady) job.onReady(rec);
    }
    asyncQueue_.clear();
}

void AssetManager::release(const std::string& path) {
    auto it = records_.find(path);
    if (it == records_.end()) return;
    AssetRecord& rec = *it->second;
    if (rec.refCount > 0 && --rec.refCount > 0) return;
    if (rec.payload && rec.disposer) rec.disposer(rec.payload);
    rec.payload = nullptr;
    rec.state = LoadState::Unloaded;
    records_.erase(it);
}

void AssetManager::releaseAll() {
    for (auto& [path, rec] : records_) {
        if (rec->payload && rec->disposer) rec->disposer(rec->payload);
        rec->payload = nullptr;
    }
    records_.clear();
}

void AssetManager::addDependency(const std::string& asset, const std::string& dependsOn) {
    AssetRecord* a = find(asset);
    AssetRecord* d = find(dependsOn);
    if (a && std::find(a->dependencies.begin(), a->dependencies.end(), dependsOn) == a->dependencies.end())
        a->dependencies.push_back(dependsOn);
    if (d && std::find(d->dependents.begin(), d->dependents.end(), asset) == d->dependents.end())
        d->dependents.push_back(asset);
}

void AssetManager::enableHotReload(bool enabled, std::vector<std::string> extensions) {
    hotReload_ = enabled;
    if (!enabled) { watcher_.stop(); return; }
    if (extensions.empty()) extensions = {".lvs", ".glsl", ".vert", ".frag", ".lvmat", ".lvpal", ".lvscene"};
    watcher_.watch(root_, std::move(extensions));
}

std::size_t AssetManager::pollHotReload() {
    if (!hotReload_) return 0;
    std::vector<std::filesystem::path> changed;
    if (!watcher_.poll(changed)) return 0;

    std::size_t n = 0;
    for (const auto& p : changed) {
        std::error_code ec;
        const std::filesystem::path rel = std::filesystem::relative(p, root_, ec);
        const std::string key = ec ? p.string() : rel.generic_string();
        AssetRecord* rec = find(key);
        if (!rec) continue;                 // ассет не загружен — перезагружать нечего
        reload(*rec);
        ++n;
    }
    reloadCount_ += n;
    return n;
}

void AssetManager::reload(AssetRecord& rec) {
    auto it = loaders_.find(rec.type);
    if (it == loaders_.end()) return;

    const std::filesystem::path full = root_ / rec.path;
    std::string err;
    void* fresh = it->second(full.string(), rec, err);
    if (!fresh) { rec.lastError = err; return; }

    // Атомарная замена payload'а: старый освобождаем ПОСЛЕ установки нового,
    // чтобы подписчики не увидели nullptr.
    void* old = rec.payload;
    rec.payload = fresh;
    rec.state = LoadState::Ready;
    rec.mtime = fileMtimeMs(full);
    rec.lastError.clear();
    if (old && rec.disposer) rec.disposer(old);

    notifyReload(rec);

    // Каскад: перезагружаем зависимые ассеты (материалы при смене шейдера и т.д.).
    const std::vector<std::string> dependents = rec.dependents;
    for (const std::string& dep : dependents)
        if (AssetRecord* d = find(dep)) reload(*d);
}

void AssetManager::notifyReload(const AssetRecord& rec) {
    for (const ReloadCallback& cb : reloadCallbacks_) cb(rec.path, rec.type);
}

AssetManager::Stats AssetManager::stats() const noexcept {
    Stats s{};
    for (const auto& [path, rec] : records_) {
        ++s.records;
        s.bytes += rec->bytes;
        if (rec->state == LoadState::Ready) ++s.ready;
        if (rec->state == LoadState::Failed) ++s.failed;
    }
    s.reloads = reloadCount_;
    s.pendingAsync = asyncQueue_.size();
    return s;
}

std::vector<const AssetRecord*> AssetManager::all() const {
    std::vector<const AssetRecord*> out;
    out.reserve(records_.size());
    for (const auto& [path, rec] : records_) out.push_back(rec.get());
    std::sort(out.begin(), out.end(), [](const AssetRecord* a, const AssetRecord* b) { return a->path < b->path; });
    return out;
}

} // namespace lv::asset
