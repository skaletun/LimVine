/**
 * @file    Renderer.h
 * @brief   Высокоуровневый рендерер: ECS -> DrawItem -> RenderQueue -> backend.
 * @ingroup Render
 *
 * @details Рендерер — это системная функция фазы @c Phase::Render. Он:
 *          1. читает из ECS пары (Transform, MeshRenderer) и строит DrawItem'ы;
 *          2. считает frustum активной камеры;
 *          3. отдаёт всё в @c Batcher (culling + instancing);
 *          4. передаёт @c RenderQueue в @c IGraphicsAPI.
 *
 *          Сбор DrawItem'ов распараллелен через @c JobSystem::parallelFor,
 *          поэтому на сцене в 50k объектов стадия стоит < 1 мс.
 */
#pragma once

#include "Batcher.h"
#include "GraphicsAPI.h"
#include "../ecs/World.h"

#include <memory>

namespace lv::render {

/**
 * @brief Компонент «видимый меш» (хранится в ECS).
 */
struct MeshRenderer {
    MeshHandle     mesh;
    MaterialHandle material;
    Color          tint{1, 1, 1, 1};
    std::uint8_t   layer = 0;
    bool           castShadows = true;
    bool           visible = true;
    AABB           localBounds{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    static constexpr std::string_view lv_component_name = "MeshRenderer";
};

/**
 * @brief Рендерер сцены.
 */
class Renderer {
public:
    Renderer();
    ~Renderer();

    /// Инициализация бэкенда. Возвращает false, если устройство создать не удалось.
    bool init(Backend preferred, const SurfaceDesc& surface);
    void shutdown();

    /// Подключить к миру (регистрирует систему в Phase::Render).
    void attachToWorld(ecs::World& world);

    /// Установить активную камеру.
    void setCamera(const Camera& cam) noexcept { camera_ = cam; }
    [[nodiscard]] const Camera& camera() const noexcept { return camera_; }

    /// Пересчитать frustum из матриц камеры (вызывается после setCamera).
    void updateFrustum() noexcept;

    /// Собрать DrawItem'ы из мира (публично для тестов и редакторского preview).
    void gatherDrawItems(ecs::World& world, std::vector<DrawItem>& out);

    /// Один кадр: gather -> batch -> submit.
    void renderFrame(ecs::World& world);

    [[nodiscard]] IGraphicsAPI* api() noexcept { return api_.get(); }
    [[nodiscard]] const BatchStats& batchStats() const noexcept { return batcher_.stats(); }
    [[nodiscard]] FrameStats lastFrameStats() const { return api_ ? api_->lastFrameStats() : FrameStats{}; }

    /// Отладочная отрисовка (гизмо редактора, хитбоксы физики).
    void drawDebugLines(const Vec3* pts, std::uint32_t segments, const Color& c);

private:
    std::unique_ptr<IGraphicsAPI> api_;
    Batcher batcher_;
    Camera  camera_;
    std::vector<DrawItem> items_;
    RenderQueue queue_;
    bool  initialized_ = false;
};

} // namespace lv::render
