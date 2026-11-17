#include "horizon/cpu.hpp"

#include "horizon/kernel.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
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

std::uint64_t width_mask(unsigned width) noexcept {
    return width == 32U ? 0xFFFFFFFFULL : UINT64_MAX;
}

std::uint64_t expand_fp_immediate(std::uint8_t immediate,
                                  unsigned width) noexcept {
    const unsigned exponent_bits = width == 32U ? 8U : 11U;
    const unsigned fraction_bits = width == 32U ? 23U : 52U;
    const std::uint64_t sign =
        static_cast<std::uint64_t>((immediate >> 7U) & 1U);
    const std::uint64_t b6 =
        static_cast<std::uint64_t>((immediate >> 6U) & 1U);
    const unsigned repeated_exponent_bits = exponent_bits - 3U;
    const std::uint64_t repeated_exponent = b6 == 0U ? 0U :
        ((1ULL << repeated_exponent_bits) - 1ULL);
    const std::uint64_t exponent =
        ((b6 ^ 1U) << (exponent_bits - 1U)) |
        (repeated_exponent << 2U) |
        static_cast<std::uint64_t>((immediate >> 4U) & 0x3U);
    const unsigned repeated_fraction_bits = fraction_bits - 4U;
    const std::uint64_t repeated_fraction =
        ((immediate & 0x8U) == 0U || repeated_fraction_bits == 0U)
        ? 0U : ((1ULL << repeated_fraction_bits) - 1ULL);
    const std::uint64_t fraction =
        (static_cast<std::uint64_t>(immediate & 0xFU) <<
         repeated_fraction_bits) | repeated_fraction;
    if (width == 32U) {
        return (sign << 31U) | (exponent << 23U) | fraction;
    }
    return (sign << 63U) | (exponent << 52U) | fraction;
}

std::int64_t float_to_signed_integer(double value) noexcept {
    if (std::isnan(value)) return 0;
    if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min()))
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(std::trunc(value));
}

std::uint64_t rotate_right_width(std::uint64_t value, unsigned amount,
                                 unsigned width) noexcept {
    const std::uint64_t mask = width_mask(width);
    value &= mask;
    amount %= width;
    if (amount == 0U) return value;
    return ((value >> amount) | (value << (width - amount))) & mask;
}

bool decode_bitmask(unsigned n, unsigned immr, unsigned imms,
                    unsigned width, std::uint64_t& result) noexcept {
    const unsigned combined = (n << 6U) | ((~imms) & 0x3FU);
    if (combined == 0U) return false;
    unsigned len = 0;
    for (unsigned value = combined; value > 1U; value >>= 1U) ++len;
    if (len < 1U) return false;
    const unsigned element_width = 1U << len;
    if (element_width > width) return false;
    const unsigned levels = element_width - 1U;
    const unsigned s = imms & levels;
    const unsigned r = immr & levels;
    if (s == levels) return false;
    const std::uint64_t ones = s == 63U ? UINT64_MAX : ((1ULL << (s + 1U)) - 1ULL);
    const std::uint64_t element = rotate_right_width(ones, r, element_width);
    result = 0;
    for (unsigned bit = 0; bit < width; bit += element_width) {
        result |= element << bit;
    }
    result &= width_mask(width);
    return true;
}

std::uint64_t apply_shift(std::uint64_t value, unsigned shift_type,
                          unsigned amount, unsigned width) noexcept {
    const std::uint64_t mask = width_mask(width);
    value &= mask;
    amount %= width;
    switch (shift_type) {
    case 0:
        return (value << amount) & mask;
    case 1:
        return value >> amount;
    case 2:
        if (width == 32U) {
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value) >> amount);
        }
        return static_cast<std::uint64_t>(
            static_cast<std::int64_t>(value) >> amount);
    default:
        return rotate_right_width(value, amount, width);
    }
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
    if (index < 31U) state_.x[index] = value;
}

void Arm64Cpu::invalidate_exclusive(std::uint64_t address,
                                   std::size_t size) noexcept {
    state_.exclusive_monitor.invalidate(address, size);
}

