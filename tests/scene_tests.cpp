/**
 * @file    scene_tests.cpp
 * @brief   Тесты сериализации сцен (.lvscene).
 *
 * Проверяется: round-trip значений всех типов полей, стабильность и
 * идемпотентность формата, ремаппинг ссылок на сущности, восстановление
 * иерархии, устойчивость к неизвестным компонентам/полям и битым файлам,
 * атомарная запись на диск и догрузка сцены поверх существующего мира.
 */
#include "ecs/Hierarchy.h"
#include "ecs/World.h"
#include "audio/Audio.h"
#include "render/Renderer.h"
#include "scene/Scene.h"
#include "scripting/EngineBindings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lv;

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

/// Компонент, существующий только в тестах: проверяет, что сериализация
/// работает для ЛЮБОГО зарегистрированного компонента, а не только встроенных.
struct TestStats {
    Real         health = 100;
    std::int64_t level = 1;
    bool         alive = true;
    Vec3         spawn{};
    Quat         facing{};
    Color        tint{1, 1, 1, 1};
    ecs::Entity  target{};
    std::string  tag;
    static constexpr std::string_view lv_component_name = "TestStats";
};

void registerTestStats() {
    static bool done = false;
    if (done) return;
    done = true;
    using FK = scripting::FieldBinding::Kind;
    auto& reg = scripting::BindingRegistry::instance();
    reg.registerComponent<TestStats>("TestStats");
    reg.field<TestStats>("TestStats", "health", &TestStats::health, FK::Float);
    reg.field<TestStats>("TestStats", "level",  &TestStats::level,  FK::Int);
    reg.field<TestStats>("TestStats", "alive",  &TestStats::alive,  FK::Bool);
    reg.field<TestStats>("TestStats", "spawn",  &TestStats::spawn,  FK::Vec3);
    reg.field<TestStats>("TestStats", "facing", &TestStats::facing, FK::Quat);
    reg.field<TestStats>("TestStats", "tint",   &TestStats::tint,   FK::Color);
    reg.field<TestStats>("TestStats", "target", &TestStats::target, FK::Entity);
    reg.field<TestStats>("TestStats", "tag",    &TestStats::tag,    FK::String);
}

void setupRegistries() {
    scripting::registerEngineComponents();
    ecs::registerHierarchyComponents();
    scene::registerSceneComponents();
    registerTestStats();
}

std::string tempPath(const char* suffix) {
    return std::string("/tmp/limvine_scene_test_") + suffix;
}

} // namespace

