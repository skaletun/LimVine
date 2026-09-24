/**
 * @file    Stdlib.cpp
 * @brief   Реализация стандартной библиотеки LV Script.
 */
#include "Stdlib.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <numeric>

namespace lv {

// ---------------------------------------------------------------------------
//  Вспомогательные
// ---------------------------------------------------------------------------
namespace util {

double requireNumber(VM& vm, const Value& v, std::string_view argName) {
    if (!v.isNumber()) {
        vm.runtimeErrorPublic(format("argument '{}' must be a number, got {}", argName, v.typeName()));
        return 0.0;
    }
    return v.asNumber();
}

ObjArray* requireArray(VM& vm, const Value& v, std::string_view argName) {
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Array) {
        vm.runtimeErrorPublic(format("argument '{}' must be an Array, got {}", argName, v.typeName()));
        return nullptr;
    }
    return v.as<ObjArray>();
}

ObjMap* requireMap(VM& vm, const Value& v, std::string_view argName) {
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::Map) {
        vm.runtimeErrorPublic(format("argument '{}' must be a Map, got {}", argName, v.typeName()));
        return nullptr;
    }
    return v.as<ObjMap>();
}

std::string_view requireString(VM& vm, const Value& v, std::string_view argName) {
    if (!v.isObject() || v.asObject()->type != ObjHeader::Type::String) {
        vm.runtimeErrorPublic(format("argument '{}' must be a String, got {}", argName, v.typeName()));
        return {};
    }
    return v.as<ObjString>()->view();
}

Value callFunction(VM& vm, Value fn, std::initializer_list<Value> args) {
    std::vector<Value> vec(args);
    Value out = Value::nil();
    vm.callFunction(fn, vec, &out);
    return out;
}

Value makeModule(VM& vm, std::string_view moduleName,
                 std::initializer_list<std::pair<std::string_view, Value>> members) {
    Value mod = vm.makeMap();
    auto* m = mod.as<ObjMap>();
    // Модуль стандартной библиотеки живёт всю сессию, поэтому закрепляется.
    //
    // Без pin он оставался достижим только через таблицу модулей VM; в
    // инкрементальном режиме сборка, начавшаяся между makeMap() и setModule(),
    // успевала освободить ещё «ничей» объект, и скрипт падал с
    // «undefined name 'math'». Закрепление снимает эту гонку и заодно избавляет
    // GC от обхода неизменяемых таблиц stdlib на каждом цикле.
    GC::pin(m);
    for (const auto& [k, v] : members) m->set(vm.gc(), vm.internString(k), v);
    vm.setModule(moduleName, mod);
    return mod;
}

} // namespace util

