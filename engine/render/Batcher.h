/**
 * @file    Batcher.h
 * @brief   Culling, сортировка и агрессивный батчинг low-poly сцены.
 * @ingroup Render
 *
 * @details Конвейер подготовки кадра (выполняется в job-потоках):
 * @code
 *   DrawItem'ы из ECS
 *        │  1. frustum culling (параллельно по чанкам)
 *        ▼
 *   видимые DrawItem
 *        │  2. сортировка: pass -> material -> mesh -> depth
 *        ▼
 *   упорядоченный список
 *        │  3. батчинг:
 *        │     - одинаковые (mesh, material) и >= kInstanceThreshold
 *        │       => GPU instancing (один draw call на N объектов);
 *        │     - статическая геометрия уровня => merged chunks (на этапе загрузки);
 *        │     - остальное => одиночные draw call'ы.
 *        ▼
 *   RenderQueue {instanced[], single[], transparent[]}
 * @endcode
 *
 * Цель — удержать число draw call'ов в пределах ~50-150 на всю low-poly сцену
 * из тысяч объектов. Именно поэтому материалы ограничены палитрой: чем меньше
 * различных (mesh, material) пар, тем крупнее батчи.
 */
#pragma once

#include "RenderTypes.h"

#include <cstdint>
#include <vector>

namespace lv::render {

/// Минимальное число одинаковых объектов, чтобы включать GPU instancing.
/// Меньше — накладные расходы на заполнение instance-буфера не окупаются.
inline constexpr std::uint32_t kInstanceThreshold = 4;

/// Максимум инстансов в одном батче (ограничение размера instance-буфера).
inline constexpr std::uint32_t kMaxInstancesPerBatch = 1024;

/// Размер чанка для параллельного culling'а (кратно кэш-линии по 64 байта).
inline constexpr std::size_t kCullChunkSize = 64;

/**
 * @brief Результат подготовки кадра.
 */
struct RenderQueue {
    std::vector<InstanceBatch> instanced;    ///< Батчи GPU instancing.
    std::vector<DrawItem>      single;      ///< Одиночные draw call'ы (opaque).
    std::vector<DrawItem>      transparent; ///< Прозрачные (сортируются по глубине, рисуем сзади вперёд).
    std::vector<DrawItem>      shadows;     ///< Кастеры теней.

    /// Полное число draw call'ов.
    [[nodiscard]] std::size_t drawCalls() const noexcept { return instanced.size() + single.size() + transparent.size() + shadows.size(); }
    /// Число нарисованных объектов (для HUD/профайлера).
    [[nodiscard]] std::size_t objectCount() const noexcept;
};

/**
 * @brief Статистика батчера за кадр.
 */
struct BatchStats {
    std::size_t submitted = 0;
    std::size_t culled = 0;
    std::size_t instancedObjects = 0;
    std::size_t instancedBatches = 0;
    std::size_t singleDraws = 0;
    std::size_t transparentDraws = 0;
    double      cullMs = 0;
    double      sortMs = 0;
    double      batchMs = 0;
};

/**
 * @brief Батчер сцены.
 *
 * Переиспользует память между кадрами (векторы не очищаются, а resize'ятся),
 * поэтому в установившемся режиме аллокаций нет.
 */
class Batcher {
public:
    /// Отфильтровать невидимое. Потокобезопасно при разных экземплярах.
    void cull(const std::vector<DrawItem>& items, const Frustum& frustum, std::vector<DrawItem>& visibleOut);

    /// Полный цикл: cull -> sort -> batch.
    void build(const std::vector<DrawItem>& items, const Frustum& frustum,
               const Vec3& cameraPos, RenderQueue& out);

    /// Только батчинг уже отсортированного списка (для тестов и редактора).
    void batch(const std::vector<DrawItem>& visible, const Vec3& cameraPos, RenderQueue& out);

    /// Статистика последнего build().
    [[nodiscard]] const BatchStats& stats() const noexcept { return stats_; }

    /// Порог инстансинга (настраивается под конкретный GPU).
    void setInstanceThreshold(std::uint32_t v) noexcept { instanceThreshold_ = v; }
    [[nodiscard]] std::uint32_t instanceThreshold() const noexcept { return instanceThreshold_; }

    /// Ключ сортировки: pass, материал, меш, затем глубина (front-to-back для opaque).
    [[nodiscard]] static std::uint64_t sortKey(const DrawItem& d, const Vec3& cameraPos) noexcept;

private:
    BatchStats stats_{};
    std::uint32_t instanceThreshold_ = kInstanceThreshold;
    std::vector<DrawItem> scratch_;
};

} // namespace lv::render
