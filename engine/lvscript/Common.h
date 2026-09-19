/**
 * @file    Common.h
 * @brief   Базовые типы, диагностика и утилиты подсистемы LV Script.
 * @ingroup LVScript
 *
 * @details Здесь определены:
 *          - @c lv::format — компактная замена @c std::format (см. примечание ниже);
 *          - @c lv::SourceLoc / @c lv::Diagnostic — единый формат сообщений об ошибках
 *            (одинаково используется лексером, парсером, тайп-чекером и VM);
 *          - @c lv::Byte — псевдоним байта байткода;
 *          - хэш-функции и форматирование чисел.
 *
 * @author  LimVine Engine Team
 * @version 1.0.0
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <optional>
#include <variant>
#include <memory>
#include <functional>
#include <concepts>
#include <type_traits>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <limits>

namespace lv {

// ---------------------------------------------------------------------------
//  lv::format — компактная замена std::format
// ---------------------------------------------------------------------------
/**
 * @anchor lv_format
 * @brief Форматирование строки с плейсхолдерами `{}` / `{:spec}`.
 *
 * LV Script поддерживает тулчейны GCC 12+, Clang 16+ и MSVC 19.34+, а
 * `<format>` есть не во всех из них. Чтобы код не зависел от наличия заголовка,
 * используется эта реализация с тем же синтаксисом. Внутри `namespace lv`
 * вызовы пишутся как `format(...)` и находятся обычным поиском имён; при
 * переходе на стандартную библиотеку достаточно удалить блок и заменить
 * `format(` на `std::format(`.
 *
 * Поддерживаемые типы аргументов: целые, числа с плавающей точкой, bool, char,
 * указатели, `std::string`, всё convertible в `std::string_view`, а также любой
 * тип с методом `toString()` (в частности @c lv::Value).
 *
 * @param fmt Строка формата. `{{` и `}}` — литеральные скобки.
 * @return Отформатированная строка.
 */
template <class... Args>
[[nodiscard]] inline std::string format(std::string_view fmt, Args&&... args);

namespace detail {

/// Прочитать спецификатор `{...}` и дописать его printf-эквивалент.
inline void fmtAppendSpec(std::string& out, const char*& p) {
    std::string spec;
    while (*p && *p != '}') spec += *p++;
    if (*p == '}') ++p;
    out += '%';
    // Пустая спецификация => строка; иначе пропускаем её как есть (`:d`, `.2f`, ...).
    out += spec.empty() ? std::string("s") : spec;
}

/// Сериализовать один аргумент в C-строку внутри @p buf.
template <class T>
inline void fmtArg(std::vector<char>& buf, std::size_t& len, T&& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        const char* s = v ? "true" : "false";
        std::memcpy(buf.data() + len, s, std::strlen(s) + 1);
        len += std::strlen(s) + 1;
    } else if constexpr (std::is_same_v<U, char>) {
        buf[len] = v; buf[len + 1] = 0; len += 2;
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        len += static_cast<std::size_t>(std::snprintf(buf.data() + len, buf.size() - len, "%lld",
                                                      static_cast<long long>(v))) + 1;
    } else if constexpr (std::is_integral_v<U> && std::is_unsigned_v<U>) {
        len += static_cast<std::size_t>(std::snprintf(buf.data() + len, buf.size() - len, "%llu",
                                                      static_cast<unsigned long long>(v))) + 1;
    } else if constexpr (std::is_floating_point_v<U>) {
        len += static_cast<std::size_t>(std::snprintf(buf.data() + len, buf.size() - len, "%g",
                                                      static_cast<double>(v))) + 1;
    } else if constexpr (std::is_pointer_v<U> && std::is_void_v<std::remove_pointer_t<U>>) {
        len += static_cast<std::size_t>(std::snprintf(buf.data() + len, buf.size() - len, "%p",
                                                      static_cast<const void*>(v))) + 1;
    } else if constexpr (std::is_pointer_v<U>) {
        // `const char*` / `char*` — строка (должно идти до string_view-ветки).
        const std::string_view sv(v ? v : "");
        std::memcpy(buf.data() + len, sv.data(), sv.size());
        buf[len + sv.size()] = 0;
        len += sv.size() + 1;
    } else if constexpr (std::is_same_v<U, std::string>) {
        std::memcpy(buf.data() + len, v.c_str(), v.size() + 1);
        len += v.size() + 1;
    } else if constexpr (std::is_convertible_v<U, std::string_view>) {
        const std::string_view sv(v);
        std::memcpy(buf.data() + len, sv.data(), sv.size());
        buf[len + sv.size()] = 0;
        len += sv.size() + 1;
    } else if constexpr (requires { v.toString(); }) {
        const std::string s = v.toString();
        std::memcpy(buf.data() + len, s.c_str(), s.size() + 1);
        len += s.size() + 1;
    } else {
        buf[len] = '?'; buf[len + 1] = 0; len += 2;
    }
}