namespace {

/// Создать нативную функцию как значение (для модулей).
Value native(VM& vm, std::string_view name, std::uint32_t arity, ObjNativeFn::Fn fn,
             std::uint32_t cap = Cap_Math, bool yielding = false) {
    auto* n = vm.gc().allocate<ObjNativeFn>(sizeof(ObjNativeFn), ObjHeader(ObjHeader::Type::NativeFn, sizeof(ObjNativeFn)));
    n->name = vm.internRaw(name);
    n->arity = arity;
    n->fn = std::move(fn);
    n->requiredCapability = cap;
    n->yielding = yielding;
    GC::pin(n);
    return Value::object(n);
}

#define ARGCHECK(n) ARGCHECK_IN(args, n)

/// Проверка числа аргументов нативной функции.
/// @param span Имя локального span'а аргументов (`args` или `a`).
#define ARGCHECK_IN(span, n)                                                   \
    if ((span).size() < static_cast<std::size_t>(n)) {                          \
        v.runtimeErrorPublic("not enough arguments");                           \
        return Value::nil();                                                    \
    }

#define ARGCHECK(n) ARGCHECK_IN(args, n)

// ---------------------------------------------------------------------------
//  Глобальные функции
// ---------------------------------------------------------------------------
void registerGlobals(VM& vm) {
    vm.registerNative("print", 0xFFFFFFFF, [](VM& v, std::span<const Value> args) -> Value {
        std::string out;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i) out += "\t";
            out += args[i].toString();
        }
        v.logLine(std::move(out));
        return Value::nil();
    }, Cap_Debug);

    vm.registerNative("assert", 0xFFFFFFFF, [](VM& v, std::span<const Value> args) -> Value {
        if (args.empty() || !args[0].truthy()) {
            v.runtimeErrorPublic(args.size() > 1 ? "assertion failed: " + args[1].toString()
                                                 : "assertion failed");
        }
        return args.empty() ? Value::nil() : args[0];
    });

    vm.registerNative("type", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        return v.internString(args[0].typeName());
    });

    vm.registerNative("len", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        const Value a = args[0];
        if (!a.isObject()) { v.runtimeErrorPublic("len() of a non-object"); return Value::nil(); }
        switch (a.asObject()->type) {
            case ObjHeader::Type::Array:  return Value::integer(a.as<ObjArray>()->count);
            case ObjHeader::Type::String: return Value::integer(a.as<ObjString>()->length);
            case ObjHeader::Type::Map:    return Value::integer(a.as<ObjMap>()->count);
            default: v.runtimeErrorPublic(format("len() is not defined for {}", a.asObject()->typeName()));
        }
        return Value::nil();
    });

    vm.registerNative("str", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        return v.internString(args[0].toString());
    });
    vm.registerNative("inspect", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        return v.internString(args[0].inspect());
    });
    vm.registerNative("num", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        if (args[0].isNumber()) return args[0];
        if (args[0].isObject() && args[0].asObject()->type == ObjHeader::Type::String) {
            const auto sv = args[0].as<ObjString>()->view();
            try { return Value::fromNumber(std::stod(std::string(sv))); }
            catch (...) { return Value::nil(); }
        }
        return Value::nil();
    });
    // Приведение к числу. ВАЖНО: Value::asInt()/asNumber() — это сырая
    // переинтерпретация битов NaN-боксинга, поэтому их НЕЛЬЗЯ применять к
    // нечисловым значениям: `int("3")` возвращал мусор вида -9.2e18 (а следом
    // «cannot add Int and String» в stdlib/pathfinding). Сначала значение
    // нормализуется: строка парсится, bool -> 0/1, nil -> 0.
    vm.registerNative("int", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        const Value& a = args[0];
        if (a.isNumber()) return Value::integer(a.asInt());
        if (a.isBool())   return Value::integer(a.asBool() ? 1 : 0);
        if (a.isNil())    return Value::integer(0);
        if (a.isObject() && a.asObject()->type == ObjHeader::Type::String) {
            const auto sv = a.as<ObjString>()->view();
            try { return Value::integer(static_cast<std::int64_t>(std::stod(std::string(sv)))); }
            catch (...) { return Value::integer(0); }
        }
        v.runtimeErrorPublic(format("cannot convert {} to Int", a.typeName()));
        return Value::integer(0);
    });
    vm.registerNative("float", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        const Value& a = args[0];
        if (a.isNumber()) return Value::fromNumber(a.asNumber());
        if (a.isBool())   return Value::fromNumber(a.asBool() ? 1.0 : 0.0);
        if (a.isNil())    return Value::fromNumber(0.0);
        if (a.isObject() && a.asObject()->type == ObjHeader::Type::String) {
            const auto sv = a.as<ObjString>()->view();
            try { return Value::fromNumber(std::stod(std::string(sv))); }
            catch (...) { return Value::fromNumber(0.0); }
        }
        v.runtimeErrorPublic(format("cannot convert {} to Float", a.typeName()));
        return Value::fromNumber(0.0);
    });

    vm.registerNative("range", 0xFFFFFFFF, [](VM& v, std::span<const Value> args) -> Value {
        std::int64_t from = 0, to = 0, step = 1;
        if (args.size() == 1) { to = args[0].asInt(); }
        else if (args.size() >= 2) { from = args[0].asInt(); to = args[1].asInt(); }
        if (args.size() >= 3) step = args[2].asInt();
        if (step == 0) { v.runtimeErrorPublic("range(): step must not be zero"); return Value::nil(); }
        ObjArray* arr = lv::makeArray(v.gc(), 0);
        if (step > 0) for (std::int64_t i = from; i < to; i += step) { arr->grow(v.gc(), arr->count + 1); arr->items[arr->count++] = Value::integer(i); }
        else          for (std::int64_t i = from; i > to; i += step) { arr->grow(v.gc(), arr->count + 1); arr->items[arr->count++] = Value::integer(i); }
        return Value::object(arr);
    });

    vm.registerNative("min", 0xFFFFFFFF, [](VM& v, std::span<const Value> args) -> Value {
        if (args.empty()) return Value::nil();
        double best = args[0].asNumber();
        for (std::size_t i = 1; i < args.size(); ++i) best = std::min(best, args[i].asNumber());
        return Value::fromNumber(best);
    });
    vm.registerNative("max", 0xFFFFFFFF, [](VM& v, std::span<const Value> args) -> Value {
        if (args.empty()) return Value::nil();
        double best = args[0].asNumber();
        for (std::size_t i = 1; i < args.size(); ++i) best = std::max(best, args[i].asNumber());
        return Value::fromNumber(best);
    });
    vm.registerNative("abs", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1); return Value::fromNumber(std::fabs(args[0].asNumber()));
    });
    vm.registerNative("clamp", 3, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(3);
        return Value::fromNumber(std::clamp(args[0].asNumber(), args[1].asNumber(), args[2].asNumber()));
    });
    vm.registerNative("lerp", 3, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(3);
        const double a = args[0].asNumber(), b = args[1].asNumber(), t = args[2].asNumber();
        return Value::fromNumber(a + (b - a) * t);
    });

    vm.registerNative("error", 1, [](VM& v, std::span<const Value> args) -> Value {
        v.runtimeErrorPublic(args.empty() ? "error()" : args[0].toString());
        return Value::nil();
    });

    vm.registerNative("Result", 2, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(2);
        return v.makeResult(args[0].truthy(), args[0].truthy() ? args[1] : Value::nil(),
                            args[0].truthy() ? Value::nil() : args[1]);
    });

    vm.registerNative("Ok", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1); return v.makeResult(true, args[0], Value::nil());
    });
    vm.registerNative("Err", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1); return v.makeResult(false, Value::nil(), args[0]);
    });

    // ---- Корутины: приостановка ----
    vm.registerNative("wait", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        const double sec = args[0].isNumber() ? args[0].asNumber() : 0.0;
        v.requestSuspend(sec < 0.0 ? 0.0 : sec, nullptr);
        return Value::nil();
    }, Cap_Coroutines, /*yielding=*/true);

    vm.registerNative("waitFrames", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        // Приблизительно: 1 кадр ~= 1/60 с; точная семантика задаётся хостом.
        v.requestSuspend(static_cast<double>(args[0].asInt()) / 60.0, nullptr);
        return Value::nil();
    }, Cap_Coroutines, true);

    vm.registerNative("waitEvent", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        ObjString* ev = v.internRaw(args[0].toString());
        v.requestSuspend(0.0, ev);
        return Value::nil();
    }, Cap_Coroutines, true);

    vm.registerNative("waitUntil", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        // waitUntil(cond: () -> Bool) — опрос каждые 50 мс.
        for (int i = 0; i < 20000; ++i) {
            if (util::callFunction(v, args[0], {}).truthy()) return Value::nil();
            v.requestSuspend(0.05, nullptr);
            return Value::nil();   // планировщик возобновит; проверка повторится при следующем вызове
        }
        return Value::nil();
    }, Cap_Coroutines, true);

    vm.registerNative("coroutine", 1, [](VM& v, std::span<const Value> args) -> Value {
        ARGCHECK(1);
        if (!args[0].isObject() || args[0].asObject()->type != ObjHeader::Type::Closure) {
            v.runtimeErrorPublic("coroutine() expects a function");
            return Value::nil();
        }
        return v.makeCoroutine(args[0].as<ObjClosure>());
    }, Cap_Coroutines);
}

