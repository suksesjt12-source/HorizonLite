#pragma once

#include "horizon/memory.hpp"

#include <SDL.h>
#include <cstdint>
#include <span>
#include <vector>

namespace horizon {

[[nodiscard]] std::vector<std::uint8_t> UntileTegraBlockLinear(
    std::span<const std::uint8_t> source, std::uint32_t width,
    std::uint32_t height, std::uint32_t block_height_gobs = 16U,
    bool block_linear = true);

class SdlDisplay {
public:
    SdlDisplay();
    ~SdlDisplay();

    SdlDisplay(const SdlDisplay&) = delete;
    SdlDisplay& operator=(const SdlDisplay&) = delete;

    [[nodiscard]] bool process_events() const;
    [[nodiscard]] std::uint64_t read_controller_buttons() const;
    void render(std::span<const std::uint8_t> rgba, int width, int height);
    void render(const AddressSpace& memory, std::uint64_t framebuffer_address,
                int width, int height);

private:
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    bool sdl_initialized_{};
};

} // namespace horizon
