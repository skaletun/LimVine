/**
 * @file    main.cpp
 * @brief   Точка входа редактора LimVine.
 * @ingroup Editor
 *
 * Два режима:
 *
 *   limvine-editor --dump-panels [template-dir]
 *       Headless: собирает мир, грузит шаблон, рисует панели в TextUIDraw и
 *       печатает их текстом. Нужен для CI и для отладки раскладки без окна.
 *
 *   limvine-editor [template-dir]        (требует LV_WITH_IMGUI)
 *       Обычный цикл: окно, панели, viewport, Play/Pause, горячая перезагрузка.
 *
 * Редактор НЕ владеет подсистемами: он собирает их так же, как TemplateRunner,
 * и передаёт в EditorContext указателями. Благодаря этому один и тот же
 * EditorApp можно встроить и в игровое окно (in-game editor).
 */
#include "Panels.h"
#include "core/Console.h"
#include "../scripting/TemplateRunner.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void usage() {
    std::printf(
        "LimVine Editor\n"
        "  limvine-editor [--dump-panels] [--frames N] [template-dir]\n"
        "\n"
        "  --dump-panels   headless: напечатать панели текстом и выйти\n"
        "  --frames N      число кадров симуляции перед дампом (по умолчанию 0)\n"
        "\n"
        "Без --dump-panels требуется сборка с LV_WITH_IMGUI=1.\n");
}

} // namespace

int main(int argc, char** argv) {
    lv::core::initConsole();
    bool dump = false;
    int frames = 0;
    std::string target;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--dump-panels") dump = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return 2; }
        else if (target.empty()) target = a;
    }

    lv::JobSystem::init(0);
    // Компоненты движка регистрируются в ComponentMetaRegistry/BindingRegistry:
    // без этого Hierarchy показывал бы только Transform (первый зарегистрированный
    // скриптом компонент), а Inspector не нашёл бы MeshRenderer/RigidBody.
    lv::scripting::registerEngineComponents();

    lv::scripting::TemplateRunner runner;
    runner.setLogCallback([](const std::string& s) { std::printf("[script] %s\n", s.c_str()); });

    if (!target.empty()) {
        const auto res = runner.load(target);
        if (!res.ok) {
            for (const auto& e : res.errors) std::fprintf(stderr, "%s\n", e.c_str());
            std::fprintf(stderr, "failed to load '%s'\n", target.c_str());
            return 1;
        }
        std::printf("# loaded template %s (%s)\n", res.name.c_str(), res.id.c_str());
    }

    lv::editor::EditorContext ctx;
    ctx.world    = &runner.world();
    ctx.scripts  = &runner.scripts();
    ctx.renderer = &runner.renderer();

    lv::editor::EditorApp app(ctx);
    app.addDefaultPanels();

    runner.runFrames(frames, 1.0f / 60.0f);

#ifdef LV_WITH_IMGUI
    if (!dump) {
        // Оконный цикл подключается здесь: создание окна/контекста ImGui,
        // затем каждый кадр app.drawFrame(imguiUi) + рендер gizmo из
        // ImGuiUIDraw::lines()/boxes(). Сборка окна специфична для платформы
        // (GLFW/SDL) и подключается вместе с LV_WITH_IMGUI.
        std::fprintf(stderr, "windowed mode requires the platform backend; use --dump-panels\n");
        return 3;
    }
#else
    if (!dump) {
        std::fprintf(stderr,
                     "this build has no ImGui backend (LV_WITH_IMGUI=OFF); "
                     "printing panels as text instead\n");
    }
#endif

    std::printf("%s", app.dumpPanels().c_str());
    return 0;
}
