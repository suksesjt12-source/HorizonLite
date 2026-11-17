#include "horizon/display.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace horizon {

SdlDisplay::SdlDisplay() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    sdl_initialized_ = true;

    window_ = SDL_CreateWindow("HorizonLite - Linear Framebuffer",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               1280, 720, SDL_WINDOW_SHOWN);
    if (window_ == nullptr) {
        const std::string error = SDL_GetError();
        SDL_Quit();
        sdl_initialized_ = false;
        throw std::runtime_error("SDL_CreateWindow failed: " + error);
    }

    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        const std::string error = SDL_GetError();
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
        sdl_initialized_ = false;
        throw std::runtime_error("SDL_CreateRenderer failed: " + error);
    }

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING, 1280, 720);
    if (texture_ == nullptr) {
        const std::string error = SDL_GetError();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        SDL_Quit();
        sdl_initialized_ = false;
        throw std::runtime_error("SDL_CreateTexture failed: " + error);
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
}

SdlDisplay::~SdlDisplay() {
    if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    if (sdl_initialized_) SDL_Quit();
}

bool SdlDisplay::process_events() const {
    SDL_Event event{};
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            return false;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE) {
            return false;
        }
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
            return false;
        }
    }
    return true;
}

void SdlDisplay::render(const AddressSpace& memory,
                        std::uint64_t framebuffer_address,
                        int width, int height) {
    const std::size_t byte_count = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4U;
    const auto pixels = memory.view(framebuffer_address, byte_count,
                                    Permission::Read);

    void* texture_pixels = nullptr;
    int texture_pitch = 0;
    if (SDL_LockTexture(texture_, nullptr, &texture_pixels, &texture_pitch) != 0) {
        throw std::runtime_error(std::string("SDL_LockTexture failed: ") +
                                 SDL_GetError());
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
    for (int row = 0; row < height; ++row) {
        const auto* source = pixels.data() +
            static_cast<std::size_t>(row) * row_bytes;
        auto* destination = static_cast<std::uint8_t*>(texture_pixels) +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(texture_pitch);
        std::memcpy(destination, source, row_bytes);
    }
    SDL_UnlockTexture(texture_);

    if (SDL_RenderClear(renderer_) != 0 ||
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) != 0) {
        throw std::runtime_error(std::string("SDL render operation failed: ") +
                                 SDL_GetError());
    }
    SDL_RenderPresent(renderer_);
}

} // namespace horizon
