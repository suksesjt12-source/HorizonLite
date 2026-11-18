#include "horizon/gpu.hpp"
#include "horizon/display.hpp"
#include "horizon/gl_renderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <array>

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
GpuVertex read_gpu_vertex(AddressSpace& memory, std::uint64_t address,
                          std::uint32_t stride) {
    const auto bytes = memory.view(address, 28U);
    std::array<float, 7> base{};
    std::memcpy(base.data(), bytes.data(), 28U);
    GpuVertex vertex{base[0], base[1], base[2], base[3],
                     base[4], base[5], base[6], 0.0F, 0.0F};
    if (stride >= 36U) {
        const auto uv_bytes = memory.view(address + 28U, 8U);
        std::array<float, 2> uv{};
        std::memcpy(uv.data(), uv_bytes.data(), 8U);
        vertex.u = uv[0];
        vertex.v = uv[1];
    }
    return vertex;
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

void Maxwell3DEngine::set_renderer(OpenGLRenderer* renderer) noexcept {
    renderer_ = renderer;
    if (renderer_ != nullptr) renderer_->begin_frame();
}

void Maxwell3DEngine::write_method(std::uint32_t subchannel, std::uint32_t method,
                                   std::uint32_t argument) {
    if (subchannel >= objects_.size() || method >= regs_.size()) return;
    regs_[method] = argument;
    if (method == kMethodSetObject) {
        objects_[subchannel] = argument;
        return;
    }
    if (objects_[subchannel] != kMaxwell3D) return;

    if (method >= 0x0360U && method <= 0x0363U) {
        clear_color_[method - 0x0360U] = std::bit_cast<float>(argument);
        return;
    }
    if (method >= 0x0280U && method <= 0x028BU) {
        update_renderer_state();
        return;
    }
    if (method >= 0x0380U && method <= 0x0382U) {
        update_renderer_state();
        return;
    }
    if ((method >= 0x03F8U && method <= 0x03FEU) ||
        method == 0x04B3U || method == 0x048CU || method == 0x04A2U ||
        (method >= 0x0646U && method <= 0x0648U) || method == 0x04D9U) {
        update_renderer_state();
        return;
    }
    if (method >= 0x0200U && method <= 0x0205U) {
        render_target_va_ = (static_cast<std::uint64_t>(regs_[0x0200U]) << 32U) |
                            regs_[0x0201U];
        width_ = regs_[0x0202U];
        height_ = regs_[0x0203U];
        format_ = regs_[0x0204U];
        return;
    }
    if (method == kMethodClearBuffers && (argument & 0x4U) != 0U) {
        clear_color();
        return;
    }
    if (method == kMethodDrawVertexArray) {
        draw_vertex_array(argument & 0xFFFFU, (argument >> 16U) & 0xFFFFU);
        return;
    }
    if (method == 0x035FU || method == 0x0586U) {
        draw_index_array(argument & 0xFFFFU, (argument >> 16U) & 0xFFFFU);
        return;
    }
    if (method == kMethodSemaphoreC) {
        write_semaphore();
        return;
    }
    if (method == kMethodVertexBegin) {
        regs_[method] = argument;
        return;
    }
    if (method == 0x0578U) {
        (void)upload_bound_texture();
        return;
    }
}

void Maxwell3DEngine::update_renderer_state() {
    if (renderer_ == nullptr) return;
    const auto as_float = [this](std::uint32_t index) {
        return std::bit_cast<float>(regs_[index]);
    };
    const float sx = as_float(0x0280U);
    const float sy = as_float(0x0281U);
    const float ox = as_float(0x0283U);
    const float oy = as_float(0x0284U);
    if (std::isfinite(sx) && std::isfinite(sy) &&
        std::isfinite(ox) && std::isfinite(oy) && sx > 0.0F && sy > 0.0F) {
        const int vx = static_cast<int>(std::floor(ox - sx));
        const int vy_top = static_cast<int>(std::floor(oy - sy));
        const int vw = std::max(1, static_cast<int>(std::ceil(sx * 2.0F)));
        const int vh = std::max(1, static_cast<int>(std::ceil(sy * 2.0F)));
        renderer_->set_viewport(vx, std::max(0, 720 - vy_top - vh), vw, vh);
    }

    const std::uint32_t sh = regs_[0x0380U];
    const std::uint32_t sv = regs_[0x0381U];
    const int sx0 = static_cast<int>((sh >> 16U) & 0xFFFFU);
    const int sx1 = static_cast<int>(sh & 0xFFFFU);
    const int sy0 = static_cast<int>((sv >> 16U) & 0xFFFFU);
    const int sy1 = static_cast<int>(sv & 0xFFFFU);
    const bool scissor_enabled = regs_[0x0382U] != 0U;
    renderer_->set_scissor(sx0, std::max(0, 720 - sy1),
        std::max(0, sx1 - sx0), std::max(0, sy1 - sy0), scissor_enabled);

    GLenum depth_function = GL_LESS;
    switch (regs_[0x048CU] & 0x7U) {
    case 0U: depth_function = GL_NEVER; break;
    case 1U: depth_function = GL_LESS; break;
    case 2U: depth_function = GL_EQUAL; break;
    case 3U: depth_function = GL_LEQUAL; break;
    case 4U: depth_function = GL_GREATER; break;
    case 5U: depth_function = GL_NOTEQUAL; break;
    case 6U: depth_function = GL_GEQUAL; break;
    case 7U: depth_function = GL_ALWAYS; break;
    }
    renderer_->set_depth_state(regs_[0x04B3U] != 0U,
                               regs_[0x04A2U] != 0U, depth_function);

    const GLenum front = (regs_[0x0647U] & 1U) != 0U ? GL_CW : GL_CCW;
    GLenum cull = GL_BACK;
    if ((regs_[0x0648U] & 3U) == 1U) cull = GL_FRONT;
    else if ((regs_[0x0648U] & 3U) == 2U) cull = GL_FRONT_AND_BACK;
    renderer_->set_raster_state(regs_[0x0646U] != 0U, front, cull,
                                regs_[0x04D9U] != 0U);
}

std::uint32_t Maxwell3DEngine::pack_color(float r,float g,float b,float a) const noexcept {
    return static_cast<std::uint32_t>(channel(r)) |
        (static_cast<std::uint32_t>(channel(g)) << 8U) |
        (static_cast<std::uint32_t>(channel(b)) << 16U) |
        (static_cast<std::uint32_t>(channel(a)) << 24U);
}

void Maxwell3DEngine::clear_color() {
    if (width_ == 0U || height_ == 0U ||
        static_cast<std::uint64_t>(width_) * height_ > 16U * 1024U * 1024U)
        return;
    if (renderer_ != nullptr) {
        renderer_->clear(clear_color_[0], clear_color_[1],
                         clear_color_[2], clear_color_[3], 1.0);
        ++clear_calls_;
        return;
    }
    if (render_target_va_ == 0U) return;
    const std::uint64_t bytes = static_cast<std::uint64_t>(width_) * height_ * 4U;
    const std::uint64_t guest = address_space_.translate(render_target_va_, bytes);
    const std::uint32_t color = pack_color(clear_color_[0],clear_color_[1],
                                            clear_color_[2],clear_color_[3]);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bytes));
    for (std::size_t i=0;i<pixels.size();i+=4U)
        for (unsigned c=0;c<4U;++c)
            pixels[i+c]=static_cast<std::uint8_t>(color>>(c*8U));
    memory_.write(guest,pixels);
    ++clear_calls_;
}

