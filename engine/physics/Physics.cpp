/**
 * @file    Physics.cpp
 * @brief   Реализация физического мира.
 *
 * Если движок собран с @c LV_WITH_JOLT, все вызовы уходят в Jolt Physics.
 * В противном случае работает встроенный fallback-симулятор: интегрирование
 * скоростей, AABB/сферы для статических тел и событий контакта. Он не претендует
 * на точность Jolt, но полностью детерминирован и достаточен для тестов
 * геймплея, CI без GPU и dedicated-сервера.
 */
#include "Physics.h"

#if defined(LV_WITH_JOLT)
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#endif

#include <algorithm>
#include <cmath>

namespace lv::physics {

struct PhysicsWorld::Body {
    BodyId      id = kInvalidBody;
    BodyDesc    desc;
    Vec3        position;
    Quat        rotation;
    Vec3        linearVelocity;
    Vec3        angularVelocity;
    Vec3        forceAccum;
    Vec3        impulseAccum;
    AABB        aabb;
    bool        alive = false;
    bool        sleeping = false;
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init(std::uint32_t maxBodies) {
    if (initialized_) return true;
    bodies_.reserve(maxBodies);
#if defined(LV_WITH_JOLT)
    joltActive_ = true;   // реальная инициализация Jolt — в backends/JoltBackend.cpp
#endif
    initialized_ = true;
    return true;
}

void PhysicsWorld::shutdown() {
    bodies_.clear();
    freeList_.clear();
    contacts_.clear();
    initialized_ = false;
    joltActive_ = false;
}

BodyId PhysicsWorld::createBody(const BodyDesc& desc) {
    BodyId id;
    if (!freeList_.empty()) { id = freeList_.back(); freeList_.pop_back(); }
    else { id = static_cast<BodyId>(bodies_.size()); bodies_.push_back(nullptr); }

    auto b = std::make_unique<Body>();
    b->id = id;
    b->desc = desc;
    b->position = desc.position;
    b->rotation = desc.rotation;
    b->alive = true;
    b->aabb = AABB{desc.position - desc.shape.halfExtents, desc.position + desc.shape.halfExtents};
    if (desc.shape.kind == ShapeKind::Sphere) {
        const Vec3 r{desc.shape.radius, desc.shape.radius, desc.shape.radius};
        b->aabb = AABB{desc.position - r, desc.position + r};
    }
    bodies_[id] = std::move(b);
    return id;
}

void PhysicsWorld::destroyBody(BodyId id) {
    if (id >= bodies_.size() || !bodies_[id]) return;
    bodies_[id]->alive = false;
    bodies_[id].reset();
    freeList_.push_back(id);
}

bool PhysicsWorld::isValid(BodyId id) const noexcept {
    return id < bodies_.size() && bodies_[id] && bodies_[id]->alive;
}

void PhysicsWorld::teleportBody(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    b.position = pos;
    b.rotation = rot;
    // Для КИНЕМАТИКА integrate() каждый шаг «доводит» тело до desc.position
    // (это его целевая точка). Если не обновить цель здесь, телепорт
    // отменялся бы на следующем же кадре — именно так скриптовое
    // setPosition() переставало работать для персонажей.
    b.desc.position = pos;
    b.desc.rotation = rot;
    // Скорость обнуляется: иначе тело «продолжило бы» прежнее движение и за
    // один кадр улетело бы из новой точки (заметно при телепорте к медпакету).
    b.linearVelocity = Vec3::zero();
    b.angularVelocity = Vec3::zero();
    const Vec3 e = b.aabb.extents();
    b.aabb = AABB{b.position - e, b.position + e};
    b.sleeping = false;
}

void PhysicsWorld::setLinearVelocity(BodyId id, const Vec3& v) {
    if (isValid(id)) { bodies_[id]->linearVelocity = v; bodies_[id]->sleeping = false; }
}

Vec3 PhysicsWorld::linearVelocity(BodyId id) const {
    return isValid(id) ? bodies_[id]->linearVelocity : Vec3::zero();
}

void PhysicsWorld::applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    if (b.desc.kind != BodyKind::Dynamic) return;
    b.impulseAccum += impulse;
    // Момент силы приближённо: r x J / m — достаточно для визуальной отдачи.
    const Vec3 r = atWorldPos - b.position;
    b.angularVelocity += cross(r, impulse) * (1.0f / std::max(0.0001f, b.desc.mass));
    b.sleeping = false;
}

void PhysicsWorld::applyForce(BodyId id, const Vec3& force) {
    if (isValid(id) && bodies_[id]->desc.kind == BodyKind::Dynamic) {
        bodies_[id]->forceAccum += force;
        bodies_[id]->sleeping = false;
    }
}

void PhysicsWorld::setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    b.desc.position = pos;
    b.desc.rotation = rot;
    b.sleeping = false;
}

