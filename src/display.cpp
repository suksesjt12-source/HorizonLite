#include "horizon/display.hpp"

#include "horizon/kernel.hpp"
#include "horizon/gl_renderer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace horizon {

std::vector<std::uint8_t> UntileTegraBlockLinear(
    std::span<const std::uint8_t> source, std::uint32_t width,
    std::uint32_t height, std::uint32_t block_height_gobs,
    bool block_linear) {
    if (width == 0U || height == 0U)
        throw std::invalid_argument("Tegra surface dimensions must be nonzero");
    if (block_height_gobs == 0U || block_height_gobs > 32U ||
        (block_height_gobs & (block_height_gobs - 1U)) != 0U)
        throw std::invalid_argument("Tegra block height must be a power of two from 1 to 32");
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(width) * height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4U)
        throw std::overflow_error("Tegra surface dimensions overflow");
    const std::size_t linear_size = static_cast<std::size_t>(pixel_count) * 4U;
    if (source.size() < linear_size)
        throw std::invalid_argument("Tegra source is smaller than a linear RGBA surface");

    if (!block_linear) {
        return std::vector<std::uint8_t>(source.begin(),
            source.begin() + static_cast<std::ptrdiff_t>(linear_size));
    }

    constexpr std::uint64_t gob_bytes = 512U;
    constexpr std::uint64_t gob_width_bytes = 64U;
    constexpr std::uint64_t gob_height_rows = 8U;
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t width_in_gobs =
        (row_bytes + gob_width_bytes - 1U) / gob_width_bytes;
    const std::uint64_t gob_rows_per_block =
        gob_height_rows * block_height_gobs;
    const std::uint64_t block_rows =
        (height + gob_rows_per_block - 1U) / gob_rows_per_block;
    const std::uint64_t tiled_size =
        width_in_gobs * block_height_gobs * block_rows * gob_bytes;

    if (tiled_size > source.size()) {
        return std::vector<std::uint8_t>(source.begin(),
            source.begin() + static_cast<std::ptrdiff_t>(linear_size));
    }

    std::vector<std::uint8_t> output(linear_size);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::uint64_t x_byte = static_cast<std::uint64_t>(x) * 4U;
            const std::uint64_t gob_y = y / gob_rows_per_block;
            const std::uint64_t block_y = (y % gob_rows_per_block) / gob_height_rows;
            const std::uint64_t gob_x = x_byte / gob_width_bytes;
            const std::uint64_t in_gob_x = x_byte % gob_width_bytes;
            const std::uint64_t in_gob_y = y % gob_height_rows;
            const std::uint64_t gob_index =
                gob_y * width_in_gobs * block_height_gobs +
                gob_x * block_height_gobs + block_y;
            const std::uint64_t in_gob =
                (in_gob_x / 32U) * 256U +
                (in_gob_y / 2U) * 64U +
                ((in_gob_x % 32U) / 16U) * 32U +
                (in_gob_y % 2U) * 16U + (in_gob_x % 16U);
            const std::uint64_t source_offset = gob_index * gob_bytes + in_gob;
            const std::size_t target_offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            if (source_offset + 4U > source.size())
                throw std::runtime_error("Tegra GOB address exceeds source allocation");
            std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_offset),
                        4U, output.begin() + static_cast<std::ptrdiff_t>(target_offset));
        }
    }
    return output;
}

SdlDisplay::SdlDisplay() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    sdl_initialized_ = true;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window_ = SDL_CreateWindow("HorizonLite Stage 11 - OpenGL Maxwell HLE",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (window_ != nullptr) {
        gl_context_ = SDL_GL_CreateContext(window_);
        if (gl_context_ != nullptr && SDL_GL_MakeCurrent(window_, gl_context_) == 0) {
            (void)SDL_GL_SetSwapInterval(1);
            try {
                gl_renderer_ = std::make_unique<OpenGLRenderer>(window_, 1280, 720);
                return;
            } catch (const std::exception&) {
                gl_renderer_.reset();
            }
        }
        if (gl_context_ != nullptr) {
            SDL_GL_DeleteContext(gl_context_);
            gl_context_ = nullptr;
        }
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    create_software_fallback();
}

void SdlDisplay::create_software_fallback() {
    window_ = SDL_CreateWindow("HorizonLite - 2D compatibility display",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr)
        throw std::runtime_error(std::string("SDL window creation failed: ") + SDL_GetError());
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr)
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (renderer_ == nullptr)
        throw std::runtime_error(std::string("SDL fallback renderer failed: ") + SDL_GetError());
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32,
                                 SDL_TEXTUREACCESS_STREAMING, 1280, 720);
    if (texture_ == nullptr)
        throw std::runtime_error(std::string("SDL fallback texture failed: ") + SDL_GetError());
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
}

