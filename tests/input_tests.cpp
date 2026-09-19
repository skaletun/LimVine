/**
 * @file    input_tests.cpp
 * @brief   Проверки Input Mapping Contexts.
 */
#include "input/Input.h"

#include <cmath>
#include <cstdio>

using namespace lv;
using namespace lv::input;

namespace {
int g_passed = 0, g_failed = 0;
void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}
constexpr KeyCode kA = 65, kD = 68, kW = 87, kS = 83, kSpace = 32, kEnter = 13, kEsc = 27;
} // namespace

int main() {
    InputSystem in;
    auto& gameplay = in.createContext("Gameplay", 0);
    gameplay.mapKeyAxis("MoveX", kA, -1.f);
    gameplay.mapKeyAxis("MoveX", kD, +1.f);
    gameplay.mapKeyAxis("MoveY", kW, +1.f);
    gameplay.mapKeyAxis("MoveY", kS, -1.f);
    gameplay.mapKey("Jump", kSpace);
    gameplay.mapMouseButton("Fire", 0);

    auto& menu = in.createContext("Menu", 10);   // выше по приоритету
    menu.mapKey("Confirm", kEnter);
    menu.mapKey("Cancel", kEsc);
    menu.mapKey("Jump", kEnter);                // перекрывает Space

    std::printf("\n== Input: axes ==\n");
    in.pushContext("Gameplay");
    in.beginFrame();
    in.onKey(kD, true);
    check(in.axis("MoveX") == 1.0f, "D maps to MoveX +1");
    in.onKey(kA, true);
    check(in.axis("MoveX") == 0.0f, "A+D cancel out");
    in.onKey(kA, false);
    in.onKey(kW, true);
    // D и W нажаты одновременно => диагональ, нормированная на единичную длину.
    const Vec2 diag = in.axis2D("MoveX", "MoveY");
    check(std::fabs(length({diag.x, diag.y, 0}) - 1.0f) < 1e-4f, "axis2D normalises the diagonal");
    in.onKey(kD, false);
    const Vec2 v = in.axis2D("MoveX", "MoveY");
    check(v.y == 1.0f && v.x == 0.0f, "axis2D reads both axes");

    std::printf("\n== Input: edge detection ==\n");
    in.beginFrame();
    in.onKey(kSpace, true);
    check(in.pressed("Jump"), "first frame of a key press is Pressed");
    in.beginFrame();
    check(in.held("Jump") && !in.pressed("Jump"), "next frame is Held, not Pressed");
    in.beginFrame();
    in.onKey(kSpace, false);
    check(in.released("Jump"), "key up produces Released exactly once");
    in.beginFrame();
    check(!in.released("Jump") && !in.held("Jump"), "Released does not repeat");

    std::printf("\n== Input: mapping contexts ==\n");
    in.beginFrame();
    in.onKey(kEnter, true);
    check(!in.pressed("Confirm"), "menu action is not visible while only Gameplay is active");
    in.pushContext("Menu");
    in.onKey(kEnter, false);
    in.beginFrame();          // кадр с отпущенным Enter -> prevKeys чистый
    in.onKey(kSpace, false);
    in.onKey(kEnter, true);
    check(in.pressed("Jump"), "Menu context overrides Jump (Enter instead of Space)");
    check(in.pressed("Confirm"), "menu-only action is now available");
    in.popContext("Menu");
    in.beginFrame();
    in.onKey(kEnter, false);
    in.onKey(kSpace, true);
    check(in.pressed("Jump"), "after pop, Gameplay binding is active again");

    std::printf("\n== Input: mouse ==\n");
    in.beginFrame();
    in.onMouseMove(100, 100);   // курсор «появился» в (100,100)
    in.beginFrame();            // сбрасываем накопленную дельту
    in.onMouseMove(120, 90);
    check(in.mouseDelta().x == 20.0f && in.mouseDelta().y == -10.0f, "mouse delta accumulates within a frame");
    in.beginFrame();
    check(in.mouseDelta().x == 0.0f, "mouse delta resets each frame");
    in.onMouseButton(0, true);
    check(in.pressed("Fire"), "mouse button drives an action");

    std::printf("\n== Input: serialization ==\n");
    const std::string json = in.serializeBindings();
    check(json.find("\"Gameplay\"") != std::string::npos, "bindings serialize with context names");
    check(json.find("\"MoveX\"") != std::string::npos, "axis actions are serialized");

    std::printf("\n----------------------------------------\n");
    std::printf("Input self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