void Arm64Cpu::write_nzcv_sub(std::uint64_t lhs, std::uint64_t rhs,
                              std::uint64_t result) noexcept {
    std::uint32_t flags = 0;
    if ((result >> 63U) != 0U) flags |= kNegativeFlag;
    if (result == 0U) flags |= kZeroFlag;
    if (lhs >= rhs) flags |= kCarryFlag;
    if ((((lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0U) flags |= kOverflowFlag;
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

    if ((instruction & 0xFFFFFFE0U) == 0xD53BD060U) {
        const unsigned rt = instruction & 0x1FU;
        write_x(rt, state_.tpidrro_el0);
        return;
    }

    if ((instruction & 0xFFE0001FU) == 0xD4000001U) {
        const auto immediate = static_cast<std::uint16_t>(
            (instruction >> 5U) & 0xFFFFU);
        svc_handler_.dispatch(immediate, state_);
        return;
    }

    if ((instruction & 0xFFE0FFE0U) == 0xAA0003E0U) {
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, read_x(rm));
        return;
    }

    if ((instruction & 0x7F800000U) == 0x12800000U) {
        const unsigned width = (instruction >> 31U) != 0U ? 64U : 32U;
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const unsigned shift = hw * 16U;
        if (width == 32U && shift >= 32U)
            throw std::runtime_error("invalid 32-bit MOVN shift");
        const std::uint64_t immediate =
            static_cast<std::uint64_t>((instruction >> 5U) & 0xFFFFU) << shift;
        write_x(instruction & 0x1FU, (~immediate) & width_mask(width));
        return;
    }

    if ((instruction & 0xFF800000U) == 0xD2800000U) {
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const std::uint64_t immediate = (instruction >> 5U) & 0xFFFFU;
        write_x(instruction & 0x1FU, immediate << (hw * 16U));
        return;
    }

    if ((instruction & 0xFF800000U) == 0xF2800000U) {
        const unsigned hw = (instruction >> 21U) & 0x3U;
        const unsigned rd = instruction & 0x1FU;
        const unsigned shift = hw * 16U;
        const std::uint64_t mask = 0xFFFFULL << shift;
        const std::uint64_t immediate =
            static_cast<std::uint64_t>((instruction >> 5U) & 0xFFFFU) << shift;
        write_x(rd, (read_x(rd) & ~mask) | immediate);
        return;
    }

    if ((instruction & 0x1F800000U) == 0x12000000U) {
        const unsigned width = (instruction >> 31U) != 0U ? 64U : 32U;
        const unsigned opc = (instruction >> 29U) & 0x3U;
        const unsigned n = (instruction >> 22U) & 1U;
        const unsigned immr = (instruction >> 16U) & 0x3FU;
        const unsigned imms = (instruction >> 10U) & 0x3FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        std::uint64_t immediate = 0;
        if (!decode_bitmask(n, immr, imms, width, immediate)) {
            throw std::runtime_error("invalid AArch64 logical immediate");
        }
        const std::uint64_t lhs = read_x(rn) & width_mask(width);
        std::uint64_t value = 0;
        if (opc == 0U) value = lhs & immediate;
        else if (opc == 1U) value = lhs | immediate;
        else if (opc == 2U) value = lhs ^ immediate;
        else {
            value = lhs & immediate;
            std::uint32_t flags = value == 0U ? kZeroFlag : 0U;
            if ((value & (1ULL << (width - 1U))) != 0U) flags |= kNegativeFlag;
            state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
        }
        write_x(rd, value & width_mask(width));
        return;
    }

    if ((instruction & 0x1F200000U) == 0x0A000000U) {
        const unsigned width = (instruction >> 31U) != 0U ? 64U : 32U;
        const unsigned opc = (instruction >> 29U) & 0x3U;
        const unsigned shift_type = (instruction >> 22U) & 0x3U;
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned amount = (instruction >> 10U) & 0x3FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        if (width == 32U && amount >= 32U) {
            throw std::runtime_error("invalid 32-bit logical shift amount");
        }
        const std::uint64_t lhs = read_x(rn) & width_mask(width);
        const std::uint64_t rhs = apply_shift(read_x(rm), shift_type,
                                               amount, width);
        std::uint64_t value = 0;
        if (opc == 0U) value = lhs & rhs;
        else if (opc == 1U) value = lhs | rhs;
        else if (opc == 2U) value = lhs ^ rhs;
        else {
            value = lhs & rhs;
            std::uint32_t flags = value == 0U ? kZeroFlag : 0U;
            if ((value & (1ULL << (width - 1U))) != 0U) flags |= kNegativeFlag;
            state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
        }
        write_x(rd, value & width_mask(width));
        return;
    }

    const std::uint32_t bitfield_class = instruction & 0x7F800000U;
    if (bitfield_class == 0x53000000U || bitfield_class == 0x53400000U) {
        const unsigned width = (instruction >> 31U) != 0U ? 64U : 32U;
        const unsigned immr = (instruction >> 16U) & 0x3FU;
        const unsigned imms = (instruction >> 10U) & 0x3FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        if (width == 32U && ((immr | imms) & 0x20U) != 0U) {
            throw std::runtime_error("invalid 32-bit bitfield shift");
        }
        const std::uint64_t source = read_x(rn) & width_mask(width);
        std::uint64_t value = 0;
        if (imms == width - 1U) {
            value = source >> immr;
        } else {
            const unsigned lsl_amount = width - 1U - imms;
            if (immr == ((width - lsl_amount) % width)) {
                value = (source << lsl_amount) & width_mask(width);
            } else {
                const std::uint64_t rotated =
                    rotate_right_width(source, immr, width);
                const std::uint64_t mask = imms == 63U
                    ? UINT64_MAX : ((1ULL << (imms + 1U)) - 1ULL);
                value = rotated & mask;
            }
        }
        write_x(rd, value & width_mask(width));
        return;
    }

    const std::uint32_t immediate_class = instruction & 0xFF000000U;
    if (immediate_class == 0x91000000U || immediate_class == 0xD1000000U ||
        immediate_class == 0xB1000000U || immediate_class == 0xF1000000U) {
        const bool is_sub = immediate_class == 0xD1000000U ||
                            immediate_class == 0xF1000000U;
        const bool set_flags = immediate_class == 0xB1000000U ||
                               immediate_class == 0xF1000000U;
        const unsigned shift = (instruction >> 22U) & 1U;
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
                std::uint32_t flags = (result >> 63U) != 0U ? kNegativeFlag : 0U;
                if (result == 0U) flags |= kZeroFlag;
                if (result < lhs) flags |= kCarryFlag;
                if (((~(lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0U)
                    flags |= kOverflowFlag;
                state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
            }
        }
        write_x(rd, result);
        return;
    }

    if (instruction == 0xD5033F5FU) {
        state_.exclusive_monitor.clear();
        return;
    }

    const std::uint32_t exclusive_load_class =
        instruction & 0x3F7FFC00U;
    if (exclusive_load_class == 0x085F7C00U ||
        exclusive_load_class == 0x085FFC00U) {
        const bool wide = ((instruction >> 30U) & 1U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rt = instruction & 0x1FU;
        const std::size_t access_size = wide ? 8U : 4U;
        const std::uint64_t address = read_base(rn);
        if (wide) write_x(rt, memory_.read_u64(address));
        else write_x(rt, memory_.read_u32(address));
        state_.exclusive_monitor.valid = true;
        state_.exclusive_monitor.address = address;
        state_.exclusive_monitor.size = access_size;
        return;
    }

    const std::uint32_t exclusive_store_class =
        instruction & 0x3F20FC00U;
    if (exclusive_store_class == 0x08007C00U ||
        exclusive_store_class == 0x0800FC00U) {
        const bool wide = (instruction >> 30U) != 0U;
        const unsigned status_register = (instruction >> 16U) & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rt = instruction & 0x1FU;
        const std::size_t access_size = wide ? 8U : 4U;
        const std::uint64_t address = read_base(rn);
        const bool success =
            state_.exclusive_monitor.matches(address, access_size);
        if (success) {
            invalidate_exclusive(address, access_size);
            if (wide) memory_.write_u64(address, read_x(rt));
            else memory_.write_u32(address, static_cast<std::uint32_t>(read_x(rt)));
        }
        write_x(status_register, success ? 0U : 1U);
        state_.exclusive_monitor.clear();
        return;
    }

    const std::uint32_t fp_move_class = instruction & 0xFFFFFC00U;
    if (fp_move_class == 0x1E270000U ||
        fp_move_class == 0x9E670000U) {
        const bool wide = (instruction >> 31U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        state_.v[rd] = wide ? read_x(rn) :
            static_cast<std::uint32_t>(read_x(rn));
        return;
    }
    if (fp_move_class == 0x1E260000U ||
        fp_move_class == 0x9E660000U) {
        const bool wide = (instruction >> 31U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        write_x(rd, wide ? state_.v[rn] :
                static_cast<std::uint32_t>(state_.v[rn]));
        return;
    }

    const std::uint32_t fp_immediate_class =
        instruction & 0xFF201FE0U;
    if (fp_immediate_class == 0x1E201000U) {
        const unsigned width = ((instruction >> 22U) & 1U) != 0U
            ? 64U : 32U;
        const unsigned rd = instruction & 0x1FU;
        const auto immediate = static_cast<std::uint8_t>(
            (instruction >> 13U) & 0xFFU);
        state_.v[rd] = expand_fp_immediate(immediate, width);
        return;
    }

    const std::uint32_t fp_compare_class =
        instruction & 0xFF20FC1FU;
    if (fp_compare_class == 0x1E202000U) {
        const bool wide = ((instruction >> 22U) & 1U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const double lhs = wide
            ? std::bit_cast<double>(state_.v[rn])
            : static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(state_.v[rn])));
        const double rhs = wide
            ? std::bit_cast<double>(state_.v[rm])
            : static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(state_.v[rm])));
        std::uint32_t flags = 0U;
        if (std::isnan(lhs) || std::isnan(rhs)) {
            flags = kCarryFlag | kOverflowFlag;
        } else if (lhs == rhs) {
            flags = kZeroFlag | kCarryFlag;
        } else if (lhs < rhs) {
            flags = kNegativeFlag;
        } else {
            flags = kCarryFlag;
        }
        state_.pstate = (state_.pstate & ~kNzcvMask) | flags;
        return;
    }

    const std::uint32_t fp_arithmetic_class =
        instruction & 0xFF20FC00U;
    if (fp_arithmetic_class == 0x1E202800U ||
        fp_arithmetic_class == 0x1E203800U ||
        fp_arithmetic_class == 0x1E200800U ||
        fp_arithmetic_class == 0x1E201800U) {
        const bool wide = ((instruction >> 22U) & 1U) != 0U;
        const unsigned rm = (instruction >> 16U) & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const double lhs = wide
            ? std::bit_cast<double>(state_.v[rn])
            : static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(state_.v[rn])));
        const double rhs = wide
            ? std::bit_cast<double>(state_.v[rm])
            : static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(state_.v[rm])));
        double result = 0.0;
        if (fp_arithmetic_class == 0x1E202800U) result = lhs + rhs;
        else if (fp_arithmetic_class == 0x1E203800U) result = lhs - rhs;
        else if (fp_arithmetic_class == 0x1E200800U) result = lhs * rhs;
        else result = lhs / rhs;
        if (wide) state_.v[rd] = std::bit_cast<std::uint64_t>(result);
        else state_.v[rd] = std::bit_cast<std::uint32_t>(
            static_cast<float>(result));
        return;
    }

    const std::uint32_t fp_convert_class =
        instruction & 0x5F3FFC00U;
    if (fp_convert_class == 0x1E220000U) {
        const bool source_wide = (instruction >> 31U) != 0U;
        const bool destination_double = ((instruction >> 22U) & 1U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const double value = source_wide
            ? static_cast<double>(static_cast<std::int64_t>(read_x(rn)))
            : static_cast<double>(static_cast<std::int32_t>(read_x(rn)));
        if (destination_double) state_.v[rd] = std::bit_cast<std::uint64_t>(value);
        else state_.v[rd] = std::bit_cast<std::uint32_t>(
            static_cast<float>(value));
        return;
    }

    const std::uint32_t fp_to_integer_class =
        instruction & 0x5F3FFC00U;
    if (fp_to_integer_class == 0x1E380000U ||
        fp_to_integer_class == 0x1E780000U) {
        const bool destination_wide = (instruction >> 31U) != 0U;
        const bool source_double = ((instruction >> 22U) & 1U) != 0U;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const unsigned rd = instruction & 0x1FU;
        const double value = source_double
            ? std::bit_cast<double>(state_.v[rn])
            : static_cast<double>(std::bit_cast<float>(
                static_cast<std::uint32_t>(state_.v[rn])));
        const std::int64_t converted = float_to_signed_integer(value);
        if (destination_wide) write_x(rd, static_cast<std::uint64_t>(converted));
        else write_x(rd, static_cast<std::uint32_t>(converted));
        return;
    }

    const std::uint32_t memory_class = instruction & 0xFFC00000U;
    const bool store64 = memory_class == 0xF9000000U;
    const bool load64 = memory_class == 0xF9400000U;
    const bool store32 = memory_class == 0xB9000000U;
    const bool load32 = memory_class == 0xB9400000U;
    const bool store8 = memory_class == 0x39000000U;
    const bool load8 = memory_class == 0x39400000U;
    const bool store16 = memory_class == 0x79000000U;
    const bool load16 = memory_class == 0x79400000U;
    if (store64 || load64 || store32 || load32 || store8 || load8 ||
        store16 || load16) {
        const bool wide = store64 || load64;
        const bool halfword = store16 || load16;
        const bool byte = store8 || load8;
        const bool is_load = load64 || load32 || load8 || load16;
        const unsigned rt = instruction & 0x1FU;
        const unsigned rn = (instruction >> 5U) & 0x1FU;
        const std::uint64_t encoded_offset = (instruction >> 10U) & 0xFFFU;
        const unsigned scale = wide ? 3U : (halfword ? 1U : (byte ? 0U : 2U));
        const std::uint64_t address = read_base(rn) + (encoded_offset << scale);
        if (is_load) {
            if (wide) write_x(rt, memory_.read_u64(address));
            else if (halfword) write_x(rt, memory_.read_u16(address));
            else if (byte) write_x(rt, memory_.read_u8(address));
            else write_x(rt, memory_.read_u32(address));
        } else {
            invalidate_exclusive(address, wide ? 8U :
                                  (halfword ? 2U : (byte ? 1U : 4U)));
            if (wide) memory_.write_u64(address, read_x(rt));
            else if (halfword)
                memory_.write_u16(address, static_cast<std::uint16_t>(read_x(rt)));
            else if (byte)
                memory_.write_u8(address, static_cast<std::uint8_t>(read_x(rt)));
            else memory_.write_u32(address, static_cast<std::uint32_t>(read_x(rt)));
        }
        return;
    }

    if ((instruction & 0x7E000000U) == 0x34000000U) {
        const bool nonzero = ((instruction >> 24U) & 1U) != 0U;
        const bool wide = (instruction >> 31U) != 0U;
        const unsigned rt = instruction & 0x1FU;
        const std::uint64_t value = read_x(rt) & width_mask(wide ? 64U : 32U);
        if ((value != 0U) == nonzero) {
            const std::int64_t displacement = sign_extend(
                (instruction >> 5U) & 0x7FFFFU, 19U);
            state_.pc = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
        }
        return;
    }

    if ((instruction & 0xFC000000U) == 0x94000000U) {
        const std::int64_t displacement = sign_extend(
            instruction & 0x03FFFFFFU, 26U);
        state_.x[30] = state_.pc;
        state_.pc = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
        return;
    }

    if ((instruction & 0xFC000000U) == 0x14000000U) {
        const std::int64_t displacement = sign_extend(
            instruction & 0x03FFFFFFU, 26U);
        state_.pc = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(instruction_pc) + displacement * 4LL);
        return;
    }

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
        if (svc_handler_.yield_requested()) break;
    }
    return executed;
}

} // namespace horizon
