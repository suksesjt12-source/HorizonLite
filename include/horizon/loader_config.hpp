#pragma once

#include "horizon/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace horizon {

enum class LoaderConfigKey : std::uint32_t {
    EndOfList = 0U,
    MainThreadHandle = 1U,
    OverrideHeap = 3U,
    AppletType = 7U,
    ProcessHandle = 10U,
    RandomSeed = 14U
};

struct LoaderConfigEntry {
    std::uint32_t key{};
    std::uint32_t flags{};
    std::uint64_t value[2]{};
};

static_assert(sizeof(LoaderConfigEntry) == 24U);
static_assert(alignof(LoaderConfigEntry) == alignof(std::uint64_t));

struct LoaderConfigValues {
    std::uint64_t main_thread_handle{};
    std::uint64_t heap_address{};
    std::uint64_t heap_size{};
    std::uint64_t process_handle{0xFFFF8001ULL};
    std::uint64_t random_seed[2]{};
};

[[nodiscard]] inline std::array<LoaderConfigEntry, 6>
make_loader_config_entries(const LoaderConfigValues& values) {
    return {{
        {static_cast<std::uint32_t>(LoaderConfigKey::MainThreadHandle), 0U,
         {values.main_thread_handle, 0U}},
        {static_cast<std::uint32_t>(LoaderConfigKey::OverrideHeap), 0U,
         {values.heap_address, values.heap_size}},
        {static_cast<std::uint32_t>(LoaderConfigKey::AppletType), 0U,
         {0U, 0U}},
        {static_cast<std::uint32_t>(LoaderConfigKey::ProcessHandle), 0U,
         {values.process_handle, 0U}},
        {static_cast<std::uint32_t>(LoaderConfigKey::RandomSeed), 0U,
         {values.random_seed[0], values.random_seed[1]}},
        {static_cast<std::uint32_t>(LoaderConfigKey::EndOfList), 0U, {0U, 0U}}
    }};
}

[[nodiscard]] inline std::vector<std::uint8_t>
encode_loader_config(const LoaderConfigValues& values) {
    const auto entries = make_loader_config_entries(values);
    std::vector<std::uint8_t> bytes(sizeof(entries), 0U);
    const auto* begin = reinterpret_cast<const std::uint8_t*>(entries.data());
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        bytes[index] = begin[index];
    return bytes;
}

inline void write_loader_config(AddressSpace& memory, std::uint64_t address,
                                const LoaderConfigValues& values) {
    memory.write(address, encode_loader_config(values));
}

[[nodiscard]] inline std::uint64_t make_random_seed(std::random_device& source) {
    return (static_cast<std::uint64_t>(source()) << 32U) |
           static_cast<std::uint64_t>(source());
}

} // namespace horizon
