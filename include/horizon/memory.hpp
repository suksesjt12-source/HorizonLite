#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace horizon {

enum class Permission : std::uint8_t {
    None = 0,
    Read = 1U << 0U,
    Write = 1U << 1U,
    Execute = 1U << 2U,
};

constexpr Permission operator|(Permission lhs, Permission rhs) noexcept {
    return static_cast<Permission>(static_cast<std::uint8_t>(lhs) |
                                   static_cast<std::uint8_t>(rhs));
}

constexpr bool has_permission(Permission value, Permission requested) noexcept {
    return (static_cast<std::uint8_t>(value) &
            static_cast<std::uint8_t>(requested)) ==
           static_cast<std::uint8_t>(requested);
}

class AddressSpace {
public:
    void map(std::uint64_t base, std::vector<std::uint8_t> bytes,
             Permission permissions);
    void map_zeroed(std::uint64_t base, std::size_t size,
                    Permission permissions);
    void unmap(std::uint64_t base, std::size_t size);

    [[nodiscard]] std::vector<std::uint8_t> read(
        std::uint64_t address, std::size_t size,
        Permission access = Permission::Read) const;
    [[nodiscard]] std::span<const std::uint8_t> view(
        std::uint64_t address, std::size_t size,
        Permission access = Permission::Read) const;
    void write(std::uint64_t address, const std::vector<std::uint8_t>& bytes);
    void write_u8(std::uint64_t address, std::uint8_t value);
    void write_u16(std::uint64_t address, std::uint16_t value);
    void write_u32(std::uint64_t address, std::uint32_t value);
    void write_u64(std::uint64_t address, std::uint64_t value);
    [[nodiscard]] std::uint16_t read_u16(
        std::uint64_t address,
        Permission access = Permission::Read) const;
    [[nodiscard]] std::uint8_t read_u8(
        std::uint64_t address,
        Permission access = Permission::Read) const;
    [[nodiscard]] std::uint32_t read_u32(
        std::uint64_t address,
        Permission access = Permission::Read) const;
    [[nodiscard]] std::uint64_t read_u64(
        std::uint64_t address,
        Permission access = Permission::Read) const;

    [[nodiscard]] std::size_t region_count() const noexcept;

private:
    struct Region {
        std::uint64_t base{};
        std::vector<std::uint8_t> bytes;
        Permission permissions{Permission::None};
    };

    [[nodiscard]] const Region& find_region(
        std::uint64_t address, std::size_t size, Permission access) const;
    [[nodiscard]] Region& find_region(
        std::uint64_t address, std::size_t size, Permission access);

    std::vector<Region> regions_;
};

} // namespace horizon