void PhysicsWorld::setTransform(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    b.position = pos;
    b.rotation = rot;
    b.linearVelocity = Vec3::zero();
    b.angularVelocity = Vec3::zero();
    const Vec3 e = b.aabb.extents();
    b.aabb = AABB{pos - e, pos + e};
}

// ---------------------------------------------------------------------------
//  Fallback-интегратор
// ---------------------------------------------------------------------------
void PhysicsWorld::integrate(float dt) {
    for (auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        Body& b = *bp;
        if (b.desc.kind != BodyKind::Dynamic) {
            // Кинематические тела плавно доводятся до цели.
            if (b.desc.kind == BodyKind::Kinematic) {
                b.position = b.desc.position;
                b.rotation = b.desc.rotation;
            }
            continue;
        }
        const Real invMass = 1.0f / std::max(0.0001f, b.desc.mass);
        b.linearVelocity += (gravity_ + b.forceAccum * invMass) * dt;
        b.linearVelocity += b.impulseAccum * invMass;
        b.linearVelocity *= (1.0f - std::min(0.9f, b.desc.linearDamping * dt));
        b.angularVelocity *= (1.0f - std::min(0.9f, b.desc.angularDamping * dt));
        b.position += b.linearVelocity * dt;
        if (lengthSq(b.angularVelocity) > kEpsilon)
            b.rotation = normalize(b.rotation * Quat::fromAxisAngle(normalize(b.angularVelocity),
                                                                   length(b.angularVelocity) * dt));
        b.forceAccum = Vec3::zero();
        b.impulseAccum = Vec3::zero();
        const Vec3 e = b.aabb.extents();
        b.aabb = AABB{b.position - e, b.position + e};

        // Простейший «пол» на y=0: keeps тесты детерминированными без Jolt.
        if (b.position.y < e.y && b.linearVelocity.y < 0) {
            b.position.y = e.y;
            b.linearVelocity.y = -b.linearVelocity.y * b.desc.restitution;
            b.linearVelocity.x *= (1.0f - b.desc.friction);
            b.linearVelocity.z *= (1.0f - b.desc.friction);
        }
    }

    // Пары контактов: O(n^2) по AABB, но только для НЕспящих тел.
    contacts_.clear();
    std::vector<Body*> awake;
    awake.reserve(bodies_.size());
    for (auto& bp : bodies_) if (bp && bp->alive && !bp->desc.trigger) awake.push_back(bp.get());
    for (std::size_t i = 0; i < awake.size(); ++i) {
        for (std::size_t j = i + 1; j < awake.size(); ++j) {
            Body& a = *awake[i];
            Body& b = *awake[j];
            if (!matrix_.collides(a.desc.group, b.desc.group)) continue;
            if (a.desc.kind != BodyKind::Dynamic && b.desc.kind != BodyKind::Dynamic) continue;
            if (!a.aabb.intersects(b.aabb)) continue;
            ContactEvent ev;
            ev.a = a.desc.entity; ev.b = b.desc.entity;
            ev.position = (a.position + b.position) * 0.5f;
            ev.normal = normalize(b.position - a.position);
            ev.impulse = length(a.linearVelocity - b.linearVelocity);
            ev.began = true;
            contacts_.push_back(ev);

            // Отталкивание по нормали (импульсное разрешение).
            if (a.desc.kind == BodyKind::Dynamic && b.desc.kind == BodyKind::Dynamic) {
                const Vec3 impulse = ev.normal * (ev.impulse * 0.5f);
                a.linearVelocity -= impulse;
                b.linearVelocity += impulse;
            } else if (a.desc.kind == BodyKind::Dynamic) {
                a.linearVelocity -= ev.normal * ev.impulse;
                a.position -= ev.normal * 0.01f;
            } else if (b.desc.kind == BodyKind::Dynamic) {
                b.linearVelocity += ev.normal * ev.impulse;
                b.position += ev.normal * 0.01f;
            }
        }
    }
}

RaycastHit PhysicsWorld::raycast(const Ray& r, Real maxDistance, std::uint16_t mask) const {
    RaycastHit best;
    best.distance = maxDistance;
    for (const auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        if (!(bp->desc.mask & mask)) continue;
        if (auto t = intersectRayAABB(r, bp->aabb)) {
            if (*t <= best.distance) {
                best.hit = true;
                best.distance = *t;
                best.position = r.at(*t);
                best.body = bp->id;
                best.entity = bp->desc.entity;
                best.normal = normalize(bp->position - best.position);
            }
        }
    }
    if (!best.hit) best.distance = 0;
    return best;
}

