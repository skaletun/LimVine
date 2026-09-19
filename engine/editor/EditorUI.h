/**
 * @file    EditorUI.h
 * @brief   Абстракция immediate-mode UI редактора и контракт панелей.
 * @ingroup Editor
 *
 * @details Зачем прослойка над ImGui
 *          --------------------------
 *          Панели редактора (Hierarchy, Inspector, Asset Browser, Viewport,
 *          Console) пишутся ОДИН РАЗ против интерфейса `UIDraw`, а не против
 *          ImGui напрямую. Это даёт три вещи:
 *
 *          1. **Headless-тесты.** `TextUIDraw` рисует панель в строку, поэтому
 *             тест может утверждать «в Hierarchy видны три сущности» без окна,
 *             GPU и самой библиотеки ImGui (которая подключается только при
 *             `LV_WITH_IMGUI`).
 *          2. **Единый контракт дерева.** Панель не знает, рисуют её в окно, в
 *             строку или в буфер для снапшота.
 *          3. **Замену бэкенда.** ImGui-реализация лежит в
 *             `ImGuiBackend.cpp` и компилируется только с `LV_WITH_IMGUI`.
 *
 *          Модель намеренно узкая: ровно те примитивы, которые реально нужны
 *             панелям движка (метки, кнопки, поля, списки, деревья, gizmo-оверлей).
 */
#pragma once

#include "../core/Math.h"
#include "../ecs/Entity.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lv::editor {

/// Идентификатор виджета внутри кадра (для состояния «открыт/активен»).
using WidgetId = std::uint32_t;

/**
 * @brief Интерфейс рисования одного кадра панели.
 *
 * Все методы возвращают `true`, если виджет «сработал» в этом кадре
 * (кнопка нажата, значение изменено, узел дерева раскрыт). Это стандартная
 * immediate-mode семантика, поэтому код панели одинаково работает и с ImGui,
 * и с текстовым рендерером.
 */
class UIDraw {
public:
    virtual ~UIDraw() = default;

    // -- Структура ----------------------------------------------------------
    virtual void beginPanel(const std::string& title) = 0;
    virtual void endPanel() = 0;
    virtual void beginGroup(const std::string& id) = 0;
    virtual void endGroup() = 0;
    virtual void separator() = 0;
    virtual void indent() = 0;
    virtual void unindent() = 0;

    // -- Примитивы ----------------------------------------------------------
    virtual void label(const std::string& text) = 0;
    virtual void heading(const std::string& text) = 0;
    virtual bool button(const std::string& label) = 0;
    virtual bool checkbox(const std::string& label, bool& value) = 0;

    /// Поле ввода числа с плавающей точкой; возвращает true при изменении.
    virtual bool dragFloat(const std::string& label, float& value, float step = 0.1f) = 0;
    /// Поле ввода целого.
    virtual bool dragInt(const std::string& label, std::int64_t& value) = 0;
    /// Поле ввода строки (буфер принадлежит вызывающему).
    virtual bool inputText(const std::string& label, std::string& value) = 0;
    /// Выпадающий список; возвращает true при смене индекса.
    virtual bool combo(const std::string& label, int& selected,
                       const std::vector<std::string>& items) = 0;

    // -- Дерево (Hierarchy, Asset Browser, Graph) ---------------------------
    /// Узел дерева. Возвращает true, если узел раскрыт (детей рисовать нужно).
    virtual bool treeNode(const std::string& label, bool defaultOpen = false) = 0;
    virtual void treePop() = 0;
    /// Выделяемый элемент списка (сущность в Hierarchy, ассет в браузере).
    virtual bool selectable(const std::string& label, bool selected) = 0;

    // -- Графика (Viewport) -------------------------------------------------
    /// Запрос на отрисовку gizmo: линия в мировых координатах.
    virtual void gizmoLine(const Vec3& a, const Vec3& b, std::uint32_t rgba) = 0;
    /// Запрос на отрисовку рамки (AABB) — подсветка выбранной сущности.
    virtual void gizmoBox(const Vec3& center, const Vec3& halfExtents, std::uint32_t rgba) = 0;

    /// Текущий кадр (для текстового рендерера — накопленный текст).
    [[nodiscard]] virtual std::string snapshot() const = 0;
};

/**
 * @brief Панель редактора.
 *
 * Панель владеет своим состоянием (фильтры, раскрытые узлы, история консоли)
 * и рисует себя в предоставленный `UIDraw` ровно один раз за кадр.
 */
class Panel {
public:
    virtual ~Panel() = default;

    /// Уникальное имя (используется как id окна и ключ раскладки).
    [[nodiscard]] virtual const char* name() const = 0;

    /// Рисование кадра. Вызывается редактором после `beginFrame`.
    virtual void draw(UIDraw& ui) = 0;

    /// Панель включена в раскладку (снимается галочкой в меню Window).
    bool visible = true;
};

/**
 * @brief Текстовый рендерер UIDraw: рисует кадр в строку.
 *
 * Нужен для headless-тестов и для консольного режима редактора
 * (`limvine-editor --dump-panels`): позволяет утверждать СОДЕРЖИМОЕ панелей
 * без окна и без ImGui. Виджеты, требующие пользовательского ввода, в этом
 * режиме всегда возвращают false (никто не нажимал).
 */
class TextUIDraw final : public UIDraw {
public:
    void beginPanel(const std::string& title) override;
    void endPanel() override;
    void beginGroup(const std::string& id) override;
    void endGroup() override;
    void separator() override;
    void indent() override;
    void unindent() override;

    void label(const std::string& text) override;
    void heading(const std::string& text) override;
    bool button(const std::string& label) override;
    bool checkbox(const std::string& label, bool& value) override;
    bool dragFloat(const std::string& label, float& value, float step) override;
    bool dragInt(const std::string& label, std::int64_t& value) override;
    bool inputText(const std::string& label, std::string& value) override;
    bool combo(const std::string& label, int& selected, const std::vector<std::string>& items) override;

    bool treeNode(const std::string& label, bool defaultOpen) override;
    void treePop() override;
    bool selectable(const std::string& label, bool selected) override;

    void gizmoLine(const Vec3& a, const Vec3& b, std::uint32_t rgba) override;
    void gizmoBox(const Vec3& center, const Vec3& halfExtents, std::uint32_t rgba) override;

    [[nodiscard]] std::string snapshot() const override { return out_; }

    /// Сбросить накопленный текст (начало нового кадра).
    void clear() { out_.clear(); depth_ = 0; }

    /// Число зарегистрированных gizmo-примитивов (проверяют тесты Viewport).
    [[nodiscard]] int gizmoCount() const noexcept { return gizmos_; }

private:
    void pad();

    std::string out_;
    int depth_ = 0;
    int gizmos_ = 0;
};

} // namespace lv::editor
