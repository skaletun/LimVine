/**
 * @file    Entity.h
 * @brief   Идентификатор сущности ECS: индекс + поколение.
 * @ingroup ECS
 *
 * @details Поколение (generation) решает классическую проблему «висящего
 *          идентификатора»: после уничтожения сущности её слот переиспользуется,
 *          но поколение увеличивается, поэтому старые handle'ы (в скриптах,
 *          в сохранении, в UI) перестают быть валидными вместо того, чтобы
 *          молча указывать на чужую сущность.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <compare>

namespace lv::ecs {

/**
 * @brief Handle сущности: 32 бита индекса + 32 бита поколения.
 *
 * @invariant Значение по умолчанию (@c kNullEntity) невалидно; любые операции
 *            с ним в @c Registry являются no-op, а не UB.
 */
struct Entity {
    std::uint32_t index      = 0xFFFFFFFFu;
    std::uint32_t generation = 0;

    /// «Пустая» сущность.
    static constexpr Entity null() noexcept { return {}; }

    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0xFFFFFFFFu; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] constexpr auto operator<=>(const Entity&) const = default;
};

/// Константа для сравнений и значений по умолчанию.
inline constexpr Entity kNullEntity = Entity::null();

/// Сериализационный идентификатор (для сохранений и сетевого кода).
[[nodiscard]] constexpr std::uint64_t entityId(const Entity e) noexcept {
    return (static_cast<std::uint64_t>(e.generation) << 32) | e.index;
}

} // namespace lv::ecs

template <>
struct std::hash<lv::ecs::Entity> {
    [[nodiscard]] std::size_t operator()(const lv::ecs::Entity e) const noexcept {
        // Хэш-комбинация Фибоначчи: дёшево и равномерно даже для плотных индексов.
        std::uint64_t h = (static_cast<std::uint64_t>(e.index) << 32) ^ e.generation;
        h *= 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(h ^ (h >> 32));
    }
};
