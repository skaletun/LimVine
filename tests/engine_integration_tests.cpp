/**
 * @file    engine_integration_tests.cpp
 * @brief   Сквозные тесты: LV Script управляет ECS/физикой/вводом движка.
 *
 * Это главный тест интеграции: он доказывает, что скрипт действительно
 * манипулирует сущностями и компонентами C++, а не живёт в своём мире.
 */
#include "scripting/EngineBindings.h"
#include "ecs/World.h"
#include "physics/Physics.h"
#include "input/Input.h"
#include "audio/Audio.h"
#include "render/Renderer.h"

#include <cstdio>
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
void section(const char* s) { std::printf("\n== %s ==\n", s);
}
bool logHas(const std::string& s) {
    for (const auto& l : g_log) if (l.find(s) != std::string::npos) return true;
    return false;
}
} // namespace

int main() {
    JobSystem::init(0);

    // --- Собираем «мини-движок» ---
    World world;
    registerComponent<Transform>();
    registerComponent<render::MeshRenderer>();

    physics::PhysicsWorld physics;
    physics.init(1024);
    physics::attachToWorld(physics, world);

    input::InputSystem input;
    auto& gameplay = input.createContext("Gameplay", 0);
    gameplay.mapKeyAxis("MoveX", 65, -1.f);   // A
    gameplay.mapKeyAxis("MoveX", 68, +1.f);   // D
    gameplay.mapKey("Jump", 32);              // Space
    gameplay.mapMouseButton("Fire", 0);
    input.pushContext("Gameplay");

    audio::AudioSystem audio;
    audio.init(16, /*forceNull=*/true);
    audio::attachToWorld(audio, world);

    render::Renderer renderer;
    render::SurfaceDesc surface;
    renderer.init(render::Backend::Null, surface);
    renderer.attachToWorld(world);

    EngineContext ctx;
    ctx.world = &world;
    ctx.physics = &physics;
    ctx.input = &input;
    ctx.audio = &audio;
    ctx.renderer = &renderer;

    ScriptWorld scripts;
    scripts.setLogCallback([](const std::string& s) { g_log.push_back(s); std::printf("    | %s\n", s.c_str()); });
    scripts.attach(ctx);

    // ------------------------------------------------------------------
    section("script drives ECS");
    g_log.clear();
    check(scripts.runFile("game.lvs", R"LV(
        global player = spawn()
        setPosition(player, vec3(0, 1, 0))
        print("spawned", entityCount())

        func update(dt) do
            let move = inputAxis("MoveX")
            translate(player, vec3(move * 5.0 * dt, 0, 0))
            if frameIndex() <= 2 do
                print("tick", frameIndex(), "dt", dt, "move", move)
                print("  player=", player, "alive=", alive(player), "pos=", getPosition(player))
            end
            if inputPressed("Jump") do
                print("jump!")
                applyImpulse(player, vec3(0, 6, 0))
            end
        end
    )LV"), "script compiled and ran");
    check(logHas("spawned"), "spawn() reported back to the script");
    check(world.entityCount() == 1, "script created exactly one entity through the binding");

    // Проверяем, что Transform действительно изменён из C++-стороны.
    // ВАЖНО: читаем именно player (первую сущность), а не «последнюю в each()» —
    // последующие скрипты создают свои сущности.
    const Entity playerEntity = world.registry().allEntities().front();
    Vec3 posBefore = world.registry().require<Transform>(playerEntity).position;
    check(posBefore.y == 1.0f, "setPosition() wrote into the C++ Transform component");

    // ------------------------------------------------------------------
    section("input -> script -> ECS");
    input.beginFrame();
    input.onKey(68, true);       // D -> MoveX = +1
    std::printf("    [dbg] MoveX axis = %g, entityCount=%zu\n",
                (double)input.axis("MoveX"), world.entityCount());
    for (int i = 0; i < 10; ++i) {
        world.tick(1.0f / 60.0f);
        scripts.tick(1.0f / 60.0f);
    }
    std::printf("    [dbg] player x after 10 ticks = %g\n",
                (double)world.registry().require<Transform>(playerEntity).position.x);
    const Vec3 posAfter = world.registry().require<Transform>(playerEntity).position;
    std::printf("    moved from x=%g to x=%g\n", (double)posBefore.x, (double)posAfter.x);
    check(posAfter.x > posBefore.x + 0.5f, "holding D moved the entity through the script update loop");

    g_log.clear();
    input.beginFrame();
    input.onKey(32, true);
    scripts.tick(1.0f / 60.0f);
    check(logHas("jump!"), "inputPressed('Jump') fired the script handler");
    // Проверяем именно ДОСТУПНОСТЬ физики из биндингов: создаём тело из скрипта
    // и убеждаемся, что оно появилось в PhysicsWorld. Простое `bodyCount() >= 0`
    // было бы пустым утверждением (unsigned всегда >= 0), а `> 0` — ложным:
    // до этого момента скрипт тел не создавал.
    const std::size_t bodiesBefore = physics.bodyCount();
    check(scripts.runFile("phys.lvs", R"LV(
        let crate = spawn()
        setPosition(crate, vec3(0, 2, 0))
        addRigidBody(crate, vec3(0.5, 0.5, 0.5), 2)   # 2 = Dynamic
        applyImpulse(crate, vec3(0, 5, 0))
    )LV"), "script created a dynamic body through the binding");
    check(physics.bodyCount() == bodiesBefore + 1, "physics world is reachable from bindings");

    // ------------------------------------------------------------------
    section("component access by name");
    g_log.clear();
    check(scripts.runFile("components.lvs", R"LV(
        let e = spawn()
        setPosition(e, vec3(1, 2, 3))
        let p = getPosition(e)
        print("pos", p.x, p.y, p.z)
        setField(e, "Transform", "position", vec3(9, 9, 9))
        let p2 = getField(e, "Transform", "position")
        print("pos2", p2.x, p2.y, p2.z)
        print("has", hasComponent(e, "Transform"), hasComponent(e, "Nope"))

        # setField обязан САМ создавать компонент, если его ещё нет:
        # spawn() добавляет только Transform, а шаблоны красят сущности через
        # setField(e, "MeshRenderer", "tint", ...). Без автосоздания такие
        # вызовы молча возвращали false, и цвет не применялся.
        print("before", hasComponent(e, "MeshRenderer"))
        setField(e, "MeshRenderer", "tint", color(0.25, 0.5, 0.75, 1.0))
        print("after", hasComponent(e, "MeshRenderer"))
        let t = getField(e, "MeshRenderer", "tint")
        print("tint", t.r, t.g, t.b)
    )LV"), "component script ran");
    check(logHas("before\tfalse"), "MeshRenderer is absent right after spawn()");
    check(logHas("after\ttrue"), "setField() creates the missing component");
    check(logHas("tint\t0.25\t0.5\t0.75"), "the written tint is readable back");
    check(logHas("pos\t1\t2\t3"), "getPosition returns the C++ Transform");
    check(logHas("pos2\t9\t9\t9"), "setField/getField write and read component memory");
    check(logHas("has\ttrue\tfalse"), "hasComponent reflects the archetype");

    Vec3 written{0, 0, 0};
    world.registry().each<Transform>([&](Entity, Transform& t) {
        if (t.position.x == 9.0f) written = t.position;
        return true;
    });
    check(written.x == 9.0f && written.z == 9.0f, "script-written value is visible in C++ memory");

    // ------------------------------------------------------------------
    section("coroutines as game timers");
    g_log.clear();
    check(scripts.runFile("timers.lvs", R"LV(
        coroutine regenerate(who) do
            wait(0.5)
            print("regen tick 1", who)
            wait(0.5)
            print("regen tick 2", who)
        end
        regenerate("player")
    )LV"), "coroutine script ran");
    check(!logHas("regen tick 1"), "wait(0.5) suspends immediately (nothing printed yet)");
    scripts.tick(0.6f);
    check(logHas("regen tick 1"), "scheduler resumed the coroutine after 0.5 s of game time");
    check(!logHas("regen tick 2"), "second wait is still pending");
    for (int i = 0; i < 4 && !logHas("regen tick 2"); ++i) scripts.tick(0.2f);
    check(logHas("regen tick 2"), "second wait resumed after another 0.5 s of game time");
    std::printf("    (active=%zu waiting=%zu gameTime=%.2f)\n",
                scripts.scheduler().activeCount(), scripts.scheduler().waitingCount(),
                scripts.vm().gameTime());

    // ------------------------------------------------------------------
    section("sandbox: mod without Cap_Spawn");
    {
        SandboxConfig modCfg;
        modCfg.capabilities = Cap_ModSandbox;    // без Cap_Spawn и Cap_IO
        ScriptWorld mod(modCfg);                 // конфиг ОБЯЗАТЕЛЬНО передаётся в конструктор
        std::vector<std::string> modLog;
        mod.setLogCallback([&](const std::string& s) { modLog.push_back(s); });
        mod.attach(ctx);
        mod.runFile("mod.lvs", "let stolen = spawn()\nprint(\"mod spawned\", entityCount())\n");
        bool blocked = false;
        for (const auto& l : modLog) {
            std::printf("    [mod] %s\n", l.c_str());
            if (l.find("sandbox violation") != std::string::npos) blocked = true;
        }
        check(blocked, "mod sandbox rejects spawn() without Cap_Spawn");
        check(!logHas("mod spawned"), "the sandboxed script did not continue past the violation");
    }

    // ------------------------------------------------------------------
    section("hot reload keeps state");
    g_log.clear();
    check(scripts.runFile("hot.lvs", R"LV(
        global counter = 0
        func bump() do
            counter += 1
            return counter
        end
        print("v1", bump())
    )LV"), "v1 loaded");
    check(logHas("v1\t1"), "v1 counter == 1");
    g_log.clear();
    check(scripts.hotReload("hot.lvs", R"LV(
        global counter = 100
        func bump() do
            counter += 5
            return counter
        end
        print("v2", bump())
    )LV"), "hot reload succeeded");
    check(logHas("hot-reload"), "reload reported to the log");

    renderer.shutdown();
    physics.shutdown();
    audio.shutdown();
    JobSystem::shutdown();

    std::printf("\n----------------------------------------\n");
    std::printf("Engine integration self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
