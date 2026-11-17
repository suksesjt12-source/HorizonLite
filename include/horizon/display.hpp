#pragma once

#include "horizon/memory.hpp"

#include <SDL.h>

namespace horizon {

class SdlDisplay {
public:
    SdlDisplay();
    ~SdlDisplay();

    SdlDisplay(const SdlDisplay&) = delete;
    SdlDisplay& operator=(const SdlDisplay&) = delete;

    [[nodiscard]] bool process_events() const;
    void render(const AddressSpace& memory, std::uint64_t framebuffer_address,
                int width, int height);

private:
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    bool sdl_initialized_{};
};

} // namespace horizon