std::uint64_t Maxwell3DEngine::upload_bound_texture() {
    if (renderer_ == nullptr) return 0U;
    const std::uint64_t tic_pool =
        (static_cast<std::uint64_t>(regs_[0x055DU]) << 32U) | regs_[0x055EU];
    const std::uint32_t tic_index = regs_[0x0578U] & 0xFFFFU;
    const std::uint64_t tic_offset = static_cast<std::uint64_t>(tic_index) * 32U;
    if (tic_pool == 0U || (regs_[0x055FU] != 0U &&
        tic_offset + 32U > regs_[0x055FU])) return 0U;
    const std::uint64_t tic_guest = address_space_.translate(tic_pool + tic_offset, 32U);
    const auto descriptor = memory_.view(tic_guest, 32U);
    const auto word = [&descriptor](std::size_t index) {
        const std::size_t p = index * 4U;
        return static_cast<std::uint32_t>(descriptor[p]) |
            (static_cast<std::uint32_t>(descriptor[p + 1U]) << 8U) |
            (static_cast<std::uint32_t>(descriptor[p + 2U]) << 16U) |
            (static_cast<std::uint32_t>(descriptor[p + 3U]) << 24U);
    };
    const std::uint64_t texture_va =
        (static_cast<std::uint64_t>(word(1U) & 0xFFU) << 32U) | word(0U);
    const std::uint32_t texture_width = word(2U);
    const std::uint32_t texture_height = word(3U);
    const std::uint32_t texture_format = word(4U);
    const bool block_linear = (word(1U) & (1U << 8U)) != 0U;
    const std::uint32_t block_height = 1U << ((word(1U) >> 9U) & 0x7U);
    const std::uint32_t sampler_index = word(7U) & 0xFFFFU;
    if (texture_va == 0U || texture_width == 0U || texture_height == 0U ||
        texture_width > 8192U || texture_height > 8192U) return 0U;

    bool linear_filter = false;
    const std::uint64_t tsc_pool =
        (static_cast<std::uint64_t>(regs_[0x0557U]) << 32U) | regs_[0x0558U];
    const std::uint64_t tsc_offset = static_cast<std::uint64_t>(sampler_index) * 32U;
    if (tsc_pool != 0U && (regs_[0x0559U] == 0U ||
        tsc_offset + 32U <= regs_[0x0559U])) {
        const std::uint64_t tsc_guest = address_space_.translate(tsc_pool + tsc_offset, 32U);
        linear_filter = (memory_.read_u32(tsc_guest) & 1U) != 0U;
    }

    const std::uint32_t bytes_per_pixel = texture_format == 1U ? 2U : 4U;
    const std::uint64_t linear_row_bytes =
        static_cast<std::uint64_t>(texture_width) * bytes_per_pixel;
    const std::uint64_t linear_size = linear_row_bytes * texture_height;
    const std::uint64_t tiling_height = std::min(block_height, 32U);
    const std::uint64_t width_gobs = (linear_row_bytes + 63U) / 64U;
    const std::uint64_t gob_block_rows =
        (static_cast<std::uint64_t>(texture_height) + 8U * tiling_height - 1U) /
        (8U * tiling_height);
    const std::uint64_t tiled_size = width_gobs * tiling_height * gob_block_rows * 512U;
    const std::uint64_t source_size = block_linear ? tiled_size : linear_size;
    if (source_size == 0U || source_size > 256ULL * 1024ULL * 1024ULL) return 0U;
    const std::uint64_t texture_guest =
        address_space_.translate(texture_va, source_size);
    const auto source = memory_.view(texture_guest, static_cast<std::size_t>(source_size));
    std::vector<std::uint8_t> rgba(
        static_cast<std::size_t>(texture_width) * texture_height * 4U);
    if (texture_format == 0U) {
        if (block_linear) {
            rgba = UntileTegraBlockLinear(source, texture_width, texture_height,
                                           std::min(block_height, 32U), true);
        } else {
            std::copy(source.begin(), source.end(), rgba.begin());
        }
    } else if (texture_format == 1U) {
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(texture_width) * 2U;
        const std::uint64_t bh = block_linear ? tiling_height : 1U;
        for (std::uint32_t y = 0U; y < texture_height; ++y) {
            for (std::uint32_t x = 0U; x < texture_width; ++x) {
                std::uint64_t offset = static_cast<std::uint64_t>(y) * row_bytes +
                                       static_cast<std::uint64_t>(x) * 2U;
                if (block_linear) {
                    const std::uint64_t xb = static_cast<std::uint64_t>(x) * 2U;
                    const std::uint64_t gob_y = y / (8U * bh);
                    const std::uint64_t block_y = (y % (gob_height * bh)) / gob_height;
                    const std::uint64_t gob_x = xb / 64U;
                    const std::uint64_t in_x = xb % 64U;
                    const std::uint64_t in_y = y % 8U;
                    const std::uint64_t gob_index =
                        gob_y * width_gobs * bh + gob_x * bh + block_y;
                    const std::uint64_t in_gob =
                        (in_x / 32U) * 256U + (in_y / 2U) * 64U +
                        ((in_x % 32U) / 16U) * 32U + (in_y % 2U) * 16U +
                        (in_x % 16U);
                    offset = gob_index * 512U + in_gob;
                }
                if (offset + 2U > source.size())
                    throw std::runtime_error("RGB565 texture GOB offset exceeds buffer");
                const std::uint16_t pixel = static_cast<std::uint16_t>(source[offset]) |
                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(source[offset + 1U]) << 8U);
                const std::uint8_t r = static_cast<std::uint8_t>(((pixel >> 11U) & 0x1FU) * 255U / 31U);
                const std::uint8_t g = static_cast<std::uint8_t>(((pixel >> 5U) & 0x3FU) * 255U / 63U);
                const std::uint8_t b = static_cast<std::uint8_t>((pixel & 0x1FU) * 255U / 31U);
                const std::size_t target =
                    (static_cast<std::size_t>(y) * texture_width + x) * 4U;
                rgba[target] = r; rgba[target + 1U] = g;
                rgba[target + 2U] = b; rgba[target + 3U] = 255U;
            }
        }
    } else {
        return 0U;
    }
    renderer_->upload_texture(texture_va, texture_width, texture_height,
                              rgba, linear_filter);
    return texture_va;
}

