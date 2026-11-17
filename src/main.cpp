#include "horizon/cpu.hpp"
#include "horizon/display.hpp"
#include "horizon/kernel.hpp"
#include "horizon/nro_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
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
constexpr std::uint64_t kTlsAddress = 0x10000000ULL;
constexpr std::size_t kTlsSize = 0x1000U;
constexpr std::uint64_t kSelfTestTextAddress = 0x00100000ULL;
constexpr std::uint64_t kSelfTestPortNameAddress = 0x00110000ULL;
constexpr std::uint64_t kHidMappingAddress = 0x20000000ULL;
constexpr std::uint64_t kStopAddress = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kInteractiveInstructionBudget = 180000U;
constexpr std::size_t kHeadlessInstructionLimit = 1000000U;

class A64Assembler {
public:
    void emit(std::uint32_t instruction) { words_.push_back(instruction); }

    void bind(const std::string& label) {
        if (labels_.contains(label)) throw std::runtime_error("duplicate assembly label: " + label);
        labels_.emplace(label, words_.size());
    }

    void movz(unsigned rd, std::uint16_t immediate, unsigned shift = 0U) {
        emit(0xD2800000U | ((shift / 16U) << 21U) |
             (static_cast<std::uint32_t>(immediate) << 5U) | rd);
    }

    void movk(unsigned rd, std::uint16_t immediate, unsigned shift = 0U) {
        emit(0xF2800000U | ((shift / 16U) << 21U) |
             (static_cast<std::uint32_t>(immediate) << 5U) | rd);
    }

    void mov64(unsigned rd, std::uint64_t value) {
        movz(rd, static_cast<std::uint16_t>(value));
        for (unsigned shift = 16U; shift < 64U; shift += 16U) {
            const auto part = static_cast<std::uint16_t>(value >> shift);
            if (part != 0U) movk(rd, part, shift);
        }
    }

    void mov_reg(unsigned rd, unsigned rm) {
        emit(0xAA0003E0U | (rm << 16U) | rd);
    }

    void add_imm(unsigned rd, unsigned rn, unsigned immediate,
                 bool shift12 = false) {
        const unsigned encoded = shift12 ? immediate / 4096U : immediate;
        emit(0x91000000U | (static_cast<std::uint32_t>(shift12) << 22U) |
             (encoded << 10U) | (rn << 5U) | rd);
    }

    void sub_imm(unsigned rd, unsigned rn, unsigned immediate) {
        emit(0xD1000000U | (immediate << 10U) | (rn << 5U) | rd);
    }

    void subs_imm(unsigned rd, unsigned rn, unsigned immediate) {
        emit(0xF1000000U | (immediate << 10U) | (rn << 5U) | rd);
    }

    void add_reg(unsigned rd, unsigned rn, unsigned rm) {
        emit(0x8B000000U | (rm << 16U) | (rn << 5U) | rd);
    }

    void lsl(unsigned rd, unsigned rn, unsigned amount) {
        const unsigned immr = (64U - amount) & 63U;
        const unsigned imms = 63U - amount;
        emit(0xD3400000U | (immr << 16U) | (imms << 10U) |
             (rn << 5U) | rd);
    }

    void lsr(unsigned rd, unsigned rn, unsigned amount) {
        emit(0xD340FC00U | (amount << 16U) | (rn << 5U) | rd);
    }

    void and_imm(unsigned rd, unsigned rn, unsigned immr, unsigned imms) {
        emit(0x92400000U | (immr << 16U) | (imms << 10U) |
             (rn << 5U) | rd);
    }

    void str_w(unsigned rt, unsigned rn, unsigned offset) {
        emit(0xB9000000U | ((offset / 4U) << 10U) | (rn << 5U) | rt);
    }

    void ldr_w(unsigned rt, unsigned rn, unsigned offset) {
        emit(0xB9400000U | ((offset / 4U) << 10U) | (rn << 5U) | rt);
    }

    void str_x(unsigned rt, unsigned rn, unsigned offset) {
        emit(0xF9000000U | ((offset / 8U) << 10U) | (rn << 5U) | rt);
    }

    void ldr_x(unsigned rt, unsigned rn, unsigned offset) {
        emit(0xF9400000U | ((offset / 8U) << 10U) | (rn << 5U) | rt);
    }

    void svc(unsigned immediate) { emit(0xD4000001U | (immediate << 5U)); }
    void mrs_tpidrro(unsigned rd) { emit(0xD53BD060U | rd); }
    void ret() { emit(0xD65F03C0U); }

    void branch(const std::string& label) {
        fixups_.push_back({words_.size(), label, 0x14000000U, 26U, 0U});
        emit(0x14000000U);
    }

    void branch_link(const std::string& label) {
        fixups_.push_back({words_.size(), label, 0x94000000U, 26U, 0U});
        emit(0x94000000U);
    }

    void cbz(unsigned rt, const std::string& label) {
        fixups_.push_back({words_.size(), label, 0xB4000000U | rt, 19U, 0U});
        emit(0xB4000000U | rt);
    }

