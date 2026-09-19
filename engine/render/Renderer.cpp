/**
 * @file    Renderer.cpp
 * @brief   Реализация рендерера сцены.
 */
#include "Renderer.h"
#include "../core/JobSystem.h"

namespace lv::render {

Renderer::Renderer() {
    camera_.projection = Mat4::perspective(radians(60.0f), 16.0f / 9.0f, 0.1f, 300.0f);
    camera_.view = Mat4::lookAt({0, 5, 10}, {0, 0, 0}, Vec3::up());
    camera_.position = {0, 5, 10};
    updateFrustum();
}

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(Backend preferred, const SurfaceDesc& surface) {
    api_ = createGraphicsAPI(preferred);
    if (!api_) return false;
    initialized_ = api_->init(surface);
    return initialized_;
}

void Renderer::shutdown() {
    if (api_ && initialized_) api_->shutdown();
    api_.reset();
    initialized_ = false;
}

void Renderer::updateFrustum() noexcept {
    camera_.frustum = Frustum::fromViewProjection(camera_.projection * camera_.view);
}

void Renderer::gatherDrawItems(ecs::World& world, std::vector<DrawItem>& out) {
    auto& reg = world.registry();
    out.clear();

    // Первый проход: считаем количество, чтобы зарезервировать память одним куском.
    std::size_t count = 0;
    reg.each<ecs::Transform, MeshRenderer>([&](ecs::Entity, ecs::Transform&, MeshRenderer& mr) {
        if (mr.visible) ++count;
        return true;
    });
    out.reserve(count);

    reg.each<ecs::Transform, MeshRenderer>([&](ecs::Entity e, ecs::Transform& t, MeshRenderer& mr) {
        if (!mr.visible) return true;
        DrawItem d;
        d.mesh = mr.mesh;
        d.material = mr.material;
        d.world = t.matrix();
        // Мировой AABB: трансформируем углы локального бокса (дёшево и достаточно
        // точно для culling'а — вращение учитывается через описанный бокс).
        const Vec3 c = t.position + lv::rotate(t.rotation, mr.localBounds.center());
        const Vec3 ext = abs(t.scale) * mr.localBounds.extents();
        d.worldBounds = AABB{c - ext, c + ext};
        d.tint = mr.tint;
        d.entityId = e.index;
        d.layer = mr.layer;
        d.pass = (mr.tint.a < 0.999f) ? 1 : 0;
        out.push_back(d);

        if (mr.castShadows) {
            DrawItem s = d;
            s.pass = 2;
            out.push_back(s);
        }
        return true;
    });
}

void Renderer::renderFrame(ecs::World& world) {
    if (!api_ || !initialized_) return;
    gatherDrawItems(world, items_);
    batcher_.build(items_, camera_.frustum, camera_.position, queue_);
    api_->beginFrame(camera_);
    api_->submitQueue(queue_);
    api_->endFrame();
}

void Renderer::attachToWorld(ecs::World& world) {
    ecs::SystemDesc sys;
    sys.name = "Renderer";
    sys.phase = ecs::Phase::Render;
    sys.order = 0;
    sys.allowParallel = false;
    sys.update = [this](ecs::World& w, float) { renderFrame(w); };
    world.addSystem(std::move(sys));
}

void Renderer::drawDebugLines(const Vec3* pts, std::uint32_t segments, const Color& c) {
    if (api_ && initialized_) api_->drawDebugLines(pts, segments, c);
}

} // namespace lv::render
