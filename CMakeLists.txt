cmake_minimum_required(VERSION 3.20)
project(HorizonLite VERSION 0.2.0 LANGUAGES CXX)

find_package(SDL2 CONFIG QUIET)
if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
endif()

add_executable(horizonlite
    src/main.cpp
    src/memory.cpp
    src/nro_loader.cpp
    src/cpu.cpp
    src/kernel.cpp
    src/audio.cpp
    src/romfs.cpp
    src/display.cpp
    src/gpu.cpp
)

target_include_directories(horizonlite PRIVATE include)
target_compile_features(horizonlite PRIVATE cxx_std_20)

if(TARGET SDL2::SDL2)
    target_link_libraries(horizonlite PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(horizonlite PRIVATE SDL2::SDL2-static)
elseif(TARGET PkgConfig::SDL2)
    target_link_libraries(horizonlite PRIVATE PkgConfig::SDL2)
else()
    message(FATAL_ERROR "SDL2 was found but no usable SDL2 target is available")
endif()

if(MSVC)
    target_compile_options(horizonlite PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(horizonlite_cpu_test
    tests/cpu_memory_test.cpp
    src/memory.cpp
    src/cpu.cpp
    src/kernel.cpp
    src/audio.cpp
    src/display.cpp
)
target_include_directories(horizonlite_cpu_test PRIVATE include)
target_compile_features(horizonlite_cpu_test PRIVATE cxx_std_20)

if(TARGET SDL2::SDL2)
    target_link_libraries(horizonlite_cpu_test PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(horizonlite_cpu_test PRIVATE SDL2::SDL2-static)
elseif(TARGET PkgConfig::SDL2)
    target_link_libraries(horizonlite_cpu_test PRIVATE PkgConfig::SDL2)
else()
    message(FATAL_ERROR "SDL2 target is required by horizonlite_cpu_test")
endif()

if(MSVC)
    target_compile_options(horizonlite_cpu_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_cpu_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(horizonlite_thread_atomic_fpu_test
    tests/thread_atomic_fpu_test.cpp
    src/memory.cpp
    src/cpu.cpp
    src/kernel.cpp
    src/audio.cpp
    src/display.cpp
)
target_include_directories(horizonlite_thread_atomic_fpu_test PRIVATE include)
target_compile_features(horizonlite_thread_atomic_fpu_test PRIVATE cxx_std_20)
if(TARGET SDL2::SDL2)
    target_link_libraries(horizonlite_thread_atomic_fpu_test PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(horizonlite_thread_atomic_fpu_test PRIVATE SDL2::SDL2-static)
elseif(TARGET PkgConfig::SDL2)
    target_link_libraries(horizonlite_thread_atomic_fpu_test PRIVATE PkgConfig::SDL2)
else()
    message(FATAL_ERROR "SDL2 target is required by horizonlite_thread_atomic_fpu_test")
endif()
if(MSVC)
    target_compile_options(horizonlite_thread_atomic_fpu_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_thread_atomic_fpu_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(horizonlite_romfs_test
    tests/romfs_asset_test.cpp
    src/romfs.cpp
    src/nro_loader.cpp
    src/memory.cpp
)
target_include_directories(horizonlite_romfs_test PRIVATE include)
target_compile_features(horizonlite_romfs_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(horizonlite_romfs_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_romfs_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(horizonlite_display_gpu_test
    tests/display_gpu_test.cpp
    src/display.cpp
    src/memory.cpp
)
target_include_directories(horizonlite_display_gpu_test PRIVATE include)
target_compile_features(horizonlite_display_gpu_test PRIVATE cxx_std_20)
if(TARGET SDL2::SDL2)
    target_link_libraries(horizonlite_display_gpu_test PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(horizonlite_display_gpu_test PRIVATE SDL2::SDL2-static)
elseif(TARGET PkgConfig::SDL2)
    target_link_libraries(horizonlite_display_gpu_test PRIVATE PkgConfig::SDL2)
endif()
if(MSVC)
    target_compile_options(horizonlite_display_gpu_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_display_gpu_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME horizonlite_headless_self_test COMMAND horizonlite --headless)
add_test(NAME horizonlite_cpu_memory_test COMMAND horizonlite_cpu_test)
add_test(NAME horizonlite_thread_atomic_fpu_test COMMAND horizonlite_thread_atomic_fpu_test)
add_test(NAME horizonlite_romfs_asset_test COMMAND horizonlite_romfs_test)
add_test(NAME horizonlite_display_gpu_test COMMAND horizonlite_display_gpu_test)
add_test(NAME horizonlite_save_data_relaunch_test COMMAND horizonlite --headless)
set_tests_properties(horizonlite_save_data_relaunch_test PROPERTIES DEPENDS horizonlite_headless_self_test)

add_executable(horizonlite_loader_config_test tests/loader_config_test.cpp)
target_include_directories(horizonlite_loader_config_test PRIVATE include)
target_compile_features(horizonlite_loader_config_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(horizonlite_loader_config_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_loader_config_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME horizonlite_loader_config_test COMMAND horizonlite_loader_config_test)

add_executable(horizonlite_gpu_gpfifo_test
    tests/gpu_gpfifo_test.cpp
    src/gpu.cpp
    src/memory.cpp
)
target_include_directories(horizonlite_gpu_gpfifo_test PRIVATE include)
target_compile_features(horizonlite_gpu_gpfifo_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(horizonlite_gpu_gpfifo_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite_gpu_gpfifo_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME horizonlite_gpu_gpfifo_test COMMAND horizonlite_gpu_gpfifo_test)
