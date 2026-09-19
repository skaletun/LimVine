/**
 * @file    Input.cpp
 * @brief   Реализация Input Mapping Contexts.
 */
#include "Input.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace lv::input {

// ---------------------------------------------------------------------------
//  MappingContext
// ---------------------------------------------------------------------------
void MappingContext::mapKey(std::string_view action, KeyCode key, std::uint32_t modifiers) {
    Binding b; b.kind = BindingKind::Key; b.key = key; b.modifiers = modifiers;
    map_[std::string(action)].push_back(b);
}
void MappingContext::mapMouseButton(std::string_view action, std::uint8_t button) {
    Binding b; b.kind = BindingKind::MouseButtonBind; b.button = button;
    map_[std::string(action)].push_back(b);
}
void MappingContext::mapGamepadButton(std::string_view action, std::uint8_t button) {
    Binding b; b.kind = BindingKind::GamepadButtonBind; b.button = button;
    map_[std::string(action)].push_back(b);
}
void MappingContext::mapAxis(std::string_view action, BindingKind kind, std::uint8_t axis, Real scale) {
    Binding b; b.kind = kind; b.axis = axis; b.scale = scale;
    map_[std::string(action)].push_back(b);
}
void MappingContext::mapKeyAxis(std::string_view action, KeyCode key, Real scale, std::uint32_t modifiers) {
    Binding b; b.kind = BindingKind::Key; b.key = key; b.scale = scale; b.modifiers = modifiers;
    map_[std::string(action)].push_back(b);
}

void MappingContext::unmap(std::string_view action) { map_.erase(std::string(action)); }

const std::vector<Binding>* MappingContext::bindings(std::string_view action) const {
    auto it = map_.find(std::string(action));
    return it == map_.end() ? nullptr : &it->second;
}

