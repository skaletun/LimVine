/**
 * @file    Batcher.cpp
 * @brief   Реализация culling/сортировки/батчинга.
 */
#include "Batcher.h"
#include "../core/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace lv::render {

namespace {
struct Key {
    std::uint32_t mesh;
    std::uint32_t material;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept {
        return (static_cast<std::size_t>(k.mesh) * 0x9E3779B97F4A7C15ULL) ^ k.material;
    }
};
} // namespace

std::size_t RenderQueue::objectCount() const noexcept {
    std::size_t n = single.size() + transparent.size() + shadows.size();
    for (const InstanceBatch& b : instanced) n += b.transforms.size();
    return n;
}

std::uint64_t Batcher::sortKey(const DrawItem& d, const Vec3& cameraPos) noexcept {
    // Ключ собираем в один uint64: старшие биты — pass и материал, младшие —
    // квантованная глубина. Это даёт ОДНО сравнение на элемент при сортировке
    // вместо лексикографического сравнения нескольких полей.
    const Real dist = distanceSq(d.worldBounds.center(), cameraPos);
    const std::uint32_t quant = static_cast<std::uint32_t>(std::min<Real>(dist, 65535.0f));
    return (static_cast<std::uint64_t>(d.pass) << 56) |
           (static_cast<std::uint64_t>(d.material.index & 0xFFFF) << 40) |
           (static_cast<std::uint64_t>(d.mesh.index & 0xFFFF) << 24) |
           (static_cast<std::uint64_t>(quant) << 8);
}

void Batcher::cull(const std::vector<DrawItem>& items, const Frustum& frustum,
                   std::vector<DrawItem>& visibleOut) {
    visibleOut.clear();
    if (items.empty()) return;

    // Маска видимости заполняется параллельно, затем компактируем одним проходом:
    // так избегаем общей блокировки на push_back из нескольких потоков.
    std::vector<std::uint8_t> visible(items.size(), 0);
    if (JobSystem::ready() && items.size() > kCullChunkSize * 2) {
        JobSystem::instance().parallelForRange(items.size(), kCullChunkSize,
            [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                    visible[i] = frustum.intersects(items[i].worldBounds) ? 1 : 0;
            });
    } else {
        for (std::size_t i = 0; i < items.size(); ++i)
            visible[i] = frustum.intersects(items[i].worldBounds) ? 1 : 0;
    }
    visibleOut.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
        if (visible[i]) visibleOut.push_back(items[i]);
}

void Batcher::batch(const std::vector<DrawItem>& visible, const Vec3& cameraPos, RenderQueue& out) {
    out.instanced.clear();
    out.single.clear();
    out.transparent.clear();
    out.shadows.clear();

    // Группируем по (mesh, material) внутри каждого pass'а.
    std::unordered_map<Key, std::vector<std::size_t>, KeyHash> groups;
    groups.reserve(visible.size() / 2 + 8);
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const DrawItem& d = visible[i];
        if (d.pass == 2) { out.shadows.push_back(d); continue; }
        groups[Key{d.mesh.index, d.material.index}].push_back(i);
    }

    for (auto& [key, indices] : groups) {
        const DrawItem& first = visible[indices.front()];
        if (indices.size() >= instanceThreshold_) {
            // GPU instancing: разбиваем на батчи по kMaxInstancesPerBatch.
            for (std::size_t begin = 0; begin < indices.size(); begin += kMaxInstancesPerBatch) {
                const std::size_t end = std::min(indices.size(), begin + static_cast<std::size_t>(kMaxInstancesPerBatch));
                InstanceBatch b;
                b.mesh = first.mesh;
                b.material = first.material;
                b.pass = first.pass;
                b.transforms.reserve(end - begin);
                b.tints.reserve(end - begin);
                for (std::size_t i = begin; i < end; ++i) {
                    b.transforms.push_back(visible[indices[i]].world);
                    b.tints.push_back(visible[indices[i]].tint);
                }
                out.instanced.push_back(std::move(b));
            }
            stats_.instancedObjects += indices.size();
            stats_.instancedBatches += (indices.size() + kMaxInstancesPerBatch - 1) / kMaxInstancesPerBatch;
        } else {
            for (std::size_t i : indices) {
                if (first.pass == 1) out.transparent.push_back(visible[i]);
                else out.single.push_back(visible[i]);
            }
        }
    }

    // Прозрачные рисуем от дальних к ближним, opaque — наоборот (early-z).
    std::sort(out.transparent.begin(), out.transparent.end(), [&](const DrawItem& a, const DrawItem& b) {
        return distanceSq(a.worldBounds.center(), cameraPos) > distanceSq(b.worldBounds.center(), cameraPos);
    });
    std::sort(out.single.begin(), out.single.end(), [&](const DrawItem& a, const DrawItem& b) {
        return distanceSq(a.worldBounds.center(), cameraPos) < distanceSq(b.worldBounds.center(), cameraPos);
    });
    stats_.singleDraws = out.single.size();
    stats_.transparentDraws = out.transparent.size();
}

void Batcher::build(const std::vector<DrawItem>& items, const Frustum& frustum,
                    const Vec3& cameraPos, RenderQueue& out) {
    stats_ = BatchStats{};
    stats_.submitted = items.size();

    auto t0 = std::chrono::steady_clock::now();
    cull(items, frustum, scratch_);
    auto t1 = std::chrono::steady_clock::now();
    stats_.culled = items.size() - scratch_.size();
    stats_.cullMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::sort(scratch_.begin(), scratch_.end(), [&](const DrawItem& a, const DrawItem& b) {
        return sortKey(a, cameraPos) < sortKey(b, cameraPos);
    });
    auto t2 = std::chrono::steady_clock::now();
    stats_.sortMs = std::chrono::duration<double, std::milli>(t2 - t1).count();

    batch(scratch_, cameraPos, out);
    stats_.batchMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
}

} // namespace lv::render
