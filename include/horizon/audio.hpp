#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace horizon {

class SdlAudioOutput {
public:
    explicit SdlAudioOutput(bool headless = false);
    ~SdlAudioOutput();

    SdlAudioOutput(const SdlAudioOutput&) = delete;
    SdlAudioOutput& operator=(const SdlAudioOutput&) = delete;

    void queue_pcm(std::span<const std::uint8_t> pcm);
    void pause(bool paused) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint32_t queued_bytes() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::uint32_t device_id() const noexcept;

private:
    SDL_AudioDeviceID device_{};
    bool audio_subsystem_initialized_{};
};

} // namespace horizon
