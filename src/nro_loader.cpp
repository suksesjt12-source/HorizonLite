#include "horizon/nro_loader.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace horizon {
namespace {

constexpr std::size_t kNroHeaderOffset = 0x10;
constexpr std::size_t kNroHeaderSize = 0x70;
constexpr std::size_t kMinimumNroSize = kNroHeaderOffset + kNroHeaderSize;
constexpr std::uint32_t kNroCompressedFlag = 1U << 1U;

std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes,
                        std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("NRO header is truncated");
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool ranges_overlap(const NroSegment& lhs, const NroSegment& rhs) {
    if (lhs.size == 0 || rhs.size == 0) {
        return false;
    }
    const std::uint64_t lhs_end =
        static_cast<std::uint64_t>(lhs.file_offset) + lhs.size;
    const std::uint64_t rhs_end =
        static_cast<std::uint64_t>(rhs.file_offset) + rhs.size;
    return lhs.file_offset < rhs_end && rhs.file_offset < lhs_end;
}

} // namespace

NroImage NroLoader::parse(const std::vector<std::uint8_t>& file) {
    if (file.size() < kMinimumNroSize) {
        throw std::runtime_error("file is too small to contain an NRO header");
    }

    constexpr std::array<std::uint8_t, 4> magic{'N', 'R', 'O', '0'};
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (file[kNroHeaderOffset + i] != magic[i]) {
            throw std::runtime_error("invalid NRO magic; expected NRO0 at offset 0x10");
        }
    }

    NroImage image;
    image.declared_size = read_le32(file, 0x18);
    image.flags = read_le32(file, 0x1C);
    image.segments = {{
        {read_le32(file, 0x20), read_le32(file, 0x24)},
        {read_le32(file, 0x28), read_le32(file, 0x2C)},
        {read_le32(file, 0x30), read_le32(file, 0x34)},
    }};
    image.bss_size = read_le32(file, 0x38);

    if (image.declared_size < kMinimumNroSize || image.declared_size > file.size()) {
        throw std::runtime_error("NRO declared size is outside the supplied file");
    }
    if ((image.flags & kNroCompressedFlag) != 0U) {
        throw std::runtime_error("compressed NRO files are not supported by this prototype");
    }
    if (image.segments[0].size == 0 || image.segments[0].file_offset != 0) {
        throw std::runtime_error("prototype expects a non-empty .text segment starting at file offset 0");
    }

    for (const NroSegment& segment : image.segments) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(segment.file_offset) + segment.size;
        if (end > image.declared_size) {
            throw std::runtime_error("NRO segment extends beyond the declared NRO image");
        }
    }
    for (std::size_t i = 0; i < image.segments.size(); ++i) {
        for (std::size_t j = i + 1; j < image.segments.size(); ++j) {
            if (ranges_overlap(image.segments[i], image.segments[j])) {
                throw std::runtime_error("NRO segments overlap");
            }
        }
    }

    image.entry_point = image.segments[0].file_offset;
    return image;
}

NroImage NroLoader::load(const std::vector<std::uint8_t>& file,
                         AddressSpace& memory) {
    const NroImage image = parse(file);
    constexpr std::array<Permission, 3> permissions{
        Permission::Read | Permission::Execute,
        Permission::Read,
        Permission::Read | Permission::Write,
    };

    for (std::size_t i = 0; i < image.segments.size(); ++i) {
        const NroSegment& segment = image.segments[i];
        if (segment.size == 0) {
            continue;
        }
        const auto first = file.begin() + static_cast<std::ptrdiff_t>(segment.file_offset);
        const auto last = first + static_cast<std::ptrdiff_t>(segment.size);
        memory.map(segment.file_offset, std::vector<std::uint8_t>(first, last),
                   permissions[i]);
    }

    const NroSegment& data = image.segments[2];
    if (image.bss_size != 0) {
        const std::uint64_t bss_base =
            static_cast<std::uint64_t>(data.file_offset) + data.size;
        memory.map_zeroed(bss_base, image.bss_size,
                          Permission::Read | Permission::Write);
    }
    return image;
}

std::vector<std::uint8_t> NroLoader::read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path);
    }
    const std::streamoff length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) >
                          std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("invalid file size");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("failed while reading file: " + path);
    }
    return bytes;
}

} // namespace horizon
