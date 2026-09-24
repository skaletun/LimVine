/**
 * @file    Scene.h
 * @brief   Сохранение и загрузка сцен в формате .lvscene (JSON).
 * @ingroup Scene
 *
 * @details Формат `.lvscene` — это JSON-снимок ECS-мира. Он НЕ содержит
 *          C++-специфичных смещений и не зависит от порядка регистрации
 *          компонентов: каждый компонент записывается по своему скриптовому
 *          имени (`"Transform"`, `"MeshRenderer"`, ...), а его поля — по именам
 *          из @c scripting::BindingRegistry. Это тот же источник истины, что
 *          используют инспектор редактора и `getField()/setField()` в LV Script,
 *          поэтому «что видно в инспекторе — то и сохраняется».
 *
 *          Ключевые свойства формата:
 *
 *          - **Стабильные ссылки на сущности.** Handle `Entity{index, generation}`
 *            зависит от истории аллокаций и при загрузке будет другим. Поэтому
 *            в файл пишется не handle, а плотный порядковый номер (`"id"`), а
 *            поля вида @c FieldBinding::Kind::Entity и родительские ссылки
 *            сохраняются как этот номер. При загрузке строится таблица
 *            «номер -> новый Entity», и вторым проходом ссылки чинятся.
 *
 *          - **Иерархия.** Сохраняется только `Parent` (как `"parent": id`);
 *            `Children` и `WorldTransform` — производные данные и
 *            восстанавливаются вызовом @c ecs::setParent + пересчётом
 *            world-матриц, а не читаются из файла. Это исключает класс багов
 *            «файл с рассогласованными Parent/Children»: рассогласовать
 *            нечего, потому что в файле нет второй копии этих данных.
 *
 *          - **Толерантность к эволюции схемы.** Неизвестный компонент или
 *            неизвестное поле не роняют загрузку: они попадают в
 *            @c SceneLoadResult::warnings. Сцена, сохранённая новой версией
 *            движка, грузится старой с потерей только новых данных.
 *
 *          - **Детерминированность.** Компоненты внутри сущности и сущности
 *            внутри файла пишутся в стабильном порядке (по имени и по id), так
 *            что сохранение -> загрузка -> сохранение даёт побайтово
 *            идентичный файл. Это делает .lvscene дружественным к git-diff и
 *            позволяет тестам сравнивать снимки строками.
 *
 * Пример файла:
 * @code{.json}
 * {
 *   "format": "lvscene",
 *   "version": 1,
 *   "entities": [
 *     { "id": 0, "name": "Player",
 *       "components": {
 *         "Transform": { "position": {"x":1,"y":0,"z":2}, "scale": {"x":1,"y":1,"z":1} }
 *       } },
 *     { "id": 1, "name": "Sword", "parent": 0, "components": { ... } }
 *   ]
 * }
 * @endcode
 */
#pragma once

#include "ecs/World.h"

#include <string>
#include <vector>

namespace lv::scene {

/// Текущая версия формата .lvscene.
inline constexpr int kSceneFormatVersion = 1;

/**
 * @brief Настройки сохранения.
 */
struct SaveOptions {
    /// Отступ pretty-print (-1 — компактно, в одну строку).
    ///
    /// Значение по умолчанию (2) рассчитано на сцены под контролем версий:
    /// однострочный файл даёт нечитаемый diff. Для рантайм-сейвов, где важен
    /// размер, ставьте -1.
    int indent = 2;
};

/**
 * @brief Результат загрузки сцены.
 */
struct SceneLoadResult {
    bool ok = false;
    /// Фатальная ошибка (битый JSON, неверный формат). Пусто при @c ok.
    std::string error;
    /// Некритичные проблемы: неизвестные компоненты/поля, висячие ссылки.
    std::vector<std::string> warnings;
    /// Сущности в порядке их `id` в файле (индекс == id).
    std::vector<ecs::Entity> entities;

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }
};

/**
 * @brief Имя сущности — единственный «редакторский» компонент в ядре.
 *
 * Хранится в ECS, а не в отдельной карте, чтобы имя переживало сериализацию,
 * копирование префабов и удаление сущности без специального кода очистки.
 */
struct Name {
    std::string value;
    static constexpr std::string_view lv_component_name = "Name";
};

/// Зарегистрировать @c Name в реестрах метаданных и биндингов.
void registerSceneComponents();

/// Установить/получить имя сущности (удобные обёртки над компонентом @c Name).
void        setEntityName(ecs::Registry& reg, ecs::Entity e, std::string name);
std::string entityName(const ecs::Registry& reg, ecs::Entity e);

// ---------------------------------------------------------------------------
//  Сериализация
// ---------------------------------------------------------------------------

/// Сериализовать мир в строку .lvscene.
[[nodiscard]] std::string saveSceneToString(const ecs::World& world, const SaveOptions& opts = {});

/// Сохранить мир в файл. Запись атомарная (tmp + rename).
[[nodiscard]] bool saveSceneToFile(const ecs::World& world, const std::string& path,
                                   const SaveOptions& opts = {}, std::string* error = nullptr);

/**
 * @brief Загрузить сцену из строки в мир.
 *
 * @param world  Целевой мир. Существующие сущности НЕ удаляются: сцена
 *               догружается поверх (это позволяет стримить уровни по частям).
 *               Чтобы заменить содержимое, вызовите @c world.registry().clear().
 */
[[nodiscard]] SceneLoadResult loadSceneFromString(ecs::World& world, std::string_view text);

/// Загрузить сцену из файла.
[[nodiscard]] SceneLoadResult loadSceneFromFile(ecs::World& world, const std::string& path);

} // namespace lv::scene
