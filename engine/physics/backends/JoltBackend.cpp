/**
 * @file    JoltBackend.cpp
 * @brief   Реальный физический бэкенд на Jolt Physics 5.x.
 *
 * @details Компилируется только при @c LV_WITH_JOLT. Весь код, зависящий от
 *          Jolt, сосредоточен здесь: ни @c PhysicsWorld, ни остальной движок
 *          не включают заголовки Jolt и не знают о его типах.
 *
 *          Решения, принятые при интеграции:
 *
 *          - **Слои.** Jolt требует два уровня фильтрации: object layer и
 *            broad phase layer. Мы заводим ровно два object-слоя (NON_MOVING /
 *            MOVING) и отображаем их один-в-один на broad phase. Наша
 *            16-групповая @c CollisionMatrix живёт НАД этим: она проверяется
 *            в @c ObjectLayerPairFilter через таблицу групп тел. Так игровые
 *            правила («пули не бьют по своим») не смешиваются со статикой
 *            брод-фазы, которая нужна Jolt для производительности.
 *
 *          - **Единицы и типы.** Jolt может быть собран с double-precision
 *            позициями (RVec3). Конвертеры toJolt/fromJolt изолируют это,
 *            поэтому движок одинаково работает и с float-, и с double-сборкой.
 *
 *          - **Контакты.** Jolt отдаёт контакты через callback из рабочих
 *            потоков solver'а, поэтому листенер складывает их под мьютексом,
 *            а @c contacts() возвращает уже собранный за шаг буфер. Без
 *            блокировки это была бы гонка данных на многоядерном solver'е.
 *
 *          - **Владение телами.** Наш BodyId — плотный индекс, а Jolt выдаёт
 *            свой BodyID. Мы храним таблицу соответствия, чтобы BodyId
 *            оставался стабильным для игрового кода и сериализации.
 */
#include "JoltBackend.h"

#if defined(LV_WITH_JOLT)

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace lv::physics {
namespace {

// --- конвертеры --------------------------------------------------------------
inline JPH::Vec3 toJolt(const Vec3& v) {
    return JPH::Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}
inline JPH::RVec3 toJoltR(const Vec3& v) {
    return JPH::RVec3(static_cast<JPH::Real>(v.x), static_cast<JPH::Real>(v.y),
                      static_cast<JPH::Real>(v.z));
}
inline JPH::Quat toJolt(const Quat& q) {
    // Кватернион Jolt требует нормализации: ненормализованный вызывает assert
    // в отладочной сборке и тихо искажает вращение в релизной.
    JPH::Quat jq(static_cast<float>(q.x), static_cast<float>(q.y),
                 static_cast<float>(q.z), static_cast<float>(q.w));
    if (jq.LengthSq() < 1.0e-12f) return JPH::Quat::sIdentity();
    return jq.Normalized();
}
inline Vec3 fromJolt(const JPH::Vec3& v) {
    return Vec3{static_cast<Real>(v.GetX()), static_cast<Real>(v.GetY()),
                static_cast<Real>(v.GetZ())};
}
inline Vec3 fromJoltR(const JPH::RVec3& v) {
    return Vec3{static_cast<Real>(v.GetX()), static_cast<Real>(v.GetY()),
                static_cast<Real>(v.GetZ())};
}
inline Quat fromJolt(const JPH::Quat& q) {
    return Quat{static_cast<Real>(q.GetX()), static_cast<Real>(q.GetY()),
                static_cast<Real>(q.GetZ()), static_cast<Real>(q.GetW())};
}

// --- слои --------------------------------------------------------------------
namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING     = 1;
static constexpr JPH::ObjectLayer NUM        = 2;
} // namespace Layers

namespace BPLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM = 2;
} // namespace BPLayers

