# HorizonLite — Stage 3: IPC + HID

HorizonLite is an educational C++20 research prototype for a limited subset of AArch64 execution, Horizon-style system calls, HIPC/CMIF service dispatch, HID shared memory, and an SDL2 linear framebuffer.

It is **not a complete Nintendo Switch emulator**. It does not implement the full Horizon kernel, all AArch64 instructions, GPU command processing, filesystem/content services, NSP/XCI loading, game decryption, or commercial-game compatibility. The IPC framing and HID data structures documented here are a deliberately reduced HLE model for this prototype; they should not be treated as a drop-in ABI implementation for arbitrary retail software or all versions of libnx.

## Stage 3 features

### AArch64 and TLS

- Main-thread TLS: 4 KiB at guest virtual address `0x10000000`.
- TLS bytes `0x00..0xFF` are used as the IPC command buffer.
- `MRS Xd, TPIDRRO_EL0`.
- Logical immediate/register forms: `AND`, `ORR`, and the related logical register decoder.
- `LSL` / `LSR` immediate aliases through `UBFM`.
- `CBZ`, `CBNZ`, and `BL`.
- Stage 2 instructions remain available: MOV/MOVZ/MOVK, ADD/SUB, flag-setting arithmetic/CMP, STR/LDR W/X, B, B.cond and RET.

### Kernel handles and SVCs

The kernel handle table stores 32-bit handles referencing `KPort`, `KClientSession`, and `KSharedMemory` objects.

| SVC | HLE behavior |
| --- | --- |
| `0x01` | `svcSetHeapSize` |
| `0x07` | `svcExitProcess` |
| `0x09` | `svcCloseHandle` |
| `0x14` | `svcMapSharedMemory`: X0 handle, X1 guest address, X2 size |
| `0x15` | `svcUnmapSharedMemory`: X0 handle, X1 guest address, X2 size |
| `0x1F` | `svcConnectToNamedPort`: reads the null-terminated port name at X1; returns the session handle in X1 |
| `0x21` | `svcSendSyncRequest`: sends the TLS command buffer through the selected client session |
| `0x26` | `svcBreak` |
| `0x27` | `svcOutputDebugString` |

Shared-memory unmapping requires the exact guest base and size used for mapping.

### HIPC / CMIF and HLE services

- The IPC codec parses a HIPC-style command header, the prototype's special-handle descriptor, and CMIF `SFCI` requests.
- Replies use `SFCO` and encode one optional Copy Handle or Move Handle descriptor.
- The service manager `sm:` supports `Initialize` (command 0) and `GetService` (command 1, currently `hid`).
- `IHidServer` supports `CreateAppletResource` (0), `ActivateNpad` (100), `SetSupportedNpadStyleSet` (101), and `SetSupportedNpadIdType` (103).
- `IAppletResource` supports `GetSharedMemoryHandle` (0), returning a Copy Handle to the HID shared-memory object.

The reduced HIPC descriptor format used by this test harness is: two 32-bit HIPC header words at offset 0, a 32-bit special-handle descriptor at offset 8, handle values beginning at offset 12, then the CMIF block aligned to 16 bytes. The low descriptor nibble counts Copy Handles and the next nibble counts Move Handles. CMIF begins with a 32-bit magic, version, command ID and token; raw command data follows. The request magic is `SFCI` (`0x49434653`) and the response magic is `SFCO` (`0x4F434653`). This local layout makes the subset deterministic for the built-in guest test; complete Horizon HIPC descriptor parsing and all domain-object semantics are outside this stage.

### HID shared memory

- Shared-memory object size: `0x40000` bytes.
- Npad area base: offset `0x9A00`.
- Ring-buffer header: 0x20 bytes containing sampling number, buffer count and current entry index.
- `NpadFullKeyState`-style entry at `0x9A20`; sampling number at entry +0 and 64-bit button mask at entry +8.
- `NpadHandheldState`-style entry at `0x9A50`, with the same leading fields.
- The prototype keeps a one-entry ring for each state and increments its sampling number once per SDL2 frame.

Button bits used in this implementation:

| Button | Bit |
| --- | ---: |
| A | 0 |
| B | 1 |
| X | 2 |
| Y | 3 |
| Plus | 10 |
| D-pad Left | 12 |
| D-pad Up | 13 |
| D-pad Right | 14 |
| D-pad Down | 15 |

SDL2 keyboard mapping: Arrow keys or WASD for D-pad; Z=A; X=B; A=X; S=Y; Enter=Plus. As requested, A and S therefore also activate Left and Down respectively when used as D-pad aliases.

## Interactive self-test

Run without an NRO path:

    ./build/horizonlite

The built-in guest instruction stream performs the following operations itself:

1. Reads TLS using `MRS TPIDRRO_EL0`.
2. Calls `svcConnectToNamedPort("sm:")`.
3. Builds a CMIF `GetService("hid")` request in TLS and calls `svcSendSyncRequest`.
4. Requests an `IAppletResource` from `IHidServer`.
5. Requests the HID shared-memory Copy Handle and maps it with `svcMapSharedMemory`.
6. Repeatedly reads the Npad button mask and draws a 32x32 box on the 1280x720 framebuffer. D-pad input moves the box.

Press Escape or close the window to exit.

Run the same end-to-end guest self-test without SDL2:

    ./build/horizonlite --headless

Headless mode runs a bounded number of guest instructions and verifies that the guest drew the colored box.

## Build requirements

- CMake 3.20 or newer
- C++20 compiler (GCC 11+, Clang 13+, or MSVC with C++20 support)
- SDL2 development package

### Debian / Ubuntu

    sudo apt-get update
    sudo apt-get install -y cmake g++ libsdl2-dev
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

### Load an NRO image

    ./build/horizonlite path/to/homebrew.nro

The current loader accepts only uncompressed NRO images. The interpreter still supports only a subset of AArch64; real homebrew programs may require process startup, relocations, exception handling, additional system calls, additional system services, and a closer implementation of Horizon's exact ABI.

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
