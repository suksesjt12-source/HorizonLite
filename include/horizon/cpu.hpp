#pragma once

#include "horizon/memory.hpp"

#include <cstddef>
#include <cstdint>

namespace horizon {

struct CpuState {
    std::uint64_t x[31]{};
    std::uint64_t sp{};
    std::uint64_t pc{};
    std::uint32_t pstate{};
};

class Arm64Cpu {
public:
    explicit Arm64Cpu(AddressSpace& memory) noexcept;

    [[nodiscard]] CpuState& state() noexcept;
    [[nodiscard]] const CpuState& state() const noexcept;
    void step();
    [[nodiscard]] std::size_t run(std::size_t max_steps,
                                  std::uint64_t stop_pc);

private:
    [[nodiscard]] std::uint64_t read_x(unsigned index) const;
    void write_x(unsigned index, std::uint64_t value);

    AddressSpace& memory_;
    CpuState state_{};
};

} // namespace horizon
