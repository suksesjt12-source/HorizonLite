#include "horizon/cpu.hpp"

#include "horizon/kernel.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace horizon {
namespace {

constexpr std::uint32_t kNegativeFlag = 1U << 31U;
constexpr std::uint32_t kZeroFlag = 1U << 30U;
constexpr std::uint32_t kCarryFlag = 1U << 29U;
constexpr std::uint32_t kOverflowFlag = 1U << 28U;
constexpr std::uint32_t kNzcvMask = 0xF0000000U;

std::int64_t sign_extend(std::uint64_t value, unsigned bits) noexcept {
    const std::uint64_t sign_bit = 1ULL << (bits - 1U);
    if ((value & sign_bit) != 0U) {
        return static_cast<std::int64_t>(value) -
               static_cast<std::int64_t>(1ULL << bits);
    }
    return static_cast<std::int64_t>(value);
}

} // namespace

Arm64Cpu::Arm64Cpu(AddressSpace& memory, SvcHandler& svc_handler) noexcept
    : memory_(memory), svc_handler_(svc_handler) {}

CpuState& Arm64Cpu::state() noexcept { return state_; }
const CpuState& Arm64Cpu::state() const noexcept { return state_; }

std::uint64_t Arm64Cpu::read_x(unsigned index) const noexcept {
    return index < 31U ? state_.x[index] : 0U;
}

std::uint64_t Arm64Cpu::read_base(unsigned index) const noexcept {
    return index == 31U ? state_.sp : read_x(index);
}

void Arm64Cpu::write_x(unsigned index, std::uint64_t value) noexcept {
    if (index < 31U) {
        state_.x[index] = value;
    }
}

