#include "horizon/cpu.hpp"
#include "horizon/display.hpp"
#include "horizon/kernel.hpp"
#include "horizon/nro_loader.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kFramebufferAddress = 0x40000000ULL;
constexpr int kFramebufferWidth = 1280;
constexpr int kFramebufferHeight = 720;
constexpr std::size_t kFramebufferBytes =
    static_cast<std::size_t>(kFramebufferWidth) *
    static_cast<std::size_t>(kFramebufferHeight) * 4U;
constexpr std::uint64_t kStackAddress = 0x60000000ULL;
constexpr std::size_t kStackSize = 1024U * 1024U;
constexpr std::uint64_t kSelfTestTextAddress = 0x00100000ULL;
constexpr std::uint64_t kSelfTestMessageAddress = 0x00110000ULL;
constexpr std::uint64_t kStopAddress = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kInteractiveInstructionBudget = 250000U;
constexpr std::size_t kHeadlessInstructionLimit = 20000000U;

constexpr char kSelfTestMessage[] =
    "HorizonLite Stage 2: SVC + RGBA framebuffer\n";

// Raw AArch64 instruction words. The loop stores one RGBA8888 pixel per pass.
// Register assignments: X19=framebuffer pointer, X20=pixel index,
// X22=pixel color, X23=color increment, X24=pixel count (1280*720).
constexpr std::array<std::uint32_t, 22> kSelfTestProgram{
    0xD2840001U, // MOVZ X1, #0x2000
    0xD4000021U, // SVC #0x01 (SetHeapSize)
    0xD2800000U, // MOVZ X0, #0
    0xF2A00220U, // MOVK X0, #0x0011, LSL #16  -> 0x00110000
    0xD2800581U, // MOVZ X1, #44
    0xD40004E1U, // SVC #0x27 (OutputDebugString)
    0xD2800013U, // MOVZ X19, #0
    0xF2A80013U, // MOVK X19, #0x4000, LSL #16 -> 0x40000000
    0xD2800014U, // MOVZ X20, #0
    0xD2800016U, // MOVZ X22, #0
    0xF2BFE016U, // MOVK X22, #0xFF00, LSL #16 -> 0xFF000000
    0xD2800037U, // MOVZ X23, #1
    0xF2A00017U, // MOVK X23, #0, LSL #16 -> color increment remains 1
    0xD2820018U, // MOVZ X24, #0x1000
    0xF2A001D8U, // MOVK X24, #0x000E, LSL #16 -> 0x000E1000
    0xB9000276U, // STR W22, [X19]
    0x91001273U, // ADD X19, X19, #4
    0x91000694U, // ADD X20, X20, #1
    0x8B1702D6U, // ADD X22, X22, X23
    0xEB18029FU, // CMP X20, X24 (SUBS XZR, X20, X24)
    0x54FFFF61U, // B.NE back five instructions to STR W22, [X19]
    0xD40000E1U, // SVC #0x07 (ExitProcess)
};

struct Options {
    std::optional<std::string> nro_path;
    bool headless{};
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [file.nro] [--headless]\n"
              << "  No file: run the built-in SVC/framebuffer self-test.\n"
              << "  file.nro: load and run an uncompressed NRO image.\n"
              << "  --headless: run without opening an SDL2 window (for CI/tests).\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (!argument.empty() && argument[0] == '-') {
            throw std::invalid_argument("unknown option: " + argument);
        } else if (!options.nro_path.has_value()) {
            options.nro_path = argument;
        } else {
            throw std::invalid_argument("only one NRO path can be specified");
        }
    }
    return options;
}

void initialize_machine_memory(horizon::AddressSpace& memory) {
    memory.map_zeroed(kFramebufferAddress, kFramebufferBytes,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kStackAddress, kStackSize,
                      horizon::Permission::Read | horizon::Permission::Write);
}