// ---------------------------------------------------------------------------
static void testRoundTripAllFieldKinds() {
    section("round-trip всех типов полей");
    setupRegistries();

    ecs::World src;
    ecs::Entity a = src.registry().create();
    ecs::Entity b = src.registry().create();
    scene::setEntityName(src.registry(), a, "Hero");
    scene::setEntityName(src.registry(), b, "Goblin");

    TestStats s;
    s.health = 73.5f;
    s.level  = 42;
    s.alive  = false;
    s.spawn  = Vec3{1.5f, -2.25f, 3.75f};
    s.facing = Quat{0, 0.7071f, 0, 0.7071f};
    s.tint   = Color{0.25f, 0.5f, 0.75f, 0.5f};
    s.target = b;                       // ссылка на другую сущность
    s.tag    = "boss/tier-2";
    src.registry().add<TestStats>(a, s);

    ecs::Transform t;
    t.position = Vec3{10, 20, 30};
    t.scale    = Vec3{2, 2, 2};
    src.registry().add<ecs::Transform>(a, t);

    const std::string text = scene::saveSceneToString(src);
    check(text.find("\"format\"") != std::string::npos, "в файле есть поле format");
    check(text.find("lvscene") != std::string::npos, "формат помечен как lvscene");
    check(text.find("\"Hero\"") != std::string::npos, "имя сущности сохранено");
    check(text.find("boss/tier-2") != std::string::npos, "строковое поле сохранено");

    // Целевой мир «состарен»: слоты Entity переиспользованы, поэтому новые
    // handle гарантированно не совпадут со старыми. Без этого проверка
    // ремаппинга ничего не доказывает — в свежем мире индексы совпадают
    // случайно.
    ecs::World dst;
    {
        // Важно создать пачку и только потом удалить: create/destroy по одному
        // переиспользует один и тот же слот, и поколения остальных не растут.
        std::vector<ecs::Entity> churn;
        for (int i = 0; i < 8; ++i) churn.push_back(dst.registry().create());
        for (ecs::Entity tmp : churn) dst.registry().destroy(tmp);
    }
    scene::SceneLoadResult r = scene::loadSceneFromString(dst, text);
    check(r.ok, "загрузка успешна");
    check(r.error.empty(), "нет фатальной ошибки");
    check(r.warnings.empty(), "нет предупреждений при чистом round-trip");
    check(r.entities.size() == 2, "загружено 2 сущности");
    if (!r.ok || r.entities.size() != 2) return;

    const ecs::Entity na = r.entities[0];
    const ecs::Entity nb = r.entities[1];
    const TestStats* ls = dst.registry().get<TestStats>(na);
    check(ls != nullptr, "компонент TestStats восстановлен");
    if (!ls) return;

    check(near(ls->health, 73.5f), "Float-поле сохраняет значение");
    check(ls->level == 42, "Int-поле сохраняет значение");
    check(ls->alive == false, "Bool-поле сохраняет значение");
    check(nearV(ls->spawn, Vec3{1.5f, -2.25f, 3.75f}), "Vec3-поле сохраняет значение");
    check(near(ls->facing.y, 0.7071f) && near(ls->facing.w, 0.7071f), "Quat-поле сохраняет значение");
    check(near(ls->tint.r, 0.25f) && near(ls->tint.a, 0.5f), "Color-поле сохраняет значение");
    check(ls->tag == "boss/tier-2", "String-поле сохраняет значение");
    check(ls->target == nb, "Entity-ссылка переотображена на новый handle");
    check(nb != b, "новый handle действительно отличается от исходного");
    check(ls->target != b, "Entity-ссылка не сохранила старый handle буквально");

    check(scene::entityName(dst.registry(), na) == "Hero", "имя Hero восстановлено");
    check(scene::entityName(dst.registry(), nb) == "Goblin", "имя Goblin восстановлено");

    const ecs::Transform* lt = dst.registry().get<ecs::Transform>(na);
    check(lt && nearV(lt->position, Vec3{10, 20, 30}), "Transform.position восстановлен");
    check(lt && nearV(lt->scale, Vec3{2, 2, 2}), "Transform.scale восстановлен");
}

// ---------------------------------------------------------------------------
static void testIdempotence() {
    section("идемпотентность формата");
    setupRegistries();

    ecs::World w;
    for (int i = 0; i < 12; ++i) {
        ecs::Entity e = w.registry().create();
        ecs::Transform t;
        t.position = Vec3{static_cast<Real>(i), static_cast<Real>(i * 2), 0};
        w.registry().add<ecs::Transform>(e, t);
        scene::setEntityName(w.registry(), e, "Entity" + std::to_string(i));
        TestStats s;
        s.level = i;
        s.tag = "tag" + std::to_string(i);
        w.registry().add<TestStats>(e, s);
    }

    const std::string first = scene::saveSceneToString(w);

    ecs::World w2;
    scene::SceneLoadResult r = scene::loadSceneFromString(w2, first);
    check(r.ok, "перезагрузка прошла");
    const std::string second = scene::saveSceneToString(w2);
    check(first == second, "save -> load -> save даёт побайтово идентичный файл");

    // Третий цикл — на случай, если стабилизация происходит только со второго раза.
    ecs::World w3;
    check(scene::loadSceneFromString(w3, second).ok, "третий цикл загрузился");
    check(scene::saveSceneToString(w3) == second, "формат стабилен и на третьем цикле");

    // Сохранение НЕ зависит от истории переиспользования слотов Entity.
    ecs::World churn;
    for (int i = 0; i < 40; ++i) {
        ecs::Entity tmp = churn.registry().create();
        churn.registry().destroy(tmp);
    }
    for (int i = 0; i < 12; ++i) {
        ecs::Entity e = churn.registry().create();
        ecs::Transform t;
        t.position = Vec3{static_cast<Real>(i), static_cast<Real>(i * 2), 0};
        churn.registry().add<ecs::Transform>(e, t);
        scene::setEntityName(churn.registry(), e, "Entity" + std::to_string(i));
        TestStats s;
        s.level = i;
        s.tag = "tag" + std::to_string(i);
        churn.registry().add<TestStats>(e, s);
    }
    check(scene::saveSceneToString(churn) == first,
          "файл не зависит от поколений/переиспользованных слотов Entity");
}

