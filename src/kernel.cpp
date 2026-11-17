#include "horizon/kernel.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace horizon {
namespace {

constexpr std::uint32_t kSuccess = 0;
constexpr std::uint32_t kInvalidHandle = 0xF001U;
constexpr std::uint32_t kInvalidArgument = 0xF601U;
constexpr std::uint32_t kInvalidCommand = 0xE401U;
constexpr std::uint32_t kSfciMagic = 0x49434653U;
constexpr std::uint32_t kSfcoMagic = 0x4F434653U;

std::uint32_t read_le32(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U)
        throw std::runtime_error("truncated HIPC/CMIF field");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_le32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                std::uint32_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 4U)
        throw std::runtime_error("CMIF response buffer is too small");
    for (unsigned i = 0; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void write_le64(std::vector<std::uint8_t>& bytes, std::size_t offset,
                std::uint64_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 8U)
        throw std::runtime_error("HID shared-memory field is out of range");
    for (unsigned i = 0; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::string trim_service_name(const std::vector<std::uint8_t>& bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), 0U);
    return std::string(bytes.begin(), end);
}

} // namespace

KPort::KPort(std::string name)
    : KObject(KernelObjectKind::Port), name_(std::move(name)) {}
const std::string& KPort::name() const noexcept { return name_; }

KClientSession::KClientSession(std::string service_name)
    : KObject(KernelObjectKind::ClientSession),
      service_name_(std::move(service_name)) {}
const std::string& KClientSession::service_name() const noexcept {
    return service_name_;
}

KSharedMemory::KSharedMemory(std::size_t size)
    : KObject(KernelObjectKind::SharedMemory), bytes(size, 0U) {}

Handle HandleTable::insert(std::shared_ptr<KObject> object) {
    if (!object) throw std::invalid_argument("cannot insert a null kernel object");
    while (next_handle_ == 0U || objects_.contains(next_handle_)) {
        ++next_handle_;
        if (next_handle_ == 0U) throw std::overflow_error("kernel handle table exhausted");
    }
    const Handle result = next_handle_++;
    objects_.emplace(result, std::move(object));
    return result;
}

std::shared_ptr<KObject> HandleTable::get(Handle handle) const noexcept {
    const auto found = objects_.find(handle);
    return found == objects_.end() ? nullptr : found->second;
}

bool HandleTable::close(Handle handle) noexcept {
    return objects_.erase(handle) != 0U;
}

std::size_t HandleTable::size() const noexcept { return objects_.size(); }

CmifRequest CmifCodec::parse_request(
    std::span<const std::uint8_t> command_buffer) {
    if (command_buffer.size() < 0x20U)
        throw std::runtime_error("IPC command buffer is shorter than a CMIF header");

    const std::uint32_t handle_descriptor = read_le32(command_buffer, 8U);
    const std::size_t copy_count = handle_descriptor & 0xFU;
    const std::size_t move_count = (handle_descriptor >> 4U) & 0xFU;
    const std::size_t cmif_offset = align_up(12U + 4U * (copy_count + move_count), 0x10U);
    if (cmif_offset > command_buffer.size() ||
        command_buffer.size() - cmif_offset < 0x10U)
        throw std::runtime_error("IPC descriptor area exceeds command buffer");
    if (read_le32(command_buffer, cmif_offset) != kSfciMagic)
        throw std::runtime_error("IPC request does not contain SFCI magic");

    CmifRequest request;
    request.command_id = read_le32(command_buffer, cmif_offset + 8U);
    const std::size_t data_offset = cmif_offset + 0x10U;
    request.data.assign(command_buffer.begin() + static_cast<std::ptrdiff_t>(data_offset),
                        command_buffer.end());
    return request;
}

