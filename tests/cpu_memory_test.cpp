#include "horizon/cpu.hpp"
#include "horizon/kernel.hpp"
#include "horizon/audio.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

int main() {
    horizon::AddressSpace memory;
    const std::array<std::uint32_t, 21> program{
        0xD2800001U, // MOVZ X1, #0
        0xF2A00041U, // MOVK X1, #2, LSL #16 (0x20000)
        0xD2824680U, // MOVZ X0, #0x1234
        0xF9000C20U, // STR X0, [X1, #24]
        0xF9400C23U, // LDR X3, [X1, #24]
        0xD28AC005U, // MOVZ X5, #0x5600
        0xF2AB0005U, // MOVK X5, #0x5800, LSL #16
        0xB9002425U, // STR W5, [X1, #36]
        0xB9402426U, // LDR W6, [X1, #36]
        0xD2801567U, // MOVZ X7, #0xAB
        0x3900A027U, // STRB W7, [X1, #40]
        0x3940A028U, // LDRB W8, [X1, #40]
        0x1283E7E9U, // MOVN W9, #0x1F3F (-8000)
        0x79005429U, // STRH W9, [X1, #42]
        0xD280002BU, // MOVZ X11, #1
        0xCA0B012AU, // EOR X10, X9, X11
        0xEB03001FU, // CMP X0, X3
        0x54000061U, // B.NE +3 (to failure marker)
        0xD2800544U, // MOVZ X4, #42
        0x14000002U, // B +2 (to SVC exit)
        0xD2800C64U, // MOVZ X4, #99
    };
    std::vector<std::uint8_t> code;
    code.reserve(program.size() * 4U + 4U);
    for (const std::uint32_t word : program) {
        for (unsigned byte = 0; byte < 4U; ++byte) {
            code.push_back(static_cast<std::uint8_t>(word >> (byte * 8U)));
        }
    }
    code.insert(code.end(), {0xE1U, 0x00U, 0x00U, 0xD4U}); // SVC #7

    memory.map(0x10000, std::move(code),
               horizon::Permission::Read | horizon::Permission::Execute);
    memory.map_zeroed(0x20000, 0x1000,
                      horizon::Permission::Read | horizon::Permission::Write);

    horizon::SdlAudioOutput audio(true);
    horizon::HorizonKernel kernel(memory, {}, audio);
    horizon::SvcHandler svc_handler(kernel);
    horizon::Arm64Cpu cpu(memory, svc_handler);
    cpu.state().pc = 0x10000;
    const std::size_t executed = cpu.run(100, UINT64_MAX);

    const bool passed = executed == 21U &&
        memory.read_u64(0x20000 + 24U) == 0x1234U &&
        memory.read_u32(0x20000 + 36U) == 0x58005600U &&
        cpu.state().x[3] == 0x1234U && cpu.state().x[6] == 0x58005600U &&
        cpu.state().x[4] == 42U && cpu.state().x[8] == 0xABU &&
        memory.read_u8(0x20000 + 40U) == 0xABU &&
        cpu.state().x[9] == 0xFFFFE0C0ULL &&
        cpu.state().x[10] == 0xFFFFE0C1ULL &&
        memory.read_u16(0x20000 + 42U) == 0xE0C0U && !kernel.running();

    if (!passed) {
        std::cerr << "CPU memory/branch test failed; executed=" << executed
                  << " X3=0x" << std::hex << cpu.state().x[3]
                  << " X4=0x" << cpu.state().x[4]
                  << " X6=0x" << cpu.state().x[6] << '\n';
        return 1;
    }

    std::cout << "CPU MOVN/EOR and byte/halfword/word/doubleword memory test passed\n";
    return 0;
}
