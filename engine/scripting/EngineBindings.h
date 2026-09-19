/**
 * @file    EngineBindings.h
 * @brief   Биндинги движка для LV Script: ECS, физика, ввод, звук, рендер.
 * @ingroup Scripting
 *
 * @details Как устроена «бесшовная» привязка
 *          --------------------------------
 *          1. **Единое представление сущности.** `Entity` в скрипте — это
 *             инстанс скриптового класса с двумя числовыми полями (`index`,
 *             `generation`), поэтому handle'ы сериализуемы, сравниваемы и
 *             переживают перезагрузку скрипта.
 *          2. **Реестр компонентов вместо ручной обёртки на каждый тип.**
 *             @c registerComponentBinding<T>() регистрирует имя, TypeId и
 *             таблицу полей. LV Script получает `entity.get("Health")`,
 *             `entity.set("Health", "current", 50)` и типизированные шорткаты
 *             (`entity.health.current`) для «горячих» компонентов.
 *          3. **Поля marshaling'ом через Marshal<T>.** Скаляры и строки
 *             конвертируются базовыми специализациями, типы движка (Vec3,
 *             Color, Entity) — специализациями ниже. Добавление нового типа
 *             движка в скрипты = одна специализация шаблона.
 *          4. **Capability-проверки.** Сторонний мод без @c Cap_Spawn не сможет
 *             создавать сущности: проверка выполняется в самом биндинге, а не
 *             в игровом коде.
 *
 *          Всё регистрируется одной функцией @c installEngineBindings(vm, ctx).
 */
#pragma once

#include "../asset/AssetManager.h"
#include "../audio/Audio.h"
#include "../ecs/World.h"
#include "../input/Input.h"
#include "../lvscript/Scheduler.h"
#include "../lvscript/Stdlib.h"
#include "../lvscript/VM.h"
#include "../physics/Physics.h"
#include "../render/Renderer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lv::scripting {

/**
 * @brief Описание одного поля компонента, доступного из скриптов.
 */
struct FieldBinding {
    std::string name;
    enum class Kind : std::uint8_t { Float, Int, Bool, Vec3, Vec2, Quat, Color, Entity, String } kind = Kind::Float;
    std::size_t offset = 0;     ///< Смещение внутри структуры компонента.
    bool readOnly = false;
};

/**
 * @brief Описание компонента, доступного из LV Script.
 */
struct ComponentBinding {
    std::string name;                    ///< Имя в скрипте: "Health", "Transform".
    lv::ecs::TypeId typeId = 0;
    std::size_t size = 0;
    std::vector<FieldBinding> fields;
    lv::ecs::ComponentPool::Ctor ctor = nullptr;
    lv::ecs::ComponentPool::Dtor dtor = nullptr;

    [[nodiscard]] const FieldBinding* field(std::string_view n) const {
        for (const FieldBinding& f : fields) if (f.name == n) return &f;
        return nullptr;
    }
};

/**
 * @brief Контекст движка, к которому привязан скриптовый мир.
 *
 * Один контекст на @c ScriptWorld. Подсистемы могут отсутствовать (headless
 * тесты логики работают без рендера и звука).
 */
struct EngineContext {
    ecs::World*          world = nullptr;
    render::Renderer*    renderer = nullptr;
    physics::PhysicsWorld* physics = nullptr;
    input::InputSystem*  input = nullptr;
    audio::AudioSystem*  audio = nullptr;
    asset::AssetManager* assets = nullptr;
};

/**
 * @brief Реестр биндингов компонентов.
 */
class BindingRegistry {
public:
    static BindingRegistry& instance();

    /// Зарегистрировать компонент и его поля.
    template <ecs::Component T>
    void registerComponent(std::string_view scriptName) {
        ComponentBinding b;
        b.name = scriptName;
        b.typeId = ecs::typeIdOf<T>();
        b.size = sizeof(T);
        b.ctor = [](void* dst) { new (dst) T(); };
        b.dtor = [](void* dst) { static_cast<T*>(dst)->~T(); };
        bindings_[b.typeId] = std::move(b);
        byName_[std::string(scriptName)] = b.typeId;
        ecs::ComponentMetaRegistry::instance().reg<T>(scriptName);
    }

    /// Добавить поле ранее зарегистрированного компонента.
    template <ecs::Component T, class M>
    void field(std::string_view componentName, std::string_view fieldName, M T::*member,
               FieldBinding::Kind kind, bool readOnly = false) {
        auto it = byName_.find(std::string(componentName));
        if (it == byName_.end()) return;
        ComponentBinding& b = bindings_[it->second];
        FieldBinding f;
        f.name = fieldName;
        f.kind = kind;
        // Смещение поля вычисляется на «нулевом» объекте — стандартный приём
        // offsetof для не-POD типов.
        f.offset = reinterpret_cast<std::size_t>(&(reinterpret_cast<T*>(0)->*member));
        f.readOnly = readOnly;
        b.fields.push_back(f);
    }