std::vector<std::uint8_t> CmifCodec::build_response(
    const CmifResponse& response, std::size_t capacity) {
    const std::size_t descriptor_count = response.handle.has_value() ? 1U : 0U;
    const std::size_t cmif_offset = align_up(12U + descriptor_count * 4U, 0x10U);
    const std::size_t required = cmif_offset + 0x10U + response.data.size();
    if (capacity < required) capacity = required;
    std::vector<std::uint8_t> result(capacity, 0U);

    write_le32(result, 0U, 0U);
    write_le32(result, 4U, static_cast<std::uint32_t>((required - 8U) / 4U));
    if (response.handle.has_value()) {
        const std::uint32_t descriptor = response.move_handle ? 0x10U : 0x01U;
        write_le32(result, 8U, descriptor);
        write_le32(result, 12U, *response.handle);
    }
    write_le32(result, cmif_offset, kSfcoMagic);
    write_le32(result, cmif_offset + 4U, response.result);
    write_le32(result, cmif_offset + 8U, response.command_id);
    write_le32(result, cmif_offset + 12U, 0U);
    std::copy(response.data.begin(), response.data.end(),
              result.begin() + static_cast<std::ptrdiff_t>(cmif_offset + 0x10U));
    return result;
}

HorizonKernel::HorizonKernel(AddressSpace& memory)
    : memory_(memory),
      hid_shared_memory_(std::make_shared<KSharedMemory>(kHidSharedMemorySize)) {
    auto sm_port = std::make_shared<KPort>("sm:");
    (void)handles_.insert(sm_port);
    ports_.emplace(sm_port->name(), std::move(sm_port));

    write_le64(hid_shared_memory_->bytes, kNpadAreaOffset, 0U);
    write_le64(hid_shared_memory_->bytes, kNpadAreaOffset + 8U, 1U);
    write_le64(hid_shared_memory_->bytes, kNpadAreaOffset + 16U, 0U);
    write_le64(hid_shared_memory_->bytes, kNpadAreaOffset + 24U, 0U);
    write_le64(hid_shared_memory_->bytes, kNpadFullEntryOffset, 0U);
    write_le64(hid_shared_memory_->bytes, kNpadHandheldEntryOffset, 0U);
}

void HorizonKernel::svc_set_heap_size(CpuState& state) {
    const std::uint64_t requested_size = state.x[1];
    if (requested_size > kMaximumHeapSize ||
        requested_size > std::numeric_limits<std::size_t>::max()) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0;
        return;
    }
    if (requested_size > heap_size_) {
        if (heap_size_ != 0U) {
            state.x[0] = kInvalidArgument;
            state.x[1] = 0;
            return;
        }
        try {
            memory_.map_zeroed(kHeapBase, static_cast<std::size_t>(requested_size),
                               Permission::Read | Permission::Write);
            heap_size_ = requested_size;
        } catch (const std::exception&) {
            state.x[0] = kInvalidArgument;
            state.x[1] = 0;
            return;
        }
    }
    state.x[0] = kSuccess;
    state.x[1] = kHeapBase;
}

void HorizonKernel::svc_exit_process(CpuState& state) noexcept {
    state.x[0] = kSuccess;
    running_ = false;
}

void HorizonKernel::svc_break(CpuState& state) {
    std::cerr << "\n[HorizonKernel] svcBreak register dump\n";
    for (unsigned index = 0; index < 31U; ++index) {
        std::cerr << "X" << std::dec << std::setw(2) << std::setfill('0')
                  << index << " = 0x" << std::hex << std::setw(16)
                  << std::setfill('0') << state.x[index];
        if ((index % 2U) == 1U || index == 30U) std::cerr << '\n';
        else std::cerr << "    ";
    }
    std::cerr << "SP  = 0x" << std::hex << std::setw(16) << std::setfill('0')
              << state.sp << '\n'
              << "PC  = 0x" << std::setw(16) << std::setfill('0')
              << state.pc << std::dec << "\n[HorizonKernel] stopped\n";
    running_ = false;
}