// ---------------------------------------------------------------------------
static void testHierarchyRoundTrip() {
    section("иерархия");
    setupRegistries();

    ecs::World src;
    ecs::Registry& sr = src.registry();
    ecs::Entity root  = sr.create();
    ecs::Entity mid   = sr.create();
    ecs::Entity leaf  = sr.create();
    ecs::Entity other = sr.create();
    for (ecs::Entity e : {root, mid, leaf, other}) sr.add<ecs::Transform>(e);

    sr.get<ecs::Transform>(root)->position = Vec3{100, 0, 0};
    sr.get<ecs::Transform>(mid)->position  = Vec3{10, 0, 0};
    sr.get<ecs::Transform>(leaf)->position = Vec3{1, 0, 0};
    ecs::setParent(sr, mid, root);
    ecs::setParent(sr, leaf, mid);
    ecs::updateWorldTransforms(sr);

    const Vec3 srcLeafWorld = sr.get<ecs::WorldTransform>(leaf)->worldPosition;
    check(nearV(srcLeafWorld, Vec3{111, 0, 0}), "исходная world-позиция листа верна");

    const std::string text = scene::saveSceneToString(src);
    check(text.find("\"parent\"") != std::string::npos, "родитель записан в файл");
    check(text.find("WorldTransform") == std::string::npos,
          "производный WorldTransform по умолчанию не сохраняется");
    check(text.find("\"Children\"") == std::string::npos,
          "производный Children по умолчанию не сохраняется");

    ecs::World dst;
    scene::SceneLoadResult r = scene::loadSceneFromString(dst, text);
    check(r.ok, "иерархическая сцена загрузилась");
    if (!r.ok || r.entities.size() != 4) { check(false, "ожидалось 4 сущности"); return; }

    ecs::Registry& dr = dst.registry();
    const ecs::Entity nroot = r.entities[0], nmid = r.entities[1], nleaf = r.entities[2];

    const ecs::Parent* pm = dr.get<ecs::Parent>(nmid);
    check(pm && pm->entity == nroot, "mid снова прикреплён к root");
    const ecs::Parent* pl = dr.get<ecs::Parent>(nleaf);
    check(pl && pl->entity == nmid, "leaf снова прикреплён к mid");

    const ecs::Children* cr = dr.get<ecs::Children>(nroot);
    check(cr && cr->entities.size() == 1 && cr->entities[0] == nmid,
          "Children восстановлен из Parent, а не прочитан из файла");

    const ecs::WorldTransform* wt = dr.get<ecs::WorldTransform>(nleaf);
    check(wt != nullptr, "WorldTransform пересчитан при загрузке");
    check(wt && nearV(wt->worldPosition, Vec3{111, 0, 0}),
          "world-позиция листа совпадает с исходной");

    const ecs::Entity nother = r.entities[3];
    check(dr.get<ecs::Parent>(nother) == nullptr, "корневая сущность осталась без родителя");
}

// ---------------------------------------------------------------------------
static void testUnknownDataTolerance() {
    section("толерантность к неизвестным данным");
    setupRegistries();

    const char* text = R"({
      "format": "lvscene",
      "version": 1,
      "entities": [
        { "id": 0, "name": "Survivor",
          "components": {
            "Transform": { "position": {"x": 5, "y": 6, "z": 7}, "somethingNew": 123 },
            "FutureComponent": { "whatever": true },
            "TestStats": { "health": 55, "unknownField": "ignored" }
          } }
      ]
    })";

    ecs::World w;
    scene::SceneLoadResult r = scene::loadSceneFromString(w, text);
    check(r.ok, "сцена с неизвестными данными всё равно загружается");
    check(r.entities.size() == 1, "сущность создана");
    if (r.entities.empty()) return;

    const ecs::Transform* t = w.registry().get<ecs::Transform>(r.entities[0]);
    check(t && nearV(t->position, Vec3{5, 6, 7}), "известные поля применены");
    const TestStats* s = w.registry().get<TestStats>(r.entities[0]);
    check(s && near(s->health, 55.f), "известные поля второго компонента применены");
    check(scene::entityName(w.registry(), r.entities[0]) == "Survivor", "имя применено");

    bool warnedComponent = false, warnedField = false;
    for (const std::string& warn : r.warnings) {
        if (warn.find("FutureComponent") != std::string::npos) warnedComponent = true;
        if (warn.find("unknownField") != std::string::npos)    warnedField = true;
    }
    check(warnedComponent, "о неизвестном компоненте выдано предупреждение");
    check(warnedField, "о неизвестном поле выдано предупреждение");
    check(r.warnings.size() >= 3, "предупреждения собраны, а не потеряны");
}

