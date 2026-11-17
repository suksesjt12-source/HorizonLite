#pragma once

#include "horizon/audio.hpp"
#include "horizon/cpu.hpp"
#include "horizon/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace horizon {

using Handle = std::uint32_t;

enum class KernelObjectKind : std::uint8_t {
    Port,
    ClientSession,
    SharedMemory,
    Storage,
    Event,
    AudioOut,
    BinderDriver,
    DisplayLayer
};

class KObject {
public:
    explicit KObject(KernelObjectKind kind) noexcept : kind_(kind) {}
    virtual ~KObject() = default;
    [[nodiscard]] KernelObjectKind kind() const noexcept { return kind_; }
private:
    KernelObjectKind kind_;
};

class KPort final : public KObject {
public:
    explicit KPort(std::string name);
    [[nodiscard]] const std::string& name() const noexcept;
private:
    std::string name_;
};

class KClientSession final : public KObject {
public:
    explicit KClientSession(std::string service_name);
    [[nodiscard]] const std::string& service_name() const noexcept;
private:
    std::string service_name_;
};

class KSharedMemory final : public KObject {
public:
    explicit KSharedMemory(std::size_t size);
    std::vector<std::uint8_t> bytes;
};

class KStorage final : public KObject {
public:
    explicit KStorage(std::shared_ptr<const std::vector<std::uint8_t>> romfs);
    [[nodiscard]] const std::vector<std::uint8_t>& romfs() const noexcept;
private:
    std::shared_ptr<const std::vector<std::uint8_t>> romfs_;
};

class KEvent final : public KObject {
public:
    KEvent() noexcept;
    bool signaled{};
};

struct QueuedAudioBuffer {
    std::uint64_t header_address{};
    std::uint64_t end_byte{};
};

class KAudioOut final : public KObject {
public:
    explicit KAudioOut(SdlAudioOutput& device);
    SdlAudioOutput& device;
    std::shared_ptr<KEvent> buffer_event;
    std::deque<QueuedAudioBuffer> active_buffers;
    std::deque<std::uint64_t> released_buffers;
    std::uint64_t submitted_bytes{};
    bool started{};
};

struct NvMapBuffer {
    std::uint32_t handle{};
    std::uint32_t id{};
    std::uint64_t size{};
    std::uint64_t guest_address{};
    std::uint64_t alignment{0x1000U};
    std::uint32_t heap_mask{};
    std::uint32_t flags{};
    bool allocated{};
};

struct DisplayBufferSlot {
    std::uint32_t nvmap_handle{};
    std::uint64_t offset{};
    bool configured{};
};

class KDisplayLayer final : public KObject {
public:
    KDisplayLayer();

    std::array<DisplayBufferSlot, 2> slots{};
    std::vector<std::uint8_t> presented_rgba;
    std::shared_ptr<KEvent> vsync_event;
    std::shared_ptr<KEvent> release_event;
    std::uint32_t next_dequeue_slot{};
    std::uint64_t frame_number{};
};

class KHOSBinderDriver final : public KObject {
public:
    explicit KHOSBinderDriver(std::shared_ptr<KDisplayLayer> layer);
    std::shared_ptr<KDisplayLayer> layer;
};

class HandleTable {
public:
    [[nodiscard]] Handle insert(std::shared_ptr<KObject> object);
    [[nodiscard]] std::shared_ptr<KObject> get(Handle handle) const noexcept;
    [[nodiscard]] bool close(Handle handle) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
private:
    Handle next_handle_{0x100U};
    std::unordered_map<Handle, std::shared_ptr<KObject>> objects_;
};

struct IpcBufferDescriptor {
    std::uint64_t address{};
    std::uint64_t size{};
};

struct CmifRequest {
    std::uint32_t command_id{};
    std::vector<std::uint8_t> data;
    std::vector<IpcBufferDescriptor> pointer_buffers;
    std::vector<IpcBufferDescriptor> send_buffers;
    std::vector<IpcBufferDescriptor> receive_buffers;
    std::vector<IpcBufferDescriptor> exchange_buffers;
    std::vector<IpcBufferDescriptor> auto_send_buffers;
    std::vector<IpcBufferDescriptor> auto_receive_buffers;
};

struct CmifResponse {
    std::uint32_t result{};
    std::uint32_t command_id{};
    std::optional<Handle> handle;
    bool move_handle{};
    std::vector<std::uint8_t> data;
};

class CmifCodec {
public:
    [[nodiscard]] static CmifRequest parse_request(
        std::span<const std::uint8_t> command_buffer);
    [[nodiscard]] static std::vector<std::uint8_t> build_response(
        const CmifResponse& response, std::size_t capacity = 0x100U);
};

