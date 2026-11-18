#pragma once

#include "horizon/memory.hpp"

#include <SDL.h>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace horizon {

class OpenGLRenderer;

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
    [[nodiscard]] OpenGLRenderer* gl_renderer() noexcept;
    [[nodiscard]] bool hardware_renderer_available() const noexcept;

private:
    void create_software_fallback();

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    SDL_GLContext gl_context_{};
    std::unique_ptr<OpenGLRenderer> gl_renderer_;
    bool sdl_initialized_{};
};

} // namespace horizon
