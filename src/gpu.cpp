#include "horizon/gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace horizon {
namespace {
constexpr std::uint32_t kMaxwell3D = 0xB197U;
constexpr std::uint32_t kFermi2D = 0x902DU;
constexpr std::uint32_t kKeplerInlineToMemory = 0xA140U;
constexpr std::uint32_t kMethodSetObject = 0x0000U;
constexpr std::uint32_t kMethodClearBuffers = 0x0674U;
constexpr std::uint32_t kMethodDrawVertexArray = 0x035EU;
constexpr std::uint32_t kMethodVertexBegin = 0x0585U;
constexpr std::uint32_t kMethodSemaphoreA = 0x06C0U;
constexpr std::uint32_t kMethodSemaphoreC = 0x06C2U;
std::uint8_t channel(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}
}

std::uint64_t GpuAddressSpace::allocate(std::uint64_t size, std::uint64_t alignment) {
    if (size == 0U || alignment == 0U || (alignment & (alignment - 1U)) != 0U)
        throw std::invalid_argument("invalid GPU VA allocation");
    if (next_va_ > kGpuVaLimit - alignment) throw std::overflow_error("GPU VA space exhausted");
    const std::uint64_t base = (next_va_ + alignment - 1U) & ~(alignment - 1U);
    if (size > kGpuVaLimit - base) throw std::overflow_error("GPU VA allocation exceeds 40-bit range");
    next_va_ = base + size;
    allocations_.emplace(base, size);
    return base;
}

void GpuAddressSpace::map_buffer(std::uint32_t handle, std::uint64_t gpu_va,
                                 std::uint64_t guest_va, std::uint64_t size) {
    if (handle == 0U || size == 0U || gpu_va >= kGpuVaLimit ||
        size > kGpuVaLimit - gpu_va || guest_va > std::numeric_limits<std::uint64_t>::max() - size)
        throw std::invalid_argument("invalid GPU buffer mapping");
    for (const auto& m : mappings_)
        if (gpu_va < m.gpu_va + m.size && m.gpu_va < gpu_va + size)
            throw std::runtime_error("GPU VA mapping overlaps an existing mapping");
    mappings_.push_back({gpu_va, guest_va, size, handle});
    std::sort(mappings_.begin(), mappings_.end(),
              [](const auto& a, const auto& b) { return a.gpu_va < b.gpu_va; });
}

void GpuAddressSpace::unmap(std::uint64_t gpu_va) {
    const auto it = std::find_if(mappings_.begin(), mappings_.end(),
        [gpu_va](const auto& m) { return m.gpu_va == gpu_va; });
    if (it == mappings_.end()) throw std::runtime_error("GPU mapping not found");
    mappings_.erase(it);
}

void GpuAddressSpace::free_space(std::uint64_t gpu_va, std::uint64_t size) {
    const auto it = allocations_.find(gpu_va);
    if (it == allocations_.end() || it->second != size)
        throw std::runtime_error("GPU VA allocation not found");
    if (std::any_of(mappings_.begin(), mappings_.end(), [gpu_va, size](const auto& m) {
        return gpu_va < m.gpu_va + m.size && m.gpu_va < gpu_va + size;
    })) throw std::runtime_error("cannot free a mapped GPU VA range");
    allocations_.erase(it);
}

std::uint64_t GpuAddressSpace::translate(std::uint64_t gpu_va, std::uint64_t size) const {
    for (const auto& m : mappings_) {
        if (gpu_va < m.gpu_va) continue;
        const std::uint64_t offset = gpu_va - m.gpu_va;
        if (offset <= m.size && size <= m.size - offset) return m.guest_va + offset;
    }
    throw std::runtime_error("GPU virtual address is not mapped");
}
const std::vector<GpuMapping>& GpuAddressSpace::mappings() const noexcept { return mappings_; }

Maxwell3DEngine::Maxwell3DEngine(AddressSpace& memory, GpuAddressSpace& as)
    : memory_(memory), address_space_(as) {}

