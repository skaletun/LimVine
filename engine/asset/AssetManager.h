/**
 * @file    AssetManager.h
 * @brief   Конвейер ассетов: загрузка, кэш, зависимости, горячая перезагрузка.
 * @ingroup Asset
 *
 * @details Конвейер
 *          -------
 * @code
 *   assets/models/tree.fbx  ──import──▶  cache/tree.lvmesh  ──load──▶  MeshHandle
 *   assets/shaders/lit.glsl ──watch───▶  (перекомпиляция на лету)
 *   scripts/player.lvs      ──watch───▶  (перекомпиляция + патч прототипов)
 * @endcode
 *
 *          - **Импорт** выполняется офлайн (@c tools/lvcook) или лениво в
 *            редакторе; рантайм читает только готовые бинарные ассеты.
 *          - **Кэш по (путь, mtime, хэш импортера)**: если исходник не менялся,
 *            повторный импорт не выполняется.
 *          - **Hot reload**: файловый монитор сравнивает mtime раз в N мс и
 *            публикует событие. Шейдеры и .lvs перезагружаются без перезапуска
 *            игры — это ключевая особенность рабочего процесса движка.
 *          - **Зависимости**: материал ссылается на шейдер и палитру; при
 *            изменении шейдера перезагружаются и зависимые материалы.
 */
#pragma once

#include "../core/Math.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lv::asset {

/// Тип ассета (определяет импортер и загрузчик).
enum class AssetType : std::uint8_t {
    Unknown, Mesh, Texture, Material, Shader, Audio, Scene, Script, Font, Palette, Animation
};

[[nodiscard]] const char* assetTypeName(AssetType t) noexcept;
[[nodiscard]] AssetType assetTypeFromExtension(std::string_view ext) noexcept;

/// Состояние загрузки.
enum class LoadState : std::uint8_t { Unloaded, Loading, Ready, Failed };

/**
 * @brief Запись реестра ассетов.
 */
struct AssetRecord {
    std::string     path;            ///< Путь относительно корня ассетов.
    AssetType       type = AssetType::Unknown;
    LoadState       state = LoadState::Unloaded;
    std::uint64_t   mtime = 0;       ///< Время изменения файла (мс).
    std::uint64_t   hash = 0;        ///< Хэш содержимого (для кэша импорта).
    std::size_t     bytes = 0;
    std::size_t     refCount = 0;
    std::vector<std::string> dependencies;
    std::vector<std::string> dependents;   ///< Обратные связи для hot reload.
    void*           payload = nullptr;     ///< Загруженный объект (владеет AssetManager).
    std::function<void(void*)> disposer;   ///< Освободитель payload.
    std::string     lastError;
};

/**
 * @brief Наблюдатель файловой системы (polling-based: переносимо и без inotify).
 */
class FileWatcher {
public:
    /// Начать наблюдение за каталогом (рекурсивно) с указанными расширениями.
    void watch(const std::filesystem::path& root, std::vector<std::string> extensions,
               std::chrono::milliseconds interval = std::chrono::milliseconds(400));
    void stop();
    /// Опросить изменения. Вызывается из главного цикла.
    /// @param changed Выходной список изменённых файлов.
    /// @return true, если список не пуст.
    bool poll(std::vector<std::filesystem::path>& changed);

private:
    std::filesystem::path root_;
    std::vector<std::string> exts_;
    std::unordered_map<std::string, std::uint64_t> snapshot_;
    std::chrono::milliseconds interval_{400};
    std::chrono::steady_clock::time_point lastPoll_{};
    bool active_ = false;
    void rescan();
};

/**
 * @brief Менеджер ассетов.
 */
class AssetManager {
public:
    AssetManager();
    ~AssetManager();

    /// Задать корень ассетов (каталог `assets` проекта).
    void setRoot(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    /// Зарегистрировать загрузчик для типа ассета.
    /// @p loader получает путь и возвращает payload (или nullptr при ошибке).
    using Loader = std::function<void*(const std::string& path, AssetRecord& rec, std::string& err)>;
    using Disposer = std::function<void(void*)>;
    void registerLoader(AssetType type, Loader loader, Disposer disposer);

    /// Синхронная загрузка (блокирующая). Возвращает запись или nullptr.
    AssetRecord* load(const std::string& path);
    /// Асинхронная загрузка: ставит задачу в очередь, результат придёт в @c pumpAsync.
    void loadAsync(const std::string& path, std::function<void(AssetRecord*)> onReady = {});
    /// Обработать готовые асинхронные задачи (вызывается в главном потоке).
    void pumpAsync();

    /// Найти запись (без загрузки).
    [[nodiscard]] AssetRecord* find(const std::string& path);
    [[nodiscard]] const AssetRecord* find(const std::string& path) const;
    /// Полезная нагрузка с приведением типа.
    template <class T> [[nodiscard]] T* getAs(const std::string& path) {
        AssetRecord* r = load(path);
        return r ? static_cast<T*>(r->payload) : nullptr;
    }

    void release(const std::string& path);
    void releaseAll();

    /// Зарегистрировать зависимость (для каскадного hot reload).
    void addDependency(const std::string& asset, const std::string& dependsOn);

    // -- Горячая перезагрузка -------------------------------------------------
    void enableHotReload(bool enabled, std::vector<std::string> extensions = {});
    /// Опросить watcher и перезагрузить изменившиеся ассеты.
    /// @return Число перезагруженных ассетов.
    std::size_t pollHotReload();

    /// Подписаться на событие перезагрузки (редактор, LV Script VM, шейдерный кэш).
    using ReloadCallback = std::function<void(const std::string& path, AssetType type)>;
    void onReload(ReloadCallback cb) { reloadCallbacks_.push_back(std::move(cb)); }

    // -- Статистика -----------------------------------------------------------
    struct Stats {
        std::size_t records = 0;
        std::size_t ready = 0;
        std::size_t failed = 0;
        std::size_t bytes = 0;
        std::size_t reloads = 0;
        std::size_t pendingAsync = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;
    [[nodiscard]] std::vector<const AssetRecord*> all() const;

    /// Прочитать файл как текст (утилита для скриптов/шейдеров).
    [[nodiscard]] static bool readTextFile(const std::filesystem::path& p, std::string& out);
    [[nodiscard]] static std::uint64_t fileMtimeMs(const std::filesystem::path& p) noexcept;
    [[nodiscard]] static std::uint64_t hashBytes(const void* data, std::size_t n) noexcept;

private:
    void reload(AssetRecord& rec);
    void notifyReload(const AssetRecord& rec);

    std::filesystem::path root_;
    std::unordered_map<std::string, std::unique_ptr<AssetRecord>> records_;
    std::unordered_map<AssetType, Loader> loaders_;
    std::unordered_map<AssetType, Disposer> disposers_;
    std::vector<ReloadCallback> reloadCallbacks_;
    FileWatcher watcher_;
    bool hotReload_ = false;
    std::size_t reloadCount_ = 0;
    struct AsyncJob { std::string path; std::function<void(AssetRecord*)> onReady; };
    std::vector<AsyncJob> asyncQueue_;
    std::vector<AsyncJob> asyncDone_;
};

} // namespace lv::asset