// ---------------------------------------------------------------------------
//  Array
// ---------------------------------------------------------------------------
void registerArrayMethods(VM& vm) {
    vm.registerMethod("Array", "push", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        arr->grow(v.gc(), arr->count + 1);
        arr->items[arr->count++] = a[1];
        return a[0];
    });
    vm.registerMethod("Array", "pop", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr || arr->count == 0) return Value::nil();
        return arr->items[--arr->count];
    });
    vm.registerMethod("Array", "len", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        return Value::integer(arr ? arr->count : 0);
    });
    vm.registerMethod("Array", "count", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        return Value::integer(arr ? arr->count : 0);
    });
    vm.registerMethod("Array", "clear", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        if (auto* arr = util::requireArray(v, a[0], "self")) arr->count = 0;
        return Value::nil();
    });
    vm.registerMethod("Array", "get", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        std::int64_t i = a[1].asInt();
        if (i < 0) i += arr->count;
        if (i < 0 || i >= static_cast<std::int64_t>(arr->count)) return Value::nil();
        return arr->items[i];
    });
    vm.registerMethod("Array", "set", 3, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 3);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        std::int64_t i = a[1].asInt();
        if (i < 0) i += arr->count;
        if (i < 0 || i >= static_cast<std::int64_t>(arr->count)) {
            v.runtimeErrorPublic("Array.set: index out of range");
            return Value::nil();
        }
        arr->items[i] = a[2];
        return a[2];
    });
    vm.registerMethod("Array", "insert", 3, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 3);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        std::int64_t i = std::clamp<std::int64_t>(a[1].asInt(), 0, arr->count);
        arr->grow(v.gc(), arr->count + 1);
        for (std::int64_t k = arr->count; k > i; --k) arr->items[k] = arr->items[k - 1];
        arr->items[i] = a[2];
        ++arr->count;
        return a[0];
    });
    vm.registerMethod("Array", "removeAt", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr || arr->count == 0) return Value::nil();
        std::int64_t i = a[1].asInt();
        if (i < 0) i += arr->count;
        if (i < 0 || i >= static_cast<std::int64_t>(arr->count)) return Value::nil();
        const Value removed = arr->items[i];
        for (std::int64_t k = i; k + 1 < arr->count; ++k) arr->items[k] = arr->items[k + 1];
        --arr->count;
        return removed;
    });
    vm.registerMethod("Array", "contains", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::boolean(false);
        for (std::uint32_t i = 0; i < arr->count; ++i)
            if (valuesEqual(arr->items[i], a[1])) return Value::boolean(true);
        return Value::boolean(false);
    });
    vm.registerMethod("Array", "indexOf", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::integer(-1);
        for (std::uint32_t i = 0; i < arr->count; ++i)
            if (valuesEqual(arr->items[i], a[1])) return Value::integer(i);
        return Value::integer(-1);
    });
    vm.registerMethod("Array", "join", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return v.internString("");
        std::string sep = a.size() > 1 ? a[1].toString() : ",";
        std::string out;
        for (std::uint32_t i = 0; i < arr->count; ++i) {
            if (i) out += sep;
            out += arr->items[i].toString();
        }
        return v.internString(std::move(out));
    });
    vm.registerMethod("Array", "reverse", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        for (std::uint32_t i = 0, j = arr->count; i + 1 < j; ++i, --j) std::swap(arr->items[i], arr->items[j - 1]);
        return a[0];
    });
    vm.registerMethod("Array", "map", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        ObjArray* out = lv::makeArray(v.gc(), arr ? arr->count : 0);
        if (!arr) return Value::object(out);
        for (std::uint32_t i = 0; i < arr->count; ++i)
            out->items[out->count++] = util::callFunction(v, a[1], {arr->items[i], Value::integer(i)});
        return Value::object(out);
    });
    vm.registerMethod("Array", "filter", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        ObjArray* out = lv::makeArray(v.gc(), arr ? arr->count : 0);
        if (!arr) return Value::object(out);
        for (std::uint32_t i = 0; i < arr->count; ++i) {
            Value keep = util::callFunction(v, a[1], {arr->items[i], Value::integer(i)});
            if (keep.truthy()) out->items[out->count++] = arr->items[i];
        }
        return Value::object(out);
    });
    vm.registerMethod("Array", "forEach", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        for (std::uint32_t i = 0; i < arr->count; ++i)
            util::callFunction(v, a[1], {arr->items[i], Value::integer(i)});
        return Value::nil();
    });
    vm.registerMethod("Array", "reduce", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        if (a.size() < 2) { v.runtimeErrorPublic("reduce(fn[, init]) needs at least 2 arguments"); return Value::nil(); }
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        Value acc;
        std::uint32_t start = 0;
        if (a.size() >= 3) { acc = a[2]; }
        else { if (arr->count == 0) { v.runtimeErrorPublic("reduce of empty array without initial value"); return Value::nil(); }
               acc = arr->items[0]; start = 1; }
        for (std::uint32_t i = start; i < arr->count; ++i)
            acc = util::callFunction(v, a[1], {acc, arr->items[i], Value::integer(i)});
        return acc;
    });
    vm.registerMethod("Array", "sort", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        if (a.size() >= 2) {
            std::stable_sort(arr->items, arr->items + arr->count, [&](const Value& x, const Value& y) {
                return util::callFunction(v, a[1], {x, y}).asNumber() < 0;
            });
        } else {
            std::stable_sort(arr->items, arr->items + arr->count, [&](const Value& x, const Value& y) {
                if (x.isNumber() && y.isNumber()) return x.asNumber() < y.asNumber();
                return x.toString() < y.toString();
            });
        }
        return a[0];
    });
    vm.registerMethod("Array", "slice", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        std::int64_t from = a[1].asInt();
        std::int64_t to = a.size() > 2 ? a[2].asInt() : static_cast<std::int64_t>(arr->count);
        if (from < 0) from += arr->count;
        if (to < 0) to += arr->count;
        from = std::clamp<std::int64_t>(from, 0, arr->count);
        to = std::clamp<std::int64_t>(to, from, arr->count);
        ObjArray* out = lv::makeArray(v.gc(), static_cast<std::uint32_t>(to - from));
        for (std::int64_t i = from; i < to; ++i) out->items[out->count++] = arr->items[i];
        return Value::object(out);
    });
    vm.registerMethod("Array", "find", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        for (std::uint32_t i = 0; i < arr->count; ++i)
            if (util::callFunction(v, a[1], {arr->items[i]}).truthy()) return arr->items[i];
        return Value::nil();
    });
    vm.registerMethod("Array", "any", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::boolean(false);
        for (std::uint32_t i = 0; i < arr->count; ++i)
            if (util::callFunction(v, a[1], {arr->items[i]}).truthy()) return Value::boolean(true);
        return Value::boolean(false);
    });
    vm.registerMethod("Array", "all", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::boolean(true);
        for (std::uint32_t i = 0; i < arr->count; ++i)
            if (!util::callFunction(v, a[1], {arr->items[i]}).truthy()) return Value::boolean(false);
        return Value::boolean(true);
    });
    vm.registerMethod("Array", "sum", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        double s = 0;
        if (arr) for (std::uint32_t i = 0; i < arr->count; ++i) s += arr->items[i].asNumber();
        return Value::fromNumber(s);
    });
    vm.registerMethod("Array", "shuffle", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* arr = util::requireArray(v, a[0], "self");
        if (!arr) return Value::nil();
        for (std::uint32_t i = arr->count; i > 1; --i) {
            const std::uint32_t j = static_cast<std::uint32_t>(v.nextRandom() % i);
            std::swap(arr->items[i - 1], arr->items[j]);
        }
        return a[0];
    });
}

