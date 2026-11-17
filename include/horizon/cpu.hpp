#pragma once

#include "horizon/memory.hpp"

#include <cstddef>
#include <cstdint>

namespace horizon {

class SvcHandler;

struct CpuState {
    std::uint64_t x[31]{};
    std::uint64_t sp{};
    std::uint64_t pc{};
    std::uint32_t pstate{};
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
    void write_nzcv_sub(std::uint64_t lhs, std::uint64_t rhs,
                        std::uint64_t result) noexcept;
    [[nodiscard]] bool condition_passed(unsigned condition) const noexcept;

    AddressSpace& memory_;
    SvcHandler& svc_handler_;
    CpuState state_{};
};

} // namespace horizon
