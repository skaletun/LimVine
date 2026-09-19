/**
 * @file    gameplay_tests.cpp
 * @brief   Тесты геймплейных модулей stdlib: character.lvs и interaction.lvs.
 *
 * Эти модули, в отличие от остального stdlib, требуют живых биндингов движка
 * (spawn/getPosition/raycast), поэтому проверяются на «мини-движке» — так же,
 * как engine_integration_tests.
 *
 * Тест обязан ловить регрессию, а не компиляцию, поэтому здесь проверяется
 * именно поведение: диагональ не быстрее прямой, персонаж падает и
 * приземляется, объект за спиной не перехватывает подсказку, стена перекрывает
 * видимость, удержание не «перетекает» между целями.
 */
#include "scripting/EngineBindings.h"
#include "ecs/World.h"
#include "physics/Physics.h"
#include "input/Input.h"
#include "audio/Audio.h"
#include "render/Renderer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace lv;
using namespace lv::ecs;
using namespace lv::scripting;

namespace {

int g_passed = 0, g_failed = 0;
std::vector<std::string> g_log;

void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
void section(const char* s) { std::printf("\n== %s ==\n", s); }

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

std::string readFile(const std::string& p) {
    std::ifstream f(p);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Мини-движок: ECS + физика + ввод + звук + рендер на Null-бэкендах.
struct Harness {
    World world;
    physics::PhysicsWorld physics;
    input::InputSystem input;
    audio::AudioSystem audio;
    render::Renderer renderer;
    EngineContext ctx;
    ScriptWorld scripts;

    Harness() {
        registerComponent<Transform>();
        registerComponent<render::MeshRenderer>();
        physics.init(1024);
        physics::attachToWorld(physics, world);
        audio.init(16, /*forceNull=*/true);
        audio::attachToWorld(audio, world);
        render::SurfaceDesc surface;
        renderer.init(render::Backend::Null, surface);
        renderer.attachToWorld(world);

        ctx.world = &world;
        ctx.physics = &physics;
        ctx.input = &input;
        ctx.audio = &audio;
        ctx.renderer = &renderer;

        scripts.setLogCallback([](const std::string& s) { g_log.push_back(s); });
        scripts.attach(ctx);
    }

    /// Загрузить модуль stdlib и выполнить сценарий поверх него.
    bool run(const std::vector<std::string>& modules, const std::string& scenario) {
        std::string src;
        for (const std::string& m : modules) {
            const std::string text = readFile("stdlib/" + m + ".lvs");
            if (text.empty()) return false;
            src += text;
            src += "\n";
        }
        src += scenario;
        return scripts.runFile("gameplay-test.lvs", src);
    }

    /// Прочитать число из глобальной переменной скрипта.
    double num(const char* name) {
        const Value v = scripts.vm().getGlobal(name);
        return v.asNumber();
    }
    bool boolean(const char* name) {
        return scripts.vm().getGlobal(name).truthy();
    }
    std::string str(const char* name) {
        return scripts.vm().getGlobal(name).toString();
    }
};

} // namespace

// ---------------------------------------------------------------------------
static void testCharacterMovement() {
    section("CharacterController: движение");
    Harness h;

    const bool ok = h.run({"character"}, R"LV(
        global e = spawn()
        setPosition(e, vec3(0, 0, 0))
        global cc = new CharacterController(e)
        cc.speed = 10.0
        cc.mode = "world"

        # Прямо вперёд одну секунду (60 кадров по 1/60).
        for i in 0..60 do
            cc.move(0.0, 1.0)
            cc.update(1.0 / 60.0)
        end
        global straightZ = cc.position().z

        # Та же дистанция по диагонали: путь должен быть той же ДЛИНЫ,
        # иначе диагональ быстрее прямой (классический баг).
        setPosition(e, vec3(0, 0, 0))
        for i in 0..60 do
            cc.move(1.0, 1.0)
            cc.update(1.0 / 60.0)
        end
        let p = cc.position()
        global diagLen = math.sqrt(p.x * p.x + p.z * p.z)
        global movingFlag = cc.moving

        # Без ввода персонаж стоит.
        let before = cc.position()
        cc.update(1.0 / 60.0)
        global idleSame = cc.position().x == before.x and cc.position().z == before.z
        global idleMoving = cc.moving
    )LV");
    check(ok, "сценарий движения выполнился");
    if (!ok) return;

    const double straight = h.num("straightZ");
    const double diag = h.num("diagLen");
    check(near(straight, 10.0, 0.2), "за секунду при speed=10 пройдено ~10 единиц");
    check(near(diag, straight, 0.05), "диагональ не быстрее прямой (вектор нормализован)");
    check(h.boolean("movingFlag"), "флаг moving выставлен при движении");
    check(h.boolean("idleSame"), "без ввода персонаж стоит на месте");
    check(!h.boolean("idleMoving"), "флаг moving снят в кадре без ввода");
}

// ---------------------------------------------------------------------------
static void testCameraRelativeMovement() {
    section("CharacterController: движение относительно камеры");
    Harness h;

    const bool ok = h.run({"character"}, R"LV(
        global e = spawn()
        global cc = new CharacterController(e)
        cc.speed = 10.0
        cc.mode = "camera"

        # Смотрим вдоль +Z (yaw=0): "вперёд" даёт +Z.
        setPosition(e, vec3(0, 0, 0))
        cc.yaw = 0.0
        for i in 0..60 do
            cc.move(0.0, 1.0)
            cc.update(1.0 / 60.0)
        end
        global fwdZ = cc.position().z
        global fwdX = cc.position().x

        # Повернулись на 90 градусов: то же "вперёд" теперь даёт +X.
        setPosition(e, vec3(0, 0, 0))
        cc.yaw = 90.0
        for i in 0..60 do
            cc.move(0.0, 1.0)
            cc.update(1.0 / 60.0)
        end
        global rightX = cc.position().x
        global rightZ = cc.position().z

        # lookAt разворачивает на цель.
        setPosition(e, vec3(0, 0, 0))
        global aimYaw = cc.lookAt(vec3(0, 0, -5))

        # Нормализация yaw держит угол в (-180, 180].
        cc.yaw = 190.0
        global wrapped = cc.normalizeYaw()

        # Ограничение pitch.
        cc.pitch = 0.0
        cc.lookDelta({"x": 0.0, "y": -10000.0})
        global pitchMax = cc.pitch
    )LV");
    check(ok, "сценарий камеры выполнился");
    if (!ok) return;

    check(near(h.num("fwdZ"), 10.0, 0.2), "при yaw=0 движение вперёд идёт по +Z");
    check(near(h.num("fwdX"), 0.0, 0.05), "при yaw=0 боковой снос отсутствует");
    check(near(h.num("rightX"), 10.0, 0.2), "при yaw=90 то же 'вперёд' идёт по +X");
    check(near(h.num("rightZ"), 0.0, 0.05), "при yaw=90 движения по Z нет");
    check(near(std::fabs(h.num("aimYaw")), 180.0, 0.5), "lookAt развернул персонажа к цели");
    check(near(h.num("wrapped"), -170.0, 0.01), "yaw нормализован в диапазон (-180, 180]");
    check(near(h.num("pitchMax"), 85.0, 0.01), "pitch ограничен сверху (нет переворота камеры)");
}

// ---------------------------------------------------------------------------
static void testGravityAndJump() {
    section("CharacterController: гравитация и прыжок");
    Harness h;

    const bool ok = h.run({"character"}, R"LV(
        global e = spawn()
        setPosition(e, vec3(0, 0, 0))
        global cc = new CharacterController(e)
        cc.gravity = 18.0
        cc.jumpImpulse = 7.0
        cc.groundY = 0.0

        global startGrounded = cc.grounded

        cc.jump()
        cc.update(1.0 / 60.0)
        global afterJumpY = cc.position().y
        global airborne = cc.grounded == false

        # Прыжок в воздухе игнорируется (нет двойного прыжка по умолчанию).
        let yBefore = cc.position().y
        let vyBefore = cc.velY
        cc.jump()
        cc.update(1.0 / 60.0)
        global noDoubleJump = cc.velY < vyBefore

        # Досимулировать до приземления.
        var frames = 0
        while cc.grounded == false and frames < 600 do
            cc.update(1.0 / 60.0)
            frames += 1
        end
        global landedY = cc.position().y
        global landed = cc.grounded
        global airFrames = frames
        global landedVel = cc.velY

        # Гравитация выключена -> персонаж висит.
        global e2 = spawn()
        setPosition(e2, vec3(0, 5, 0))
        global flat = new CharacterController(e2)
        flat.gravity = 0.0
        flat.jump()
        for i in 0..30 do flat.update(1.0 / 60.0) end
        global floatY = flat.position().y

        # teleport сбрасывает вертикальную скорость.
        cc.velY = -50.0
        cc.grounded = false
        cc.teleport(vec3(0, 0, 0))
        global teleVel = cc.velY
    )LV");
    check(ok, "сценарий гравитации выполнился");
    if (!ok) return;

    check(h.boolean("startGrounded"), "персонаж стартует на земле");
    check(h.num("afterJumpY") > 0.0, "после прыжка персонаж поднялся над землёй");
    check(h.boolean("airborne"), "в прыжке флаг grounded снят");
    check(h.boolean("noDoubleJump"), "повторный прыжок в воздухе не даёт импульса");
    check(h.boolean("landed"), "персонаж приземлился");
    check(near(h.num("landedY"), 0.0, 1e-6), "приземление точно на уровень земли, без проваливания");
    check(near(h.num("landedVel"), 0.0, 1e-6), "вертикальная скорость обнулена при приземлении");
    const double frames = h.num("airFrames");
    check(frames > 30 && frames < 120, "время полёта физично (~0.8 с при v=7, g=18)");
    check(near(h.num("floatY"), 5.0, 1e-6), "при gravity=0 персонаж не падает");
    check(near(h.num("teleVel"), 0.0, 1e-6), "teleport сбрасывает вертикальную скорость");
}

// ---------------------------------------------------------------------------
static void testBoundsAndFacing() {
    section("CharacterController: границы мира и разворот");
    Harness h;

    const bool ok = h.run({"character"}, R"LV(
        global e = spawn()
        setPosition(e, vec3(0, 0, 0))
        global cc = new CharacterController(e)
        cc.speed = 100.0
        cc.mode = "world"
        cc.bounds(-5.0, 5.0)

        for i in 0..60 do
            cc.move(1.0, 0.0)
            cc.update(1.0 / 60.0)
        end
        global clampedX = cc.position().x

        for i in 0..60 do
            cc.move(0.0, -1.0)
            cc.update(1.0 / 60.0)
        end
        global clampedZ = cc.position().z

        # Прямоугольные границы.
        global e2 = spawn()
        setPosition(e2, vec3(0, 0, 0))
        global cc2 = new CharacterController(e2)
        cc2.speed = 100.0
        cc2.mode = "world"
        cc2.bounds(-2.0, 2.0, -50.0, 50.0)
        for i in 0..60 do
            cc2.move(1.0, 1.0)
            cc2.update(1.0 / 60.0)
        end
        global rectX = cc2.position().x
        global rectZ = cc2.position().z

        # faceMovement разворачивает модель по направлению хода.
        global e3 = spawn()
        setPosition(e3, vec3(0, 0, 0))
        global cc3 = new CharacterController(e3)
        cc3.mode = "world"
        cc3.faceMovement = true
        cc3.move(1.0, 0.0)
        cc3.update(1.0 / 60.0)
        global faceYaw = cc3.modelYaw()
    )LV");
    check(ok, "сценарий границ выполнился");
    if (!ok) return;

    check(near(h.num("clampedX"), 5.0, 1e-4), "движение ограничено правой границей");
    check(near(h.num("clampedZ"), -5.0, 1e-4), "движение ограничено дальней границей");
    check(near(h.num("rectX"), 2.0, 1e-4), "прямоугольные границы работают по X");
    check(h.num("rectZ") > 10.0, "по свободной оси Z ограничения нет");
    check(near(h.num("faceYaw"), 90.0, 0.5), "faceMovement развернул модель по ходу движения");
}

// ---------------------------------------------------------------------------
static void testInteractionBasics() {
    section("InteractionSystem: выбор цели");
    Harness h;

    const bool ok = h.run({"interaction"}, R"LV(
        global chest = spawn()
        setPosition(chest, vec3(0, 0, 2))
        global door = spawn()
        setPosition(door, vec3(0, 0, -2))      # за спиной
        global far = spawn()
        setPosition(far, vec3(0, 0, 50))       # вне досягаемости

        global opened = 0
        global sys = new InteractionSystem()
        sys.defaultRange = 3.0
        sys.register(chest, { "label": "Открыть сундук", "tag": "chest",
                              "action": || opened += 1 })
        sys.register(door,  { "label": "Открыть дверь", "tag": "door" })
        sys.register(far,   { "label": "Далеко", "tag": "far" })

        # Смотрим вперёд (+Z).
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global prompt1 = sys.prompt
        global tag1 = sys.state()["tag"]

        sys.activate()
        global openedAfter = opened

        # Развернулись — теперь ближайшая доступная цель это дверь.
        sys.update(vec3(0, 0, 0), vec3(0, 0, -1), 0.016)
        global tag2 = sys.state()["tag"]

        # Отошли ото всех — подсказки нет.
        sys.update(vec3(100, 0, 100), vec3(0, 0, 1), 0.016)
        global prompt3 = sys.prompt
        global hasTarget3 = sys.state()["hasTarget"]

        # activate() без цели безопасен.
        global nilResult = sys.activate() == nil

        global registered = sys.count()
    )LV");
    check(ok, "сценарий взаимодействия выполнился");
    if (!ok) return;

    check(h.str("prompt1") == "Открыть сундук", "выбран объект перед игроком");
    check(h.str("tag1") == "chest", "объект за спиной не перехватил подсказку");
    check(near(h.num("openedAfter"), 1.0), "activate() выполнил действие ровно один раз");
    check(h.str("tag2") == "door", "после разворота выбран объект за спиной");
    check(h.str("prompt3").empty(), "вне досягаемости подсказка пуста");
    check(!h.boolean("hasTarget3"), "вне досягаемости цели нет");
    check(h.boolean("nilResult"), "activate() без цели возвращает nil и не падает");
    check(near(h.num("registered"), 3.0), "зарегистрированы все три объекта");
}

// ---------------------------------------------------------------------------
static void testInteractionFilters() {
    section("InteractionSystem: фильтры, приоритет, once");
    Harness h;

    const bool ok = h.run({"interaction"}, R"LV(
        global a = spawn()
        setPosition(a, vec3(0, 0, 2))
        global b = spawn()
        setPosition(b, vec3(0, 0, 2.5))

        global locked = true
        global sys = new InteractionSystem()
        sys.defaultRange = 5.0

        # a ближе, но заблокирован условием -> должен победить b.
        sys.register(a, { "label": "Заперто", "tag": "a", "enabled": || not locked })
        sys.register(b, { "label": "Открыто", "tag": "b" })

        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global lockedTag = sys.state()["tag"]
        global availCount = sys.availableCount()

        # Разблокировали -> a ближе, значит a.
        locked = false
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global unlockedTag = sys.state()["tag"]

        # Приоритет перебивает дистанцию.
        sys.clear()
        sys.register(a, { "label": "Ближний", "tag": "a" })
        sys.register(b, { "label": "Важный", "tag": "b", "priority": 10 })
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global priorityTag = sys.state()["tag"]

        # once: после активации объект исчезает из выбора.
        sys.clear()
        global picks = 0
        sys.register(a, { "label": "Поднять", "tag": "a", "once": true,
                          "action": || picks += 1 })
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        sys.activate()
        sys.activate()
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        sys.activate()
        global pickCount = picks
        global afterOnce = sys.state()["hasTarget"]

        # unregister убирает объект и снимает выделение.
        sys.clear()
        sys.register(a, { "label": "Временный", "tag": "a" })
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        let had = sys.state()["hasTarget"]
        sys.unregister(a)
        global afterUnregister = sys.state()["hasTarget"]
        global hadBefore = had

        # Мёртвая сущность не выбирается.
        sys.clear()
        global ghost = spawn()
        setPosition(ghost, vec3(0, 0, 1))
        sys.register(ghost, { "label": "Призрак", "tag": "ghost" })
        # destroy() откладывает удаление до конца кадра (CommandBuffer),
        # поэтому в ЭТОМ кадре призрак ещё жив и выбирается — это корректно.
        destroy(ghost)
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global ghostBeforeFlush = sys.state()["hasTarget"]
    )LV");
    check(ok, "сценарий фильтров выполнился");
    if (!ok) return;

    check(h.str("lockedTag") == "b", "заблокированный условием объект пропущен");
    check(near(h.num("availCount"), 1.0), "availableCount учитывает условие enabled");
    check(h.str("unlockedTag") == "a", "после разблокировки выбран ближайший");
    check(h.str("priorityTag") == "b", "priority перебивает дистанцию");
    check(near(h.num("pickCount"), 1.0), "объект once срабатывает ровно один раз");
    check(!h.boolean("afterOnce"), "после once объект больше не выбирается");
    check(h.boolean("hadBefore"), "до unregister цель была");
    check(!h.boolean("afterUnregister"), "unregister снял выделение");
    check(h.boolean("ghostBeforeFlush"),
          "destroy() отложен до конца кадра: в текущем кадре объект ещё доступен");

    // А вот ПОСЛЕ применения команд мёртвая сущность обязана отсеяться —
    // иначе проверка выше просто маскировала бы отсутствие фильтра alive().
    h.world.flushCommands();
    const bool afterFlush = h.scripts.runFile("ghost-after.lvs", R"LV(
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        ghostAfterFlush = sys.state()["hasTarget"]
    )LV");
    check(afterFlush, "повторная проверка выполнилась");
    check(!h.boolean("ghostAfterFlush"),
          "после flushCommands уничтоженная сущность больше не выбирается");
}

// ---------------------------------------------------------------------------
static void testInteractionHold() {
    section("InteractionSystem: активация удержанием");
    Harness h;

    const bool ok = h.run({"interaction"}, R"LV(
        global a = spawn()
        setPosition(a, vec3(0, 0, 1))
        global b = spawn()
        setPosition(b, vec3(0, 0, -1))

        global fired = 0
        global sys = new InteractionSystem()
        sys.defaultRange = 3.0
        sys.register(a, { "label": "Взломать", "tag": "a", "hold": 1.0,
                          "action": || fired += 1 })
        sys.register(b, { "label": "Другое", "tag": "b", "hold": 1.0 })

        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)

        # Полсекунды удержания — ещё рано.
        for i in 0..30 do sys.hold(1.0 / 60.0) end
        global midProgress = sys.holdProgress
        global firedMid = fired

        # Ещё полсекунды — срабатывает. Останавливаемся ровно в кадре
        # срабатывания, иначе цикл начнёт копить прогресс заново.
        var endProg = -1.0
        for i in 0..31 do
            if sys.hold(1.0 / 60.0) != nil do
                endProg = sys.holdProgress
                break
            end
        end
        global firedEnd = fired
        global endProgress = endProg

        # Отпустили — прогресс сбрасывается.
        for i in 0..20 do sys.hold(1.0 / 60.0) end
        sys.releaseHold()
        global afterRelease = sys.holdProgress

        # Смена цели сбрасывает прогресс: удержание сундука не "перетекает"
        # на дверь, к которой игрок повернулся.
        for i in 0..30 do sys.hold(1.0 / 60.0) end
        let carried = sys.holdProgress
        sys.update(vec3(0, 0, 0), vec3(0, 0, -1), 0.016)
        global switchedTag = sys.state()["tag"]
        global afterSwitch = sys.holdProgress
        global carriedBefore = carried
    )LV");
    check(ok, "сценарий удержания выполнился");
    if (!ok) return;

