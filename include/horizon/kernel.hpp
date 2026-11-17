#pragma once

#include "horizon/cpu.hpp"
#include "horizon/memory.hpp"

#include <cstdint>

namespace horizon {

class HorizonKernel {
public:
    explicit HorizonKernel(AddressSpace& memory) noexcept;

    void svc_set_heap_size(CpuState& state);
    void svc_exit_process(CpuState& state) noexcept;
    void svc_break(CpuState& state);
    void svc_output_debug_string(CpuState& state);

    [[nodiscard]] bool running() const noexcept;

private:
    static constexpr std::uint64_t kHeapBase = 0x50000000ULL;
    static constexpr std::uint64_t kMaximumHeapSize = 0x10000000ULL;

    AddressSpace& memory_;
    std::uint64_t heap_size_{};
    bool running_{true};
};

class SvcHandler {
public:
    explicit SvcHandler(HorizonKernel& kernel) noexcept;

    void dispatch(std::uint16_t immediate, CpuState& state);
    [[nodiscard]] bool running() const noexcept;

private:
    HorizonKernel& kernel_;
};

} // namespace horizon
