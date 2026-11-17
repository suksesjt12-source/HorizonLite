#include "horizon/cpu.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace horizon {

Arm64Cpu::Arm64Cpu(AddressSpace& memory) noexcept : memory_(memory) {}

CpuState& Arm64Cpu::state() noexcept { return state_; }
const CpuState& Arm64Cpu::state() const noexcept { return state_; }

std::uint64_t Arm64Cpu::read_x(unsigned index) const {
    return index < 31U ? state_.x[index] : 0U;
}

void Arm64Cpu::write_x(unsigned index, std::uint64_t value) {
    if (index < 31U) {
        state_.x[index] = value;
    }
}

void Arm64Cpu::step() {
    const std::uint64_t instruction_pc = state_.pc;
    const std::uint32_t instruction =
        memory_.read_u32(instruction_pc, Permission::Execute);
    state_.pc += 4U;

    // MOV Xd, Xm is the ORR Xd, XZR, Xm alias (unshifted, 64-bit form).
    if ((instruction & 0xFFE0FFE0U) == 0xAA0003E0U) {
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, read_x(rm));
        return;
    }

    // MOVZ Xd, #imm16, LSL #(16 * hw).
    if ((instruction & 0xFF800000U) == 0xD2800000U) {
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const std::uint64_t imm16 = (instruction >> 5U) & 0xFFFFU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, imm16 << (hw * 16U));
        return;
    }

    // ADD/SUB (immediate), 64-bit, without flag updates.
    const std::uint32_t immediate_class = instruction & 0xFF000000U;
    if (immediate_class == 0x91000000U || immediate_class == 0xD1000000U) {
        const bool is_sub = immediate_class == 0xD1000000U;
        const unsigned shift = (instruction >> 22U) & 0x1U;
        const std::uint64_t immediate =
            static_cast<std::uint64_t>((instruction >> 10U) & 0xFFFU)
            << (shift * 12U);
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const std::uint64_t lhs = read_x(rn);
        write_x(rd, is_sub ? lhs - immediate : lhs + immediate);
        return;
    }

    // ADD/SUB (register), 64-bit, unshifted, without flag updates.
    const std::uint32_t register_class = instruction & 0xFFE0FC00U;
    if (register_class == 0x8B000000U || register_class == 0xCB000000U) {
        const bool is_sub = register_class == 0xCB000000U;
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const std::uint64_t lhs = read_x(rn);
        const std::uint64_t rhs = read_x(rm);
        write_x(rd, is_sub ? lhs - rhs : lhs + rhs);
        return;
    }

    // B imm26: branch target is relative to the address of this instruction.
    if ((instruction & 0xFC000000U) == 0x14000000U) {
        std::int64_t displacement =
            static_cast<std::int64_t>(instruction & 0x03FFFFFFU);
        if ((displacement & (1LL << 25U)) != 0) {
            displacement -= (1LL << 26U);
        }
        state_.pc = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
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
    while (executed < max_steps && state_.pc != stop_pc) {
        step();
        ++executed;
    }
    return executed;
}

} // namespace horizon
