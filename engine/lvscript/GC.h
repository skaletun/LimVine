/**
 * @file    GC.h
 * @brief   Автоматическое управление памятью LV Script: mark-and-sweep с
 *          побитовой трассировкой и бюджетом песочницы.
 * @ingroup LVScript
 *
 * @details Выбор GC вместо RC:
 *          1. Скрипты активно создают циклы (замыкания ↔ инстансы, графы диалогов,
 *             state-machine) — RC потребовал бы weak-ref дисциплины от гейм-дизайнера.
 *          2. Паузы GC предсказуемы: сборка запускается по порогу аллокаций и
 *             может быть отложена на «тихий» кадр (@c GC::setFrameBudget).
 *          3. Один аллокатор => полный контроль над лимитом памяти песочницы.
 *
 *          Архитектура допускает апгрейд до инкрементального tri-color GC:
 *          write-barrier уже объявлен (@c GC::writeBarrier), а @c grayStack готов
 *          к работе по шагам.
 */
#pragma once

#include "Value.h"

namespace lv {

class VM;

/**
 * @brief Сборщик мусора LV Script.
 *
 * Владеет всеми объектами кучи (интрузивный список @c ObjHeader::next) и всей
 * памятью под элементы массивов/map. Корни предоставляются VM через интерфейс
 * @c GCRootProvider.
 */
class GC {
public:
    /// Поставщик корней трассировки (реализует VM).
    struct RootProvider {
        virtual ~RootProvider() = default;
        /// Пометить все достижимые значения (стек, кадры, глобалы, регистри, интерн).
        virtual void markRoots(GC& gc) = 0;
    };

    explicit GC(RootProvider* roots = nullptr, std::size_t initialThreshold = 1 << 20);
    ~GC();

    GC(const GC&) = delete;
    GC& operator=(const GC&) = delete;

    // -- Аллокация ----------------------------------------------------------
    /**
     * @brief Выделить кучевой объект.
     * @tparam T Тип объекта; его конструктор вызывается placement-new'ом в @c allocate.
     * @param size Полный размер объекта в байтах (включая гибкие массивы).
     * @return Указатель на новый объект, зарегистрированный в GC.
     */
    template <class T, class... Args>
    T* allocate(std::size_t size, Args&&... args) {
        if (bytesAllocated_ + size > nextThreshold_ && !inCollection_)
            collect();
        void* mem = ::operator new(size);
        T* obj = new (mem) T(std::forward<Args>(args)...);
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += size;
        ++objectCount_;
        return obj;
    }

    /// Перевыделить буфер, принадлежащий GC-объекту (учитывается в bytesAllocated_).
    void* reallocate(void* ptr, std::size_t oldSize, std::size_t newSize);

    // -- Управление ---------------------------------------------------------
    void setRootProvider(RootProvider* p) noexcept { roots_ = p; }

    /// Полный цикл сборки. Возвращает число освобождённых объектов.
    std::size_t collect();

    /// Пометить значение как достижимое.
    void mark(Value v) noexcept;
    /// Пометить объект как достижимый.
    void markObject(ObjHeader* obj) noexcept;

    /// Write-barrier: вызывается при записи ссылочного значения в кучевой объект.
    /// В текущей (stop-the-world) реализации — no-op, оставлен для инкрементального GC.
    void writeBarrier(ObjHeader* /*owner*/, Value /*newValue*/) noexcept {}

    // -- Песочница / лимиты -------------------------------------------------
    /// Жёсткий лимит памяти скриптового мира (байт). Превышение => ScriptError.
    void   setMemoryLimit(std::size_t bytes) noexcept { memoryLimit_ = bytes; }
    [[nodiscard]] std::size_t memoryLimit()   const noexcept { return memoryLimit_; }
    [[nodiscard]] std::size_t bytesAllocated()const noexcept { return bytesAllocated_; }
    [[nodiscard]] std::size_t objectCount()   const noexcept { return objectCount_; }
    [[nodiscard]] std::size_t totalCollected()const noexcept { return totalCollected_; }
    [[nodiscard]] std::size_t collections()   const noexcept { return collections_; }

    /// «Бюджет кадра»: если задан, GC не стартует, пока не истечёт лимит —
    /// вместо этого выставляется флаг @c gcRequested, и хост сам решает, когда собрать.
    void setDeferred(bool d) noexcept { deferred_ = d; }
    [[nodiscard]] bool gcRequested() const noexcept { return gcRequested_; }
    void clearRequest() noexcept { gcRequested_ = false; }

    /// Коэффициент роста порога после каждой сборки (по умолчанию 2.0).
    void setGrowthFactor(double f) noexcept { growthFactor_ = f; }

    /// Запретить освобождение объекта навсегда (интернированные строки, системные классы).
    static void pin(ObjHeader* o) noexcept { o->pinned = true; }

    /// Статистика для редакторской панели Profiler.
    struct Stats {
        std::size_t objects       = 0;
        std::size_t bytes         = 0;
        std::size_t collections   = 0;
        std::size_t totalFreed    = 0;
        double      lastPauseMs   = 0.0;
    };
    [[nodiscard]] Stats stats() const noexcept;

    [[nodiscard]] ObjHeader* objects() const noexcept { return objects_; }

private:
    void markPhase();
    std::size_t sweepPhase();
    void traceObject(ObjHeader* obj);

    RootProvider* roots_ = nullptr;
    ObjHeader*    objects_ = nullptr;
    std::vector<ObjHeader*> grayStack_;

    std::size_t bytesAllocated_  = 0;
    std::size_t nextThreshold_   = 1 << 20;
    std::size_t memoryLimit_     = static_cast<std::size_t>(-1);
    std::size_t objectCount_     = 0;
    std::size_t collections_     = 0;
    std::size_t totalCollected_  = 0;
    double      growthFactor_    = 2.0;
    double      lastPauseMs_     = 0.0;
    bool        inCollection_    = false;
    bool        deferred_        = false;
    bool        gcRequested_     = false;
};

} // namespace lv
