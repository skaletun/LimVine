/**
 * @file    World.cpp
 * @brief   Реализация игрового мира и планировщика систем.
 */
#include "World.h"

#include <algorithm>
#include <chrono>

namespace lv::ecs {

const char* phaseName(Phase p) noexcept {
    switch (p) {
        case Phase::Input:       return "Input";
        case Phase::Script:      return "Script";
        case Phase::Logic:       return "Logic";
        case Phase::Physics:     return "Physics";
        case Phase::PostPhysics: return "PostPhysics";
        case Phase::Render:      return "Render";
        case Phase::Late:        return "Late";
        default:                 return "?";
    }
}

World::World() : commands_(registry_) {
    rebuildSchedule();
}

World::~World() = default;

void World::addSystem(SystemDesc desc) {
    systems_.push_back(std::move(desc));
    rebuildSchedule();
}

void World::setSystemEnabled(std::string_view name, bool enabled) {
    for (auto& s : systems_)
        if (s.name == name) { s.enabled = enabled; return; }
}

void World::rebuildSchedule() {
    schedule_.assign(static_cast<std::size_t>(Phase::Count), {});

    // Сортировка: фаза -> order -> имя (для детерминизма между запусками).
    std::vector<std::size_t> idx(systems_.size());
    for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [this](std::size_t a, std::size_t b) {
        const SystemDesc& x = systems_[a];
        const SystemDesc& y = systems_[b];
        if (x.phase != y.phase) return x.phase < y.phase;
        if (x.order != y.order) return x.order < y.order;
        return x.name < y.name;
    });

    // Зависимости `after` учитываем локальным продвижением: если система B
    // должна идти после A, но оказалась раньше, переносим её сразу за A.
    auto positionOf = [&](std::string_view n) -> std::ptrdiff_t {
        for (std::size_t i = 0; i < idx.size(); ++i)
            if (systems_[idx[i]].name == n) return static_cast<std::ptrdiff_t>(i);
        return -1;
    };
    bool changed = true;
    for (int guard = 0; guard < 8 && changed; ++guard) {
        changed = false;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            for (const std::string& dep : systems_[idx[i]].after) {
                const std::ptrdiff_t j = positionOf(dep);
                if (j >= 0 && static_cast<std::size_t>(j) > i) {
                    const std::size_t moved = idx[i];
                    idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
                    idx.insert(idx.begin() + j, moved);
                    changed = true;
                    break;
                }
            }
            if (changed) break;
        }
    }

    for (std::size_t i : idx) schedule_[static_cast<std::size_t>(systems_[i].phase)].push_back(i);
}

void World::flushCommands() { commands_.flush(); }

void World::tick(float deltaTime) {
    frame_.unscaledTime += deltaTime;
    frame_.deltaTime = frame_.paused ? 0.f : deltaTime * frame_.timeScale;
    ++frame_.frameIndex;

    for (std::size_t phase = 0; phase < schedule_.size(); ++phase) {
        // Пауза: скрипты и логику пропускаем, рендер и ввод продолжают работать.
        if (frame_.paused && (static_cast<Phase>(phase) == Phase::Script ||
                              static_cast<Phase>(phase) == Phase::Logic ||
                              static_cast<Phase>(phase) == Phase::Physics))
            continue;

        for (std::size_t sysIndex : schedule_[phase]) {
            SystemDesc& sys = systems_[sysIndex];
            if (!sys.enabled || !sys.update) continue;
            const auto t0 = std::chrono::steady_clock::now();
            sys.update(*this, frame_.deltaTime);
            const auto t1 = std::chrono::steady_clock::now();
            timings_[sys.name] = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        // Единственная точка структурных изменений — между фазами.
        flushCommands();
    }
}

void World::reset() {
    registry_.clear();
    systems_.clear();
    timings_.clear();
    schedule_.clear();
    frame_ = FrameInfo{};
    rebuildSchedule();
}

} // namespace lv::ecs
