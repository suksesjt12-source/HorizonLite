#include "horizon/gpu.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

static void packet(std::vector<std::uint32_t>& words, std::uint32_t method,
                   std::uint32_t value) {
    words.push_back((1U << 29U) | (1U << 16U) | method);
    words.push_back(value);
}

int main() {
    horizon::AddressSpace memory;
    memory.map_zeroed(0x100000U, 0x40000U,
        horizon::Permission::Read | horizon::Permission::Write);
    horizon::GpuAddressSpace as;
    const auto rt = as.allocate(64U * 64U * 4U, 0x1000U);
    as.map_buffer(1U, rt, 0x100000U, 64U * 64U * 4U);
    const auto vb = as.allocate(3U * 28U, 0x1000U);
    as.map_buffer(2U, vb, 0x110000U, 3U * 28U);
    const std::array<float, 21> vertices{
        -0.8F,-0.8F,0.0F,1.0F,0.0F,0.0F,1.0F,
         0.8F,-0.8F,0.0F,0.0F,1.0F,0.0F,1.0F,
         0.0F, 0.8F,0.0F,0.0F,0.0F,1.0F,1.0F
    };
    const auto* raw = reinterpret_cast<const std::uint8_t*>(vertices.data());
    memory.write(0x110000U, std::vector<std::uint8_t>(raw, raw + sizeof(vertices)));

    std::vector<std::uint32_t> words;
    packet(words, 0x0000U, 0xB197U);
    packet(words, 0x0200U, static_cast<std::uint32_t>(rt >> 32U));
    packet(words, 0x0201U, static_cast<std::uint32_t>(rt));
    packet(words, 0x0202U, 64U);
    packet(words, 0x0203U, 64U);
    packet(words, 0x0360U, 0U);
    packet(words, 0x0361U, 0U);
    packet(words, 0x0362U, 0U);
    packet(words, 0x0363U, 0x3F800000U);
    packet(words, 0x0674U, 4U);
    packet(words, 0x0700U, static_cast<std::uint32_t>(vb >> 32U));
    packet(words, 0x0701U, static_cast<std::uint32_t>(vb));
    packet(words, 0x0702U, 28U);
    packet(words, 0x035EU, 3U << 16U);
    const std::uint64_t push_guest = 0x120000U;
    for (std::size_t i = 0; i < words.size(); ++i)
        memory.write_u32(push_guest + i * 4U, words[i]);
    const auto push_va = as.allocate(words.size() * 4U, 0x1000U);
    as.map_buffer(3U, push_va, push_guest, words.size() * 4U);
    const std::uint64_t gpfifo_guest = 0x130000U;
    memory.write_u64(gpfifo_guest, push_va | (static_cast<std::uint64_t>(words.size()) << 42U));

    horizon::GpuChannel channel(memory, as);
    channel.allocate_gpfifo(8U);
    assert(channel.allocate_object_context(0xB197U) == 1U);
    channel.submit_gpfifo(gpfifo_guest, 1U);
    assert(channel.submissions() == 1U);
    assert(channel.pusher().methods_processed() == 14U);
    assert(channel.engine().clear_calls() == 1U);
    assert(channel.engine().draw_calls() == 1U);
    assert(channel.engine().triangles() == 1U);
    assert(memory.read_u32(0x100000U + 32U * 64U * 4U + 32U * 4U) != 0U);
    assert(as.translate(rt, 4U) == 0x100000U);

    std::vector<std::uint32_t> modes;
    modes.push_back((3U << 29U) | (2U << 16U) | 0x0702U);
    modes.push_back(16U);
    modes.push_back(28U);
    modes.push_back((5U << 29U) | (2U << 16U) | 0x0703U);
    modes.push_back(0x111U);
    modes.push_back(0x222U);
    modes.push_back((4U << 29U) | (0x123U << 16U) | 0x0705U);
    const std::uint64_t modes_guest = 0x121000U;
    for (std::size_t i = 0; i < modes.size(); ++i)
        memory.write_u32(modes_guest + i * 4U, modes[i]);
    const auto modes_va = as.allocate(modes.size() * 4U, 0x1000U);
    as.map_buffer(4U, modes_va, modes_guest, modes.size() * 4U);
    memory.write_u64(gpfifo_guest + 8U,
        modes_va | (static_cast<std::uint64_t>(modes.size()) << 42U));
    channel.submit_gpfifo(gpfifo_guest, 1U);
    const auto& regs = channel.engine().registers();
    assert(regs[0x0702U] == 28U);
    assert(regs[0x0703U] == 0x111U);
    assert(regs[0x0704U] == 0x222U);
    assert(regs[0x0705U] == 0x123U);
    assert(channel.pusher().methods_processed() == 19U);
    assert(channel.submissions() == 2U);
}
