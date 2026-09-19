/**
 * @file    World.h
 * @brief   Игровой мир: ECS-реестр, фазы систем, командные буферы, время.
 * @ingroup ECS
 *
 * @details Порядок кадра (fixed + variable):
 * @code
 *  ┌─ Frame ─────────────────────────────────────────────────────────────┐
 *  │ 1. Input       — опрос устройств, генерация событий                  │
 *  │ 2. Script      — LV Script: tick планировщика корутин + Update()     │
 *  │ 3. Logic       — системы фазы Logic (AI, геймплей)                   │
 *  │ 4. Physics     — шаг симуляции (Jolt), запись Transform              │
 *  │ 5. PostPhysics — системы, читающие свежие трансформации              │
 *  │ 6. Render      — culling, батчинг, отрисовка                         │
 *  │ 7. Late        — камеры, UI, очистка                                 │
 *  └─────────────────────────────────────────────────────────────────────┘
 * @endcode
 * После каждой фазы применяется @c CommandBuffer: это единственная точка,
 * где разрешены структурные изменения ECS.
 *
 * Системы регистрируются с явным порядком и зависимостями; планировщик строит
 * из них DAG и выполняет независимые ветви параллельно через @c JobSystem.
 */
#pragma once

#include "CommandBuffer.h"
#include "Registry.h"
#include "../core/JobSystem.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lv::ecs {

/**
 * @brief Фазы кадра. Значения упорядочены — планировщик сортирует по ним.
 */
enum class Phase : std::uint8_t {
    Input,
    Script,
    Logic,
    Physics,
    PostPhysics,
    Render,
    Late,
    Count
};

[[nodiscard]] const char* phaseName(Phase p) noexcept;

class World;   // forward declaration: SystemDesc::update ссылается на World

/**
 * @brief Описание системы.
 *
 * Система — это просто функтор `(World&, float dt)`. Никакого наследования:
 * движок намеренно не вводит «базовый класс системы», чтобы логика игры
 * оставалась данными и функциями, а не иерархией объектов.
 */
struct SystemDesc {
    std::string name;
    Phase       phase = Phase::Logic;
    int         order = 0;                    ///< Порядок внутри фазы.
    std::vector<std::string> after;           ///< Имена систем-предшественников.
    bool        allowParallel = false;        ///< Можно запускать одновременно с другими.
    std::function<void(World&, float)> update;
    bool        enabled = true;
};

/**
 * @brief Контекст кадра, передаваемый системам.
 */
struct FrameInfo {
    float deltaTime = 0.f;        ///< Время кадра (сек).
    float unscaledTime = 0.f;     ///< Накопленное время без учёта timeScale.
    float timeScale = 1.f;
    std::uint64_t frameIndex = 0;
    bool  paused = false;         ///< Пауза: Logic/Script пропускаются, Render работает.
};

/**
 * @brief Игровой мир.
 */
class World {
public:
    World();
    ~World();

    /// Доступ к реестру сущностей.
    [[nodiscard]] Registry& registry() noexcept { return registry_; }
    /// Число живых сущностей (шорткат для @c registry().entityCount()).
    [[nodiscard]] std::size_t entityCount() const noexcept { return registry_.entityCount(); }
    [[nodiscard]] const Registry& registry() const noexcept { return registry_; }

    /// Командный буфер текущей фазы (системы и скрипты пишут в него).
    [[nodiscard]] CommandBuffer& commands() noexcept { return commands_; }

    /// Зарегистрировать систему.
    void addSystem(SystemDesc desc);
    /// Включить/выключить систему по имени.
    void setSystemEnabled(std::string_view name, bool enabled);
    [[nodiscard]] const std::vector<SystemDesc>& systems() const noexcept { return systems_; }

    /// Перестроить порядок выполнения (вызывается автоматически после addSystem).
    void rebuildSchedule();

    /// Один кадр: выполнить все фазы и применить команды.
    void tick(float deltaTime);

    /// Информация о текущем кадре.
    [[nodiscard]] const FrameInfo& frame() const noexcept { return frame_; }
    [[nodiscard]] FrameInfo& frame() noexcept { return frame_; }

    /// Применить отложенные структурные изменения.
    void flushCommands();

    /// Уничтожить все сущности и снять системы (перезагрузка сцены).
    void reset();

    /// Профилирование: время каждой системы за последний кадр (мс).
    [[nodiscard]] const std::unordered_map<std::string, double>& systemTimings() const noexcept {
        return timings_;
    }

    /// Произвольные данные подсистем (рендер-сцена, физический мир и т.д.).
    void  setUserData(void* p) noexcept { userData_ = p; }
    void* userData() const noexcept { return userData_; }

private:
    Registry      registry_;
    CommandBuffer commands_;
    FrameInfo     frame_;
    std::vector<SystemDesc> systems_;
    std::vector<std::vector<std::size_t>> schedule_;   ///< Phase -> индексы систем
    std::unordered_map<std::string, double> timings_;
    void*         userData_ = nullptr;
};

} // namespace lv::ecs
