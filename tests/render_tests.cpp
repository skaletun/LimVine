/**
 * @file    render_tests.cpp
 * @brief   Проверки frustum culling и батчинга low-poly сцены.
 */
#include "render/Batcher.h"
#include "core/JobSystem.h"

#include <cstdio>
#include <random>

using namespace lv;
using namespace lv::render;

namespace {
int g_passed = 0, g_failed = 0;
void check(bool cond, const char* what) {
    if (cond) { ++g_passed; std::printf("  PASS  %s\n", what); }
    else      { ++g_failed; std::printf("  FAIL  %s\n", what); }
}

DrawItem makeItem(std::uint32_t mesh, std::uint32_t mat, const Vec3& pos, std::uint8_t pass = 0) {
    DrawItem d;
    d.mesh.index = mesh;
    d.material.index = mat;
    d.world = Mat4::translation(pos);
    d.worldBounds = AABB{pos - Vec3{0.5f, 0.5f, 0.5f}, pos + Vec3{0.5f, 0.5f, 0.5f}};
    d.entityId = mesh * 1000 + mat;
    d.pass = pass;
    return d;
}
} // namespace

int main() {
    JobSystem::init(0);
    std::printf("\n== Frustum culling ==\n");
    {
        const Mat4 proj = Mat4::perspective(radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
        const Mat4 view = Mat4::lookAt({0, 5, 20}, {0, 0, 0}, Vec3::up());
        const Frustum f = Frustum::fromViewProjection(proj * view);

        check(f.contains({0, 0, 0}), "origin is inside the frustum");
        check(!f.contains({0, 0, 60}), "point behind the camera is outside");
        check(!f.contains({500, 0, 0}), "point far to the right is outside");
        check(f.intersects(AABB{{-1, -1, -1}, {1, 1, 1}}), "box at origin intersects");
        check(!f.intersects(AABB{{0, 300, 0}, {1, 301, 1}}), "box high above does not intersect");
    }

    std::printf("\n== Batching (low-poly optimisation) ==\n");
    {
        Batcher batcher;
        RenderQueue q;
        std::vector<DrawItem> items;
        std::mt19937 rng(42);
        std::uniform_real_distribution<Real> dx(-40.f, 40.f), dy(0.f, 6.f), dz(-40.f, 10.f);

        // 4000 одинаковых «деревьев» (mesh 7, material 2) -> должны стать
        // несколькими instanced-батчами вместо 4000 draw call'ов.
        for (int i = 0; i < 4000; ++i)
            items.push_back(makeItem(7, 2, {dx(rng), dy(rng), dz(rng)}));
        // 30 уникальных «зданий» — по одному draw call'у каждое.
        for (int i = 0; i < 30; ++i)
            items.push_back(makeItem(100 + static_cast<std::uint32_t>(i), 3, {dx(rng), dy(rng), dz(rng)}));
        // 10 прозрачных объектов.
        for (int i = 0; i < 10; ++i)
            items.push_back(makeItem(200, 9, {dx(rng), dy(rng), dz(rng)}, 1));
        // 500 кастеров теней.
        for (int i = 0; i < 500; ++i)
            items.push_back(makeItem(7, 2, {dx(rng), dy(rng), dz(rng)}, 2));

        const Mat4 proj = Mat4::perspective(radians(60.0f), 16.0f / 9.0f, 0.1f, 200.0f);
        const Mat4 view = Mat4::lookAt({0, 5, 20}, {0, 0, 0}, Vec3::up());
        batcher.build(items, Frustum::fromViewProjection(proj * view), {0, 5, 20}, q);

        const BatchStats& st = batcher.stats();
        std::printf("  submitted=%zu culled=%zu visible=%zu drawCalls=%zu instancedBatches=%zu instancedObjects=%zu\n",
                    st.submitted, st.culled, q.objectCount(), q.drawCalls(), st.instancedBatches, st.instancedObjects);
        std::printf("  cull=%.2f ms sort=%.2f ms batch=%.2f ms\n", st.cullMs, st.sortMs, st.batchMs);

        check(st.submitted == items.size(), "all items submitted");
        check(st.culled > 0, "frustum culling removed off-screen objects");
        check(q.instanced.size() > 0, "instanced batches were produced");
        check(q.instanced.size() <= 8, "4000 identical meshes collapse into <= 8 batches");
        check(st.instancedObjects >= 1000, "at least 1000 objects went through instancing");
        check(q.drawCalls() < items.size() / 10,
              "draw calls are at least 10x fewer than submitted objects");
        check(q.transparent.size() <= 10, "transparent items are separated");
        check(!q.shadows.empty(), "shadow pass items are separated");
    }

    std::printf("\n== Sort key ==\n");
    {
        const DrawItem a = makeItem(1, 1, {0, 0, 0}, 0);
        const DrawItem b = makeItem(1, 1, {0, 0, 0}, 1);
        check(Batcher::sortKey(a, {0, 0, 0}) < Batcher::sortKey(b, {0, 0, 0}),
              "opaque pass sorts before transparent");
        const DrawItem near_ = makeItem(1, 5, {1, 0, 0});
        const DrawItem far_  = makeItem(1, 5, {90, 0, 0});
        check(Batcher::sortKey(near_, {0, 0, 0}) < Batcher::sortKey(far_, {0, 0, 0}),
              "same material+mesh sorts front-to-back for early-z");
    }

    JobSystem::shutdown();
    std::printf("\n----------------------------------------\n");
    std::printf("Render self-test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