void traceImpl(const char* fmt, ...) {
    va_list list;
    va_start(list, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, list);
    va_end(list);
    std::fprintf(stderr, "[jolt] %s\n", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool assertFailedImpl(const char* expr, const char* msg, const char* file, JPH::uint line) {
    std::fprintf(stderr, "[jolt] assert: %s:%u: (%s) %s\n", file, line, expr, msg ? msg : "");
    return true;   // прервать в отладчике
}
#endif

/// Глобальная инициализация Jolt (Factory + RegisterTypes) — ровно один раз
/// на процесс, даже если создано несколько PhysicsWorld.
class JoltGlobalInit {
public:
    static void ensure() {
        static JoltGlobalInit instance;
        (void)instance;
    }

private:
    JoltGlobalInit() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = traceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = assertFailedImpl;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltGlobalInit() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

} // namespace

// ---------------------------------------------------------------------------
//  Реализация слоёв и фильтров
// ---------------------------------------------------------------------------

/// Отображение object layer -> broad phase layer.
class JoltBackend::BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        objectToBroadPhase_[Layers::NON_MOVING] = BPLayers::NON_MOVING;
        objectToBroadPhase_[Layers::MOVING]     = BPLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return objectToBroadPhase_[l];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(l) ==
                       static_cast<JPH::BroadPhaseLayer::Type>(BPLayers::NON_MOVING)
                   ? "NON_MOVING"
                   : "MOVING";
    }
#endif
private:
    JPH::BroadPhaseLayer objectToBroadPhase_[Layers::NUM];
};

/// Статика не сталкивается со статикой — остальное решает CollisionMatrix
/// движка (проверяется в ObjectVsObjectFilter).
class JoltBackend::ObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        if (layer == Layers::NON_MOVING)
            return static_cast<JPH::BroadPhaseLayer::Type>(bp) ==
                   static_cast<JPH::BroadPhaseLayer::Type>(BPLayers::MOVING);
        return true;
    }
};

class JoltBackend::ObjectVsObjectFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // Два статических тела никогда не проверяются друг с другом.
        return !(a == Layers::NON_MOVING && b == Layers::NON_MOVING);
    }
};

/// Сбор контактов. Jolt вызывает эти методы из потоков solver'а, поэтому
/// буфер защищён мьютексом.
class JoltBackend::ContactCollector final : public JPH::ContactListener {
public:
    explicit ContactCollector(JoltBackend& owner) : owner_(owner) {}

    JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2,
                                          JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
        // Игровая матрица коллизий применяется здесь: у Jolt нет понятия
        // наших 16 групп, а прокидывать их в ObjectLayer означало бы
        // комбинаторный взрыв слоёв.
        if (!owner_.groupsCollide(b1.GetID(), b2.GetID()))
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                        const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
        push(b1, b2, manifold, /*began=*/true);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        ContactEvent ev;
        ev.began = false;
        {
            std::scoped_lock lock(mutex_);
            ev.a = owner_.entityForJoltBody(pair.GetBody1ID());
            ev.b = owner_.entityForJoltBody(pair.GetBody2ID());
            pending_.push_back(ev);
        }
    }

    void drainInto(std::vector<ContactEvent>& out) {
        std::scoped_lock lock(mutex_);
        out.insert(out.end(), pending_.begin(), pending_.end());
        pending_.clear();
    }

private:
    void push(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, bool began) {
        ContactEvent ev;
        ev.began = began;
        ev.normal = fromJolt(m.mWorldSpaceNormal);
        ev.position = m.mRelativeContactPointsOn1.empty()
                          ? fromJoltR(m.mBaseOffset)
                          : fromJoltR(m.GetWorldSpaceContactPointOn1(0));
        // «Сила» контакта: относительная скорость сближения — по ней геймплей
        // отличает касание от удара (звук, урон от падения).
        const Vec3 v1 = fromJolt(b1.GetLinearVelocity());
        const Vec3 v2 = fromJolt(b2.GetLinearVelocity());
        ev.impulse = length(v1 - v2);
        {
            std::scoped_lock lock(mutex_);
            ev.a = owner_.entityForJoltBody(b1.GetID());
            ev.b = owner_.entityForJoltBody(b2.GetID());
            pending_.push_back(ev);
        }
    }

    JoltBackend&              owner_;
    std::mutex                mutex_;
    std::vector<ContactEvent> pending_;
};

// ---------------------------------------------------------------------------
//  JoltBackend
// ---------------------------------------------------------------------------

