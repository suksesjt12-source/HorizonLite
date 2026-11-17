#include "horizon/display.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

std::uint64_t tiled_offset(std::uint32_t x, std::uint32_t y,
                           std::uint32_t width,
                           std::uint32_t block_height) {
    constexpr std::uint64_t gob_bytes = 512U;
    constexpr std::uint64_t gob_width = 64U;
    constexpr std::uint64_t gob_rows = 8U;
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t width_gobs = (row_bytes + gob_width - 1U) / gob_width;
    const std::uint64_t rows_per_block = gob_rows * block_height;
    const std::uint64_t gob_y = y / rows_per_block;
    const std::uint64_t block_y = (y % rows_per_block) / gob_rows;
    const std::uint64_t x_byte = static_cast<std::uint64_t>(x) * 4U;
    const std::uint64_t gob_x = x_byte / gob_width;
    const std::uint64_t in_x = x_byte % gob_width;
    const std::uint64_t in_y = y % gob_rows;
    const std::uint64_t gob_index =
        gob_y * width_gobs * block_height + gob_x * block_height + block_y;
    const std::uint64_t in_gob =
        (in_x / 32U) * 256U + (in_y / 2U) * 64U +
        ((in_x % 32U) / 16U) * 32U + (in_y % 2U) * 16U +
        (in_x % 16U);
    return gob_index * gob_bytes + in_gob;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t width = 32U;
        constexpr std::uint32_t height = 32U;
        constexpr std::uint32_t block_height = 16U;
        const std::size_t linear_size =
            static_cast<std::size_t>(width) * height * 4U;
        std::vector<std::uint8_t> linear(linear_size, 0U);
        for (std::uint32_t y = 0U; y < height; ++y) {
            for (std::uint32_t x = 0U; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                linear[offset] = static_cast<std::uint8_t>(x);
                linear[offset + 1U] = static_cast<std::uint8_t>(y);
                linear[offset + 2U] = static_cast<std::uint8_t>(x ^ y);
                linear[offset + 3U] = 0xFFU;
            }
        }

        const auto linear_result =
            horizon::UntileTegraBlockLinear(linear, width, height, block_height, false);
        require(linear_result == linear, "linear fallback changed RGBA pixels");

        constexpr std::size_t tiled_size =
            2U * block_height * 512U;
        std::vector<std::uint8_t> tiled(tiled_size, 0U);
        for (std::uint32_t y = 0U; y < height; ++y) {
            for (std::uint32_t x = 0U; x < width; ++x) {
                const std::size_t source_offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                const std::uint64_t destination_offset =
                    tiled_offset(x, y, width, block_height);
                for (std::size_t byte = 0U; byte < 4U; ++byte) {
                    tiled[static_cast<std::size_t>(destination_offset) + byte] =
                        linear[source_offset + byte];
                }
            }
        }

        const auto untiled =
            horizon::UntileTegraBlockLinear(tiled, width, height, block_height, true);
        require(untiled == linear, "16Bx2 GOB untile output differs from linear pixels");

        bool invalid_rejected = false;
        try {
            (void)horizon::UntileTegraBlockLinear(linear, width, height, 3U, true);
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        require(invalid_rejected, "non-power-of-two block height was accepted");

        std::cout << "Tegra GOB untile and linear fallback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Display/GPU test failed: " << error.what() << '\n';
        return 1;
    }
}