    void cbnz(unsigned rt, const std::string& label) {
        fixups_.push_back({words_.size(), label, 0xB5000000U | rt, 19U, 0U});
        emit(0xB5000000U | rt);
    }

    void bcond(unsigned condition, const std::string& label) {
        fixups_.push_back({words_.size(), label, 0x54000000U | condition, 19U, condition});
        emit(0x54000000U | condition);
    }

    [[nodiscard]] std::vector<std::uint32_t> finish() {
        for (const Fixup& fixup : fixups_) {
            const auto found = labels_.find(fixup.label);
            if (found == labels_.end())
                throw std::runtime_error("undefined assembly label: " + fixup.label);
            const std::int64_t delta =
                static_cast<std::int64_t>(found->second) -
                static_cast<std::int64_t>(fixup.instruction_index);
            if (fixup.immediate_bits == 26U) {
                if (delta < -(1LL << 25U) || delta >= (1LL << 25U))
                    throw std::runtime_error("AArch64 branch target is out of range");
                words_[fixup.instruction_index] =
                    fixup.base | (static_cast<std::uint32_t>(delta) & 0x03FFFFFFU);
            } else {
                if (delta < -(1LL << 18U) || delta >= (1LL << 18U))
                    throw std::runtime_error("AArch64 conditional branch target is out of range");
                words_[fixup.instruction_index] =
                    (fixup.base & ~0x00FFFFE0U) |
                    ((static_cast<std::uint32_t>(delta) & 0x7FFFFU) << 5U) |
                    fixup.condition;
            }
        }
        return words_;
    }

private:
    struct Fixup {
        std::size_t instruction_index{};
        std::string label;
        std::uint32_t base{};
        unsigned immediate_bits{};
        unsigned condition{};
    };

    std::vector<std::uint32_t> words_;
    std::unordered_map<std::string, std::size_t> labels_;
    std::vector<Fixup> fixups_;
};

void emit_cmif_request(A64Assembler& a, unsigned command_id,
                       bool include_hid_service_name) {
    a.movz(0U, 4U);
    a.str_w(0U, 20U, 0U);
    a.movz(0U, include_hid_service_name ? 6U : 4U);
    a.str_w(0U, 20U, 4U);
    a.movz(0U, 0U);
    a.str_w(0U, 20U, 8U);
    a.movz(0U, 0x4653U);
    a.movk(0U, 0x4943U, 16U);
    a.str_w(0U, 20U, 16U);
    a.movz(0U, 0U);
    a.str_w(0U, 20U, 20U);
    a.movz(0U, static_cast<std::uint16_t>(command_id));
    a.str_w(0U, 20U, 24U);
    a.movz(0U, 0U);
    a.str_w(0U, 20U, 28U);
    if (include_hid_service_name) {
        a.movz(0U, 0x6968U);
        a.movk(0U, 0x0064U, 16U);
        a.str_x(0U, 20U, 32U);
    }
}

std::vector<std::uint32_t> build_self_test_program() {
    A64Assembler a;

    a.movz(1U, 0x0011U, 16U);
    a.svc(0x1FU);
    a.mov_reg(19U, 1U);
    a.mrs_tpidrro(20U);

    emit_cmif_request(a, 1U, true);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(21U, 20U, 12U);

    emit_cmif_request(a, 0U, false);
    a.mov_reg(0U, 21U);
    a.svc(0x21U);
    a.ldr_w(22U, 20U, 12U);

    emit_cmif_request(a, 0U, false);
    a.mov_reg(0U, 22U);
    a.svc(0x21U);
    a.ldr_w(23U, 20U, 12U);

    a.mov_reg(0U, 23U);
    a.movz(1U, 0x2000U, 16U);
    a.movz(2U, 4U, 16U);
    a.svc(0x14U);
    a.mov_reg(24U, 1U);

    a.movz(25U, 0x9A28U);
    a.add_reg(25U, 24U, 25U);
    a.movz(29U, 0x4000U, 16U);
    a.movz(26U, 100U);
    a.movz(27U, 100U);
    a.mov64(17U, 0xFF202020U);
    a.mov64(28U, 0xFFFF8040U);

    a.bind("input_loop");
    a.mov_reg(0U, 26U);
    a.mov_reg(1U, 27U);
    a.mov_reg(2U, 17U);
    a.branch_link("draw_rect");

    a.ldr_x(0U, 25U, 0U);
    a.and_imm(1U, 0U, 52U, 0U);
    a.cbz(1U, "no_left");
    a.sub_imm(26U, 26U, 4U);
    a.bind("no_left");

    a.and_imm(1U, 0U, 51U, 0U);
    a.cbz(1U, "no_up");
    a.sub_imm(27U, 27U, 4U);
    a.bind("no_up");

    a.and_imm(1U, 0U, 50U, 0U);
    a.cbz(1U, "no_right");
    a.add_imm(26U, 26U, 4U);
    a.bind("no_right");

    a.and_imm(1U, 0U, 49U, 0U);
    a.cbz(1U, "no_down");
    a.add_imm(27U, 27U, 4U);
    a.bind("no_down");

    a.and_imm(26U, 26U, 0U, 9U);
    a.and_imm(27U, 27U, 0U, 8U);

    a.mov_reg(0U, 26U);
    a.mov_reg(1U, 27U);
    a.mov_reg(2U, 28U);
    a.branch_link("draw_rect");
    a.branch("input_loop");

    a.bind("draw_rect");
    a.mov_reg(8U, 0U);
    a.mov_reg(9U, 1U);
    a.mov_reg(10U, 2U);
    a.lsl(3U, 9U, 10U);
    a.lsl(4U, 9U, 8U);
    a.add_reg(3U, 3U, 4U);
    a.add_reg(3U, 3U, 8U);
    a.lsl(3U, 3U, 2U);
    a.add_reg(11U, 29U, 3U);
    a.movz(12U, 32U);
    a.bind("draw_row");
    a.mov_reg(13U, 11U);
    a.movz(14U, 32U);
    a.bind("draw_column");
    a.str_w(10U, 13U, 0U);
    a.add_imm(13U, 13U, 4U);
    a.subs_imm(14U, 14U, 1U);
    a.bcond(1U, "draw_column");
    a.add_imm(11U, 11U, 4096U, true);
    a.add_imm(11U, 11U, 896U);
    a.subs_imm(12U, 12U, 1U);
    a.bcond(1U, "draw_row");
    a.ret();

    return a.finish();
}