void HorizonKernel::svc_output_debug_string(CpuState& state) {
    constexpr std::uint64_t kMaximumDebugString = 1024U * 1024U;
    const std::uint64_t address = state.x[0];
    const std::uint64_t length = state.x[1];
    if (length > kMaximumDebugString ||
        length > std::numeric_limits<std::size_t>::max()) {
        state.x[0] = kInvalidArgument;
        return;
    }
    try {
        const auto bytes = memory_.view(address, static_cast<std::size_t>(length),
                                        Permission::Read);
        std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

std::string HorizonKernel::read_guest_string(std::uint64_t address,
                                             std::size_t maximum) const {
    std::string value;
    value.reserve(maximum);
    for (std::size_t i = 0; i < maximum; ++i) {
        const std::uint8_t byte = memory_.view(address + i, 1U)[0];
        if (byte == 0U) return value;
        value.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("guest string is not null terminated within its limit");
}

void HorizonKernel::svc_connect_to_named_port(CpuState& state) {
    try {
        const std::string name = read_guest_string(state.x[1], 8U);
        const auto found = ports_.find(name);
        if (found == ports_.end()) {
            state.x[0] = kInvalidArgument;
            state.x[1] = 0U;
            return;
        }
        auto session = std::make_shared<KClientSession>("sm");
        state.x[1] = handles_.insert(std::move(session));
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0U;
    }
}

CmifResponse HorizonKernel::dispatch_cmif(
    const KClientSession& session, const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (session.service_name() == "sm") {
        if (request.command_id == 0U) {
            return response;
        }
        if (request.command_id == 1U) {
            if (request.data.size() < 8U) {
                response.result = kInvalidArgument;
                return response;
            }
            std::vector<std::uint8_t> name_bytes(request.data.begin(),
                                                  request.data.begin() + 8);
            const std::string service_name = trim_service_name(name_bytes);
            if (service_name != "hid") {
                response.result = kInvalidArgument;
                return response;
            }
            response.handle = handles_.insert(
                std::make_shared<KClientSession>("hid"));
            response.move_handle = true;
            return response;
        }
        response.result = kInvalidCommand;
        return response;
    }

    if (session.service_name() == "hid") {
        if (request.command_id == 0U) {
            response.handle = handles_.insert(
                std::make_shared<KClientSession>("applet_resource"));
            response.move_handle = true;
            return response;
        }
        if (request.command_id == 100U || request.command_id == 101U ||
            request.command_id == 103U) {
            return response;
        }
        response.result = kInvalidCommand;
        return response;
    }

    if (session.service_name() == "applet_resource") {
        if (request.command_id == 0U) {
            response.handle = handles_.insert(hid_shared_memory_);
            response.move_handle = false;
            return response;
        }
        response.result = kInvalidCommand;
        return response;
    }

    response.result = kInvalidCommand;
    return response;
}

void HorizonKernel::svc_send_sync_request(CpuState& state) {
    try {
        const Handle handle = static_cast<Handle>(state.x[0]);
        const auto object = handles_.get(handle);
        if (!object || object->kind() != KernelObjectKind::ClientSession) {
            state.x[0] = kInvalidHandle;
            return;
        }
        const auto session = std::static_pointer_cast<KClientSession>(object);
        const auto command_buffer = memory_.view(state.tpidrro_el0, 0x100U);
        const CmifRequest request = CmifCodec::parse_request(command_buffer);
        const CmifResponse response = dispatch_cmif(*session, request);
        const std::vector<std::uint8_t> reply = CmifCodec::build_response(response, 0x100U);
        memory_.write(state.tpidrro_el0, reply);
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

void HorizonKernel::svc_map_shared_memory(CpuState& state) {
    const Handle handle = static_cast<Handle>(state.x[0]);
    const auto object = handles_.get(handle);
    if (!object || object->kind() != KernelObjectKind::SharedMemory) {
        state.x[0] = kInvalidHandle;
        return;
    }
    const auto shared = std::static_pointer_cast<KSharedMemory>(object);
    const std::uint64_t requested_address = state.x[1];
    const std::uint64_t requested_size = state.x[2];
    if (requested_size == 0U || requested_size > shared->bytes.size() ||
        requested_size > std::numeric_limits<std::size_t>::max()) {
        state.x[0] = kInvalidArgument;
        return;
    }
    try {
        std::vector<std::uint8_t> copy(
            shared->bytes.begin(),
            shared->bytes.begin() + static_cast<std::ptrdiff_t>(requested_size));
        memory_.map(requested_address, std::move(copy),
                    Permission::Read | Permission::Write);
        shared_mappings_[handle] = SharedMapping{
            shared, requested_address, static_cast<std::size_t>(requested_size)};
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

void HorizonKernel::svc_unmap_shared_memory(CpuState& state) {
    const Handle handle = static_cast<Handle>(state.x[0]);
    const auto found = shared_mappings_.find(handle);
    if (found == shared_mappings_.end() ||
        found->second.guest_address != state.x[1] ||
        found->second.size != state.x[2]) {
        state.x[0] = kInvalidArgument;
        return;
    }
    try {
        memory_.unmap(found->second.guest_address, found->second.size);
        shared_mappings_.erase(found);
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

void HorizonKernel::svc_close_handle(CpuState& state) {
    state.x[0] = handles_.close(static_cast<Handle>(state.x[0]))
        ? kSuccess : kInvalidHandle;
}

void HorizonKernel::update_hid_input(std::uint64_t buttons) {
    ++sampling_number_;
    auto& bytes = hid_shared_memory_->bytes;
    write_le64(bytes, kNpadAreaOffset, sampling_number_);
    write_le64(bytes, kNpadAreaOffset + 8U, 1U);
    write_le64(bytes, kNpadAreaOffset + 16U, 0U);
    write_le64(bytes, kNpadAreaOffset + 24U, 0U);
    write_le64(bytes, kNpadFullEntryOffset, sampling_number_);
    write_le64(bytes, kNpadFullEntryOffset + 8U, buttons);
    write_le64(bytes, kNpadFullEntryOffset + 16U, 0U);
    write_le64(bytes, kNpadFullEntryOffset + 24U, 0U);
    write_le64(bytes, kNpadFullEntryOffset + 32U, 0U);
    write_le64(bytes, kNpadFullEntryOffset + 40U, 0U);
    write_le64(bytes, kNpadHandheldEntryOffset, sampling_number_);
    write_le64(bytes, kNpadHandheldEntryOffset + 8U, buttons);
    write_le64(bytes, kNpadHandheldEntryOffset + 16U, 0U);
    write_le64(bytes, kNpadHandheldEntryOffset + 24U, 0U);
    write_le64(bytes, kNpadHandheldEntryOffset + 32U, 0U);
    write_le64(bytes, kNpadHandheldEntryOffset + 40U, 0U);

    const std::vector<std::uint8_t> section(
        bytes.begin() + static_cast<std::ptrdiff_t>(kNpadAreaOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(kNpadAreaOffset + 0x80U));
    for (const auto& [handle, mapping] : shared_mappings_) {
        (void)handle;
        if (mapping.object == hid_shared_memory_ &&
            mapping.size >= kNpadAreaOffset + section.size()) {
            try {
                memory_.write(mapping.guest_address + kNpadAreaOffset, section);
            } catch (const std::exception&) {
            }
        }
    }
}

bool HorizonKernel::running() const noexcept { return running_; }
const HandleTable& HorizonKernel::handles() const noexcept { return handles_; }

SvcHandler::SvcHandler(HorizonKernel& kernel) noexcept : kernel_(kernel) {}

void SvcHandler::dispatch(std::uint16_t immediate, CpuState& state) {
    switch (immediate) {
    case 0x01: kernel_.svc_set_heap_size(state); break;
    case 0x07: kernel_.svc_exit_process(state); break;
    case 0x09: kernel_.svc_close_handle(state); break;
    case 0x14: kernel_.svc_map_shared_memory(state); break;
    case 0x15: kernel_.svc_unmap_shared_memory(state); break;
    case 0x1F: kernel_.svc_connect_to_named_port(state); break;
    case 0x21: kernel_.svc_send_sync_request(state); break;
    case 0x26: kernel_.svc_break(state); break;
    case 0x27: kernel_.svc_output_debug_string(state); break;
    default:
        state.x[0] = kInvalidCommand;
        std::cerr << "[HorizonKernel] unimplemented SVC 0x"
                  << std::hex << immediate << std::dec << '\n';
        break;
    }
}

bool SvcHandler::running() const noexcept { return kernel_.running(); }

} // namespace horizon
