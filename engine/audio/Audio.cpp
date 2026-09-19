/**
 * @file    Audio.cpp
 * @brief   Реализация звуковой подсистемы и null-бэкенда.
 */
#include "Audio.h"

#if defined(LV_WITH_OPENAL)
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace lv::audio {

// ---------------------------------------------------------------------------
//  NullAudioBackend
// ---------------------------------------------------------------------------
bool NullAudioBackend::init(std::uint32_t maxVoices) {
    maxVoices_ = maxVoices == 0 ? 32 : maxVoices;
    voices_.assign(maxVoices_, Voice{});
    freeVoices_.clear();
    for (std::uint32_t i = 0; i < maxVoices_; ++i) freeVoices_.push_back(maxVoices_ - 1 - i);
    initialized_ = true;
    return true;
}

void NullAudioBackend::shutdown() {
    voices_.clear();
    freeVoices_.clear();
    clips_.clear();
    initialized_ = false;
}

ClipHandle NullAudioBackend::loadClip(const AudioClip& clip) {
    clips_.push_back(clip);
    if (clips_.back().durationSeconds <= 0 && clip.sampleRate > 0)
        clips_.back().durationSeconds = static_cast<Real>(clip.samples.size()) /
                                        (static_cast<Real>(clip.sampleRate) * clip.channels);
    return ClipHandle{static_cast<std::uint32_t>(clips_.size() - 1)};
}

void NullAudioBackend::unloadClip(ClipHandle c) {
    if (c.index < clips_.size()) clips_[c.index].samples.clear();
}

VoiceHandle NullAudioBackend::play(const PlayParams& p) {
    if (!initialized_ || !p.clip.valid() || p.clip.index >= clips_.size()) return kInvalidVoice;
    std::uint32_t slot;
    if (!freeVoices_.empty()) {
        slot = freeVoices_.back();
        freeVoices_.pop_back();
    } else {
        // Все голоса заняты: вытесняем самый тихий/низкоприоритетный.
        std::uint32_t victim = 0;
        int bestScore = std::numeric_limits<int>::max();
        for (std::uint32_t i = 0; i < voices_.size(); ++i) {
            if (!voices_[i].active) continue;
            const Real dist = distance(voices_[i].params.position, listenerPos_);
            // Чем меньше приоритет и чем дальше источник, тем «дешевле» голос.
            const int score = static_cast<int>(voices_[i].params.priority * 100) -
                              static_cast<int>(std::min<Real>(dist, 999.f) * 10);
            if (score < bestScore) { bestScore = score; victim = i; }
        }
        if (static_cast<int>(p.priority * 100) <= bestScore) return kInvalidVoice;  // новый звук хуже
        slot = victim;
    }
    Voice& v = voices_[slot];
    v.params = p;
    v.elapsed = 0;
    v.active = true;
    return VoiceHandle{slot};
}

void NullAudioBackend::stop(VoiceHandle v) {
    if (v.index < voices_.size() && voices_[v.index].active) {
        voices_[v.index].active = false;
        freeVoices_.push_back(v.index);
    }
}

void NullAudioBackend::stopAll() {
    for (std::uint32_t i = 0; i < voices_.size(); ++i) voices_[i].active = false;
    freeVoices_.clear();
    for (std::uint32_t i = 0; i < maxVoices_; ++i) freeVoices_.push_back(maxVoices_ - 1 - i);
}

void NullAudioBackend::setVoicePosition(VoiceHandle v, const Vec3& p) {
    if (v.index < voices_.size()) voices_[v.index].params.position = p;
}
void NullAudioBackend::setVolume(VoiceHandle v, Real vol) {
    if (v.index < voices_.size()) voices_[v.index].params.volume = vol;
}
void NullAudioBackend::setListener(const Vec3& position, const Vec3& forward, const Vec3& up) {
    listenerPos_ = position; listenerFwd_ = forward; listenerUp_ = up;
}
void NullAudioBackend::setBusVolume(Bus b, Real vol) {
    busVolume_[static_cast<std::size_t>(b)] = clampReal(vol, 0.f, 1.f);
}

void NullAudioBackend::update(float dt) {
    for (std::uint32_t i = 0; i < voices_.size(); ++i) {
        Voice& v = voices_[i];
        if (!v.active) continue;
        v.elapsed += dt;
        const AudioClip& clip = clips_[v.params.clip.index];
        if (!v.params.loop && clip.durationSeconds > 0 && v.elapsed >= clip.durationSeconds) {
            v.active = false;
            freeVoices_.push_back(i);
        }
    }
}

std::uint32_t NullAudioBackend::activeVoices() const {
    std::uint32_t n = 0;
    for (const Voice& v : voices_) if (v.active) ++n;
    return n;
}

// ---------------------------------------------------------------------------
//  Фабрика и фасад
// ---------------------------------------------------------------------------
std::unique_ptr<IAudioBackend> createAudioBackend(bool forceNull) {
#if defined(LV_WITH_OPENAL)
    if (!forceNull) {
        // Реальный OpenAL-бэкенд живёт в backends/OpenALBackend.cpp.
        extern std::unique_ptr<IAudioBackend> createOpenALBackend();
        if (auto b = createOpenALBackend()) return b;
    }
#else
    (void)forceNull;
#endif
    return std::make_unique<NullAudioBackend>();
}

AudioSystem::AudioSystem() = default;
AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::init(std::uint32_t maxVoices, bool forceNull) {
    backend_ = createAudioBackend(forceNull);
    return backend_ && backend_->init(maxVoices);
}
void AudioSystem::shutdown() {
    if (backend_) backend_->shutdown();
    backend_.reset();
}

ClipHandle AudioSystem::registerClip(AudioClip clip) {
    return backend_ ? backend_->loadClip(std::move(clip)) : kInvalidClip;
}

VoiceHandle AudioSystem::play2D(ClipHandle clip, Real volume, Real pitch, bool loop) {
    PlayParams p; p.clip = clip; p.volume = volume; p.pitch = pitch; p.loop = loop; p.positional = false;
    return play(p);
}

VoiceHandle AudioSystem::play3D(ClipHandle clip, const Vec3& position, Real volume) {
    PlayParams p; p.clip = clip; p.volume = volume; p.positional = true; p.position = position;
    return play(p);
}

VoiceHandle AudioSystem::play(const PlayParams& p) {
    return backend_ ? backend_->play(p) : kInvalidVoice;
}

void AudioSystem::stop(VoiceHandle v) { if (backend_) backend_->stop(v); }
void AudioSystem::stopAll() { if (backend_) backend_->stopAll(); }
void AudioSystem::setListener(const Vec3& pos, const Vec3& fwd, const Vec3& up) {
    if (backend_) backend_->setListener(pos, fwd, up);
}
void AudioSystem::setBusVolume(Bus b, Real v) { if (backend_) backend_->setBusVolume(b, v); }

void AudioSystem::update(ecs::World& world, float dt) {
    if (!backend_) return;
    // Источники, привязанные к сущностям, следуют за их Transform.
    world.registry().each<ecs::Transform, AudioSource>(
        [this](ecs::Entity, ecs::Transform& t, AudioSource& src) {
            if (src.voice.valid()) backend_->setVoicePosition(src.voice, t.position);
            return true;
        });
    backend_->update(dt);
}

std::uint32_t AudioSystem::activeVoices() const {
    return backend_ ? backend_->activeVoices() : 0;
}

void attachToWorld(AudioSystem& audio, ecs::World& world) {
    ecs::registerComponent<AudioSource>();
    ecs::SystemDesc sys;
    sys.name = "Audio";
    sys.phase = ecs::Phase::Late;
    sys.order = 10;
    sys.update = [&audio](ecs::World& w, float dt) { audio.update(w, dt); };
    world.addSystem(std::move(sys));
}

} // namespace lv::audio
