#pragma once

#include "horizon/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace horizon {\n\nclass OpenGLRenderer;

struct GpuMapping {
    std::uint64_t gpu_va{};
    std::uint64_t guest_va{};
    std::uint64_t size{};
    std::uint32_t nvmap_handle{};
};

class GpuAddressSpace {
public:
    static constexpr std::uint64_t kGpuVaLimit = 1ULL << 40U;
    [[nodiscard]] std::uint64_t allocate(std::uint64_t size, std::uint64_t alignment);
    void map_buffer(std::uint32_t handle, std::uint64_t gpu_va,
                    std::uint64_t guest_va, std::uint64_t size);
    void unmap(std::uint64_t gpu_va);
    void free_space(std::uint64_t gpu_va, std::uint64_t size);
    [[nodiscard]] std::uint64_t translate(std::uint64_t gpu_va,
                                          std::uint64_t size = 1U) const;
    [[nodiscard]] const std::vector<GpuMapping>& mappings() const noexcept;
private:
    std::vector<GpuMapping> mappings_;
    std::map<std::uint64_t, std::uint64_t> allocations_;
    std::uint64_t next_va_{0x100000ULL};
};

struct GpuVertex {
    float x{}, y{}, z{};
    float r{}, g{}, b{}, a{1.0F};
};

class Maxwell3DEngine {
public:
    explicit Maxwell3DEngine(AddressSpace& memory, GpuAddressSpace& address_space);
    void write_method(std::uint32_t subchannel, std::uint32_t method,
                      std::uint32_t argument);
    [[nodiscard]] const std::array<std::uint32_t, 0x1000>& registers() const noexcept;
    [[nodiscard]] std::uint64_t draw_calls() const noexcept;
    [[nodiscard]] std::uint64_t triangles() const noexcept;
    [[nodiscard]] std::uint64_t clear_calls() const noexcept;
private:
    void clear_color();
    void draw_vertex_array(std::uint32_t first, std::uint32_t count);\n    void draw_index_array(std::uint32_t first, std::uint32_t count);\n    void update_renderer_state();\n    [[nodiscard]] std::uint64_t upload_bound_texture();
    void rasterize_triangle(const GpuVertex& a, const GpuVertex& b,
                            const GpuVertex& c);
    void write_semaphore();
    [[nodiscard]] std::uint32_t pack_color(float r, float g, float b, float a) const noexcept;

    AddressSpace& memory_;
    GpuAddressSpace& address_space_;
    std::array<std::uint32_t, 0x1000> regs_{};
    std::array<std::uint32_t, 8> objects_{};
    std::uint64_t render_target_va_{};
    std::uint32_t width_{}, height_{}, format_{};
    std::array<float, 4> clear_color_{0.0F, 0.0F, 0.0F, 1.0F};
    std::uint64_t draw_calls_{}, triangles_{}, clear_calls_{};
};

class DmaPusher {
public:
    DmaPusher(AddressSpace& memory, Maxwell3DEngine& engine);
    void push(std::uint64_t guest_address, std::uint32_t word_count);
    [[nodiscard]] std::uint64_t words_processed() const noexcept;
    [[nodiscard]] std::uint64_t methods_processed() const noexcept;
private:
    AddressSpace& memory_;
    Maxwell3DEngine& engine_;
    std::uint64_t words_processed_{}, methods_processed_{};
};

class GpuChannel {
public:
    GpuChannel(AddressSpace& memory, GpuAddressSpace& address_space);
    void set_nvmap_fd(std::int32_t fd) noexcept;
    void set_timeout(std::uint32_t timeout) noexcept;
    void set_priority(std::uint32_t priority) noexcept;\n    void set_renderer(OpenGLRenderer* renderer) noexcept;
    [[nodiscard]] std::uint32_t allocate_object_context(std::uint32_t class_id);
    void allocate_gpfifo(std::uint32_t entries);
    void submit_gpfifo(std::uint64_t guest_entries_address, std::uint32_t entries);
    [[nodiscard]] Maxwell3DEngine& engine() noexcept;
    [[nodiscard]] const Maxwell3DEngine& engine() const noexcept;
    [[nodiscard]] const DmaPusher& pusher() const noexcept;
    [[nodiscard]] std::uint64_t submissions() const noexcept;
private:
    AddressSpace& memory_;
    GpuAddressSpace& address_space_;
    Maxwell3DEngine engine_;
    DmaPusher pusher_;
    std::int32_t nvmap_fd_{-1};
    std::uint32_t timeout_{3000U}, priority_{2U}, gpfifo_capacity_{};
    std::uint32_t next_object_id_{1U};
    std::unordered_map<std::uint32_t, std::uint32_t> object_classes_;
    std::uint64_t submissions_{};
};

namespace nvhost_ioctl {
inline constexpr std::uint32_t AllocAs = 0x40044101U;
inline constexpr std::uint32_t BindChannel = 0x40044102U;
inline constexpr std::uint32_t AllocSpace = 0xC0104103U;
inline constexpr std::uint32_t MapBufferEx = 0xC0204104U;
inline constexpr std::uint32_t UnmapBuffer = 0xC0104105U;
inline constexpr std::uint32_t FreeSpace = 0xC0104106U;
inline constexpr std::uint32_t GetCharacteristics = 0xC0084701U;
inline constexpr std::uint32_t ZcullGetCtxSize = 0x80044702U;
inline constexpr std::uint32_t ZcullGetInfo = 0xC0184703U;
inline constexpr std::uint32_t GetTpcMasks = 0xC0084704U;
inline constexpr std::uint32_t SetNvmapFd = 0x40044701U;
inline constexpr std::uint32_t SetTimeout = 0x40044702U;
inline constexpr std::uint32_t SetPriority = 0x40044703U;
inline constexpr std::uint32_t SetErrorNotifier = 0x40104704U;
inline constexpr std::uint32_t ZcullBind = 0x40084705U;
inline constexpr std::uint32_t AllocGpfifoEx = 0xC0104706U;
inline constexpr std::uint32_t AllocObjCtx = 0xC0084707U;
inline constexpr std::uint32_t SubmitGpfifo = 0xC0104708U;
inline constexpr std::uint32_t SubmitGpfifoEx = 0xC0184709U;
}

} // namespace horizon