void Maxwell3DEngine::draw_vertex_array(std::uint32_t first, std::uint32_t count) {
    if (count < 3U || count > 4096U || width_ == 0U || height_ == 0U) return;
    const std::uint64_t stream_va =
        (static_cast<std::uint64_t>(regs_[0x0700U]) << 32U) | regs_[0x0701U];
    const std::uint32_t stride = regs_[0x0702U] == 0U ? 28U : regs_[0x0702U];
    if (stride < 28U || stride > 256U) return;
    const std::uint64_t start = stream_va + static_cast<std::uint64_t>(first) * stride;
    const std::uint64_t guest = address_space_.translate(start,
        static_cast<std::uint64_t>(count) * stride);
    std::vector<GpuVertex> vertices;
    std::vector<OpenGLVertex> gl_vertices;
    vertices.reserve(count);
    gl_vertices.reserve(count);
    for (std::uint32_t i=0;i<count;++i) {
        const auto bytes=memory_.view(guest+static_cast<std::uint64_t>(i)*stride,28U);
        GpuVertex v = read_gpu_vertex(memory_,
            guest + static_cast<std::uint64_t>(i) * stride, stride);
        vertices.push_back(v);
        gl_vertices.push_back(OpenGLVertex{v.x,v.y,v.z,v.r,v.g,v.b,v.a,v.u,v.v});
    }
    if (renderer_ != nullptr) {
        update_renderer_state();
        const std::uint64_t texture = upload_bound_texture();
        GLenum primitive = GL_TRIANGLES;
        if ((regs_[0x0585U] & 0x7U) == 1U) primitive = GL_LINES;
        else if ((regs_[0x0585U] & 0x7U) == 2U) primitive = GL_TRIANGLE_STRIP;
        renderer_->draw_arrays(gl_vertices, primitive, texture);
    } else {
        for (std::size_t i=0;i+2U<vertices.size();i+=3U)
            rasterize_triangle(vertices[i],vertices[i+1U],vertices[i+2U]);
    }
    ++draw_calls_;
}

