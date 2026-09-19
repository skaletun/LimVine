/**
 * @file    BuiltinBackend.h
 * @brief   Встроенный детерминированный физический бэкенд.
 * @ingroup Physics
 */
#pragma once

#include "PhysicsBackend.h"

#include <memory>
#include <vector>

namespace lv::physics {

/**
 * @brief Простая детерминированная физика без внешних зависимостей.
 *
 * Используется, когда движок собран без @c LV_WITH_JOLT, а также принудительно
 * в тестах, реплеях и на headless-сервере, где важна побитовая
 * воспроизводимость, а не физическая точность.
 */
class BuiltinBackend final : public IPhysicsBackend {
public:
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

private:
    /// Нормаль грани AABB в точке попадания луча.
    [[nodiscard]] static Vec3 faceNormal(const AABB& box, const Vec3& p) noexcept;

    struct Body {
        BodyId   id = kInvalidBody;
        BodyDesc desc;
        Vec3     position;
        Quat     rotation;
        Vec3     linearVelocity;
        Vec3     angularVelocity;
        Vec3     forceAccum;
        Vec3     impulseAccum;
        AABB     aabb;
        bool     alive = false;
    };

    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<BodyId>       freeList_;
    std::vector<ContactEvent> contacts_;
    Vec3                      gravity_{0, -9.81f, 0};
    const CollisionMatrix*    matrix_ = nullptr;
};

} // namespace lv::physics