// ---------------------------------------------------------------------------
//  String
// ---------------------------------------------------------------------------
void registerStringMethods(VM& vm) {
    vm.registerMethod("String", "len", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        if (!a[0].isObject() || a[0].asObject()->type != ObjHeader::Type::String) return Value::integer(0);
        return Value::integer(a[0].as<ObjString>()->length);
    });
    vm.registerMethod("String", "upper", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        std::string s(util::requireString(v, a[0], "self"));
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return v.internString(std::move(s));
    });
    vm.registerMethod("String", "lower", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        std::string s(util::requireString(v, a[0], "self"));
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v.internString(std::move(s));
    });
    vm.registerMethod("String", "trim", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        std::string s(util::requireString(v, a[0], "self"));
        const auto b = s.find_first_not_of(" \t\r\n");
        const auto e = s.find_last_not_of(" \t\r\n");
        return v.internString(b == std::string::npos ? std::string() : s.substr(b, e - b + 1));
    });
    vm.registerMethod("String", "contains", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        return Value::boolean(util::requireString(v, a[0], "self").find(util::requireString(v, a[1], "needle")) != std::string_view::npos);
    });
    vm.registerMethod("String", "startsWith", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        const auto s = util::requireString(v, a[0], "self");
        const auto p = util::requireString(v, a[1], "prefix");
        return Value::boolean(s.size() >= p.size() && s.compare(0, p.size(), p) == 0);
    });
    vm.registerMethod("String", "find", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        const auto pos = util::requireString(v, a[0], "self").find(util::requireString(v, a[1], "needle"));
        return pos == std::string_view::npos ? Value::integer(-1) : Value::integer(static_cast<std::int64_t>(pos));
    });
    vm.registerMethod("String", "replace", 3, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 3);
        std::string s(util::requireString(v, a[0], "self"));
        const std::string from(util::requireString(v, a[1], "from"));
        const std::string to(util::requireString(v, a[2], "to"));
        if (from.empty()) return v.internString(s);
        for (std::size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
            s.replace(p, from.size(), to);
        return v.internString(std::move(s));
    });
    vm.registerMethod("String", "split", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        const std::string s(util::requireString(v, a[0], "self"));
        const std::string sep = a.size() > 1 ? std::string(util::requireString(v, a[1], "separator")) : ",";
        ObjArray* out = lv::makeArray(v.gc(), 8);
        std::size_t start = 0;
        for (;;) {
            const std::size_t p = s.find(sep, start);
            const std::string part = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
            out->grow(v.gc(), out->count + 1);
            out->items[out->count++] = v.internString(part);
            if (p == std::string::npos) break;
            start = p + sep.size();
        }
        return Value::object(out);
    });
    vm.registerMethod("String", "sub", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        const auto s = util::requireString(v, a[0], "self");
        std::int64_t from = a[1].asInt();
        std::int64_t len = a.size() > 2 ? a[2].asInt() : static_cast<std::int64_t>(s.size()) - from;
        if (from < 0) from += static_cast<std::int64_t>(s.size());
        from = std::clamp<std::int64_t>(from, 0, static_cast<std::int64_t>(s.size()));
        len = std::clamp<std::int64_t>(len, 0, static_cast<std::int64_t>(s.size()) - from);
        return v.internString(s.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(len)));
    });
    vm.registerMethod("String", "format", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        // Простой формат: "{}" заменяется очередным аргументом (без printf-рисков).
        std::string s(util::requireString(v, a[0], "self"));
        std::string out;
        std::size_t argi = 1;
        for (std::size_t i = 0; i < s.size();) {
            if (i + 1 < s.size() && s[i] == '{' && s[i + 1] == '}') {
                out += (argi < a.size()) ? a[argi++].toString() : "{}";
                i += 2;
            } else out += s[i++];
        }
        return v.internString(std::move(out));
    });
}

