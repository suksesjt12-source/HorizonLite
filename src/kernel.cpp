#include "horizon/kernel.hpp"

#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>

namespace horizon {

HorizonKernel::HorizonKernel(AddressSpace& memory) noexcept : memory_(memory) {}

void HorizonKernel::svc_set_heap_size(CpuState& state) {
    const std::uint64_t requested_size = state.x[1];
    if (requested_size > kMaximumHeapSize ||
        requested_size > std::numeric_limits<std::size_t>::max()) {
        state.x[0] = 0xF601ULL;
        state.x[1] = 0;
        return;
    }

    if (requested_size > heap_size_) {
        if (heap_size_ != 0) {
            state.x[0] = 0xF601ULL;
            state.x[1] = 0;
            return;
        }
        try {
            memory_.map_zeroed(kHeapBase, static_cast<std::size_t>(requested_size),
                               Permission::Read | Permission::Write);
            heap_size_ = requested_size;
        } catch (const std::exception&) {
            state.x[0] = 0xF601ULL;
            state.x[1] = 0;
            return;
        }
    }

    state.x[0] = 0;
    state.x[1] = kHeapBase;
}

void HorizonKernel::svc_exit_process(CpuState& state) noexcept {
    state.x[0] = 0;
    running_ = false;
}

void HorizonKernel::svc_break(CpuState& state) {
    std::cerr << "\n[HorizonKernel] svcBreak register dump\n";
    for (unsigned index = 0; index < 31U; ++index) {
        std::cerr << "X" << std::dec << std::setw(2) << std::setfill('0')
                  << index << " = 0x" << std::hex << std::setw(16)
                  << std::setfill('0') << state.x[index];
        if ((index % 2U) == 1U || index == 30U) {
            std::cerr << '\n';
        } else {
            std::cerr << "    ";
        }
    }
    std::cerr << "SP  = 0x" << std::hex << std::setw(16) << std::setfill('0')
              << state.sp << '\n'
              << "PC  = 0x" << std::hex << std::setw(16) << std::setfill('0')
              << state.pc << std::dec << "\n[HorizonKernel] stopped\n";
    running_ = false;
}

void HorizonKernel::svc_output_debug_string(CpuState& state) {
    constexpr std::uint64_t kMaximumDebugString = 1024U * 1024U;
    const std::uint64_t address = state.x[0];
    const std::uint64_t length = state.x[1];
    if (length > kMaximumDebugString ||
        length > std::numeric_limits<std::size_t>::max()) {
        state.x[0] = 0xF601ULL;
        return;
    }

    try {
        const auto bytes = memory_.view(address, static_cast<std::size_t>(length),
                                        Permission::Read);
        std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        state.x[0] = 0;
    } catch (const std::exception&) {
        state.x[0] = 0xF601ULL;
    }
}

bool HorizonKernel::running() const noexcept { return running_; }

SvcHandler::SvcHandler(HorizonKernel& kernel) noexcept : kernel_(kernel) {}

void SvcHandler::dispatch(std::uint16_t immediate, CpuState& state) {
    switch (immediate) {
    case 0x01:
        kernel_.svc_set_heap_size(state);
        break;
    case 0x07:
        kernel_.svc_exit_process(state);
        break;
    case 0x26:
        kernel_.svc_break(state);
        break;
    case 0x27:
        kernel_.svc_output_debug_string(state);
        break;
    default:
        state.x[0] = 0xF001ULL;
        std::cerr << "[HorizonKernel] unimplemented SVC 0x" << std::hex
                  << immediate << std::dec << '\n';
        break;
    }
}

bool SvcHandler::running() const noexcept { return kernel_.running(); }

} // namespace horizon