JoltBackend::JoltBackend() = default;

JoltBackend::~JoltBackend() { shutdown(); }

const char* JoltBackend::name() const noexcept { return "jolt"; }

bool JoltBackend::init(std::uint32_t maxBodies) {
    if (system_) return true;
    JoltGlobalInit::ensure();

    tempAllocator_ = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);

    // Пул потоков Jolt. Оставляем одно ядро главному потоку движка, чтобы
    // solver не конкурировал с игровой логикой за все ядра сразу.
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int workers = std::max(1, hw - 1);
    jobSystem_ = std::make_unique<JPH::JobSystemThreadPool>(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);

    bpLayerInterface_ = std::make_unique<BPLayerInterface>();
    objectVsBpFilter_ = std::make_unique<ObjectVsBPFilter>();
    objectVsObjFilter_ = std::make_unique<ObjectVsObjectFilter>();

    const JPH::uint maxBodyPairs = std::max<JPH::uint>(1024, maxBodies * 4);
    const JPH::uint maxContacts  = std::max<JPH::uint>(1024, maxBodies * 2);

    system_ = std::make_unique<JPH::PhysicsSystem>();
    system_->Init(maxBodies, /*inNumBodyMutexes=*/0, maxBodyPairs, maxContacts,
                  *bpLayerInterface_, *objectVsBpFilter_, *objectVsObjFilter_);

    contactCollector_ = std::make_unique<ContactCollector>(*this);
    system_->SetContactListener(contactCollector_.get());
    system_->SetGravity(toJolt(gravity_));

    bodies_.clear();
    freeList_.clear();
    return true;
}

void JoltBackend::shutdown() {
    if (!system_) return;
    JPH::BodyInterface& bi = system_->GetBodyInterface();
    for (auto& rec : bodies_) {
        if (!rec.alive) continue;
        if (!rec.joltId.IsInvalid()) {
            bi.RemoveBody(rec.joltId);
            bi.DestroyBody(rec.joltId);
        }
        rec.alive = false;
    }
    bodies_.clear();
    freeList_.clear();
    contacts_.clear();
    joltToBody_.clear();

    system_->SetContactListener(nullptr);
    contactCollector_.reset();
    system_.reset();
    objectVsObjFilter_.reset();
    objectVsBpFilter_.reset();
    bpLayerInterface_.reset();
    jobSystem_.reset();
    tempAllocator_.reset();
}

JPH::Ref<JPH::Shape> JoltBackend::buildShape(const ShapeDesc& s) const {
    // Общая с встроенным бэкендом проверка: оба движка обязаны отвергать
    // одни и те же вырожденные формы, иначе контрактный тест разойдётся.
    if (!isShapeValid(s)) return {};

    JPH::ShapeSettings::ShapeResult result;
    switch (s.kind) {
        case ShapeKind::Box: {
            // Jolt требует, чтобы half-extent был не меньше convex radius,
            // иначе создание формы завершается ошибкой.
            const float cx = std::max(0.001f, static_cast<float>(s.halfExtents.x));
            const float cy = std::max(0.001f, static_cast<float>(s.halfExtents.y));
            const float cz = std::max(0.001f, static_cast<float>(s.halfExtents.z));
            const float minExtent = std::min({cx, cy, cz});
            const float convexRadius = std::min(0.05f, minExtent * 0.5f);
            result = JPH::BoxShapeSettings(JPH::Vec3(cx, cy, cz), convexRadius).Create();
            break;
        }
        case ShapeKind::Sphere:
            result = JPH::SphereShapeSettings(std::max(0.001f, static_cast<float>(s.radius)))
                         .Create();
            break;
        case ShapeKind::Capsule:
            result = JPH::CapsuleShapeSettings(std::max(0.001f, static_cast<float>(s.halfHeight)),
                                               std::max(0.001f, static_cast<float>(s.radius)))
                         .Create();
            break;
        case ShapeKind::Cylinder: {
            const float hh = std::max(0.001f, static_cast<float>(s.halfHeight));
            const float r  = std::max(0.001f, static_cast<float>(s.radius));
            result = JPH::CylinderShapeSettings(hh, r, std::min(0.05f, std::min(hh, r) * 0.5f))
                         .Create();
            break;
        }
        case ShapeKind::ConvexHull: {
            JPH::Array<JPH::Vec3> pts;
            pts.reserve(s.points.size());
            for (const Vec3& p : s.points) pts.push_back(toJolt(p));
            result = JPH::ConvexHullShapeSettings(pts).Create();
            break;
        }
        case ShapeKind::TriangleMesh: {
            JPH::VertexList verts;
            verts.reserve(s.meshVerts.size());
            for (const Vec3& v : s.meshVerts)
                verts.push_back(JPH::Float3(static_cast<float>(v.x), static_cast<float>(v.y),
                                            static_cast<float>(v.z)));
            JPH::IndexedTriangleList tris;
            tris.reserve(s.meshIndices.size() / 3);
            for (std::size_t i = 0; i + 2 < s.meshIndices.size(); i += 3)
                tris.push_back(JPH::IndexedTriangle(s.meshIndices[i], s.meshIndices[i + 1],
                                                    s.meshIndices[i + 2], 0));
            result = JPH::MeshShapeSettings(verts, tris).Create();
            break;
        }
    }
    if (!result.IsValid() || result.HasError()) {
        std::fprintf(stderr, "[jolt] shape creation failed: %s\n", result.GetError().c_str());
        return {};
    }
    return result.Get();
}