// ---------------------------------------------------------------------------
//  Map / Result / Coroutine
// ---------------------------------------------------------------------------
void registerMapMethods(VM& vm) {
    vm.registerMethod("Map", "get", 0xFFFFFFFF, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* m = util::requireMap(v, a[0], "self");
        if (!m) return a.size() > 2 ? a[2] : Value::nil();
        Value* p = m->find(a[1]);
        return p ? *p : (a.size() > 2 ? a[2] : Value::nil());
    });
    vm.registerMethod("Map", "set", 3, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 3);
        if (auto* m = util::requireMap(v, a[0], "self")) m->set(v.gc(), a[1], a[2]);
        return a[0];
    });
    vm.registerMethod("Map", "has", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* m = util::requireMap(v, a[0], "self");
        return Value::boolean(m && m->find(a[1]) != nullptr);
    });
    vm.registerMethod("Map", "remove", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* m = util::requireMap(v, a[0], "self");
        return Value::boolean(m && m->remove(a[1]));
    });
    vm.registerMethod("Map", "keys", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* m = util::requireMap(v, a[0], "self");
        ObjArray* out = lv::makeArray(v.gc(), m ? m->count : 0);
        if (m) for (std::uint32_t i = 0; i < m->capacity; ++i)
            if (m->entries[i].used) out->items[out->count++] = m->entries[i].key;
        return Value::object(out);
    });
    vm.registerMethod("Map", "values", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* m = util::requireMap(v, a[0], "self");
        ObjArray* out = lv::makeArray(v.gc(), m ? m->count : 0);
        if (m) for (std::uint32_t i = 0; i < m->capacity; ++i)
            if (m->entries[i].used) out->items[out->count++] = m->entries[i].value;
        return Value::object(out);
    });
    vm.registerMethod("Map", "len", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        auto* m = util::requireMap(v, a[0], "self");
        return Value::integer(m ? m->count : 0);
    });
    vm.registerMethod("Map", "forEach", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        auto* m = util::requireMap(v, a[0], "self");
        if (!m) return Value::nil();
        for (std::uint32_t i = 0; i < m->capacity; ++i)
            if (m->entries[i].used)
                util::callFunction(v, a[1], {m->entries[i].value, m->entries[i].key});
        return Value::nil();
    });
}

