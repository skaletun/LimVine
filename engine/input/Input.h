/**
 * @file    Input.h
 * @brief   Input Mapping Contexts: действия вместо клавиш.
 * @ingroup Input
 *
 * @details Игровой код никогда не спрашивает «нажата ли E». Он спрашивает
 *          «выполнено ли действие @c Interact». Привязки живут в **контекстах**:
 * @code
 *   Gameplay : Move=WASD, Interact=E, Jump=Space
 *   Vehicle  : Move=W/S, Handbrake=Space       (перекрывает Gameplay)
 *   Menu     : Navigate=Arrows, Confirm=Enter
 * @endcode
 *          Контексы образуют стек: активный перекрывает нижележащие по имени
 *          действия. Это решает классическую проблему «в меню WASD двигает
 *          персонажа» без единого флага в геймплейном коде.
 *
 *          Реbinding сохраняется в JSON и применяется в рантайме, поэтому
 *          настройка управления не требует перезапуска.
 */
#pragma once

#include "../core/Math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lv::input {

/// Абстрактный код устройства (сквозная нумерация, маппинг делает бэкенд GLFW/SDL).
using KeyCode = std::uint32_t;
using MouseButton = std::uint8_t;
using GamepadButton = std::uint8_t;

inline constexpr KeyCode kKeyUnknown = 0;

/// Тип источника привязки.
enum class BindingKind : std::uint8_t { Key, Mouse, MouseButtonBind, GamepadButtonBind, GamepadAxis, MouseAxis };

/**
 * @brief Одна привязка действия.
 */
struct Binding {
    BindingKind kind = BindingKind::Key;
    KeyCode     key = kKeyUnknown;
    std::uint8_t button = 0;
    std::uint8_t axis = 0;
    Real         scale = 1.0f;     ///< Для осей: -1 (стрелка влево) / +1
    std::uint32_t modifiers = 0;   ///< Битовая маска Shift/Ctrl/Alt
    std::string  displayName;      ///< Для экрана настроек
};

/// Состояние действия за кадр.
enum class ActionState : std::uint8_t { None, Pressed, Held, Released };

/**
 * @brief Контекст привязок (набор «действие -> привязки»).
 */
class MappingContext {
public:
    explicit MappingContext(std::string name, std::int32_t priority = 0)
        : name_(std::move(name)), priority_(priority) {}

    /// Добавить привязку. @p action — имя действия ("Move", "Jump", ...).
    void mapKey(std::string_view action, KeyCode key, std::uint32_t modifiers = 0);
    void mapMouseButton(std::string_view action, std::uint8_t button);
    void mapGamepadButton(std::string_view action, std::uint8_t button);
    /// Ось: @p axisIndex + @p scale (геймпад-стик или дельта мыши).
    void mapAxis(std::string_view action, BindingKind kind, std::uint8_t axis, Real scale);
    /**
     * @brief Клавиша как ось (WASD-движение).
     *
     * Отдельный метод вместо @c mapAxis нужен потому, что у клавишной привязки
     * код лежит в поле @c key, а не @c axis — смешение этих полей приводит к
     * «мёртвым» действиям, которые крайне тяжело отлаживать.
     */
    void mapKeyAxis(std::string_view action, KeyCode key, Real scale, std::uint32_t modifiers = 0);

    void unmap(std::string_view action);
    [[nodiscard]] const std::vector<Binding>* bindings(std::string_view action) const;
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::int32_t priority() const noexcept { return priority_; }
    [[nodiscard]] std::vector<std::string> actions() const;

private:
    std::string name_;
    std::int32_t priority_;
    std::unordered_map<std::string, std::vector<Binding>> map_;
};

/**
 * @brief Состояние устройств за кадр.
 */
