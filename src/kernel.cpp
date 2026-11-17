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

constexpr std::uint32_t kSuccess = 0U;
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

std::uint64_t read_le64(std::span<const std::uint8_t> bytes,
                        std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8U)
        throw std::runtime_error("truncated CMIF u64 parameter");
    std::uint64_t value = 0U;
    for (unsigned i = 0; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
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

void append_le32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4U; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}

void append_le64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8U; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::string trim_string(std::span<const std::uint8_t> bytes) {
    const auto end = std::find(bytes.begin(), bytes.end(), 0U);
    return std::string(bytes.begin(), end);
}

IpcBufferDescriptor decode_map_descriptor(
    std::span<const std::uint8_t> bytes, std::size_t offset) {
    const std::uint32_t size_low = read_le32(bytes, offset);
    const std::uint32_t address_low = read_le32(bytes, offset + 4U);
    const std::uint32_t attributes = read_le32(bytes, offset + 8U);
    IpcBufferDescriptor descriptor;
    descriptor.size = static_cast<std::uint64_t>(size_low) |
        (static_cast<std::uint64_t>((attributes >> 24U) & 0xFU) << 32U);
    descriptor.address = static_cast<std::uint64_t>(address_low) |
        (static_cast<std::uint64_t>((attributes >> 28U) & 0xFU) << 32U) |
        (static_cast<std::uint64_t>((attributes >> 2U) & 0x7U) << 36U);
    return descriptor;
}