void Arm64Cpu::write_nzcv_sub(std::uint64_t lhs, std::uint64_t rhs,
                              std::uint64_t result) noexcept {
    std::uint32_t flags = 0;
    if ((result >> 63U) != 0U) {
        flags |= kNegativeFlag;
    }
    if (result == 0U) {
        flags |= kZeroFlag;
    }
    if (lhs >= rhs) {
        flags |= kCarryFlag;
    }
    if ((((lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0U) {
        flags |= kOverflowFlag;
    }
    state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
}

bool Arm64Cpu::condition_passed(unsigned condition) const noexcept {
    const bool negative = (state_.pstate & kNegativeFlag) != 0U;
    const bool zero = (state_.pstate & kZeroFlag) != 0U;
    const bool carry = (state_.pstate & kCarryFlag) != 0U;
    const bool overflow = (state_.pstate & kOverflowFlag) != 0U;

    switch (condition & 0xFU) {
    case 0x0: return zero;
    case 0x1: return !zero;
    case 0x2: return carry;
    case 0x3: return !carry;
    case 0x4: return negative;
    case 0x5: return !negative;
    case 0x6: return overflow;
    case 0x7: return !overflow;
    case 0x8: return carry && !zero;
    case 0x9: return !carry || zero;
    case 0xA: return negative == overflow;
    case 0xB: return negative != overflow;
    case 0xC: return !zero && (negative == overflow);
    case 0xD: return zero || (negative != overflow);
    case 0xE: return true;
    default: return false;
    }
}

void Arm64Cpu::step() {
    const std::uint64_t instruction_pc = state_.pc;
    const std::uint32_t instruction =
        memory_.read_u32(instruction_pc, Permission::Execute);
    state_.pc += 4U;

    // SVC #imm16: the handler receives the immediate and current CPU context.
    if ((instruction & 0xFFE0001FU) == 0xD4000001U) {
        const auto immediate = static_cast<std::uint16_t>(
            (instruction >> 5U) & 0xFFFFU);
        svc_handler_.dispatch(immediate, state_);
        return;
    }

    // MOV Xd, Xm is the unshifted ORR Xd, XZR, Xm alias.
    if ((instruction & 0xFFE0FFE0U) == 0xAA0003E0U) {
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, read_x(rm));
        return;
    }

    // MOVZ Xd, #imm16, LSL #(16 * hw).
    if ((instruction & 0xFF800000U) == 0xD2800000U) {
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const std::uint64_t immediate = (instruction >> 5U) & 0xFFFFU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, immediate << (hw * 16U));
        return;
    }

    // MOVK Xd, #imm16, LSL #(16 * hw), preserving the other halfwords.
    if ((instruction & 0xFF800000U) == 0xF2800000U) {
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const unsigned rd = instruction & 0x1FU;
        const unsigned shift = hw * 16U;
        const std::uint64_t mask = 0xFFFFULL << shift;
        const std::uint64_t immediate =
            static_cast<std::uint64_t>((instruction >> 5U) & 0xFFFFU) << shift;
        const std::uint64_t value = (read_x(rd) & ~mask) | immediate;
        write_x(rd, value);
        return;
    }

    // ADD/SUB and ADDS/SUBS (immediate), 64-bit form.
    const std::uint32_t immediate_class = instruction & 0xFF000000U;
    if (immediate_class == 0x91000000U || immediate_class == 0xD1000000U ||
        immediate_class == 0xB1000000U || immediate_class == 0xF1000000U) {
        const bool is_sub = immediate_class == 0xD1000000U ||
                            immediate_class == 0xF1000000U;
        const bool set_flags = immediate_class == 0xB1000000U ||
                               immediate_class == 0xF1000000U;
        const unsigned shift = (instruction >> 22U) & 0x1U;
        const std::uint64_t immediate =
            static_cast<std::uint64_t>((instruction >> 10U) & 0xFFFU)
            << (shift * 12U);
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const std::uint64_t lhs = set_flags ? read_x(rn) : read_base(rn);
        const std::uint64_t result = is_sub ? lhs - immediate : lhs + immediate;
        if (set_flags) {
            write_nzcv_sub(lhs, immediate, result);
            write_x(rd, result);
        } else if (rd == 31U) {
            state_.sp = result;
        } else {
            write_x(rd, result);
        }
        return;
    }

    // ADD/SUB and ADDS/SUBS (register), 64-bit and unshifted.
    const std::uint32_t register_class = instruction & 0xFFE0FC00U;
    if (register_class == 0x8B000000U || register_class == 0xCB000000U ||
        register_class == 0xAB000000U || register_class == 0xEB000000U) {
        const bool is_sub = register_class == 0xCB000000U ||
                            register_class == 0xEB000000U;
        const bool set_flags = register_class == 0xAB000000U ||
                               register_class == 0xEB000000U;
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const std::uint64_t lhs = read_x(rn);
        const std::uint64_t rhs = read_x(rm);
        const std::uint64_t result = is_sub ? lhs - rhs : lhs + rhs;
        if (set_flags) {
            if (is_sub) {
                write_nzcv_sub(lhs, rhs, result);
            } else {
                const std::uint64_t carry_result = lhs + rhs;
                std::uint32_t flags = (result >> 63U) != 0U ? kNegativeFlag : 0U;
                if (result == 0U) flags |= kZeroFlag;
                if (carry_result < lhs) flags |= kCarryFlag;
                const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0U;
                if (overflow) flags |= kOverflowFlag;
                state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
            }
        }
        write_x(rd, result);
        return;
    }

    // STR/LDR unsigned immediate, 32-bit and 64-bit variants.
    const std::uint32_t memory_class = instruction & 0xFFC00000U;
    const bool store64 = memory_class == 0xF9000000U;
    const bool load64 = memory_class == 0xF9400000U;
    const bool store32 = memory_class == 0xB9000000U;
    const bool load32 = memory_class == 0xB9400000U;
    if (store64 || load64 || store32 || load32) {
        const bool wide = store64 || load64;
        const bool is_load = load64 || load32;
        const unsigned rt = instruction & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const std::uint64_t encoded_offset = (instruction >> 10U) & 0xFFFU;
        const std::uint64_t address =
            read_base(rn) + (encoded_offset << (wide ? 3U : 2U));
        if (is_load) {
            if (wide) {
                write_x(rt, memory_.read_u64(address));
            } else {
                write_x(rt, memory_.read_u32(address));
            }
        } else if (wide) {
            memory_.write_u64(address, read_x(rt));
        } else {
            memory_.write_u32(address, static_cast<std::uint32_t>(read_x(rt)));
        }
        return;
    }

    // B imm26.
    if ((instruction & 0xFC000000U) == 0x14000000U) {
        const std::int64_t displacement = sign_extend(
            instruction & 0x03FFFFFFU, 26U);
        state_.pc = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
        return;
    }

    // B.cond imm19, including the NE condition used by the framebuffer payload.
    if ((instruction & 0xFF000010U) == 0x54000000U) {
        const unsigned condition = instruction & 0xFU;
        if (condition_passed(condition)) {
            const std::int64_t displacement = sign_extend(
                (instruction >> 5U) & 0x7FFFFU, 19U);
            state_.pc = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
        }
        return;
    }

    // RET Xn (the default encoding is RET X30).
    if ((instruction & 0xFFFFFC1FU) == 0xD65F0000U) {
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        state_.pc = read_x(rn);
        return;
    }

    std::ostringstream message;
    message << "unsupported AArch64 instruction 0x" << std::hex
            << std::setw(8) << std::setfill('0') << instruction
            << " at PC 0x" << instruction_pc;
    throw std::runtime_error(message.str());
}

std::size_t Arm64Cpu::run(std::size_t max_steps, std::uint64_t stop_pc) {
    std::size_t executed = 0;
    while (executed < max_steps && state_.pc != stop_pc &&
           svc_handler_.running()) {
        step();
        ++executed;
    }
    return executed;
}

} // namespace horizon
