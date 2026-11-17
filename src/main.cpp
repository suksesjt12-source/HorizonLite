#include "horizon/cpu.hpp"
#include "horizon/nro_loader.hpp"

#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " <file.nro> [--run]\n"
              << "  Default: validate and map an uncompressed NRO image.\n"
              << "  --run: execute the small supported AArch64 instruction subset.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return argc < 2 ? 0 : 2;
    }

    const std::string path = argv[1];
    const bool run_code = argc == 3 && std::string(argv[2]) == "--run";
    if (argc == 3 && !run_code) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const auto file = horizon::NroLoader::read_file(path);
        horizon::AddressSpace memory;
        const horizon::NroImage image = horizon::NroLoader::load(file, memory);

        std::cout << "NRO0 image validated\n"
                  << "  Declared size: 0x" << std::hex << image.declared_size << "\n"
                  << "  Entry point:   0x" << image.entry_point << "\n"
                  << "  .text:         offset=0x" << image.segments[0].file_offset
                  << " size=0x" << image.segments[0].size << "\n"
                  << "  .rodata:       offset=0x" << image.segments[1].file_offset
                  << " size=0x" << image.segments[1].size << "\n"
                  << "  .data:         offset=0x" << image.segments[2].file_offset
                  << " size=0x" << image.segments[2].size << "\n"
                  << "  BSS size:      0x" << image.bss_size << std::dec << "\n"
                  << "  Mapped regions: " << memory.region_count() << "\n";

        if (run_code) {
            horizon::Arm64Cpu cpu(memory);
            constexpr std::uint64_t stop_pc =
                std::numeric_limits<std::uint64_t>::max();
            cpu.state().pc = image.entry_point;
            cpu.state().x[30] = stop_pc;
            const std::size_t executed = cpu.run(10000, stop_pc);
            std::cout << "Executed instructions: " << executed << "\n"
                      << "X0: 0x" << std::hex << cpu.state().x[0]
                      << std::dec << "\n";
            if (cpu.state().pc != stop_pc) {
                std::cerr << "Execution stopped at the instruction limit.\n";
                return 3;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HorizonLite: " << error.what() << '\n';
        return 1;
    }
}
