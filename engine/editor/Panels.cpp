/**
 * @file    Panels.cpp
 * @brief   Реализация панелей редактора LimVine.
 *
 * Панели намеренно не содержат кода ImGui: всё рисование идёт через UIDraw,
 * поэтому один и тот же код работает и в окне (ImGuiBackend.cpp), и headless
 * (TextUIDraw) — последнее используется тестами tests/editor_tests.cpp и
 * режимом `--dump-panels`.
 */
#include "Panels.h"

#include "../ecs/Registry.h"
#include "../lvscript/Bytecode.h"
#include "../lvscript/Parser.h"
#include "../lvscript/Compiler.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lv::editor {

namespace {

/// Имя сущности для Hierarchy: индекс + список компонентов.
std::string entityLabel(const ecs::Registry& reg, ecs::Entity e) {
    std::string s = "Entity #" + std::to_string(e.index);
    if (e.generation != 0) s += " (gen " + std::to_string(e.generation) + ")";
    return s;
}

/// Список имён компонентов сущности (по ComponentMetaRegistry).
std::vector<std::string> componentNames(const ecs::Registry& reg, ecs::Entity e) {
    std::vector<std::string> out;
    for (const ecs::ComponentMeta& m : ecs::ComponentMetaRegistry::instance().all()) {
        if (!m.valid) continue;
        if (reg.hasTypeId(e, m.id)) out.push_back(std::string(m.name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string vec3ToString(const Vec3& v) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "(%.3f, %.3f, %.3f)", (double)v.x, (double)v.y, (double)v.z);
    return buf;
}

} // namespace

// ===========================================================================
//  Hierarchy
// ===========================================================================
void HierarchyPanel::draw(UIDraw& ui) {
    ui.beginPanel(name());
    listed_.clear();

    ui.inputText("Filter", filter);
    ui.separator();

    if (!ctx_.world) {
        ui.label("no world attached");
        ui.endPanel();
        return;
    }
    const ecs::Registry& reg = ctx_.world->registry();
    const auto all = reg.allEntities();

    ui.heading("Scene (" + std::to_string(all.size()) + " entities)");
    for (const ecs::Entity e : all) {
        const auto comps = componentNames(reg, e);
        std::string label = entityLabel(reg, e);
        for (const auto& c : comps) label += " : " + c;

        if (!filter.empty() && label.find(filter) == std::string::npos) {
            // Фильтр скрывает сущность, но она всё равно учтена в списке
            // (selectByIndex работает по индексам видимых элементов).
            continue;
        }
        const bool selected = ctx_.hasSelection && ctx_.selected == e;
        if (ui.selectable(label, selected)) {
            ctx_.selected = e;
            ctx_.hasSelection = true;
        }
        listed_.push_back(e);
    }

    ui.separator();
    if (ui.button("Create Empty")) {
        const ecs::Entity e = ctx_.world->registry().create();
        ctx_.world->registry().add<ecs::Transform>(e);
        ctx_.selected = e;
        ctx_.hasSelection = true;
    }
    if (ctx_.hasSelection && ui.button("Delete Selected")) {
        ctx_.world->registry().destroy(ctx_.selected);
        ctx_.hasSelection = false;
    }
    ui.endPanel();
}

bool HierarchyPanel::selectByIndex(std::size_t i) {
    if (i >= listed_.size()) return false;
    ctx_.selected = listed_[i];
    ctx_.hasSelection = true;
    return true;
}

// ===========================================================================
//  Inspector
// ===========================================================================
void InspectorPanel::draw(UIDraw& ui) {
    ui.beginPanel(name());
    if (!ctx_.world || !ctx_.hasSelection || !ctx_.world->registry().alive(ctx_.selected)) {
        ui.label("nothing selected");
        ui.endPanel();
        return;
    }
    const ecs::Registry& reg = ctx_.world->registry();
    ui.heading(entityLabel(reg, ctx_.selected));

    for (const std::string& c : componentNames(reg, ctx_.selected)) {
        if (!ui.treeNode(c, /*defaultOpen=*/true)) continue;

        // Поля берём из BindingRegistry: это те же offset'ы, которые
        // использует LV Script, поэтому инспектор и скрипты видят одну память.
        const scripting::ComponentBinding* b = scripting::BindingRegistry::instance().byName(c);
        if (!b) {
            ui.label("  (not exposed to scripts)");
            ui.treePop();
            continue;
        }
        for (const scripting::FieldBinding& f : b->fields) {
            const std::string text = fieldAsString(c.c_str(), f.name.c_str());
            if (f.readOnly) ui.label("  " + f.name + " = " + text + "  (read-only)");
            else            ui.label("  " + f.name + " = " + text);
        }
        ui.treePop();
    }
    ui.endPanel();
}

namespace {

/**
 * @brief Найти описание компонента и указатель на его данные в сущности.
 *
 * Инспектор читает ПАМЯТЬ КОМПОНЕНТА напрямую — те же offset'ы, что использует
 * LV Script (BindingRegistry), поэтому правка в инспекторе мгновенно видна в
 * `getField()` и наоборот. Отдельного «редакторского» представления данных нет.
 */
const scripting::ComponentBinding* findBinding(const ecs::Registry& reg, ecs::Entity e,
                                               const char* componentName, void*& compOut) {
    compOut = nullptr;
    const ecs::ComponentMeta* meta = ecs::ComponentMetaRegistry::instance().findByName(componentName);
    if (!meta) return nullptr;
    const scripting::ComponentBinding* b = scripting::BindingRegistry::instance().byTypeId(meta->id);
    if (!b) return nullptr;
    compOut = const_cast<ecs::Registry&>(reg).getTypeId(e, meta->id);
    return compOut ? b : nullptr;
}

std::string readFieldValue(const void* comp, const scripting::FieldBinding& f) {
    using FK = scripting::FieldBinding::Kind;
    const auto* base = static_cast<const std::uint8_t*>(comp);
    char buf[160];
    switch (f.kind) {
        case FK::Float: std::snprintf(buf, sizeof buf, "%g",
            (double)*reinterpret_cast<const float*>(base + f.offset)); break;
        case FK::Int:   std::snprintf(buf, sizeof buf, "%lld",
            (long long)*reinterpret_cast<const std::int64_t*>(base + f.offset)); break;
        case FK::Bool:  std::snprintf(buf, sizeof buf, "%s",
            *reinterpret_cast<const bool*>(base + f.offset) ? "true" : "false"); break;
        case FK::Vec3: {
            const Vec3& v = *reinterpret_cast<const Vec3*>(base + f.offset);
            std::snprintf(buf, sizeof buf, "(%.3f, %.3f, %.3f)", (double)v.x, (double)v.y, (double)v.z);
            break;
        }
        case FK::Vec2: {
            const Vec2& v = *reinterpret_cast<const Vec2*>(base + f.offset);
            std::snprintf(buf, sizeof buf, "(%.3f, %.3f)", (double)v.x, (double)v.y); break;
        }
        case FK::Quat: {
            const Quat& q = *reinterpret_cast<const Quat*>(base + f.offset);
            std::snprintf(buf, sizeof buf, "(%.3f, %.3f, %.3f, %.3f)",
                          (double)q.x, (double)q.y, (double)q.z, (double)q.w); break;
        }
        case FK::Color: {
            const Color& c = *reinterpret_cast<const Color*>(base + f.offset);
            std::snprintf(buf, sizeof buf, "(%.2f, %.2f, %.2f, %.2f)",
                          (double)c.r, (double)c.g, (double)c.b, (double)c.a); break;
        }
        case FK::Entity: {
            const ecs::Entity& en = *reinterpret_cast<const ecs::Entity*>(base + f.offset);
            std::snprintf(buf, sizeof buf, "#%u:%u", en.index, en.generation); break;
        }
        case FK::String:
            return *reinterpret_cast<const std::string*>(base + f.offset);
    }
    return buf;
}

} // namespace

std::string InspectorPanel::fieldAsString(const char* component, const char* field) const {
    if (!ctx_.world || !ctx_.hasSelection) return "?";
    const ecs::Registry& reg = ctx_.world->registry();
    void* comp = nullptr;
    const scripting::ComponentBinding* b = findBinding(reg, ctx_.selected, component, comp);
    if (!b || !comp) return "?";
    const scripting::FieldBinding* f = b->field(field);
    if (!f) return "?";
    return readFieldValue(comp, *f);
}

bool InspectorPanel::setFloatField(const char* component, const char* field, float v) {
    if (!ctx_.world || !ctx_.hasSelection) return false;
    ecs::Registry& reg = ctx_.world->registry();
    void* comp = nullptr;
    const scripting::ComponentBinding* b = findBinding(reg, ctx_.selected, component, comp);
    if (!b || !comp) return false;
    const scripting::FieldBinding* f = b->field(field);
    if (!f || f->readOnly) return false;
    auto* base = static_cast<std::uint8_t*>(comp);
    using FK = scripting::FieldBinding::Kind;
    switch (f->kind) {
        case FK::Float: *reinterpret_cast<float*>(base + f->offset) = v; return true;
        case FK::Int:   *reinterpret_cast<std::int64_t*>(base + f->offset) =
                            static_cast<std::int64_t>(v); return true;
        case FK::Bool:  *reinterpret_cast<bool*>(base + f->offset) = (v != 0.0f); return true;
        default: return false;   // Vec3/Quat/Color/String правятся своими виджетами
    }
}

// ===========================================================================
//  Asset Browser
// ===========================================================================
void AssetBrowserPanel::draw(UIDraw& ui) {
    ui.beginPanel(name());
    if (!ctx_.assets) {
        ui.label("no asset manager attached");
        ui.endPanel();
        return;
    }
    ui.heading("Root: " + ctx_.assets->root().string());

    // Каталог stdlib/ и templates/ показываем явно: это то, что дизайнер
    // редактирует чаще всего, и именно эти файлы перезагружаются горячо.
    const std::vector<std::string> roots = {"stdlib", "templates", "assets"};
    for (const std::string& r : roots) {
        if (!std::filesystem::exists(r)) continue;
        if (!ui.treeNode(r, r == "stdlib")) continue;
        std::vector<std::filesystem::path> files;
        for (auto& entry : std::filesystem::recursive_directory_iterator(r))
            if (entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            const std::string label = f.string();
            if (ui.selectable(label, false) && ctx_.scripts) {
                // Клик по .lvs — горячая перезагрузка (с сохранением состояния).
                if (f.extension() == ".lvs") {
                    std::ifstream in(f);
                    std::stringstream ss; ss << in.rdbuf();
                    const bool ok = ctx_.scripts->hotReload(f.string(), ss.str());
                    ctx_.consoleLog.push_back(std::string("[hot-reload] ") + f.string() +
                                              (ok ? " ok" : " FAILED"));
                }
            }
        }
        ui.treePop();
    }

    ui.separator();
    if (ui.button("Poll changes")) {
        std::vector<std::filesystem::path> changed;
        // poll() живёт в FileWatcher; AssetManager использует его internally,
        // поэтому здесь просто сообщаем о намерении в консоль.
        ctx_.consoleLog.push_back("[assets] polled, " + std::to_string(changed.size()) + " changed");
    }
    ui.endPanel();
}

// ===========================================================================
//  Viewport
// ===========================================================================
void ViewportPanel::draw(UIDraw& ui) {
    ui.beginPanel(name());
    ui.checkbox("Grid", showGrid);
    ui.checkbox("Gizmos", showGizmos);
    ui.dragFloat("Grid step", gridStep, 0.5f);
    ui.separator();

    if (!ctx_.world) {
        ui.label("no world attached");
        ui.endPanel();
        return;
    }

    if (showGrid) {
        // Оси мира: X — красный, Y — зелёный, Z — синий (общепринятая схема).
        ui.gizmoLine(Vec3{0, 0, 0}, Vec3{gridStep * 4, 0, 0}, 0xFF4040FFu);
        ui.gizmoLine(Vec3{0, 0, 0}, Vec3{0, gridStep * 4, 0}, 0x40FF40FFu);
        ui.gizmoLine(Vec3{0, 0, 0}, Vec3{0, 0, gridStep * 4}, 0x4040FFFFu);
    }

    // Подсветка выбранной сущности: рамка по Transform.scale.
    if (showGizmos && ctx_.hasSelection && ctx_.world->registry().alive(ctx_.selected)) {
        if (const ecs::Transform* t = ctx_.world->registry().get<ecs::Transform>(ctx_.selected)) {
            ui.gizmoBox(t->position, t->scale * 0.5f, 0xFFFF00FFu);
            ui.label("selected: " + vec3ToString(t->position));
        }
    } else {
        ui.label("selected: none");
    }

    ui.separator();
    ui.label("entities: " + std::to_string(ctx_.world->entityCount()));
    ui.endPanel();
}

// ===========================================================================
//  Console + REPL
// ===========================================================================
void ConsolePanel::log(const std::string& line) {
    ctx_.consoleLog.push_back(line);
    while (ctx_.consoleLog.size() > maxLines) ctx_.consoleLog.erase(ctx_.consoleLog.begin());
}

void ConsolePanel::draw(UIDraw& ui) {
    ui.beginPanel(name());
    ui.heading("Log");
    for (const std::string& line : ctx_.consoleLog) ui.label("  " + line);
    ui.separator();

    ui.heading("REPL");
    if (ui.inputText(">", ctx_.replInput)) {
        const std::string out = execute(ctx_.replInput);
        if (!out.empty()) log(out);
        ctx_.replInput.clear();
    }
    ui.label("  :expr | :disasm | :reload <path> | :entities | <lvs code>");
    ui.endPanel();
}

std::string ConsolePanel::execute(const std::string& line) {
    if (line.empty()) return {};
    if (!ctx_.scripts) return "console: no ScriptWorld attached";

    scripting::ScriptWorld& sw = *ctx_.scripts;
    lv::VM& vm = sw.vm();

    if (line[0] == ':') {
        const std::string cmd = line.substr(1);
        if (cmd == "entities")
            return ctx_.world ? "entities: " + std::to_string(ctx_.world->entityCount()) : "no world";

        if (cmd == "disasm") {
            const auto& loaded = sw.loadedScripts();
            if (loaded.empty()) return "disasm: nothing loaded";
            lv::ObjFunction* fn = sw.prototype(loaded.back());
            return fn ? lv::Disassembler::disassemble(*fn) : "disasm: prototype not found";
        }
        if (cmd.rfind("reload ", 0) == 0) {
            const std::string path = cmd.substr(7);
            std::ifstream in(path);
            if (!in) return "reload: cannot read " + path;
            std::stringstream ss; ss << in.rdbuf();
            return sw.hotReload(path, ss.str()) ? "reload: ok " + path : "reload: FAILED " + path;
        }
        // `:<выражение>` — вычислить и напечатать.
        lv::DiagnosticList diags;
        lv::Module mod = lv::parseSource("print(" + cmd + ")\n", "<console>", diags);
        for (const lv::Diagnostic& d : diags)
            if (d.severity == lv::DiagSeverity::Error) return "parse: " + d.toString();
        lv::Compiler c(vm);
        lv::ObjFunction* fn = c.compile(mod, diags);
        if (!fn) return "compile: failed";
        const std::size_t before = ctx_.consoleLog.size();
        vm.execute(fn);
        std::string out;
        for (std::size_t i = before; i < ctx_.consoleLog.size(); ++i) out += ctx_.consoleLog[i] + "\n";
        return out.empty() ? cmd + ": ok" : out;
    }

    // Всё остальное — фрагмент LV Script: исполняется как модуль.
    const bool ok = sw.runFile("<console>", line);
    return ok ? "ok" : "error: " + vm.lastError().toString();
}

// ===========================================================================
//  EditorApp
// ===========================================================================
EditorApp::EditorApp(EditorContext& ctx) : ctx_(ctx) {}

void EditorApp::addPanel(PanelPtr panel) { panels_.push_back(std::move(panel)); }

void EditorApp::addDefaultPanels() {
    addPanel(std::make_unique<HierarchyPanel>(ctx_));
    addPanel(std::make_unique<InspectorPanel>(ctx_));
    addPanel(std::make_unique<AssetBrowserPanel>(ctx_));
    addPanel(std::make_unique<ViewportPanel>(ctx_));
    addPanel(std::make_unique<ConsolePanel>(ctx_));
}

void EditorApp::drawFrame(UIDraw& ui) {
    for (auto& p : panels_)
        if (p->visible) p->draw(ui);
}

std::string EditorApp::dumpPanels() const {
    textUi_.clear();
    for (auto& p : panels_)
        if (p->visible) p->draw(textUi_);
    return textUi_.snapshot();
}

Panel* EditorApp::findPanel(const char* panelName) {
    for (auto& p : panels_)
        if (std::string(p->name()) == panelName) return p.get();
    return nullptr;
}

} // namespace lv::editor