void registerResultMethods(VM& vm) {
    vm.registerMethod("Result", "ok", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        if (!a[0].isObject() || a[0].asObject()->type != ObjHeader::Type::Result) return Value::boolean(false);
        return Value::boolean(a[0].as<ObjResult>()->ok);
    });
    vm.registerMethod("Result", "unwrap", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        if (!a[0].isObject() || a[0].asObject()->type != ObjHeader::Type::Result) return a[0];
        auto* r = a[0].as<ObjResult>();
        if (!r->ok) v.runtimeErrorPublic("unwrap() called on Err: " + r->error.toString());
        return r->value;
    });
    vm.registerMethod("Result", "value", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        return (a[0].isObject() && a[0].asObject()->type == ObjHeader::Type::Result) ? a[0].as<ObjResult>()->value : Value::nil();
    });
    vm.registerMethod("Result", "error", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        return (a[0].isObject() && a[0].asObject()->type == ObjHeader::Type::Result) ? a[0].as<ObjResult>()->error : Value::nil();
    });
    vm.registerMethod("Result", "orElse", 2, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 2);
        if (a[0].isObject() && a[0].asObject()->type == ObjHeader::Type::Result) {
            auto* r = a[0].as<ObjResult>();
            return r->ok ? r->value : a[1];
        }
        return a[0];
    });
}