std::vector<std::string> MappingContext::actions() const {
    std::vector<std::string> out;
    out.reserve(map_.size());
    for (const auto& [k, v] : map_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
//  InputSystem
// ---------------------------------------------------------------------------
InputSystem::InputSystem() {
    state_.keys.assign(512, 0);
    state_.prevKeys.assign(512, 0);
    state_.mouseButtons.assign(8, 0);
    state_.prevMouseButtons.assign(8, 0);
    state_.gamepadAxes.assign(8, 0.f);
    state_.gamepadButtons.assign(16, 0);
    state_.prevGamepadButtons.assign(16, 0);
}

MappingContext& InputSystem::createContext(std::string name, std::int32_t priority) {
    for (auto& c : contexts_)
        if (c->name() == name) return *c;
    contexts_.push_back(std::make_unique<MappingContext>(std::move(name), priority));
    return *contexts_.back();
}

MappingContext* InputSystem::findContext(std::string_view name) noexcept {
    for (auto& c : contexts_)
        if (c->name() == name) return c.get();
    return nullptr;
}

void InputSystem::pushContext(std::string_view name) {
    for (auto& c : contexts_) {
        if (c->name() != name) continue;
        if (std::find(activeStack_.begin(), activeStack_.end(), c.get()) == activeStack_.end())
            activeStack_.push_back(c.get());
        break;
    }
    // Сортируем по приоритету: активнее тот, у кого приоритет выше.
    std::stable_sort(activeStack_.begin(), activeStack_.end(),
                     [](const MappingContext* a, const MappingContext* b) { return a->priority() > b->priority(); });
    cacheValid_ = false;
}

void InputSystem::popContext(std::string_view name) {
    activeStack_.erase(std::remove_if(activeStack_.begin(), activeStack_.end(),
                                      [&](MappingContext* c) { return c->name() == name; }),
                       activeStack_.end());
    cacheValid_ = false;
}

void InputSystem::clearContexts() { activeStack_.clear(); cacheValid_ = false; }

void InputSystem::beginFrame() {
    state_.prevKeys = state_.keys;
    state_.prevMouseButtons = state_.mouseButtons;
    state_.prevGamepadButtons = state_.gamepadButtons;
    state_.mouseDX = state_.mouseDY = 0;
    state_.wheel = 0;
    cacheValid_ = false;
}

void InputSystem::endFrame() { cacheValid_ = false; }

void InputSystem::onKey(KeyCode key, bool down) {
    if (key < state_.keys.size()) { state_.keys[key] = down ? 1 : 0; cacheValid_ = false; }
}
void InputSystem::onMouseButton(std::uint8_t button, bool down) {
    if (button < state_.mouseButtons.size()) { state_.mouseButtons[button] = down ? 1 : 0; cacheValid_ = false; }
}
void InputSystem::onMouseMove(Real x, Real y) {
    state_.mouseDX += x - state_.mouseX;
    state_.mouseDY += y - state_.mouseY;
    state_.mouseX = x;
    state_.mouseY = y;
    cacheValid_ = false;
}
void InputSystem::onMouseWheel(Real delta) { state_.wheel += delta; }
void InputSystem::onGamepadAxis(std::uint8_t axis, Real value) {
    if (axis < state_.gamepadAxes.size()) state_.gamepadAxes[axis] = value;
}
void InputSystem::onGamepadButton(std::uint8_t button, bool down) {
    if (button < state_.gamepadButtons.size()) { state_.gamepadButtons[button] = down ? 1 : 0; cacheValid_ = false; }
}

const std::vector<Binding>* InputSystem::findActionBindings(std::string_view action) const {
    // Обход стека контекстов по приоритету: первое совпадение побеждает.
    for (const MappingContext* c : activeStack_)
        if (const std::vector<Binding>* b = c->bindings(action)) return b;
    return nullptr;
}

Real InputSystem::axisValue(const Binding& b) const {
    Real raw = 0.f;
    switch (b.kind) {
        case BindingKind::Key:
            // ВАЖНО: для клавишной привязки код хранится в поле `key`, а не `axis`.
            raw = (b.key < state_.keys.size() && state_.keys[b.key]) ? 1.f : 0.f;
            break;
        case BindingKind::MouseButtonBind:
            raw = (b.button < state_.mouseButtons.size() && state_.mouseButtons[b.button]) ? 1.f : 0.f;
            break;
        case BindingKind::GamepadButtonBind:
            raw = (b.button < state_.gamepadButtons.size() && state_.gamepadButtons[b.button]) ? 1.f : 0.f;
            break;
        case BindingKind::GamepadAxis:
            raw = b.axis < state_.gamepadAxes.size() ? state_.gamepadAxes[b.axis] : 0.f;
            if (std::fabs(raw) < deadZone_) raw = 0.f;
            break;
        case BindingKind::MouseAxis:
            raw = b.axis == 0 ? state_.mouseDX : state_.mouseDY;
            break;
        default: break;
    }
    return raw * b.scale;
}

void InputSystem::rebuildCache() const {
    actionCache_.clear();
    axisCache_.clear();
    for (const MappingContext* c : activeStack_) {
        for (const std::string& action : c->actions()) {
            if (actionCache_.count(action)) continue;   // контекст с большим приоритетом уже ответил
            const std::vector<Binding>* binds = c->bindings(action);
            if (!binds || binds->empty()) continue;

            Real axisSum = 0.f;
            bool anyDown = false, anyPrev = false;
            for (const Binding& b : *binds) {
                const Real v = axisValue(b);
                axisSum += v;
                bool down = false, prev = false;
                switch (b.kind) {
                    case BindingKind::Key:
                        down = b.key < state_.keys.size() && state_.keys[b.key];
                        prev = b.key < state_.prevKeys.size() && state_.prevKeys[b.key];
                        break;
                    case BindingKind::MouseButtonBind:
                        down = b.button < state_.mouseButtons.size() && state_.mouseButtons[b.button];
                        prev = b.button < state_.prevMouseButtons.size() && state_.prevMouseButtons[b.button];
                        break;
                    case BindingKind::GamepadButtonBind:
                        down = b.button < state_.gamepadButtons.size() && state_.gamepadButtons[b.button];
                        prev = b.button < state_.prevGamepadButtons.size() && state_.prevGamepadButtons[b.button];
                        break;
                    case BindingKind::GamepadAxis:
                        down = std::fabs(v) > deadZone_;
                        prev = down;
                        break;
                    default: break;
                }
                if (b.modifiers && (state_.modifiers & b.modifiers) != b.modifiers) { down = false; prev = false; }
                anyDown = anyDown || down;
                anyPrev = anyPrev || prev;
            }
            axisCache_[action] = axisSum;
            actionCache_[action] = anyDown && !anyPrev ? ActionState::Pressed
                                 : anyDown && anyPrev  ? ActionState::Held
                                 : !anyDown && anyPrev ? ActionState::Released
                                                       : ActionState::None;
        }
    }
    cacheValid_ = true;
}

ActionState InputSystem::actionState(std::string_view action) const {
    if (!cacheValid_) rebuildCache();
    auto it = actionCache_.find(std::string(action));
    return it == actionCache_.end() ? ActionState::None : it->second;
}

bool InputSystem::pressed(std::string_view a) const  { return actionState(a) == ActionState::Pressed; }
bool InputSystem::held(std::string_view a) const     { const ActionState s = actionState(a); return s == ActionState::Held || s == ActionState::Pressed; }
bool InputSystem::released(std::string_view a) const { return actionState(a) == ActionState::Released; }

Real InputSystem::axis(std::string_view action) const {
    if (!cacheValid_) rebuildCache();
    auto it = axisCache_.find(std::string(action));
    return it == axisCache_.end() ? 0.f : it->second;
}

Vec2 InputSystem::axis2D(std::string_view actionX, std::string_view actionY) const {
    Vec2 v{axis(actionX), axis(actionY)};
    const Real len = length({v.x, v.y, 0});
    if (len > 1.0f) { v.x /= len; v.y /= len; }   // нормализация диагонали геймпада
    return v;
}

bool InputSystem::anyKeyPressed() const noexcept {
    for (std::uint8_t k : state_.keys) if (k) return true;
    return false;
}

// ---------------------------------------------------------------------------
//  Сериализация (мини-JSON, без внешних зависимостей)
// ---------------------------------------------------------------------------
std::string InputSystem::serializeBindings() const {
    std::ostringstream os;
    os << "{\n";
    bool firstCtx = true;
    for (const auto& c : contexts_) {
        if (!firstCtx) os << ",\n";
        firstCtx = false;
        os << "  \"" << c->name() << "\": {\n";
        const auto actions = c->actions();
        for (std::size_t i = 0; i < actions.size(); ++i) {
            const std::vector<Binding>* binds = c->bindings(actions[i]);
            os << "    \"" << actions[i] << "\": [";
            for (std::size_t j = 0; binds && j < binds->size(); ++j) {
                if (j) os << ", ";
                os << "{\"kind\":" << static_cast<int>((*binds)[j].kind)
                   << ",\"key\":" << (*binds)[j].key
                   << ",\"button\":" << static_cast<int>((*binds)[j].button)
                   << ",\"axis\":" << static_cast<int>((*binds)[j].axis)
                   << ",\"scale\":" << (*binds)[j].scale
                   << ",\"mods\":" << (*binds)[j].modifiers << "}";
            }
            os << "]" << (i + 1 < actions.size() ? "," : "") << "\n";
        }
        os << "  }";
    }
    os << "\n}\n";
    return os.str();
}

bool InputSystem::applyBindings(const std::string&) {
    // Парсер JSON-конфига привязок подключается в модуле asset/JsonReader.
    // Здесь оставлена точка расширения: редактор вызывает её после правки.
    cacheValid_ = false;
    return true;
}

} // namespace lv::input
