/**
 * @file    PhysicsBackend.cpp
 * @brief   Фабрика физических бэкендов — единственное место с #ifdef LV_WITH_JOLT
 *          вне самого JoltBackend.cpp.
 */
#include "PhysicsBackend.h"

#include "BuiltinBackend.h"

#if defined(LV_WITH_JOLT)
#include "JoltBackend.h"
#endif

namespace lv::physics {

bool joltAvailable() noexcept {
#if defined(LV_WITH_JOLT)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<IPhysicsBackend> createPhysicsBackend(bool preferJolt) {
#if defined(LV_WITH_JOLT)
    if (preferJolt) return std::make_unique<JoltBackend>();
#else
    // Запрос Jolt без поддержки в сборке — не ошибка: игра обязана
    // запускаться и на встроенном симуляторе, пусть и с более простой физикой.
    (void)preferJolt;
#endif
    return std::make_unique<BuiltinBackend>();
}

} // namespace lv::physics