void Maxwell3DEngine::draw_index_array(std::uint32_t first, std::uint32_t count) {
    if (count < 3U || count > 1U << 20U || width_ == 0U || height_ == 0U)
        return;
    const std::uint64_t index_va =
        (static_cast<std::uint64_t>(regs_[0x05F2U]) << 32U) | regs_[0x05F3U];
    const std::uint32_t index_format = regs_[0x05F6U] & 0x3U;
    const std::uint32_t index_size = index_format == 0U ? 1U :
                                     index_format == 1U ? 2U : 4U;
    if (index_format > 2U) return;
    const std::uint64_t index_guest = address_space_.translate(
        index_va + static_cast<std::uint64_t>(first) * index_size,
        static_cast<std::uint64_t>(count) * index_size);
    std::vector<std::uint32_t> indices(count);
    std::uint32_t max_index = 0U;
    for (std::uint32_t i = 0U; i < count; ++i) {
        const std::uint64_t p = index_guest + static_cast<std::uint64_t>(i) * index_size;
        if (index_size == 1U) indices[i] = memory_.read_u8(p);
        else if (index_size == 2U) indices[i] = memory_.read_u16(p);
        else indices[i] = memory_.read_u32(p);
        max_index = std::max(max_index, indices[i]);
    }
    const std::uint64_t vertex_va =
        (static_cast<std::uint64_t>(regs_[0x0700U]) << 32U) | regs_[0x0701U];
    const std::uint32_t stride = regs_[0x0702U] == 0U ? 36U : regs_[0x0702U];
    if (stride < 28U || stride > 256U || max_index > 65535U) return;
    const std::uint64_t vertex_guest = address_space_.translate(
        vertex_va, (static_cast<std::uint64_t>(max_index) + 1U) * stride);
    std::vector<GpuVertex> vertices;
    std::vector<OpenGLVertex> gl_vertices;
    vertices.reserve(static_cast<std::size_t>(max_index) + 1U);
    gl_vertices.reserve(static_cast<std::size_t>(max_index) + 1U);
    for (std::uint32_t i = 0U; i <= max_index; ++i) {
        const std::uint64_t address = vertex_guest + static_cast<std::uint64_t>(i) * stride;
        GpuVertex v = read_gpu_vertex(memory_, address, stride);
        vertices.push_back(v);
        gl_vertices.push_back(OpenGLVertex{v.x,v.y,v.z,v.r,v.g,v.b,v.a,v.u,v.v});
    }
    if (renderer_ != nullptr) {
        update_renderer_state();
        const std::uint64_t texture = upload_bound_texture();
        GLenum primitive = GL_TRIANGLES;
        if ((regs_[0x0585U] & 0x7U) == 1U) primitive = GL_LINES;
        else if ((regs_[0x0585U] & 0x7U) == 2U) primitive = GL_TRIANGLE_STRIP;
        renderer_->draw_elements(gl_vertices, indices, primitive, texture);
    } else {
        for (std::size_t i=0;i+2U<indices.size();i+=3U)
            if (indices[i] < vertices.size() && indices[i+1U] < vertices.size() &&
                indices[i+2U] < vertices.size())
                rasterize_triangle(vertices[indices[i]],vertices[indices[i+1U]],
                                   vertices[indices[i+2U]]);
    }
    ++draw_calls_;
}

