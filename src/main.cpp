#include "horizon/cpu.hpp"
#include "horizon/display.hpp"
#include "horizon/kernel.hpp"
#include "horizon/nro_loader.hpp"
#include "horizon/romfs.hpp"

#include <algorithm>
#include <array>
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
constexpr std::uint64_t kSelfTestStringsAddress = 0x00110000ULL;
constexpr std::uint64_t kHidMappingAddress = 0x20000000ULL;
constexpr std::uint64_t kSpriteBufferAddress = 0x21000000ULL;
constexpr std::uint64_t kStopAddress = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kInteractiveInstructionBudget = 180000U;
constexpr std::size_t kHeadlessInstructionLimit = 1000000U;
constexpr std::uint32_t kHipcInvoke2Method = 4U;

class A64Assembler {
public:
    void emit(std::uint32_t instruction) { words_.push_back(instruction); }

    void bind(const std::string& label) {
        if (labels_.contains(label))
            throw std::runtime_error("duplicate assembly label: " + label);
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
        fixups_.push_back({words_.size(), label, 0x54000000U | condition,
                           19U, condition});
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

enum class IpcDescriptorKind {
    None,
    Pointer,
    Send,
    Receive
};

void emit_cmif_request(A64Assembler& a, unsigned command_id,
                       IpcDescriptorKind descriptor_kind,
                       std::uint64_t descriptor_address,
                       std::uint64_t descriptor_size,
                       const std::vector<std::uint8_t>& payload = {}) {
    std::uint32_t header0 = kHipcInvoke2Method;
    std::size_t descriptor_bytes = 0U;
    if (descriptor_kind == IpcDescriptorKind::Pointer) {
        header0 |= 1U << 16U;
        descriptor_bytes = 8U;
    } else if (descriptor_kind == IpcDescriptorKind::Send) {
        header0 |= 1U << 20U;
        descriptor_bytes = 12U;
    } else if (descriptor_kind == IpcDescriptorKind::Receive) {
        header0 |= 1U << 24U;
        descriptor_bytes = 12U;
    }

    const std::size_t raw_words = (0x10U + payload.size() + 3U) / 4U;
    if (raw_words > 0x3FFU)
        throw std::runtime_error("guest self-test CMIF request exceeds HIPC raw limit");
    const std::size_t cmif_offset =
        (8U + descriptor_bytes + 0xFU) & ~static_cast<std::size_t>(0xFU);

    a.mov64(0U, header0);
    a.str_w(0U, 20U, 0U);
    a.mov64(0U, static_cast<std::uint32_t>(raw_words));
    a.str_w(0U, 20U, 4U);

    if (descriptor_kind == IpcDescriptorKind::Pointer) {
        const std::uint32_t tag =
            (static_cast<std::uint32_t>(descriptor_size & 0xFFFFU) << 16U);
        a.mov64(0U, tag);
        a.str_w(0U, 20U, 8U);
        a.mov64(0U, descriptor_address);
        a.str_w(0U, 20U, 12U);
    } else if (descriptor_kind == IpcDescriptorKind::Send ||
               descriptor_kind == IpcDescriptorKind::Receive) {
        a.mov64(0U, static_cast<std::uint32_t>(descriptor_size));
        a.str_w(0U, 20U, 8U);
        a.mov64(0U, static_cast<std::uint32_t>(descriptor_address));
        a.str_w(0U, 20U, 12U);
        a.movz(0U, 0U);
        a.str_w(0U, 20U, 16U);
    }

    const auto store_word = [&a, cmif_offset](std::size_t byte_offset,
                                               std::uint32_t value) {
        a.mov64(0U, value);
        a.str_w(0U, 20U, static_cast<unsigned>(cmif_offset + byte_offset));
    };

    store_word(0U, 0x49434653U);
    store_word(4U, 0U);
    store_word(8U, command_id);
    store_word(12U, 0U);

    for (std::size_t index = 0; index < payload.size(); index += 4U) {
        std::uint32_t word = 0U;
        for (unsigned byte = 0U; byte < 4U && index + byte < payload.size(); ++byte)
            word |= static_cast<std::uint32_t>(payload[index + byte]) << (byte * 8U);
        store_word(0x10U + index, word);
    }
}

void emit_named_service_request(A64Assembler& a, std::uint64_t name_address) {
    emit_cmif_request(a, 1U, IpcDescriptorKind::Send,
                      name_address, 8U);
}

std::vector<std::uint32_t> build_self_test_program() {
    A64Assembler a;

    a.mov64(1U, kSelfTestStringsAddress + 12U);
    a.svc(0x1FU);
    a.mov_reg(19U, 1U);
    a.mrs_tpidrro(20U);

    emit_named_service_request(a, kSelfTestStringsAddress);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(21U, 20U, 12U);

    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 21U);
    a.svc(0x21U);
    a.ldr_w(22U, 20U, 12U);

    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 22U);
    a.svc(0x21U);
    a.ldr_w(23U, 20U, 12U);

