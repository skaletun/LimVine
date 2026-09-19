/**
 * @file    Hierarchy.h
 * @brief   Родительско-дочерние отношения сущностей и world-transform.
 * @ingroup ECS
 *
 * @details Компоненты @c Parent / @c Children задают пространственную иерархию
 *          (оружие на персонаже, камера на голове, меши как дети актора).
 *          Система @c HierarchySystem в фазе @c PostPhysics вычисляет
 *          world-матрицы обходом корней и записывает их в компонент
 *          @c WorldTransform — кэш для рендера и физики.
 *
 *          Детали реализации:
 *          - инвалидация идёт по dirty-флагу: движется только ветка, в которой
 *            изменился родитель или локальная трансформация;
 *          - циклы в графе предотвращаются в @c setParent();
 *          - порядок обхода — BFS от корней (гарантирует, что родитель всегда
 *            обработан раньше детей).
 */
#pragma once

#include "Entity.h"
#include "Registry.h"
#include "../core/Math.h"

#include <vector>
#include <cstdint>

namespace lv::ecs {

/**
 * @brief Родитель сущности. Отсутствие компонента == корень.
 */
struct Parent {
    Entity entity = kNullEntity;
    /// Для кэширования: локальная матрица в момент последнего обновления.
    mutable std::uint32_t lastParentVersion = 0;
    static constexpr std::string_view lv_component_name = "Parent";
};

/**
 * @brief Список детей сущности (заполняется и поддерживается иерархической системой).
 *
 * Редактировать @c entities вручную нельзя: используйте @c setParent().
 */
struct Children {
    std::vector<Entity> entities;
    static constexpr std::string_view lv_component_name = "Children";
};

/**
 * @brief Кэш world-матрицы, обновляемый HierarchySystem.
 *
 * Рендер, физика и sound-системы читают world-матрицу отсюда, а не пересчитывают
 * цепочку родителей каждый раз.
 */
struct WorldTransform {
    Vec3 worldPosition{};
    Quat worldRotation{};
    Vec3 worldScale{1, 1, 1};
    Mat4 matrix = Mat4::identity();
    /// Монотонно растущая версия — инкрементируется при каждом изменении
    /// worldTransform; рендер и физика используют его как dirty-триггер.
    std::uint32_t version = 0;
    /// Флаг «движок ещё не обработал изменения этого кадра».
    bool dirty = true;
    static constexpr std::string_view lv_component_name = "WorldTransform";
};

/**
 * @brief Исключение зацикливания (A -> ... -> A).
 */
struct HierarchyCycleError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Установить родителя сущности. Перестраивает списки Children.
 *
 * @param world       Реестр.
 * @param child       Сущность, которую прикрепляем.
 * @param newParent   Новый родитель, или @c kNullEntity для отсоединения в корень.
 * @param keepWorldPosition  Если true, локальная позиция корректируется так,
 *                    чтобы world-позиция не изменилась (удобно для редактора).
 * @throw HierarchyCycleError если @c newParent является потомком @c child.
 */
void setParent(Registry& reg, Entity child, Entity newParent, bool keepWorldPosition = false);

/**
 * @brief Обновить все world-матрицы иерархии (BFS от корней).
 *
 * Вызывается один раз за кадр системой фазы PostPhysics.
 * @return число затронутых сущностей (для профилирования).
 */
std::size_t updateWorldTransforms(Registry& reg);

/**
 * @brief Вернуть worldTransform сущности (работает и для корней).
 *
 * Если компонента @c WorldTransform нет, вычисляет из @c Transform напрямую.
 * Не вызывайте в горячем пути рендера — там всегда есть WorldTransform.
 */
Mat4 computeWorldMatrix(const Registry& reg, Entity e);

/**
 * @brief Зарегистрировать все hierarchy-компоненты в реестре метаданных.
 */
void registerHierarchyComponents();

} // namespace lv::ecs