std::uint64_t load_self_test(horizon::AddressSpace& memory) {
    std::vector<std::uint8_t> text;
    text.reserve(kSelfTestProgram.size() * sizeof(std::uint32_t));
    for (const std::uint32_t instruction : kSelfTestProgram) {
        for (unsigned byte = 0; byte < 4U; ++byte) {
            text.push_back(static_cast<std::uint8_t>(
                (instruction >> (byte * 8U)) & 0xFFU));
        }
    }

    std::vector<std::uint8_t> message(kSelfTestMessage,
                                      kSelfTestMessage + sizeof(kSelfTestMessage) - 1U);
    memory.map(kSelfTestTextAddress, std::move(text),
               horizon::Permission::Read | horizon::Permission::Execute);
    memory.map(kSelfTestMessageAddress, std::move(message),
               horizon::Permission::Read);
    return kSelfTestTextAddress;
}

std::uint64_t load_nro(const std::string& path,
                       horizon::AddressSpace& memory) {
    const auto file = horizon::NroLoader::read_file(path);
    const horizon::NroImage image = horizon::NroLoader::load(file, memory);
    std::cout << "Loaded NRO0 image: " << path << '\n'
              << "  Declared size: 0x" << std::hex << image.declared_size << '\n'
              << "  Entry point:   0x" << image.entry_point << std::dec << '\n';
    return image.entry_point;
}

void verify_self_test(const horizon::AddressSpace& memory) {
    const std::uint32_t first_pixel = memory.read_u32(kFramebufferAddress);
    const std::uint32_t final_pixel = memory.read_u32(
        kFramebufferAddress + static_cast<std::uint64_t>(kFramebufferBytes - 4U));
    if (first_pixel != 0xFF000000U || final_pixel == first_pixel) {
        throw std::runtime_error("framebuffer self-test produced an unexpected pixel pattern");
    }

    const auto framebuffer = memory.view(kFramebufferAddress,
                                           kFramebufferBytes);
    std::uint64_t checksum = 1469598103934665603ULL;
    for (const std::uint8_t byte : framebuffer) {
        checksum ^= byte;
        checksum *= 1099511628211ULL;
    }
    std::cout << "Framebuffer self-test passed\n"
              << "  First pixel: 0x" << std::hex << first_pixel << '\n'
              << "  Last pixel:  0x" << final_pixel << '\n'
              << "  FNV-1a64:    0x" << checksum << std::dec << '\n';
}

int run_headless(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel,
                 const horizon::AddressSpace& memory, bool self_test) {
    const std::size_t executed = cpu.run(kHeadlessInstructionLimit, kStopAddress);
    std::cout << "Executed instructions: " << executed << '\n';
    if (kernel.running()) {
        std::cerr << "Execution reached the headless instruction limit.\n";
        return 3;
    }
    if (self_test) {
        verify_self_test(memory);
    }
    return 0;
}

int run_interactive(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel,
                    const horizon::AddressSpace& memory) {
    horizon::SdlDisplay display;
    constexpr auto frame_duration = std::chrono::microseconds(16667);

    bool keep_open = true;
    while (keep_open) {
        const auto frame_start = std::chrono::steady_clock::now();
        keep_open = display.process_events();
        if (!keep_open) {
            break;
        }

        if (kernel.running()) {
            (void)cpu.run(kInteractiveInstructionBudget, kStopAddress);
        }
        display.render(memory, kFramebufferAddress,
                       kFramebufferWidth, kFramebufferHeight);

        std::this_thread::sleep_until(frame_start + frame_duration);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        horizon::AddressSpace memory;
        initialize_machine_memory(memory);

        const bool self_test = !options.nro_path.has_value();
        const std::uint64_t entry_point = self_test
            ? load_self_test(memory)
            : load_nro(*options.nro_path, memory);

        horizon::HorizonKernel kernel(memory);
        horizon::SvcHandler svc_handler(kernel);
        horizon::Arm64Cpu cpu(memory, svc_handler);
        cpu.state().pc = entry_point;
        cpu.state().sp = kStackAddress + kStackSize - 16U;

        if (options.headless) {
            return run_headless(cpu, kernel, memory, self_test);
        }
        return run_interactive(cpu, kernel, memory);
    } catch (const std::exception& error) {
        std::cerr << "HorizonLite: " << error.what() << '\n';
        return 1;
    }
}
