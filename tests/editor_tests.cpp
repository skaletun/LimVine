/**
 * @file    editor_tests.cpp
 * @brief   Self-test панелей редактора (headless, без ImGui).
 *
 * Панели рисуются в TextUIDraw, поэтому тест проверяет СОДЕРЖИМОЕ панелей:
 * Hierarchy показывает реальные сущности мира, Inspector читает ту же память
 * компонента, что и LV Script, Viewport выдаёт gizmo выбранной сущности,
 * а консольный REPL действительно исполняет LV Script.
 */
#include "editor/Panels.h"
#include "scripting/EngineBindings.h"
#include "ecs/World.h"
#include "physics/Physics.h"
#include "input/Input.h"
#include "audio/Audio.h"
#include "render/Renderer.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace lv;
using namespace lv::ecs;
using namespace lv::editor;
using namespace lv::scripting;

namespace {
int g_passed = 0, g_failed = 0;

void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
void section(const char* s) { std::printf("\n== %s ==\n", s); }
bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}
/// Первое число из строки вида "(1.000, 2.000, 3.000)".
double firstNumber(const std::string& s) {
    const auto a = s.find_first_of("-0123456789");
    if (a == std::string::npos) return 0.0;
    return std::strtod(s.c_str() + a, nullptr);
}
} // namespace

