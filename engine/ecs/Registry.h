/**
 * @file    Registry.h
 * @brief   Архетипный ECS-реестр LimVine: хранение, view, параллельная итерация.
 * @ingroup ECS
 *
 * @details Архитектура хранения
 *          ---------------------
 *          Сущности группируются в **архетипы** — множества компонентов.
 *          Внутри архетипа каждый компонент лежит отдельным сплошным массивом
 *          (SoA), а список сущностей — параллельным массивом handle'ов:
 * @code
 *          Archetype[Transform | Health]
 *            entities : [E12, E7, E33, ...]      // плотный массив
 *            pools    : Transform[] Health[]     // индекс == «строка» сущности
 * @endcode
 *          Такой дизайн даёт:
 *          - **линейное сканирование** без пропусков (в отличие от sparse-set
 *            при итерации нескольких компонентов сразу);
 *          - предсказуемый prefetch: пул компонента — один непрерывный блок;
 *          - безопасный swap-remove: при удалении сущности последняя строка
 *            переносится на освободившееся место, и мы правим её @c record.row.
 *
 *          Структурные изменения (add/remove/destroy) перемещают сущность между
 *          архетипами, поэтому во время итерации они запрещены: для этого есть
 *          @c CommandBuffer, который применяется между фазами систем.
 */
#pragma once

#include "Component.h"
#include "Entity.h"
#include "../core/Math.h"

#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <unordered_map>
#include <stdexcept>

/// Маркер недостижимого кода (отдельная функция, чтобы не тянуть <cassert>).

#include <vector>

namespace lv::ecs {

/// Базовый компонент трансформации — есть у каждой видимой сущности.
/// Определён здесь, чтобы подсистемы (рендер, физика, скрипты) не тянули
/// друг друга ради одного типа.
struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1, 1, 1};

    [[nodiscard]] Mat4 matrix() const noexcept { return Mat4::trs(position, rotation, scale); }
    [[nodiscard]] Vec3 forward() const noexcept { return rotate(rotation, Vec3::forward()); }
    [[nodiscard]] Vec3 right()   const noexcept { return rotate(rotation, Vec3::right()); }
    [[nodiscard]] Vec3 up()      const noexcept { return rotate(rotation, Vec3::up()); }

    static constexpr std::string_view lv_component_name = "Transform";
};

using ArchetypeId = std::uint32_t;
inline constexpr ArchetypeId kInvalidArchetype = 0xFFFFFFFFu;

/**
 * @brief Блок памяти одного компонента внутри архетипа.
 *
 * Хранит «сырые» байты, но вызывает конструкторы/деструкторы через
 * функторы, записанные при создании пула. Это позволяет держать компоненты
 * с нетривиальными типами (std::vector, std::string) без виртуальных таблиц.
 */
class ComponentPool {
public:
    using Ctor      = void (*)(void* dst);
    using Dtor      = void (*)(void* dst);
    using CopyMove  = void (*)(void* dst, void* src);   ///< перемещение/копирование строки
    using DefaultInit = void (*)(void* dst);

    ComponentPool() = default;
    ComponentPool(TypeId id, std::size_t size, std::size_t align, Ctor c, Dtor d, CopyMove mv)
        : typeId(id), elemSize(size), alignment(align), ctor(c), dtor(d), moveElem(mv) {}

    /// Перестроить буфер под @p newRows строк (с сохранением данных).
    void reserveRows(std::size_t newRows);
    /// Освободить память.
    void clear();

    [[nodiscard]] void* rowPtr(std::size_t row) noexcept {
        return static_cast<std::byte*>(data) + row * elemSize;
    }
    template <class T> [[nodiscard]] T* at(std::size_t row) noexcept {
        return reinterpret_cast<T*>(rowPtr(row));
    }