    a.mov_reg(0U, 23U);
    a.mov64(1U, kHidMappingAddress);
    a.mov64(2U, 0x40000U);
    a.svc(0x14U);
    a.mov_reg(24U, 1U);

    emit_named_service_request(a, kSelfTestStringsAddress + 4U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(25U, 20U, 12U);

    std::vector<std::uint8_t> pid_payload(8U, 0U);
    pid_payload[0] = 0x34U;
    pid_payload[1] = 0x12U;
    emit_cmif_request(a, 1U, IpcDescriptorKind::None, 0U, 0U, pid_payload);
    a.mov_reg(0U, 25U);
    a.svc(0x21U);

    emit_cmif_request(a, 200U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 25U);
    a.svc(0x21U);
    a.ldr_w(26U, 20U, 12U);

    emit_cmif_request(a, 4U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 26U);
    a.svc(0x21U);
    a.ldr_x(27U, 20U, 32U);

    std::vector<std::uint8_t> read_payload(16U, 0U);
    read_payload[0] = static_cast<std::uint8_t>(horizon::RomFsBuilder::kDemoSpriteDataOffset);
    read_payload[1] = static_cast<std::uint8_t>(horizon::RomFsBuilder::kDemoSpriteDataOffset >> 8U);
    read_payload[2] = static_cast<std::uint8_t>(horizon::RomFsBuilder::kDemoSpriteDataOffset >> 16U);
    read_payload[3] = static_cast<std::uint8_t>(horizon::RomFsBuilder::kDemoSpriteDataOffset >> 24U);
    constexpr std::uint64_t sprite_bytes = 16U * 16U * 4U;
    read_payload[8] = static_cast<std::uint8_t>(sprite_bytes);
    read_payload[9] = static_cast<std::uint8_t>(sprite_bytes >> 8U);
    read_payload[10] = static_cast<std::uint8_t>(sprite_bytes >> 16U);
    read_payload[11] = static_cast<std::uint8_t>(sprite_bytes >> 24U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::Receive,
                      kSpriteBufferAddress, sprite_bytes, read_payload);
    a.mov_reg(0U, 26U);
    a.svc(0x21U);

    a.mov64(15U, kHidMappingAddress + 0x9A28U);
    a.mov64(16U, kSpriteBufferAddress);
    a.mov64(29U, kFramebufferAddress);
    a.movz(19U, 100U);
    a.movz(18U, 100U);
    a.mov64(17U, 0xFF101820U);
    a.movz(28U, 0U);

    a.bind("frame_loop");
    a.mov_reg(0U, 19U);
    a.mov_reg(1U, 18U);
    a.mov_reg(2U, 17U);
    a.branch_link("draw_solid_sprite_area");

    a.ldr_x(0U, 15U, 0U);
    a.and_imm(1U, 0U, 52U, 0U);
    a.cbz(1U, "not_left");
    a.sub_imm(19U, 19U, 2U);
    a.bind("not_left");

    a.and_imm(1U, 0U, 51U, 0U);
    a.cbz(1U, "not_up");
    a.sub_imm(18U, 18U, 2U);
    a.bind("not_up");

    a.and_imm(1U, 0U, 50U, 0U);
    a.cbz(1U, "not_right");
    a.add_imm(19U, 19U, 2U);
    a.bind("not_right");

    a.and_imm(1U, 0U, 49U, 0U);
    a.cbz(1U, "not_down");
    a.add_imm(18U, 18U, 2U);
    a.bind("not_down");

    a.and_imm(19U, 19U, 0U, 10U);
    a.subs_imm(1U, 19U, 1264U);
    a.bcond(2U, "wrap_x");
    a.branch("x_ready");
    a.bind("wrap_x");
    a.movz(19U, 0U);
    a.bind("x_ready");

    a.and_imm(18U, 18U, 0U, 9U);
    a.subs_imm(1U, 18U, 704U);
    a.bcond(2U, "wrap_y");
    a.branch("y_ready");
    a.bind("wrap_y");
    a.movz(18U, 0U);
    a.bind("y_ready");

    a.mov_reg(0U, 19U);
    a.mov_reg(1U, 18U);
    a.branch_link("draw_rgba_sprite");
    a.branch("frame_loop");

    a.bind("draw_solid_sprite_area");
    a.mov_reg(8U, 0U);
    a.mov_reg(9U, 1U);
    a.mov_reg(10U, 2U);
    a.lsl(3U, 9U, 12U);
    a.lsl(4U, 9U, 10U);
    a.add_reg(3U, 3U, 4U);
    a.lsl(4U, 8U, 2U);
    a.add_reg(3U, 3U, 4U);
    a.add_reg(6U, 29U, 3U);
    a.movz(12U, 16U);
    a.bind("solid_row");
    a.mov_reg(11U, 6U);
    a.movz(13U, 16U);
    a.bind("solid_column");
    a.str_w(10U, 11U, 0U);
    a.add_imm(11U, 11U, 4U);
    a.subs_imm(13U, 13U, 1U);
    a.bcond(1U, "solid_column");
    a.add_imm(6U, 6U, 4096U, true);
    a.add_imm(6U, 6U, 1024U);
    a.subs_imm(12U, 12U, 1U);
    a.bcond(1U, "solid_row");
    a.ret();

    a.bind("draw_rgba_sprite");
    a.mov_reg(8U, 0U);
    a.mov_reg(9U, 1U);
    a.lsl(3U, 9U, 12U);
    a.lsl(4U, 9U, 10U);
    a.add_reg(3U, 3U, 4U);
    a.lsl(4U, 8U, 2U);
    a.add_reg(3U, 3U, 4U);
    a.add_reg(6U, 29U, 3U);
    a.mov_reg(7U, 16U);
    a.movz(12U, 16U);
    a.bind("sprite_row");
    a.mov_reg(11U, 6U);
    a.mov_reg(13U, 7U);
    a.movz(14U, 16U);
    a.bind("sprite_column");
    a.ldr_w(10U, 13U, 0U);
    a.cbz(10U, "transparent_pixel");
    a.str_w(10U, 11U, 0U);
    a.bind("transparent_pixel");
    a.add_imm(13U, 13U, 4U);
    a.add_imm(11U, 11U, 4U);
    a.subs_imm(14U, 14U, 1U);
    a.bcond(1U, "sprite_column");
    a.add_imm(6U, 6U, 4096U, true);
    a.add_imm(6U, 6U, 1024U);
    a.add_imm(7U, 7U, 64U);
    a.subs_imm(12U, 12U, 1U);
    a.bcond(1U, "sprite_row");
    a.ret();

    return a.finish();
}

struct Options {
    std::optional<std::string> nro_path;
    bool headless{};
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [file.nro] [--headless]\n"
              << "  No file: run the built-in SM/HID/fsp-srv RomFS sprite demo.\n"
              << "  file.nro: load an uncompressed NRO and its optional ASET RomFS.\n"
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
    memory.map_zeroed(kSpriteBufferAddress, 0x1000U,
                      horizon::Permission::Read | horizon::Permission::Write);

