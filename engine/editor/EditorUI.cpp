/**
 * @file    EditorUI.cpp
 * @brief   Текстовый рендерер UIDraw (headless-панели редактора).
 */
#include "EditorUI.h"

#include <cmath>

namespace lv::editor {

void TextUIDraw::pad() {
    for (int i = 0; i < depth_; ++i) out_ += "  ";
}

void TextUIDraw::beginPanel(const std::string& title) {
    out_ += "=== " + title + " ===\n";
    depth_ = 1;
}
void TextUIDraw::endPanel() { depth_ = 0; out_ += "\n"; }

void TextUIDraw::beginGroup(const std::string& id) { pad(); out_ += "[" + id + "]\n"; ++depth_; }
void TextUIDraw::endGroup() { if (depth_ > 1) --depth_; }

void TextUIDraw::separator() { pad(); out_ += "----------------\n"; }
void TextUIDraw::indent() { ++depth_; }
void TextUIDraw::unindent() { if (depth_ > 0) --depth_; }

void TextUIDraw::label(const std::string& text) { pad(); out_ += text + "\n"; }
void TextUIDraw::heading(const std::string& text) { pad(); out_ += "## " + text + "\n"; }

bool TextUIDraw::button(const std::string& label) { pad(); out_ += "[ " + label + " ]\n"; return false; }

bool TextUIDraw::checkbox(const std::string& label, bool& value) {
    pad();
    out_ += std::string("[") + (value ? "x" : " ") + "] " + label + "\n";
    return false;
}

bool TextUIDraw::dragFloat(const std::string& label, float& value, float step) {
    (void)step;
    pad();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%-14s %g", label.c_str(), (double)value);
    out_ += buf;
    out_ += "\n";
    return false;
}

bool TextUIDraw::dragInt(const std::string& label, std::int64_t& value) {
    pad();
    char buf[64];
    std::snprintf(buf, sizeof buf, "%-14s %lld", label.c_str(), (long long)value);
    out_ += buf;
    out_ += "\n";
    return false;
}

bool TextUIDraw::inputText(const std::string& label, std::string& value) {
    pad();
    out_ += label + ": \"" + value + "\"\n";
    return false;
}

bool TextUIDraw::combo(const std::string& label, int& selected, const std::vector<std::string>& items) {
    pad();
    out_ += label + ": <";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out_ += " | ";
        if (static_cast<int>(i) == selected) out_ += "*" + items[i] + "*";
        else out_ += items[i];
    }
    out_ += ">\n";
    return false;
}

bool TextUIDraw::treeNode(const std::string& label, bool defaultOpen) {
    pad();
    out_ += std::string(defaultOpen ? "- " : "+ ") + label + "\n";
    if (defaultOpen) ++depth_;
    return defaultOpen;
}
void TextUIDraw::treePop() { if (depth_ > 1) --depth_; }

bool TextUIDraw::selectable(const std::string& label, bool selected) {
    pad();
    out_ += std::string(selected ? "> " : "  ") + label + "\n";
    return false;
}

void TextUIDraw::gizmoLine(const Vec3& a, const Vec3& b, std::uint32_t rgba) {
    (void)rgba;
    ++gizmos_;
    pad();
    char buf[128];
    std::snprintf(buf, sizeof buf, "gizmo line (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                  (double)a.x, (double)a.y, (double)a.z, (double)b.x, (double)b.y, (double)b.z);
    out_ += buf;
    out_ += "\n";
}

void TextUIDraw::gizmoBox(const Vec3& center, const Vec3& half, std::uint32_t rgba) {
    (void)rgba;
    ++gizmos_;
    pad();
    char buf[128];
    std::snprintf(buf, sizeof buf, "gizmo box c=(%.2f,%.2f,%.2f) h=(%.2f,%.2f,%.2f)",
                  (double)center.x, (double)center.y, (double)center.z,
                  (double)half.x, (double)half.y, (double)half.z);
    out_ += buf;
    out_ += "\n";
}

} // namespace lv::editor