IpcBufferDescriptor decode_pointer_descriptor(
    std::span<const std::uint8_t> bytes, std::size_t offset) {
    const std::uint32_t tag = read_le32(bytes, offset);
    const std::uint32_t address_low = read_le32(bytes, offset + 4U);
    IpcBufferDescriptor descriptor;
    descriptor.size = (tag >> 16U) & 0xFFFFU;
    descriptor.address = static_cast<std::uint64_t>(address_low) |
        (static_cast<std::uint64_t>((tag >> 12U) & 0xFU) << 32U) |
        (static_cast<std::uint64_t>((tag >> 6U) & 0x7U) << 36U);
    return descriptor;
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

KEvent::KEvent() noexcept : KObject(KernelObjectKind::Event) {}

KAudioOut::KAudioOut(SdlAudioOutput& output)
    : KObject(KernelObjectKind::AudioOut),
      device(output),
      buffer_event(std::make_shared<KEvent>()),
      started(true) {
    device.pause(false);
}

KStorage::KStorage(std::shared_ptr<const std::vector<std::uint8_t>> romfs)
    : KObject(KernelObjectKind::Storage), romfs_(std::move(romfs)) {
    if (!romfs_) throw std::invalid_argument("IStorage requires a RomFS image");
}
const std::vector<std::uint8_t>& KStorage::romfs() const noexcept {
    return *romfs_;
}

Handle HandleTable::insert(std::shared_ptr<KObject> object) {
    if (!object) throw std::invalid_argument("cannot insert a null kernel object");
    while (next_handle_ == 0U || objects_.contains(next_handle_)) {
        ++next_handle_;
        if (next_handle_ == 0U)
            throw std::overflow_error("kernel handle table exhausted");
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

    const std::uint32_t header0 = read_le32(command_buffer, 0U);
    const std::uint32_t header1 = read_le32(command_buffer, 4U);
    const std::size_t pointer_count = (header0 >> 16U) & 0xFU;
    const std::size_t send_count = (header0 >> 20U) & 0xFU;
    const std::size_t receive_count = (header0 >> 24U) & 0xFU;
    const std::size_t exchange_count = (header0 >> 28U) & 0xFU;
    const bool has_special_header = (header1 & 0x80000000U) != 0U;

    std::size_t cursor = 8U;
    if (has_special_header) {
        const std::uint32_t special = read_le32(command_buffer, cursor);
        cursor += 4U;
        if ((special & 1U) != 0U) cursor += 8U;
        const std::size_t copy_count = (special >> 1U) & 0xFU;
        const std::size_t move_count = (special >> 5U) & 0xFU;
        cursor += (copy_count + move_count) * 4U;
    }

    CmifRequest request;
    const auto read_descriptors = [&](std::size_t count, bool pointer,
                                      std::vector<IpcBufferDescriptor>& output,
                                      std::vector<IpcBufferDescriptor>* auto_output,
                                      std::uint32_t auto_type) {
        const std::size_t stride = pointer ? 8U : 12U;
        if (count > (command_buffer.size() - std::min(cursor, command_buffer.size())) / stride)
            throw std::runtime_error("HIPC buffer descriptor array exceeds TLS command buffer");
        output.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (pointer) {
                output.push_back(decode_pointer_descriptor(command_buffer, cursor));
            } else {
                const IpcBufferDescriptor descriptor =
                    decode_map_descriptor(command_buffer, cursor);
                const std::uint32_t attributes = read_le32(command_buffer, cursor + 8U);
                if (auto_output != nullptr && (attributes & 0xFFU) == auto_type)
                    auto_output->push_back(descriptor);
                else
                    output.push_back(descriptor);
            }
            cursor += stride;
        }
    };

    if (cursor > command_buffer.size())
        throw std::runtime_error("HIPC special header exceeds TLS command buffer");
    read_descriptors(pointer_count, true, request.pointer_buffers, nullptr, 0U);
    read_descriptors(send_count, false, request.send_buffers,
                     &request.auto_send_buffers, 0x21U);
    read_descriptors(receive_count, false, request.receive_buffers,
                     &request.auto_receive_buffers, 0x22U);
    read_descriptors(exchange_count, false, request.exchange_buffers, nullptr, 0U);

    const std::size_t raw_offset = align_up(cursor, 0x10U);
    const std::size_t raw_words = header1 & 0x3FFU;
    const std::size_t raw_size = raw_words * 4U;
    if (raw_size < 0x10U || raw_offset > command_buffer.size() ||
        raw_size > command_buffer.size() - raw_offset)
        throw std::runtime_error("HIPC raw data range is invalid");
    if (read_le32(command_buffer, raw_offset) != kSfciMagic)
        throw std::runtime_error("IPC request does not contain SFCI magic");

    request.command_id = read_le32(command_buffer, raw_offset + 8U);
    const std::size_t payload_offset = raw_offset + 0x10U;
    const std::size_t payload_size = raw_size - 0x10U;
    request.data.assign(
        command_buffer.begin() + static_cast<std::ptrdiff_t>(payload_offset),
        command_buffer.begin() + static_cast<std::ptrdiff_t>(payload_offset + payload_size));
    return request;
}

std::vector<std::uint8_t> CmifCodec::build_response(
    const CmifResponse& response, std::size_t capacity) {
    const bool has_handle = response.handle.has_value();
    const std::size_t special_size = has_handle ? 8U : 0U;
    const std::size_t raw_offset = align_up(8U + special_size, 0x10U);
    const std::size_t raw_size = align_up(0x10U + response.data.size(), 4U);
    const std::size_t required = raw_offset + raw_size;
    const std::size_t output_size = std::max(capacity, required);
    if (raw_size / 4U > 0x3FFU)
        throw std::runtime_error("CMIF response exceeds the HIPC raw-data limit");

    std::vector<std::uint8_t> result(output_size, 0U);
    write_le32(result, 0U, 0U);
    std::uint32_t header1 = static_cast<std::uint32_t>(raw_size / 4U);
    if (has_handle) {
        header1 |= 0x80000000U;
        write_le32(result, 8U, response.move_handle ? 0x20U : 0x02U);
        write_le32(result, 12U, *response.handle);
    }
    write_le32(result, 4U, header1);
    write_le32(result, raw_offset, kSfcoMagic);
    write_le32(result, raw_offset + 4U, 0U);
    write_le32(result, raw_offset + 8U, response.result);
    write_le32(result, raw_offset + 12U, 0U);
    std::copy(response.data.begin(), response.data.end(),
              result.begin() + static_cast<std::ptrdiff_t>(raw_offset + 0x10U));
    return result;
}

HorizonKernel::HorizonKernel(AddressSpace& memory,
                             std::vector<std::uint8_t> romfs,
                             SdlAudioOutput& audio)
    : memory_(memory),
      hid_shared_memory_(std::make_shared<KSharedMemory>(kHidSharedMemorySize)),
      romfs_(std::make_shared<const std::vector<std::uint8_t>>(std::move(romfs))),
      audio_device_(audio) {
    auto sm_port = std::make_shared<KPort>("sm:");
    (void)handles_.insert(sm_port);
    const std::string sm_port_name = sm_port->name();
    ports_.emplace(sm_port_name, sm_port);

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
        state.x[1] = 0U;
        return;
    }
    if (requested_size > heap_size_) {
        if (heap_size_ != 0U) {
            state.x[0] = kInvalidArgument;
            state.x[1] = 0U;
            return;
        }
        try {
            memory_.map_zeroed(kHeapBase, static_cast<std::size_t>(requested_size),
                               Permission::Read | Permission::Write);
            heap_size_ = requested_size;
        } catch (const std::exception&) {
            state.x[0] = kInvalidArgument;
            state.x[1] = 0U;
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
        const std::uint8_t byte = memory_.read_u8(address + i, Permission::Read);
        if (byte == 0U) return value;
        value.push_back(static_cast<char>(byte));
    }
    return value;
}

std::string HorizonKernel::read_service_name(const CmifRequest& request) const {
    const IpcBufferDescriptor* descriptor = nullptr;
    if (!request.send_buffers.empty()) descriptor = &request.send_buffers.front();
    else if (!request.auto_send_buffers.empty()) descriptor = &request.auto_send_buffers.front();
    else if (!request.pointer_buffers.empty()) descriptor = &request.pointer_buffers.front();

    if (descriptor != nullptr) {
        if (descriptor->size == 0U || descriptor->size > 0x100U)
            throw std::runtime_error("GetService buffer descriptor has an invalid size");
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(descriptor->size, 8U));
        const auto bytes = memory_.view(descriptor->address, count, Permission::Read);
        return trim_string(bytes);
    }
    if (request.data.empty())
        throw std::runtime_error("GetService did not provide a service name");
    return trim_string(std::span<const std::uint8_t>(request.data).first(
        std::min<std::size_t>(request.data.size(), 8U)));
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

CmifResponse HorizonKernel::dispatch_storage(
    const KStorage& storage, const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (request.command_id == 4U) {
        append_le64(response.data, storage.romfs().size());
        return response;
    }

    if (request.command_id == 0U) {
        if (request.data.size() < 16U || request.receive_buffers.empty()) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::uint64_t offset = read_le64(request.data, 0U);
        const std::uint64_t size = read_le64(request.data, 8U);
        const IpcBufferDescriptor& destination = request.receive_buffers.front();
        if (size > destination.size ||
            offset > storage.romfs().size() ||
            size > storage.romfs().size() - offset ||
            size > std::numeric_limits<std::size_t>::max()) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            const auto first = storage.romfs().begin() +
                static_cast<std::ptrdiff_t>(offset);
            const auto last = first + static_cast<std::ptrdiff_t>(size);
            std::vector<std::uint8_t> bytes(first, last);
            memory_.write(destination.address, bytes);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_cmif(
    const KClientSession& session, const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (session.service_name() == "sm") {
        if (request.command_id == 0U) return response;
        if (request.command_id == 1U) {
            std::string service_name;
            try {
                service_name = read_service_name(request);
            } catch (const std::exception&) {
                response.result = kInvalidArgument;
                return response;
            }
            if (service_name != "hid" && service_name != "fsp-srv" &&
                service_name != "audout:u") {
                response.result = kInvalidArgument;
                return response;
            }
            response.handle = handles_.insert(
                std::make_shared<KClientSession>(service_name));
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
            request.command_id == 103U)
            return response;
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

    if (session.service_name() == "audout:u") {
        return dispatch_audio_manager(request);
    }

    if (session.service_name() == "fsp-srv") {
        if (request.command_id == 1U) {
            if (request.data.size() < 8U)
                response.result = kInvalidArgument;
            return response;
        }
        if (request.command_id == 200U) {
            response.handle = handles_.insert(
                std::make_shared<KStorage>(romfs_));
            response.move_handle = true;
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
        if (!object) {
            state.x[0] = kInvalidHandle;
            return;
        }
        const auto command_buffer = memory_.view(state.tpidrro_el0, 0x100U);
        const CmifRequest request = CmifCodec::parse_request(command_buffer);
        CmifResponse response;
        if (object->kind() == KernelObjectKind::ClientSession) {
            response = dispatch_cmif(
                *std::static_pointer_cast<KClientSession>(object), request);
        } else if (object->kind() == KernelObjectKind::Storage) {
            response = dispatch_storage(
                *std::static_pointer_cast<KStorage>(object), request);
        } else if (object->kind() == KernelObjectKind::AudioOut) {
            response = dispatch_audio_out(
                std::static_pointer_cast<KAudioOut>(object), request);
        } else {
            state.x[0] = kInvalidHandle;
            return;
        }
        if (response.result != kSuccess) {
            std::cerr << "[HorizonKernel] IPC command " << request.command_id
                      << " returned 0x" << std::hex << response.result
                      << std::dec << '\n';
        }
        const std::vector<std::uint8_t> reply =
            CmifCodec::build_response(response, 0x100U);
        if (reply.size() > 0x100U)
            throw std::runtime_error("IPC reply does not fit the TLS command buffer");
        memory_.write(state.tpidrro_el0, reply);
        state.x[0] = kSuccess;
    } catch (const std::exception& error) {
        std::cerr << "[HorizonKernel] svcSendSyncRequest failed: "
                  << error.what() << '\n';
        state.x[0] = kInvalidArgument;
    }
}

const IpcBufferDescriptor* HorizonKernel::find_send_buffer(
    const CmifRequest& request, bool allow_pointer) const noexcept {
    if (!request.send_buffers.empty()) return &request.send_buffers.front();
    if (!request.auto_send_buffers.empty()) return &request.auto_send_buffers.front();
    if (allow_pointer && !request.pointer_buffers.empty())
        return &request.pointer_buffers.front();
    return nullptr;
}

const IpcBufferDescriptor* HorizonKernel::find_receive_buffer(
    const CmifRequest& request) const noexcept {
    if (!request.receive_buffers.empty()) return &request.receive_buffers.front();
    if (!request.auto_receive_buffers.empty())
        return &request.auto_receive_buffers.front();
    if (!request.exchange_buffers.empty()) return &request.exchange_buffers.front();
    return nullptr;
}

CmifResponse HorizonKernel::dispatch_audio_manager(
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (request.command_id == 0U) {
        const IpcBufferDescriptor* destination = find_receive_buffer(request);
        if (destination == nullptr || destination->size < 10U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::vector<std::uint8_t> name{
            'D', 'e', 'v', 'i', 'c', 'e', 'O', 'u', 't', 0U
        };
        try {
            memory_.write(destination->address, name);
            append_le32(response.data, 1U);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    if (request.command_id == 1U) {
        const IpcBufferDescriptor* source = find_send_buffer(request);
        if (source == nullptr || source->size == 0U || source->size > 0x100U) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            const auto name_bytes = memory_.view(
                source->address, static_cast<std::size_t>(source->size),
                Permission::Read);
            const std::string device_name = trim_string(name_bytes);
            if (device_name != "DeviceOut") {
                response.result = kInvalidArgument;
                return response;
            }
            auto audio_out = std::make_shared<KAudioOut>(audio_device_);
            audio_sessions_.push_back(audio_out);
            response.handle = handles_.insert(audio_out);
            response.move_handle = true;
            append_le32(response.data, 48000U);
            append_le32(response.data, 2U);
            append_le32(response.data, 2U);
            append_le32(response.data, 0U);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_audio_out(
    const std::shared_ptr<KAudioOut>& audio_out,
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (request.command_id == 0U) {
        append_le32(response.data, audio_out->started ? 0U : 1U);
        return response;
    }

    if (request.command_id == 1U) {
        audio_out->started = true;
        audio_out->device.pause(false);
        return response;
    }

    if (request.command_id == 2U) {
        audio_out->started = false;
        audio_out->device.pause(true);
        audio_out->device.clear();
        release_all_audio_buffers(audio_out);
        return response;
    }

    if (request.command_id == 3U || request.command_id == 7U) {
        const IpcBufferDescriptor* descriptor = find_send_buffer(request, true);
        if (descriptor == nullptr ||
            descriptor->size < kAudioOutBufferHeaderSize) {
            response.result = kInvalidArgument;
            return response;
        }

        try {
            const auto header = memory_.view(
                descriptor->address, kAudioOutBufferHeaderSize, Permission::Read);
            const std::uint64_t sample_buffer_ptr = read_le64(header, 8U);
            const std::uint64_t sample_buffer_capacity = read_le64(header, 16U);
            const std::uint64_t data_size = read_le64(header, 24U);
            const std::uint64_t data_offset = read_le64(header, 32U);

            if (data_size == 0U || data_size > sample_buffer_capacity ||
                data_offset > sample_buffer_capacity ||
                data_size > sample_buffer_capacity - data_offset ||
                sample_buffer_ptr > std::numeric_limits<std::uint64_t>::max() - data_offset ||
                data_size > 4U * 1024U * 1024U ||
                (data_size & 3U) != 0U ||
                data_size > std::numeric_limits<std::size_t>::max()) {
                response.result = kInvalidArgument;
                return response;
            }

            const auto pcm = memory_.view(
                sample_buffer_ptr + data_offset,
                static_cast<std::size_t>(data_size), Permission::Read);
            audio_out->device.queue_pcm(pcm);
            audio_out->submitted_bytes += data_size;
            audio_out->active_buffers.push_back(
                QueuedAudioBuffer{descriptor->address, audio_out->submitted_bytes});
            if (audio_out->released_buffers.empty())
                audio_out->buffer_event->signaled = false;
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    if (request.command_id == 4U) {
        response.handle = handles_.insert(audio_out->buffer_event);
        response.move_handle = false;
        return response;
    }

    if (request.command_id == 5U || request.command_id == 8U) {
        update_audio_out(audio_out);
        const IpcBufferDescriptor* destination = find_receive_buffer(request);
        if (destination == nullptr || destination->size < 8U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::size_t capacity = static_cast<std::size_t>(
            std::min<std::uint64_t>(destination->size / 8U, 256U));
        std::uint32_t count = 0U;
        try {
            while (count < capacity && !audio_out->released_buffers.empty()) {
                memory_.write_u64(destination->address +
                                  static_cast<std::uint64_t>(count) * 8U,
                                  audio_out->released_buffers.front());
                audio_out->released_buffers.pop_front();
                ++count;
            }
            append_le32(response.data, count);
            audio_out->buffer_event->signaled =
                !audio_out->released_buffers.empty();
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    response.result = kInvalidCommand;
    return response;
}

void HorizonKernel::update_audio_out(
    const std::shared_ptr<KAudioOut>& audio_out) {
    const std::uint64_t queued = audio_out->device.queued_bytes();
    const std::uint64_t consumed = audio_out->submitted_bytes > queued
        ? audio_out->submitted_bytes - queued : 0U;
    while (!audio_out->active_buffers.empty() &&
           audio_out->active_buffers.front().end_byte <= consumed) {
        audio_out->released_buffers.push_back(
            audio_out->active_buffers.front().header_address);
        audio_out->active_buffers.pop_front();
    }
    if (!audio_out->released_buffers.empty())
        audio_out->buffer_event->signaled = true;
}

void HorizonKernel::release_all_audio_buffers(
    const std::shared_ptr<KAudioOut>& audio_out) {
    while (!audio_out->active_buffers.empty()) {
        audio_out->released_buffers.push_back(
            audio_out->active_buffers.front().header_address);
        audio_out->active_buffers.pop_front();
    }
    audio_out->buffer_event->signaled = true;
}

void HorizonKernel::update_audio_events() {
    auto destination = audio_sessions_.begin();
    for (auto it = audio_sessions_.begin(); it != audio_sessions_.end(); ++it) {
        if (auto session = it->lock()) {
            update_audio_out(session);
            *destination++ = *it;
        }
    }
    audio_sessions_.erase(destination, audio_sessions_.end());
}

void HorizonKernel::svc_reset_signal(CpuState& state) {
    const auto object = handles_.get(static_cast<Handle>(state.x[0]));
    if (!object || object->kind() != KernelObjectKind::Event) {
        state.x[0] = kInvalidHandle;
        return;
    }
    std::static_pointer_cast<KEvent>(object)->signaled = false;
    state.x[0] = kSuccess;
}

void HorizonKernel::svc_wait_synchronization(CpuState& state) {
    const std::uint64_t handles_address = state.x[1];
    const std::uint64_t handle_count = state.x[2];
    if (handle_count == 0U || handle_count > 64U ||
        handles_address > std::numeric_limits<std::uint64_t>::max() -
            handle_count * sizeof(std::uint32_t)) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0U;
        return;
    }

    try {
        const auto raw_handles = memory_.view(
            handles_address, static_cast<std::size_t>(handle_count) * 4U,
            Permission::Read);
        std::vector<Handle> handles;
        handles.reserve(static_cast<std::size_t>(handle_count));
        for (std::size_t index = 0; index < handle_count; ++index)
            handles.push_back(read_le32(raw_handles, index * 4U));

        const auto find_signaled = [&]() -> std::optional<std::uint32_t> {
            for (std::size_t index = 0; index < handles.size(); ++index) {
                const auto object = handles_.get(handles[index]);
                if (object && object->kind() == KernelObjectKind::Event &&
                    std::static_pointer_cast<KEvent>(object)->signaled) {
                    return static_cast<std::uint32_t>(index);
                }
            }
            return std::nullopt;
        };

        std::optional<std::uint32_t> ready = find_signaled();
        for (unsigned attempt = 0U; !ready.has_value() && attempt < 120U; ++attempt) {
            update_audio_events();
            ready = find_signaled();
            if (!ready.has_value()) SDL_Delay(1U);
        }

        if (!ready.has_value()) {
            for (std::size_t index = 0; index < handles.size(); ++index) {
                const auto object = handles_.get(handles[index]);
                if (object && object->kind() == KernelObjectKind::Event) {
                    std::static_pointer_cast<KEvent>(object)->signaled = true;
                    ready = static_cast<std::uint32_t>(index);
                    break;
                }
            }
        }

        state.x[1] = ready.value_or(0U);
        state.x[0] = ready.has_value() ? kSuccess : kInvalidHandle;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0U;
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
std::size_t HorizonKernel::romfs_size() const noexcept {
    return romfs_ ? romfs_->size() : 0U;
}

SvcHandler::SvcHandler(HorizonKernel& kernel) noexcept : kernel_(kernel) {}

void SvcHandler::dispatch(std::uint16_t immediate, CpuState& state) {
    switch (immediate) {
    case 0x01: kernel_.svc_set_heap_size(state); break;
    case 0x07: kernel_.svc_exit_process(state); break;
    case 0x09: kernel_.svc_close_handle(state); break;
    case 0x14: kernel_.svc_map_shared_memory(state); break;
    case 0x15: kernel_.svc_unmap_shared_memory(state); break;
    case 0x17: kernel_.svc_reset_signal(state); break;
    case 0x18: kernel_.svc_wait_synchronization(state); break;
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
