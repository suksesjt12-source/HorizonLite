cmake_minimum_required(VERSION 3.20)
project(HorizonLite VERSION 0.1.0 LANGUAGES CXX)

add_executable(horizonlite
    src/main.cpp
    src/memory.cpp
    src/nro_loader.cpp
    src/cpu.cpp
)

target_include_directories(horizonlite PRIVATE include)
target_compile_features(horizonlite PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(horizonlite PRIVATE /W4 /permissive-)
else()
    target_compile_options(horizonlite PRIVATE -Wall -Wextra -Wpedantic)
endif()
