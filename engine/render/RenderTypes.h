/**
 * @file    RenderTypes.h
 * @brief   Бэкенд-независимые типы рендера: handle'ы, меш, материал, батч.
 * @ingroup Render
 *
 * @details Всё, что передаётся между игровой логикой и рендерером, выражено
 *          в POD-handle'ах (MeshHandle, MaterialHandle, ...). Логика никогда не
 *          держит указателей на GPU-ресурсы, поэтому:
 *          - потоки логики и рендера не разделяют память;
 *          - hot-reload шейдера/меша не инвалидирует игровые объекты;
 *          - сериализация сцены тривиальна (handle = индекс).
 */
#pragma once

#include "../core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lv::render {

/// Типовой «сильный» handle: индекс в таблице ресурсов.
template <class Tag>
struct Handle {
    std::uint32_t index = 0xFFFFFFFFu;
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0xFFFFFFFFu; }
    [[nodiscard]] constexpr auto operator<=>(const Handle&) const = default;
};

struct MeshTag; struct MaterialTag; struct TextureTag; struct ShaderTag; struct PipelineTag;
using MeshHandle     = Handle<MeshTag>;
using MaterialHandle = Handle<MaterialTag>;
using TextureHandle  = Handle<TextureTag>;
using ShaderHandle   = Handle<ShaderTag>;
using PipelineHandle = Handle<PipelineTag>;

inline constexpr MeshHandle     kInvalidMesh{};
inline constexpr MaterialHandle kInvalidMaterial{};

/**
 * @brief Формат вершины low-poly меша.
 *
 * Ключевое отличие от «взрослого» PBR-конвейера: вместо UV + нормалей +
 * тангенсов + нескольких наборов цветов мы держим ОДИН интерполируемый цвет
 * и нормаль. Это:
 *  - 64 байта на вершину с учётом SIMD-выравнивания (против 96-128 у PBR-меша
 *    с тангенсами, двумя UV-наборами и костными весами);
 *  - позволяет рисовать всю low-poly сцену одним pipeline'ом;
 *  - даёт художнику палитру вместо текстур — стиль движка.
 */
struct Vertex {
    Vec3   position;    ///< 16 байт (Vec3 дополнен w для SIMD-выравнивания).
    Vec3   normal;      ///< 16 байт.
    Color  color;       ///< 16 байт: вершинный цвет вместо альбедо-текстуры.
    Vec2   uv;          ///< 8 байт + 8 байт выравнивания до 16.
};
static_assert(sizeof(Vertex) == 64, "Vertex layout is part of the asset format ABI");
static_assert(alignof(Vertex) == 16, "Vertex must be 16-byte aligned for SIMD uploads");

/**
 * @brief Индексный меш. Индексы 32-битные — low-poly сцены редко требуют
 *        больше, а единый формат упрощает батчинг.
 */
struct MeshData {
    std::string        name;
    std::vector<Vertex>   vertices;
    std::vector<std::uint32_t> indices;
    AABB               bounds;
    bool               flatShaded = true;  ///< Не сглаживать нормали (low-poly look).
};

/**
 * @brief Материал low-poly: один шейдер + базовые параметры.
 *
 * Никаких PBR-карт по умолчанию. @c paletteIndex поддерживает режим
 * «палитровая текстура»: все меши сцены ссылаются на одну 256-цветную
 * текстуру 16x16, поэтому переключение материалов не ломает батчинг.
 */
struct MaterialParams {
    Color  albedo{1, 1, 1, 1};
    Real   roughness = 0.9f;     ///< Только для «дорогих» материалов.
    Real   emissive = 0.0f;
    Real   alphaCutoff = 0.0f;   ///< >0 включает alpha-test (листва, решётки).
    std::uint32_t paletteIndex = 0;
    ShaderHandle shader;
    bool   receiveShadows = true;
    bool   castShadows = true;
};

/**
 * @brief Команда отрисовки, которую собирает culling и потребляет батчер.
 */
struct DrawItem {
    MeshHandle     mesh;
    MaterialHandle material;
    Mat4           world;
    AABB           worldBounds;
    Color          tint{1, 1, 1, 1};
    std::uint32_t  entityId = 0;   ///< Для picking'а и сортировки.
    std::uint8_t   layer = 0;      ///< Слой камеры (scene/overlay/gizmo).
    std::uint8_t   pass = 0;       ///< 0=opaque, 1=alpha, 2=shadow.
};

/**
 * @brief GPU instancing-батч: один меш+материал и массив трансформаций.
 *
 * Это основная оптимизация движка: трава, камни, заборы, толпа NPC — тысячи
 * идентичных мешей, которые уходят одним draw call'ом.
 */
struct InstanceBatch {
    MeshHandle     mesh;
    MaterialHandle material;
    std::vector<Mat4>  transforms;
    std::vector<Color> tints;
    std::uint8_t   pass = 0;
};

/**
 * @brief Статически объединённый меш (merged geometry).
 *
 * Для НЕподвижной геометрии уровня (земля, здания) инстансинг не подходит —
 * меши разные. Вместо них на этапе загрузки ассетов геометрия сливается в
 * один VBO по материалу, и весь уровень рисуется десятком draw call'ов.
 */
struct MergedChunk {
    MaterialHandle   material;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    AABB             bounds;
    std::uint32_t    chunkX = 0, chunkZ = 0;   ///< Координаты чанка сетки уровня.
};

/// Плоскость для frustum culling (нормаль + смещение).
struct Plane {
    Vec3 normal;
    Real offset = 0;
    [[nodiscard]] constexpr Real distanceTo(const Vec3& p) const noexcept { return dot(normal, p) + offset; }
};

/**
 * @brief Усечённая пирамида камеры: 6 плоскостей в мировом пространстве.
 */
struct Frustum {
    Plane planes[6];   ///< left, right, bottom, top, near, far

    /// Извлечь плоскости из view-projection матрицы (метод Гритца-Зурawски).
    static Frustum fromViewProjection(const Mat4& vp) noexcept;
    [[nodiscard]] bool intersects(const AABB& box) const noexcept;
    [[nodiscard]] bool contains(const Vec3& p) const noexcept;
};

} // namespace lv::render