    check(h.num("midProgress") > 0.4 && h.num("midProgress") < 0.6,
          "на половине удержания прогресс ~0.5");
    check(near(h.num("firedMid"), 0.0), "до конца удержания действие не сработало");
    check(near(h.num("firedEnd"), 1.0), "по завершении удержания действие сработало один раз");
    check(near(h.num("endProgress"), 0.0), "после срабатывания прогресс сброшен");
    check(near(h.num("afterRelease"), 0.0), "releaseHold() сбрасывает прогресс");
    check(h.num("carriedBefore") > 0.3, "перед сменой цели прогресс был накоплен");
    check(h.str("switchedTag") == "b", "после разворота выбрана другая цель");
    check(near(h.num("afterSwitch"), 0.0), "смена цели обнулила прогресс удержания");
}

// ---------------------------------------------------------------------------
static void testLineOfSight() {
    section("InteractionSystem: проверка видимости");
    Harness h;

    const bool ok = h.run({"interaction"}, R"LV(
        global target = spawn()
        setPosition(target, vec3(0, 0, 4))
        addRigidBody(target, vec3(0.5, 0.5, 0.5), 0)

        global sys = new InteractionSystem()
        sys.defaultRange = 10.0
        sys.requireLineOfSight = true
        sys.register(target, { "label": "Рычаг", "tag": "lever" })

        # Препятствий нет — цель видна.
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global clearTag = sys.state()["tag"]

        # Ставим стену между игроком и целью.
        global wall = spawn()
        setPosition(wall, vec3(0, 0, 2))
        addRigidBody(wall, vec3(2.0, 2.0, 0.3), 0)
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global blockedTarget = sys.state()["hasTarget"]

        # Без требования видимости стена не мешает.
        sys.requireLineOfSight = false
        sys.update(vec3(0, 0, 0), vec3(0, 0, 1), 0.016)
        global ignoredTag = sys.state()["tag"]
    )LV");
    check(ok, "сценарий видимости выполнился");
    if (!ok) return;

    check(h.str("clearTag") == "lever", "без препятствий цель доступна");
    check(!h.boolean("blockedTarget"), "стена перекрывает взаимодействие");
    check(h.str("ignoredTag") == "lever", "при requireLineOfSight=false стена не мешает");
}