BodyId JoltBackend::createBody(const BodyDesc& desc) {
    if (!system_) return kInvalidBody;

    JPH::Ref<JPH::Shape> shape = buildShape(desc.shape);
    if (shape == nullptr) return kInvalidBody;

    // TriangleMesh не может быть динамическим телом в Jolt — это корректное
    // ограничение (нет инерции у произвольной сетки), поэтому такую форму
    // принудительно делаем статической, а не падаем.
    BodyKind kind = desc.kind;
    if (desc.shape.kind == ShapeKind::TriangleMesh && kind == BodyKind::Dynamic)
        kind = BodyKind::Static;

    JPH::EMotionType motion = JPH::EMotionType::Static;
    JPH::ObjectLayer layer  = Layers::NON_MOVING;
    switch (kind) {
        case BodyKind::Static:    motion = JPH::EMotionType::Static;    layer = Layers::NON_MOVING; break;
        case BodyKind::Kinematic: motion = JPH::EMotionType::Kinematic; layer = Layers::MOVING;     break;
        case BodyKind::Dynamic:   motion = JPH::EMotionType::Dynamic;   layer = Layers::MOVING;     break;
    }

    JPH::BodyCreationSettings settings(shape, toJoltR(desc.position), toJolt(desc.rotation),
                                       motion, layer);
    settings.mFriction = static_cast<float>(desc.friction);
    settings.mRestitution = static_cast<float>(desc.restitution);
    settings.mLinearDamping = static_cast<float>(desc.linearDamping);
    settings.mAngularDamping = static_cast<float>(desc.angularDamping);
    settings.mIsSensor = desc.trigger;
    if (kind == BodyKind::Dynamic) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = std::max(0.001f, static_cast<float>(desc.mass));
    }

    JPH::BodyInterface& bi = system_->GetBodyInterface();
    JPH::Body* body = bi.CreateBody(settings);
    if (!body) {
        std::fprintf(stderr, "[jolt] body limit reached\n");
        return kInvalidBody;
    }
    bi.AddBody(body->GetID(), kind == BodyKind::Static ? JPH::EActivation::DontActivate
                                                       : JPH::EActivation::Activate);

    BodyId id;
    if (!freeList_.empty()) {
        id = freeList_.back();
        freeList_.pop_back();
    } else {
        id = static_cast<BodyId>(bodies_.size());
        bodies_.emplace_back();
    }
    Record& rec = bodies_[id];
    rec.alive = true;
    rec.joltId = body->GetID();
    rec.entity = desc.entity;
    rec.kind = kind;
    rec.group = desc.group;
    rec.mask = desc.mask;
    joltToBody_[rec.joltId.GetIndexAndSequenceNumber()] = id;
    return id;
}

