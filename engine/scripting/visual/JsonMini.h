/**
 * @file    JsonMini.h
 * @brief   Минимальный JSON-парсер/сериализатор для форматов движка.
 * @ingroup Asset
 *
 * @details Зависимости от сторонних JSON-библиотек сознательно нет: движку
 *          нужны только чтение конфигов, .lvgraph и .lvscene, а собственный
 *          парсер занимает ~300 строк, не тянет исключений в горячий путь и
 *          компилируется везде. Формат — строгое подмножество RFC 8259
 *          (объекты, массивы, строки с экранированием, числа, true/false/null).
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace lv::json {

class Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

/**
 * @brief JSON-значение.
 */
class Value {
public:
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool b) : data_(b) {}
    Value(double d) : data_(d) {}
    Value(int i) : data_(static_cast<double>(i)) {}
    Value(std::int64_t i) : data_(static_cast<double>(i)) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}
    Value(Array a) : data_(std::move(a)) {}
    Value(Object o) : data_(std::move(o)) {}

    [[nodiscard]] bool isNull()   const noexcept { return std::holds_alternative<std::nullptr_t>(data_); }
    [[nodiscard]] bool isBool()   const noexcept { return std::holds_alternative<bool>(data_); }
    [[nodiscard]] bool isNumber() const noexcept { return std::holds_alternative<double>(data_); }
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(data_); }
    [[nodiscard]] bool isArray()  const noexcept { return std::holds_alternative<Array>(data_); }
    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(data_); }

    [[nodiscard]] bool               asBool(bool def = false)            const { return isBool() ? std::get<bool>(data_) : def; }
    [[nodiscard]] double             asNumber(double def = 0)            const { return isNumber() ? std::get<double>(data_) : def; }
    [[nodiscard]] std::int64_t       asInt(std::int64_t def = 0)         const { return isNumber() ? static_cast<std::int64_t>(std::get<double>(data_)) : def; }
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] std::string        asStringOr(std::string_view def) const { return isString() ? std::get<std::string>(data_) : std::string(def); }
    [[nodiscard]] const Array&       asArray()  const;
    [[nodiscard]] const Object&      asObject() const;

    /// Доступ к полю объекта (создаёт null при отсутствии — только для чтения).
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    [[nodiscard]] const Value& operator[](std::size_t index) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    /// Запись в объект.
    Value& set(std::string key, Value v);

    /// Сериализация (compact или pretty).
    [[nodiscard]] std::string dump(int indent = -1) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    Storage data_;
};

/// Разбор JSON. При ошибке возвращает false и заполняет @p err.
[[nodiscard]] bool parse(std::string_view text, Value& out, std::string& err);

/// Удобная обёртка: parse или null.
[[nodiscard]] Value parseOr(std::string_view text);

} // namespace lv::json
