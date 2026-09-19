/**
 * @file    Physics.h
 * @brief   Обёртка над Jolt Physics с ECS-интеграцией.
 * @ingroup Physics
 *
 * @details Выбор Jolt, а не Bullet:
 *          - детерминированная симуляция при фиксированном шаге (важно для
 *            воспроизведения модов и сетевой синхронизации);
 *          - multicore solver из коробки — согласуется с Job System движка;
 *          - активное сопровождение и современная C++-кодовая база.
 *
 *          Обёртка спроектирована так, чтобы @c LV_WITH_JOLT можно было
 *          выключить: тогда используется встроенный простой симулятор
 *          (сферы/бокс-коллизии + raycast по AABB), на котором работают тесты,
 *          CI и headless-сервер.
 */
#pragma once

#include "../core/Math.h"
#include "../ecs/World.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lv::physics {

using BodyId = std::uint32_t;
inline constexpr BodyId kInvalidBody = 0xFFFFFFFFu;

enum class ShapeKind : std::uint8_t { Box, Sphere, Capsule, Cylinder, ConvexHull, TriangleMesh };
enum class BodyKind  : std::uint8_t { Static, Kinematic, Dynamic };

/// Описание формы (параметры зависят от @c kind).
struct ShapeDesc {
    ShapeKind kind = ShapeKind::Box;
    Vec3      halfExtents{0.5f, 0.5f, 0.5f};   ///< Box
    Real      radius = 0.5f;                    ///< Sphere / Capsule / Cylinder
    Real      halfHeight = 0.5f;                ///< Capsule / Cylinder
    std::vector<Vec3> points;                   ///< ConvexHull
    std::vector<Vec3> meshVerts;                ///< TriangleMesh
    std::vector<std::uint32_t> meshIndices;
};

/// Параметры тела.
struct BodyDesc {
    ShapeDesc shape;
    BodyKind  kind = BodyKind::Dynamic;
    Vec3      position;
    Quat      rotation;
    Real      mass = 1.0f;
    Real      friction = 0.4f;
    Real      restitution = 0.0f;
    Real      linearDamping = 0.05f;
    Real      angularDamping = 0.15f;
    std::uint16_t group = 0;    ///< Collision group / mask (см. CollisionMatrix).
    std::uint16_t mask = 0xFFFF;
    ecs::Entity entity = ecs::kNullEntity;
    bool        trigger = false;  ///< Не толкает другие тела, только генерирует события.
};

/// Результат raycast.
struct RaycastHit {
    bool      hit = false;
    Vec3      position;
    Vec3      normal;
    Real      distance = 0;
    BodyId    body = kInvalidBody;
    ecs::Entity entity = ecs::kNullEntity;
};

/// Событие столкновения (доставляется в LV Script через `onCollision`).
struct ContactEvent {
    ecs::Entity a;
    ecs::Entity b;
    Vec3  position;
    Vec3  normal;
    Real  impulse = 0;
    bool  began = true;   ///< false => тела расстались
};

/**
 * @brief Матрица коллизий: какие группы сталкиваются друг с другом.
 *
 * Хранится как битовая маска 16x16 — одна проверка вместо перебора правил.
 */
class CollisionMatrix {
public:
    void setCollides(std::uint16_t a, std::uint16_t b, bool value) noexcept {
        if (a < 16 && b < 16) {
            if (value) { bits_[a] |= (1u << b); bits_[b] |= (1u << a); }
            else       { bits_[a] &= ~(1u << b); bits_[b] &= ~(1u << a); }
        }
    }
    [[nodiscard]] bool collides(std::uint16_t a, std::uint16_t b) const noexcept {
        return a < 16 && b < 16 && (bits_[a] & (1u << b)) != 0;
    }
private:
    std::uint32_t bits_[16] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                               0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                               0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                               0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
};