    TypeId      typeId = 0;
    std::size_t elemSize = 0;
    std::size_t alignment = 1;
    Ctor        ctor = nullptr;
    Dtor        dtor = nullptr;
    CopyMove    moveElem = nullptr;
    void*       data = nullptr;
    std::size_t rows = 0;      ///< занятых строк
    std::size_t capacity = 0;  ///< выделено строк
};

/**
 * @brief Архетип: сигнатура + наборы компонентов + список сущностей.
 */
struct Archetype {
    ArchetypeId             id = kInvalidArchetype;
    Signature               signature;
    std::vector<Entity>     entities;   ///< Плотный массив сущностей (индекс == row).
    std::vector<ComponentPool> pools;   ///< По одному пулу на компонент сигнатуры.
    std::vector<TypeId>     typeOrder;  ///< Порядок пулов (для быстрого поиска).

    /// Индекс пула для TypeId или -1.
    [[nodiscard]] int poolIndexOf(TypeId t) const noexcept {
        for (std::size_t i = 0; i < typeOrder.size(); ++i)
            if (typeOrder[i] == t) return static_cast<int>(i);
        return -1;
    }
    [[nodiscard]] ComponentPool* pool(TypeId t) noexcept {
        const int i = poolIndexOf(t);
        return i < 0 ? nullptr : &pools[static_cast<std::size_t>(i)];
    }
    [[nodiscard]] std::size_t rowCount() const noexcept { return entities.size(); }
};

/**
 * @brief Внутренняя запись о сущности.
 */
struct EntityRecord {
    ArchetypeId archetype = kInvalidArchetype;
    std::uint32_t row = 0;
    std::uint32_t generation = 0;
    bool alive = false;
};

/**
 * @brief Реестр сущностей и компонентов.
 *
 * @thread_safety Итерация (view/each) потокобезопасна, если никакие структурные
 *                изменения не выполняются одновременно. Для отложенных изменений
 *                используйте @c CommandBuffer.
 */
class Registry {
public:
    Registry();
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;

    // -- Жизненный цикл сущностей -------------------------------------------
    /// Создать сущность (пустой архетип).
    [[nodiscard]] Entity create();
    /// Уничтожить сущность. Повторный вызов — no-op.
    void destroy(Entity e);
    /// Жива ли сущность (проверяет и индекс, и поколение).
    [[nodiscard]] bool alive(Entity e) const noexcept;
    /// Число живых сущностей.
    [[nodiscard]] std::size_t entityCount() const noexcept { return aliveCount_; }

    // -- Компоненты ----------------------------------------------------------
    /**
     * @brief Добавить компонент (или перезаписать существующий).
     * @return Ссылка на компонент в пуле архетипа.
     *
     * @warning Вызов выполняет структурное изменение: все активные итераторы
     *          становятся недействительными.
     */
    /// @return Указатель на компонент или @c nullptr, если сущность мертва.
    template <Component T>
    T* add(Entity e, const T& value = T{});

    /// Добавить, если ещё нет; вернуть текущее значение.
    template <Component T>
    T* emplace(Entity e, const T& value = T{}) {
        if (!has<T>(e)) return add<T>(e, value);
        return get<T>(e);
    }

    /// Удалить компонент. Возвращает true, если компонент был.
    template <Component T>
    bool remove(Entity e);

    /// Есть ли компонент у сущности.
    template <Component T>
    [[nodiscard]] bool has(Entity e) const noexcept {
        return hasTypeId(e, typeIdOf<T>());
    }

    /// Доступ к компоненту или nullptr.
    template <Component T>
    [[nodiscard]] T* get(Entity e) noexcept {
        return static_cast<T*>(getTypeId(e, typeIdOf<T>()));
    }
    template <Component T>
    [[nodiscard]] const T* get(Entity e) const noexcept {
        return static_cast<const T*>(const_cast<Registry*>(this)->getTypeId(e, typeIdOf<T>()));
    }

    /// Доступ с проверкой: бросает @c std::out_of_range, если компонента нет.
    template <Component T>
    [[nodiscard]] T& require(Entity e);