std::vector<BodyId> PhysicsWorld::overlapSphere(const Vec3& center, Real radius, std::uint16_t mask) const {
    std::vector<BodyId> out;
    const AABB sphere{center - Vec3(radius), center + Vec3(radius)};
    for (const auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        if (!(bp->desc.mask & mask)) continue;
        if (bp->aabb.intersects(sphere) && distance(bp->position, center) <= radius + length(bp->aabb.extents()))
            out.push_back(bp->id);
    }
    return out;
}

std::size_t PhysicsWorld::bodyCount() const noexcept {
    std::size_t n = 0;
    for (const auto& b : bodies_) if (b && b->alive) ++n;
    return n;
}

void PhysicsWorld::syncFromECS(ecs::World& world) {
    // Kinematic-тела и телепорты: берём Transform из ECS как источник истины.
    world.registry().each<ecs::Transform, RigidBody>([&](ecs::Entity, ecs::Transform& t, RigidBody& rb) {
        if (!isValid(rb.body)) return true;
        Body& b = *bodies_[rb.body];
        if (b.desc.kind != BodyKind::Dynamic) {
            // Для статических и кинематических тел Transform — источник истины,
            // поэтому обновляем и текущую позицию тела, и его ЦЕЛЬ (desc):
            // иначе integrate() вернул бы кинематика на прежнюю точку.
            b.desc.position = t.position;
            b.desc.rotation = t.rotation;
            b.position = t.position;
            b.rotation = t.rotation;
            const Vec3 e = b.aabb.extents();
            b.aabb = AABB{b.position - e, b.position + e};
        }
        return true;
    });
}

void PhysicsWorld::syncToECS(ecs::World& world) {
    world.registry().each<ecs::Transform, RigidBody>([&](ecs::Entity, ecs::Transform& t, RigidBody& rb) {
        if (!rb.syncTransformToECS || !isValid(rb.body)) return true;
        const Body& b = *bodies_[rb.body];
        if (b.desc.kind == BodyKind::Dynamic || b.desc.kind == BodyKind::Kinematic) {
            t.position = b.position;
            t.rotation = b.rotation;
        }
        return true;
    });
}

void PhysicsWorld::step(ecs::World& world, float deltaTime) {
    if (!initialized_) return;
    syncFromECS(world);

    // Фиксированный шаг с аккумулятором: физика не зависит от fps,
    // а maxSubSteps_ защищает от «спирали смерти» на слабых машинах.
    accumulator_ += deltaTime;
    std::uint32_t steps = 0;
    while (accumulator_ >= fixedStep_ && steps < maxSubSteps_) {
        integrate(fixedStep_);
        accumulator_ -= fixedStep_;
        ++steps;
    }
    if (steps == maxSubSteps_) accumulator_ = 0.f;   // сбрасываем долг

    syncToECS(world);
}

void PhysicsWorld::collectDebugLines(std::vector<Vec3>& out, const Color&) const {
    for (const auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        const AABB& b = bp->aabb;
        const Vec3 c[8] = {
            {b.minPt.x, b.minPt.y, b.minPt.z}, {b.maxPt.x, b.minPt.y, b.minPt.z},
            {b.maxPt.x, b.minPt.y, b.maxPt.z}, {b.minPt.x, b.minPt.y, b.maxPt.z},
            {b.minPt.x, b.maxPt.y, b.minPt.z}, {b.maxPt.x, b.maxPt.y, b.minPt.z},
            {b.maxPt.x, b.maxPt.y, b.maxPt.z}, {b.minPt.x, b.maxPt.y, b.maxPt.z},
        };
        static const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                                         {0,4},{1,5},{2,6},{3,7}};
        for (auto& e : edges) { out.push_back(c[e[0]]); out.push_back(c[e[1]]); }
    }
}

void attachToWorld(PhysicsWorld& physics, ecs::World& world) {
    ecs::registerComponent<RigidBody>();
    ecs::registerComponent<Collider>();
    ecs::SystemDesc sys;
    sys.name = "Physics";
    sys.phase = ecs::Phase::Physics;
    sys.order = 0;
    sys.update = [&physics](ecs::World& w, float dt) { physics.step(w, dt); };
    world.addSystem(std::move(sys));
}

} // namespace lv::physics
