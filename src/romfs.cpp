#include "horizon/romfs.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace horizon {
namespace {

constexpr std::uint64_t kRomFsHeaderSize = 0x50U;
constexpr std::uint64_t kDirectoryHashOffset = 0x50U;
constexpr std::uint64_t kDirectoryHashSize = 0x0CU;
constexpr std::uint64_t kDirectoryTableOffset = 0x5CU;
constexpr std::uint64_t kDirectoryTableSize = 0x18U;
constexpr std::uint64_t kFileHashOffset = 0x74U;
constexpr std::uint64_t kFileHashSize = 0x0CU;
constexpr std::uint64_t kFileTableOffset = 0x80U;
constexpr std::uint32_t kNone = 0xFFFFFFFFU;

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U)
        throw std::runtime_error("RomFS builder u32 write is out of range");
    for (unsigned i = 0; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint64_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 8U)
        throw std::runtime_error("RomFS builder u64 write is out of range");
    for (unsigned i = 0; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::uint32_t path_hash(std::span<const std::uint8_t> name,
                        std::uint32_t parent_offset) noexcept {
    std::uint32_t hash = parent_offset ^ 123456789U;
    for (std::size_t index = 0; index < name.size(); index += 2U) {
        hash = (hash >> 5U) | (hash << 27U);
        const std::uint16_t code = static_cast<std::uint16_t>(name[index]) |
            static_cast<std::uint16_t>(
                index + 1U < name.size() ?
                    static_cast<std::uint16_t>(name[index + 1U]) << 8U : 0U);
        hash ^= code;
    }
    return hash;
}

} // namespace

std::vector<std::uint8_t> RomFsBuilder::build_single_file(
    const std::string& filename, std::span<const std::uint8_t> contents) {
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos) {
        throw std::invalid_argument("RomFS filename must be a non-empty root filename");
    }
    if (filename.size() > std::numeric_limits<std::uint32_t>::max() ||
        contents.size() > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("RomFS file is too large");
    }

    const std::size_t file_entry_size = align_up(0x20U + filename.size(), 4U);
    const std::size_t file_table_size = file_entry_size;
    const std::size_t file_data_offset = static_cast<std::size_t>(
        kDemoSpriteDataOffset);
    if (kFileTableOffset + file_table_size > file_data_offset)
        throw std::runtime_error("RomFS metadata does not fit before file data");
    if (contents.size() > std::numeric_limits<std::size_t>::max() - file_data_offset)
        throw std::length_error("RomFS image size overflows host size_t");

    std::vector<std::uint8_t> image(file_data_offset + contents.size(), 0U);

    write_u64(image, 0x00U, kRomFsHeaderSize);
    write_u64(image, 0x08U, kDirectoryHashOffset);
    write_u64(image, 0x10U, kDirectoryHashSize);
    write_u64(image, 0x18U, kDirectoryTableOffset);
    write_u64(image, 0x20U, kDirectoryTableSize);
    write_u64(image, 0x28U, kFileHashOffset);
    write_u64(image, 0x30U, kFileHashSize);
    write_u64(image, 0x38U, kFileTableOffset);
    write_u64(image, 0x40U, file_table_size);
    write_u64(image, 0x48U, file_data_offset);

    for (std::size_t bucket = 0; bucket < 3U; ++bucket) {
        write_u32(image, static_cast<std::size_t>(kDirectoryHashOffset) + bucket * 4U,
                  kNone);
        write_u32(image, static_cast<std::size_t>(kFileHashOffset) + bucket * 4U,
                  kNone);
    }

    const std::size_t root = static_cast<std::size_t>(kDirectoryTableOffset);
    write_u32(image, root + 0x00U, 0U);
    write_u32(image, root + 0x04U, kNone);
    write_u32(image, root + 0x08U, kNone);
    write_u32(image, root + 0x0CU, 0U);
    write_u32(image, root + 0x10U, kNone);
    write_u32(image, root + 0x14U, 0U);

    const std::size_t file_entry = static_cast<std::size_t>(kFileTableOffset);
    write_u32(image, file_entry + 0x00U, 0U);
    write_u32(image, file_entry + 0x04U, kNone);
    write_u64(image, file_entry + 0x08U, 0U);
    write_u64(image, file_entry + 0x10U, contents.size());
    write_u32(image, file_entry + 0x18U, kNone);
    write_u32(image, file_entry + 0x1CU,
              static_cast<std::uint32_t>(filename.size()));
    std::copy(filename.begin(), filename.end(),
              image.begin() + static_cast<std::ptrdiff_t>(file_entry + 0x20U));

    const auto name = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(filename.data()), filename.size());
    const std::uint32_t bucket = path_hash(name, 0U) % 3U;
    write_u32(image, static_cast<std::size_t>(kFileHashOffset) + bucket * 4U, 0U);

    std::copy(contents.begin(), contents.end(),
              image.begin() + static_cast<std::ptrdiff_t>(file_data_offset));
    return image;
}

std::vector<std::uint8_t> RomFsBuilder::make_demo_sprite_rgba() {
    constexpr unsigned width = 16U;
    constexpr unsigned height = 16U;
    std::vector<std::uint8_t> pixels(width * height * 4U, 0U);
    for (unsigned y = 1U; y < 15U; ++y) {
        for (unsigned x = 1U; x < 15U; ++x) {
            std::array<std::uint8_t, 4> color{0x42U, 0xB8U, 0xF0U, 0xFFU};
            if ((y == 3U || y == 12U) && (x == 3U || x == 12U))
                color = {0xFFU, 0xD0U, 0x40U, 0xFFU};
            if ((y == 6U || y == 7U) && (x == 5U || x == 10U))
                color = {0x18U, 0x20U, 0x38U, 0xFFU};
            if (y >= 10U && x >= 6U && x <= 9U)
                color = {0xE8U, 0xF2U, 0xFFU, 0xFFU};
            const std::size_t offset = (y * width + x) * 4U;
            std::copy(color.begin(), color.end(),
                      pixels.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    return pixels;
}

} // namespace horizon
