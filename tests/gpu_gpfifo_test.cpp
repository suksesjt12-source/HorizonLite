#include "horizon/gpu.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>
int main() {
    horizon::AddressSpace memory;
    memory.map_zeroed(0x100000U,0x20000U,horizon::Permission::Read|horizon::Permission::Write);
    horizon::GpuAddressSpace as;
    const auto rt=as.allocate(64U*64U*4U,0x1000U);
    as.map_buffer(1U,rt,0x100000U,64U*64U*4U);
    const auto vb=as.allocate(3U*28U,0x1000U);
    as.map_buffer(2U,vb,0x110000U,3U*28U);
    const std::array<float,21> v{-0.8F,-0.8F,0,1,0,0,1, 0.8F,-0.8F,0,0,1,0,1, 0,0.8F,0,0,0,1,1};
    const auto* raw=reinterpret_cast<const std::uint8_t*>(v.data());
    memory.write(0x110000U,std::vector<std::uint8_t>(raw,raw+sizeof(v)));
    horizon::Maxwell3DEngine engine(memory,as);
    engine.write_method(0,0,0xB197U);
    engine.write_method(0,0x0200U,static_cast<std::uint32_t>(rt>>32U));
    engine.write_method(0,0x0201U,static_cast<std::uint32_t>(rt));
    engine.write_method(0,0x0202U,64U);
    engine.write_method(0,0x0203U,64U);
    engine.write_method(0,0x0360U,0U);
    engine.write_method(0,0x0361U,0U);
    engine.write_method(0,0x0362U,0U);
    engine.write_method(0,0x0363U,0x3F800000U);
    engine.write_method(0,0x0674U,4U);
    engine.write_method(0,0x0700U,static_cast<std::uint32_t>(vb>>32U));
    engine.write_method(0,0x0701U,static_cast<std::uint32_t>(vb));
    engine.write_method(0,0x0702U,28U);
    engine.write_method(0,0x035EU,3U<<16U);
    assert(engine.clear_calls()==1U);
    assert(engine.draw_calls()==1U);
    assert(engine.triangles()==1U);
    assert(memory.read_u32(0x100000U+32U*64U*4U+32U*4U)!=0U);
    assert(as.translate(rt,4U)==0x100000U);
    horizon::GpuChannel channel(memory,as);
    channel.allocate_gpfifo(8U);
    assert(channel.allocate_object_context(0xB197U)==1U);
}
