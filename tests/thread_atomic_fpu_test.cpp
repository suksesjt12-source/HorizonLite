#include "horizon/audio.hpp"
#include "horizon/cpu.hpp"
#include "horizon/kernel.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    try {
        horizon::AddressSpace memory;
        const std::array<std::uint32_t, 33> program{
            0xD2800001U, // MOVZ X1, #0
            0xF2A00041U, // MOVK X1, #2, LSL #16 -> 0x20000
            0xD2800080U, // MOVZ X0, #4
            0xB9000020U, // STR W0, [X1]
            0x885FFC22U, // LDAXR W2, [X1]
            0xD2800123U, // MOVZ X3, #9
            0x8804FC23U, // STLXR W4, W3, [X1]
            0xB9000424U, // STR W4, [X1, #4] (successful status)
            0x885FFC22U, // LDAXR W2, [X1]
            0xD2800166U, // MOVZ X6, #11
            0xB9000026U, // STR W6, [X1] invalidates reservation
            0x8804FC23U, // STLXR W4, W3, [X1] must fail
            0xB9000824U, // STR W4, [X1, #8] (failed status)
            0x885FFC22U, // LDAXR W2, [X1]
            0xD5033F5FU, // CLREX
            0x8804FC23U, // STLXR W4, W3, [X1] must fail after CLREX
            0xB9000C24U, // STR W4, [X1, #12]
            0x91004022U, // ADD X2, X1, #16
            0xD2824683U, // MOVZ X3, #0x1234
            0xF9000043U, // STR X3, [X2]
            0xC85FFC44U, // LDAXR X4, [X2]
            0xD2886425U, // MOVZ X5, #0x4321
            0xC806FC45U, // STLXR W6, X5, [X2]
            0xF9400047U, // LDR X7, [X2]
            0xF9000C27U, // STR X7, [X1, #24]
            0xB9001426U, // STR W6, [X1, #20]
            0xD2800145U, // MOVZ X5, #10
            0x1E2200A0U, // SCVTF S0, W5
            0x1E2C1001U, // FMOV S1, #0.5
            0x1E210802U, // FMUL S2, S0, S1
            0x1E212843U, // FADD S3, S2, S1
            0x1E380066U, // FCVTZS W6, S3
            0x1E212060U, // FCMP S3, S1
        };
        std::vector<std::uint32_t> instructions(program.begin(), program.end());
        const std::array<std::uint32_t, 10> double_program{
            0x9E6200A4U, // SCVTF D4, X5
            0x1E6C1005U, // FMOV D5, #0.5
            0x1E650886U, // FMUL D6, D4, D5
            0x1E6528C7U, // FADD D7, D6, D5
            0x9E7800E8U, // FCVTZS X8, D7
            0x1E6520E0U, // FCMP D7, D5
            0x9E6600E9U, // FMOV X9, D7
            0x9E670128U, // FMOV D8, X9
            0x1E260067U, // FMOV W7, S3
            0x1E2700E4U  // FMOV S4, W7
        };
        instructions.insert(instructions.end(),
                            double_program.begin(), double_program.end());
        std::vector<std::uint8_t> code;
        code.reserve(instructions.size() * 4U + 4U);
        for (const std::uint32_t word : instructions) {
            for (unsigned byte = 0U; byte < 4U; ++byte)
                code.push_back(static_cast<std::uint8_t>(word >> (byte * 8U)));
        }
        code.insert(code.end(), {0xE1U, 0x00U, 0x00U, 0xD4U});

        memory.map(0x10000U, std::move(code),
                   horizon::Permission::Read | horizon::Permission::Execute);
        memory.map_zeroed(0x20000U, 0x1000U,
                          horizon::Permission::Read | horizon::Permission::Write);

        horizon::SdlAudioOutput audio(true);
        horizon::HorizonKernel kernel(memory, {}, audio);
        horizon::SvcHandler svc_handler(kernel);
        horizon::Arm64Cpu cpu(memory, svc_handler);
        cpu.state().pc = 0x10000U;
        const std::size_t executed =
            cpu.run(instructions.size() + 1U, UINT64_MAX);

        const bool atomics_ok =
            memory.read_u32(0x20000U) == 11U &&
            memory.read_u32(0x20004U) == 0U &&
            memory.read_u32(0x20008U) == 1U &&
            memory.read_u32(0x2000CU) == 1U &&
            memory.read_u64(0x20010U) == 0x4321U &&
            memory.read_u32(0x20014U) == 0U &&
            memory.read_u64(0x20018U) == 0x4321U &&
            !cpu.state().exclusive_monitor.valid;

        const bool single_fp_ok =
            cpu.state().v[3] == std::bit_cast<std::uint32_t>(5.5F) &&
            cpu.state().v[4] == std::bit_cast<std::uint32_t>(5.5F);
        const bool double_fp_ok =
            cpu.state().x[8] == 5U &&
            cpu.state().v[8] == std::bit_cast<std::uint64_t>(5.5);

        const bool compare_flags_ok =
            (cpu.state().pstate & 0xF0000000U) == 0x20000000U;
        if (!atomics_ok || !single_fp_ok || !double_fp_ok ||
            !compare_flags_ok || kernel.running()) {
            std::cerr << "Thread/atomic/FPU CPU test failed; executed=" << executed
                      << " mem0=" << memory.read_u32(0x20000U)
                      << " stxr_success=" << memory.read_u32(0x20004U)
                      << " stxr_failure=" << memory.read_u32(0x20008U)
                      << " clrex_failure=" << memory.read_u32(0x2000CU)
                      << " x8=" << cpu.state().x[8]
                      << " NZCV=0x" << std::hex
                      << (cpu.state().pstate & 0xF0000000U) << std::dec << '\n';
            return 1;
        }

        std::cout << "ARM64 LDXR/STXR/CLREX and scalar FP conversion/arithmetic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Thread/atomic/FPU test failed: " << error.what() << '\n';
        return 1;
    }
}
