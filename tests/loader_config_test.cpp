#include "horizon/loader_config.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        horizon::LoaderConfigValues values;
        values.main_thread_handle = 0x101U;
        values.heap_address = 0x50000000ULL;
        values.heap_size = 0x08000000ULL;
        values.random_seed[0] = 0x0123456789ABCDEFULL;
        values.random_seed[1] = 0xFEDCBA9876543210ULL;

        const auto entries = horizon::make_loader_config_entries(values);
        constexpr std::array<std::uint32_t, 6> expected_keys{
            1U, 3U, 7U, 10U, 14U, 0U
        };
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            if (entries[index].key != expected_keys[index] ||
                entries[index].flags != 0U)
                throw std::runtime_error("LoaderConfig key or flags mismatch");
        }
        if (sizeof(horizon::LoaderConfigEntry) != 24U ||
            entries[0].value[0] != values.main_thread_handle ||
            entries[1].value[0] != values.heap_address ||
            entries[1].value[1] != values.heap_size ||
            entries[2].value[0] != 0U ||
            entries[3].value[0] != 0xFFFF8001ULL ||
            entries[4].value[0] != values.random_seed[0] ||
            entries[4].value[1] != values.random_seed[1])
            throw std::runtime_error("LoaderConfig values or ABI size mismatch");

        const auto encoded = horizon::encode_loader_config(values);
        if (encoded.size() != entries.size() * sizeof(horizon::LoaderConfigEntry))
            throw std::runtime_error("LoaderConfig byte encoding has an invalid size");

        std::cout << "libnx LoaderConfig layout and values test passed\\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LoaderConfig test failed: " << error.what() << '\\n';
        return 1;
    }
}