void JoltBackend::destroyBody(BodyId id) {
    if (!isValid(id)) return;
    Record& rec = bodies_[id];
    JPH::BodyInterface& bi = system_->GetBodyInterface();
    bi.RemoveBody(rec.joltId);
    bi.DestroyBody(rec.joltId);
    joltToBody_.erase(rec.joltId.GetIndexAndSequenceNumber());
    rec.alive = false;
    rec.joltId = JPH::BodyID();
    rec.entity = ecs::kNullEntity;
    freeList_.push_back(id);
}

bool JoltBackend::isValid(BodyId id) const noexcept {
    return system_ && id < bodies_.size() && bodies_[id].alive;
}

std::size_t JoltBackend::bodyCount() const noexcept {
    std::size_t n = 0;
    for (const Record& r : bodies_) if (r.alive) ++n;
    return n;
}

ecs::Entity JoltBackend::entityForJoltBody(const JPH::BodyID& id) const {
    const auto it = joltToBody_.find(id.GetIndexAndSequenceNumber());
    if (it == joltToBody_.end()) return ecs::kNullEntity;
    return bodies_[it->second].entity;
}

bool JoltBackend::groupsCollide(const JPH::BodyID& a, const JPH::BodyID& b) const {
    if (!matrix_) return true;
    const auto ia = joltToBody_.find(a.GetIndexAndSequenceNumber());
    const auto ib = joltToBody_.find(b.GetIndexAndSequenceNumber());
    if (ia == joltToBody_.end() || ib == joltToBody_.end()) return true;
    const Record& ra = bodies_[ia->second];
    const Record& rb = bodies_[ib->second];
    if (!(ra.mask & rb.mask)) return false;
    return matrix_->collides(ra.group, rb.group);
}

Vec3 JoltBackend::position(BodyId id) const {
    if (!isValid(id)) return Vec3::zero();
    return fromJoltR(system_->GetBodyInterface().GetPosition(bodies_[id].joltId));
}

Quat JoltBackend::rotation(BodyId id) const {
    if (!isValid(id)) return Quat{};
    return fromJolt(system_->GetBodyInterface().GetRotation(bodies_[id].joltId));
}

Vec3 JoltBackend::linearVelocity(BodyId id) const {
    if (!isValid(id)) return Vec3::zero();
    return fromJolt(system_->GetBodyInterface().GetLinearVelocity(bodies_[id].joltId));
}

BodyKind JoltBackend::kindOf(BodyId id) const {
    return isValid(id) ? bodies_[id].kind : BodyKind::Static;
}

ecs::Entity JoltBackend::entityOf(BodyId id) const {
    return isValid(id) ? bodies_[id].entity : ecs::kNullEntity;
}

AABB JoltBackend::boundsOf(BodyId id) const {
    if (!isValid(id)) return AABB{};
    const JPH::AABox box =
        system_->GetBodyInterface().GetTransformedShape(bodies_[id].joltId).GetWorldSpaceBounds();
    return AABB{fromJolt(box.mMin), fromJolt(box.mMax)};
}

void JoltBackend::setLinearVelocity(BodyId id, const Vec3& v) {
    if (!isValid(id)) return;
    system_->GetBodyInterface().SetLinearVelocity(bodies_[id].joltId, toJolt(v));
}

void JoltBackend::teleportBody(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    JPH::BodyInterface& bi = system_->GetBodyInterface();
    const JPH::BodyID jid = bodies_[id].joltId;
    bi.SetPositionAndRotation(jid, toJoltR(pos), toJolt(rot), JPH::EActivation::Activate);
    // Скорости обнуляем явно: телепорт не должен сохранять набранную инерцию.
    if (bodies_[id].kind != BodyKind::Static) {
        bi.SetLinearVelocity(jid, JPH::Vec3::sZero());
        bi.SetAngularVelocity(jid, JPH::Vec3::sZero());
    }
}

void JoltBackend::setTransform(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    system_->GetBodyInterface().SetPositionAndRotation(
        bodies_[id].joltId, toJoltR(pos), toJolt(rot),
        bodies_[id].kind == BodyKind::Static ? JPH::EActivation::DontActivate
                                             : JPH::EActivation::Activate);
}

