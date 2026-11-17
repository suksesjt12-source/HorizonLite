# HorizonLite — Stage 6: vi:m, Binder parcels and nvdrv/nvmap

HorizonLite is an educational C++20 HLE research prototype for a bounded AArch64 instruction set, selected Horizon-style SVCs and HIPC/CMIF services, HID input, NRO ASET/RomFS access, SDL2 audio, and a host-rendered display pipeline.

Stage 6 adds a guest-heap-backed framebuffer path through reduced `nvdrv`, `nvmap`, `vi:m`, and `IHOSBinderDriver` services. It removes the old hardcoded framebuffer base from the self-test. It remains a prototype: it is not a complete Nintendo Switch emulator, a Tegra GPU command processor, or a drop-in retail libnx ABI implementation.

## Stage 6 features

### nvdrv and nvmap

The `sm:` service manager recognizes `nvdrv`. The reduced `INvDrvServices` interface supports:

| Command | Behavior |
| --- | --- |
| 0 — Open | Opens `/dev/nvmap`, `/dev/nvhost-ctrl`, or `/dev/nvhost-gpu` and returns a unique file descriptor |
| 1 / 11 — Ioctl / Ioctl2 | Dispatches the supported nvmap ioctl subset |
| 3 — Initialize | Returns success |
| 8 — SetAruid | Returns success |

The `/dev/nvmap` ioctl subset includes:

- `NVMAP_IOC_CREATE` (`0xC0080101`): allocates a kernel-side nvmap record with requested byte size and returns its handle.
- `NVMAP_IOC_ALLOC` (`0xC0200104`): binds the record to a validated writable guest virtual-memory range and records heap mask, flags and alignment.
- `NVMAP_IOC_GET_ID` (`0xC008010E`) and `NVMAP_IOC_FROM_ID` (`0xC0080103`): resolve the HLE handle/id pair.
- `NVMAP_IOC_PARAM` (`0xC0080109`): returns supported size, alignment or flags metadata.

The fd table is per emulator process. The guest virtual address is accepted only when the complete declared nvmap allocation lies in a mapped guest region with read/write permission.

### vi:m and display sessions

The reduced service tree is:

`sm:` → `vi:m` → `IApplicationRootService` → `IApplicationDisplayService`

- `GetDisplayService` creates an application display session.
- `GetRelayService` returns an `IHOSBinderDriver` session.
- `GetSystemDisplayService` and `GetManagerDisplayService` return bounded configuration stubs.
- `OpenDisplay("Default")` returns display ID 0.
- `OpenLayer` returns a small NativeWindow-style Parcel containing Binder ID 1 and the `dispdrv` interface token in UTF-16LE.
- `SetLayerScalingMode` returns success.
- `GetDisplayVsyncEvent` returns a Copy Handle to the layer's KEvent.

### IHOSBinderDriver transactions

The Binder HLE accepts the project-local Parcel envelope and a restricted subset of `android.gui.IGraphicBufferProducer` transactions:

| Transaction | Behavior |
| --- | --- |
| 10 — CONNECT | Returns the 1280×720 QueueBufferOutput dimensions and zero transform/pending-buffer values |
| 14 — SET_PREALLOCATED_BUFFER | Resolves an nvmap handle or ID and attaches an offset to one of two layer slots |
| 3 — DEQUEUE_BUFFER | Selects the next configured slot |
| 1 — REQUEST_BUFFER | Returns reduced GraphicBuffer metadata for the requested slot |
| 7 — QUEUE_BUFFER | Validates the slot allocation, converts its pixels, publishes the new frame, and signals release/Vsync events |

Binder commands 1 and 2 implement AdjustRefcount and GetNativeHandle. The latter returns a Copy Handle to the layer release event.

The Parcel helper recognizes the prototype's `PARC` envelope and a limited String16 header form. This is not full Android Binder driver behavior, object-offset parsing, flat Binder object relocation, or complete libnx Parcel compatibility.

### Tegra block-linear framebuffer conversion

`UntileTegraBlockLinear` converts RGBA8888 pixels using 64-byte by 8-row GOB addressing, block-height GOB groups (the default is 16), and the 16Bx2 in-GOB byte ordering. It also supports direct linear RGBA input. If the declared source allocation is too small for the requested padded tiled footprint, the current display path uses its documented linear fallback.

The converted surface is uploaded with `SDL_UpdateTexture`; SDL2 is the host presentation backend. This does not execute Tegra GPU command buffers, shaders, synchronization fences, or render targets.

## Guest self-test

Run the built-in guest:

```sh
./build/horizonlite
```

The self-test follows this sequence:

1. Requests a 12 MiB guest heap using `svcSetHeapSize`.
2. Connects to `nvdrv`, opens `/dev/nvmap`, creates an 8 MiB buffer, and allocates it at an address within that guest heap.
3. Connects to `vi:m`, opens the `Default` display and layer, and obtains the relay Binder session, Vsync event and release event handle.
4. Sends CONNECT, registers two preallocated buffer slots, and requests slot metadata.
5. Repeats DEQUEUE → draws the RomFS sprite and processes HID/audio → QUEUE. The two frame slots share the guest allocation at distinct offsets.
6. The host presents the queued frame through SDL2. The self-test checks the pixel in the presented surface and requires at least one completed QueueBuffer frame.

The interactive host loop is paced at approximately 60 Hz. The HLE Vsync event is signaled after QueueBuffer and again after the host render iteration. It is a simplified event model, not a hardware display controller's exact vblank timing.

The retained keyboard mapping is: Arrow keys/WASD for movement; Z=A, X=B, A=X, S=Y, Enter=Plus. Escape closes the host window.

Headless CI mode:

```sh
./build/horizonlite --headless
```

## Tests

The CTest suite covers:

- Full built-in headless guest path, including heap allocation, nvdrv, vi:m, Binder QueueBuffer, RomFS sprite rendering and audio.
- AArch64 integer/memory operations and SDL2 queued audio behavior.
- NRO ASET/RomFS parsing and asset building.
- Tegra GOB untile, linear framebuffer fallback, and invalid block-height rejection.

Run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Scope and compatibility limits

This stage is a host-backed HLE demonstration, not an executable replacement for the real NVIDIA Tegra driver. It does not submit GPU command buffers or implement NVHOST syncpoints, GPU channels, address-space bindings, GPU fences, full GraphicBuffer formats/metadata, every Binder transaction, full HIPC ABI variants, or all startup/runtime requirements used by typical retail homebrew. The 16Bx2 block-linear converter is a standalone memory-layout path; the self-test uses linear pixels. An ordinary `.nro` may still require additional AArch64 instructions, relocations, kernel services, display formats and ABI structures before it runs.

## Build requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC 11+, Clang 13+, or MSVC with C++20 support)
- SDL2 development package

Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y cmake g++ libsdl2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Project structure

```text
HorizonLite/
├── CMakeLists.txt
├── include/horizon/
│   ├── audio.hpp
│   ├── cpu.hpp
│   ├── display.hpp
│   ├── kernel.hpp
│   ├── memory.hpp
│   ├── nro_loader.hpp
│   └── romfs.hpp
├── src/
│   ├── audio.cpp
│   ├── cpu.cpp
│   ├── display.cpp
│   ├── kernel.cpp
│   ├── main.cpp
│   ├── memory.cpp
│   ├── nro_loader.cpp
│   └── romfs.cpp
├── tests/
│   ├── cpu_memory_test.cpp
│   ├── display_gpu_test.cpp
│   └── romfs_asset_test.cpp
└── .github/workflows/build.yml
```
