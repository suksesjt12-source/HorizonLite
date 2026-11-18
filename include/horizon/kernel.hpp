#pragma once

#include "horizon/audio.hpp"
#include "horizon/cpu.hpp"
#include "horizon/memory.hpp"
#include "horizon/gpu.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace horizon {

using Handle = std::uint32_t;

[[nodiscard]] constexpr std::uint32_t MakeResult(
    std::uint32_t module, std::uint32_t description) noexcept {
    return (module & 0x1FFU) | ((description & 0x1FFFU) << 9U);
}

enum class KernelObjectKind : std::uint8_t {
    Port,
    ClientSession,
    SharedMemory,
    Storage,
    Event,
    AudioOut,
    BinderDriver,
    DisplayLayer,
    Thread,
    Profile,
    FileSystem,
    File,
    GenericStubService
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

class GenericStubService final : public KObject {
public:
    explicit GenericStubService(std::string service_name);
    [[nodiscard]] const std::string& service_name() const noexcept;
private:
    std::string service_name_;
};

class KSharedMemory final : public KObject {
public:
    explicit KSharedMemory(std::size_t size);
    std::vector<std::uint8_t> bytes;
};

enum class ThreadStatus : std::uint8_t {
    Suspended,
    Runnable,
    Sleeping,
    WaitingLock,
    WaitingSynchronization,
    Terminated
};

class KThread final : public KObject {
public:
    explicit KThread(std::size_t thread_index);

    Handle handle{};
    std::size_t thread_index{};
    CpuState context{};
    ThreadStatus status{ThreadStatus::Suspended};
    std::int32_t priority{0x2C};
    std::int32_t processor_id{-1};
    std::uint64_t waiting_mutex{};
    std::vector<Handle> waiting_handles;
    std::chrono::steady_clock::time_point wake_deadline{};
    bool has_deadline{};
};

struct Uid {
    std::uint64_t low{};
    std::uint64_t high{};
    friend bool operator==(const Uid&, const Uid&) = default;
};

class KFile;

class KProfile final : public KObject {
public:
    KProfile();
};

class KFileSystem final : public KObject {
public:
    explicit KFileSystem(std::filesystem::path root_path);
    std::filesystem::path root;
    std::vector<std::weak_ptr<KFile>> open_files;
};

class KFile final : public KObject {
public:
    KFile(std::filesystem::path file_path, std::uint32_t open_mode);
    ~KFile() override;
    std::filesystem::path path;
    std::uint32_t open_mode{};
    std::fstream stream;
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
    void prepare_loader_heap(std::uint64_t capacity);
    void attach_gl_renderer(OpenGLRenderer* renderer) noexcept;
    [[nodiscard]] std::uint64_t loader_heap_base() const noexcept;
    [[nodiscard]] std::uint64_t loader_heap_size() const noexcept;
    void update_main_thread_context(const CpuState& state);
    void request_applet_exit() noexcept;
    [[nodiscard]] bool validate_stage9_services();
    void print_ipc_statistics() const;
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
    void svc_create_thread(CpuState& state);
    void svc_start_thread(CpuState& state);
    void svc_exit_thread(CpuState& state) noexcept;
    void svc_sleep_thread(CpuState& state);
    void svc_arbitrate_lock(CpuState& state);
    void svc_arbitrate_unlock(CpuState& state);
    void initialize_scheduler(const CpuState& initial_context);
    [[nodiscard]] std::size_t run_scheduler(
        Arm64Cpu& cpu, std::size_t max_instructions,
        std::uint64_t stop_pc, std::size_t time_slice = 750U);
    [[nodiscard]] std::size_t scheduled_thread_count() const noexcept;
    [[nodiscard]] Handle current_thread_handle() const noexcept;
    [[nodiscard]] bool yield_requested() const noexcept;
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
    [[nodiscard]] CmifResponse dispatch_account(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_profile(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_system_settings(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_settings(
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_applet_service(
        const std::string& service_name, const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_time_service(
        const std::string& service_name, const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_generic_stub(
        const GenericStubService& service, const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_file_system(
        const std::shared_ptr<KFileSystem>& file_system,
        const CmifRequest& request);
    [[nodiscard]] CmifResponse dispatch_file(
        const std::shared_ptr<KFile>& file, const CmifRequest& request);
    [[nodiscard]] std::filesystem::path resolve_save_path(
        const std::filesystem::path& root, const std::string& guest_path) const;
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
    void request_scheduler_yield() noexcept;
    void reset_all_exclusive_monitors() noexcept;
    void update_thread_waiters();
    [[nodiscard]] std::shared_ptr<KThread> find_thread(Handle handle) const noexcept;
    [[nodiscard]] std::shared_ptr<KThread> next_runnable_thread();
    [[nodiscard]] bool any_runnable_thread() const noexcept;

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
    GpuAddressSpace gpu_address_space_;
    GpuChannel gpu_channel_;
    std::shared_ptr<KDisplayLayer> display_layer_;
    std::shared_ptr<KEvent> applet_event_;
    std::queue<std::uint32_t> applet_messages_;
    std::unordered_map<std::string, std::vector<std::uint32_t>> ipc_accessed_;
    std::chrono::steady_clock::time_point steady_clock_origin_{
        std::chrono::steady_clock::now()};
    std::filesystem::path save_data_root_{"save_data"};
    std::int32_t next_nv_fd_{3};
    std::uint32_t next_nvmap_handle_{1U};
    std::uint32_t next_nvmap_id_{1U};
    std::unordered_map<Handle, SharedMapping> shared_mappings_;
    std::vector<std::shared_ptr<KThread>> threads_;
    std::shared_ptr<KThread> active_thread_;
    std::shared_ptr<KThread> main_thread_;
    std::size_t next_thread_index_{1U};
    std::size_t scheduler_cursor_{};
    bool scheduler_initialized_{};
    bool applet_exit_requested_{};
    bool scheduler_yield_requested_{};
    std::uint64_t sampling_number_{};
    std::uint64_t heap_size_{};
    std::uint64_t heap_capacity_{};
    bool running_{true};
};

class SvcHandler {
public:
    explicit SvcHandler(HorizonKernel& kernel) noexcept;
    void dispatch(std::uint16_t immediate, CpuState& state);
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool yield_requested() const noexcept;
private:
    HorizonKernel& kernel_;
};

} // namespace horizon
