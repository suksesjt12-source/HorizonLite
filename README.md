# HorizonLite — Stage 2

HorizonLite is an educational C++20 research prototype for a small subset of the Nintendo Switch NRO format, AArch64 execution, selected Horizon-style system calls, and a linear framebuffer presented through SDL2.

**This is not a Nintendo Switch emulator.** It does not implement the Horizon OS kernel, the full AArch64 ISA, required system services, GPU command processing, NSP/XCI packages, content decryption, or commercial-game compatibility.

## Stage 2 features

- 64-bit virtual address space with read/write/execute permissions and little-endian 32-/64-bit memory access.
- Selected AArch64 instructions: MOV, MOVZ, MOVK, ADD, SUB, ADDS, SUBS/CMP, STR/LDR unsigned offset (W and X forms), B, B.cond (including B.NE), RET, and SVC.
- HorizonKernel / SvcHandler HLE dispatch for SVC 0x01, 0x07, 0x26, and 0x27.
- A 1280 x 720 RGBA byte-order framebuffer at virtual address 0x40000000 (3,686,400 bytes).
- SDL2 window and streaming texture. The window loop targets 60 updates per second and exits when the window is closed or Escape is pressed.
- Built-in raw AArch64 self-test when no NRO path is supplied. It requests a heap, prints a debug string, paints every framebuffer pixel with a changing RGBA pattern using STR W, then calls svcExitProcess.
- Headless mode runs the same instruction payload without creating an SDL window.
- CTest coverage for the payload and 32-/64-bit load/store plus conditional-branch behavior.

## Build requirements

- CMake 3.20 or newer
- C++20 compiler (GCC 11+, Clang 13+, or MSVC with C++20 support)
- SDL2 development package

### Debian / Ubuntu

    sudo apt-get update
    sudo apt-get install -y cmake g++ libsdl2-dev
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel

### Run the built-in self-test

    ./build/horizonlite

An SDL2 window opens at 1280x720. The terminal prints the self-test debug string. The program draws a generated pixel gradient, then remains open showing the final framebuffer until the user closes the window or presses Escape.

Run without a window:

    ./build/horizonlite --headless

Headless mode runs the same instruction payload to completion and validates the first and final framebuffer pixels plus an FNV-1a64 framebuffer checksum.

### Load an NRO image

    ./build/horizonlite path/to/homebrew.nro

The current NRO loader accepts only uncompressed NRO images and the interpreter only executes the supported instruction subset. A real homebrew program generally needs process startup, relocation, TLS, exception handling, additional SVCs, IPC services, filesystem services, and other Horizon OS facilities outside this prototype.

## Implemented SVC subset

| SVC | HLE behavior |
| --- | --- |
| 0x01 svcSetHeapSize | Reads requested size from X1, maps zeroed read/write memory at 0x50000000, returns result code 0 in X0 and base address in X1. Prototype limit: 256 MiB. |
| 0x07 svcExitProcess | Sets X0 to success and stops guest instruction execution. The SDL window continues displaying the final framebuffer until closed. |
| 0x26 svcBreak | Prints X0-X30, SP, and PC to stderr, then stops guest instruction execution. |
| 0x27 svcOutputDebugString | Reads X1 bytes from virtual address X0 and writes them to the host terminal. Debug strings are limited to 1 MiB. |

Unknown SVCs return a prototype error value in X0 and print a diagnostic. These handlers are educational HLE approximations, not a complete or ABI-compatible Horizon kernel.

## Framebuffer details

- Base: 0x40000000
- Dimensions: 1280 x 720
- Bytes per pixel: 4
- Size: 1280 * 720 * 4 = 3,686,400 bytes
- Byte layout: R, G, B, A
- SDL2 texture: SDL_PIXELFORMAT_RGBA32, streaming access

## Project structure

    HorizonLite/
    ├── CMakeLists.txt
    ├── include/horizon/
    │   ├── cpu.hpp
    │   ├── display.hpp
    │   ├── kernel.hpp
    │   ├── memory.hpp
    │   └── nro_loader.hpp
    ├── src/
    │   ├── cpu.cpp
    │   ├── display.cpp
    │   ├── kernel.cpp
    │   ├── main.cpp
    │   ├── memory.cpp
    │   └── nro_loader.cpp
    ├── tests/
    │   └── cpu_memory_test.cpp
    └── .github/workflows/build.yml

## References

- AArch64 instruction encoding: https://csci.viu.ca/~kabirh/courses/csci261/resources/ARM8-A64-InstructionSetEncoding.pdf
- MOVK instruction: https://www.scs.stanford.edu/~zyedidia/arm64/movk.html
- SDL2 texture creation: https://wiki.libsdl.org/SDL2/SDL_CreateTexture
- SDL2 texture locking: https://wiki.libsdl.org/SDL2/SDL_LockTexture
