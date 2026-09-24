/**
 * @file    hierarchy_tests.cpp
 * @brief   Тесты иерархии сущностей (Parent/Children/WorldTransform).
 *
 * Проверяется: прикрепление/открепление, каскадное вычисление world-матриц,
 * защита от циклов, keepWorldPosition, отсоединение «сирот» и стабильность
 * при структурных изменениях во время обхода.
 */
#include "ecs/Hierarchy.h"
#include "ecs/World.h"

#include <cmath>
#include <cstdio>

using namespace lv;
using namespace lv::ecs;

namespace {

int g_passed = 0, g_failed = 0;

void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
void section(const char* s) { std::printf("\n== %s ==\n", s); }

bool near(Real a, Real b, Real eps = 1e-4f) { return std::fabs(a - b) < eps; }
bool nearV(const Vec3& a, const Vec3& b, Real eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

Entity makeAt(Registry& reg, Vec3 pos, Vec3 scale = Vec3::one()) {
    Entity e = reg.create();
    Transform* t = reg.add<Transform>(e);
    t->position = pos;
    t->scale = scale;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
static void testBasicParenting() {
    section("базовое прикрепление");
    Registry reg;
    registerHierarchyComponents();

    Entity parent = makeAt(reg, {10, 0, 0});
    Entity child  = makeAt(reg, {1, 0, 0});

    check(!reg.has<Parent>(child), "новая сущность не имеет родителя");

    setParent(reg, child, parent);
    check(reg.has<Parent>(child), "после setParent появился компонент Parent");
    check(reg.get<Parent>(child)->entity.index == parent.index, "Parent указывает на верную сущность");
    check(reg.has<Children>(parent), "у родителя появился список детей");
    check(reg.get<Children>(parent)->entities.size() == 1, "в списке ровно один ребёнок");

    updateWorldTransforms(reg);
    const WorldTransform* cwt = reg.get<WorldTransform>(child);
    check(cwt != nullptr, "ребёнок получил WorldTransform");
    check(nearV(cwt->worldPosition, Vec3{11, 0, 0}),
          "world-позиция ребёнка = родитель + локальная (10 + 1 = 11)");

    const WorldTransform* pwt = reg.get<WorldTransform>(parent);
    check(pwt && nearV(pwt->worldPosition, Vec3{10, 0, 0}), "корень сохраняет свою позицию");
}

// ---------------------------------------------------------------------------
static void testDetach() {
    section("открепление");
    Registry reg;
    Entity parent = makeAt(reg, {5, 0, 0});
    Entity child  = makeAt(reg, {2, 0, 0});
    setParent(reg, child, parent);
    updateWorldTransforms(reg);
    check(nearV(reg.get<WorldTransform>(child)->worldPosition, Vec3{7, 0, 0}), "до открепления world = 7");

    setParent(reg, child, kNullEntity);
    check(!reg.has<Parent>(child), "Parent снят");
    check(reg.get<Children>(parent)->entities.empty(), "ребёнок удалён из списка родителя");

    updateWorldTransforms(reg);
    check(nearV(reg.get<WorldTransform>(child)->worldPosition, Vec3{2, 0, 0}),
          "после открепления world-позиция = локальной");
}

// ---------------------------------------------------------------------------
static void testDeepChain() {
    section("глубокая цепочка (A -> B -> C -> D)");
    Registry reg;
    Entity a = makeAt(reg, {1, 0, 0});
    Entity b = makeAt(reg, {2, 0, 0});
    Entity c = makeAt(reg, {4, 0, 0});
    Entity d = makeAt(reg, {8, 0, 0});
    setParent(reg, b, a);
    setParent(reg, c, b);
    setParent(reg, d, c);

    const std::size_t touched = updateWorldTransforms(reg);
    check(touched == 4, "обработаны все четыре сущности цепочки");
    check(nearV(reg.get<WorldTransform>(a)->worldPosition, Vec3{1, 0, 0}), "A = 1");
    check(nearV(reg.get<WorldTransform>(b)->worldPosition, Vec3{3, 0, 0}), "B = 1+2 = 3");
    check(nearV(reg.get<WorldTransform>(c)->worldPosition, Vec3{7, 0, 0}), "C = 3+4 = 7");
    check(nearV(reg.get<WorldTransform>(d)->worldPosition, Vec3{15, 0, 0}), "D = 7+8 = 15");

    // Двигаем корень — вся ветка обязана сдвинуться.
    reg.get<Transform>(a)->position = Vec3{100, 0, 0};
    updateWorldTransforms(reg);
    check(nearV(reg.get<WorldTransform>(d)->worldPosition, Vec3{114, 0, 0}),
          "сдвиг корня каскадно двигает всю ветку (100+2+4+8)");
}

// ---------------------------------------------------------------------------
static void testScalePropagation() {
    section("наследование масштаба");
    Registry reg;
    Entity parent = makeAt(reg, {0, 0, 0}, Vec3{2, 2, 2});
    Entity child  = makeAt(reg, {3, 0, 0}, Vec3{0.5f, 0.5f, 0.5f});
    setParent(reg, child, parent);
    updateWorldTransforms(reg);

    const WorldTransform* cwt = reg.get<WorldTransform>(child);
    check(nearV(cwt->worldScale, Vec3{1, 1, 1}), "world-масштаб перемножается (2 * 0.5 = 1)");
    check(nearV(cwt->worldPosition, Vec3{6, 0, 0}),
          "локальная позиция ребёнка масштабируется родителем (3 * 2 = 6)");
}

// ---------------------------------------------------------------------------
static void testCycleProtection() {
    section("защита от циклов");
    Registry reg;
    Entity a = makeAt(reg, {0, 0, 0});
    Entity b = makeAt(reg, {0, 0, 0});
    Entity c = makeAt(reg, {0, 0, 0});
    setParent(reg, b, a);
    setParent(reg, c, b);

    bool selfThrew = false;
    try { setParent(reg, a, a); } catch (const HierarchyCycleError&) { selfThrew = true; }
    check(selfThrew, "сущность не может быть родителем самой себя");

    bool cycleThrew = false;
    try { setParent(reg, a, c); } catch (const HierarchyCycleError&) { cycleThrew = true; }
    check(cycleThrew, "нельзя сделать потомка родителем предка (A -> B -> C -> A)");

    // Иерархия осталась валидной.
    updateWorldTransforms(reg);
    check(reg.get<Parent>(b)->entity.index == a.index, "иерархия не повреждена после отказа");
}

// ---------------------------------------------------------------------------
static void testKeepWorldPosition() {
    section("keepWorldPosition");
    Registry reg;
    Entity parent = makeAt(reg, {10, 5, 0});
    Entity child  = makeAt(reg, {3, 0, 0});
    updateWorldTransforms(reg);

    setParent(reg, child, parent, /*keepWorldPosition=*/true);
    updateWorldTransforms(reg);

    const WorldTransform* cwt = reg.get<WorldTransform>(child);
    check(nearV(cwt->worldPosition, Vec3{3, 0, 0}, 1e-3f),
          "world-позиция сохранилась при смене родителя");
    check(nearV(reg.get<Transform>(child)->position, Vec3{-7, -5, 0}, 1e-3f),
          "локальная позиция пересчитана относительно нового родителя");
}

// ---------------------------------------------------------------------------
static void testOrphanCleanup() {
    section("отсоединение сирот");
    Registry reg;
    Entity parent = makeAt(reg, {0, 0, 0});
    Entity child  = makeAt(reg, {1, 0, 0});
    setParent(reg, child, parent);

    reg.destroy(parent);
    updateWorldTransforms(reg);
    check(!reg.has<Parent>(child), "ребёнок мёртвого родителя автоматически откреплён");
    check(reg.alive(child), "сам ребёнок остался жив");
}

// ---------------------------------------------------------------------------
static void testMultipleChildren() {
    section("несколько детей и порядок обхода");
    Registry reg;
    Entity parent = makeAt(reg, {10, 0, 0});
    Entity c1 = makeAt(reg, {1, 0, 0});
    Entity c2 = makeAt(reg, {2, 0, 0});
    Entity c3 = makeAt(reg, {3, 0, 0});
    setParent(reg, c1, parent);
    setParent(reg, c2, parent);
    setParent(reg, c3, parent);

    check(reg.get<Children>(parent)->entities.size() == 3, "у родителя три ребёнка");
    updateWorldTransforms(reg);
    check(nearV(reg.get<WorldTransform>(c1)->worldPosition, Vec3{11, 0, 0}), "ребёнок 1 = 11");
    check(nearV(reg.get<WorldTransform>(c2)->worldPosition, Vec3{12, 0, 0}), "ребёнок 2 = 12");
    check(nearV(reg.get<WorldTransform>(c3)->worldPosition, Vec3{13, 0, 0}), "ребёнок 3 = 13");

    // Повторный setParent того же ребёнка не должен дублировать запись.
    setParent(reg, c1, parent);
    check(reg.get<Children>(parent)->entities.size() == 3, "повторное прикрепление не дублирует ребёнка");
}

// ---------------------------------------------------------------------------
static void testStructuralStability() {
    section("устойчивость к структурным изменениям");
    // Регрессионный тест: updateWorldTransforms добавляет WorldTransform во
    // время обхода, что переселяет сущности между архетипами. Если собирать
    // корни и итерировать одновременно, итератор указывает в освобождённую
    // память (наблюдалось как segfault на 780-м кадре farming_iso).
    Registry reg;
    for (int i = 0; i < 200; ++i) makeAt(reg, {static_cast<Real>(i), 0, 0});

    const std::size_t first = updateWorldTransforms(reg);
    check(first == 200, "первый проход обработал все 200 сущностей (добавив WorldTransform)");

    const std::size_t second = updateWorldTransforms(reg);
    check(second == 200, "повторный проход стабилен");

    // Добавляем ещё сущности между проходами.
    for (int i = 0; i < 50; ++i) makeAt(reg, {0, static_cast<Real>(i), 0});
    const std::size_t third = updateWorldTransforms(reg);
    check(third == 250, "новые сущности подхвачены следующим проходом");
}

// ---------------------------------------------------------------------------
static void testWorldIntegration() {
    section("интеграция с World (фаза PostPhysics)");
    World w;
    Registry& reg = w.registry();

    Entity parent = makeAt(reg, {4, 0, 0});
    Entity child  = makeAt(reg, {1, 0, 0});
    setParent(reg, child, parent);

    bool found = false;
    for (const SystemDesc& s : w.systems())
        if (s.name == "Hierarchy.UpdateWorldTransforms" && s.phase == Phase::PostPhysics) found = true;
    check(found, "World регистрирует систему иерархии в фазе PostPhysics");

    w.tick(1.0f / 60.0f);
    const WorldTransform* cwt = reg.get<WorldTransform>(child);
    check(cwt && nearV(cwt->worldPosition, Vec3{5, 0, 0}),
          "world-матрицы обновляются автоматически каждый кадр");

    reg.get<Transform>(parent)->position = Vec3{40, 0, 0};
    w.tick(1.0f / 60.0f);
    check(nearV(reg.get<WorldTransform>(child)->worldPosition, Vec3{41, 0, 0}),
          "следующий кадр подхватывает изменение родителя");
}

// ---------------------------------------------------------------------------
static void testComputeWorldMatrix() {
    section("computeWorldMatrix");
    Registry reg;
    Entity lone = makeAt(reg, {7, 8, 9});
    const Mat4 m = computeWorldMatrix(reg, lone);
    check(nearV(extractTranslation(m), Vec3{7, 8, 9}),
          "работает и без WorldTransform (вычисляет из Transform)");

    Entity parent = makeAt(reg, {1, 1, 1});
    Entity child  = makeAt(reg, {2, 2, 2});
    setParent(reg, child, parent);
    updateWorldTransforms(reg);
    const Mat4 cm = computeWorldMatrix(reg, child);
    check(nearV(extractTranslation(cm), Vec3{3, 3, 3}), "возвращает кэш WorldTransform после обновления");
}

// ---------------------------------------------------------------------------
static void testMathHelpers() {
    section("матричные хелперы (inverse / extract*)");
    const Vec3 t{3, -4, 5};
    const Quat r = Quat::fromEuler(radians(30.f), radians(45.f), radians(0.f));
    const Vec3 s{2, 2, 2};
    const Mat4 m = Mat4::trs(t, r, s);

    check(nearV(extractTranslation(m), t), "extractTranslation возвращает смещение");
    check(nearV(extractScale(m), s, 1e-3f), "extractScale возвращает масштаб");

    const Quat er = extractRotation(m);
    const Vec3 probe{1, 0, 0};
    check(nearV(rotate(er, probe), rotate(r, probe), 1e-3f),
          "extractRotation восстанавливает вращение (проверено поворотом вектора)");

    const Mat4 inv = inverse(m);
    const Mat4 id = m * inv;
    bool isIdentity = true;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!near(id.m[i * 4 + j], i == j ? 1.0f : 0.0f, 1e-3f)) isIdentity = false;
    check(isIdentity, "m * inverse(m) == identity");
}

int main() {
    registerHierarchyComponents();

    testBasicParenting();
    testDetach();
    testDeepChain();
    testScalePropagation();
    testCycleProtection();
    testKeepWorldPosition();
    testOrphanCleanup();
    testMultipleChildren();
    testStructuralStability();
    testWorldIntegration();
    testComputeWorldMatrix();
    testMathHelpers();

    std::printf("\n----------------------------------------\n");
    std::printf("Hierarchy self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
