/**
 * @file    GraphicsAPI.cpp
 * @brief   Фабрика бэкенда и (при включённых макросах) точки входа Vulkan/OpenGL.
 */
#include "GraphicsAPI.h"

#if defined(LV_WITH_VULKAN)
#include "backends/VulkanBackend.h"
#endif
#if defined(LV_WITH_OPENGL)
#include "backends/GLBackend.h"
#endif

namespace lv::render {

std::unique_ptr<IGraphicsAPI> createGraphicsAPI(Backend preferred) {
#if defined(LV_WITH_VULKAN)
    if (preferred == Backend::Vulkan) {
        auto api = std::make_unique<VulkanBackend>();
        if (api->probeAvailable()) return api;
        // Vulkan недоступен (нет драйвера/слоёв) — падаем на OpenGL.
    }
#endif
#if defined(LV_WITH_OPENGL)
    if (preferred == Backend::Vulkan || preferred == Backend::OpenGL)
        return std::make_unique<GLBackend>();
#endif
    // Headless / CI / dedicated server.
    return std::make_unique<NullGraphicsAPI>();
}

} // namespace lv::render
