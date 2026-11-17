#include "horizon/audio.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace horizon {

SdlAudioOutput::SdlAudioOutput(bool headless) {
    if (headless && SDL_getenv("SDL_AUDIODRIVER") == nullptr) {
        if (SDL_setenv("SDL_AUDIODRIVER", "dummy", 0) != 0)
            throw std::runtime_error("failed to select SDL dummy audio driver");
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        throw std::runtime_error(std::string("SDL audio initialization failed: ") +
                                 SDL_GetError());
    audio_subsystem_initialized_ = true;

    SDL_AudioSpec desired{};
    desired.freq = 48000;
    desired.format = AUDIO_S16LSB;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = nullptr;

    SDL_AudioSpec obtained{};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (device_ == 0U) {
        const std::string error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_initialized_ = false;
        throw std::runtime_error("SDL_OpenAudioDevice failed: " + error);
    }

    if (obtained.freq != desired.freq || obtained.format != desired.format ||
        obtained.channels != desired.channels) {
        SDL_CloseAudioDevice(device_);
        device_ = 0U;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_initialized_ = false;
        throw std::runtime_error("SDL audio device did not accept 48000 Hz S16LE stereo");
    }

    SDL_PauseAudioDevice(device_, 1);
}

SdlAudioOutput::~SdlAudioOutput() {
    if (device_ != 0U) {
        SDL_ClearQueuedAudio(device_);
        SDL_CloseAudioDevice(device_);
        device_ = 0U;
    }
    if (audio_subsystem_initialized_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_initialized_ = false;
    }
}

void SdlAudioOutput::queue_pcm(std::span<const std::uint8_t> pcm) {
    if (device_ == 0U) throw std::runtime_error("SDL audio output is not open");
    if ((pcm.size() & 3U) != 0U)
        throw std::invalid_argument("stereo S16LE PCM size must be divisible by four");
    if (pcm.size() > std::numeric_limits<Uint32>::max())
        throw std::length_error("PCM buffer exceeds SDL_QueueAudio limit");
    if (!pcm.empty() &&
        SDL_QueueAudio(device_, pcm.data(), static_cast<Uint32>(pcm.size())) != 0) {
        throw std::runtime_error(std::string("SDL_QueueAudio failed: ") +
                                 SDL_GetError());
    }
}

void SdlAudioOutput::pause(bool paused) noexcept {
    if (device_ != 0U) SDL_PauseAudioDevice(device_, paused ? 1 : 0);
}

void SdlAudioOutput::clear() noexcept {
    if (device_ != 0U) SDL_ClearQueuedAudio(device_);
}

std::uint32_t SdlAudioOutput::queued_bytes() const noexcept {
    return device_ == 0U ? 0U : SDL_GetQueuedAudioSize(device_);
}

bool SdlAudioOutput::available() const noexcept { return device_ != 0U; }
std::uint32_t SdlAudioOutput::device_id() const noexcept { return device_; }

} // namespace horizon
