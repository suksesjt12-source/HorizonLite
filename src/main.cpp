#include "horizon/audio.hpp"
#include "horizon/cpu.hpp"
#include "horizon/display.hpp"
#include "horizon/kernel.hpp"
#include "horizon/loader_config.hpp"
#include "horizon/nro_loader.hpp"
#include "horizon/romfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

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
constexpr std::uint64_t kStage8StringsAddress = 0x00112000ULL;
constexpr std::uint64_t kHidMappingAddress = 0x20000000ULL;
constexpr std::uint64_t kSpriteBufferAddress = 0x21000000ULL;
constexpr std::uint64_t kPcmBufferAddress = 0x22000000ULL;
constexpr std::uint64_t kAudioBufferHeaderAddress = 0x22010000ULL;
constexpr std::uint64_t kAudioReleasedArrayAddress = 0x22011000ULL;
constexpr std::uint64_t kStage6ScratchAddress = 0x23000000ULL;
constexpr std::uint64_t kNroLoadBase = 0x01000000ULL;
constexpr std::uint64_t kLoaderConfigAddress = 0x3F000000ULL;
constexpr std::size_t kLoaderConfigRegionSize = 0x1000U;
constexpr std::uint64_t kLoaderHeapCapacity = 0x08000000ULL;
constexpr std::uint64_t kWorkerThreadTextAddress = 0x00108000ULL;
constexpr std::uint64_t kSharedThreadDataOffset = 0x00900000ULL;
constexpr std::uint32_t kMainThreadHandle = 0x101U;
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

    void movn_w(unsigned rd, std::uint16_t immediate) {
        emit(0x12800000U |
             (static_cast<std::uint32_t>(immediate) << 5U) | rd);
    }

    void eor_reg(unsigned rd, unsigned rn, unsigned rm) {
        emit(0xCA000000U | (rm << 16U) | (rn << 5U) | rd);
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

    void strh(unsigned rt, unsigned rn, unsigned offset) {
        emit(0x79000000U | ((offset / 2U) << 10U) | (rn << 5U) | rt);
    }

    void ldaxr_w(unsigned rt, unsigned rn) {
        emit(0x885FFC00U | (rn << 5U) | rt);
    }

    void stlxr_w(unsigned rs, unsigned rt, unsigned rn) {
        emit(0x8800FC00U | (rs << 16U) | (rn << 5U) | rt);
    }

    void scvtf_s(unsigned rd, unsigned rn) {
        emit(0x1E220000U | (rn << 5U) | rd);
    }

    void fmov_s_imm(unsigned rd, std::uint8_t immediate) {
        emit(0x1E201000U |
             (static_cast<std::uint32_t>(immediate) << 13U) | rd);
    }

    void fadd_s(unsigned rd, unsigned rn, unsigned rm) {
        emit(0x1E202800U | (rm << 16U) | (rn << 5U) | rd);
    }

    void fmul_s(unsigned rd, unsigned rn, unsigned rm) {
        emit(0x1E200800U | (rm << 16U) | (rn << 5U) | rd);
    }

    void fcvtzs_w_s(unsigned rd, unsigned rn) {
        emit(0x1E380000U | (rn << 5U) | rd);
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
    Receive,
    AutoSend,
    AutoReceive
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
    } else if (descriptor_kind == IpcDescriptorKind::Send ||
               descriptor_kind == IpcDescriptorKind::AutoSend) {
        header0 |= 1U << 20U;
        descriptor_bytes = 12U;
    } else if (descriptor_kind == IpcDescriptorKind::Receive ||
               descriptor_kind == IpcDescriptorKind::AutoReceive) {
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
               descriptor_kind == IpcDescriptorKind::Receive ||
               descriptor_kind == IpcDescriptorKind::AutoSend ||
               descriptor_kind == IpcDescriptorKind::AutoReceive) {
        a.mov64(0U, static_cast<std::uint32_t>(descriptor_size));
        a.str_w(0U, 20U, 8U);
        a.mov64(0U, static_cast<std::uint32_t>(descriptor_address));
        a.str_w(0U, 20U, 12U);
        const std::uint32_t descriptor_type =
            descriptor_kind == IpcDescriptorKind::AutoSend ? 0x21U :
            descriptor_kind == IpcDescriptorKind::AutoReceive ? 0x22U : 0U;
        a.movz(0U, static_cast<std::uint16_t>(descriptor_type));
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

void emit_cmif_request_send_receive(
    A64Assembler& a, unsigned command_id,
    std::uint64_t send_address, std::uint64_t send_size,
    std::uint64_t receive_address, std::uint64_t receive_size,
    const std::vector<std::uint8_t>& payload = {}) {
    constexpr std::uint32_t header0 =
        kHipcInvoke2Method | (1U << 20U) | (1U << 24U);
    const std::size_t raw_words = (0x10U + payload.size() + 3U) / 4U;
    if (raw_words > 0x3FFU)
        throw std::runtime_error("guest self-test CMIF request exceeds HIPC raw limit");
    constexpr std::size_t cmif_offset = 0x20U;
    a.mov64(0U, header0);
    a.str_w(0U, 20U, 0U);
    a.mov64(0U, static_cast<std::uint32_t>(raw_words));
    a.str_w(0U, 20U, 4U);
    a.mov64(0U, static_cast<std::uint32_t>(send_size));
    a.str_w(0U, 20U, 8U);
    a.mov64(0U, static_cast<std::uint32_t>(send_address));
    a.str_w(0U, 20U, 12U);
    a.movz(0U, 0U);
    a.str_w(0U, 20U, 16U);
    a.mov64(0U, static_cast<std::uint32_t>(receive_size));
    a.str_w(0U, 20U, 20U);
    a.mov64(0U, static_cast<std::uint32_t>(receive_address));
    a.str_w(0U, 20U, 24U);
    a.movz(0U, 0U);
    a.str_w(0U, 20U, 28U);
    const auto store_word = [&a](std::size_t offset, std::uint32_t value) {
        a.mov64(0U, value);
        a.str_w(0U, 20U, static_cast<unsigned>(cmif_offset + offset));
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

std::vector<std::uint32_t> build_worker_thread_program() {
    A64Assembler a;
    a.mov_reg(19U, 0U);

    a.bind("worker_lock_retry");
    a.ldaxr_w(2U, 19U);
    a.cbz(2U, "worker_try_atomic_lock");
    a.mov_reg(0U, 2U);
    a.mov_reg(1U, 19U);
    a.ldr_w(2U, 19U, 8U);
    a.svc(0x1AU);
    a.branch("worker_mutex_acquired");

    a.bind("worker_try_atomic_lock");
    a.ldr_w(3U, 19U, 8U);
    a.stlxr_w(4U, 3U, 19U);
    a.cbnz(4U, "worker_lock_retry");

    a.bind("worker_mutex_acquired");
    a.ldr_w(5U, 19U, 4U);
    a.scvtf_s(0U, 5U);
    a.fmov_s_imm(1U, 0x70U);
    a.fmul_s(2U, 0U, 1U);
    a.fadd_s(2U, 2U, 1U);
    a.fcvtzs_w_s(5U, 2U);
    a.str_w(5U, 19U, 4U);

    a.mov_reg(0U, 19U);
    a.svc(0x1BU);
    a.mov64(0U, 1000000U);
    a.svc(0x0CU);
    a.branch("worker_lock_retry");
    return a.finish();
}

std::vector<std::uint32_t> build_self_test_program() {
    A64Assembler a;

    a.mov64(1U, kSelfTestStringsAddress + 12U);
    a.svc(0x1FU);
    a.mov_reg(19U, 1U);
    a.mrs_tpidrro(20U);

    emit_named_service_request(a, kStage8StringsAddress);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(14U, 20U, 12U);
    a.mov64(2U, kStage6ScratchAddress);
    a.str_w(14U, 2U, 0xB4U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 14U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 32U);
    a.str_w(10U, 2U, 0xB0U);
    emit_cmif_request(a, 4U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 14U);
    a.svc(0x21U);
    a.ldr_x(10U, 20U, 32U);
    a.ldr_x(11U, 20U, 40U);
    a.str_x(10U, 2U, 0xC0U);
    a.str_x(11U, 2U, 0xC8U);

    std::vector<std::uint8_t> user_uid_payload(16U, 0U);
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        user_uid_payload[byte] = static_cast<std::uint8_t>(0x0000000000000001ULL >> (byte * 8U));
        user_uid_payload[8U + byte] = static_cast<std::uint8_t>(0x1122334455667788ULL >> (byte * 8U));
    }
    emit_cmif_request(a, 1U, IpcDescriptorKind::None, 0U, 0U, user_uid_payload);
    a.mov_reg(0U, 14U);
    a.svc(0x21U);
    emit_cmif_request(a, 5U, IpcDescriptorKind::None, 0U, 0U, user_uid_payload);
    a.mov_reg(0U, 14U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0xCCU);
    emit_cmif_request(a, 1U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0xCCU);
    a.svc(0x21U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::Receive,
                      kStage6ScratchAddress + 0x600U, 0x80U);
    a.ldr_w(0U, 2U, 0xCCU);
    a.svc(0x21U);

    emit_named_service_request(a, kStage8StringsAddress + 7U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(13U, 20U, 12U);
    a.str_w(13U, 2U, 0xD8U);
    emit_cmif_request(a, 3U, IpcDescriptorKind::Receive,
                      kStage6ScratchAddress + 0x500U, 0x100U);
    a.ldr_w(0U, 2U, 0xD8U);
    a.svc(0x21U);
    emit_named_service_request(a, kStage8StringsAddress + 15U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(12U, 20U, 12U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 12U);
    a.svc(0x21U);
    a.ldr_x(10U, 20U, 32U);
    a.str_x(10U, 2U, 0xE0U);
    emit_cmif_request(a, 4U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 12U);
    a.svc(0x21U);

    a.movz(0U, 0U);
    a.mov64(1U, 0x00C00000U);
    a.svc(0x01U);
    a.mov64(2U, kStage6ScratchAddress);
    a.str_x(1U, 2U, 0x80U);

    a.mov_reg(8U, 1U);
    a.mov64(10U, kSharedThreadDataOffset);
    a.add_reg(9U, 8U, 10U);
    a.movz(11U, 0U);
    a.str_w(11U, 9U, 0U);
    a.movz(11U, 100U);
    a.str_w(11U, 9U, 4U);
    a.mov64(1U, kWorkerThreadTextAddress);
    a.mov_reg(2U, 9U);
    a.mov64(10U, 0x00C00000U);
    a.add_reg(3U, 8U, 10U);
    a.mov64(4U, 0x2CU);
    a.movz(5U, 0U);
    a.svc(0x08U);
    a.str_w(1U, 9U, 8U);
    a.mov_reg(0U, 1U);
    a.svc(0x0AU);

    emit_named_service_request(a, kSelfTestStringsAddress + 48U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.mov64(2U, kStage6ScratchAddress);
    a.str_w(10U, 2U, 0x88U);

    emit_cmif_request(a, 0U, IpcDescriptorKind::Send,
                      kSelfTestStringsAddress + 64U, 11U);
    a.ldr_w(0U, 2U, 0x88U);
    a.svc(0x21U);
    a.ldr_w(11U, 20U, 32U);
    a.str_w(11U, 2U, 0x8CU);

    a.mov64(0U, 0x00800000U);
    a.str_w(0U, 2U, 0x200U);
    a.mov64(0U, 0U);
    a.str_w(0U, 2U, 0x204U);
    const std::vector<std::uint8_t> create_ioctl{
        0U, 0U, 0U, 0U, 0x01U, 0x01U, 0x08U, 0xC0U
    };
    emit_cmif_request(a, 1U, IpcDescriptorKind::Send,
                      kStage6ScratchAddress + 0x200U, 8U, create_ioctl);
    a.ldr_w(0U, 2U, 0x8CU);
    a.str_w(0U, 20U, 0x30U);
    a.ldr_w(0U, 2U, 0x88U);
    a.svc(0x21U);
    a.ldr_w(10U, 2U, 0x204U);
    a.str_w(10U, 2U, 0x90U);

    a.ldr_w(0U, 2U, 0x90U);
    a.str_w(0U, 2U, 0x220U);
    a.mov64(0U, 1U);
    a.str_w(0U, 2U, 0x224U);
    a.movz(0U, 0U);
    a.str_w(0U, 2U, 0x228U);
    a.mov64(0U, 0x1000U);
    a.str_w(0U, 2U, 0x22CU);
    a.movz(0U, 0U);
    a.str_w(0U, 2U, 0x230U);
    a.str_w(0U, 2U, 0x234U);
    a.ldr_x(1U, 2U, 0x80U);
    a.mov64(0U, 0x00100000U);
    a.add_reg(0U, 1U, 0U);
    a.str_x(0U, 2U, 0x238U);
    a.str_x(0U, 2U, 0xA0U);
    const std::vector<std::uint8_t> alloc_ioctl{
        0U, 0U, 0U, 0U, 0x04U, 0x01U, 0x20U, 0xC0U
    };
    emit_cmif_request(a, 1U, IpcDescriptorKind::Send,
                      kStage6ScratchAddress + 0x220U, 32U, alloc_ioctl);
    a.ldr_w(0U, 2U, 0x8CU);
    a.str_w(0U, 20U, 0x30U);
    a.ldr_w(0U, 2U, 0x88U);
    a.svc(0x21U);

    emit_named_service_request(a, kSelfTestStringsAddress + 56U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0x94U);

    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x94U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0x98U);

    emit_cmif_request(a, 0U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0x98U);

    emit_cmif_request(a, 1010U, IpcDescriptorKind::Send,
                      kSelfTestStringsAddress + 80U, 8U);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);

    const std::vector<std::uint8_t> open_layer_payload(16U, 0U);
    emit_cmif_request_send_receive(
        a, 2020U, kSelfTestStringsAddress + 80U, 8U,
        kStage6ScratchAddress + 0x340U, 0x100U, open_layer_payload);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);

    emit_cmif_request(a, 100U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0x9CU);

    emit_cmif_request(a, 5202U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0xA8U);

    emit_cmif_request(a, 2101U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x98U);
    a.svc(0x21U);

    emit_cmif_request(a, 2U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0xACU);

    const std::vector<std::uint8_t> connect_parcel{
        1U, 0U, 0U, 0U, 10U, 0U, 0U, 0U
    };
    emit_cmif_request_send_receive(
        a, 0U, kStage6ScratchAddress + 0x350U, 0U,
        kStage6ScratchAddress + 0x3A0U, 0x80U, connect_parcel);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);

    a.mov64(0U, 0x43524150U);
    a.str_w(0U, 2U, 0x300U);
    a.mov64(0U, 1U);
    a.str_w(0U, 2U, 0x304U);
    a.movz(0U, 0U);
    a.str_w(0U, 2U, 0x308U);
    a.ldr_w(0U, 2U, 0x90U);
    a.str_w(0U, 2U, 0x30CU);
    a.movz(0U, 0U);
    a.str_x(0U, 2U, 0x310U);
    const std::vector<std::uint8_t> set_preallocated{
        1U, 0U, 0U, 0U, 14U, 0U, 0U, 0U
    };
    emit_cmif_request_send_receive(
        a, 0U, kStage6ScratchAddress + 0x300U, 24U,
        kStage6ScratchAddress + 0x3A0U, 8U, set_preallocated);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);

    a.mov64(0U, 1U);
    a.str_w(0U, 2U, 0x308U);
    a.mov64(0U, 0x00400000U);
    a.str_x(0U, 2U, 0x310U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::Send,
                      kStage6ScratchAddress + 0x300U, 24U,
                      set_preallocated);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);

    const std::vector<std::uint8_t> request_buffer_tx{
        1U, 0U, 0U, 0U, 1U, 0U, 0U, 0U
    };
    a.movz(0U, 0U);
    a.str_w(0U, 2U, 0x308U);
    emit_cmif_request_send_receive(
        a, 0U, kStage6ScratchAddress + 0x300U, 12U,
        kStage6ScratchAddress + 0x3A0U, 0x80U, request_buffer_tx);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);

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

    std::vector<std::uint8_t> save_attribute(0x48U, 0U);
    const std::uint64_t save_program_id = 0x0100000000000000ULL;
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        save_attribute[8U + byte] =
            static_cast<std::uint8_t>(save_program_id >> (byte * 8U));
        save_attribute[16U + byte] =
            static_cast<std::uint8_t>(0x0000000000000001ULL >> (byte * 8U));
        save_attribute[24U + byte] =
            static_cast<std::uint8_t>(0x1122334455667788ULL >> (byte * 8U));
    }
    emit_cmif_request(a, 51U, IpcDescriptorKind::None, 0U, 0U,
                      save_attribute);
    a.mov_reg(0U, 25U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 12U);
    a.mov64(2U, kStage6ScratchAddress);
    a.str_w(10U, 2U, 0xE8U);

    const std::vector<std::uint8_t> open_save_file_payload{
        3U, 0U, 0U, 0U
    };
    emit_cmif_request(a, 8U, IpcDescriptorKind::Pointer,
                      kStage8StringsAddress + 19U, 9U,
                      open_save_file_payload);
    a.ldr_w(0U, 2U, 0xE8U);
    a.svc(0x21U);
    a.ldr_w(10U, 20U, 24U);
    a.cbz(10U, "save_file_opened");
    const std::vector<std::uint8_t> create_save_file_payload(8U, 0U);
    emit_cmif_request(a, 0U, IpcDescriptorKind::Pointer,
                      kStage8StringsAddress + 19U, 9U,
                      create_save_file_payload);
    a.ldr_w(0U, 2U, 0xE8U);
    a.svc(0x21U);
    emit_cmif_request(a, 8U, IpcDescriptorKind::Pointer,
                      kStage8StringsAddress + 19U, 9U,
                      open_save_file_payload);
    a.ldr_w(0U, 2U, 0xE8U);
    a.svc(0x21U);
    a.bind("save_file_opened");
    a.ldr_w(10U, 20U, 12U);
    a.str_w(10U, 2U, 0xECU);

    const std::vector<std::uint8_t> read_save_payload{
        0U,0U,0U,0U,0U,0U,0U,0U,
        8U,0U,0U,0U,0U,0U,0U,0U
    };
    emit_cmif_request(a, 0U, IpcDescriptorKind::Receive,
                      kStage6ScratchAddress + 0x420U, 8U,
                      read_save_payload);
    a.ldr_w(0U, 2U, 0xECU);
    a.svc(0x21U);
    a.ldr_x(10U, 20U, 32U);
    a.cbz(10U, "save_position_default");
    a.ldr_w(19U, 2U, 0x420U);
    a.ldr_w(18U, 2U, 0x424U);
    a.branch("save_position_seed");
    a.bind("save_position_default");
    a.movz(19U, 100U);
    a.movz(18U, 100U);
    a.bind("save_position_seed");
    a.ldr_x(10U, 2U, 0x80U);
    a.mov64(11U, kSharedThreadDataOffset);
    a.add_reg(10U, 10U, 11U);
    a.str_w(19U, 10U, 4U);
    a.mov64(2U, kStage6ScratchAddress);
    a.str_w(19U, 2U, 0xF0U);
    a.str_w(18U, 2U, 0xF4U);

    emit_named_service_request(a, kSelfTestStringsAddress + 16U);
    a.mov_reg(0U, 19U);
    a.svc(0x21U);
    a.ldr_w(27U, 20U, 12U);

    emit_cmif_request(a, 1U, IpcDescriptorKind::AutoSend,
                      kSelfTestStringsAddress + 32U, 16U);
    a.mov_reg(0U, 27U);
    a.svc(0x21U);
    a.ldr_w(28U, 20U, 12U);

    emit_cmif_request(a, 4U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 28U);
    a.svc(0x21U);
    a.ldr_w(23U, 20U, 12U);

    emit_cmif_request(a, 1U, IpcDescriptorKind::None, 0U, 0U);
    a.mov_reg(0U, 28U);
    a.svc(0x21U);

    a.mov64(0U, kPcmBufferAddress);
    a.movz(1U, 4800U);
    a.movz(2U, 55U);
    a.movz(3U, 0U);
    a.movz(4U, 8000U);
    a.movn_w(5U, 0x1F3FU);
    a.movz(6U, 1U);
    a.bind("beep_frame");
    a.cbz(1U, "beep_done");
    a.cbz(3U, "beep_positive");
    a.mov_reg(7U, 5U);
    a.branch("beep_sample_ready");
    a.bind("beep_positive");
    a.mov_reg(7U, 4U);
    a.bind("beep_sample_ready");
    a.strh(7U, 0U, 0U);
    a.strh(7U, 0U, 2U);
    a.add_imm(0U, 0U, 4U);
    a.subs_imm(1U, 1U, 1U);
    a.subs_imm(2U, 2U, 1U);
    a.cbnz(2U, "beep_frame");
    a.eor_reg(3U, 3U, 6U);
    a.movz(2U, 55U);
    a.branch("beep_frame");
    a.bind("beep_done");

    a.mov64(0U, kAudioBufferHeaderAddress);
    a.movz(1U, 0U);
    a.str_x(1U, 0U, 0U);
    a.mov64(1U, kPcmBufferAddress);
    a.str_x(1U, 0U, 8U);
    a.mov64(1U, 19200U);
    a.str_x(1U, 0U, 16U);
    a.str_x(1U, 0U, 24U);
    a.movz(1U, 0U);
    a.str_x(1U, 0U, 32U);

    a.mov64(15U, kHidMappingAddress + 0x9A28U);
    a.mov64(16U, kSpriteBufferAddress);
    a.mov64(2U, kStage6ScratchAddress);
    a.ldr_w(19U, 2U, 0xF0U);
    a.ldr_w(18U, 2U, 0xF4U);
    a.mov64(17U, 0xFF101820U);
    a.movz(24U, 0U);

    a.bind("frame_loop");
    const std::vector<std::uint8_t> dequeue_tx{
        1U, 0U, 0U, 0U, 3U, 0U, 0U, 0U
    };
    emit_cmif_request_send_receive(
        a, 0U, kStage6ScratchAddress + 0x350U, 0U,
        kStage6ScratchAddress + 0x380U, 8U, dequeue_tx);
    a.mov64(2U, kStage6ScratchAddress);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);
    a.ldr_w(10U, 2U, 0x384U);
    a.str_w(10U, 2U, 0xB0U);
    a.ldr_x(29U, 2U, 0xA0U);
    a.cbz(10U, "use_framebuffer_slot_zero");
    a.add_imm(29U, 29U, 0x00400000U, true);
    a.bind("use_framebuffer_slot_zero");

    a.mov64(10U, kStage6ScratchAddress);
    a.ldr_x(13U, 10U, 0x80U);
    a.mov64(11U, kSharedThreadDataOffset);
    a.add_reg(13U, 13U, 11U);

    a.bind("main_lock_retry");
    a.ldaxr_w(10U, 13U);
    a.cbz(10U, "main_try_atomic_lock");
    a.mov_reg(0U, 10U);
    a.mov_reg(1U, 13U);
    a.mov64(2U, kMainThreadHandle);
    a.svc(0x1AU);
    a.branch("main_mutex_acquired");

    a.bind("main_try_atomic_lock");
    a.mov64(11U, kMainThreadHandle);
    a.stlxr_w(12U, 11U, 13U);
    a.cbnz(12U, "main_lock_retry");

    a.bind("main_mutex_acquired");
    a.movz(0U, 0U);
    a.svc(0x0CU);
    a.ldr_w(19U, 13U, 4U);
    a.mov_reg(0U, 13U);
    a.svc(0x1BU);

    a.mov_reg(0U, 19U);
    a.mov_reg(1U, 18U);
    a.mov_reg(2U, 17U);
    a.branch_link("draw_solid_sprite_area");

    a.ldr_x(0U, 15U, 0U);
    a.mov_reg(26U, 0U);
    a.and_imm(1U, 26U, 52U, 0U);
    a.cbz(1U, "not_left");
    a.sub_imm(19U, 19U, 2U);
    a.bind("not_left");

    a.and_imm(1U, 26U, 51U, 0U);
    a.cbz(1U, "not_up");
    a.sub_imm(18U, 18U, 2U);
    a.bind("not_up");

    a.and_imm(1U, 26U, 50U, 0U);
    a.cbz(1U, "not_right");
    a.add_imm(19U, 19U, 2U);
    a.bind("not_right");

    a.and_imm(1U, 26U, 49U, 0U);
    a.cbz(1U, "not_down");
    a.add_imm(18U, 18U, 2U);
    a.bind("not_down");

    a.and_imm(1U, 26U, 0U, 0U);
    a.cbz(1U, "audio_button_released");
    a.cbnz(24U, "audio_button_done");
    emit_cmif_request(a, 7U, IpcDescriptorKind::AutoSend,
                      kAudioBufferHeaderAddress, 0x28U);
    a.mov_reg(0U, 28U);
    a.svc(0x21U);
    a.movz(24U, 1U);
    a.branch("audio_button_done");
    a.bind("audio_button_released");
    a.movz(24U, 0U);
    a.bind("audio_button_done");

    emit_cmif_request(a, 8U, IpcDescriptorKind::AutoReceive,
                      kAudioReleasedArrayAddress, 0x20U);
    a.mov_reg(0U, 28U);
    a.svc(0x21U);

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

    a.and_imm(1U, 26U, 62U, 0U);
    a.cbz(1U, "persistent_save_done");
    a.mov64(2U, kStage6ScratchAddress);
    a.str_w(19U, 2U, 0x420U);
    a.str_w(18U, 2U, 0x424U);
    const std::vector<std::uint8_t> write_save_payload{
        0U,0U,0U,0U,0U,0U,0U,0U,
        8U,0U,0U,0U,0U,0U,0U,0U
    };
    emit_cmif_request(a, 1U, IpcDescriptorKind::Send,
                      kStage6ScratchAddress + 0x420U, 8U,
                      write_save_payload);
    a.ldr_w(0U, 2U, 0xECU);
    a.svc(0x21U);
    emit_cmif_request(a, 2U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0xECU);
    a.svc(0x21U);
    emit_cmif_request(a, 10U, IpcDescriptorKind::None, 0U, 0U);
    a.ldr_w(0U, 2U, 0xE8U);
    a.svc(0x21U);
    a.bind("persistent_save_done");

    a.mov_reg(0U, 19U);
    a.mov_reg(1U, 18U);
    a.branch_link("draw_rgba_sprite");

    a.mov64(2U, kStage6ScratchAddress);
    a.mov64(0U, 0x43524150U);
    a.str_w(0U, 2U, 0x300U);
    a.mov64(0U, 1U);
    a.str_w(0U, 2U, 0x304U);
    a.ldr_w(0U, 2U, 0xB0U);
    a.str_w(0U, 2U, 0x308U);
    const std::vector<std::uint8_t> queue_tx{
        1U, 0U, 0U, 0U, 7U, 0U, 0U, 0U
    };
    emit_cmif_request_send_receive(
        a, 3U, kStage6ScratchAddress + 0x300U, 12U,
        kStage6ScratchAddress + 0x3C0U, 4U, queue_tx);
    a.ldr_w(0U, 2U, 0x9CU);
    a.svc(0x21U);
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
    a.movz(5U, 5120U);
    a.bind("solid_row");
    a.mov_reg(11U, 6U);
    a.movz(13U, 16U);
    a.bind("solid_column");
    a.str_w(10U, 11U, 0U);
    a.add_imm(11U, 11U, 4U);
    a.subs_imm(13U, 13U, 1U);
    a.bcond(1U, "solid_column");
    a.add_reg(6U, 6U, 5U);
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
    a.movz(5U, 5120U);
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
    a.add_reg(6U, 6U, 5U);
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
              << "  No file: run the built-in vi:m/nvdrv Binder display self-test.\n"
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
    memory.map_zeroed(kStage6ScratchAddress, 0x1000U,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kStackAddress, kStackSize,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kTlsAddress, kTlsSize,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kSpriteBufferAddress, 0x1000U,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kPcmBufferAddress, 0x20000U,
                      horizon::Permission::Read | horizon::Permission::Write);
    memory.map_zeroed(kLoaderConfigAddress, kLoaderConfigRegionSize,
                      horizon::Permission::Read | horizon::Permission::Write);
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
        's', 'm', ':', 0U,
        'a', 'u', 'd', 'o', 'u', 't', ':', 'u', 0U,
        0U, 0U, 0U, 0U, 0U, 0U, 0U,
        'D', 'e', 'v', 'i', 'c', 'e', 'O', 'u', 't', 0U, 0U, 0U, 0U, 0U, 0U, 0U,
        'n', 'v', 'd', 'r', 'v', 0U, 0U, 0U,
        'v', 'i', ':', 'm', 0U, 0U, 0U, 0U,
        '/', 'd', 'e', 'v', '/', 'n', 'v', 'm', 'a', 'p', 0U,
        0U, 0U, 0U, 0U, 0U,
        'D', 'e', 'f', 'a', 'u', 'l', 't', 0U
    };
    memory.map(kSelfTestTextAddress, std::move(text),
               horizon::Permission::Read | horizon::Permission::Execute);
    const auto worker_instructions = build_worker_thread_program();
    std::vector<std::uint8_t> worker_text;
    worker_text.reserve(worker_instructions.size() * sizeof(std::uint32_t));
    for (const std::uint32_t instruction : worker_instructions) {
        for (unsigned byte = 0U; byte < 4U; ++byte)
            worker_text.push_back(static_cast<std::uint8_t>(instruction >> (byte * 8U)));
    }
    memory.map(kWorkerThreadTextAddress, std::move(worker_text),
               horizon::Permission::Read | horizon::Permission::Execute);
    memory.map(kSelfTestStringsAddress, service_strings,
               horizon::Permission::Read);
    const std::vector<std::uint8_t> stage8_strings{
        'a','c','c',':','u','0',0U,
        's','e','t',':','s','y','s',0U,
        's','e','t',0U,
        '/','p','o','s','.','b','i','n',0U
    };
    memory.map(kStage8StringsAddress, stage8_strings,
               horizon::Permission::Read);
    std::cout << "HorizonLite Stage 8 self-test: Account, System Settings and persistent save-data IPC, "
                 "plus threaded sprite motion through vi:m.\n";
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
    const horizon::NroImage image = horizon::NroLoader::load(
        file, memory, kNroLoadBase);
    std::cout << "Loaded NRO0 image: " << *options.nro_path << '\n'
              << "  Declared size: 0x" << std::hex << image.declared_size << '\n'
              << "  Entry point:   0x" << image.entry_point << std::dec << '\n'
              << "  BSS:           0x" << std::hex << image.bss_base << "+0x"
              << image.bss_size << std::dec << " bytes\n"
              << "  ASET RomFS:    " << image.romfs.size() << " bytes\n";
    return LoadedProgram{image.entry_point, image.romfs, false};
}