    // -- Запросы -------------------------------------------------------------
    /**
     * @brief Обойти все сущности, имеющие ВСЕ перечисленные компоненты.
     *
     * Сигнатура колбэка: `(Entity, Ts&...)`. Возврат @c false из колбэка
     * прерывает обход (используется редактором для early-out).
     */
    template <Component... Ts, class Fn>
    void each(Fn&& fn);

    /// То же, но по частям (чанкам) — для распараллеливания Job System.
    template <Component... Ts, class Fn>
    void eachChunk(std::size_t chunkSize, Fn&& fn);

    /// Собрать сущности, подходящие под запрос (для скриптов и редактора).
    template <Component... Ts>
    [[nodiscard]] std::vector<Entity> collect();

    /// Есть ли у сущности все компоненты из списка TypeId (динамический путь).
    [[nodiscard]] bool hasAll(Entity e, std::span<const TypeId> ids) const noexcept;

    // -- Динамический (runtime) доступ — нужен LV Script и редактору ----------
    [[nodiscard]] bool hasTypeId(Entity e, TypeId t) const noexcept;
    [[nodiscard]] void* getTypeId(Entity e, TypeId t) noexcept;
    /// Добавить компонент по TypeId, используя фабрику из @c ComponentMetaRegistry.
    void addDynamic(Entity e, TypeId t);
    void removeDynamic(Entity e, TypeId t);
    /// Размер/выравнивание/конструкторы для TypeId (из метаданных).
    [[nodiscard]] const class ComponentMeta* metaOf(TypeId t) const noexcept;

    // -- Интроспекция ---------------------------------------------------------
    [[nodiscard]] std::size_t archetypeCount() const noexcept { return archetypes_.size(); }
    [[nodiscard]] const Archetype& archetype(ArchetypeId id) const noexcept { return *archetypes_[id]; }
    [[nodiscard]] ArchetypeId archetypeOf(Entity e) const noexcept;
    /// Все живые сущности (для редакторского Hierarchy).
    [[nodiscard]] std::vector<Entity> allEntities() const;
    /// Сбросить всё (используется при выгрузке сцены).
    void clear();

    /// Статистика для профайлера.
    struct Stats {
        std::size_t entities = 0;
        std::size_t archetypes = 0;
        std::size_t bytes = 0;
        std::size_t migrations = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    Archetype& archOf(Entity e) noexcept;
    const Archetype& archOf(Entity e) const noexcept;
    ArchetypeId findOrCreateArchetype(const Signature& sig);
    std::uint32_t allocSlot();
    void freeSlot(std::uint32_t index);
    void moveEntityToArchetype(Entity e, ArchetypeId target);
    static void removeRow(Archetype& arch, std::size_t row, Registry& reg);

    std::vector<EntityRecord>                 records_;
    std::vector<std::uint32_t>                freeList_;
    std::vector<std::unique_ptr<Archetype>>   archetypes_;
    std::unordered_map<Signature, ArchetypeId, SignatureHash> archetypeIndex_;
    std::size_t aliveCount_ = 0;
    std::size_t migrations_ = 0;
};

// ---------------------------------------------------------------------------
//  Метаданные компонентов (для динамического доступа из скриптов/редактора)
// ---------------------------------------------------------------------------

/**
 * @brief Описание типа компонента, зарегистрированного в движке.
 *
 * Нужно, потому что LV Script и инспектор работают с компонентами по имени
 * и TypeId, не зная C++-типа на этапе компиляции.
 */
struct ComponentMeta {
    /// TypeId 0 — ДОПУСТИМЫЙ идентификатор (первый зарегистрированный тип),
    /// поэтому валидность определяется отдельным флагом, а не значением id.
    bool               valid = false;
    TypeId             id = 0;
    std::string_view   name;
    std::size_t        size = 0;
    std::size_t        alignment = 1;
    ComponentPool::Ctor     ctor = nullptr;
    ComponentPool::Dtor     dtor = nullptr;
    ComponentPool::CopyMove move = nullptr;
    /// Присваивание в УЖЕ сконструированный объект.
    /// Нужно @c CommandBuffer: компонент сначала конструируется в пуле архетипа,
    /// затем в него переносится значение из отложенной команды.
    ComponentPool::CopyMove assign = nullptr;
    /// Сериализация в текст (для инспектора и .lvscene-файлов).
    std::string (*inspect)(const void* obj) = nullptr;
};

/**
 * @brief Глобальный реестр метаданных компонентов.
 *
 * Регистрация происходит автоматически при первом обращении к
 * @c ComponentMetaRegistry::instance().reg<T>(), поэтому биндинги движка
 * просто вызывают @c registerComponent<T>() в своих модулях.
 */
class ComponentMetaRegistry {
public:
    static ComponentMetaRegistry& instance();