enum HidButton : std::uint64_t {
    HidButtonA = 1ULL << 0U,
    HidButtonB = 1ULL << 1U,
    HidButtonX = 1ULL << 2U,
    HidButtonY = 1ULL << 3U,
    HidButtonPlus = 1ULL << 10U,
    HidDpadLeft = 1ULL << 12U,
    HidDpadUp = 1ULL << 13U,
    HidDpadRight = 1ULL << 14U,
    HidDpadDown = 1ULL << 15U
};

class HorizonKernel {
public:
    HorizonKernel(AddressSpace& memory,
                  std::vector<std::uint8_t> romfs,
                  SdlAudioOutput& audio);

    void svc_set_heap_size(CpuState& state);
    void svc_exit_process(CpuState& state) noexcept;
    void svc_break(CpuState& state);
    void svc_output_debug_string(CpuState& state);
    void svc_connect_to_named_port(CpuState& state);
    void svc_send_sync_request(CpuState& state);
    void svc_map_shared_memory(CpuState& state);
    void svc_unmap_shared_memory(CpuState& state);
    void svc_close_handle(CpuState& state);
    void svc_reset_signal(CpuState& state);
    void svc_wait_synchronization(CpuState& state);
    void update_hid_input(std::uint64_t buttons);
    void update_audio_events();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const HandleTable& handles() const noexcept;
    [[nodiscard]] std::size_t romfs_size() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> display_pixels() const noexcept;
    [[nodiscard]] std::uint64_t display_frame_number() const noexcept;
    void signal_vsync() noexcept;

private:
    struct SharedMapping {
        std::shared_ptr<KSharedMemory> object;
        std::uint64_t guest_address{};
        std::size_t size{};
    };

    [[nodiscard]] CmifResponse dispatch_cmif(
        const KClientSession& session, const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_storage(
        const KStorage& storage, const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_audio_manager(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_nvdrv(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_display_root(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_display_service(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_hos_binder(
        const std::shared_ptr<KHOSBinderDriver>& binder,
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_audio_out(
        const std::shared_ptr<KAudioOut>& audio_out,
        const CmifRequest& request);
    [[nodiscard]] std::string read_guest_string(
        std::uint64_t address, std::size_t maximum) const;
    [[nodiscard]] std::string read_service_name(
        const CmifRequest& request) const;
    [[nodiscard]] const IpcBufferDescriptor* find_send_buffer(
        const CmifRequest& request, bool allow_pointer = true) const noexcept;
    [[nodiscard]] const IpcBufferDescriptor* find_receive_buffer(
        const CmifRequest& request) const noexcept;
    void update_audio_out(const std::shared_ptr<KAudioOut>& audio_out);
    void release_all_audio_buffers(const std::shared_ptr<KAudioOut>& audio_out);

    static constexpr std::uint64_t kHeapBase = 0x50000000ULL;
    static constexpr std::uint64_t kMaximumHeapSize = 0x10000000ULL;
    static constexpr std::uint64_t kNpadAreaOffset = 0x9A00ULL;
    static constexpr std::uint64_t kNpadFullEntryOffset = 0x9A20ULL;
    static constexpr std::uint64_t kNpadHandheldEntryOffset = 0x9A50ULL;
    static constexpr std::size_t kHidSharedMemorySize = 0x40000U;
    static constexpr std::size_t kAudioOutBufferHeaderSize = 0x28U;

    AddressSpace& memory_;
    HandleTable handles_;
    std::unordered_map<std::string, std::shared_ptr<KPort>> ports_;
    std::shared_ptr<KSharedMemory> hid_shared_memory_;
    std::shared_ptr<const std::vector<std::uint8_t>> romfs_;
    SdlAudioOutput& audio_device_;
    std::vector<std::weak_ptr<KAudioOut>> audio_sessions_;
    std::unordered_map<std::int32_t, std::string> nv_devices_;
    std::unordered_map<std::uint32_t, NvMapBuffer> nvmap_buffers_;
    std::shared_ptr<KDisplayLayer> display_layer_;
    std::int32_t next_nv_fd_{3};
    std::uint32_t next_nvmap_handle_{1U};
    std::uint32_t next_nvmap_id_{1U};
    std::unordered_map<Handle, SharedMapping> shared_mappings_;
    std::uint64_t sampling_number_{};
    std::uint64_t heap_size_{};
    bool running_{true};
};

class SvcHandler {
public:
    explicit SvcHandler(HorizonKernel& kernel) noexcept;
    void dispatch(std::uint16_t immediate, CpuState& state);
    [[nodiscard]] bool running() const noexcept;
private:
    HorizonKernel& kernel_;
};

} // namespace horizon