// ---------------------------------------------------------------------------
static void testMalformedInput() {
    section("битые и враждебные входные данные");
    setupRegistries();

    struct Case { const char* text; const char* what; };
    const Case cases[] = {
        {"",                                        "пустая строка отвергнута"},
        {"not json at all",                         "мусор отвергнут"},
        {"[1,2,3]",                                 "массив вместо объекта отвергнут"},
        {R"({"version":1,"entities":[]})",          "файл без \"format\" отвергнут"},
        {R"({"format":"other","version":1,"entities":[]})", "чужой формат отвергнут"},
        {R"({"format":"lvscene","version":999,"entities":[]})", "будущая версия отвергнута"},
        {R"({"format":"lvscene","version":0,"entities":[]})",   "нулевая версия отвергнута"},
        {R"({"format":"lvscene","version":1})",     "файл без \"entities\" отвергнут"},
        {R"({"format":"lvscene","version":1,"entities":{}})",   "entities-объект отвергнут"},
    };
    for (const Case& c : cases) {
        ecs::World w;
        scene::SceneLoadResult r = scene::loadSceneFromString(w, c.text);
        check(!r.ok && !r.error.empty(), c.what);
        check(w.registry().entityCount() == 0, "мир не изменён при отказе");
    }

    // Пустая, но валидная сцена — это успех, а не ошибка.
    {
        ecs::World w;
        scene::SceneLoadResult r = scene::loadSceneFromString(
            w, R"({"format":"lvscene","version":1,"entities":[]})");
        check(r.ok, "пустая валидная сцена загружается");
        check(w.registry().entityCount() == 0, "пустая сцена не создаёт сущностей");
    }

    // Висячие ссылки не должны ронять загрузку.
    {
        const char* text = R"({
          "format": "lvscene", "version": 1,
          "entities": [
            { "id": 0, "parent": 77, "components": { "TestStats": { "target": 99 } } }
          ]
        })";
        ecs::World w;
        scene::SceneLoadResult r = scene::loadSceneFromString(w, text);
        check(r.ok, "сцена с висячими ссылками загружается");
        const TestStats* s = w.registry().get<TestStats>(r.entities[0]);
        check(s && !s->target.valid(), "висячая Entity-ссылка обнулена");
        check(w.registry().get<ecs::Parent>(r.entities[0]) == nullptr,
              "висячая ссылка на родителя оставила сущность в корне");
        bool warned = false;
        for (const std::string& warn : r.warnings)
            if (warn.find("dangling") != std::string::npos) warned = true;
        check(warned, "о висячих ссылках выдано предупреждение");
    }
}