struct DeviceState {
    std::vector<std::uint8_t> keys;         ///< 1 = нажата
    std::vector<std::uint8_t> prevKeys;
    std::vector<std::uint8_t> mouseButtons;
    std::vector<std::uint8_t> prevMouseButtons;
    Real mouseX = 0, mouseY = 0;
    Real mouseDX = 0, mouseDY = 0;          ///< Дельта за кадр (для FPS-камеры).
    Real wheel = 0;
    std::vector<Real> gamepadAxes;
    std::vector<std::uint8_t> gamepadButtons;
    std::vector<std::uint8_t> prevGamepadButtons;
    bool  gamepadConnected = false;
    std::uint32_t modifiers = 0;
};

/**
 * @brief Подсистема ввода.
 */
class InputSystem {
public:
    InputSystem();

    /// Создать и зарегистрировать контекст.
    MappingContext& createContext(std::string name, std::int32_t priority = 0);
    /// Поместить контекст в активный стек (чем позже вызван, тем выше приоритет).
    void pushContext(std::string_view name);
    void popContext(std::string_view name);
    void clearContexts();

    /// Начало кадра: сохранить предыдущее состояние.
    void beginFrame();
    /// Конец кадра: вычислить состояния действий.
    void endFrame();

    /// События от оконной системы (GLFW-колбэки).
    void onKey(KeyCode key, bool down);
    void onMouseButton(std::uint8_t button, bool down);
    void onMouseMove(Real x, Real y);
    void onMouseWheel(Real delta);
    void onGamepadAxis(std::uint8_t axis, Real value);
    void onGamepadButton(std::uint8_t button, bool down);

    // -- Запросы из игрового кода и LV Script --------------------------------
    /// Текущее состояние действия.
    [[nodiscard]] ActionState actionState(std::string_view action) const;
    [[nodiscard]] bool pressed(std::string_view action) const;
    [[nodiscard]] bool held(std::string_view action) const;
    [[nodiscard]] bool released(std::string_view action) const;
    /// Аналоговое значение действия (ось или сумма осей), [-1, 1] или больше.
    [[nodiscard]] Real axis(std::string_view action) const;
    /// Вектор движения из двух осевых действий (MoveX/MoveY).
    [[nodiscard]] Vec2 axis2D(std::string_view actionX, std::string_view actionY) const;

    [[nodiscard]] Vec2 mousePosition() const noexcept { return {state_.mouseX, state_.mouseY}; }
    [[nodiscard]] Vec2 mouseDelta() const noexcept { return {state_.mouseDX, state_.mouseDY}; }
    [[nodiscard]] Real scrollDelta() const noexcept { return state_.wheel; }
    [[nodiscard]] bool anyKeyPressed() const noexcept;

    /// «Мёртвая зона» осей геймпада.
    void setDeadZone(Real dz) noexcept { deadZone_ = dz; }

    /// Сериализация привязок (экран настроек / конфиг игрока).
    [[nodiscard]] std::string serializeBindings() const;
    bool applyBindings(const std::string& json);

    /**
     * @brief Найти контекст по имени (для добавления привязок из скриптов).
     *
     * Возвращает nullptr, если контекста нет: LV Script-биндинг
     * `mapKey(contextName, action, keyCode)` использует это, чтобы шаблон мог
     * описывать свою раскладку рядом со своей логикой, а не в C++-коде движка.
     */
    [[nodiscard]] MappingContext* findContext(std::string_view name) noexcept;

    [[nodiscard]] DeviceState& state() noexcept { return state_; }
    [[nodiscard]] const std::vector<std::unique_ptr<MappingContext>>& contexts() const noexcept { return contexts_; }

private:
    [[nodiscard]] const std::vector<Binding>* findActionBindings(std::string_view action) const;
    Real axisValue(const Binding& b) const;

    std::vector<std::unique_ptr<MappingContext>> contexts_;
    std::vector<MappingContext*> activeStack_;
    DeviceState state_;
    Real deadZone_ = 0.15f;
    mutable std::unordered_map<std::string, ActionState> actionCache_;
    mutable std::unordered_map<std::string, Real> axisCache_;
    mutable bool cacheValid_ = false;
    void rebuildCache() const;
};

} // namespace lv::input
