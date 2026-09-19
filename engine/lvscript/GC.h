/**
 * @file    GC.h
 * @brief   Автоматическое управление памятью LV Script: инкрементальный
 *          трёхцветный mark-and-sweep с бюджетом шага и лимитом песочницы.
 * @ingroup LVScript
 *
 * @details Выбор GC вместо RC:
 *          1. Скрипты активно создают циклы (замыкания ↔ инстансы, графы диалогов,
 *             state-machine) — RC потребовал бы weak-ref дисциплины от гейм-дизайнера.
 *          2. Паузы GC предсказуемы: сборка идёт шагами с бюджетом в байтах.
 *          3. Один аллокатор => полный контроль над лимитом памяти песочницы.
 *
 *          Трёхцветная инварианта
 *          ----------------------
 *          - **белый** — объект ещё не посещён (кандидат на удаление);
 *          - **серый** — посещён, но его поля ещё не просмотрены (лежит в grayStack);
 *          - **чёрный** — посещён вместе со всеми полями.
 *
 *          Сборка разбита на фазы @c Phase, между которыми исполняется код
 *          скрипта. Чтобы мутации не «спрятали» живой объект за уже почерневшим
 *          владельцем, каждая запись ссылки в кучу проходит через
 *          @c writeBarrier(): чёрный владелец, получивший белую ссылку,
 *          возвращается в серые (dijkstra-барьер).
 *
 *          Режимы
 *          ------
 *          - @c setIncremental(false) — классический stop-the-world (по умолчанию
 *            для тестов и headless-инструментов: полностью детерминирован);
 *          - @c setIncremental(true) — сборка по шагам, вызывайте @c step()
 *            раз в кадр; максимальная пауза ограничена @c setStepBudget().
 *
 *          В обоих режимах @c collect() выполняет полный цикл немедленно.
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
        if (bytesAllocated_ + size > nextThreshold_ && !inCollection_) {
            if (incremental_) stepInternal(stepBudget_);   // порция работы вместо полной паузы
            else              collect();
        }
        void* mem = ::operator new(size);
        T* obj = new (mem) T(std::forward<Args>(args)...);
        // Вставка в ГОЛОВУ списка. Во время фазы Sweep курсор уже ушёл вперёд
        // от головы, поэтому свежий объект гарантированно не попадёт под нож
        // текущего прохода — его рассмотрит следующий цикл.
        obj->next = objects_;
        objects_ = obj;
        bytesAllocated_ += size;
        ++objectCount_;
        // Объект, родившийся во время сборки, обязан пережить текущий цикл:
        // корни уже просканированы, и ссылку на него мог получить кто угодно.
        //
        // В фазе Mark он помечается СЕРЫМ (marked + попадание в grayStack), а не
        // чёрным: поля только что созданного массива/инстанса заполняются сразу
        // после аллокации, и без последующего обхода его дети остались бы
        // белыми и были бы освобождены.
        //
        // В фазе Sweep обход уже закончен, поэтому достаточно пометки: объект
        // доживёт до конца цикла, а его флаг снимет следующая сборка.
        if (phase_ == Phase::Mark) { obj->marked = true; grayStack_.push_back(obj); }
        else if (phase_ == Phase::Sweep) { obj->marked = true; }
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

    /**
     * @brief Фаза инкрементального цикла.
     */
    enum class Phase : std::uint8_t {
        Idle,   ///< сборка не идёт
        Mark,   ///< обход графа достижимости порциями
        Sweep   ///< освобождение белых объектов порциями
    };

    /// Текущая фаза (для панели Profiler и тестов).
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    /// Включить инкрементальный режим. По умолчанию выключен (stop-the-world).
    void setIncremental(bool on) noexcept;
    [[nodiscard]] bool incremental() const noexcept { return incremental_; }

    /// Бюджет одного шага в «единицах работы» (примерно байт обработанных данных).
    void setStepBudget(std::size_t bytes) noexcept { stepBudget_ = bytes ? bytes : 1; }
    [[nodiscard]] std::size_t stepBudget() const noexcept { return stepBudget_; }

    /**
     * @brief Выполнить одну порцию работы сборщика.
     *
     * Вызывайте раз в кадр (фаза Late), когда включён инкрементальный режим.
     * Если цикл не начат, шаг стартует его при достижении порога аллокаций.
     *
     * @param budget Бюджет шага; 0 — использовать @c stepBudget().
     * @return true, если цикл сборки завершился на этом шаге.
     */
    bool step(std::size_t budget = 0);

    /**
     * @brief Write-barrier: запись ссылочного значения в кучевой объект.
     *
     * Поддерживает трёхцветную инварианту: если @p owner уже чёрный (просмотрен),
     * а записываемое значение — белое, владелец возвращается в серые, чтобы его
     * поля просмотрели заново. Без барьера объект, перевешенный из белого
     * контейнера в чёрный во время разметки, был бы ошибочно освобождён.
     *
     * Вне фазы Mark барьер бесплатен (одно сравнение).
     */
    void writeBarrier(ObjHeader* owner, Value newValue) noexcept {
        if (phase_ != Phase::Mark || !owner || !owner->marked) return;
        if (!newValue.isObject()) return;
        ObjHeader* target = newValue.asObject();
        if (!target || target->marked) return;
        // Владелец почернел раньше, чем получил эту ссылку -> перекрасить в серый.
        owner->marked = false;
        markObject(owner);
    }

    /// Вариант барьера для прямой записи указателя на объект.
    void writeBarrierObject(ObjHeader* owner, ObjHeader* target) noexcept {
        if (phase_ != Phase::Mark || !owner || !owner->marked) return;
        if (!target || target->marked) return;
        owner->marked = false;
        markObject(owner);
    }

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
        double      lastPauseMs   = 0.0;   ///< Длительность последнего ШАГА (или полного цикла).
        double      maxPauseMs    = 0.0;   ///< Худшая пауза за сессию — главный показатель плавности.
        std::size_t steps         = 0;     ///< Число инкрементальных шагов.
        Phase       phase         = Phase::Idle;
    };
    [[nodiscard]] Stats stats() const noexcept;

    [[nodiscard]] ObjHeader* objects() const noexcept { return objects_; }

private:
    void markRootsIntoGray();
    /// Обход серых объектов в пределах бюджета. @return true, если серых не осталось.
    bool markStep(std::size_t budget);
    /// Освобождение белых в пределах бюджета. @return true, если список пройден.
    bool sweepStep(std::size_t budget, std::size_t& freed);
    /// Общая точка входа шага (используется и из allocate()).
    bool stepInternal(std::size_t budget);
    void finishCycle();
    /// Стоимость обхода объекта в единицах бюджета.
    [[nodiscard]] static std::size_t workOf(const ObjHeader* obj) noexcept;
    void traceObject(ObjHeader* obj);
    void notePause(double ms) noexcept;

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
    double      maxPauseMs_      = 0.0;
    bool        inCollection_    = false;
    bool        deferred_        = false;
    bool        gcRequested_     = false;

    // -- Инкрементальное состояние -----------------------------------------
    Phase       phase_           = Phase::Idle;
    bool        incremental_     = false;
    std::size_t stepBudget_      = 64 * 1024;   ///< ~64 КБ работы за шаг
    std::size_t steps_           = 0;
    std::size_t cycleFreed_      = 0;           ///< Освобождено в текущем цикле.
    /// Курсор фазы Sweep: адрес указателя, в который пишется «следующий живой».
    ObjHeader** sweepCursor_     = nullptr;
};

} // namespace lv