void JoltBackend::setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    if (bodies_[id].kind != BodyKind::Kinematic) {
        setTransform(id, pos, rot);
        return;
    }
    // MoveKinematic задаёт скорость так, чтобы за dt тело пришло в цель —
    // именно это даёт кинематику корректные контакты, в отличие от
    // мгновенной телепортации.
    system_->GetBodyInterface().MoveKinematic(bodies_[id].joltId, toJoltR(pos), toJolt(rot),
                                              lastStep_ > 0 ? lastStep_ : 1.0f / 60.0f);
}

void JoltBackend::applyImpulse(BodyId id, const Vec3& impulse, const Vec3& atWorldPos) {
    if (!isValid(id) || bodies_[id].kind != BodyKind::Dynamic) return;
    JPH::BodyInterface& bi = system_->GetBodyInterface();
    if (lengthSq(atWorldPos) > kEpsilon)
        bi.AddImpulse(bodies_[id].joltId, toJolt(impulse), toJoltR(atWorldPos));
    else
        bi.AddImpulse(bodies_[id].joltId, toJolt(impulse));
}

void JoltBackend::applyForce(BodyId id, const Vec3& force) {
    if (!isValid(id) || bodies_[id].kind != BodyKind::Dynamic) return;
    system_->GetBodyInterface().AddForce(bodies_[id].joltId, toJolt(force));
}

void JoltBackend::setGravity(const Vec3& g) {
    gravity_ = g;
    if (system_) system_->SetGravity(toJolt(g));
}

void JoltBackend::step(float dt) {
    if (!system_ || dt <= 0.f) return;
    lastStep_ = dt;
    contacts_.clear();
    // collisionSteps=1: движок сам управляет длиной шага через аккумулятор
    // PhysicsWorld, поэтому дробить шаг ещё раз внутри Jolt не нужно.
    system_->Update(dt, /*inCollisionSteps=*/1, tempAllocator_.get(), jobSystem_.get());
    contactCollector_->drainInto(contacts_);
}

RaycastHit JoltBackend::raycast(const Ray& r, Real maxDistance, std::uint16_t mask) const {
    RaycastHit out;
    if (!system_ || maxDistance <= 0) return out;

    const Vec3 dir = normalize(r.direction);
    const JPH::RRayCast ray{toJoltR(r.origin), toJolt(dir * maxDistance)};
    JPH::RayCastResult hit;
    if (!system_->GetNarrowPhaseQuery().CastRay(ray, hit)) return out;

    const auto it = joltToBody_.find(hit.mBodyID.GetIndexAndSequenceNumber());
    if (it != joltToBody_.end()) {
        const Record& rec = bodies_[it->second];
        // Маска применяется после попадания: Jolt-фильтры работают со слоями,
        // а наши 16 групп живут отдельно.
        if (!groupAllowed(rec.group, mask)) return RaycastHit{};
        out.body = it->second;
        out.entity = rec.entity;
    }
    out.hit = true;
    out.distance = static_cast<Real>(hit.mFraction) * maxDistance;
    out.position = r.origin + dir * out.distance;

    JPH::BodyLockRead lock(system_->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        out.normal = fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
            hit.mSubShapeID2, toJoltR(out.position)));
    } else {
        out.normal = -dir;
    }
    return out;
}

std::vector<BodyId> JoltBackend::overlapSphere(const Vec3& center, Real radius,
                                               std::uint16_t mask) const {
    std::vector<BodyId> out;
    if (!system_) return out;

    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    system_->GetBroadPhaseQuery().CollideSphere(toJolt(center), static_cast<float>(radius),
                                                collector);
    out.reserve(collector.mHits.size());
    for (const JPH::BodyID& jid : collector.mHits) {
        const auto it = joltToBody_.find(jid.GetIndexAndSequenceNumber());
        if (it == joltToBody_.end()) continue;
        if (!groupAllowed(bodies_[it->second].group, mask)) continue;
        out.push_back(it->second);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace lv::physics

#endif // LV_WITH_JOLT