/// Разобрать printf-спецификатор, начинающийся с '%', вернуть его длину.
inline std::size_t fmtScanSpec(const std::string& s, std::size_t i) {
    std::size_t j = i + 1;
    while (j < s.size() && (std::strchr("-+ #0", s[j]) ||
                            std::isdigit(static_cast<unsigned char>(s[j])) ||
                            s[j] == '.' || s[j] == 'l' || s[j] == 'h' ||
                            s[j] == 'z' || s[j] == 'j' || s[j] == 't' || s[j] == 'L'))
        ++j;
    return (j < s.size()) ? j - i + 1 : j - i;
}

} // namespace detail

template <class... Args>
[[nodiscard]] inline std::string format(std::string_view fmt, Args&&... args) {
    std::vector<char> buf(64 * (sizeof...(args) + 1) + fmt.size() * 2 + 256, '\0');
    std::size_t len = 0;
    (detail::fmtArg(buf, len, std::forward<Args>(args)), ...);

    // 1) Превращаем `{}`-формат в printf-формат.
    std::string spec;
    spec.reserve(fmt.size() + 64);
    const char* p = fmt.data();
    const char* const end = fmt.data() + fmt.size();
    while (p < end) {
        if (*p == '{') {
            if (p + 1 < end && p[1] == '{') { spec += '{'; p += 2; continue; }
            ++p;
            detail::fmtAppendSpec(spec, p);
        } else if (*p == '}') {
            if (p + 1 < end && p[1] == '}') { spec += '}'; p += 2; continue; }
            ++p;
        } else {
            spec += *p++;
        }
    }

    // 2) Разрезаем буфер аргументов на строки.
    std::vector<std::string> argStrings;
    argStrings.reserve(sizeof...(args));
    const char* cur = buf.data();
    for (std::size_t i = 0; i < sizeof...(args); ++i) {
        argStrings.emplace_back(cur);
        cur += std::strlen(cur) + 1;
    }

    // 3) Последовательно подставляем. Все аргументы уже приведены к строкам,
    //    поэтому спецификатор применяется к `const char*`.
    std::string result;
    result.reserve(spec.size() + len);
    std::size_t i = 0, ai = 0;
    while (i < spec.size()) {
        if (spec[i] == '%' && i + 1 < spec.size() && spec[i + 1] == '%') { result += '%'; i += 2; continue; }
        if (spec[i] == '%') {
            const std::size_t specLen = detail::fmtScanSpec(spec, i);
            const std::string one = spec.substr(i, specLen);
            if (ai < argStrings.size()) {
                const std::size_t need = argStrings[ai].size() + 64;
                std::string tmp(need, '\0');
                std::snprintf(tmp.data(), need, one.c_str(), argStrings[ai].c_str());
                tmp.resize(std::strlen(tmp.c_str()));
                result += tmp;
                ++ai;
            } else {
                result += one;
            }
            i += specLen;
        } else {
            result += spec[i++];
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
//  Базовые константы и типы
// ---------------------------------------------------------------------------

/// Псевдоним байта байткода.
using Byte = std::uint8_t;

/// 24-битный операнд инструкции (адресация до 16 МБ локалов/констант).
static constexpr std::uint32_t kMaxOperand24 = (1u << 24) - 1u;

/// Максимальное число локальных переменных в одной функции.
static constexpr std::uint32_t kMaxLocals = 65535;

/// Максимальная глубина стека вызовов (защита от бесконечной рекурсии).
static constexpr std::int32_t kMaxCallFrames = 1024;

/// Ёмкость стека значений одного контекста исполнения.
static constexpr std::size_t kValueStackCapacity = 256 * 1024;

/// Версия формата байткода (.lvc). Несовпадение => перекомпиляция из исходника.
static constexpr std::uint32_t kBytecodeVersion = 0x0001'0000;

/**
 * @brief Позиция в исходном тексте.
 *
 * Хранится в каждом узле AST и в таблице строк байткода, что позволяет печатать
 * точные трейсбеки рантайм-ошибок.
 */
struct SourceLoc {
    std::uint32_t line   = 1;   ///< Номер строки, начиная с 1.
    std::uint32_t column = 1;   ///< Номер колонки, начиная с 1.
    std::uint32_t offset = 0;   ///< Байтовое смещение от начала файла.

    [[nodiscard]] constexpr bool operator==(const SourceLoc&) const = default;
};

/// Имя исходного файла/модуля (используется в трейсбеках).
using SourceName = std::string;

/**
 * @brief Уровень серьёзности диагностического сообщения.
 */
enum class DiagSeverity : std::uint8_t {
    Note,    ///< Информационное сообщение.
    Warning, ///< Предупреждение — компиляция продолжается.
    Error    ///< Ошибка — компиляция прерывается.
};

/**
 * @brief Сообщение компилятора/рантайма.
 *
 * Единый формат для всех фаз. Редактор рисует их в консоли LV Script
 * (см. @c lv::editor::ConsolePanel) с переходом к строке по клику.
 */
struct Diagnostic {
    DiagSeverity severity = DiagSeverity::Error;
    SourceName   source   = "<anon>";
    SourceLoc    loc{};
    std::string  message;
    std::string  hint;      ///< Необязательная подсказка «а не хотели ли вы ...».

    /// Печать в стиле clang/rustc: `file.lvs:12:5: error: ...`.
    [[nodiscard]] std::string toString() const {
        std::string s = format("{}:{}:{}: {}: {}", source, loc.line, loc.column,
                               severity == DiagSeverity::Error     ? "error"
                               : severity == DiagSeverity::Warning ? "warning"
                                                                   : "note",
                               message);
        if (!hint.empty()) s += format("\n  hint: {}", hint);
        return s;
    }
};

/// Список диагностики, накапливаемый фазами компиляции.
using DiagnosticList = std::vector<Diagnostic>;

/**
 * @brief Минимальный аналог @c std::expected (отсутствует в GCC 12).
 *
 * Как только минимальный поддерживаемый тулчейн получит `<expected>`,
 * алиас заменяется на `std::expected<T, E>` без изменения кода пользователей.
 */
template <class T, class E>
class Expected {
public:
    Expected(T v) : has_(true), val_(std::move(v)) {}                 ///< NOLINT(google-explicit-constructor)
    static Expected error(E e) { Expected x; x.has_ = false; x.err_ = std::move(e); return x; }

    [[nodiscard]] bool  hasValue() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }
    [[nodiscard]] T&       value() &       { return val_; }
    [[nodiscard]] const T& value() const&  { return val_; }
    [[nodiscard]] T&&      value() &&      { return std::move(val_); }
    [[nodiscard]] E&       error() &       { return err_; }
    [[nodiscard]] const E& error() const&  { return err_; }
    [[nodiscard]] T&       operator*() &   { return val_; }
    [[nodiscard]] const T& operator*() const& { return val_; }

private:
    Expected() = default;
    bool has_ = false;
    T    val_{};
    E    err_{};
};

/// Результат фазы компиляции: значение либо список ошибок.
template <class T>
using CompileResult = Expected<T, DiagnosticList>;

// ---------------------------------------------------------------------------
//  Хэши и форматирование чисел
// ---------------------------------------------------------------------------

/**
 * @brief FNV-1a хэш для интернирования строк.
 *
 * Интернированные строки сравниваются по указателю, что ускоряет доступ
 * к полям классов и ключам map.
 */
[[nodiscard]] inline std::uint64_t hashString(std::string_view sv) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : sv) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

/// Хэш-комбинатор для структур.
[[nodiscard]] inline std::uint64_t hashCombine(std::uint64_t a, std::uint64_t b) noexcept {
    return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

/**
 * @brief Round-trip представление числа (shortest round-trip через `%.17g`).
 */
[[nodiscard]] inline std::string numberToString(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s(buf);
    // Целое значение печатаем без ".0" — Int и Float различаются в рантайме по значению.
    return s;
}

/// Целочисленное форматирование.
[[nodiscard]] inline std::string intToString(std::int64_t v) { return std::to_string(v); }

/// Внутренняя проверка инвариантов (в release — no-op).
#define LV_ASSERT(cond) assert(cond)

/// Недостижимый код.
#define LV_UNREACHABLE() __builtin_unreachable()

} // namespace lv