struct Options {
    std::optional<std::string> nro_path;
    bool headless{};
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [file.nro] [--headless]\n"
              << "  No file: run the built-in SM/CMIF/HID controller demo.\n"
              << "  file.nro: load and run an uncompressed NRO image.\n"
              << "  --headless: run without opening an SDL2 window.\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--headless") options.headless = true;
        else if (argument == "--help" || argument == "-h") {
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
    memory.map_zeroed(kTlsAddress, kTlsSize,
                      horizon::Permission::Read | horizon::Permission::Write);
}

std::uint64_t load_self_test(horizon::AddressSpace& memory) {
    const auto instructions = build_self_test_program();
    std::vector<std::uint8_t> text;
    text.reserve(instructions.size() * sizeof(std::uint32_t));
    for (const std::uint32_t instruction : instructions) {
        for (unsigned byte = 0; byte < 4U; ++byte)
            text.push_back(static_cast<std::uint8_t>(instruction >> (byte * 8U)));
    }

    const std::vector<std::uint8_t> port_name{'s', 'm', ':', 0U};
    memory.map(kSelfTestTextAddress, std::move(text),
               horizon::Permission::Read | horizon::Permission::Execute);
    memory.map(kSelfTestPortNameAddress, port_name, horizon::Permission::Read);
    std::cout << "HorizonLite Stage 3 self-test: guest will connect sm:, request hid, "
                 "map HID shared memory, and draw a keyboard-controlled box.\n";
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
    const auto framebuffer = memory.view(kFramebufferAddress, kFramebufferBytes);
    constexpr std::uint8_t color[4]{0x40U, 0x80U, 0xFFU, 0xFFU};
    bool found_box_pixel = false;
    for (std::size_t offset = 0; offset + 4U <= framebuffer.size(); offset += 4U) {
        if (std::equal(std::begin(color), std::end(color),
                       framebuffer.begin() + static_cast<std::ptrdiff_t>(offset))) {
            found_box_pixel = true;
            break;
        }
    }
    if (!found_box_pixel)
        throw std::runtime_error("HID self-test did not render the guest-controlled box");
    std::cout << "HID/CMIF framebuffer self-test passed: guest-generated box is present.\n";
}

int run_headless(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel,
                 const horizon::AddressSpace& memory, bool self_test) {
    if (self_test) {
        kernel.update_hid_input(0U);
        const std::size_t executed = cpu.run(kHeadlessInstructionLimit, kStopAddress);
        std::cout << "Executed guest instructions: " << executed << '\n';
        verify_self_test(memory);
        return 0;
    }

    const std::size_t executed = cpu.run(20000000U, kStopAddress);
    std::cout << "Executed instructions: " << executed << '\n';
    if (kernel.running()) {
        std::cerr << "Execution reached the headless instruction limit.\n";
        return 3;
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
        if (!keep_open) break;

        kernel.update_hid_input(display.read_controller_buttons());
        if (kernel.running())
            (void)cpu.run(kInteractiveInstructionBudget, kStopAddress);
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
        cpu.state().tpidrro_el0 = kTlsAddress;

        if (options.headless)
            return run_headless(cpu, kernel, memory, self_test);
        return run_interactive(cpu, kernel, memory);
    } catch (const std::exception& error) {
        std::cerr << "HorizonLite: " << error.what() << '\n';
        return 1;
    }
}
