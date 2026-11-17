#pragma once

#include "horizon/cpu.hpp"
#include "horizon/memory.hpp"

#include <cstddef>
#include <cstdint>
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
    SharedMemory
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

struct CmifRequest {
    std::uint32_t command_id{};
    std::vector<std::uint8_t> data;
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
    explicit HorizonKernel(AddressSpace& memory);

    void svc_set_heap_size(CpuState& state);
    void svc_exit_process(CpuState& state) noexcept;
    void svc_break(CpuState& state);
    void svc_output_debug_string(CpuState& state);
    void svc_connect_to_named_port(CpuState& state);
    void svc_send_sync_request(CpuState& state);
    void svc_map_shared_memory(CpuState& state);
    void svc_unmap_shared_memory(CpuState& state);
    void svc_close_handle(CpuState& state);
    void update_hid_input(std::uint64_t buttons);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const HandleTable& handles() const noexcept;

private:
    struct SharedMapping {
        std::shared_ptr<KSharedMemory> object;
        std::uint64_t guest_address{};
        std::size_t size{};
    };

    [[nodiscard]] CmifResponse dispatch_cmif(
        const KClientSession& session, const CmifRequest& request);
    [[nodiscard]] std::string read_guest_string(
        std::uint64_t address, std::size_t maximum) const;

    static constexpr std::uint64_t kHeapBase = 0x50000000ULL;
    static constexpr std::uint64_t kMaximumHeapSize = 0x10000000ULL;
    static constexpr std::uint64_t kNpadAreaOffset = 0x9A00ULL;
    static constexpr std::uint64_t kNpadFullEntryOffset = 0x9A20ULL;
    static constexpr std::uint64_t kNpadHandheldEntryOffset = 0x9A50ULL;
    static constexpr std::size_t kHidSharedMemorySize = 0x40000U;

    AddressSpace& memory_;
    HandleTable handles_;
    std::unordered_map<std::string, std::shared_ptr<KPort>> ports_;
    std::shared_ptr<KSharedMemory> hid_shared_memory_;
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
