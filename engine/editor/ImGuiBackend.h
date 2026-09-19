/**
 * @file    ImGuiBackend.h
 * @brief   Бэкенд UIDraw на Dear ImGui.
 * @ingroup Editor
 *
 * Файл компилируется ТОЛЬКО при `LV_WITH_IMGUI=1`: без этого макроса он
 * представляет собой пустую единицу трансляции, поэтому дерево движка
 * собирается без единой внешней зависимости (проверяется CI).
 *
 * Панели редактора пишутся против `UIDraw` (см. EditorUI.h), поэтому переход
 * «текстовый снапшот ↔ окно ImGui» не требует ни строчки дублирующей логики:
 * тот же `HierarchyPanel::draw()` рисует и в строку для тестов, и в окно.
 */
#pragma once

#include "EditorUI.h"

namespace lv::editor {

#ifdef LV_WITH_IMGUI

/**
 * @brief Рисование панели в окно Dear ImGui.
 *
 * Семантика полностью совпадает с UIDraw: методы возвращают true, если виджет
 * «сработал» в этом кадре. Вложенность (beginPanel/beginGroup/treeNode)
 * отображается на Begin/Child/TreeNode ImGui один-к-одному, поэтому
 * сбалансированность вызовов контролируется самой панелью.
 */
class ImGuiUIDraw final : public UIDraw {
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

    [[nodiscard]] std::string snapshot() const override { return {}; }

    /// Gizmo-примитивы, накопленные за кадр: Viewport отдаёт их рендереру.
    struct Line { Vec3 a, b; std::uint32_t rgba; };
    struct Box  { Vec3 c, h; std::uint32_t rgba; };
    [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }
    [[nodiscard]] const std::vector<Box>&  boxes() const noexcept { return boxes_; }
    void clearGizmos() { lines_.clear(); boxes_.clear(); }

private:
    std::vector<Line> lines_;
    std::vector<Box>  boxes_;
    int openDepth_ = 0;   ///< баланс Begin/End, чтобы не «поехали» окна ImGui
};

#endif // LV_WITH_IMGUI

} // namespace lv::editor
