# HorizonLite — Stage 4: NRO RomFS + fsp-srv storage

HorizonLite is an educational C++20 HLE research prototype for a limited subset of AArch64 execution, Horizon-style SVCs, HIPC/CMIF service dispatch, HID shared memory, the NRO ASET asset section, a subset of Switch RomFS, and an SDL2 linear framebuffer.

It is **not a complete Nintendo Switch emulator**. It does not implement the full Horizon kernel, all AArch64 instructions, GPU command processing, complete filesystem services, NSP/XCI loading, NCA decryption, process metadata, or commercial-game compatibility. The IPC framing, NRO boot setup, and service behavior are intentionally bounded to the built-in guest demonstration and should not be treated as full retail ABI compatibility.

## Stage 4 features

### NRO ASET and embedded RomFS

- Checks for the optional ASET section at file offset `NroHeader.size`.
- Validates the ASET magic `0x54455341` ("ASET") and version 0.
- Reads the RomFS asset section's 64-bit offset and size at ASET offsets `0x28` and `0x30`. Asset offsets are relative to the start of the ASET header.
- Bounds-checks the section against the NRO file and copies the RomFS image into `NroImage::romfs`.
- The current extractor accepts a direct RomFS image with a 0x50-byte RomFS header. It does not unwrap an IVFC image used by other container formats.

The NRO ASET layout is based on the documented optional homebrew assets section in the Switch NRO format. 【turn882866search2】

### RomFsBuilder

- Emits the ten-u64 / 0x50-byte RomFS header.
- Builds directory hash, directory metadata, file hash, and file metadata tables.
- Emits a root directory entry and one root-level file entry, with data offset and file size.
- Places the sample sprite file data at RomFS offset `0x100`.
- Generates a 16x16 RGBA8888 sample sprite.

The table fields and metadata entry layouts follow the structures used by libnx's RomFS driver. 【turn500717view0】

### HIPC / CMIF buffer descriptors

The Stage 3 CMIF codec is extended to parse the HIPC descriptor counts in the command header and extract:

- X descriptors (Pointer Buffer).
- A descriptors (Send Buffer).
- B descriptors (Receive Buffer).
- W descriptors (Send/Receive Exchange Buffer).

The service handler validates descriptor sizes and guest memory ranges before reading from or writing to guest memory. The request raw-data area is read according to the HIPC raw-data word count; CMIF requests use `SFCI` and replies use `SFCO`.

### Filesystem service HLE

The `sm:` service manager now offers `hid` and `fsp-srv`.

| Service | Command | Behavior |
| --- | ---: | --- |
| `IFileSystemProxy` | 1 | `SetCurrentProcess`: accepts the process-id payload and returns success |
| `IFileSystemProxy` | 200 | `OpenDataStorageByCurrentProcess`: returns an `IStorage` Move Handle backed by the active NRO RomFS buffer |
| `IStorage` | 0 | `Read`: validates raw `offset` and `size`, then writes into the first B (Receive Buffer) descriptor |
| `IStorage` | 4 | `GetSize`: returns the RomFS byte size as a u64 raw response parameter |

The Stage 4 `IStorage` object retains the RomFS bytes loaded for the current NRO. For no-argument self-test, the kernel receives the generated RomFS image instead.

### ARM64 memory

The AArch64 interpreter now supports byte memory operations:

- `LDRB Wt, [Xn, #imm]`
- `STRB Wt, [Xn, #imm]`

Existing 32-bit `LDR Wt` / `STR Wt` and 64-bit `LDR Xt` / `STR Xt` operations remain available. Stage 3 logical, shift, TLS, and branch instructions remain present.

## Interactive self-test

Run without an NRO path:

    ./build/horizonlite

The generated guest ARM64 program performs the service sequence itself:

1. Connects to `sm:`.
2. Requests the `hid` service and obtains the HID shared memory.
3. Requests `fsp-srv`.
4. Calls `SetCurrentProcess` and `OpenDataStorageByCurrentProcess`.
5. Calls `IStorage::GetSize` and `IStorage::Read` with a B Receive Buffer descriptor to copy the 16x16 RGBA sprite into guest RAM.
6. Reads HID input and draws the sprite from guest RAM to the 1280x720 framebuffer.

Arrow keys or WASD move the sprite. Z=A, X=B, A=X, S=Y, and Enter=Plus. Press Escape or close the SDL2 window to exit.

Run the guest self-test in CI/headless mode:

    ./build/horizonlite --headless

Headless mode runs a bounded instruction budget and checks that the guest has drawn the expected sprite pixel.

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

The loader accepts only uncompressed NRO files in the current NRO subset. If ASET contains a direct RomFS image, it is exposed to the guest through `fsp-srv` and `IStorage`. The interpreter still supports only a subset of AArch64; ordinary homebrew applications may need additional startup, relocations, exception handling, services, filesystem commands, and graphics interfaces that are not implemented here.

## Project structure

    HorizonLite/
    ├── CMakeLists.txt
    ├── include/horizon/
    │   ├── cpu.hpp
    │   ├── display.hpp
    │   ├── kernel.hpp
    │   ├── memory.hpp
    │   ├── nro_loader.hpp
    │   └── romfs.hpp
    ├── src/
    │   ├── cpu.cpp
    │   ├── display.cpp
    │   ├── kernel.cpp
    │   ├── main.cpp
    │   ├── memory.cpp
    │   ├── nro_loader.cpp
    │   └── romfs.cpp
    ├── tests/
    │   ├── cpu_memory_test.cpp
    │   └── romfs_asset_test.cpp
    └── .github/workflows/build.yml
