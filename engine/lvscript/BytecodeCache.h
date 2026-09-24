/**
 * @file    BytecodeCache.h
 * @brief   Дисковый кэш скомпилированных модулей LV Script (.lvc).
 * @ingroup LVScript
 *
 * @details Назначение
 *          ----------
 *          Компиляция большого проекта при каждом старте редактора стоит
 *          заметного времени. Кэш пропускает лексер/парсер/компилятор, если
 *          рядом с исходником лежит актуальный `.lvc`:
 *
 * @code
 *   assets/scripts/player.lvs   ← исходник
 *   .lvcache/assets/scripts/player.lvc  ← кэш (версия + хэш + байткод)
 * @endcode
 *
 *          Кэш считается валидным, когда совпали ОБА условия:
 *            1. @c kBytecodeVersion в файле == версии движка;
 *            2. FNV-1a хэш текста исходника == хэшу, записанному в файл.
 *          Хэш содержимого (а не mtime) выбран намеренно: он не зависит от
 *          файловой системы, переживает `git checkout` и корректно работает
 *          в CI, где mtime у всех файлов одинаковый.
 *
 *          Кэш никогда не является источником истины: при любом сомнении
 *          (повреждённый файл, чужая версия, недоступный каталог) модуль
 *          просто компилируется заново. Ошибки записи кэша не считаются
 *          ошибками сборки.
 */
#pragma once

#include "Bytecode.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lv {

class VM;

/**
 * @brief Статистика кэша за сессию (для панели Profiler и вывода lvcook).
 */
struct BytecodeCacheStats {
    std::size_t hits = 0;        ///< Загружено из .lvc.
    std::size_t misses = 0;      ///< Скомпилировано заново (кэша не было).
    std::size_t stale = 0;       ///< Кэш был, но устарел (версия/хэш).
    std::size_t writes = 0;      ///< Успешно записано .lvc.
    std::size_t writeErrors = 0; ///< Не удалось записать (не фатально).

    [[nodiscard]] double hitRate() const noexcept {
        const std::size_t total = hits + misses + stale;
        return total ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    }
};

/**
 * @brief Дисковый кэш байткода.
 *
 * Класс не владеет VM: прототипы создаются в переданной VM и живут по её
 * правилам (GC). Экземпляр кэша дешёвый — держите его рядом с загрузчиком
 * ассетов или создавайте на время сборки.
 */
class BytecodeCache {
public:
    /**
     * @param cacheDir Корень кэша. Пустой путь отключает и чтение, и запись
     *                 (полезно для тестов и для `--no-cache` в CLI).
     */
    explicit BytecodeCache(std::filesystem::path cacheDir = ".lvcache");

    /// Путь .lvc, соответствующий исходнику.
    [[nodiscard]] std::filesystem::path pathFor(const std::filesystem::path& sourcePath) const;

    /**
     * @brief Попытаться загрузить модуль из кэша.
     *
     * @param vm          VM, в которой создаются прототипы.
     * @param sourcePath  Путь исходника (определяет имя файла кэша).
     * @param sourceText  Текст исходника — по нему считается хэш для проверки.
     * @return Модуль, если кэш существует, читается и актуален; иначе nullopt.
     */
    [[nodiscard]] std::optional<BytecodeModule> load(VM& vm,
                                                     const std::filesystem::path& sourcePath,
                                                     std::string_view sourceText);

    /**
     * @brief Записать скомпилированный модуль в кэш.
     *
     * Ошибки записи не бросают исключений: кэш — это оптимизация, а не
     * обязательная часть пайплайна.
     *
     * @return true, если файл записан.
     */
    bool store(VM& vm, const std::filesystem::path& sourcePath,
               std::string_view sourceText, const BytecodeModule& module);

    /// Удалить весь кэш (кнопка «Clear cache» в редакторе, `lvcook clean`).
    std::size_t clear();

    /// Включить/выключить кэш на лету, не пересоздавая объект.
    void setEnabled(bool on) noexcept { enabled_ = on; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_ && !cacheDir_.empty(); }

    [[nodiscard]] const BytecodeCacheStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = {}; }

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return cacheDir_; }

private:
    std::filesystem::path cacheDir_;
    bool enabled_ = true;
    BytecodeCacheStats stats_;
};

} // namespace lv