    template <Component T>
    const ComponentMeta& reg(std::string_view name = {}) {
        const TypeId id = typeIdOf<T>();
        ensureSize(id);
        ComponentMeta& m = metas_[id];
        if (m.valid) return m;   // уже зарегистрирован
        m.valid = true;
        m.id = id;
        m.name = name.empty() ? typeNameOf<T>() : name;
        m.size = sizeof(T);
        m.alignment = alignof(T);
        m.ctor = [](void* dst) { new (dst) T(); };
        m.dtor = [](void* dst) { static_cast<T*>(dst)->~T(); };
        m.move = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); };
        m.assign = [](void* dst, void* src) { *static_cast<T*>(dst) = std::move(*static_cast<T*>(src)); };
        return m;
    }

    [[nodiscard]] const ComponentMeta* find(TypeId id) const noexcept {
        return id < metas_.size() && metas_[id].valid ? &metas_[id] : nullptr;
    }
    [[nodiscard]] const ComponentMeta* findByName(std::string_view name) const noexcept {
        for (const ComponentMeta& m : metas_) if (m.valid && m.name == name) return &m;
        return nullptr;
    }
    [[nodiscard]] const std::vector<ComponentMeta>& all() const noexcept { return metas_; }

private:
    void ensureSize(TypeId id) {
        if (id >= metas_.size()) metas_.resize(id + 1);
    }
    std::vector<ComponentMeta> metas_;
};

/// Удобная свободная функция регистрации.
template <Component T>
inline const ComponentMeta& registerComponent(std::string_view name = {}) {
    return ComponentMetaRegistry::instance().reg<T>(name);
}

// ---------------------------------------------------------------------------
//  Шаблоны Registry
// ---------------------------------------------------------------------------

inline Archetype& Registry::archOf(Entity e) noexcept {
    return *archetypes_[records_[e.index].archetype];
}
inline const Archetype& Registry::archOf(Entity e) const noexcept {
    return *archetypes_[records_[e.index].archetype];
}

inline ArchetypeId Registry::archetypeOf(Entity e) const noexcept {
    if (e.index >= records_.size() || !records_[e.index].alive) return kInvalidArchetype;
    return records_[e.index].archetype;
}

template <Component T>
T* Registry::add(Entity e, const T& value) {
    ComponentMetaRegistry::instance().reg<T>();
    if (!alive(e)) return nullptr;

    const TypeId tid = typeIdOf<T>();
    if (records_[e.index].archetype != kInvalidArchetype) {
        Archetype& src = *archetypes_[records_[e.index].archetype];
        if (src.signature.test(tid)) {
            // Компонент уже есть — перезаписываем без миграции архетипа.
            T* dst = src.pool(tid)->at<T>(records_[e.index].row);
            *dst = value;
            return dst;
        }
        Signature sig = src.signature;
        sig.set(tid);
        moveEntityToArchetype(e, findOrCreateArchetype(sig));
    } else {
        Signature sig;
        sig.set(tid);
        moveEntityToArchetype(e, findOrCreateArchetype(sig));
    }

    EntityRecord& rec = records_[e.index];
    Archetype& dst = *archetypes_[rec.archetype];
    T* out = dst.pool(tid)->at<T>(rec.row);
    *out = value;
    return out;
}

