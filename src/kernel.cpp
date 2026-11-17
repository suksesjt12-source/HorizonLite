#include "horizon/kernel.hpp"
#include "horizon/display.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
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

std::size_t parcel_payload_offset(std::span<const std::uint8_t> parcel) {
    if (parcel.size() >= 8U && read_le32(parcel, 0U) == 0x43524150U)
        return 8U;
    if (parcel.size() >= 12U && read_le32(parcel, 0U) == 0U &&
        read_le32(parcel, 4U) == 0U) {
        const std::uint32_t length = read_le32(parcel, 8U);
        if (length <= 256U) {
            const std::size_t end = 12U +
                (static_cast<std::size_t>(length) + 1U) * 2U;
            if (end <= parcel.size()) return align_up(end, 4U);
        }
    }
    if (parcel.size() >= 4U) {
        const std::uint32_t length = read_le32(parcel, 0U);
        if (length <= 256U) {
            const std::size_t end = 4U +
                (static_cast<std::size_t>(length) + 1U) * 2U;
            if (end <= parcel.size()) return align_up(end, 4U);
        }
    }
    return 0U;
}

std::uint32_t parcel_u32(std::span<const std::uint8_t> parcel,
                         std::size_t offset) {
    return read_le32(parcel, offset);
}

std::vector<std::uint8_t> make_native_window_parcel() {
    std::vector<std::uint8_t> parcel;
    append_le32(parcel, 0x43524150U);
    append_le32(parcel, 1U);
    append_le32(parcel, 1U);
    append_le32(parcel, 7U);
    for (const char ch : std::string("dispdrv")) {
        parcel.push_back(static_cast<std::uint8_t>(ch));
        parcel.push_back(0U);
    }
    parcel.push_back(0U);
    parcel.push_back(0U);
    while ((parcel.size() & 3U) != 0U) parcel.push_back(0U);
    return parcel;
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

KThread::KThread(std::size_t index)
    : KObject(KernelObjectKind::Thread), thread_index(index) {}

KProfile::KProfile() : KObject(KernelObjectKind::Profile) {}

KFileSystem::KFileSystem(std::filesystem::path root_path)
    : KObject(KernelObjectKind::FileSystem), root(std::move(root_path)) {
    std::filesystem::create_directories(root);
}

KFile::KFile(std::filesystem::path file_path, std::uint32_t mode)
    : KObject(KernelObjectKind::File),
      path(std::move(file_path)),
      open_mode(mode) {
    std::ios::openmode flags = std::ios::binary;
    if ((mode & 1U) != 0U) flags |= std::ios::in;
    if ((mode & 2U) != 0U) flags |= std::ios::out;
    stream.open(path, flags);
    if (!stream.is_open())
        throw std::runtime_error("failed to open save-data file");
}

KFile::~KFile() {
    if (stream.is_open()) {
        stream.flush();
        stream.close();
    }
}

KEvent::KEvent() noexcept : KObject(KernelObjectKind::Event) {}

KAudioOut::KAudioOut(SdlAudioOutput& output)
    : KObject(KernelObjectKind::AudioOut),
      device(output),
      buffer_event(std::make_shared<KEvent>()),
      started(true) {
    device.pause(false);
}

KDisplayLayer::KDisplayLayer()
    : KObject(KernelObjectKind::DisplayLayer),
      presented_rgba(1280U * 720U * 4U, 0U),
      vsync_event(std::make_shared<KEvent>()),
      release_event(std::make_shared<KEvent>()) {}

KHOSBinderDriver::KHOSBinderDriver(std::shared_ptr<KDisplayLayer> display_layer)
    : KObject(KernelObjectKind::BinderDriver), layer(std::move(display_layer)) {
    if (!layer) throw std::invalid_argument("Binder driver requires a display layer");
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
      audio_device_(audio),
      display_layer_(std::make_shared<KDisplayLayer>()) {
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


CmifResponse HorizonKernel::dispatch_account(const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    constexpr Uid profile_uid{0x0000000000000001ULL, 0x1122334455667788ULL};

    switch (request.command_id) {
    case 0U:
        append_le32(response.data, 1U);
        return response;
    case 1U:
        if (request.data.size() < 16U) {
            response.result = kInvalidArgument;
            return response;
        }
        append_le32(response.data,
            read_le64(request.data, 0U) == profile_uid.low &&
            read_le64(request.data, 8U) == profile_uid.high ? 1U : 0U);
        return response;
    case 2U:
    case 3U: {
        const IpcBufferDescriptor* output = nullptr;
        if (!request.pointer_buffers.empty()) output = &request.pointer_buffers.front();
        else output = find_receive_buffer(request);
        if (output == nullptr || output->size < 16U) {
            response.result = kInvalidArgument;
            return response;
        }
        std::vector<std::uint8_t> uid;
        append_le64(uid, profile_uid.low);
        append_le64(uid, profile_uid.high);
        memory_.write(output->address, uid);
        return response;
    }
    case 4U:
        append_le64(response.data, profile_uid.low);
        append_le64(response.data, profile_uid.high);
        return response;
    case 5U:
        if (request.data.size() < 16U ||
            read_le64(request.data, 0U) != profile_uid.low ||
            read_le64(request.data, 8U) != profile_uid.high) {
            response.result = kInvalidArgument;
            return response;
        }
        response.handle = handles_.insert(std::make_shared<KProfile>());
        response.move_handle = true;
        return response;
    case 100U:
    case 140U:
        return response;
    default:
        response.result = kInvalidCommand;
        return response;
    }
}

CmifResponse HorizonKernel::dispatch_profile(const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    constexpr Uid profile_uid{0x0000000000000001ULL, 0x1122334455667788ULL};
    std::vector<std::uint8_t> base(0x38U, 0U);
    for (unsigned byte = 0U; byte < 8U; ++byte) {
        base[byte] = static_cast<std::uint8_t>(profile_uid.low >> (byte * 8U));
        base[8U + byte] = static_cast<std::uint8_t>(profile_uid.high >> (byte * 8U));
    }
    const std::string nickname = "Player1";
    std::copy(nickname.begin(), nickname.end(), base.begin() + 24U);

    if (request.command_id == 1U) {
        response.data = std::move(base);
        return response;
    }
    if (request.command_id == 0U) {
        const IpcBufferDescriptor* output = find_receive_buffer(request);
        if (output == nullptr || output->size < 0x80U) {
            response.result = kInvalidArgument;
            return response;
        }
        std::vector<std::uint8_t> user_data(0x80U, 0U);
        std::copy(base.begin(), base.begin() + 16U, user_data.begin());
        memory_.write(output->address, user_data);
        response.data = std::move(base);
        return response;
    }
    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_system_settings(
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    if (request.command_id == 3U || request.command_id == 4U) {
        const IpcBufferDescriptor* output = find_receive_buffer(request);
        if (output == nullptr || output->size < 0x100U) {
            response.result = kInvalidArgument;
            return response;
        }
        std::vector<std::uint8_t> firmware(0x100U, 0U);
        firmware[0] = 18U;
        firmware[1] = 1U;
        firmware[2] = 0U;
        const auto put_string = [&firmware](std::size_t offset,
                                            std::size_t capacity,
                                            const std::string& value) {
            const std::size_t count = std::min(capacity - 1U, value.size());
            std::copy_n(value.begin(), count,
                        firmware.begin() + static_cast<std::ptrdiff_t>(offset));
        };
        put_string(0x04U, 0x20U, "NX");
        put_string(0x24U, 0x40U, "HorizonLite-18.1.0");
        put_string(0x64U, 0x18U, "18.1.0");
        put_string(0x7CU, 0x80U, "NintendoSDK Firmware for NX 18.1.0");
        memory_.write(output->address, firmware);
        return response;
    }
    if (request.command_id == 23U) {
        append_le32(response.data, 1U);
        return response;
    }
    if (request.command_id == 77U) {
        const IpcBufferDescriptor* output = find_receive_buffer(request);
        if (output == nullptr || output->size < 9U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::vector<std::uint8_t> nickname{
            'S','w','i','t','c','h','H','L','E',0U
        };
        memory_.write(output->address, nickname);
        return response;
    }
    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_settings(const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    if (request.command_id == 0U) {
        append_le64(response.data, 0x00000053552D6E65ULL);
        return response;
    }
    if (request.command_id == 4U) {
        append_le32(response.data, 1U);
        return response;
    }
    response.result = kInvalidCommand;
    return response;
}

std::filesystem::path HorizonKernel::resolve_save_path(
    const std::filesystem::path& root, const std::string& guest_path) const {
    if (guest_path.empty() || guest_path.size() > 0x400U)
        throw std::invalid_argument("save-data path is empty or too long");
    std::string normalized = guest_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::filesystem::path relative(normalized);
    relative = relative.relative_path();
    for (const auto& component : relative) {
        if (component == ".." || component == "." || component.empty())
            throw std::invalid_argument("save-data path traversal rejected");
    }
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(root);
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(canonical_root / relative);
    auto root_it = canonical_root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *candidate_it != *root_it)
            throw std::invalid_argument("save-data path escapes its sandbox");
    }
    return candidate;
}

CmifResponse HorizonKernel::dispatch_file_system(
    const std::shared_ptr<KFileSystem>& file_system,
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    const auto read_path = [this, &request]() {
        const IpcBufferDescriptor* descriptor = nullptr;
        if (!request.pointer_buffers.empty())
            descriptor = &request.pointer_buffers.front();
        else if (!request.send_buffers.empty())
            descriptor = &request.send_buffers.front();
        else if (!request.auto_send_buffers.empty())
            descriptor = &request.auto_send_buffers.front();
        if (descriptor == nullptr || descriptor->size == 0U ||
            descriptor->size > 0x400U)
            throw std::invalid_argument("filesystem path descriptor is invalid");
        const auto bytes = memory_.view(descriptor->address,
            static_cast<std::size_t>(descriptor->size), Permission::Read);
        return trim_string(bytes);
    };

    try {
        if (request.command_id == 0U) {
            if (request.data.size() < 8U) {
                response.result = kInvalidArgument;
                return response;
            }
            const auto target = resolve_save_path(file_system->root, read_path());
            if (std::filesystem::exists(target)) {
                response.result = 0x202U;
                return response;
            }
            std::ofstream output(target, std::ios::binary | std::ios::trunc);
            if (!output) {
                response.result = kInvalidArgument;
                return response;
            }
            output.close();
            const std::uint64_t initial_size = read_le64(request.data, 0U);
            if (initial_size != 0U)
                std::filesystem::resize_file(target, initial_size);
            return response;
        }
        if (request.command_id == 1U) {
            const auto target = resolve_save_path(file_system->root, read_path());
            if (!std::filesystem::remove(target)) response.result = 0x202U;
            return response;
        }
        if (request.command_id == 2U) {
            const auto target = resolve_save_path(file_system->root, read_path());
            std::filesystem::create_directories(target);
            return response;
        }
        if (request.command_id == 7U) {
            const auto target = resolve_save_path(file_system->root, read_path());
            if (!std::filesystem::exists(target)) {
                response.result = 0x202U;
                return response;
            }
            append_le32(response.data,
                std::filesystem::is_directory(target) ? 0U : 1U);
            return response;
        }
        if (request.command_id == 8U) {
            if (request.data.size() < 4U) {
                response.result = kInvalidArgument;
                return response;
            }
            const std::uint32_t mode = read_le32(request.data, 0U);
            if ((mode & 7U) == 0U || (mode & ~7U) != 0U) {
                response.result = kInvalidArgument;
                return response;
            }
            const auto target = resolve_save_path(file_system->root, read_path());
            if (!std::filesystem::is_regular_file(target)) {
                response.result = 0x202U;
                return response;
            }
            auto file = std::make_shared<KFile>(target, mode);
            const Handle handle = handles_.insert(file);
            file_system->open_files.emplace_back(file);
            response.handle = handle;
            response.move_handle = true;
            return response;
        }
        if (request.command_id == 10U) {
            for (auto iterator = file_system->open_files.begin();
                 iterator != file_system->open_files.end();) {
                if (auto file = iterator->lock()) {
                    if (file->stream.is_open()) {
                        file->stream.flush();
                        if (!file->stream) {
                            response.result = kInvalidArgument;
                            return response;
                        }
                    }
                    ++iterator;
                } else {
                    iterator = file_system->open_files.erase(iterator);
                }
            }
            return response;
        }
    } catch (const std::filesystem::filesystem_error& error) {
        response.result = error.code() == std::errc::no_such_file_or_directory
            ? 0x202U : kInvalidArgument;
        return response;
    } catch (const std::exception&) {
        response.result = kInvalidArgument;
        return response;
    }
    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_file(
    const std::shared_ptr<KFile>& file, const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    try {
        if (!file->stream.is_open()) {
            response.result = kInvalidArgument;
            return response;
        }
        if (request.command_id == 0U) {
            if (request.data.size() < 16U) {
                response.result = kInvalidArgument;
                return response;
            }
            const std::uint64_t offset = read_le64(request.data, 0U);
            const std::uint64_t requested = read_le64(request.data, 8U);
            const IpcBufferDescriptor* output = find_receive_buffer(request);
            if ((file->open_mode & 1U) == 0U || output == nullptr ||
                requested > output->size ||
                requested > std::numeric_limits<std::size_t>::max()) {
                response.result = kInvalidArgument;
                return response;
            }
            file->stream.clear();
            file->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::vector<std::uint8_t> data(static_cast<std::size_t>(requested));
            file->stream.read(reinterpret_cast<char*>(data.data()),
                              static_cast<std::streamsize>(data.size()));
            const std::size_t count = static_cast<std::size_t>(file->stream.gcount());
            data.resize(count);
            memory_.write(output->address, data);
            append_le64(response.data, count);
            return response;
        }
        if (request.command_id == 1U) {
            if (request.data.size() < 16U) {
                response.result = kInvalidArgument;
                return response;
            }
            const std::uint64_t offset = read_le64(request.data, 0U);
            const std::uint64_t requested = read_le64(request.data, 8U);
            const IpcBufferDescriptor* source = find_send_buffer(request, false);
            if ((file->open_mode & 2U) == 0U || source == nullptr ||
                requested > source->size ||
                requested > std::numeric_limits<std::size_t>::max()) {
                response.result = kInvalidArgument;
                return response;
            }
            const auto bytes = memory_.view(source->address,
                static_cast<std::size_t>(requested), Permission::Read);
            file->stream.clear();
            file->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
            file->stream.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()));
            file->stream.flush();
            if (!file->stream) response.result = kInvalidArgument;
            return response;
        }
        if (request.command_id == 2U) {
            file->stream.flush();
            if (!file->stream) response.result = kInvalidArgument;
            return response;
        }
        if (request.command_id == 3U) {
            if (request.data.size() < 8U || (file->open_mode & 2U) == 0U) {
                response.result = kInvalidArgument;
                return response;
            }
            const std::uint64_t size = read_le64(request.data, 0U);
            file->stream.flush();
            file->stream.close();
            std::filesystem::resize_file(file->path, size);
            std::ios::openmode flags = std::ios::binary;
            if ((file->open_mode & 1U) != 0U) flags |= std::ios::in;
            if ((file->open_mode & 2U) != 0U) flags |= std::ios::out;
            file->stream.open(file->path, flags);
            if (!file->stream) response.result = kInvalidArgument;
            return response;
        }
        if (request.command_id == 4U) {
            append_le64(response.data, std::filesystem::file_size(file->path));
            return response;
        }
    } catch (const std::filesystem::filesystem_error& error) {
        response.result = error.code() == std::errc::no_such_file_or_directory
            ? 0x202U : kInvalidArgument;
    } catch (const std::exception&) {
        response.result = kInvalidArgument;
    }
    if (request.command_id > 4U) response.result = kInvalidCommand;
    return response;
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
                service_name != "audout:u" && service_name != "nvdrv" &&
                service_name != "vi:m") {
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

    if (session.service_name() == "nvdrv")
        return dispatch_nvdrv(request);

    if (session.service_name() == "vi:m") {
        if (request.command_id == 0U) {
            response.handle = handles_.insert(
                std::make_shared<KClientSession>("application_root"));
            response.move_handle = true;
            return response;
        }
        response.result = kInvalidCommand;
        return response;
    }

    if (session.service_name() == "application_root")
        return dispatch_display_root(request);

    if (session.service_name() == "application_display")
        return dispatch_display_service(request);

    if (session.service_name() == "display_stub")
        return response;

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
        } else if (object->kind() == KernelObjectKind::BinderDriver) {
            response = dispatch_hos_binder(
                std::static_pointer_cast<KHOSBinderDriver>(object), request);
        } else {
            state.x[0] = kInvalidHandle;
            return;
        }
        if (response.result != kSuccess) {
            std::cerr << "[HorizonKernel] IPC ";
            if (object->kind() == KernelObjectKind::ClientSession)
                std::cerr << std::static_pointer_cast<KClientSession>(object)->service_name();
            else
                std::cerr << "object";
            std::cerr << " command " << request.command_id
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

CmifResponse HorizonKernel::dispatch_nvdrv(
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (request.command_id == 0U) {
        const IpcBufferDescriptor* source = find_send_buffer(request);
        if (source == nullptr || source->size == 0U || source->size > 0x100U) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            const auto path_bytes = memory_.view(
                source->address, static_cast<std::size_t>(source->size),
                Permission::Read);
            const std::string path = trim_string(path_bytes);
            if (path != "/dev/nvmap" && path != "/dev/nvhost-ctrl" &&
                path != "/dev/nvhost-gpu") {
                append_le32(response.data, 0xFFFFFFFFU);
                append_le32(response.data, kInvalidArgument);
                return response;
            }
            if (next_nv_fd_ <= 0 || next_nv_fd_ == std::numeric_limits<std::int32_t>::max()) {
                response.result = kInvalidArgument;
                return response;
            }
            const std::int32_t fd = next_nv_fd_++;
            nv_devices_.emplace(fd, path);
            append_le32(response.data, static_cast<std::uint32_t>(fd));
            append_le32(response.data, kSuccess);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }

    if (request.command_id == 3U || request.command_id == 8U) {
        append_le32(response.data, kSuccess);
        return response;
    }

    if (request.command_id != 1U && request.command_id != 11U) {
        response.result = kInvalidCommand;
        return response;
    }
    if (request.data.size() < 8U) {
        response.result = kInvalidArgument;
        return response;
    }

    const std::int32_t fd = static_cast<std::int32_t>(read_le32(request.data, 0U));
    const std::uint32_t ioctl_code = read_le32(request.data, 4U);
    const auto device = nv_devices_.find(fd);
    if (device == nv_devices_.end() || device->second != "/dev/nvmap") {
        append_le32(response.data, kInvalidArgument);
        return response;
    }

    const IpcBufferDescriptor* input = find_send_buffer(request);
    const IpcBufferDescriptor* output = find_receive_buffer(request);
    if (input == nullptr) input = find_receive_buffer(request);
    if (output == nullptr) output = input;
    if (input == nullptr || input->size == 0U ||
        input->size > 0x1000U || output == nullptr) {
        append_le32(response.data, kInvalidArgument);
        return response;
    }

    try {
        const auto input_bytes = memory_.view(
            input->address, static_cast<std::size_t>(input->size),
            Permission::Read);
        std::vector<std::uint8_t> out(
            input_bytes.begin(), input_bytes.end());
        std::uint32_t error = kSuccess;

        constexpr std::uint32_t kNvMapCreate = 0xC0080101U;
        constexpr std::uint32_t kNvMapFromId = 0xC0080103U;
        constexpr std::uint32_t kNvMapAlloc = 0xC0200104U;
        constexpr std::uint32_t kNvMapParam = 0xC0080109U;
        constexpr std::uint32_t kNvMapGetId = 0xC008010EU;

        if (ioctl_code == kNvMapCreate) {
            if (input_bytes.size() < 4U) {
                error = kInvalidArgument;
            } else {
                const std::uint32_t requested_size = read_le32(input_bytes, 0U);
                if (requested_size == 0U || requested_size > 0x20000000U ||
                    next_nvmap_handle_ == 0U || next_nvmap_id_ == 0U) {
                    error = kInvalidArgument;
                } else {
                    NvMapBuffer buffer;
                    buffer.handle = next_nvmap_handle_++;
                    buffer.id = next_nvmap_id_++;
                    buffer.size = requested_size;
                    nvmap_buffers_.emplace(buffer.handle, buffer);
                    if (out.size() < 8U) out.resize(8U, 0U);
                    write_le32(out, 0U, requested_size);
                    write_le32(out, 4U, buffer.handle);
                }
            }
        } else if (ioctl_code == kNvMapAlloc) {
            if (input_bytes.size() < 24U) {
                error = kInvalidArgument;
            } else {
                const std::uint32_t handle = read_le32(input_bytes, 0U);
                const auto found = nvmap_buffers_.find(handle);
                const std::uint32_t heap_mask = read_le32(input_bytes, 4U);
                const std::uint32_t flags = read_le32(input_bytes, 8U);
                const std::uint32_t alignment = read_le32(input_bytes, 12U);
                const std::uint64_t guest_address =
                    read_le64(input_bytes, input_bytes.size() >= 32U ? 24U : 16U);
                const std::uint64_t actual_alignment = alignment == 0U
                    ? 0x1000U : alignment;
                if (found == nvmap_buffers_.end() || guest_address == 0U ||
                    actual_alignment > 0x100000U ||
                    (actual_alignment & (actual_alignment - 1U)) != 0U ||
                    (guest_address % actual_alignment) != 0U ||
                    guest_address > std::numeric_limits<std::uint64_t>::max() -
                        found->second.size) {
                    error = kInvalidArgument;
                } else {
                    (void)memory_.view(guest_address,
                        static_cast<std::size_t>(found->second.size),
                        Permission::Read | Permission::Write);
                    found->second.heap_mask = heap_mask;
                    found->second.flags = flags;
                    found->second.alignment = actual_alignment;
                    found->second.guest_address = guest_address;
                    found->second.allocated = true;
                }
            }
        } else if (ioctl_code == kNvMapGetId) {
            if (input_bytes.size() < 8U) {
                error = kInvalidArgument;
            } else {
                const std::uint32_t handle = read_le32(input_bytes, 0U);
                const auto found = nvmap_buffers_.find(handle);
                if (found == nvmap_buffers_.end()) error = kInvalidArgument;
                else {
                    if (out.size() < 8U) out.resize(8U, 0U);
                    write_le32(out, 0U, handle);
                    write_le32(out, 4U, found->second.id);
                }
            }
        } else if (ioctl_code == kNvMapFromId) {
            if (input_bytes.size() < 8U) {
                error = kInvalidArgument;
            } else {
                const std::uint32_t id = read_le32(input_bytes, 0U);
                const auto found = std::find_if(
                    nvmap_buffers_.begin(), nvmap_buffers_.end(),
                    [id](const auto& entry) { return entry.second.id == id; });
                if (found == nvmap_buffers_.end()) error = kInvalidArgument;
                else {
                    if (out.size() < 8U) out.resize(8U, 0U);
                    write_le32(out, 0U, id);
                    write_le32(out, 4U, found->second.handle);
                }
            }
        } else if (ioctl_code == kNvMapParam) {
            if (input_bytes.size() < 8U) {
                error = kInvalidArgument;
            } else {
                const std::uint32_t handle = read_le32(input_bytes, 0U);
                const std::uint32_t parameter = read_le32(input_bytes, 4U);
                const auto found = nvmap_buffers_.find(handle);
                if (found == nvmap_buffers_.end()) error = kInvalidArgument;
                else {
                    std::uint32_t value = 0U;
                    if (parameter == 1U)
                        value = static_cast<std::uint32_t>(found->second.size);
                    else if (parameter == 2U)
                        value = static_cast<std::uint32_t>(found->second.alignment);
                    else if (parameter == 3U)
                        value = found->second.flags;
                    else
                        error = kInvalidArgument;
                    if (out.size() < 8U) out.resize(8U, 0U);
                    write_le32(out, 0U, handle);
                    write_le32(out, 4U, value);
                }
            }
        } else {
            error = kInvalidCommand;
        }

        if (error == kSuccess) {
            if (output->size < out.size())
                error = kInvalidArgument;
            else
                memory_.write(output->address, out);
        }
        append_le32(response.data, error);
    } catch (const std::exception&) {
        append_le32(response.data, kInvalidArgument);
    }
    return response;
}

CmifResponse HorizonKernel::dispatch_display_root(
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    if (request.command_id == 0U) {
        response.handle = handles_.insert(
            std::make_shared<KClientSession>("application_display"));
        response.move_handle = true;
    } else {
        response.result = kInvalidCommand;
    }
    return response;
}

CmifResponse HorizonKernel::dispatch_display_service(
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;

    if (request.command_id == 100U) {
        response.handle = handles_.insert(
            std::make_shared<KHOSBinderDriver>(display_layer_));
        response.move_handle = true;
        return response;
    }
    if (request.command_id == 101U || request.command_id == 102U) {
        response.handle = handles_.insert(
            std::make_shared<KClientSession>("display_stub"));
        response.move_handle = true;
        return response;
    }
    if (request.command_id == 1010U) {
        const IpcBufferDescriptor* source = find_send_buffer(request);
        if (source != nullptr) {
            const auto name = memory_.view(
                source->address, static_cast<std::size_t>(source->size),
                Permission::Read);
            if (trim_string(name) != "Default") {
                response.result = kInvalidArgument;
                return response;
            }
        }
        append_le64(response.data, 0U);
        return response;
    }
    if (request.command_id == 2020U) {
        const IpcBufferDescriptor* source = find_send_buffer(request);
        const IpcBufferDescriptor* destination = find_receive_buffer(request);
        if (source == nullptr || source->size == 0U || source->size > 0x100U) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            const auto name = memory_.view(
                source->address, static_cast<std::size_t>(source->size),
                Permission::Read);
            if (trim_string(name) != "Default") {
                response.result = kInvalidArgument;
                return response;
            }
            if (request.data.size() < 16U) {
                response.result = kInvalidArgument;
                return response;
            }
            (void)read_le64(request.data, 0U);
            (void)read_le64(request.data, 8U);
            const auto native_window = make_native_window_parcel();
            if (destination != nullptr) {
                if (destination->size < native_window.size()) {
                    response.result = kInvalidArgument;
                    return response;
                }
                memory_.write(destination->address, native_window);
                append_le64(response.data, 0U);
            } else {
                response.data = native_window;
            }
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
        return response;
    }
    if (request.command_id == 2101U)
        return response;
    if (request.command_id == 5202U) {
        response.handle = handles_.insert(display_layer_->vsync_event);
        response.move_handle = false;
        return response;
    }

    response.result = kInvalidCommand;
    return response;
}

CmifResponse HorizonKernel::dispatch_hos_binder(
    const std::shared_ptr<KHOSBinderDriver>& binder,
    const CmifRequest& request) {
    CmifResponse response;
    response.command_id = request.command_id;
    if (request.command_id == 1U) return response;
    if (request.command_id == 2U) {
        response.handle = handles_.insert(binder->layer->release_event);
        response.move_handle = false;
        return response;
    }
    if (request.command_id != 0U && request.command_id != 3U) {
        response.result = kInvalidCommand;
        return response;
    }
    if (request.data.size() < 8U) {
        response.result = kInvalidArgument;
        return response;
    }
    const std::uint32_t binder_id = read_le32(request.data, 0U);
    const std::uint32_t transaction = read_le32(request.data, 4U);
    if (binder_id != 1U) {
        response.result = kInvalidArgument;
        return response;
    }

    std::span<const std::uint8_t> parcel;
    const IpcBufferDescriptor* source = find_send_buffer(request);
    if (source != nullptr) {
        parcel = memory_.view(source->address,
            static_cast<std::size_t>(source->size), Permission::Read);
    } else if (request.data.size() > 8U) {
        parcel = std::span<const std::uint8_t>(request.data).subspan(8U);
    }
    const std::size_t cursor = parcel_payload_offset(parcel);
    const auto require_u32 = [&parcel, cursor](std::size_t relative) {
        return parcel_u32(parcel, cursor + relative);
    };
    std::vector<std::uint8_t> reply;

    if (transaction == 10U) {
        append_le32(reply, kSuccess);
        append_le32(reply, 1280U);
        append_le32(reply, 720U);
        append_le32(reply, 0U);
        append_le32(reply, 0U);
    } else if (transaction == 14U) {
        if (parcel.size() < cursor + 16U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::uint32_t slot = require_u32(0U);
        const std::uint32_t reference = require_u32(4U);
        const std::uint64_t offset = read_le64(parcel, cursor + 8U);
        if (slot >= binder->layer->slots.size()) {
            response.result = kInvalidArgument;
            return response;
        }
        auto found = nvmap_buffers_.find(reference);
        if (found == nvmap_buffers_.end()) {
            found = std::find_if(nvmap_buffers_.begin(), nvmap_buffers_.end(),
                [reference](const auto& entry) {
                    return entry.second.id == reference;
                });
        }
        if (found == nvmap_buffers_.end() || !found->second.allocated ||
            offset > found->second.size ||
            found->second.size - offset < 1280U * 720U * 4U) {
            response.result = kInvalidArgument;
            return response;
        }
        binder->layer->slots[slot] =
            DisplayBufferSlot{found->second.handle, offset, true};
        append_le32(reply, kSuccess);
    } else if (transaction == 3U) {
        std::uint32_t selected = 2U;
        for (std::uint32_t attempt = 0U; attempt < 2U; ++attempt) {
            const std::uint32_t candidate =
                (binder->layer->next_dequeue_slot + attempt) % 2U;
            if (binder->layer->slots[candidate].configured) {
                selected = candidate;
                break;
            }
        }
        if (selected >= 2U) {
            response.result = kInvalidArgument;
            return response;
        }
        binder->layer->next_dequeue_slot = (selected + 1U) % 2U;
        append_le32(reply, kSuccess);
        append_le32(reply, selected);
    } else if (transaction == 1U) {
        if (parcel.size() < cursor + 4U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::uint32_t slot = require_u32(0U);
        if (slot >= binder->layer->slots.size() ||
            !binder->layer->slots[slot].configured) {
            response.result = kInvalidArgument;
            return response;
        }
        const auto& binding = binder->layer->slots[slot];
        const auto& buffer = nvmap_buffers_.at(binding.nvmap_handle);
        append_le32(reply, kSuccess);
        append_le32(reply, 0x47425546U);
        append_le32(reply, 1280U);
        append_le32(reply, 720U);
        append_le32(reply, 1280U);
        append_le32(reply, 1U);
        append_le32(reply, buffer.id);
        append_le64(reply, binding.offset);
    } else if (transaction == 7U) {
        if (parcel.size() < cursor + 4U) {
            response.result = kInvalidArgument;
            return response;
        }
        const std::uint32_t slot = require_u32(0U);
        if (slot >= binder->layer->slots.size() ||
            !binder->layer->slots[slot].configured) {
            response.result = kInvalidArgument;
            return response;
        }
        const auto& binding = binder->layer->slots[slot];
        const auto& buffer = nvmap_buffers_.at(binding.nvmap_handle);
        constexpr std::size_t frame_bytes = 1280U * 720U * 4U;
        if (!buffer.allocated || binding.offset > buffer.size ||
            frame_bytes > buffer.size - binding.offset ||
            buffer.guest_address > std::numeric_limits<std::uint64_t>::max() -
                binding.offset) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            const auto source_pixels = memory_.view(
                buffer.guest_address + binding.offset, frame_bytes,
                Permission::Read);
            binder->layer->presented_rgba = UntileTegraBlockLinear(
                source_pixels, 1280U, 720U, 16U,
                (buffer.flags & 1U) != 0U);
            binder->layer->release_event->signaled = true;
            binder->layer->frame_number++;
            binder->layer->vsync_event->signaled = true;
            append_le32(reply, kSuccess);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
            return response;
        }
    } else {
        response.result = kInvalidCommand;
        return response;
    }

    const IpcBufferDescriptor* destination = find_receive_buffer(request);
    if (destination != nullptr && !reply.empty()) {
        if (destination->size < reply.size()) {
            response.result = kInvalidArgument;
            return response;
        }
        try {
            memory_.write(destination->address, reply);
        } catch (const std::exception&) {
            response.result = kInvalidArgument;
        }
    } else {
        response.data = std::move(reply);
    }
    return response;
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
        std::vector<Handle> wait_handles;
        wait_handles.reserve(static_cast<std::size_t>(handle_count));
        for (std::size_t index = 0; index < handle_count; ++index)
            wait_handles.push_back(read_le32(raw_handles, index * 4U));

        const auto find_ready = [this, &wait_handles]()
            -> std::optional<std::uint32_t> {
            for (std::size_t index = 0; index < wait_handles.size(); ++index) {
                const auto object = handles_.get(wait_handles[index]);
                if (!object) return std::nullopt;
                if (object->kind() == KernelObjectKind::Event &&
                    std::static_pointer_cast<KEvent>(object)->signaled)
                    return static_cast<std::uint32_t>(index);
            }
            return std::nullopt;
        };

        const auto ready = find_ready();
        if (ready.has_value()) {
            state.x[0] = kSuccess;
            state.x[1] = *ready;
            return;
        }

        if (active_thread_) {
            const std::int64_t timeout_ns =
                static_cast<std::int64_t>(state.x[3]);
            if (timeout_ns == 0) {
                state.x[0] = 0xEA01U;
                state.x[1] = 0U;
                return;
            }
            active_thread_->waiting_handles = std::move(wait_handles);
            active_thread_->has_deadline = timeout_ns > 0;
            if (timeout_ns > 0) {
                active_thread_->wake_deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(timeout_ns);
            }
            active_thread_->status = ThreadStatus::WaitingSynchronization;
            request_scheduler_yield();
            return;
        }

        std::optional<std::uint32_t> legacy_ready;
        for (unsigned attempt = 0U; !legacy_ready.has_value() && attempt < 120U; ++attempt) {
            update_audio_events();
            legacy_ready = find_ready();
            if (!legacy_ready.has_value()) SDL_Delay(1U);
        }
        state.x[0] = legacy_ready.has_value() ? kSuccess : 0xEA01U;
        state.x[1] = legacy_ready.value_or(0U);
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

void HorizonKernel::initialize_scheduler(const CpuState& initial_context) {
    if (scheduler_initialized_) return;
    auto main = std::make_shared<KThread>(0U);
    main->context = initial_context;
    main->context.exclusive_monitor.clear();
    main->status = ThreadStatus::Runnable;
    main->priority = 0x2CU;
    main->processor_id = -1;
    main->handle = handles_.insert(main);
    threads_.push_back(main);
    main_thread_ = std::move(main);
    active_thread_.reset();
    scheduler_cursor_ = 0U;
    next_thread_index_ = 1U;
    scheduler_initialized_ = true;
    scheduler_yield_requested_ = false;
}

std::size_t HorizonKernel::run_scheduler(
    Arm64Cpu& cpu, std::size_t max_instructions,
    std::uint64_t stop_pc, std::size_t time_slice) {
    if (!scheduler_initialized_) initialize_scheduler(cpu.state());
    if (time_slice == 0U) time_slice = 750U;

    std::size_t executed_total = 0U;
    std::size_t idle_rounds = 0U;
    while (running_ && executed_total < max_instructions) {
        update_thread_waiters();
        update_audio_events();

        std::shared_ptr<KThread> thread = next_runnable_thread();
        if (!thread) {
            const bool has_blocked_threads = std::any_of(
                threads_.begin(), threads_.end(),
                [](const std::shared_ptr<KThread>& item) {
                    return item->status == ThreadStatus::Sleeping ||
                           item->status == ThreadStatus::WaitingSynchronization ||
                           item->status == ThreadStatus::WaitingLock;
                });
            if (!has_blocked_threads || idle_rounds++ >= 100U) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle_rounds = 0U;

        active_thread_ = thread;
        scheduler_yield_requested_ = false;
        thread->context.exclusive_monitor.clear();
        cpu.state() = thread->context;
        const std::size_t slice = std::min(
            time_slice, max_instructions - executed_total);
        const std::size_t executed = cpu.run(slice, stop_pc);
        executed_total += executed;
        thread->context = cpu.state();
        thread->context.exclusive_monitor.clear();

        if (thread->status == ThreadStatus::Runnable &&
            thread->context.pc == stop_pc) {
            thread->status = ThreadStatus::Terminated;
        }
        active_thread_.reset();
        scheduler_yield_requested_ = false;

        if (executed == 0U && thread->status == ThreadStatus::Runnable)
            break;
    }
    return executed_total;
}

std::shared_ptr<KThread> HorizonKernel::find_thread(Handle handle) const noexcept {
    const auto found = std::find_if(
        threads_.begin(), threads_.end(),
        [handle](const std::shared_ptr<KThread>& thread) {
            return thread->handle == handle &&
                   thread->status != ThreadStatus::Terminated;
        });
    return found == threads_.end() ? nullptr : *found;
}

std::shared_ptr<KThread> HorizonKernel::next_runnable_thread() {
    if (threads_.empty()) return nullptr;
    for (std::size_t attempt = 0U; attempt < threads_.size(); ++attempt) {
        const std::size_t index =
            (scheduler_cursor_ + attempt) % threads_.size();
        const auto& thread = threads_[index];
        if (thread->status == ThreadStatus::Runnable) {
            scheduler_cursor_ = (index + 1U) % threads_.size();
            return thread;
        }
    }
    return nullptr;
}

bool HorizonKernel::any_runnable_thread() const noexcept {
    return std::any_of(threads_.begin(), threads_.end(),
        [](const std::shared_ptr<KThread>& thread) {
            return thread->status == ThreadStatus::Runnable;
        });
}

void HorizonKernel::request_scheduler_yield() noexcept {
    scheduler_yield_requested_ = true;
}

void HorizonKernel::reset_all_exclusive_monitors() noexcept {
    for (const auto& thread : threads_)
        thread->context.exclusive_monitor.clear();
    if (active_thread_)
        active_thread_->context.exclusive_monitor.clear();
}

void HorizonKernel::update_thread_waiters() {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& thread : threads_) {
        if (thread->status == ThreadStatus::Sleeping) {
            if (!thread->has_deadline || now >= thread->wake_deadline) {
                thread->status = ThreadStatus::Runnable;
                thread->has_deadline = false;
                thread->context.x[0] = kSuccess;
            }
            continue;
        }
        if (thread->status != ThreadStatus::WaitingSynchronization) continue;

        bool woke = false;
        for (std::size_t index = 0U; index < thread->waiting_handles.size(); ++index) {
            const auto object = handles_.get(thread->waiting_handles[index]);
            if (!object) {
                thread->context.x[0] = kInvalidHandle;
                thread->context.x[1] = 0U;
                woke = true;
                break;
            }
            if (object->kind() == KernelObjectKind::Event &&
                std::static_pointer_cast<KEvent>(object)->signaled) {
                thread->context.x[0] = kSuccess;
                thread->context.x[1] = index;
                woke = true;
                break;
            }
        }
        if (!woke && thread->has_deadline && now >= thread->wake_deadline) {
            thread->context.x[0] = 0xEA01U;
            thread->context.x[1] = 0U;
            woke = true;
        }
        if (woke) {
            thread->waiting_handles.clear();
            thread->has_deadline = false;
            thread->status = ThreadStatus::Runnable;
        }
    }
}

void HorizonKernel::svc_create_thread(CpuState& state) {
    const std::uint64_t entry_pc = state.x[1];
    const std::uint64_t argument = state.x[2];
    const std::uint64_t stack_top = state.x[3];
    const std::int32_t priority = static_cast<std::int32_t>(state.x[4]);
    const std::int32_t processor_id = static_cast<std::int32_t>(state.x[5]);
    if (!scheduler_initialized_ || entry_pc == 0U ||
        stack_top < 16U || (stack_top & 0xFU) != 0U ||
        priority < 0 || priority > 0x3F ||
        (processor_id < -1 || processor_id > 3) ||
        next_thread_index_ >= 0x10000U) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0U;
        return;
    }

    try {
        (void)memory_.view(entry_pc, 4U, Permission::Execute);
        (void)memory_.view(stack_top - 16U, 16U, Permission::Write);
        const std::uint64_t tls_address =
            0x10000000ULL + static_cast<std::uint64_t>(next_thread_index_) * 0x1000ULL;
        memory_.map_zeroed(tls_address, 0x1000U,
                           Permission::Read | Permission::Write);

        auto thread = std::make_shared<KThread>(next_thread_index_++);
        thread->context = CpuState{};
        thread->context.x[0] = argument;
        thread->context.sp = stack_top;
        thread->context.pc = entry_pc;
        thread->context.tpidrro_el0 = tls_address;
        thread->context.exclusive_monitor.clear();
        thread->status = ThreadStatus::Suspended;
        thread->priority = priority;
        thread->processor_id = processor_id;
        thread->handle = handles_.insert(thread);
        threads_.push_back(thread);
        state.x[0] = kSuccess;
        state.x[1] = thread->handle;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
        state.x[1] = 0U;
    }
}

void HorizonKernel::svc_start_thread(CpuState& state) {
    const Handle handle = static_cast<Handle>(state.x[0]);
    const auto thread = find_thread(handle);
    if (!thread || thread->status != ThreadStatus::Suspended) {
        state.x[0] = kInvalidHandle;
        return;
    }
    thread->status = ThreadStatus::Runnable;
    state.x[0] = kSuccess;
}

void HorizonKernel::svc_exit_thread(CpuState& state) noexcept {
    if (!active_thread_) {
        state.x[0] = kInvalidHandle;
        return;
    }
    active_thread_->status = ThreadStatus::Terminated;
    active_thread_->waiting_handles.clear();
    active_thread_->has_deadline = false;
    state.x[0] = kSuccess;
    state.exclusive_monitor.clear();
    request_scheduler_yield();
}

void HorizonKernel::svc_sleep_thread(CpuState& state) {
    if (!active_thread_) {
        state.x[0] = kInvalidArgument;
        return;
    }
    const std::int64_t nanoseconds =
        static_cast<std::int64_t>(state.x[0]);
    if (nanoseconds <= 0) {
        state.x[0] = kSuccess;
        request_scheduler_yield();
        return;
    }
    active_thread_->status = ThreadStatus::Sleeping;
    active_thread_->wake_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(nanoseconds);
    active_thread_->has_deadline = true;
    state.x[0] = kSuccess;
    state.exclusive_monitor.clear();
    request_scheduler_yield();
}

void HorizonKernel::svc_arbitrate_lock(CpuState& state) {
    const Handle owner_handle = static_cast<Handle>(state.x[0]);
    const std::uint64_t mutex_address = state.x[1];
    const Handle requester_handle = static_cast<Handle>(state.x[2]);
    if ((mutex_address & 3U) != 0U) {
        state.x[0] = kInvalidArgument;
        return;
    }
    try {
        const std::uint32_t mutex_value =
            memory_.read_u32(mutex_address, Permission::Read);
        const std::uint32_t owner_value = mutex_value & 0x3FFFFFFFU;
        if (owner_value != owner_handle) {
            state.x[0] = kSuccess;
            return;
        }
        const auto requester = find_thread(requester_handle);
        if (!requester || requester->status == ThreadStatus::Terminated) {
            state.x[0] = kInvalidHandle;
            return;
        }
        requester->status = ThreadStatus::WaitingLock;
        requester->waiting_mutex = mutex_address;
        requester->context.exclusive_monitor.clear();
        state.x[0] = kSuccess;
        state.exclusive_monitor.clear();
        if (active_thread_ == requester) request_scheduler_yield();
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

void HorizonKernel::svc_arbitrate_unlock(CpuState& state) {
    const std::uint64_t mutex_address = state.x[0];
    if ((mutex_address & 3U) != 0U) {
        state.x[0] = kInvalidArgument;
        return;
    }
    try {
        (void)memory_.read_u32(mutex_address, Permission::Read);
        std::shared_ptr<KThread> next_owner;
        std::size_t waiting_count = 0U;
        for (const auto& thread : threads_) {
            if (thread->status == ThreadStatus::WaitingLock &&
                thread->waiting_mutex == mutex_address) {
                ++waiting_count;
                if (!next_owner) next_owner = thread;
            }
        }
        if (!next_owner) {
            memory_.write_u32(mutex_address, 0U);
        } else {
            next_owner->status = ThreadStatus::Runnable;
            next_owner->waiting_mutex = 0U;
            const bool has_more_waiters = waiting_count > 1U;
            const std::uint32_t new_value = next_owner->handle |
                (has_more_waiters ? 0x40000000U : 0U);
            memory_.write_u32(mutex_address, new_value);
        }
        reset_all_exclusive_monitors();
        state.exclusive_monitor.clear();
        state.x[0] = kSuccess;
    } catch (const std::exception&) {
        state.x[0] = kInvalidArgument;
    }
}

std::size_t HorizonKernel::scheduled_thread_count() const noexcept {
    return threads_.size();
}

Handle HorizonKernel::current_thread_handle() const noexcept {
    return active_thread_ ? active_thread_->handle :
        (main_thread_ ? main_thread_->handle : 0U);
}

bool HorizonKernel::yield_requested() const noexcept {
    return scheduler_yield_requested_;
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

std::span<const std::uint8_t> HorizonKernel::display_pixels() const noexcept {
    return display_layer_ ? std::span<const std::uint8_t>(
        display_layer_->presented_rgba) : std::span<const std::uint8_t>{};
}

std::uint64_t HorizonKernel::display_frame_number() const noexcept {
    return display_layer_ ? display_layer_->frame_number : 0U;
}

void HorizonKernel::signal_vsync() noexcept {
    if (display_layer_) display_layer_->vsync_event->signaled = true;
}

SvcHandler::SvcHandler(HorizonKernel& kernel) noexcept : kernel_(kernel) {}

void SvcHandler::dispatch(std::uint16_t immediate, CpuState& state) {
    switch (immediate) {
    case 0x01: kernel_.svc_set_heap_size(state); break;
    case 0x07: kernel_.svc_exit_process(state); break;
    case 0x08: kernel_.svc_create_thread(state); break;
    case 0x09: kernel_.svc_close_handle(state); break;
    case 0x0A: kernel_.svc_start_thread(state); break;
    case 0x0B: kernel_.svc_exit_thread(state); break;
    case 0x0C: kernel_.svc_sleep_thread(state); break;
    case 0x14: kernel_.svc_map_shared_memory(state); break;
    case 0x15: kernel_.svc_unmap_shared_memory(state); break;
    case 0x17: kernel_.svc_reset_signal(state); break;
    case 0x18: kernel_.svc_wait_synchronization(state); break;
    case 0x1A: kernel_.svc_arbitrate_lock(state); break;
    case 0x1B: kernel_.svc_arbitrate_unlock(state); break;
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
bool SvcHandler::yield_requested() const noexcept {
    return kernel_.yield_requested();
}

} // namespace horizon