void Maxwell3DEngine::rasterize_triangle(const GpuVertex& a,const GpuVertex& b,const GpuVertex& c) {
    if (width_==0U || height_==0U || render_target_va_ == 0U) return;
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

DmaPusher::DmaPusher(AddressSpace& memory, Maxwell3DEngine& engine)
    : memory_(memory), engine_(engine) {}
void DmaPusher::push(std::uint64_t address, std::uint32_t count) {
    if (count == 0U || count > (1U << 20U))
        throw std::runtime_error("pushbuffer word count is outside the supported range");
    const auto stream = memory_.view(address, static_cast<std::size_t>(count) * 4U);
    std::vector<std::uint32_t> words(count);
    for (std::uint32_t i = 0U; i < count; ++i) {
        const std::size_t p = static_cast<std::size_t>(i) * 4U;
        words[i] = static_cast<std::uint32_t>(stream[p]) |
            (static_cast<std::uint32_t>(stream[p + 1U]) << 8U) |
            (static_cast<std::uint32_t>(stream[p + 2U]) << 16U) |
            (static_cast<std::uint32_t>(stream[p + 3U]) << 24U);
    }
    std::size_t cursor = 0U;
    while (cursor < words.size()) {
        const std::uint32_t header = words[cursor++];
        const std::uint32_t mode = (header >> 29U) & 0x7U;
        const std::uint32_t method = header & 0x1FFFU;
        const std::uint32_t subchannel = (header >> 13U) & 0x7U;
        const std::uint32_t argument_count = (header >> 16U) & 0x1FFFU;
        ++words_processed_;
        if (mode == 4U) {
            engine_.write_method(subchannel, method, (header >> 16U) & 0x1FFFU);
            ++methods_processed_;
            continue;
        }
        if (mode != 1U && mode != 3U && mode != 5U)
            throw std::runtime_error("unsupported Tegra submission mode");
        if (argument_count == 0U || argument_count > words.size() - cursor)
            throw std::runtime_error("truncated Tegra pushbuffer method packet");
        for (std::uint32_t i = 0U; i < argument_count; ++i) {
            const std::uint32_t argument = words[cursor++];
            std::uint32_t target = method;
            if (mode == 1U) target = method + i;
            else if (mode == 5U && i != 0U) target = method + 1U;
            engine_.write_method(subchannel, target, argument);
            ++words_processed_;
            ++methods_processed_;
        }
    }
}
std::uint64_t DmaPusher::words_processed() const noexcept{return words_processed_;}
std::uint64_t DmaPusher::methods_processed() const noexcept{return methods_processed_;}

GpuChannel::GpuChannel(AddressSpace& memory,GpuAddressSpace& as)
    :memory_(memory),address_space_(as),engine_(memory,as),pusher_(memory_,engine_){}
void GpuChannel::set_nvmap_fd(std::int32_t fd) noexcept{nvmap_fd_=fd;}
void GpuChannel::set_timeout(std::uint32_t timeout) noexcept{timeout_=timeout;}
void GpuChannel::set_priority(std::uint32_t priority) noexcept{priority_=priority;}
void GpuChannel::set_renderer(OpenGLRenderer* renderer) noexcept{engine_.set_renderer(renderer);}
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
const Maxwell3DEngine& GpuChannel::engine() const noexcept{return engine_;}
const DmaPusher& GpuChannel::pusher() const noexcept{return pusher_;}
std::uint64_t GpuChannel::submissions() const noexcept{return submissions_;}

} // namespace horizon