void registerCoroutineMethods(VM& vm) {
    vm.registerMethod("Coroutine", "status", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        if (!a[0].isObject() || a[0].asObject()->type != ObjHeader::Type::Coroutine) return v.internString("invalid");
        switch (a[0].as<ObjCoroutine>()->state) {
            case ObjCoroutine::State::Created:   return v.internString("created");
            case ObjCoroutine::State::Running:   return v.internString("running");
            case ObjCoroutine::State::Suspended: return v.internString("suspended");
            case ObjCoroutine::State::Dead:      return v.internString("dead");
            case ObjCoroutine::State::Error:     return v.internString("error");
        }
        return v.internString("?");
    });
    vm.registerMethod("Coroutine", "dead", 1, [](VM& v, std::span<const Value> a) -> Value {
        ARGCHECK_IN(a, 1);
        return Value::boolean(a[0].isObject() && a[0].asObject()->type == ObjHeader::Type::Coroutine &&
                              a[0].as<ObjCoroutine>()->state == ObjCoroutine::State::Dead);
    });
}

// ---------------------------------------------------------------------------
//  Модуль math
// ---------------------------------------------------------------------------
void installMathModuleImpl(VM& vm) {
    Value pi    = Value::fromNumber(3.14159265358979323846);
    Value tau   = Value::fromNumber(6.28318530717958647692);
    Value e     = Value::fromNumber(2.71828182845904523536);
    Value infin = Value::fromNumber(std::numeric_limits<double>::infinity());

    auto fn = [&](std::string_view n, std::uint32_t arity, ObjNativeFn::Fn f) {
        return native(vm, n, arity, std::move(f));
    };

    util::makeModule(vm, "math", {
        {"pi", pi}, {"tau", tau}, {"e", e}, {"inf", infin},
        {"sqrt",  fn("sqrt", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::sqrt(a[0].asNumber())); })},
        {"sin",   fn("sin", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::sin(a[0].asNumber())); })},
        {"cos",   fn("cos", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::cos(a[0].asNumber())); })},
        {"tan",   fn("tan", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::tan(a[0].asNumber())); })},
        {"asin",  fn("asin", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::asin(a[0].asNumber())); })},
        {"acos",  fn("acos", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::acos(a[0].asNumber())); })},
        {"atan",  fn("atan", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::atan(a[0].asNumber())); })},
        {"atan2", fn("atan2", 2, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 2); return Value::fromNumber(std::atan2(a[0].asNumber(), a[1].asNumber())); })},
        {"pow",   fn("pow", 2, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 2); return Value::fromNumber(std::pow(a[0].asNumber(), a[1].asNumber())); })},
        {"exp",   fn("exp", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::exp(a[0].asNumber())); })},
        {"log",   fn("log", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::log(a[0].asNumber())); })},
        {"floor", fn("floor", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::floor(a[0].asNumber())); })},
        {"ceil",  fn("ceil", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::ceil(a[0].asNumber())); })},
        {"round", fn("round", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::nearbyint(a[0].asNumber())); })},
        {"sign",  fn("sign", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); const double x = a[0].asNumber(); return Value::integer(x > 0 ? 1 : x < 0 ? -1 : 0); })},
        {"min",   fn("min", 2, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 2); return Value::fromNumber(std::min(a[0].asNumber(), a[1].asNumber())); })},
        {"max",   fn("max", 2, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 2); return Value::fromNumber(std::max(a[0].asNumber(), a[1].asNumber())); })},
        {"abs",   fn("abs", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(std::fabs(a[0].asNumber())); })},
        {"clamp", fn("clamp", 3, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 3); return Value::fromNumber(std::clamp(a[0].asNumber(), a[1].asNumber(), a[2].asNumber())); })},
        {"lerp",  fn("lerp", 3, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 3); const double x = a[0].asNumber(), y = a[1].asNumber(), t = a[2].asNumber(); return Value::fromNumber(x + (y - x) * t); })},
        {"random", fn("random", 0, [](VM& v, std::span<const Value>) { return Value::fromNumber(static_cast<double>(v.nextRandom() >> 11) / 9007199254740992.0); })},
        {"randomInt", fn("randomInt", 2, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 2); const std::int64_t lo = a[0].asInt(), hi = a[1].asInt(); return Value::integer(hi <= lo ? lo : lo + static_cast<std::int64_t>(v.nextRandom() % static_cast<std::uint64_t>(hi - lo + 1))); })},
        {"deg",   fn("deg", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(a[0].asNumber() * 57.29577951308232); })},
        {"rad",   fn("rad", 1, [](VM& v, std::span<const Value> a) { ARGCHECK_IN(a, 1); return Value::fromNumber(a[0].asNumber() * 0.017453292519943295); })},
    });
}

} // namespace

// ---------------------------------------------------------------------------
void installStdlib(VM& vm) {
    registerGlobals(vm);
    registerArrayMethods(vm);
    registerStringMethods(vm);
    registerMapMethods(vm);
    registerResultMethods(vm);
    registerCoroutineMethods(vm);
    installMathModuleImpl(vm);
}

void installMathModule(VM& vm) { installMathModuleImpl(vm); }

void VM::installStdlib() { lv::installStdlib(*this); }

} // namespace lv
