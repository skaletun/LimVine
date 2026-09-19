/**
 * @file    physics_tests.cpp
 * @brief   Контрактные тесты физики, прогоняемые на КАЖДОМ бэкенде.
 *
 * @details Главная идея файла — не «проверить встроенный симулятор», а
 *          зафиксировать контракт @c IPhysicsBackend и прогнать по нему все
 *          доступные реализации. Один и тот же набор проверок исполняется для
 *          builtin и (если движок собран с @c LV_WITH_JOLT) для Jolt, поэтому
 *          расхождение в поведении движков всплывает в CI, а не в игре.
 *
 *          Проверки специально сформулированы как физические инварианты
 *          (энергия не растёт из ниоткуда, тело не проваливается сквозь пол,
 *          маска коллизий соблюдается), а не как сверка с конкретными
 *          числами: у Jolt и у встроенного солвера разные интеграторы, и
 *          требовать побитового совпадения было бы неправильно.
 */
#include "physics/Physics.h"
#include "physics/backends/PhysicsBackend.h"
#include "ecs/World.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace lv;
using namespace lv::physics;

namespace {

int g_passed = 0, g_failed = 0;
const char* g_backend = "";

void check(bool cond, const std::string& what) {
    if (cond) { ++g_passed; std::printf("  PASS  [%s] %s\n", g_backend, what.c_str()); }
    else      { ++g_failed; std::printf("  FAIL  [%s] %s\n", g_backend, what.c_str()); }
}

/// Прогнать `frames` кадров по 1/60 с.
void simulate(PhysicsWorld& w, ecs::World& world, int frames, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < frames; ++i) w.step(world, dt);
}

BodyDesc boxDesc(BodyKind kind, const Vec3& pos, const Vec3& half = {0.5f, 0.5f, 0.5f}) {
    BodyDesc d;
    d.kind = kind;
    d.position = pos;
    d.shape.kind = ShapeKind::Box;
    d.shape.halfExtents = half;
    return d;
}

/// Большая статическая плита как «пол» на y = 0 (верхняя грань).
BodyId makeGround(PhysicsWorld& w) {
    BodyDesc d = boxDesc(BodyKind::Static, {0, -1.0f, 0}, {50.0f, 1.0f, 50.0f});
    d.friction = 0.6f;
    return w.createBody(d);
}

