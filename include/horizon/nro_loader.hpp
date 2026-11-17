#pragma once

#include "horizon/memory.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace horizon {

struct NroSegment {
    std::uint32_t file_offset{};
    std::uint32_t size{};
};

struct NroImage {
    std::uint32_t declared_size{};
    std::uint32_t flags{};
    std::array<NroSegment, 3> segments{};
    std::uint32_t bss_size{};
    std::uint64_t entry_point{};
    std::uint64_t load_base{};
    std::uint64_t bss_base{};
    std::vector<std::uint8_t> romfs;
};

class NroLoader {
public:
    [[nodiscard]] static NroImage parse(const std::vector<std::uint8_t>& file);
    [[nodiscard]] static NroImage load(const std::vector<std::uint8_t>& file,
                                       AddressSpace& memory,
                                       std::uint64_t load_base = 0U);
    [[nodiscard]] static std::vector<std::uint8_t> read_file(
        const std::string& path);
};

} // namespace horizon
