/**
 * @file    Component.h
 * @brief   Метаданные компонентов: TypeId, концепции, признаки.
 * @ingroup ECS
 *
 * @details Каждый тип компонента получает стабильный числовой @c TypeId через
 *          статический счётчик в шаблоне. Это даёт:
 *          - O(1) сравнение типов в архетипах (без type_info и RTTI);
 *          - возможность строить «сигнатуру архетипа» как битовую маску/вектор;
 *          - детерминированный порядок при сериализации сцены.
 */
#pragma once

#include "Entity.h"

#include <atomic>
#include <bitset>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lv::ecs {

using TypeId = std::uint32_t;

/// Число различных типов компонентов, поддерживаемое движком.
inline constexpr TypeId kMaxComponentTypes = 256;

namespace detail {
/// Глобальный монотонный счётчик идентификаторов типов.
inline std::atomic<TypeId>& typeIdCounter() noexcept {
    static std::atomic<TypeId> counter{0};
    return counter;
}
} // namespace detail

/**
 * @brief Идентификатор типа компонента (один на тип T на процесс).
 */
template <class T>
[[nodiscard]] inline TypeId typeIdOf() noexcept {
    static const TypeId id = detail::typeIdCounter().fetch_add(1, std::memory_order_relaxed);
    return id;
}

/**
 * @brief Имя типа для инспектора и логов.
 *
 * По умолчанию берётся из @c __PRETTY_FUNCTION__ / @c __FUNCSIG__; компонент
 * может переопределить его, объявив @c static constexpr std::string_view lv_component_name.
 */
template <class T>
[[nodiscard]] inline std::string_view typeNameOf() noexcept {
    if constexpr (requires { T::lv_component_name; }) {
        return T::lv_component_name;
    } else {
#if defined(__clang__) || defined(__GNUC__)
        static const std::string_view cached = [] {
            std::string_view s = __PRETTY_FUNCTION__;
            const auto b = s.find("T = ");
            if (b == std::string_view::npos) return std::string_view("?");
            s = s.substr(b + 4);
            const auto e = s.find_first_of(";]");
            return s.substr(0, e);
        }();
        return cached;
#else
        return "Component";
#endif
    }
}

/**
 * @brief Концепция «пригоден как компонент ECS».
 *
 * Требования сознательно мягкие: тривиальная копируемость НЕ обязательна
 * (компонент может владеть std::vector, например список целей AI), но тип
 * должен быть разрушаемым и не полиморфным — архетип хранит его массивом.
 */
template <class T>
concept Component = std::is_class_v<T> && std::is_destructible_v<T> && !std::is_polymorphic_v<T>;

/// Признак: компонент-«тег» (нулевой размер) — используется только для фильтрации.
template <class T>
concept TagComponent = Component<T> && (sizeof(T) <= 1);

/// Признак: компонент изменяется каждый кадр => его пул стоит держать «горячим».
template <class T>
concept VolatileComponent = Component<T> && requires { T::lv_volatile; } && T::lv_volatile;

/**
 * @brief Сигнатура архетипа — компактное множество TypeId.
 *
 * 256 бит = 32 байта: сравнение сигнатур — это 4 сравнения uint64,
 * что быстрее любого хэш-мапа на малых размерах.
 */
class Signature {
public:
    void set(TypeId id) noexcept { bits_.set(id); }
    void clear(TypeId id) noexcept { bits_.reset(id); }
    [[nodiscard]] bool test(TypeId id) const noexcept { return bits_.test(id); }

    /// Содержит ли эта сигнатура все биты другой (нужно для матчинга view).
    [[nodiscard]] bool contains(const Signature& other) const noexcept {
        return ((bits_ & other.bits_) == other.bits_);
    }

    [[nodiscard]] std::size_t count() const noexcept { return bits_.count(); }
    [[nodiscard]] bool operator==(const Signature&) const = default;

    /// Список установленных битов (для отладки и сериализации).
    [[nodiscard]] std::vector<TypeId> toList() const {
        std::vector<TypeId> out;
        out.reserve(bits_.count());
        for (std::size_t i = 0; i < kMaxComponentTypes; ++i)
            if (bits_.test(i)) out.push_back(static_cast<TypeId>(i));
        return out;
    }

    /// Отсортированный вектор TypeId — каноническая форма для хэш-ключа.
    [[nodiscard]] static Signature fromList(std::vector<TypeId> ids) {
        Signature s;
        for (TypeId id : ids) s.set(id);
        return s;
    }

private:
    std::bitset<kMaxComponentTypes> bits_{};
};

/// Хэш сигнатуры (для @c unordered_map<Signature, ArchetypeId>).
struct SignatureHash {
    [[nodiscard]] std::size_t operator()(const Signature& s) const noexcept {
        std::size_t h = 1469598103934665603ULL;
        for (TypeId id : s.toList()) {
            h ^= id;
            h *= 1099511628211ULL;
        }
        return h;
    }
};

} // namespace lv::ecs
