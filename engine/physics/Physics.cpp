/**
 * @file    Physics.cpp
 * @brief   Физический мир: трансляция ECS <-> бэкенд и фиксированный шаг.
 *
 * @details Сама симуляция живёт в бэкенде (@c backends/BuiltinBackend.cpp или
 *          @c backends/JoltBackend.cpp). Здесь остаётся то, что одинаково для
 *          любого движка физики и потому не должно дублироваться:
 *
 *          - синхронизация Transform <-> тело в обе стороны;
 *          - аккумулятор фиксированного шага с защитой от «спирали смерти»;
 *          - публичный API и матрица коллизий.
 *
 *          Благодаря этому в файле нет ни одного @c #ifdef: выбор реализации
 *          сделан один раз в фабрике @c createPhysicsBackend().
 */
#include "Physics.h"

#include "backends/PhysicsBackend.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace lv::physics {

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::init(std::uint32_t maxBodies, bool preferJolt) {
    if (initialized_) return true;

    backend_ = createPhysicsBackend(preferJolt);
    if (!backend_ || !backend_->init(maxBodies)) {
        backend_.reset();
        return false;
    }
    backend_->setCollisionMatrix(&matrix_);
    backend_->setGravity(gravity_);

    // usesJolt() должен отражать РЕАЛЬНО активный бэкенд, а не пожелание:
    // при сборке без LV_WITH_JOLT запрос Jolt молча даёт встроенный симулятор.
    joltActive_ = std::string_view(backend_->name()) == "jolt";
    initialized_ = true;
    return true;
}

void PhysicsWorld::shutdown() {
    if (backend_) backend_->shutdown();
    backend_.reset();
    contacts_.clear();
    accumulator_ = 0.f;
    initialized_ = false;
    joltActive_ = false;
}

const char* PhysicsWorld::backendName() const noexcept {
    return backend_ ? backend_->name() : "none";
}

BodyId PhysicsWorld::createBody(const BodyDesc& desc) {
    if (!backend_) return kInvalidBody;
    return backend_->createBody(desc);
}

void PhysicsWorld::destroyBody(BodyId id) {
    if (backend_) backend_->destroyBody(id);
}

bool PhysicsWorld::isValid(BodyId id) const noexcept {
    return backend_ && backend_->isValid(id);
}

std::size_t PhysicsWorld::bodyCount() const noexcept {
    return backend_ ? backend_->bodyCount() : 0;
}

void PhysicsWorld::setGravity(const Vec3& g) {
    gravity_ = g;
    if (backend_) backend_->setGravity(g);
}

void PhysicsWorld::setLinearVelocity(BodyId id, const Vec3& v) {
    if (backend_) backend_->setLinearVelocity(id, v);
}

Vec3 PhysicsWorld::linearVelocity(BodyId id) const {
    return backend_ ? backend_->linearVelocity(id) : Vec3::zero();
}

Vec3 PhysicsWorld::position(BodyId id) const {
    return backend_ ? backend_->position(id) : Vec3::zero();
}

Quat PhysicsWorld::rotation(BodyId id) const {
    return backend_ ? backend_->rotation(id) : Quat{};
}

BodyKind PhysicsWorld::kindOf(BodyId id) const {
    return backend_ ? backend_->kindOf(id) : BodyKind::Static;
}

ecs::Entity PhysicsWorld::entityOf(BodyId id) const {
    return backend_ ? backend_->entityOf(id) : ecs::kNullEntity;
}

void PhysicsWorld::teleportBody(BodyId id, const Vec3& pos, const Quat& rot) {
    if (backend_) backend_->teleportBody(id, pos, rot);
}

void PhysicsWorld::setTransform(BodyId id, const Vec3& pos, const Quat& rot) {
    if (backend_) backend_->setTransform(id, pos, rot);
}

void PhysicsWorld::setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) {
    if (backend_) backend_->setKinematicTarget(id, pos, rot);
}

void PhysicsWorld::applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos) {
    if (backend_) backend_->applyImpulse(id, impulse, atWorldPos);
}