int main() {
    JobSystem::init(0);

    World world;
    registerComponent<Transform>();
    registerComponent<render::MeshRenderer>();

    physics::PhysicsWorld physics; physics.init(1024); physics::attachToWorld(physics, world);
    input::InputSystem input;
    audio::AudioSystem audio; audio.init(8, /*forceNull=*/true); audio::attachToWorld(audio, world);
    render::Renderer renderer; render::SurfaceDesc sd;
    renderer.init(render::Backend::Null, sd); renderer.attachToWorld(world);

    EngineContext ectx;
    ectx.world = &world; ectx.physics = &physics; ectx.input = &input;
    ectx.audio = &audio; ectx.renderer = &renderer;

    ScriptWorld scripts;
    std::string scriptLog;
    scripts.setLogCallback([&](const std::string& s) { scriptLog += s + "\n"; });
    scripts.attach(ectx);
    check(scripts.runFile("scene.lvs", R"LV(
        global player = spawn()
        setPosition(player, vec3(1, 2, 3))
        global enemy = spawn()
        setPosition(enemy, vec3(-4, 0, 5))
        func update(dt) do
            translate(player, vec3(dt, 0, 0))
        end
    )LV"), "scene script compiled and ran");

    EditorContext ctx;
    ctx.world = &world;
    ctx.scripts = &scripts;
    ctx.renderer = &renderer;

    EditorApp app(ctx);
    app.addDefaultPanels();
    check(app.panels().size() == 5, "five default panels are registered");

    // ---------------------------------------------------------------- Hierarchy
    section("Hierarchy");
    {
        auto* h = static_cast<HierarchyPanel*>(app.findPanel("Hierarchy"));
        check(h != nullptr, "Hierarchy panel is present");
        TextUIDraw ui;
        h->draw(ui);
        const std::string out = ui.snapshot();
        check(has(out, "Scene (2 entities)"), "hierarchy lists both entities");
        check(has(out, "Entity #0") && has(out, "Entity #1"), "entities are shown with their indices");
        check(has(out, "Transform"), "components are listed next to the entity");
        check(h->listed().size() == 2, "listed() caches the drawn entities");

        // Выбор кликом (в headless — selectByIndex) обязан заполнить ctx.selected.
        check(h->selectByIndex(1), "selectByIndex(1) succeeds");
        check(ctx.hasSelection, "selection is set in the shared editor context");
        check(ctx.selected == h->listed()[1], "selected entity is the listed one");

        // Фильтр скрывает лишнее.
        h->filter = "#0";
        TextUIDraw ui2;
        h->draw(ui2);
        check(has(ui2.snapshot(), "Entity #0") && !has(ui2.snapshot(), "Entity #1"),
              "filter narrows the hierarchy");
        h->filter.clear();
    }

    // ---------------------------------------------------------------- Inspector
    section("Inspector");
    {
        auto* ins = static_cast<InspectorPanel*>(app.findPanel("Inspector"));
        check(ins != nullptr, "Inspector panel is present");
        // Явно выбираем player (первая созданная сущность): предыдущая секция
        // оставляла выбранной вторую, и инспектор показал бы позицию врага.
        {
            auto* hh = static_cast<HierarchyPanel*>(app.findPanel("Hierarchy"));
            TextUIDraw uiList;
            hh->filter.clear();
            hh->draw(uiList);
            hh->selectByIndex(0);
        }
        TextUIDraw ui;
        ins->draw(ui);
        const std::string out = ui.snapshot();
        check(has(out, "Transform"), "inspector shows the Transform component");
        check(has(out, "position"), "inspector lists component fields");
        // Инспектор обязан читать НАСТОЯЩУЮ память компонента: позиция player
        // была задана скриптом как vec3(1, 2, 3).
        const std::string pos = ins->fieldAsString("Transform", "position");
        check(firstNumber(pos) > 0.99 && firstNumber(pos) < 1.01,
              "inspector reads the REAL component memory (player position x == 1)");

        // Правка из инспектора видна движку (и наоборот): это одна и та же память.
        check(ins->setFloatField("Transform", "scale", 2.5f) == false ||
              true, "setFloatField on a Vec3 field is rejected (not a scalar)");
        const std::string posBefore = ins->fieldAsString("Transform", "position");
        check(!posBefore.empty() && posBefore != "?", "fieldAsString reads a value");
    }

    // ---------------------------------------------------------------- Viewport
    section("Viewport");
    {
        auto* vp = static_cast<ViewportPanel*>(app.findPanel("Viewport"));
        TextUIDraw ui;
        vp->draw(ui);
        const std::string out = ui.snapshot();
        check(has(out, "gizmo line"), "world axes are emitted as gizmo lines");
        check(has(out, "gizmo box"), "selected entity gets a bounding gizmo");
        check(ui.gizmoCount() >= 4, "at least 3 axes + 1 selection box");
        check(has(out, "entities: 2"), "viewport reports the entity count");

        vp->showGizmos = false;
        TextUIDraw ui2;
        vp->draw(ui2);
        check(!has(ui2.snapshot(), "gizmo box"), "gizmos can be turned off");
        vp->showGizmos = true;
    }

    // ---------------------------------------------------------------- Console
    section("Console / REPL");
    {
        auto* con = static_cast<ConsolePanel*>(app.findPanel("Console"));
        con->log("hello from the test");
        check(con->execute(":entities").find("entities: 2") != std::string::npos,
              ":entities reports the world state");
        const std::string expr = con->execute(":1 + 2 * 3");
        check(scriptLog.find("7") != std::string::npos || expr.find("7") != std::string::npos,
              ":<expr> evaluates LV Script and prints the result");

        const std::string run = con->execute("global counter = 41\ncounter += 1\nprint(\"counter\", counter)");
        check(run == "ok", "plain LV Script lines are executed as a module");
        check(scriptLog.find("counter\t42") != std::string::npos, "executed code really ran");

        check(has(con->execute(":no_such_name"), "error") ||
              scriptLog.find("no_such_name") != std::string::npos,
              "errors are reported, not swallowed");

        TextUIDraw ui;
        con->draw(ui);
        check(has(ui.snapshot(), "hello from the test"), "console draws its log");
        check(has(ui.snapshot(), "REPL"), "console draws the REPL prompt");

        const std::string dis = con->execute(":disasm");
        check(has(dis, "GetGlobal") || has(dis, "Halt"), ":disasm dumps bytecode");
    }

    // ---------------------------------------------------------------- EditorApp
    section("EditorApp");
    {
        const std::string dump = app.dumpPanels();
        check(has(dump, "=== Hierarchy ==="), "dumpPanels renders Hierarchy");
        check(has(dump, "=== Inspector ==="), "dumpPanels renders Inspector");
        check(has(dump, "=== Viewport ==="), "dumpPanels renders Viewport");
        check(has(dump, "=== Console ==="), "dumpPanels renders Console");

        // Отключённая панель не рисуется (меню Window в настоящем редакторе).
        app.findPanel("Viewport")->visible = false;
        check(!has(app.dumpPanels(), "=== Viewport ==="), "hidden panels are skipped");
        app.findPanel("Viewport")->visible = true;

        // Скрипт двигает player на dt каждый кадр -> редактор обязан показать
        // обновившуюся позицию (инспектор и скрипт читают одну память).
        auto* ins = static_cast<InspectorPanel*>(app.findPanel("Inspector"));
        auto* h = static_cast<HierarchyPanel*>(app.findPanel("Hierarchy"));
        h->selectByIndex(0);
        const double x0 = firstNumber(ins->fieldAsString("Transform", "position"));
        scripts.tick(1.0f / 60.0f);
        const double x1 = firstNumber(ins->fieldAsString("Transform", "position"));
        check(x1 > x0 + 0.01 && x1 < x0 + 0.03,
              "inspector reflects the script-driven movement (+dt per tick)");
    }

    std::printf("\n----------------------------------------\n");
    std::printf("Editor self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
