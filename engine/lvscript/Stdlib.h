/**
 * @file    Stdlib.h
 * @brief   Стандартная библиотека LV Script (нативные функции и методы).
 * @ingroup LVScript
 *
 * Состав:
 *  - глобальные функции: print, assert, type, len, str, num, range, min, max, ...
 *  - методы встроенных типов: Array, String, Map, Result, Coroutine
 *  - модуль `math` (Vec2/Vec3-операции, тригонометрия, случайные числа)
 *  - модуль `coroutine` (create/start/status)
 *
 * Всё, что требует знаний о движке (ECS, рендер, физика), регистрируется
 * отдельно в @c engine/scripting/EngineBindings.h — стандартная библиотека
 * намеренно не знает о движке, чтобы её можно было тестировать изолированно.
 */
#pragma once

#include "VM.h"

namespace lv {

/// Установить в VM полную стандартную библиотеку.
void installStdlib(VM& vm);

/// Только математический модуль (используется тестами).
void installMathModule(VM& vm);

/// Вспомогательные функции для биндингов движка.
namespace util {
    /// Аргумент с проверкой типа; при несоответствии генерирует рантайм-ошибку.
    [[nodiscard]] double requireNumber(VM& vm, const Value& v, std::string_view argName);
    [[nodiscard]] ObjArray* requireArray(VM& vm, const Value& v, std::string_view argName);
    [[nodiscard]] ObjMap* requireMap(VM& vm, const Value& v, std::string_view argName);
    [[nodiscard]] std::string_view requireString(VM& vm, const Value& v, std::string_view argName);
    /// Вызвать скриптовое значение (лямбду) из нативного кода, не разрушая текущий кадр.
    [[nodiscard]] Value callFunction(VM& vm, Value fn, std::initializer_list<Value> args);
    /// Собрать модуль (map) из набора именованных нативных функций.
    Value makeModule(VM& vm, std::string_view moduleName,
                     std::initializer_list<std::pair<std::string_view, Value>> members);
} // namespace util

} // namespace lv