    std::vector<std::uint8_t> background(kFramebufferBytes, 0U);
    for (std::size_t offset = 0U; offset < background.size(); offset += 4U) {
        background[offset] = 0x10U;
        background[offset + 1U] = 0x18U;
        background[offset + 2U] = 0x20U;
        background[offset + 3U] = 0xFFU;
    }
    memory.write(kFramebufferAddress, background);
}

std::uint64_t load_self_test(horizon::AddressSpace& memory) {
    const auto instructions = build_self_test_program();
    std::vector<std::uint8_t> text;
    text.reserve(instructions.size() * sizeof(std::uint32_t));
    for (const std::uint32_t instruction : instructions) {
        for (unsigned byte = 0U; byte < 4U; ++byte)
            text.push_back(static_cast<std::uint8_t>(instruction >> (byte * 8U)));
    }

    const std::vector<std::uint8_t> service_strings{
        'h', 'i', 'd', 0U,
        'f', 's', 'p', '-', 's', 'r', 'v', 0U,
        's', 'm', ':', 0U
    };
    memory.map(kSelfTestTextAddress, std::move(text),
               horizon::Permission::Read | horizon::Permission::Execute);
    memory.map(kSelfTestStringsAddress, service_strings,
               horizon::Permission::Read);
    std::cout << "HorizonLite Stage 4 self-test: guest connects to sm:, requests hid and fsp-srv, "
                 "reads sprite.rgba through IStorage::Read, then draws the sprite from guest RAM.\n";
    return kSelfTestTextAddress;
}

std::vector<std::uint8_t> make_self_test_romfs() {
    const std::vector<std::uint8_t> sprite =
        horizon::RomFsBuilder::make_demo_sprite_rgba();
    return horizon::RomFsBuilder::build_single_file("sprite.rgba", sprite);
}

struct LoadedProgram {
    std::uint64_t entry_point{};
    std::vector<std::uint8_t> romfs;
    bool self_test{};
};

LoadedProgram load_program(const Options& options,
                           horizon::AddressSpace& memory) {
    if (!options.nro_path.has_value()) {
        LoadedProgram result;
        result.entry_point = load_self_test(memory);
        result.romfs = make_self_test_romfs();
        result.self_test = true;
        return result;
    }

    const auto file = horizon::NroLoader::read_file(*options.nro_path);
    const horizon::NroImage image = horizon::NroLoader::load(file, memory);
    std::cout << "Loaded NRO0 image: " << *options.nro_path << '\n'
              << "  Declared size: 0x" << std::hex << image.declared_size << '\n'
              << "  Entry point:   0x" << image.entry_point << std::dec << '\n'
              << "  ASET RomFS:    " << image.romfs.size() << " bytes\n";
    return LoadedProgram{image.entry_point, image.romfs, false};
}

void verify_self_test(const horizon::AddressSpace& memory) {
    const std::uint64_t pixel_address =
        kFramebufferAddress +
        (static_cast<std::uint64_t>(101U) * kFramebufferWidth + 101U) * 4U;
    constexpr std::uint32_t expected_rgba = 0xFFF0B842U;
    const std::uint32_t actual = memory.read_u32(pixel_address);
    if (actual != expected_rgba) {
        std::cerr << "  Sprite pixel (0,0): 0x" << std::hex
                  << memory.read_u32(kSpriteBufferAddress)
                  << "  Sprite pixel (1,1): 0x"
                  << memory.read_u32(kSpriteBufferAddress + 68U)
                  << "  Framebuffer probe: 0x" << actual << std::dec << '\n';
        throw std::runtime_error("RomFS sprite self-test failed: guest did not draw expected RGBA pixel");
    }
    std::cout << "RomFS/fsp-srv/HIPC sprite self-test passed\n"
              << "  RomFS sprite pixel: 0x" << std::hex << actual << std::dec << '\n';
}

int run_headless(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel,
                 const horizon::AddressSpace& memory, bool self_test) {
    if (self_test) {
        kernel.update_hid_input(0U);
        const std::size_t executed =
            cpu.run(kHeadlessInstructionLimit, kStopAddress);
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
        LoadedProgram program = load_program(options, memory);

        horizon::HorizonKernel kernel(memory, std::move(program.romfs));
        horizon::SvcHandler svc_handler(kernel);
        horizon::Arm64Cpu cpu(memory, svc_handler);
        cpu.state().pc = program.entry_point;
        cpu.state().sp = kStackAddress + kStackSize - 16U;
        cpu.state().tpidrro_el0 = kTlsAddress;

        if (options.headless)
            return run_headless(cpu, kernel, memory, program.self_test);
        return run_interactive(cpu, kernel, memory);
    } catch (const std::exception& error) {
        std::cerr << "HorizonLite: " << error.what() << '\n';
        return 1;
    }
}
