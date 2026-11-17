#include "horizon/memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace horizon {

void AddressSpace::map(std::uint64_t base, std::vector<std::uint8_t> bytes,
                       Permission permissions) {
    if (bytes.empty()) {
        throw std::invalid_argument("cannot map an empty memory region");
    }
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - base) {
        throw std::overflow_error("memory region address overflow");
    }

    const std::uint64_t end = base + static_cast<std::uint64_t>(bytes.size());
    for (const Region& region : regions_) {
        const std::uint64_t other_end =
            region.base + static_cast<std::uint64_t>(region.bytes.size());
        if (base < other_end && region.base < end) {
            throw std::runtime_error("memory region overlaps an existing mapping");
        }
    }

    regions_.push_back(Region{base, std::move(bytes), permissions});
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& lhs, const Region& rhs) {
                  return lhs.base < rhs.base;
              });
}

void AddressSpace::map_zeroed(std::uint64_t base, std::size_t size,
                              Permission permissions) {
    if (size == 0) {
        return;
    }
    map(base, std::vector<std::uint8_t>(size, 0), permissions);
}

const AddressSpace::Region& AddressSpace::find_region(
    std::uint64_t address, std::size_t size, Permission access) const {
    for (const Region& region : regions_) {
        if (address < region.base) {
            continue;
        }
        const std::uint64_t offset64 = address - region.base;
        if (offset64 > region.bytes.size()) {
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(offset64);
        if (size > region.bytes.size() - offset) {
            continue;
        }
        if (!has_permission(region.permissions, access)) {
            throw std::runtime_error("memory access violates region permissions");
        }
        return region;
    }
    throw std::runtime_error("memory access is outside mapped regions");
}

AddressSpace::Region& AddressSpace::find_region(
    std::uint64_t address, std::size_t size, Permission access) {
    for (Region& region : regions_) {
        if (address < region.base) {
            continue;
        }
        const std::uint64_t offset64 = address - region.base;
        if (offset64 > region.bytes.size()) {
            continue;
        }
        const std::size_t offset = static_cast<std::size_t>(offset64);
        if (size > region.bytes.size() - offset) {
            continue;
        }
        if (!has_permission(region.permissions, access)) {
            throw std::runtime_error("memory access violates region permissions");
        }
        return region;
    }
    throw std::runtime_error("memory access is outside mapped regions");
}

std::vector<std::uint8_t> AddressSpace::read(std::uint64_t address,
                                            std::size_t size,
                                            Permission access) const {
    const Region& region = find_region(address, size, access);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    return {region.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            region.bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
}

void AddressSpace::write(std::uint64_t address,
                         const std::vector<std::uint8_t>& bytes) {
    Region& region = find_region(address, bytes.size(), Permission::Write);
    const std::size_t offset = static_cast<std::size_t>(address - region.base);
    std::copy(bytes.begin(), bytes.end(),
              region.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::uint32_t AddressSpace::read_u32(std::uint64_t address,
                                     Permission access) const {
    const auto bytes = read(address, 4, access);
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::size_t AddressSpace::region_count() const noexcept {
    return regions_.size();
}

} // namespace horizon