// ---------------------------------------------------------------------------
static void testCombinedWithInput() {
    section("Связка: ввод -> CharacterController -> InteractionSystem");
    Harness h;

    auto& gameplay = h.input.createContext("Gameplay", 0);
    gameplay.mapKeyAxis("MoveX", 65, -1.f);   // A
    gameplay.mapKeyAxis("MoveX", 68, +1.f);   // D
    gameplay.mapKeyAxis("MoveY", 87, +1.f);   // W
    gameplay.mapKeyAxis("MoveY", 83, -1.f);   // S
    gameplay.mapKey("Interact", 69);          // E
    h.input.pushContext("Gameplay");

    const bool ok = h.run({"character", "interaction"}, R"LV(
        global player = spawn()
        setPosition(player, vec3(0, 0, 0))
        global cc = new CharacterController(player)
        cc.speed = 4.0
        cc.mode = "world"

        global chest = spawn()
        setPosition(chest, vec3(0, 0, 5))
        global opened = false
        global dbgPrompt = ""
        global sys = new InteractionSystem()
        sys.defaultRange = 2.0
        sys.register(chest, { "label": "Открыть сундук", "action": || opened = true })

        func update(dt) do
            cc.move(inputAxis("MoveX"), inputAxis("MoveY"))
            cc.update(dt)
            sys.update(cc.position(), cc.forward(), dt)
            dbgPrompt = sys.prompt
            if inputPressed("Interact") do sys.activate() end
        end
    )LV");
    check(ok, "связка загрузилась");
    if (!ok) return;

    // Сундук далеко — подсказки быть не должно.
    h.input.beginFrame();
    h.scripts.tick(1.0f / 60.0f);
    check(h.scripts.vm().getGlobal("sys").isObject(), "система взаимодействия создана");

    // Держим W ровно секунду: при speed=4 игрок проходит 4 единицы и
    // останавливается ПЕРЕД сундуком (z=5), а не проскакивает его насквозь.
    for (int i = 0; i < 60; ++i) {
        h.input.beginFrame();
        h.input.onKey(87, true);          // W
        h.world.tick(1.0f / 60.0f);
        h.scripts.tick(1.0f / 60.0f);
    }
    const Vec3 pos = h.world.registry().require<Transform>(
        h.world.registry().allEntities().front()).position;
    check(pos.z > 3.0f && pos.z < 5.0f,
          "удержание W подвело игрока к сундуку, не проскочив его");
    check(h.str("dbgPrompt") == "Открыть сундук",
          "подсказка появилась, когда игрок подошёл на расстояние взаимодействия");

    // Подошли — нажимаем E.
    h.input.beginFrame();
    h.input.onKey(69, true);              // E
    h.scripts.tick(1.0f / 60.0f);
    check(h.scripts.vm().getGlobal("opened").truthy(),
          "нажатие E у сундука выполнило действие взаимодействия");
}

// ---------------------------------------------------------------------------
int main() {
    JobSystem::init(0);
    std::printf("=== LimVine gameplay modules self-test ===\n");

    testCharacterMovement();
    testCameraRelativeMovement();
    testGravityAndJump();
    testBoundsAndFacing();
    testInteractionBasics();
    testInteractionFilters();
    testInteractionHold();
    testLineOfSight();
    testCombinedWithInput();

    std::printf("\n----------------------------------------\n");
    std::printf("Gameplay self-test: %d passed, %d failed\n", g_passed, g_failed);
    JobSystem::shutdown();
    return g_failed == 0 ? 0 : 1;
}
