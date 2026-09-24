/**
 * @file    JoltBackend.h
 * @brief   Физический бэкенд на Jolt Physics.
 * @ingroup Physics
 *
 * @note Заголовок безопасно включать всегда: без @c LV_WITH_JOLT класс просто
 *       не объявляется, а фабрика вернёт встроенный симулятор. Типы Jolt сюда
 *       не просачиваются — только forward-декларации, поэтому остальной движок
 *       не зависит от заголовков SDK.
 */
#pragma once

#include "PhysicsBackend.h"

#if defined(LV_WITH_JOLT)

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace JPH {
class PhysicsSystem;
class TempAllocator;
class JobSystem;
class Shape;
struct ShapeDesc;
} // namespace JPH

namespace lv::physics {

/**
 * @brief Полноценная физика на Jolt 5.x.
 *
 * Отображает наш стабильный @c BodyId на @c JPH::BodyID, транслирует формы,
 * применяет игровую @c CollisionMatrix через ContactListener и собирает
 * контакты потокобезопасно (solver Jolt многопоточный).
 */
class JoltBackend final : public IPhysicsBackend {
public:
    JoltBackend();
    ~JoltBackend() override;

    [[nodiscard]] const char* name() const noexcept override;

    bool init(std::uint32_t maxBodies) override;
    void shutdown() override;

    [[nodiscard]] BodyId createBody(const BodyDesc& desc) override;
    void destroyBody(BodyId id) override;
    [[nodiscard]] bool isValid(BodyId id) const noexcept override;
    [[nodiscard]] std::size_t bodyCount() const noexcept override;

    [[nodiscard]] Vec3 position(BodyId id) const override;
    [[nodiscard]] Quat rotation(BodyId id) const override;
    [[nodiscard]] Vec3 linearVelocity(BodyId id) const override;
    [[nodiscard]] BodyKind kindOf(BodyId id) const override;
    [[nodiscard]] ecs::Entity entityOf(BodyId id) const override;
    [[nodiscard]] AABB boundsOf(BodyId id) const override;

    void setLinearVelocity(BodyId id, const Vec3& v) override;
    void teleportBody(BodyId id, const Vec3& pos, const Quat& rot) override;
    void setTransform(BodyId id, const Vec3& pos, const Quat& rot) override;
    void setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) override;
    void applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos) override;
    void applyForce(BodyId id, const Vec3& force) override;

    void step(float dt) override;
    void setGravity(const Vec3& g) override;

    [[nodiscard]] const std::vector<ContactEvent>& contacts() const noexcept override {
        return contacts_;
    }

    [[nodiscard]] RaycastHit raycast(const Ray& r, Real maxDistance,
                                     std::uint16_t mask) const override;
    [[nodiscard]] std::vector<BodyId> overlapSphere(const Vec3& center, Real radius,
                                                    std::uint16_t mask) const override;

    void setCollisionMatrix(const CollisionMatrix* m) noexcept override { matrix_ = m; }

    /// Используются ContactListener'ом (он объявлен в .cpp).
    [[nodiscard]] ecs::Entity entityForJoltBody(const JPH::BodyID& id) const;
    [[nodiscard]] bool groupsCollide(const JPH::BodyID& a, const JPH::BodyID& b) const;

private:
    class BPLayerInterface;
    class ObjectVsBPFilter;
    class ObjectVsObjectFilter;
    class ContactCollector;

    struct Record {
        JPH::BodyID  joltId;
        ecs::Entity  entity = ecs::kNullEntity;
        BodyKind     kind = BodyKind::Static;
        std::uint16_t group = 0;
        std::uint16_t mask = 0xFFFF;
        bool         alive = false;
    };

    [[nodiscard]] JPH::Ref<JPH::Shape> buildShape(const ShapeDesc& s) const;

    std::unique_ptr<JPH::PhysicsSystem>   system_;
    std::unique_ptr<JPH::TempAllocator>   tempAllocator_;
    std::unique_ptr<JPH::JobSystem>       jobSystem_;
    std::unique_ptr<BPLayerInterface>     bpLayerInterface_;
    std::unique_ptr<ObjectVsBPFilter>     objectVsBpFilter_;
    std::unique_ptr<ObjectVsObjectFilter> objectVsObjFilter_;
    std::unique_ptr<ContactCollector>     contactCollector_;

    std::vector<Record>       bodies_;
    std::vector<BodyId>       freeList_;
    std::vector<ContactEvent> contacts_;
    /// JPH::BodyID (index+sequence) -> наш BodyId.
    std::unordered_map<std::uint32_t, BodyId> joltToBody_;

    Vec3                   gravity_{0, -9.81f, 0};
    const CollisionMatrix* matrix_ = nullptr;
    float                  lastStep_ = 1.0f / 60.0f;
};

} // namespace lv::physics

#endif // LV_WITH_JOLT
