/**
 * @file    GraphicsAPI.h
 * @brief   Абстракция графического бэкенда (Vulkan / OpenGL).
 * @ingroup Render
 *
 * @details Зачем RHI, если движок «заточен» под low-poly:
 *          - Vulkan даёт предсказуемое время кадра и explicit sync (нужно для
 *            многопоточной записи команд из JobSystem);
 *          - OpenGL 3.3/ES3 остаётся как fallback для слабых машин и веба
 *            (через Emscripten) — тот же RenderQueue исполняется другим драйвером;
 *          - игровая логика и редактор не содержат ни одного вызова GL/VK.
 *
 *          Интерфейс намеренно узкий: движок не пытается быть универсальным
 *          RHI, он реализует ровно тот конвейер, который нужен low-poly рендеру
 *          (instancing, merged chunks, один directional light + shadow map).
 */
#pragma once

#include "Batcher.h"
#include "RenderTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace lv::render {

/// Доступные бэкенды.
enum class Backend : std::uint8_t { Vulkan, OpenGL, Null };

/// Настройки устройства.
struct DeviceInfo {
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t vramBytes = 0;
    bool supportsInstancing = true;
    bool supportsCompute = true;
};

/// Параметры окна/поверхности.
struct SurfaceDesc {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool vsync = true;
    std::uint8_t msaaSamples = 1;   ///< 1, 2, 4 или 8
    void* nativeWindow = nullptr;   ///< GLFWwindow* / HWND / xcb_window_t
};

/**
 * @brief Камера для одного прохода отрисовки.
 */
struct Camera {
    Mat4  view;
    Mat4  projection;
    Vec3  position;
    Frustum frustum;
    std::uint8_t layerMask = 0xFF;
    Real  nearPlane = 0.1f;
    Real  farPlane = 300.0f;
    bool  orthographic = false;   ///< true для изометрического шаблона
};

/**
 * @brief Статистика кадра (для редакторского Profiler и HUD).
 */
struct FrameStats {
    std::uint32_t drawCalls = 0;
    std::uint32_t instancedCalls = 0;
    std::uint32_t triangles = 0;
    std::uint32_t vertices = 0;
    std::uint32_t batches = 0;
    std::uint64_t vboBytes = 0;
    double cpuMs = 0;
    double gpuMs = 0;
};

/**
 * @brief Интерфейс графического бэкенда.
 *
 * Время жизни: создаётся @c Renderer::init, уничтожается при выходе. Все вызовы
 * — из потока рендера, кроме @c uploadMesh/@c uploadTexture, которые потокобезопасны
 * (очередь загрузки ассетов).
 */
class IGraphicsAPI {
public:
    virtual ~IGraphicsAPI() = default;

    virtual bool init(const SurfaceDesc& surface) = 0;
    virtual void shutdown() = 0;
    virtual void resize(std::uint32_t w, std::uint32_t h) = 0;

    // -- Ресурсы -------------------------------------------------------------
    virtual MeshHandle     uploadMesh(const MeshData& data) = 0;
    virtual MaterialHandle uploadMaterial(const MaterialParams& params) = 0;
    virtual TextureHandle  uploadTexture(std::uint32_t w, std::uint32_t h, std::uint32_t layers,
                                         const void* rgba8) = 0;
    virtual ShaderHandle   compileShader(std::string_view stage, std::string_view source) = 0;
    virtual void           destroyMesh(MeshHandle h) = 0;

    /// Горячая замена программы шейдера (hot-reload .glsl/.spv).
    virtual bool reloadShader(ShaderHandle h, std::string_view source) = 0;

    // -- Кадр ----------------------------------------------------------------
    virtual void beginFrame(const Camera& camera) = 0;
    virtual void submitQueue(const RenderQueue& queue) = 0;
    virtual void endFrame() = 0;

    // -- Отладка -------------------------------------------------------------
    virtual void drawDebugLines(const Vec3* points, std::uint32_t segmentCount, const Color& c) = 0;
    [[nodiscard]] virtual DeviceInfo deviceInfo() const = 0;
    [[nodiscard]] virtual FrameStats lastFrameStats() const = 0;
    [[nodiscard]] virtual Backend backend() const noexcept = 0;
};

/**
 * @brief «Пустой» бэкенд: используется в headless-тестах, CI и dedicated server.
 *
 * Он реально считает статистику кадра (draw calls, треугольники), поэтому
 * интеграционные тесты шаблонов работают без GPU.
 */
class NullGraphicsAPI final : public IGraphicsAPI {
public:
    bool init(const SurfaceDesc&) override { initialized_ = true; return true; }
    void shutdown() override { initialized_ = false; }
    void resize(std::uint32_t w, std::uint32_t h) override { width_ = w; height_ = h; }

    MeshHandle uploadMesh(const MeshData& d) override {
        meshes_.push_back(d);
        stats_.triangles += static_cast<std::uint32_t>(d.indices.size() / 3);
        return MeshHandle{static_cast<std::uint32_t>(meshes_.size() - 1)};
    }
    MaterialHandle uploadMaterial(const MaterialParams&) override {
        return MaterialHandle{materials_++};
    }
    TextureHandle uploadTexture(std::uint32_t, std::uint32_t, std::uint32_t, const void*) override {
        return TextureHandle{textures_++};
    }
    ShaderHandle compileShader(std::string_view, std::string_view) override { return ShaderHandle{shaders_++}; }
    void destroyMesh(MeshHandle) override {}
    bool reloadShader(ShaderHandle, std::string_view) override { return true; }

    void beginFrame(const Camera& c) override { camera_ = c; frameStats_ = FrameStats{}; }
    void submitQueue(const RenderQueue& q) override {
        frameStats_.instancedCalls = static_cast<std::uint32_t>(q.instanced.size());
        frameStats_.batches = static_cast<std::uint32_t>(q.instanced.size());
        frameStats_.drawCalls = static_cast<std::uint32_t>(q.drawCalls());
        for (const InstanceBatch& b : q.instanced) frameStats_.vertices += static_cast<std::uint32_t>(b.transforms.size());
    }
    void endFrame() override { ++frames_; stats_ = frameStats_; }
    void drawDebugLines(const Vec3*, std::uint32_t, const Color&) override {}

    [[nodiscard]] DeviceInfo deviceInfo() const override {
        DeviceInfo d; d.name = "LimVine Null Renderer"; d.supportsInstancing = true; return d;
    }
    [[nodiscard]] FrameStats lastFrameStats() const override { return stats_; }
    [[nodiscard]] Backend backend() const noexcept override { return Backend::Null; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }

private:
    bool initialized_ = false;
    std::uint32_t width_ = 0, height_ = 0;
    std::vector<MeshData> meshes_;
    std::uint32_t materials_ = 0, textures_ = 0, shaders_ = 0;
    Camera camera_;
    FrameStats stats_{}, frameStats_{};
    std::uint64_t frames_ = 0;
};

/**
 * @brief Фабрика бэкенда.
 *
 * Vulkan/OpenGL реализации включаются макросами @c LV_WITH_VULKAN и
 * @c LV_WITH_OPENGL, чтобы движок собирался и без установленных SDK
 * (например, в CI для headless-тестов логики).
 */
std::unique_ptr<IGraphicsAPI> createGraphicsAPI(Backend preferred);

} // namespace lv::render