SdlDisplay::~SdlDisplay() {
    gl_renderer_.reset();
    if (gl_context_ != nullptr) {
        SDL_GL_MakeCurrent(window_, gl_context_);
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (texture_ != nullptr) SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    if (sdl_initialized_) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

bool SdlDisplay::process_events() const {
    SDL_Event event{};
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) return false;
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE) return false;
        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
            return false;
    }
    return true;
}

std::uint64_t SdlDisplay::read_controller_buttons() const {
    int key_count = 0;
    const Uint8* keys = SDL_GetKeyboardState(&key_count);
    const auto down = [keys, key_count](SDL_Scancode code) {
        const int index = static_cast<int>(code);
        return keys != nullptr && index >= 0 && index < key_count && keys[index] != 0U;
    };

    std::uint64_t buttons = 0U;
    if (down(SDL_SCANCODE_Z)) buttons |= HidButtonA;
    if (down(SDL_SCANCODE_X)) buttons |= HidButtonB;
    if (down(SDL_SCANCODE_A)) buttons |= HidButtonX;
    if (down(SDL_SCANCODE_S)) buttons |= HidButtonY;
    if (down(SDL_SCANCODE_RETURN) || down(SDL_SCANCODE_KP_ENTER))
        buttons |= HidButtonPlus;

    if (down(SDL_SCANCODE_LEFT) || down(SDL_SCANCODE_A))
        buttons |= HidDpadLeft;
    if (down(SDL_SCANCODE_UP) || down(SDL_SCANCODE_W))
        buttons |= HidDpadUp;
    if (down(SDL_SCANCODE_RIGHT) || down(SDL_SCANCODE_D))
        buttons |= HidDpadRight;
    if (down(SDL_SCANCODE_DOWN) || down(SDL_SCANCODE_S))
        buttons |= HidDpadDown;
    return buttons;
}

void SdlDisplay::render(std::span<const std::uint8_t> rgba,
                        int width, int height) {
    if (width <= 0 || height <= 0 ||
        rgba.size() < static_cast<std::size_t>(width) *
                      static_cast<std::size_t>(height) * 4U)
        throw std::invalid_argument("SDL display received an invalid RGBA surface");
    if (gl_renderer_ != nullptr) {
        if (!gl_renderer_->has_pending_gpu_frame())
            gl_renderer_->upload_rgba(rgba, width, height);
        gl_renderer_->present();
        return;
    }
    if (SDL_UpdateTexture(texture_, nullptr, rgba.data(), width * 4) != 0)
        throw std::runtime_error(std::string("SDL_UpdateTexture failed: ") + SDL_GetError());
    if (SDL_RenderClear(renderer_) != 0 ||
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) != 0)
        throw std::runtime_error(std::string("SDL render operation failed: ") + SDL_GetError());
    SDL_RenderPresent(renderer_);
}

OpenGLRenderer* SdlDisplay::gl_renderer() noexcept { return gl_renderer_.get(); }
bool SdlDisplay::hardware_renderer_available() const noexcept {
    return gl_renderer_ != nullptr && gl_renderer_->available();
}

void SdlDisplay::render(const AddressSpace& memory,
                        std::uint64_t framebuffer_address,
                        int width, int height) {
    const std::size_t byte_count = static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * 4U;
    render(memory.view(framebuffer_address, byte_count, Permission::Read),
           width, height);
}

} // namespace horizon
