#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace horizon {

class RomFsBuilder {
public:
    static constexpr std::uint64_t kDemoSpriteDataOffset = 0x100U;

    [[nodiscard]] static std::vector<std::uint8_t> build_single_file(
        const std::string& filename,
        std::span<const std::uint8_t> contents);

    [[nodiscard]] static std::vector<std::uint8_t> make_demo_sprite_rgba();
};

} // namespace horizon