void Maxwell3DEngine::write_method(std::uint32_t subchannel, std::uint32_t method,
                                   std::uint32_t argument) {
    if (subchannel >= objects_.size() || method >= regs_.size()) return;
    regs_[method] = argument;
    if (method == kMethodSetObject) { objects_[subchannel] = argument; return; }
    if (objects_[subchannel] != kMaxwell3D) return;
    if (method >= 0x0360U && method <= 0x0363U) {
        clear_color_[method - 0x0360U] = std::bit_cast<float>(argument);
        return;
    }
    if (method == kMethodClearBuffers && (argument & 0x4U) != 0U) {
        clear_color();
        return;
    }
    if (method == kMethodDrawVertexArray) {
        const std::uint32_t first = argument & 0xFFFFU;
        const std::uint32_t count = (argument >> 16U) & 0xFFFFU;
        draw_vertex_array(first, count);
        return;
    }
    if (method == kMethodSemaphoreC) { write_semaphore(); return; }
    if (method == kMethodVertexBegin) regs_[method] = argument;
    if (method >= 0x0200U && method <= 0x0205U) {
        const std::uint64_t hi = static_cast<std::uint64_t>(regs_[0x0200U]) << 32U;
        render_target_va_ = hi | regs_[0x0201U];
        width_ = regs_[0x0202U]; height_ = regs_[0x0203U]; format_ = regs_[0x0204U];
    }
}

std::uint32_t Maxwell3DEngine::pack_color(float r,float g,float b,float a) const noexcept {
    return static_cast<std::uint32_t>(channel(r)) |
        (static_cast<std::uint32_t>(channel(g)) << 8U) |
        (static_cast<std::uint32_t>(channel(b)) << 16U) |
        (static_cast<std::uint32_t>(channel(a)) << 24U);
}

void Maxwell3DEngine::clear_color() {
    if (render_target_va_ == 0U || width_ == 0U || height_ == 0U ||
        static_cast<std::uint64_t>(width_) * height_ > 16U * 1024U * 1024U)
        return;
    const std::uint64_t bytes = static_cast<std::uint64_t>(width_) * height_ * 4U;
    const std::uint64_t guest = address_space_.translate(render_target_va_, bytes);
    const std::uint32_t color = pack_color(clear_color_[0],clear_color_[1],clear_color_[2],clear_color_[3]);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    for (std::size_t i=0;i<pixels.size();i+=4U)
        for (unsigned c=0;c<4U;++c) pixels[i+c]=static_cast<std::uint8_t>(color>>(c*8U));
    memory_.write(guest,pixels);
    ++clear_calls_;
}

void Maxwell3DEngine::draw_vertex_array(std::uint32_t first, std::uint32_t count) {
    if (count < 3U || count > 4096U || width_ == 0U || height_ == 0U ||
        render_target_va_ == 0U) return;
    const std::uint64_t stream_va =
        (static_cast<std::uint64_t>(regs_[0x0700U]) << 32U) | regs_[0x0701U];
    const std::uint32_t stride = regs_[0x0702U] == 0U ? 28U : regs_[0x0702U];
    const std::uint64_t start = stream_va + static_cast<std::uint64_t>(first) * stride;
    const std::uint64_t guest = address_space_.translate(start,
        static_cast<std::uint64_t>(count) * stride);
    std::vector<GpuVertex> vertices;
    vertices.reserve(count);
    for (std::uint32_t i=0;i<count;++i) {
        const auto bytes=memory_.view(guest+static_cast<std::uint64_t>(i)*stride,28U);
        GpuVertex v{};
        std::memcpy(&v,bytes.data(),28U);
        vertices.push_back(v);
    }
    for (std::size_t i=0;i+2U<vertices.size();i+=3U)
        rasterize_triangle(vertices[i],vertices[i+1U],vertices[i+2U]);
    ++draw_calls_;
}

void Maxwell3DEngine::rasterize_triangle(const GpuVertex& a,const GpuVertex& b,const GpuVertex& c) {
    if (width_==0U || height_==0U) return;
    const auto screen=[this](const GpuVertex& v) {
        return std::array<float,2>{(v.x*0.5F+0.5F)*static_cast<float>(width_-1U),
            (1.0F-(v.y*0.5F+0.5F))*static_cast<float>(height_-1U)};
    };
    const auto p0=screen(a),p1=screen(b),p2=screen(c);
    const float area=(p1[0]-p0[0])*(p2[1]-p0[1])-(p1[1]-p0[1])*(p2[0]-p0[0]);
    if (std::abs(area)<1.0e-6F) return;
    const int minx=std::max(0,static_cast<int>(std::floor(std::min({p0[0],p1[0],p2[0]}))));
    const int maxx=std::min(static_cast<int>(width_)-1,static_cast<int>(std::ceil(std::max({p0[0],p1[0],p2[0]}))));
    const int miny=std::max(0,static_cast<int>(std::floor(std::min({p0[1],p1[1],p2[1]}))));
    const int maxy=std::min(static_cast<int>(height_)-1,static_cast<int>(std::ceil(std::max({p0[1],p1[1],p2[1]}))));
    const std::uint64_t guest=address_space_.translate(render_target_va_,
        static_cast<std::uint64_t>(width_)*height_*4U);
    for(int y=miny;y<=maxy;++y) for(int x=minx;x<=maxx;++x) {
        const float px=static_cast<float>(x)+0.5F,py=static_cast<float>(y)+0.5F;
        const float w0=((p1[0]-px)*(p2[1]-py)-(p1[1]-py)*(p2[0]-px))/area;
        const float w1=((p2[0]-px)*(p0[1]-py)-(p2[1]-py)*(p0[0]-px))/area;
        const float w2=1.0F-w0-w1;
        if(w0<0.0F||w1<0.0F||w2<0.0F) continue;
        const auto mix=[&](float GpuVertex::*field) {
            return w0*(a.*field)+w1*(b.*field)+w2*(c.*field);
        };
        const std::uint32_t color=pack_color(mix(&GpuVertex::r),mix(&GpuVertex::g),
            mix(&GpuVertex::b),mix(&GpuVertex::a));
        const std::uint64_t pixel=guest+(static_cast<std::uint64_t>(y)*width_+
            static_cast<std::uint32_t>(x))*4U;
        memory_.write_u32(pixel,color);
    }
    ++triangles_;
}