bool verify_loader_config(const horizon::AddressSpace& memory,
                           std::uint64_t address,
                           std::uint64_t thread_handle,
                           std::uint64_t heap_address,
                           std::uint64_t heap_size) {
    constexpr std::array<std::uint32_t, 6> expected_keys{1U, 3U, 7U, 10U, 14U, 0U};
    for (std::size_t index = 0U; index < expected_keys.size(); ++index) {
        const std::uint64_t entry = address + index * sizeof(horizon::LoaderConfigEntry);
        if (memory.read_u32(entry) != expected_keys[index] ||
            memory.read_u32(entry + 4U) != 0U)
            return false;
    }
    return memory.read_u64(address + 8U) == thread_handle &&
           memory.read_u64(address + 24U + 8U) == heap_address &&
           memory.read_u64(address + 24U + 16U) == heap_size &&
           memory.read_u64(address + 48U + 8U) == 0U &&
           memory.read_u64(address + 72U + 8U) == 0xFFFF8001ULL &&
           memory.read_u64(address + 96U + 8U) != 0U &&
           memory.read_u64(address + 96U + 16U) != 0U;
}

void verify_self_test(const horizon::AddressSpace& memory,
                       std::span<const std::uint8_t> pixels,
                       std::uint64_t frame_number) {
    constexpr std::uint32_t expected_rgba = 0xFFF0B842U;
    if (pixels.size() < kFramebufferBytes || frame_number == 0U)
        throw std::runtime_error("Display self-test did not queue a complete framebuffer");

    bool sprite_pixel_found = false;
    for (std::size_t offset = 0U; offset + 4U <= kFramebufferBytes; offset += 4U) {
        const std::uint32_t pixel =
            static_cast<std::uint32_t>(pixels[offset]) |
            (static_cast<std::uint32_t>(pixels[offset + 1U]) << 8U) |
            (static_cast<std::uint32_t>(pixels[offset + 2U]) << 16U) |
            (static_cast<std::uint32_t>(pixels[offset + 3U]) << 24U);
        if (pixel == expected_rgba) {
            sprite_pixel_found = true;
            break;
        }
    }

    const std::uint64_t heap_base = memory.read_u64(kStage6ScratchAddress + 0x80U);
    const std::uint64_t shared_data =
        heap_base + kSharedThreadDataOffset;
    const std::uint32_t worker_position =
        memory.read_u32(shared_data + 4U);
    const std::filesystem::path save_path =
        std::filesystem::path("save_data") /
        "11223344556677880000000000000001" /
        "0100000000000000" / "pos.bin";
    if (!std::filesystem::exists(save_path) ||
        std::filesystem::file_size(save_path) != 8U) {
        throw std::runtime_error("persistent save-data file was not created with the 8-byte sprite state");
    }
    if (!sprite_pixel_found || worker_position == 100U) {
        std::cerr << "  Sprite pixel found: " << sprite_pixel_found
                  << "  Worker position: " << worker_position << '\n';
        throw std::runtime_error(
            "vi:m QueueBuffer did not present the moving RomFS sprite from the worker thread");
    }
    std::uint32_t saved_x = 0U;
    std::uint32_t saved_y = 0U;
    {
        std::ifstream input(save_path, std::ios::binary);
        input.read(reinterpret_cast<char*>(&saved_x), sizeof(saved_x));
        input.read(reinterpret_cast<char*>(&saved_y), sizeof(saved_y));
        if (!input)
            throw std::runtime_error("saved sprite coordinates could not be read back");
    }
    std::cout << "Account + set:sys + fsp-srv persistent save-data self-test passed\n"
              << "  Saved sprite: (" << saved_x << ", " << saved_y << ")"
              << "  Worker position: " << worker_position
              << "  Frames: " << frame_number << '\n';
}
int run_headless(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel,
                 const horizon::AddressSpace& memory, bool self_test) {
    if (self_test) {
        kernel.update_hid_input(horizon::HidButtonA | horizon::HidButtonX);
        const std::size_t executed =
            kernel.run_scheduler(cpu, kHeadlessInstructionLimit, kStopAddress);
        std::cout << "Executed guest instructions: " << executed << '\n';
        kernel.print_ipc_statistics();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        kernel.update_audio_events();
        verify_self_test(memory, kernel.display_pixels(),
                        kernel.display_frame_number());
        return 0;
    }

    const std::size_t executed =
        kernel.run_scheduler(cpu, 20000000U, kStopAddress);
    std::cout << "Executed instructions: " << executed << '\n';
    kernel.print_ipc_statistics();
    if (kernel.running()) {
        std::cerr << "Execution reached the headless instruction limit.\n";
        return 3;
    }
    return 0;
}