void PhysicsWorld::applyForce(BodyId id, const Vec3& force) {
    if (backend_) backend_->applyForce(id, force);
}

RaycastHit PhysicsWorld::raycast(const Ray& r, Real maxDistance, std::uint16_t mask) const {
    return backend_ ? backend_->raycast(r, maxDistance, mask) : RaycastHit{};
}

std::vector<BodyId> PhysicsWorld::overlapSphere(const Vec3& center, Real radius,
                                                std::uint16_t mask) const {
    return backend_ ? backend_->overlapSphere(center, radius, mask) : std::vector<BodyId>{};
}

void PhysicsWorld::syncFromECS(ecs::World& world) {
    // Для статических и кинематических тел источник истины — Transform из ECS.
    // Для динамических наоборот: их положение считает солвер, и запись сюда
    // затёрла бы результат симуляции.
    world.registry().each<ecs::Transform, RigidBody>(
        [&](ecs::Entity, ecs::Transform& t, RigidBody& rb) {
            if (!isValid(rb.body)) return true;
            const BodyKind kind = backend_->kindOf(rb.body);
            if (kind == BodyKind::Kinematic) {
                backend_->setKinematicTarget(rb.body, t.position, t.rotation);
            } else if (kind == BodyKind::Static) {
                backend_->setTransform(rb.body, t.position, t.rotation);
            }
            return true;
        });
}

void PhysicsWorld::syncToECS(ecs::World& world) {
    world.registry().each<ecs::Transform, RigidBody>(
        [&](ecs::Entity, ecs::Transform& t, RigidBody& rb) {
            if (!rb.syncTransformToECS || !isValid(rb.body)) return true;
            const BodyKind kind = backend_->kindOf(rb.body);
            if (kind == BodyKind::Dynamic || kind == BodyKind::Kinematic) {
                t.position = backend_->position(rb.body);
                t.rotation = backend_->rotation(rb.body);
            }
            return true;
        });
}

void PhysicsWorld::step(ecs::World& world, float deltaTime) {
    if (!initialized_ || !backend_) return;
    syncFromECS(world);

    // Фиксированный шаг с аккумулятором: поведение физики не зависит от fps,
    // а maxSubSteps_ не даёт слабой машине уйти в «спираль смерти», когда
    // каждый кадр досимулировывает всё больше подшагов.
    contacts_.clear();
    accumulator_ += deltaTime;
    std::uint32_t steps = 0;
    while (accumulator_ >= fixedStep_ && steps < maxSubSteps_) {
        backend_->step(fixedStep_);
        const std::vector<ContactEvent>& stepContacts = backend_->contacts();
        contacts_.insert(contacts_.end(), stepContacts.begin(), stepContacts.end());
        accumulator_ -= fixedStep_;
        ++steps;
    }
    if (steps == maxSubSteps_) accumulator_ = 0.f;   // сбрасываем накопленный долг

    syncToECS(world);
}

void PhysicsWorld::collectDebugLines(std::vector<Vec3>& out, const Color&) const {
    if (!backend_) return;
    // Перебираем плотный диапазон BodyId: и Jolt, и встроенный бэкенд выдают
    // идентификаторы из одного счётчика с переиспользованием.
    const std::size_t alive = backend_->bodyCount();
    std::size_t found = 0;
    for (BodyId id = 0; found < alive && id < 65536; ++id) {
        if (!backend_->isValid(id)) continue;
        ++found;
        const AABB b = backend_->boundsOf(id);
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

// ---------------------------------------------------------------------------
//  Интеграция с ECS
// ---------------------------------------------------------------------------
void attachToWorld(PhysicsWorld& physics, ecs::World& world) {
    ecs::registerComponent<RigidBody>();
    ecs::registerComponent<Collider>();

    ecs::SystemDesc desc;
    desc.name = "Physics";
    desc.phase = ecs::Phase::Physics;
    desc.order = 0;
    desc.update = [&physics](ecs::World& w, float dt) { physics.step(w, dt); };
    world.addSystem(std::move(desc));
}

} // namespace lv::physics