// ---------------------------------------------------------------------------
static void testFileIO() {
    section("файловый ввод-вывод");
    setupRegistries();

    const std::string path = tempPath("io.lvscene");
    std::remove(path.c_str());

    ecs::World src;
    ecs::Entity e = src.registry().create();
    scene::setEntityName(src.registry(), e, "Persisted");
    ecs::Transform t;
    t.position = Vec3{-1, -2, -3};
    src.registry().add<ecs::Transform>(e, t);

    std::string err;
    check(scene::saveSceneToFile(src, path, {}, &err), "сохранение в файл успешно");
    check(err.empty(), "при сохранении нет текста ошибки");

    {
        std::ifstream f(path);
        check(f.good(), "файл существует на диске");
    }
    {
        std::ifstream tmpf(path + ".tmp");
        check(!tmpf.good(), "временный файл удалён после переименования");
    }

    ecs::World dst;
    scene::SceneLoadResult r = scene::loadSceneFromFile(dst, path);
    check(r.ok, "загрузка из файла успешна");
    check(!r.entities.empty() && scene::entityName(dst.registry(), r.entities[0]) == "Persisted",
          "данные из файла корректны");

    // Перезапись существующего файла (типичный «Save» в редакторе).
    check(scene::saveSceneToFile(src, path, {}, &err), "повторное сохранение перезаписывает файл");

    ecs::World missing;
    scene::SceneLoadResult mr = scene::loadSceneFromFile(missing, tempPath("does_not_exist.lvscene"));
    check(!mr.ok && !mr.error.empty(), "отсутствующий файл даёт ошибку, а не падение");

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
static void testAdditiveLoadAndOptions() {
    section("догрузка поверх мира и опции сохранения");
    setupRegistries();

    ecs::World w;
    ecs::Entity existing = w.registry().create();
    scene::setEntityName(w.registry(), existing, "AlreadyHere");

    const char* text = R"({
      "format": "lvscene", "version": 1,
      "entities": [ { "id": 0, "name": "Streamed" } ]
    })";
    scene::SceneLoadResult r = scene::loadSceneFromString(w, text);
    check(r.ok, "догрузка успешна");
    check(w.registry().entityCount() == 2, "существующие сущности сохранены (additive load)");
    check(w.registry().alive(existing), "старая сущность жива");
    check(scene::entityName(w.registry(), existing) == "AlreadyHere", "старое имя не затёрто");
    check(scene::entityName(w.registry(), r.entities[0]) == "Streamed", "новая сущность добавлена");

    ecs::World h;
    ecs::Entity p = h.registry().create(), c = h.registry().create();
    h.registry().add<ecs::Transform>(p);
    h.registry().add<ecs::Transform>(c);
    ecs::setParent(h.registry(), c, p);
    ecs::updateWorldTransforms(h.registry());

    // Компактный режим — для рантайм-сейвов, где размер важнее читаемости.
    scene::SaveOptions compact;
    compact.indent = -1;
    const std::string small = scene::saveSceneToString(h, compact);
    const std::string pretty = scene::saveSceneToString(h);
    check(small.find('\n') == std::string::npos, "indent=-1 даёт одну строку");
    check(small.size() < pretty.size(), "компактный режим меньше pretty-варианта");

    // Компактный и pretty варианты должны описывать одну и ту же сцену.
    ecs::World a, b;
    check(scene::loadSceneFromString(a, small).ok, "компактный вариант загружается");
    check(scene::loadSceneFromString(b, pretty).ok, "pretty-вариант загружается");
    check(scene::saveSceneToString(a) == scene::saveSceneToString(b),
          "оба варианта дают идентичное состояние мира");
}

// ---------------------------------------------------------------------------
static void testLargeScene() {
    section("крупная сцена");
    setupRegistries();

    constexpr int kCount = 2000;
    ecs::World src;
    ecs::Registry& sr = src.registry();
    std::vector<ecs::Entity> made;
    made.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        ecs::Entity e = sr.create();
        made.push_back(e);
        ecs::Transform t;
        t.position = Vec3{static_cast<Real>(i % 50), 0, static_cast<Real>(i / 50)};
        sr.add<ecs::Transform>(e, t);
        TestStats s;
        s.level  = i;
        s.target = made[static_cast<std::size_t>(i / 2)];   // плотная сеть ссылок
        sr.add<TestStats>(e, s);
        if (i > 0 && i % 10 != 0) ecs::setParent(sr, e, made[static_cast<std::size_t>(i / 10)]);
    }

    const std::string text = scene::saveSceneToString(src);
    ecs::World dst;
    scene::SceneLoadResult r = scene::loadSceneFromString(dst, text);
    check(r.ok, "сцена из 2000 сущностей загружается");
    check(r.warnings.empty(), "нет предупреждений на крупной сцене");
    check(dst.registry().entityCount() == kCount, "число сущностей совпадает");

    bool allRefsOk = true, allLevelsOk = true;
    for (int i = 0; i < kCount; ++i) {
        const TestStats* s = dst.registry().get<TestStats>(r.entities[static_cast<std::size_t>(i)]);
        if (!s) { allRefsOk = allLevelsOk = false; break; }
        if (s->level != i) allLevelsOk = false;
        if (s->target != r.entities[static_cast<std::size_t>(i / 2)]) allRefsOk = false;
    }
    check(allLevelsOk, "все Int-поля переданы верно");
    check(allRefsOk, "все 2000 Entity-ссылок переотображены верно");
    check(scene::saveSceneToString(dst) == text, "крупная сцена тоже идемпотентна");
}

