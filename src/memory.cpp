#include "horizon/memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace horizon {

void AddressSpace::map(std::uint64_t base, std::vector<std::uint8_t> bytes,
                       Permission permissions) {
    if (bytes.empty()) throw std::invalid_argument("cannot map an empty memory region");
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - base)
        throw std::overflow_error("memory region address overflow");
    const std::uint64_t end = base + static_cast<std::uint64_t>(bytes.size());
    for (const Region& region : regions_) {
        const std::uint64_t other_end =
            region.base + static_cast<std::uint64_t>(region.bytes.size());
        if (base < other_end && region.base < end)
            throw std::runtime_error("memory region overlaps an existing mapping");
    }
    regions_.push_back(Region{base, std::move(bytes), permissions});
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& lhs, const Region& rhs) { return lhs.base < rhs.base; });
}

void AddressSpace::map_zeroed(std::uint64_t base, std::size_t size,
                              Permission permissions) {
    if (size != 0) map(base, std::vector<std::uint8_t>(size, 0), permissions);
}

void AddressSpace::unmap(std::uint64_t base, std::size_t size) {
    const auto found = std::find_if(regions_.begin(), regions_.end(),
        [base, size](const Region& region) {
            return region.base == base && region.bytes.size() == size;
        });
    if (found == regions_.end())
        throw std::runtime_error("unmap requires an exact mapped region");
    regions_.erase(found);
}

const AddressSpace::Region& AddressSpace::find_region(
    std::uint64_t address, std::size_t size, Permission access) const {
    for (const Region& region : regions_) {
        if (address < region.base) continue;
        const std::uint64_t offset64 = address - region.base;
        if (offset64 > region.bytes.size()) continue;
        const std::size_t offset = static_cast<std::size_t>(offset64);
        if (size > region.bytes.size() - offset) continue;
        if (!has_permission(region.permissions, access))
            throw std::runtime_error("memory access violates region permissions");
        return region;
    }
    throw std::runtime_error("memory access is outside mapped regions");
}

AddressSpace::Region& AddressSpace::find_region(
    std::uint64_t address, std::size_t size, Permission access) {
    for (Region& region : regions_) {
        if (address < region.base) continue;
        const std::uint64_t offset64 = address - region.base;
        if (offset64 > region.bytes.size()) continue;
        const std::size_t offset = static_cast<std::size_t>(offset64);
        if (size > region.bytes.size() - offset) continue;
        if (!has_permission(region.permissions, access))
            throw std::runtime_error("memory access violates region permissions");
        return region;
    }
    throw std::runtime_error("memory access is outside mapped regions");
}

std::vector<std::uint8_t> AddressSpace::read(std::uint64_t address,
                                            std::size_t size,
                                            Permission access) const {
    const std::span<const std::uint8_t> bytes = view(address, size, access);
    return {bytes.begin(), bytes.end()};
}

std::span<const std::uint8_t> AddressSpace::view(std::uint64_t address,
                                                 std::size_t size,
                                                 Permission access) const {
    const Region& region = find_region(address, size, access);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    return std::span<const std::uint8_t>(region.bytes.data() + offset, size);
}

void AddressSpace::write(std::uint64_t address,
                         const std::vector<std::uint8_t>& bytes) {
    Region& region = find_region(address, bytes.size(), Permission::Write);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    std::copy(bytes.begin(), bytes.end(),
              region.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void AddressSpace::write_u8(std::uint64_t address, std::uint8_t value) {
    Region& region = find_region(address, sizeof(value), Permission::Write);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    region.bytes[offset] = value;
}

void AddressSpace::write_u32(std::uint64_t address, std::uint32_t value) {
    Region& region = find_region(address, sizeof(value), Permission::Write);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    for (unsigned byte = 0; byte < 4U; ++byte)
        region.bytes[offset + byte] = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
}

void AddressSpace::write_u64(std::uint64_t address, std::uint64_t value) {
    Region& region = find_region(address, sizeof(value), Permission::Write);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    for (unsigned byte = 0; byte < 8U; ++byte)
        region.bytes[offset + byte] = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
}

std::uint8_t AddressSpace::read_u8(std::uint64_t address, Permission access) const {
    const Region& region = find_region(address, sizeof(std::uint8_t), access);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    return region.bytes[offset];
}

std::uint32_t AddressSpace::read_u32(std::uint64_t address, Permission access) const {
    const Region& region = find_region(address, sizeof(std::uint32_t), access);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4U; ++byte)
        value |= static_cast<std::uint32_t>(region.bytes[offset + byte]) << (byte * 8U);
    return value;
}

std::uint64_t AddressSpace::read_u64(std::uint64_t address, Permission access) const {
    const Region& region = find_region(address, sizeof(std::uint64_t), access);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < 8U; ++byte)
        value |= static_cast<std::uint64_t>(region.bytes[offset + byte]) << (byte * 8U);
    return value;
}

std::size_t AddressSpace::region_count() const noexcept { return regions_.size(); }

} // namespace horizon
