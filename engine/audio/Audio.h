/**
 * @file    Audio.h
 * @brief   Звуковая подсистема (OpenAL Soft / null-backend).
 * @ingroup Audio
 *
 * @details Для low-poly игры не нужен полноценный интерактивный саунд-дизайн
 *          уровня AAA, поэтому подсистема сознательно узкая:
 *          - пул голосов (32 по умолчанию) с приоритетами и «украдкой» голоса;
 *          - 2D (UI/музыка) и 3D (позиционные) источники;
 *          - одна шина SFX + одна шина Music с независимой громкостью;
 *          - стриминг длинных треков отдельными буферами (не держим WAV в памяти).
 *
 *          @c LV_WITH_OPENAL выключен => работает NullAudioBackend, который
 *          ведёт полную статистику (играющие голоса, позиции) — на нём пишутся
 *          тесты и работает dedicated server.
 */
#pragma once

#include "../core/Math.h"
#include "../ecs/World.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lv::audio {

struct ClipHandle { std::uint32_t index = 0xFFFFFFFFu; [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFu; } };
struct VoiceHandle { std::uint32_t index = 0xFFFFFFFFu; [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFu; } };
inline constexpr ClipHandle kInvalidClip{};
inline constexpr VoiceHandle kInvalidVoice{};

enum class Bus : std::uint8_t { Sfx, Music, Voice, Count };

/// Декодированный клип (PCM16, моно или стерео).
struct AudioClip {
    std::string name;
    std::uint32_t sampleRate = 44100;
    std::uint16_t channels = 1;
    std::vector<std::int16_t> samples;
    Real durationSeconds = 0;
    bool stream = false;   ///< длинные треки не держим в памяти целиком
};

/// Параметры воспроизведения.
struct PlayParams {
    ClipHandle clip;
    Bus        bus = Bus::Sfx;
    Real       volume = 1.0f;
    Real       pitch = 1.0f;
    bool       loop = false;
    bool       positional = false;
    Vec3       position;
    Real       minDistance = 1.0f;
    Real       maxDistance = 40.0f;
    std::uint8_t priority = 128;   ///< 0 = тише всех, 255 = нельзя вытеснить
    ecs::Entity follow = ecs::kNullEntity;  ///< источник следует за сущностью
};

/**
 * @brief Интерфейс звукового бэкенда.
 */
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual bool init(std::uint32_t maxVoices) = 0;
    virtual void shutdown() = 0;
    virtual ClipHandle loadClip(const AudioClip& clip) = 0;
    virtual void unloadClip(ClipHandle c) = 0;
    virtual VoiceHandle play(const PlayParams& p) = 0;
    virtual void stop(VoiceHandle v) = 0;
    virtual void stopAll() = 0;
    virtual void setVoicePosition(VoiceHandle v, const Vec3& p) = 0;
    virtual void setVolume(VoiceHandle v, Real vol) = 0;
    virtual void setListener(const Vec3& position, const Vec3& forward, const Vec3& up) = 0;
    virtual void setBusVolume(Bus b, Real vol) = 0;
    virtual void update(float dt) = 0;
    [[nodiscard]] virtual std::uint32_t activeVoices() const = 0;
    [[nodiscard]] virtual const char* name() const = 0;
};

/**
 * @brief Null-бэкенд: полная логика пула голосов без устройства вывода.
 */
class NullAudioBackend final : public IAudioBackend {
public:
    bool init(std::uint32_t maxVoices) override;
    void shutdown() override;
    ClipHandle loadClip(const AudioClip& clip) override;
    void unloadClip(ClipHandle c) override;
    VoiceHandle play(const PlayParams& p) override;
    void stop(VoiceHandle v) override;
    void stopAll() override;
    void setVoicePosition(VoiceHandle v, const Vec3& p) override;
    void setVolume(VoiceHandle v, Real vol) override;
    void setListener(const Vec3& position, const Vec3& forward, const Vec3& up) override;
    void setBusVolume(Bus b, Real vol) override;
    void update(float dt) override;
    [[nodiscard]] std::uint32_t activeVoices() const override;
    [[nodiscard]] const char* name() const override { return "NullAudio"; }
    [[nodiscard]] const std::vector<AudioClip>& clips() const noexcept { return clips_; }

private:
    struct Voice {
        PlayParams params;
        Real elapsed = 0;
        bool active = false;
    };
    std::vector<AudioClip> clips_;
    std::vector<Voice> voices_;
    std::vector<std::uint32_t> freeVoices_;
    Real busVolume_[static_cast<std::size_t>(Bus::Count)] = {1, 1, 1};
    Vec3 listenerPos_, listenerFwd_{0, 0, -1}, listenerUp_{0, 1, 0};
    std::uint32_t maxVoices_ = 32;
    bool initialized_ = false;
};

/**
 * @brief Фасад подсистемы звука для игрового кода и LV Script.
 */
class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    bool init(std::uint32_t maxVoices = 32, bool forceNull = false);
    void shutdown();

    /// Загрузить клип из PCM-данных (декодирование WAV/OGG — в asset::AudioLoader).
    ClipHandle registerClip(AudioClip clip);

    /// Простые вызовы «выстрел/шаг/UI».
    VoiceHandle play2D(ClipHandle clip, Real volume = 1.0f, Real pitch = 1.0f, bool loop = false);
    VoiceHandle play3D(ClipHandle clip, const Vec3& position, Real volume = 1.0f);
    VoiceHandle play(const PlayParams& p);
    void stop(VoiceHandle v);
    void stopAll();

    void setListener(const Vec3& pos, const Vec3& fwd, const Vec3& up);
    void setBusVolume(Bus b, Real v);
    void update(ecs::World& world, float dt);

    [[nodiscard]] IAudioBackend* backend() noexcept { return backend_.get(); }
    [[nodiscard]] std::uint32_t activeVoices() const;

private:
    std::unique_ptr<IAudioBackend> backend_;
};

/// ECS-компонент позиционного источника (следует за сущностью).
struct AudioSource {
    audio::VoiceHandle voice;
    audio::ClipHandle  clip;
    Real volume = 1.0f;
    Real minDistance = 1.0f;
    Real maxDistance = 40.0f;
    bool loop = false;
    bool playOnEnable = false;
    static constexpr std::string_view lv_component_name = "AudioSource";
};

void attachToWorld(AudioSystem& audio, ecs::World& world);
std::unique_ptr<IAudioBackend> createAudioBackend(bool forceNull);

} // namespace lv::audio