void Maxwell3DEngine::write_semaphore() {
    const std::uint64_t va=(static_cast<std::uint64_t>(regs_[kMethodSemaphoreA])<<32U)|
        regs_[kMethodSemaphoreA+1U];
    const std::uint64_t guest=address_space_.translate(va,4U);
    memory_.write_u32(guest,regs_[kMethodSemaphoreC]);
}
const std::array<std::uint32_t,0x1000>& Maxwell3DEngine::registers() const noexcept{return regs_;}
std::uint64_t Maxwell3DEngine::draw_calls() const noexcept{return draw_calls_;}
std::uint64_t Maxwell3DEngine::triangles() const noexcept{return triangles_;}
std::uint64_t Maxwell3DEngine::clear_calls() const noexcept{return clear_calls_;}

DmaPusher::DmaPusher(Maxwell3DEngine& engine):engine_(engine){}
void DmaPusher::push(std::uint64_t address,std::uint32_t count) {
    if(count>1U<<20U) throw std::runtime_error("pushbuffer word count exceeds limit");
    const auto bytes=std::span<const std::uint8_t>();
    (void)bytes;
    throw std::logic_error("DmaPusher requires an AddressSpace-bound channel");
}
std::uint64_t DmaPusher::words_processed() const noexcept{return words_processed_;}
std::uint64_t DmaPusher::methods_processed() const noexcept{return methods_processed_;}

GpuChannel::GpuChannel(AddressSpace& memory,GpuAddressSpace& as)
    :memory_(memory),address_space_(as),engine_(memory,as),pusher_(engine_){}
void GpuChannel::set_nvmap_fd(std::int32_t fd) noexcept{nvmap_fd_=fd;}
void GpuChannel::set_timeout(std::uint32_t timeout) noexcept{timeout_=timeout;}
void GpuChannel::set_priority(std::uint32_t priority) noexcept{priority_=priority;}
std::uint32_t GpuChannel::allocate_object_context(std::uint32_t class_id) {
    if(class_id!=kMaxwell3D&&class_id!=kFermi2D&&class_id!=kKeplerInlineToMemory)
        throw std::invalid_argument("unsupported GPU engine class");
    const std::uint32_t id=next_object_id_++;
    object_classes_.emplace(id,class_id);
    return id;
}
void GpuChannel::allocate_gpfifo(std::uint32_t entries){if(entries==0U||entries>65536U)throw std::invalid_argument("invalid GPFIFO size");gpfifo_capacity_=entries;}
void GpuChannel::submit_gpfifo(std::uint64_t address,std::uint32_t entries) {
    if(entries==0U||entries>gpfifo_capacity_||entries>65536U)throw std::invalid_argument("invalid GPFIFO submission size");
    for(std::uint32_t i=0;i<entries;++i) {
        const std::uint64_t entry=memory_.read_u64(address+static_cast<std::uint64_t>(i)*8U);
        const std::uint64_t push_va=(entry&0xFFFFFFFFFFULL)&~0x3ULL;
        const std::uint32_t words=static_cast<std::uint32_t>((entry>>42U)&0x1FFFFFULL);
        const std::uint64_t guest=address_space_.translate(push_va,static_cast<std::uint64_t>(words)*4U);
        pusher_.push(guest,words);
    }
    ++submissions_;
}
Maxwell3DEngine& GpuChannel::engine() noexcept{return engine_;}
const DmaPusher& GpuChannel::pusher() const noexcept{return pusher_;}
std::uint64_t GpuChannel::submissions() const noexcept{return submissions_;}

} // namespace horizon