/**
 * @brief Физический мир.
 */
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    bool init(std::uint32_t maxBodies = 8192);
    void shutdown();

    /// Шаг симуляции с фиксированным dt и аккумулятором (см. step()).
    void step(ecs::World& world, float deltaTime);

    /// Тела.
    [[nodiscard]] BodyId createBody(const BodyDesc& desc);
    void destroyBody(BodyId id);
    [[nodiscard]] bool isValid(BodyId id) const noexcept;

    void setLinearVelocity(BodyId id, const Vec3& v);

    /**
     * @brief Телепорт тела: позиция/поворот + обнуление скорости + пересчёт AABB.
     *
     * Обязателен для ДИНАМИЧЕСКИХ тел: `syncFromECS()` считает источником
     * истины тело (а не Transform), поэтому простая запись в Transform из
     * скрипта была бы перезаписана обратно на следующем кадре. Вызывается из
     * биндинга `setPosition()`/`translate()`, если у сущности есть RigidBody.
     */
    void teleportBody(BodyId id, const Vec3& pos, const Quat& rot);
    [[nodiscard]] Vec3 linearVelocity(BodyId id) const;
    void applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos);
    void applyForce(BodyId id, const Vec3& force);
    void setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot);
    void setTransform(BodyId id, const Vec3& pos, const Quat& rot);

    /// Запросы.
    [[nodiscard]] RaycastHit raycast(const Ray& r, Real maxDistance, std::uint16_t mask = 0xFFFF) const;
    [[nodiscard]] std::vector<BodyId> overlapSphere(const Vec3& center, Real radius, std::uint16_t mask = 0xFFFF) const;

    /// Гравитация.
    void setGravity(const Vec3& g) noexcept { gravity_ = g; }
    [[nodiscard]] const Vec3& gravity() const noexcept { return gravity_; }

    /// Матрица коллизий.
    [[nodiscard]] CollisionMatrix& collisionMatrix() noexcept { return matrix_; }

    /// События контакта за последний шаг (очищаются в начале следующего).
    [[nodiscard]] const std::vector<ContactEvent>& contacts() const noexcept { return contacts_; }

    /// Фиксированный шаг (по умолчанию 60 Гц).
    void setFixedStep(float dt) noexcept { fixedStep_ = dt; }
    [[nodiscard]] float fixedStep() const noexcept { return fixedStep_; }
    /// Максимум подшагов за кадр (защита от «спирали смерти»).
    void setMaxSubSteps(std::uint32_t v) noexcept { maxSubSteps_ = v; }

    /// Статистика.
    [[nodiscard]] std::size_t bodyCount() const noexcept;
    [[nodiscard]] bool usesJolt() const noexcept { return joltActive_; }

    /// Отладочная геометрия (линии) — для гизмо в редакторе.
    void collectDebugLines(std::vector<Vec3>& outLines, const Color& color) const;

private:
    void syncFromECS(ecs::World& world);
    void syncToECS(ecs::World& world);
    void integrate(float dt);

    struct Body;
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<BodyId> freeList_;
    Vec3 gravity_{0, -9.81f, 0};
    CollisionMatrix matrix_;
    std::vector<ContactEvent> contacts_;
    float fixedStep_ = 1.0f / 60.0f;
    float accumulator_ = 0.f;
    std::uint32_t maxSubSteps_ = 5;
    bool joltActive_ = false;
    bool initialized_ = false;
};

/**
 * @brief ECS-компонент «у этого тела есть физика».
 */
struct RigidBody {
    physics::BodyId body = kInvalidBody;
    bool syncTransformToECS = true;   ///< Писать позицию из физики в Transform.
    static constexpr std::string_view lv_component_name = "RigidBody";
};

/// Компонент-коллайдер без тела (статическая геометрия уровня).
struct Collider {
    physics::BodyId body = kInvalidBody;
    bool trigger = false;
    static constexpr std::string_view lv_component_name = "Collider";
};

/// Зарегистрировать физику в мире движка (создаёт систему в Phase::Physics).
void attachToWorld(PhysicsWorld& physics, ecs::World& world);

} // namespace lv::physics
