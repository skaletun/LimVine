/**
 * @file    ImGuiBackend.cpp
 * @brief   Реализация UIDraw на Dear ImGui (только при LV_WITH_IMGUI).
 */
#include "ImGuiBackend.h"

#ifdef LV_WITH_IMGUI
#include <imgui.h>

#include <cstdio>

namespace lv::editor {

namespace {
/// ImGui требует уникальные id у одноимённых виджетов в одном окне.
std::string uid(const std::string& label) {
    static int counter = 0;
    return label + "###lv" + std::to_string(++counter);
}
} // namespace

void ImGuiUIDraw::beginPanel(const std::string& title) {
    if (ImGui::Begin(title.c_str())) { ++openDepth_; } else { ImGui::End(); openDepth_ = -1; }
}
void ImGuiUIDraw::endPanel() {
    if (openDepth_ > 0) ImGui::End();
    openDepth_ = 0;
}

void ImGuiUIDraw::beginGroup(const std::string& id) { ImGui::BeginGroup(); (void)id; }
void ImGuiUIDraw::endGroup() { ImGui::EndGroup(); }
void ImGuiUIDraw::separator() { ImGui::Separator(); }
void ImGuiUIDraw::indent() { ImGui::Indent(); }
void ImGuiUIDraw::unindent() { ImGui::Unindent(); }

void ImGuiUIDraw::label(const std::string& text) { ImGui::TextUnformatted(text.c_str()); }
void ImGuiUIDraw::heading(const std::string& text) {
    ImGui::SeparatorText(text.c_str());
}
bool ImGuiUIDraw::button(const std::string& label) { return ImGui::Button(uid(label).c_str()); }
bool ImGuiUIDraw::checkbox(const std::string& label, bool& value) {
    return ImGui::Checkbox(uid(label).c_str(), &value);
}
bool ImGuiUIDraw::dragFloat(const std::string& label, float& value, float step) {
    return ImGui::DragFloat(uid(label).c_str(), &value, step);
}
bool ImGuiUIDraw::dragInt(const std::string& label, std::int64_t& value) {
    int v = static_cast<int>(value);
    if (!ImGui::DragInt(uid(label).c_str(), &v)) return false;
    value = v;
    return true;
}
bool ImGuiUIDraw::inputText(const std::string& label, std::string& value) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    if (!ImGui::InputText(uid(label).c_str(), buf, sizeof buf,
                          ImGuiInputTextFlags_EnterReturnsTrue)) return false;
    value = buf;
    return true;
}
bool ImGuiUIDraw::combo(const std::string& label, int& selected, const std::vector<std::string>& items) {
    std::string preview = (selected >= 0 && selected < static_cast<int>(items.size()))
                              ? items[static_cast<std::size_t>(selected)] : std::string();
    if (!ImGui::BeginCombo(uid(label).c_str(), preview.c_str())) return false;
    bool changed = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (ImGui::Selectable(items[i].c_str(), static_cast<int>(i) == selected)) {
            selected = static_cast<int>(i);
            changed = true;
        }
    }
    ImGui::EndCombo();
    return changed;
}

bool ImGuiUIDraw::treeNode(const std::string& label, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    return ImGui::TreeNodeEx(uid(label).c_str(), flags);
}
void ImGuiUIDraw::treePop() { ImGui::TreePop(); }
bool ImGuiUIDraw::selectable(const std::string& label, bool selected) {
    return ImGui::Selectable(uid(label).c_str(), selected);
}

void ImGuiUIDraw::gizmoLine(const Vec3& a, const Vec3& b, std::uint32_t rgba) {
    lines_.push_back(Line{a, b, rgba});
}
void ImGuiUIDraw::gizmoBox(const Vec3& center, const Vec3& halfExtents, std::uint32_t rgba) {
    boxes_.push_back(Box{center, halfExtents, rgba});
}

} // namespace lv::editor

#else  // !LV_WITH_IMGUI

namespace lv::editor {
// Пустая единица трансляции: без ImGui бэкенд не собирается, а панели
// редактора остаются доступны через TextUIDraw (headless-тесты, --dump-panels).
} // namespace lv::editor

#endif // LV_WITH_IMGUI
