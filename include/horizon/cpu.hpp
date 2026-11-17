#pragma once

#include "horizon/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace horizon {

class SvcHandler;

struct ExclusiveMonitor {
    bool valid{};
    std::uint64_t address{};
    std::size_t size{};

    void clear() noexcept {
        valid = false;
        address = 0U;
        size = 0U;
    }

    [[nodiscard]] bool matches(std::uint64_t target,
                               std::size_t access_size) const noexcept {
        return valid && address == target && size == access_size;
    }

    void invalidate(std::uint64_t target,
                    std::size_t access_size) noexcept {
        if (!valid) return;
        const std::uint64_t end = address + size;
        const std::uint64_t write_end = target + access_size;
        if (target < end && address < write_end) clear();
    }
};

struct CpuState {
    std::uint64_t x[31]{};
    std::array<std::uint64_t, 32> v{};
    std::uint64_t sp{};
    std::uint64_t pc{};
    std::uint64_t tpidrro_el0{0x10000000ULL};
    std::uint32_t pstate{};
    ExclusiveMonitor exclusive_monitor{};
};

class Arm64Cpu {
public:
    Arm64Cpu(AddressSpace& memory, SvcHandler& svc_handler) noexcept;

    [[nodiscard]] CpuState& state() noexcept;
    [[nodiscard]] const CpuState& state() const noexcept;
    void step();
    [[nodiscard]] std::size_t run(std::size_t max_steps,
                                  std::uint64_t stop_pc);

private:
    [[nodiscard]] std::uint64_t read_x(unsigned index) const noexcept;
    [[nodiscard]] std::uint64_t read_base(unsigned index) const noexcept;
    void write_x(unsigned index, std::uint64_t value) noexcept;
    void invalidate_exclusive(std::uint64_t address,
                              std::size_t size) noexcept;
    void write_nzcv_sub(std::uint64_t lhs, std::uint64_t rhs,
                        std::uint64_t result) noexcept;
    [[nodiscard]] bool condition_passed(unsigned condition) const noexcept;

    AddressSpace& memory_;
    SvcHandler& svc_handler_;
    CpuState state_{};
};

} // namespace horizon