int run_interactive(horizon::Arm64Cpu& cpu, horizon::HorizonKernel& kernel) {
    horizon::SdlDisplay display;
    kernel.attach_gl_renderer(display.gl_renderer());
    std::cout << "Graphics backend: "
              << (display.hardware_renderer_available() ? "SDL OpenGL 3.3 Core" : "SDL 2D fallback")
              << '\\n';
    constexpr auto frame_duration = std::chrono::microseconds(16667);
    bool keep_open = true;
    std::size_t executed_total = 0U;
    while (keep_open && kernel.running()) {
        const auto frame_start = std::chrono::steady_clock::now();
        keep_open = display.process_events();
        if (!keep_open) {
            kernel.request_applet_exit();
            const std::size_t cleanup_instructions =
                kernel.run_scheduler(cpu, 500000U, kStopAddress, 750U);
            executed_total += cleanup_instructions;
            if (kernel.running())
                std::cout << "SDL2 close: applet ExitRequest was queued; "
                             "guest did not exit within the cleanup instruction budget.\n";
            break;
        }

        kernel.update_hid_input(display.read_controller_buttons());
        kernel.update_audio_events();
        if (kernel.running())
            executed_total += kernel.run_scheduler(
                cpu, kInteractiveInstructionBudget, kStopAddress, 750U);
        kernel.update_audio_events();
        display.render(kernel.display_pixels(),
                       kFramebufferWidth, kFramebufferHeight);
        kernel.signal_vsync();
        std::this_thread::sleep_until(frame_start + frame_duration);
    }
    std::cout << "Executed guest instructions: " << executed_total << '\n';
    kernel.print_ipc_statistics();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        horizon::AddressSpace memory;
        initialize_machine_memory(memory);
        LoadedProgram program = load_program(options, memory);
        horizon::SdlAudioOutput audio(options.headless);

        horizon::HorizonKernel kernel(memory, std::move(program.romfs), audio);
        horizon::SvcHandler svc_handler(kernel);
        horizon::Arm64Cpu cpu(memory, svc_handler);
        cpu.state().pc = program.entry_point;
        cpu.state().sp = kStackAddress + kStackSize - 16U;
        cpu.state().tpidrro_el0 = kTlsAddress;

        kernel.prepare_loader_heap(kLoaderHeapCapacity);
        kernel.initialize_scheduler(cpu.state());

        std::random_device random_source;
        horizon::LoaderConfigValues loader_values;
        loader_values.main_thread_handle = kernel.current_thread_handle();
        loader_values.heap_address = kernel.loader_heap_base();
        loader_values.heap_size = kernel.loader_heap_size();
        loader_values.random_seed[0] = horizon::make_random_seed(random_source);
        loader_values.random_seed[1] = horizon::make_random_seed(random_source);
        horizon::write_loader_config(memory, kLoaderConfigAddress, loader_values);

        cpu.state().x[0] = kLoaderConfigAddress;
        cpu.state().x[1] = std::numeric_limits<std::uint64_t>::max();
        kernel.update_main_thread_context(cpu.state());

        if (program.self_test) {
            if (!verify_loader_config(memory, kLoaderConfigAddress,
                    kernel.current_thread_handle(), kernel.loader_heap_base(),
                    kernel.loader_heap_size()))
                throw std::runtime_error("LoaderConfig self-test failed");
            if (!kernel.validate_stage9_services())
                throw std::runtime_error("appletOE/time:u self-test failed");
            std::cout << "LoaderConfig + appletOE + time:u self-test passed\n";
        }

        if (options.headless)
            return run_headless(cpu, kernel, memory, program.self_test);
        return run_interactive(cpu, kernel);
    } catch (const std::exception& error) {
        std::cerr << "HorizonLite: " << error.what() << '\n';
        return 1;
    }
}