template <Component T>
bool Registry::remove(Entity e) {
    if (!alive(e)) return false;
    const TypeId tid = typeIdOf<T>();
    EntityRecord& rec = records_[e.index];
    Archetype& src = *archetypes_[rec.archetype];
    if (!src.signature.test(tid)) return false;

    Signature sig = src.signature;
    sig.clear(tid);
    const ArchetypeId target = findOrCreateArchetype(sig);
    moveEntityToArchetype(e, target);
    return true;
}

template <Component T>
T& Registry::require(Entity e) {
    T* p = get<T>(e);
    if (!p) throw std::out_of_range(std::string("entity has no component ") + std::string(typeNameOf<T>()));
    return *p;
}

/// Раскрытие вызова `fn(entity, pools[I]->at<Ts>(row)...)` без нарушения
/// порядка вычисления (см. комментарий в Registry::each).
template <class Fn, Component... Ts, std::size_t... I>
[[nodiscard]] inline decltype(auto) expandCall(Fn& fn, Entity e, ComponentPool* const* pools,
                                              std::size_t row, std::index_sequence<I...>) {
    return fn(e, *pools[I]->template at<Ts>(row)...);
}

template <Component... Ts, class Fn>
void Registry::each(Fn&& fn) {
    static_assert(sizeof...(Ts) > 0, "each<> requires at least one component type");
    (ComponentMetaRegistry::instance().reg<Ts>(), ...);

    Signature want;
    (want.set(typeIdOf<Ts>()), ...);

    using Result = decltype(fn(std::declval<Entity>(), std::declval<Ts&>()...));
    constexpr bool canStop = std::is_convertible_v<Result, bool>;

    for (auto& archPtr : archetypes_) {
        Archetype& arch = *archPtr;
        if (!arch.signature.contains(want)) continue;

        // Распаковка параметр-пака в массив указателей на пулы: i-й элемент
        // соответствует i-му типу из Ts..., что позволяет дальше раскрыть их
        // в том же порядке. Индексы генерируются через index_sequence, а не
        // через `i++` внутри пакета: пост-инкремент в pack expansion — это
        // нарушение sequencing (GCC честно предупреждает -Wsequence-point).
        ComponentPool* pools[sizeof...(Ts)] = {arch.pool(typeIdOf<Ts>())...};
        const std::size_t rows = arch.rowCount();
        for (std::size_t row = 0; row < rows; ++row) {
            if constexpr (canStop) {
                const bool keepGoing = expandCall<Fn, Ts...>(fn, arch.entities[row], pools, row,
                                                             std::index_sequence_for<Ts...>{});
                if (!keepGoing) return;
            } else {
                expandCall<Fn, Ts...>(fn, arch.entities[row], pools, row,
                                      std::index_sequence_for<Ts...>{});
            }
        }
    }
}

template <Component... Ts, class Fn>
void Registry::eachChunk(std::size_t chunkSize, Fn&& fn) {
    static_assert(sizeof...(Ts) > 0, "eachChunk<> requires at least one component type");
    (ComponentMetaRegistry::instance().reg<Ts>(), ...);

    Signature want;
    (want.set(typeIdOf<Ts>()), ...);
    if (chunkSize == 0) chunkSize = 64;

    for (auto& archPtr : archetypes_) {
        Archetype& arch = *archPtr;
        if (!arch.signature.contains(want)) continue;
        const std::size_t rows = arch.rowCount();
        for (std::size_t begin = 0; begin < rows; begin += chunkSize) {
            const std::size_t end = std::min(rows, begin + chunkSize);
            fn(arch, begin, end);
        }
    }
}

template <Component... Ts>
std::vector<Entity> Registry::collect() {
    std::vector<Entity> out;
    each<Ts...>([&](Entity e, auto&...) { out.push_back(e); return true; });
    return out;
}

} // namespace lv::ecs