// ---------------------------------------------------------------------------
static void testWorldStillTicks() {
    section("загруженная сцена живёт в обычном кадре");
    setupRegistries();

    ecs::World w;
    ecs::Entity p = w.registry().create(), c = w.registry().create();
    w.registry().add<ecs::Transform>(p)->position = Vec3{5, 0, 0};
    w.registry().add<ecs::Transform>(c)->position = Vec3{2, 0, 0};
    ecs::setParent(w.registry(), c, p);

    const std::string text = scene::saveSceneToString(w);
    ecs::World dst;
    scene::SceneLoadResult r = scene::loadSceneFromString(dst, text);
    check(r.ok, "сцена загружена");

    // Сдвигаем родителя и прогоняем кадр: система иерархии должна подхватить
    // загруженные из файла связи без дополнительной инициализации.
    dst.registry().get<ecs::Transform>(r.entities[0])->position = Vec3{50, 0, 0};
    dst.tick(1.0f / 60.0f);

    const ecs::WorldTransform* wt = dst.registry().get<ecs::WorldTransform>(r.entities[1]);
    check(wt && nearV(wt->worldPosition, Vec3{52, 0, 0}),
          "иерархия из файла обновляется штатной системой кадра");
}

// ---------------------------------------------------------------------------
static void testScriptBindings() {
    section("биндинги сцен в LV Script");
    setupRegistries();

    const std::string path = tempPath("script.lvscene");
    std::remove(path.c_str());

    ecs::World world;
    scripting::EngineContext ctx;
    ctx.world = &world;

    // --- доверенный скрипт: полный цикл spawn -> save -> clear -> load -------
    {
        lv::SandboxConfig cfg;
        cfg.capabilities = lv::Cap_GameDefault | lv::Cap_IO;
        scripting::ScriptWorld sw(cfg);
        sw.attach(ctx);

        std::string log;
        sw.setLogCallback([&](const std::string& s) { log += s; log += "\n"; });

        const std::string src =
            "let e = spawn()\n"
            "setName(e, \"ScriptMade\")\n"
            "setField(e, \"Transform\", \"position\", vec3(3, 4, 5))\n"
            "let saved = saveScene(\"" + path + "\")\n"
            "clearScene()\n"
            "let r = loadScene(\"" + path + "\")\n"
            "result = { \"saved\": saved, \"ok\": r.ok, \"count\": r.entities, \"warns\": r.warnings }\n";

        const bool ran = sw.runFile("scene_bindings.lvs", src);
        check(ran, "скрипт со сценарными биндингами выполнился");
        if (!ran) { std::printf("  log: %s\n", log.c_str()); return; }

        check(world.registry().entityCount() == 1, "после clearScene+loadScene в мире 1 сущность");

        bool found = false;
        for (ecs::Entity e : world.registry().allEntities()) {
            if (scene::entityName(world.registry(), e) != "ScriptMade") continue;
            found = true;
            const ecs::Transform* t = world.registry().get<ecs::Transform>(e);
            check(t && nearV(t->position, Vec3{3, 4, 5}),
                  "позиция, записанная из скрипта, пережила save/load");
        }
        check(found, "сущность, созданная скриптом, восстановлена по имени");
    }

    // --- мод в песочнице: файловых прав нет --------------------------------
    {
        ecs::World modWorld;
        scripting::EngineContext modCtx;
        modCtx.world = &modWorld;

        lv::SandboxConfig modCfg;
        modCfg.capabilities = lv::Cap_ModSandbox;
        scripting::ScriptWorld mod(modCfg);
        mod.attach(modCtx);

        std::string log;
        mod.setLogCallback([&](const std::string& s) { log += s; log += "\n"; });

        const std::string evil = "saveScene(\"" + tempPath("evil.lvscene") + "\")\n";
        const bool ran = mod.runFile("evil.lvs", evil);
        check(!ran, "мод без Cap_IO не может сохранить сцену");
        check(log.find("saveScene") != std::string::npos ||
              log.find("capabilit") != std::string::npos ||
              log.find("disabled") != std::string::npos,
              "отказ песочницы объяснён в логе");

        std::ifstream leaked(tempPath("evil.lvscene"));
        check(!leaked.good(), "мод не создал файл на диске");
    }

    std::remove(path.c_str());
    std::remove(tempPath("evil.lvscene").c_str());
}

// ---------------------------------------------------------------------------
int main() {
    std::printf("=== LimVine Scene (.lvscene) self-test ===\n");
    testRoundTripAllFieldKinds();
    testIdempotence();
    testHierarchyRoundTrip();
    testUnknownDataTolerance();
    testMalformedInput();
    testFileIO();
    testAdditiveLoadAndOptions();
    testLargeScene();
    testWorldStillTicks();
    testScriptBindings();

    std::printf("\n----------------------------------------\n");
    std::printf("Scene self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
