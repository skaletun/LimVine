/**
 * @file    BuiltinBackend.cpp
 * @brief   Встроенный детерминированный физический бэкенд (без внешних SDK).
 *
 * @details Это не «заглушка», а рабочая упрощённая физика: интегрирование
 *          скоростей, пол на y=0, AABB-контакты с импульсным разрешением,
 *          raycast и overlapSphere. Она сознательно проще Jolt, но обладает
 *          двумя свойствами, которых от Jolt не требуется:
 *
 *          - **полная детерминированность** при фиксированном шаге — на ней
 *            держатся тесты геймплея, реплеи и headless-сервер;
 *          - **отсутствие зависимостей** — движок собирается и проходит все
 *            тесты на машине без единого установленного SDK.
 *
 *          Код перенесён из PhysicsWorld без изменения поведения: задача
 *          рефакторинга была развести два бэкенда за общим интерфейсом, а не
 *          менять физику. Поэтому существующие тесты остаются в силе.
 */
#include "BuiltinBackend.h"

#include <algorithm>
#include <cmath>

namespace lv::physics {

const char* BuiltinBackend::name() const noexcept { return "builtin"; }

bool BuiltinBackend::init(std::uint32_t maxBodies) {
    bodies_.reserve(maxBodies);
    return true;
}

void BuiltinBackend::shutdown() {
    bodies_.clear();
    freeList_.clear();
    contacts_.clear();
}

BodyId BuiltinBackend::createBody(const BodyDesc& desc) {
    if (!isShapeValid(desc.shape)) return kInvalidBody;

    BodyId id;
    if (!freeList_.empty()) {
        id = freeList_.back();
        freeList_.pop_back();
    } else {
        id = static_cast<BodyId>(bodies_.size());
        bodies_.push_back(nullptr);
    }
    auto b = std::make_unique<Body>();
    b->id = id;
    b->desc = desc;
    b->position = desc.position;
    b->rotation = desc.rotation;
    b->alive = true;

    // Полуразмеры AABB зависят от формы: капсула и цилиндр «выше», чем шире.
    Vec3 e{0.5f, 0.5f, 0.5f};
    switch (desc.shape.kind) {
        case ShapeKind::Box:    e = desc.shape.halfExtents; break;
        case ShapeKind::Sphere: e = Vec3(desc.shape.radius); break;
        case ShapeKind::Capsule:
        case ShapeKind::Cylinder:
            e = Vec3{desc.shape.radius, desc.shape.halfHeight + desc.shape.radius,
                     desc.shape.radius};
            break;
        case ShapeKind::ConvexHull:
        case ShapeKind::TriangleMesh: {
            const auto& pts = desc.shape.kind == ShapeKind::ConvexHull ? desc.shape.points
                                                                      : desc.shape.meshVerts;
            if (!pts.empty()) {
                Vec3 lo = pts[0], hi = pts[0];
                for (const Vec3& p : pts) {
                    lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                e = (hi - lo) * 0.5f;
            }
            break;
        }
    }
    b->aabb = AABB{b->position - e, b->position + e};
    bodies_[id] = std::move(b);
    return id;
}

void BuiltinBackend::destroyBody(BodyId id) {
    if (!isValid(id)) return;
    bodies_[id]->alive = false;
    bodies_[id].reset();
    freeList_.push_back(id);
}

bool BuiltinBackend::isValid(BodyId id) const noexcept {
    return id < bodies_.size() && bodies_[id] && bodies_[id]->alive;
}

std::size_t BuiltinBackend::bodyCount() const noexcept {
    std::size_t n = 0;
    for (const auto& b : bodies_) if (b && b->alive) ++n;
    return n;
}

Vec3 BuiltinBackend::position(BodyId id) const {
    return isValid(id) ? bodies_[id]->position : Vec3::zero();
}
Quat BuiltinBackend::rotation(BodyId id) const {
    return isValid(id) ? bodies_[id]->rotation : Quat{};
}
Vec3 BuiltinBackend::linearVelocity(BodyId id) const {
    return isValid(id) ? bodies_[id]->linearVelocity : Vec3::zero();
}
BodyKind BuiltinBackend::kindOf(BodyId id) const {
    return isValid(id) ? bodies_[id]->desc.kind : BodyKind::Static;
}
ecs::Entity BuiltinBackend::entityOf(BodyId id) const {
    return isValid(id) ? bodies_[id]->desc.entity : ecs::kNullEntity;
}
AABB BuiltinBackend::boundsOf(BodyId id) const {
    return isValid(id) ? bodies_[id]->aabb : AABB{};
}

void BuiltinBackend::setLinearVelocity(BodyId id, const Vec3& v) {
    if (isValid(id)) bodies_[id]->linearVelocity = v;
}

void BuiltinBackend::teleportBody(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    b.position = pos;
    b.rotation = rot;
    b.desc.position = pos;
    b.desc.rotation = rot;
    // Обнуление скорости обязательно: иначе тело «продолжит падать» после
    // телепорта со скоростью, набранной до него.
    b.linearVelocity = Vec3::zero();
    b.angularVelocity = Vec3::zero();
    b.forceAccum = Vec3::zero();
    b.impulseAccum = Vec3::zero();
    const Vec3 e = b.aabb.extents();
    b.aabb = AABB{pos - e, pos + e};
}

void BuiltinBackend::setTransform(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    Body& b = *bodies_[id];
    b.position = pos;
    b.rotation = rot;
    b.desc.position = pos;
    b.desc.rotation = rot;
    const Vec3 e = b.aabb.extents();
    b.aabb = AABB{pos - e, pos + e};
}

void BuiltinBackend::setKinematicTarget(BodyId id, const Vec3& pos, const Quat& rot) {
    if (!isValid(id)) return;
    bodies_[id]->desc.position = pos;
    bodies_[id]->desc.rotation = rot;
}

void BuiltinBackend::applyImpulse(BodyId id, const Vec3& impulse, const Vec3&) {
    if (isValid(id)) bodies_[id]->impulseAccum += impulse;
}

void BuiltinBackend::applyForce(BodyId id, const Vec3& force) {
    if (isValid(id)) bodies_[id]->forceAccum += force;
}

void BuiltinBackend::setGravity(const Vec3& g) { gravity_ = g; }

void BuiltinBackend::step(float dt) {
    for (auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        Body& b = *bp;
        if (b.desc.kind != BodyKind::Dynamic) {
            if (b.desc.kind == BodyKind::Kinematic) {
                b.position = b.desc.position;
                b.rotation = b.desc.rotation;
                const Vec3 e = b.aabb.extents();
                b.aabb = AABB{b.position - e, b.position + e};
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
            b.rotation = normalize(b.rotation * Quat::fromAxisAngle(
                normalize(b.angularVelocity), length(b.angularVelocity) * dt));
        b.forceAccum = Vec3::zero();
        b.impulseAccum = Vec3::zero();
        const Vec3 e = b.aabb.extents();
        b.aabb = AABB{b.position - e, b.position + e};

        // ВАЖНО: здесь НЕТ неявного пола на y=0.
        //
        // Раньше встроенный симулятор ловил любое тело на нулевой высоте. Это
        // удобно для демо, но делает бэкенд принципиально несовместимым с
        // Jolt: тело, которое по матрице коллизий обязано провалиться сквозь
        // платформу, всё равно зависало на невидимой плоскости. Контрактный
        // тест физики ловит такое расхождение. Пол теперь задаётся обычным
        // статическим телом, как и в Jolt.
    }

    contacts_.clear();
    std::vector<Body*> awake;
    awake.reserve(bodies_.size());
    for (auto& bp : bodies_)
        if (bp && bp->alive) awake.push_back(bp.get());

    for (std::size_t i = 0; i < awake.size(); ++i) {
        for (std::size_t j = i + 1; j < awake.size(); ++j) {
            Body& a = *awake[i];
            Body& b = *awake[j];
            if (matrix_ && !matrix_->collides(a.desc.group, b.desc.group)) continue;
            // Маски тел: столкновение происходит, только если каждое тело
            // готово сталкиваться с группой другого.
            if (!(a.desc.mask & (1u << std::min<std::uint16_t>(b.desc.group, 15))) ||
                !(b.desc.mask & (1u << std::min<std::uint16_t>(a.desc.group, 15)))) continue;
            if (a.desc.kind != BodyKind::Dynamic && b.desc.kind != BodyKind::Dynamic) continue;
            if (!a.aabb.intersects(b.aabb)) continue;

            // Глубина проникновения по каждой оси; ось с минимальным
            // перекрытием и есть нормаль контакта (minimum translation vector).
            // Раньше нормаль бралась как направление между центрами — для
            // сильно вытянутых тел (пол, стена) это давало почти
            // горизонтальную нормаль, и тело «съезжало» вбок вместо того,
            // чтобы остановиться.
            const Vec3 ovMin{
                std::min(a.aabb.maxPt.x, b.aabb.maxPt.x) - std::max(a.aabb.minPt.x, b.aabb.minPt.x),
                std::min(a.aabb.maxPt.y, b.aabb.maxPt.y) - std::max(a.aabb.minPt.y, b.aabb.minPt.y),
                std::min(a.aabb.maxPt.z, b.aabb.maxPt.z) - std::max(a.aabb.minPt.z, b.aabb.minPt.z)};
            if (ovMin.x <= 0 || ovMin.y <= 0 || ovMin.z <= 0) continue;

            Vec3 n{0, 0, 0};
            Real depth = ovMin.x;
            if (ovMin.x <= ovMin.y && ovMin.x <= ovMin.z) {
                n = Vec3{b.position.x >= a.position.x ? 1.0f : -1.0f, 0, 0};
                depth = ovMin.x;
            } else if (ovMin.y <= ovMin.z) {
                n = Vec3{0, b.position.y >= a.position.y ? 1.0f : -1.0f, 0};
                depth = ovMin.y;
            } else {
                n = Vec3{0, 0, b.position.z >= a.position.z ? 1.0f : -1.0f};
                depth = ovMin.z;
            }

            // Нормальная составляющая относительной скорости: сближение < 0.
            const Vec3 rel = b.linearVelocity - a.linearVelocity;
            const Real vn = dot(rel, n);

            ContactEvent ev;
            ev.a = a.desc.entity;
            ev.b = b.desc.entity;
            ev.position = (a.position + b.position) * 0.5f;
            ev.normal = n;
            ev.impulse = std::fabs(vn);
            ev.began = true;
            contacts_.push_back(ev);

            if (a.desc.trigger || b.desc.trigger) continue;   // триггер не толкает

            const bool aDyn = a.desc.kind == BodyKind::Dynamic;
            const bool bDyn = b.desc.kind == BodyKind::Dynamic;
            const Real invA = aDyn ? 1.0f / std::max(0.0001f, a.desc.mass) : 0.0f;
            const Real invB = bDyn ? 1.0f / std::max(0.0001f, b.desc.mass) : 0.0f;
            const Real invSum = invA + invB;
            if (invSum <= 0) continue;

            // Разводим тела пропорционально обратным массам (statics не двигаются).
            const Vec3 correction = n * (depth / invSum);
            if (aDyn) { a.position -= correction * invA; }
            if (bDyn) { b.position += correction * invB; }

            if (vn < 0) {
                const Real e = std::min(a.desc.restitution, b.desc.restitution);
                const Real jn = -(1.0f + e) * vn / invSum;
                const Vec3 impulse = n * jn;
                if (aDyn) a.linearVelocity -= impulse * invA;
                if (bDyn) b.linearVelocity += impulse * invB;

                // Кулоново трение по касательной составляющей.
                const Vec3 relAfter = b.linearVelocity - a.linearVelocity;
                const Vec3 tangent = relAfter - n * dot(relAfter, n);
                if (lengthSq(tangent) > kEpsilon) {
                    const Real mu = std::sqrt(a.desc.friction * b.desc.friction);
                    const Vec3 tdir = normalize(tangent);
                    const Real jt = std::min(length(tangent) / invSum, mu * jn);
                    const Vec3 fr = tdir * jt;
                    if (aDyn) a.linearVelocity += fr * invA;
                    if (bDyn) b.linearVelocity -= fr * invB;
                }
            }

            const Vec3 ea = a.aabb.extents();
            const Vec3 eb = b.aabb.extents();
            a.aabb = AABB{a.position - ea, a.position + ea};
            b.aabb = AABB{b.position - eb, b.position + eb};
        }
    }
}

/// Нормаль грани AABB в точке попадания: берём ось, по которой точка ближе
/// всего к своей грани. Нормаль «из центра тела» указывала бы внутрь
/// геометрии и ломала отражения и скольжение вдоль стен.
Vec3 BuiltinBackend::faceNormal(const AABB& box, const Vec3& p) noexcept {
    const Vec3 c = box.center();
    const Vec3 e = box.extents();
    const Vec3 d = p - c;
    const Real dx = std::fabs(std::fabs(d.x) - e.x);
    const Real dy = std::fabs(std::fabs(d.y) - e.y);
    const Real dz = std::fabs(std::fabs(d.z) - e.z);
    if (dx <= dy && dx <= dz) return Vec3{d.x >= 0 ? 1.0f : -1.0f, 0, 0};
    if (dy <= dz)             return Vec3{0, d.y >= 0 ? 1.0f : -1.0f, 0};
    return Vec3{0, 0, d.z >= 0 ? 1.0f : -1.0f};
}

RaycastHit BuiltinBackend::raycast(const Ray& r, Real maxDistance, std::uint16_t mask) const {
    RaycastHit best;
    best.distance = maxDistance;
    for (const auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        if (!groupAllowed(bp->desc.group, mask)) continue;
        if (auto t = intersectRayAABB(r, bp->aabb)) {
            if (*t >= 0 && *t <= best.distance) {
                best.hit = true;
                best.distance = *t;
                best.position = r.at(*t);
                best.body = bp->id;
                best.entity = bp->desc.entity;
                best.normal = faceNormal(bp->aabb, best.position);
            }
        }
    }
    if (!best.hit) best.distance = 0;
    return best;
}

std::vector<BodyId> BuiltinBackend::overlapSphere(const Vec3& center, Real radius,
                                                  std::uint16_t mask) const {
    std::vector<BodyId> out;
    const AABB sphere{center - Vec3(radius), center + Vec3(radius)};
    for (const auto& bp : bodies_) {
        if (!bp || !bp->alive) continue;
        if (!groupAllowed(bp->desc.group, mask)) continue;
        if (bp->aabb.intersects(sphere) &&
            distance(bp->position, center) <= radius + length(bp->aabb.extents()))
            out.push_back(bp->id);
    }
    return out;
}

} // namespace lv::physics
