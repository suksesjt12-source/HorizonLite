#include "horizon/nro_loader.hpp"
#include "horizon/romfs.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8U));
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i)
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8U));
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned i = 0; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes.at(offset + i)) << (i * 8U);
    return value;
}

} // namespace

int main() {
    try {
        const std::vector<std::uint8_t> sprite =
            horizon::RomFsBuilder::make_demo_sprite_rgba();
        if (sprite.size() != 16U * 16U * 4U)
            throw std::runtime_error("generated sprite has an incorrect byte length");

        const std::vector<std::uint8_t> romfs =
            horizon::RomFsBuilder::build_single_file("sprite.rgba", sprite);
        if (read_u64(romfs, 0U) != 0x50U ||
            read_u64(romfs, 0x48U) != horizon::RomFsBuilder::kDemoSpriteDataOffset ||
            !std::equal(sprite.begin(), sprite.end(),
                romfs.begin() + static_cast<std::ptrdiff_t>(
                    horizon::RomFsBuilder::kDemoSpriteDataOffset))) {
            throw std::runtime_error("RomFS header, data offset, or file data is invalid");
        }

        constexpr std::size_t nro_size = 0x100U;
        constexpr std::size_t asset_header_size = 0x38U;
        std::vector<std::uint8_t> nro(nro_size + asset_header_size + romfs.size(), 0U);
        nro[0x10U] = 'N';
        nro[0x11U] = 'R';
        nro[0x12U] = 'O';
        nro[0x13U] = '0';
        write_u32(nro, 0x18U, static_cast<std::uint32_t>(nro_size));
        write_u32(nro, 0x20U, 0U);
        write_u32(nro, 0x24U, static_cast<std::uint32_t>(nro_size));
        write_u32(nro, nro_size, 0x54455341U);
        write_u32(nro, nro_size + 4U, 0U);
        write_u64(nro, nro_size + 0x28U, asset_header_size);
        write_u64(nro, nro_size + 0x30U, romfs.size());
        std::copy(romfs.begin(), romfs.end(),
                  nro.begin() + static_cast<std::ptrdiff_t>(nro_size + asset_header_size));

        const horizon::NroImage parsed = horizon::NroLoader::parse(nro);
        if (parsed.romfs != romfs)
            throw std::runtime_error("NRO ASET parser did not extract the exact RomFS blob");

        std::cout << "RomFS builder and NRO ASET extraction test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RomFS/NRO test failed: " << error.what() << '\n';
        return 1;
    }
}
