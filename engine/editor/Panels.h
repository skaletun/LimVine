/**
 * @file    Panels.h
 * @brief   Панели редактора LimVine.
 * @ingroup Editor
 *
 * Панели не зависят от ImGui: они рисуют себя в `UIDraw` (см. EditorUI.h),
 * поэтому их можно прогнать headless (TextUIDraw) и проверить тестами.
 */
#pragma once

#include "EditorUI.h"
#include "../asset/AssetManager.h"
#include "../ecs/World.h"
#include "../scripting/EngineBindings.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace lv::editor {

/// Панель принадлежит EditorApp.
using PanelPtr = std::unique_ptr<Panel>;

/**
 * @brief Общий контекст панелей: на что они смотрят и что могут менять.
 */
struct EditorContext {
    ecs::World*                 world    = nullptr;
    scripting::ScriptWorld*     scripts  = nullptr;
    asset::AssetManager*        assets   = nullptr;
    render::Renderer*           renderer = nullptr;

    ecs::Entity                 selected{};        ///< Выбранная сущность.
    bool                        hasSelection = false;
    std::vector<std::string>    consoleLog;        ///< История консоли.
    std::string                 replInput;         ///< Текущая строка REPL.
};

// ---------------------------------------------------------------------------
//  Hierarchy
// ---------------------------------------------------------------------------
/**
 * @brief Дерево сущностей сцены.
 *
 * Показывает все живые сущности, их компоненты (именами из
 * ComponentMetaRegistry) и позволяет выбрать сущность кликом. В headless-режиме
 * выбор делается методом `selectByIndex` (его используют тесты).
 */
class HierarchyPanel final : public Panel {
public:
    explicit HierarchyPanel(EditorContext& ctx) : ctx_(ctx) {}
    [[nodiscard]] const char* name() const override { return "Hierarchy"; }
    void draw(UIDraw& ui) override;

    /// Список сущностей в порядке отрисовки (кэш последнего кадра).
    [[nodiscard]] const std::vector<ecs::Entity>& listed() const noexcept { return listed_; }
    /// Выбрать i-ю сущность из списка (эквивалент клика).
    bool selectByIndex(std::size_t i);

    std::string filter;                ///< Фильтр по имени/компоненту.

private:
    EditorContext& ctx_;
    std::vector<ecs::Entity> listed_;
};

// ---------------------------------------------------------------------------
//  Inspector
// ---------------------------------------------------------------------------
/**
 * @brief Инспектор выбранной сущности: компоненты и их поля.
 *
 * Значения полей читаются через `BindingRegistry` (те же offset'ы, что
 * использует LV Script), поэтому инспектор и скрипты видят ОДНУ память:
 * правка в инспекторе мгновенно видна в `getField()`.
 */
class InspectorPanel final : public Panel {
public:
    explicit InspectorPanel(EditorContext& ctx) : ctx_(ctx) {}
    [[nodiscard]] const char* name() const override { return "Inspector"; }
    void draw(UIDraw& ui) override;

    /// Прочитать поле компонента как строку (для тестов и снапшотов).
    [[nodiscard]] std::string fieldAsString(const char* component, const char* field) const;
    /// Записать числовое поле (эквивалент правки в виджете).
    bool setFloatField(const char* component, const char* field, float v);

private:
    EditorContext& ctx_;
};

// ---------------------------------------------------------------------------
//  Asset Browser
// ---------------------------------------------------------------------------
/**
 * @brief Браузер ассетов: дерево каталогов, состояние кэша, перезагрузка.
 */
class AssetBrowserPanel final : public Panel {
public:
    explicit AssetBrowserPanel(EditorContext& ctx) : ctx_(ctx) {}
    [[nodiscard]] const char* name() const override { return "Asset Browser"; }
    void draw(UIDraw& ui) override;

private:
    EditorContext& ctx_;
    std::string root_ = "assets";
};

// ---------------------------------------------------------------------------
//  Viewport
// ---------------------------------------------------------------------------
/**
 * @brief Окно сцены: камера, gizmo выбранной сущности, оси мира.
 *
 * Реальная отрисовка идёт через `render::Renderer`; здесь панель лишь выдаёт
 * gizmo-примитивы в `UIDraw`, поэтому её содержимое проверяемо headless.
 */
class ViewportPanel final : public Panel {
public:
    explicit ViewportPanel(EditorContext& ctx) : ctx_(ctx) {}
    [[nodiscard]] const char* name() const override { return "Viewport"; }
    void draw(UIDraw& ui) override;

    bool showGizmos = true;
    bool showGrid = true;
    float gridStep = 2.0f;

private:
    EditorContext& ctx_;
};

// ---------------------------------------------------------------------------
//  Console + REPL
// ---------------------------------------------------------------------------
/**
 * @brief Консоль: лог движка/скриптов и REPL поверх LV Script.
 *
 * Команды:
 *   `:<выражение>`  — вычислить выражение LV Script и напечатать результат;
 *   `:disasm`       — дизассемблер последнего загруженного модуля;
 *   `:reload <path>`— горячая перезагрузка скрипта (с сохранением состояния);
 *   `:entities`     — число живых сущностей;
 *   всё остальное   — исполняется как фрагмент LV Script (через runFile).
 */
class ConsolePanel final : public Panel {
public:
    explicit ConsolePanel(EditorContext& ctx) : ctx_(ctx) {}
    [[nodiscard]] const char* name() const override { return "Console"; }
    void draw(UIDraw& ui) override;

    /// Выполнить строку ввода (эквивалент Enter). Возвращает вывод.
    std::string execute(const std::string& line);
    /// Добавить строку в лог (туда же пишет setLogCallback движка).
    void log(const std::string& line);

    std::size_t maxLines = 512;

private:
    EditorContext& ctx_;
};

// ---------------------------------------------------------------------------
//  Приложение редактора
// ---------------------------------------------------------------------------
/**
 * @brief Собирает панели и рисует кадр.
 *
 * Редактор не владеет подсистемами: они приходят извне (из игрового цикла или
 * из main() редактора), поэтому один и тот же EditorApp работает и «в игре»,
 * и в отдельном окне.
 */
class EditorApp {
public:
    explicit EditorApp(EditorContext& ctx);

    /// Зарегистрировать стандартный набор панелей.
    void addDefaultPanels();
    void addPanel(PanelPtr panel);

    /// Один кадр редактора.
    void drawFrame(UIDraw& ui);

    /// Снимок всех видимых панелей текстом (для `--dump-panels` и тестов).
    [[nodiscard]] std::string dumpPanels() const;

    [[nodiscard]] const std::vector<PanelPtr>& panels() const noexcept { return panels_; }
    [[nodiscard]] Panel* findPanel(const char* name);



private:
    EditorContext& ctx_;
    std::vector<PanelPtr> panels_;
    mutable TextUIDraw textUi_;
};

} // namespace lv::editor