// ---------------------------------------------------------------------------
//  Контракт, общий для всех бэкендов
// ---------------------------------------------------------------------------
void runContract(bool preferJolt) {
    ecs::World world;
    PhysicsWorld phys;
    check(phys.init(4096, preferJolt), "init succeeds");
    g_backend = phys.backendName();

    std::printf("\n== [%s] Жизненный цикл тел ==\n", g_backend);
    {
        const BodyId a = phys.createBody(boxDesc(BodyKind::Dynamic, {0, 5, 0}));
        const BodyId b = phys.createBody(boxDesc(BodyKind::Static, {10, 0, 0}));
        check(a != kInvalidBody && b != kInvalidBody, "createBody returns valid ids");
        check(a != b, "ids are unique");
        check(phys.isValid(a) && phys.isValid(b), "bodies report valid");
        check(phys.bodyCount() == 2, "bodyCount counts live bodies");

        phys.destroyBody(a);
        check(!phys.isValid(a), "destroyed body is invalid");
        check(phys.bodyCount() == 1, "bodyCount drops after destroy");

        // Идентификаторы переиспользуются — это не должно воскрешать тело.
        const BodyId c = phys.createBody(boxDesc(BodyKind::Dynamic, {0, 5, 0}));
        check(phys.isValid(c), "recycled id is valid again");
        phys.destroyBody(b);
        phys.destroyBody(c);
        check(phys.bodyCount() == 0, "all bodies removed");

        check(!phys.isValid(kInvalidBody), "kInvalidBody is never valid");
        check(phys.linearVelocity(kInvalidBody) == Vec3::zero(),
              "queries on invalid id are safe");
        phys.destroyBody(kInvalidBody);   // не должно падать
        check(true, "destroying an invalid id does not crash");
    }

    std::printf("\n== [%s] Гравитация и падение ==\n", g_backend);
    {
        (void)makeGround(phys);
        const BodyId ball = phys.createBody(boxDesc(BodyKind::Dynamic, {0, 10, 0}));

        simulate(phys, world, 5);
        check(phys.linearVelocity(ball).y < 0.0f, "gravity accelerates body downward");

        simulate(phys, world, 300);
        const Vec3 rest = phys.position(ball);
        check(rest.y > -0.5f, "body does not tunnel through the floor");
        check(rest.y < 1.5f, "body actually fell down to the floor");
        check(std::fabs(phys.linearVelocity(ball).y) < 1.0f, "body comes to rest");
        check(std::fabs(rest.x) < 0.5f && std::fabs(rest.z) < 0.5f,
              "body does not drift sideways without forces");

        // Выключенная гравитация => свободное тело висит.
        phys.setGravity(Vec3::zero());
        const BodyId floater = phys.createBody(boxDesc(BodyKind::Dynamic, {20, 10, 0}));
        simulate(phys, world, 60);
        check(std::fabs(phys.position(floater).y - 10.0f) < 0.1f,
              "zero gravity leaves a body floating");
        phys.setGravity({0, -9.81f, 0});
        phys.destroyBody(floater);
        phys.destroyBody(ball);
    }

    std::printf("\n== [%s] Статика и кинематика ==\n", g_backend);
    {
        const BodyId wall = phys.createBody(boxDesc(BodyKind::Static, {30, 3, 0}));
        simulate(phys, world, 60);
        check(phys.position(wall).y > 2.9f, "static body ignores gravity");

        const BodyId lift = phys.createBody(boxDesc(BodyKind::Kinematic, {40, 0, 0}));
        simulate(phys, world, 30);
        check(std::fabs(phys.position(lift).y) < 0.05f, "kinematic body ignores gravity");

        phys.setKinematicTarget(lift, {40, 2, 0}, Quat{});
        simulate(phys, world, 30);
        check(phys.position(lift).y > 1.0f, "kinematic body follows its target");

        phys.destroyBody(wall);
        phys.destroyBody(lift);
    }

    std::printf("\n== [%s] Импульсы, скорости, телепорт ==\n", g_backend);
    {
        phys.setGravity(Vec3::zero());
        const BodyId b = phys.createBody(boxDesc(BodyKind::Dynamic, {0, 20, 0}));

        phys.setLinearVelocity(b, {5, 0, 0});
        check(std::fabs(phys.linearVelocity(b).x - 5.0f) < 0.01f,
              "setLinearVelocity is observable immediately");
        simulate(phys, world, 60);
        check(phys.position(b).x > 3.0f, "velocity moves the body along +X");

        phys.setLinearVelocity(b, Vec3::zero());
        phys.applyImpulse(b, {0, 0, 10}, phys.position(b));
        simulate(phys, world, 1);
        check(phys.linearVelocity(b).z > 0.5f, "applyImpulse adds velocity along +Z");

        phys.teleportBody(b, {0, 50, 0}, Quat{});
        check(phys.position(b).y > 49.0f, "teleportBody moves the body");
        check(length(phys.linearVelocity(b)) < 0.01f, "teleportBody clears velocity");

        phys.setGravity({0, -9.81f, 0});
        phys.destroyBody(b);
    }

    std::printf("\n== [%s] Контакты ==\n", g_backend);
    {
        const BodyId dropper = phys.createBody(boxDesc(BodyKind::Dynamic, {0, 3, 0}));
        bool sawContact = false;
        for (int i = 0; i < 240 && !sawContact; ++i) {
            phys.step(world, 1.0f / 60.0f);
            for (const ContactEvent& e : phys.contacts()) {
                if (e.began) sawContact = true;
            }
        }
        check(sawContact, "falling body generates a contact with the ground");
        phys.destroyBody(dropper);
    }

    std::printf("\n== [%s] Raycast ==\n", g_backend);
    {
        BodyDesc d = boxDesc(BodyKind::Static, {0, 20, 0}, {1, 1, 1});
        d.group = 2;
        const BodyId target = phys.createBody(d);
        simulate(phys, world, 1);

        const Ray down{{0, 30, 0}, {0, -1, 0}};
        const RaycastHit hit = phys.raycast(down, 100.0f);
        check(hit.hit, "raycast hits the box below");
        check(hit.body == target, "raycast reports the right body");
        check(hit.distance > 8.0f && hit.distance < 10.0f,
              "raycast distance is the distance to the top face");
        check(hit.normal.y > 0.5f, "raycast normal points back at the ray");

        const RaycastHit shortRay = phys.raycast(down, 3.0f);
        check(!shortRay.hit, "raycast respects maxDistance");

        const Ray sideways{{0, 30, 0}, {1, 0, 0}};
        check(!phys.raycast(sideways, 100.0f).hit, "raycast misses when pointing away");

        // Маска: группа 2 => бит 1<<2.
        check(phys.raycast(down, 100.0f, 1u << 2).hit, "raycast passes a matching mask");
        check(!phys.raycast(down, 100.0f, 1u << 5).hit, "raycast filtered out by mask");

        phys.destroyBody(target);
    }

    std::printf("\n== [%s] OverlapSphere ==\n", g_backend);
    {
        BodyDesc d = boxDesc(BodyKind::Static, {100, 0, 0}, {0.5f, 0.5f, 0.5f});
        d.group = 3;
        const BodyId near1 = phys.createBody(d);
        d.position = {101.0f, 0, 0};
        const BodyId near2 = phys.createBody(d);
        d.position = {140.0f, 0, 0};
        const BodyId far1 = phys.createBody(d);
        simulate(phys, world, 1);

        const std::vector<BodyId> found = phys.overlapSphere({100.5f, 0, 0}, 3.0f);
        bool has1 = false, has2 = false, hasFar = false;
        for (BodyId id : found) {
            has1   |= (id == near1);
            has2   |= (id == near2);
            hasFar |= (id == far1);
        }
        check(has1 && has2, "overlapSphere finds both nearby bodies");
        check(!hasFar, "overlapSphere excludes the distant body");
        check(phys.overlapSphere({1000, 1000, 1000}, 1.0f).empty(),
              "overlapSphere in empty space returns nothing");
        check(phys.overlapSphere({100.5f, 0, 0}, 3.0f, 1u << 7).empty(),
              "overlapSphere respects the mask");

        phys.destroyBody(near1);
        phys.destroyBody(near2);
        phys.destroyBody(far1);
    }

    std::printf("\n== [%s] Матрица коллизий ==\n", g_backend);
    {
        // Группа 4 не сталкивается с группой 5 => тело проваливается сквозь плиту.
        phys.collisionMatrix().setCollides(4, 5, false);

        BodyDesc plate = boxDesc(BodyKind::Static, {200, 0, 0}, {5, 0.5f, 5});
        plate.group = 4;
        const BodyId p = phys.createBody(plate);

        BodyDesc faller = boxDesc(BodyKind::Dynamic, {200, 5, 0});
        faller.group = 5;
        const BodyId f = phys.createBody(faller);

        simulate(phys, world, 180);
        check(phys.position(f).y < -1.0f, "bodies in non-colliding groups pass through");

        phys.collisionMatrix().setCollides(4, 5, true);
        phys.teleportBody(f, {200, 5, 0}, Quat{});
        simulate(phys, world, 180);
        check(phys.position(f).y > -0.5f, "re-enabling the pair restores collision");

        phys.destroyBody(p);
        phys.destroyBody(f);
    }

    std::printf("\n== [%s] Фиксированный шаг ==\n", g_backend);
    {
        phys.setGravity({0, -10.0f, 0});
        const BodyId a = phys.createBody(boxDesc(BodyKind::Dynamic, {300, 100, 0}));

        // Один кадр в 1 с и 60 кадров по 1/60 с должны дать близкий результат:
        // именно для этого существует аккумулятор фиксированного шага.
        phys.setMaxSubSteps(120);
        phys.step(world, 1.0f);
        const Real yBig = phys.position(a).y;

        phys.teleportBody(a, {300, 100, 0}, Quat{});
        simulate(phys, world, 60);
        const Real ySmall = phys.position(a).y;
        check(std::fabs(yBig - ySmall) < 0.5f,
              "one big frame matches 60 small frames (fixed step)");

        // Защита от «спирали смерти»: огромный dt ограничен maxSubSteps.
        phys.setMaxSubSteps(2);
        phys.teleportBody(a, {300, 100, 0}, Quat{});
        phys.step(world, 10.0f);
        check(phys.position(a).y > 99.0f, "maxSubSteps caps work done in one frame");
        phys.setMaxSubSteps(5);

        phys.setGravity({0, -9.81f, 0});
        phys.destroyBody(a);
    }

    std::printf("\n== [%s] Интеграция с ECS ==\n", g_backend);
    {
        ecs::World w2;
        PhysicsWorld p2;
        p2.init(256, preferJolt);
        attachToWorld(p2, w2);

        (void)makeGround(p2);

        const ecs::Entity e = w2.registry().create();
        w2.registry().add<ecs::Transform>(e, ecs::Transform{});
        {
            ecs::Transform* t = w2.registry().get<ecs::Transform>(e);
            t->position = {0, 10, 0};
        }
        RigidBody rb;
        rb.body = p2.createBody(boxDesc(BodyKind::Dynamic, {0, 10, 0}));
        rb.syncTransformToECS = true;
        w2.registry().add<RigidBody>(e, rb);

        for (int i = 0; i < 120; ++i) w2.tick(1.0f / 60.0f);

        const ecs::Transform* t = w2.registry().get<ecs::Transform>(e);
        check(t->position.y < 9.0f, "physics writes the fallen position back into Transform");
        check(std::fabs(t->position.y - p2.position(rb.body).y) < 0.01f,
              "Transform matches the body position");

        // syncTransformToECS = false => физика не трогает Transform.
        const ecs::Entity ghost = w2.registry().create();
        w2.registry().add<ecs::Transform>(ghost, ecs::Transform{});
        w2.registry().get<ecs::Transform>(ghost)->position = {5, 10, 0};
        RigidBody rb2;
        rb2.body = p2.createBody(boxDesc(BodyKind::Dynamic, {5, 10, 0}));
        rb2.syncTransformToECS = false;
        w2.registry().add<RigidBody>(ghost, rb2);

        for (int i = 0; i < 120; ++i) w2.tick(1.0f / 60.0f);
        check(std::fabs(w2.registry().get<ecs::Transform>(ghost)->position.y - 10.0f) < 0.01f,
              "syncTransformToECS=false keeps Transform untouched");

        // Статическое тело: источник истины — Transform.
        const ecs::Entity platform = w2.registry().create();
        w2.registry().add<ecs::Transform>(platform, ecs::Transform{});
        w2.registry().get<ecs::Transform>(platform)->position = {50, 4, 0};
        RigidBody rb3;
        rb3.body = p2.createBody(boxDesc(BodyKind::Static, {0, 0, 0}));
        w2.registry().add<RigidBody>(platform, rb3);
        w2.tick(1.0f / 60.0f);
        check(std::fabs(p2.position(rb3.body).x - 50.0f) < 0.01f,
              "static body follows Transform from ECS");
    }

    std::printf("\n== [%s] Формы ==\n", g_backend);
    {
        ecs::World w3;
        PhysicsWorld p3;
        p3.init(256, preferJolt);
        (void)makeGround(p3);

        BodyDesc sphere = boxDesc(BodyKind::Dynamic, {0, 6, 0});
        sphere.shape.kind = ShapeKind::Sphere;
        sphere.shape.radius = 0.5f;
        const BodyId s = p3.createBody(sphere);
        check(s != kInvalidBody, "sphere shape is accepted");

        BodyDesc capsule = boxDesc(BodyKind::Dynamic, {3, 6, 0});
        capsule.shape.kind = ShapeKind::Capsule;
        capsule.shape.radius = 0.4f;
        capsule.shape.halfHeight = 0.8f;
        const BodyId c = p3.createBody(capsule);
        check(c != kInvalidBody, "capsule shape is accepted");

        BodyDesc cyl = boxDesc(BodyKind::Dynamic, {6, 6, 0});
        cyl.shape.kind = ShapeKind::Cylinder;
        cyl.shape.radius = 0.4f;
        cyl.shape.halfHeight = 0.6f;
        check(p3.createBody(cyl) != kInvalidBody, "cylinder shape is accepted");

        BodyDesc hull = boxDesc(BodyKind::Dynamic, {9, 6, 0});
        hull.shape.kind = ShapeKind::ConvexHull;
        hull.shape.points = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
                             {0.0f, -0.5f, 0.5f},   {0.0f, 0.5f, 0.0f}};
        check(p3.createBody(hull) != kInvalidBody, "convex hull with 4 points is accepted");

        BodyDesc badHull = hull;
        badHull.shape.points = {{0, 0, 0}, {1, 0, 0}};
        check(p3.createBody(badHull) == kInvalidBody, "degenerate convex hull is rejected");

        BodyDesc mesh = boxDesc(BodyKind::Static, {12, 0, 0});
        mesh.shape.kind = ShapeKind::TriangleMesh;
        mesh.shape.meshVerts = {{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}};
        mesh.shape.meshIndices = {0, 1, 2, 0, 2, 3};
        check(p3.createBody(mesh) != kInvalidBody, "triangle mesh is accepted");

        BodyDesc badMesh = mesh;
        badMesh.shape.meshIndices = {0};
        check(p3.createBody(badMesh) == kInvalidBody, "triangle mesh without a triangle is rejected");

        simulate(p3, w3, 240);
        check(p3.position(s).y > -1.0f, "sphere lands on the ground");
        check(p3.position(c).y > -1.0f, "capsule lands on the ground");
    }

    std::printf("\n== [%s] Отладочная геометрия и shutdown ==\n", g_backend);
    {
        ecs::World w4;
        PhysicsWorld p4;
        p4.init(64, preferJolt);
        (void)p4.createBody(boxDesc(BodyKind::Static, {0, 0, 0}));
        (void)p4.createBody(boxDesc(BodyKind::Static, {5, 0, 0}));

        std::vector<Vec3> lines;
        p4.collectDebugLines(lines, Color{});
        check(lines.size() == 2 * 24, "debug lines: 12 edges x 2 points per body");

        p4.shutdown();
        check(p4.bodyCount() == 0, "shutdown clears all bodies");
        check(std::string(p4.backendName()) == "none", "backend released on shutdown");
        check(p4.init(64, preferJolt), "world can be re-initialised after shutdown");
    }
}

} // namespace

int main() {
    std::printf("\n#### Бэкенд: builtin ####\n");
    runContract(/*preferJolt=*/false);

    if (joltAvailable()) {
        std::printf("\n#### Бэкенд: jolt ####\n");
        runContract(/*preferJolt=*/true);
    } else {
        std::printf("\n(собрано без LV_WITH_JOLT — набор для Jolt пропущен)\n");
    }

    std::printf("\n== Итог: %d passed, %d failed ==\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