    [[nodiscard]] const ComponentBinding* byTypeId(ecs::TypeId id) const {
        auto it = bindings_.find(id);
        return it == bindings_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const ComponentBinding* byName(std::string_view name) const {
        auto it = byName_.find(std::string(name));
        return it == byName_.end() ? nullptr : byTypeId(it->second);
    }
    [[nodiscard]] const std::unordered_map<ecs::TypeId, ComponentBinding>& all() const noexcept { return bindings_; }

private:
    std::unordered_map<ecs::TypeId, ComponentBinding> bindings_;
    std::unordered_map<std::string, ecs::TypeId> byName_;
};

/**
 * @brief Скриптовый мир: VM + планировщик + привязка к движку.
 *
 * Движок создаёт один @c ScriptWorld для игровой логики и (опционально) второй
 * с урезанными capability для модов.
 */
class ScriptWorld {
public:
    explicit ScriptWorld(lv::SandboxConfig sandbox = {});
    ~ScriptWorld();

    /// Привязать подсистемы движка и установить стандартную библиотеку + биндинги.
    void attach(const EngineContext& ctx);

    [[nodiscard]] lv::VM& vm() noexcept { return vm_; }
    [[nodiscard]] lv::Scheduler& scheduler() noexcept { return scheduler_; }
    [[nodiscard]] const EngineContext& engine() const noexcept { return ctx_; }

    /// Загрузить и выполнить .lvs файл (или модуль stdlib по имени).
    bool runFile(const std::string& path, const std::string& source);

    /// Вызвать скриптовый колбэк (например, onCollision) у всех сущностей,
    /// у которых есть компонент @c ScriptBehaviour с таким методом.
    void dispatch(std::string_view method, std::span<const lv::Value> args = {});

    /// Кадр: tick планировщика + dispatch Update.
    void tick(float dt);

    /// Горячая перезагрузка скрипта: перекомпилировать и подменить прототипы,
    /// сохранив состояние инстансов.
    bool hotReload(const std::string& path, const std::string& newSource);

    /// Создать «Entity»-значение для скриптов.
    [[nodiscard]] lv::Value wrapEntity(ecs::Entity e);
    /// Распаковать «Entity»-значение обратно.
    [[nodiscard]] static ecs::Entity unwrapEntity(const lv::Value& v);

    /// Лог консоли (редактор подписывается на него).
    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback cb);

    [[nodiscard]] const std::vector<std::string>& loadedScripts() const noexcept { return loaded_; }

    /// Прототип модуля по имени (нужен дизассемблеру и горячей перезагрузке).
    [[nodiscard]] lv::ObjFunction* prototype(const std::string& path) const noexcept {
        const auto it = protos_.find(path);
        return it == protos_.end() ? nullptr : it->second;
    }

private:
    void installBindings();

    lv::VM vm_;
    lv::Scheduler scheduler_;
    EngineContext ctx_;
    LogCallback logCb_;
    std::vector<std::string> loaded_;
    std::unordered_map<std::string, lv::ObjFunction*> protos_;
};

/// Установить все биндинги движка в VM (вызывается из @c ScriptWorld::attach).
void installEngineBindings(lv::VM& vm, EngineContext& ctx);

/// Зарегистрировать стандартный набор компонентов движка в BindingRegistry.
void registerEngineComponents();

// ---------------------------------------------------------------------------
//  Представление математических типов в LV Script
// ---------------------------------------------------------------------------
/**
 * @brief Vec3/Vec2/Quat/Color передаются в скрипты как Map с полями x/y/z/w.
 *
 * Почему map, а не скриптовый класс:
 *  - отсутствие аллокации класса и диспетчеризации методов на каждый вектор
 *    (в кадре их тысячи);
 *  - литерал `{"x": 1, "y": 0, "z": 0}` читается естественно;
 *  - сериализация в .lvscene тривиальна.
 *
 * Арифметика делается нативными функциями `vec3Add`, `vec3Mul` и т.д., поэтому
 * интерпретатор не выполняет побайтовых операций над map'ами.
 */
[[nodiscard]] lv::Value vec3ToValue(lv::VM& vm, const Vec3& v);
[[nodiscard]] Vec3      vec3FromValue(const lv::Value& v);
[[nodiscard]] lv::Value quatToValue(lv::VM& vm, const Quat& q);
[[nodiscard]] Quat      quatFromValue(const lv::Value& v);
[[nodiscard]] lv::Value colorToValue(lv::VM& vm, const Color& c);
[[nodiscard]] Color     colorFromValue(const lv::Value& v);

/// Прочитать/записать поле компонента по @c FieldBinding (marshaling по offset).
[[nodiscard]] lv::Value readField(lv::VM& vm, const void* componentBase, const FieldBinding& f);
bool                      writeField(const void* componentBase, const FieldBinding& f, const lv::Value& v);

} // namespace lv::scripting
