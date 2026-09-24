/**
 * @file    PhysicsBackend.h
 * @brief   Интерфейс физического бэкенда (встроенный симулятор или Jolt).
 * @ingroup Physics
 *
 * @details Раньше выбор реализации делался через `#ifdef LV_WITH_JOLT` прямо
 *          в теле каждого метода @c PhysicsWorld. Это плохо масштабируется:
 *          два пути исполнения переплетены в одном файле, и ни один из них
 *          нельзя протестировать в отрыве от другого.
 *
 *          Здесь выбор вынесен в один полиморфный объект, создаваемый
 *          фабрикой @c createPhysicsBackend(). Выигрыш:
 *
 *          - **Оба бэкенда проходят ОДИН И ТОТ ЖЕ набор тестов.** Тест физики
 *            параметризован бэкендом, поэтому расхождение в поведении Jolt и
 *            встроенного симулятора ловится сразу, а не в игре.
 *          - `PhysicsWorld` не содержит ни одного `#ifdef`: вся условная
 *            компиляция сосредоточена в фабрике и в JoltBackend.cpp.
 *          - Бэкенд можно подменить в рантайме (например, headless-сервер
 *            принудительно берёт детерминированный встроенный симулятор).
 *
 *          Интерфейс намеренно «толстый» (со свойствами тел), а не
 *          «умный»: трансляция ECS <-> физика, аккумулятор фиксированного шага
 *          и события контактов живут в @c PhysicsWorld и одинаковы для всех
 *          бэкендов — дублировать их в каждой реализации незачем.
 */
#pragma once

#include "../Physics.h"

#include <memory>
#include <vector>

namespace lv::physics {

/**
 * @brief Абстракция физического движка.
 *
 * Реализации: @c BuiltinBackend (всегда доступен, детерминированный) и
 * @c JoltBackend (под @c LV_WITH_JOLT).
 */
/**
 * @brief Проходит ли тело группы @p group через маску запроса @p mask.
 *
 * Единая семантика для ВСЕХ бэкендов: @p mask — это битовый набор групп,
 * то есть тело видно запросу, если взведён бит @c 1<<group. Значение по
 * умолчанию @c 0xFFFF пропускает все 16 групп.
 *
 * Раньше встроенный бэкенд сравнивал маску запроса с полем @c BodyDesc::mask
 * (которое описывает, с чем тело СТАЛКИВАЕТСЯ), и фильтр в raycast молча не
 * работал: у тел по умолчанию mask=0xFFFF, поэтому подходило любое.
 */
[[nodiscard]] inline bool groupAllowed(std::uint16_t group, std::uint16_t mask) noexcept {
    return group < 16 && (mask & (1u << group)) != 0;
}

/**
 * @brief Пригодна ли форма к созданию тела.
 *
 * Общая для всех бэкендов проверка: вырожденная форма должна быть отвергнута
 * на входе, а не приводить к NaN в солвере через сотню кадров или к разному
 * поведению у Jolt и встроенного симулятора.
 */
[[nodiscard]] inline bool isShapeValid(const ShapeDesc& s) noexcept {
    constexpr Real kMin = 1e-4f;
    switch (s.kind) {
        case ShapeKind::Box:
            return s.halfExtents.x > kMin && s.halfExtents.y > kMin && s.halfExtents.z > kMin;
        case ShapeKind::Sphere:
            return s.radius > kMin;
        case ShapeKind::Capsule:
        case ShapeKind::Cylinder:
            return s.radius > kMin && s.halfHeight > kMin;
        case ShapeKind::ConvexHull:
            return s.points.size() >= 4;   // меньше четырёх точек не задают объём
        case ShapeKind::TriangleMesh:
            return s.meshVerts.size() >= 3 && s.meshIndices.size() >= 3 &&
                   (s.meshIndices.size() % 3) == 0;
    }
    return false;
}

class IPhysicsBackend {
public:
    virtual ~IPhysicsBackend() = default;

    /// Человекочитаемое имя для логов и панели отладки: "builtin" | "jolt".
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    virtual bool init(std::uint32_t maxBodies) = 0;
    virtual void shutdown() = 0;

    // -- Тела ----------------------------------------------------------------
    [[nodiscard]] virtual BodyId createBody(const BodyDesc& desc) = 0;
    virtual void destroyBody(BodyId id) = 0;
    [[nodiscard]] virtual bool isValid(BodyId id) const noexcept = 0;
    [[nodiscard]] virtual std::size_t bodyCount() const noexcept = 0;

    // -- Состояние тела ------------------------------------------------------
    [[nodiscard]] virtual Vec3 position(BodyId id) const = 0;
    [[nodiscard]] virtual Quat rotation(BodyId id) const = 0;
    [[nodiscard]] virtual Vec3 linearVelocity(BodyId id) const = 0;
    [[nodiscard]] virtual BodyKind kindOf(BodyId id) const = 0;
    [[nodiscard]] virtual ecs::Entity entityOf(BodyId id) const = 0;
    [[nodiscard]] virtual AABB boundsOf(BodyId id) const = 0;

    virtual void setLinearVelocity(BodyId id, const Vec3& v) = 0;
    virtual void teleportBody(BodyId id, const Vec3& pos, const Quat& rot) = 0;
    virtual void setTransform(BodyId id, const Vec3& pos, const Quat& rot) = 0;
    virtual void setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) = 0;
    virtual void applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos) = 0;
    virtual void applyForce(BodyId id, const Vec3& force) = 0;

    // -- Симуляция -----------------------------------------------------------
    /// Один фиксированный подшаг. Аккумулятор времени — забота PhysicsWorld.
    virtual void step(float dt) = 0;
    virtual void setGravity(const Vec3& g) = 0;

    /// Контакты, накопленные за последний @c step (бэкенд очищает их сам).
    [[nodiscard]] virtual const std::vector<ContactEvent>& contacts() const noexcept = 0;

    // -- Запросы -------------------------------------------------------------
    [[nodiscard]] virtual RaycastHit raycast(const Ray& r, Real maxDistance,
                                             std::uint16_t mask) const = 0;
    [[nodiscard]] virtual std::vector<BodyId> overlapSphere(const Vec3& center, Real radius,
                                                            std::uint16_t mask) const = 0;

    /// Матрица коллизий движка: бэкенд обязан её учитывать.
    virtual void setCollisionMatrix(const CollisionMatrix* m) noexcept = 0;
};

/**
 * @brief Создать бэкенд.
 *
 * @param preferJolt  Просить Jolt. Если движок собран без @c LV_WITH_JOLT,
 *                    молча возвращается встроенный симулятор — игра запустится
 *                    и без физического SDK, пусть и с более простой физикой.
 */
[[nodiscard]] std::unique_ptr<IPhysicsBackend> createPhysicsBackend(bool preferJolt);

/// Собран ли движок с поддержкой Jolt (для логов, тестов и панели отладки).
[[nodiscard]] bool joltAvailable() noexcept;

} // namespace lv::physics
