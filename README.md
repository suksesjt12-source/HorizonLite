# HorizonLite — Stage 9: libnx startup, appletOE and time services

HorizonLite is an educational C++20 HLE research prototype for a bounded AArch64 instruction set, selected Horizon-style SVCs and HIPC/CMIF services, HID input, NRO ASET/RomFS access, SDL2 audio, and a host-rendered display pipeline.

Stage 7 adds a deterministic cooperative thread scheduler, per-thread CPU contexts and TLS, reduced Horizon thread and mutex SVCs, an ARM64 exclusive monitor, and scalar single/double floating-point instructions. The built-in guest creates a worker thread which uses exclusive atomic instructions, kernel mutex arbitration and floating-point arithmetic to update shared sprite motion while the main thread processes HID/audio and presents frames through the Stage 6 `vi:m`/Binder/`nvdrv` path.

This is an HLE prototype, not a complete Nintendo Switch emulator or a complete retail Horizon/libnx ABI implementation.

## Stage 7: threads and scheduling

### KThread and CPU context

Each `KThread` is a kernel object stored in `HandleTable`. Its saved context includes:

- General purpose registers X0–X30
- SP, PC and PSTATE
- 32 64-bit backing registers for V0–V31 (S/D scalar views use the low 32/64 bits)
- TPIDRRO_EL0 / per-thread TLS address
- Exclusive monitor state

The main thread is registered when the scheduler is first entered. New threads receive TLS at:

`0x10000000 + thread_index * 0x1000`

Their initial register state is X0 = argument, SP = supplied stack top, PC = supplied entry point, and the status is Suspended until started.

### Cooperative round-robin scheduler

`HorizonKernel::run_scheduler` runs each Runnable thread for a deterministic instruction budget, defaulting to 750 instructions, then saves its complete context and rotates to the next runnable thread. A blocking SVC requests an immediate yield, so the active slice ends at the SVC boundary.

The scheduler processes sleep deadlines and event waiters between slices. It has no host thread per guest thread; guest concurrency is deterministic cooperative interleaving on the emulator CPU.

### Thread and synchronization SVC subset

| SVC | Implementation |
| --- | --- |
| `0x08 CreateThread` | Validates entry/stack, allocates a KThread and 4 KiB TLS, returns handle in X1 |
| `0x0A StartThread` | Transitions Suspended to Runnable |
| `0x0B ExitThread` | Terminates the active thread and yields |
| `0x0C SleepThread` | Zero duration yields; positive nanoseconds suspend until the wake deadline |
| `0x18 WaitSynchronization` | Returns a signaled event or blocks cooperatively with an optional timeout |
| `0x1A ArbitrateLock` | Places the requesting thread into WaitingLock when the mutex still belongs to the specified owner |
| `0x1B ArbitrateUnlock` | Wakes one waiter, transfers the owner handle and maintains the waiter bit |

The SVC model implements the project’s guest mutex convention and a simplified waiter queue. It is not a byte-for-byte replacement for every Horizon kernel arbitration corner case.

### ARM64 exclusive monitor and atomic operations

The CPU context carries an `ExclusiveMonitor` reservation with valid/address/size fields. It is set by:

- `LDXR Wt, [Xn]` and `LDAXR Wt, [Xn]`
- `LDXR Xt, [Xn]` and `LDAXR Xt, [Xn]`

And consumed by:

- `STXR Ws, Wt, [Xn]` and `STLXR Ws, Wt, [Xn]`
- `STXR Ws, Xt, [Xn]` and `STLXR Ws, Xt, [Xn]`

A successful store-conditional writes status 0; a failed reservation writes status 1 without writing memory. `CLREX`, overlapping ordinary writes and context switches clear reservations. This is a single-emulated-CPU local monitor; it does not model cache-coherency traffic or multiple hardware cores.

### Scalar FPU subset

The CPU stores V0–V31 as 64-bit values and implements scalar 32-bit single and 64-bit double variants for:

- `FMOV` GPR↔S/D register transfers and FP immediate
- `FADD`, `FSUB`, `FMUL`, `FDIV`
- `FCMP`, including NZCV updates for equal, less-than, greater-than and unordered comparisons
- `SCVTF` signed integer to single/double
- `FCVTZS` single/double to signed integer

This is a focused scalar subset. It does not implement vector lane operations, Advanced SIMD, FP exception traps, all rounding modes, FP load/store families, or the complete FP/SIMD architectural state such as FPSR/FPCR behavior.

## Stage 7 guest self-test

The built-in guest runs the Stage 6 display setup, then:

1. Requests a guest heap and stores a shared mutex/position block inside it.
2. Creates and starts a worker thread with a separate stack and TLS.
3. Both main and worker acquire the shared mutex using `LDAXR`/`STLXR`, and use ArbitrateLock/ArbitrateUnlock when contended.
4. The worker converts an integer position to FP, multiplies/adds a scalar FP value and converts the result back to integer using `SCVTF`, `FMUL`, `FADD` and `FCVTZS`.
5. The main thread reads the shared position, handles input and audio, draws the RomFS sprite, then queues a frame through Binder `QUEUE_BUFFER`.

Headless mode checks that at least one frame was queued, the moving sprite colour appears somewhere in the presented frame, and the worker changed the shared position from its starting value.

Run:

```sh
./build/horizonlite --headless
```

Interactive mode:

```sh
./build/horizonlite
```

Keyboard mapping: Arrow keys/WASD for movement; Z=A, X=B, A=X, S=Y, Enter=Plus. Escape closes the host window.



## Stage 8: Account, System Settings and persistent save data

Stage 8 adds three user-facing service families to the existing HIPC/CMIF dispatcher:

- `acc:u0`: one deterministic local profile (UID low `0x0000000000000001`, high `0x1122334455667788`, nickname `Player1`), user count/existence/list/last-opened queries, profile session creation, and `IProfile::Get/GetBase`.
- `set:sys`: 0x100-byte firmware-version output (18.1.0 / NX / NintendoSDK Firmware for NX 18.1.0), BasicBlack color set, and device nickname.
- `set`: language code `en-US` and USA region compatibility responses.
- `fsp-srv` command 51 opens a per-user/per-program save-data sandbox under `./save_data/<uid-hex>/<program-id-hex>/`. The host directory is created on demand.
- `IFileSystem`: CreateFile, DeleteFile, CreateDirectory, GetEntryType, OpenFile and Commit.
- `IFile`: Read, Write, Flush, SetSize and GetSize backed by host `std::fstream` and `std::filesystem`.

Guest paths are normalized and canonicalized before access. Absolute guest paths are treated as paths relative to the save root; parent traversal and symlink escapes are rejected. The implementation is a deliberately reduced HLE contract, not a drop-in implementation of every retail Horizon filesystem ABI detail.

The built-in AArch64 guest now obtains its profile UID from `acc:u0`, fetches firmware data from `set:sys`, opens its save filesystem through `fsp-srv`, restores the two 32-bit sprite coordinates from `/pos.bin`, and writes the updated 8-byte state plus commits when HID X is pressed (keyboard A in the SDL mapping). The headless test injects that button to exercise the write path. Save files are relative to the emulator process working directory.

The CTest suite includes a relaunch test that runs the headless guest again after the first run has created the persistent save file.

Build and test remain:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

**Scope:** this stage provides one built-in profile and fixed compatibility settings. The filesystem command layer implements the specified subset and basic path sandboxing; it does not yet model all save-data metadata, journaling, multi-user account management, file timestamps, rights checks, or the full Horizon result-code space. The emulator is still a host-rendered HLE prototype and this desktop SDL2 workflow does not produce an Android APK.

## Stage 9: Finalization Target 1 — standard 2D NRO startup

Stage 9 adds a reduced libnx-compatible process startup contract and the service calls commonly used by basic 2D homebrew:

- `LoaderConfig` entries use the 24-byte `{u32 key, u32 flags, u64 value[2]}` layout for MainThreadHandle, OverrideHeap, AppletType, ProcessHandle, RandomSeed and EndOfList. The entry pointer is supplied in X0 and X1 is initialized to `UINT64_MAX`.
- A 1 MiB main-thread stack, a 128 MiB preallocated guest heap and a dedicated virtual LoaderConfig page are mapped before guest execution. `svcSetHeapSize` may claim a size up to the preallocated heap capacity.
- The NRO loader treats section offsets as virtual offsets relative to the program image, reads section bytes after the 0x80-byte NRO header, relocates all three segments at a configurable load base, zero-initializes the BSS range, and extracts the optional ASET RomFS.
- `appletOE` exposes IApplicationProxy, ICommonStateGetter, ISelfController, IWindowController, IAudioController, IDisplayController and IApplicationFunctions. The initial FocusStateChanged message is queued, the applet event starts signaled, and an empty message queue returns `MakeResult(128, 3) = 0x680`.
- `time:u`, `time:s` and `time:a` expose system clock, steady clock and time-zone sessions. The timezone implementation returns UTC location data and converts POSIX timestamps into the 0x20-byte CalendarTime + CalendarAdditionalInfo result layout using `gmtime`.
- Optional `apm`, `psm`, `bpc`, `spl:` and `hid:dbg` requests are routed to a logging GenericStubService. The dispatcher records unique command IDs per service.
- Closing the SDL2 window queues AppletMessage_ExitRequest and signals the applet event before allowing a bounded guest cleanup slice.

Run an external NRO with:

```sh
./build/horizonlite path/to/homebrew.nro
./build/horizonlite path/to/homebrew.nro --headless
```

The built-in headless startup test now checks the LoaderConfig keys/values, applet focus and initial/no-message behavior, and the UTC calendar conversion. A separate LoaderConfig test validates the 24-byte entry ABI. The executable prints guest instruction totals and the service/command IDs observed in the IPC dispatcher.

**Compatibility boundary:** this is still a bounded AArch64 interpreter and HLE prototype. It is not a complete Switch emulator and does not promise that arbitrary third-party NROs will run. Standard homebrew can still depend on unsupported AArch64 instructions, relocations, TLS/runtime setup, IPC descriptor forms, GPU/audio behavior, filesystem commands or services. The optional GenericStubService prevents an unimplemented optional service request from failing immediately; it does not implement the requested service's functionality. CI validates the built-in guest and loader structures, not compatibility with a broad external NRO corpus.

## Retained Stage 6 display and GPU-memory HLE

Stage 6 functionality remains available:

- `nvdrv` Open/Initialize/SetAruid and reduced Ioctl/Ioctl2 routing for `/dev/nvmap`, `/dev/nvhost-ctrl` and `/dev/nvhost-gpu`
- nvmap CREATE, ALLOC, GET_ID, FROM_ID and PARAM records, including validated guest virtual ranges
- `vi:m`, Application Root/Display Service, Binder session and NativeWindow-style Parcel subset
- Binder CONNECT, SET_PREALLOCATED_BUFFER, DEQUEUE_BUFFER, REQUEST_BUFFER and QUEUE_BUFFER transactions
- Tegra 16Bx2 GOB block-linear RGBA untile helper and linear fallback, followed by SDL2 `SDL_UpdateTexture`

The display path is host-backed HLE, not Tegra GPU command execution. It does not implement NVHOST syncpoints/channels, GPU command buffers, shaders, GPU fences, or all Android Binder object relocation and Parcel semantics.

## Tests

The CTest suite contains seven targets:

1. `horizonlite_headless_self_test`: built-in guest, account/settings/save data, LoaderConfig, appletOE/time:u, nvdrv/vi:m Binder display, worker thread and audio initialization
2. `horizonlite_cpu_memory_test`: ARM64 integer/memory and SDL2 audio tests
3. `horizonlite_thread_atomic_fpu_test`: exclusive load/store, reservation invalidation, CLREX and scalar FP
4. `horizonlite_romfs_asset_test`: NRO ASET/RomFS parsing, relocated section loading and zero-filled BSS
5. `horizonlite_display_gpu_test`: Tegra GOB conversion and linear framebuffer fallback
6. `horizonlite_save_data_relaunch_test`: second headless run against persistent save data
7. `horizonlite_loader_config_test`: 24-byte LoaderConfig entries, keys and values

Build and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Compatibility and architectural limits

The scheduler is cooperative rather than preemptive. Guest threads are interleaved on one emulated CPU; priority and processor affinity are stored but not used for a multicore execution policy. Sleep and event waits are processed at scheduler boundaries. Mutex arbitration implements the HLE convention used by the built-in guest rather than every retail Horizon kernel condition.

Only the listed ARM64 instruction and SVC subsets are implemented. A standard third-party `.nro` can still require additional instructions, exception handling, relocations, TLS/thread runtime conventions, services, IPC descriptor variants and graphics/audio formats before it runs. No Android APK has been produced by this desktop SDL2 CI workflow.

## Build requirements

- CMake 3.20 or newer
- C++20 compiler (GCC 11+, Clang 13+, or MSVC with C++20 support)
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
│   ├── romfs_asset_test.cpp
│   └── thread_atomic_fpu_test.cpp
└── .github/workflows/build.yml
```


## Stage 10: GPU command processor foundation

Stage 10 introduces a bounded software HLE for the first Maxwell 3D command path:

- `include/horizon/gpu.hpp` and `src/gpu.cpp` provide a 40-bit GPU virtual address space, nvmap-backed mappings, range allocation/free and guest-memory translation.
- `/dev/nvhost-as-gpu`, `/dev/nvhost-ctrl-gpu` and `/dev/nvhost-gpu` are recognized by the existing `nvdrv` dispatcher. The HLE exposes address-space allocation/mapping, Tegra X1 profile fields, ZCULL/TPC responses, channel settings, object contexts and GPFIFO submission.
- `GpuChannel` consumes 64-bit GPFIFO entries, resolves pushbuffer GPU addresses and forwards decoded words to `DmaPusher`.
- `DmaPusher` decodes Increasing, NonIncreasing, Inline and IncreasingOnce method packets and dispatches method writes to the Maxwell 3D register bank.
- The initial `0xB197` software engine tracks 4096 registers, clears a linear RGBA8888 target, reads a fixed position/colour vertex stream, rasterizes triangles with barycentric colour interpolation and writes semaphore payloads.
- Kernel shutdown statistics include GPFIFO submissions, pushbuffer words, method writes, draw calls, triangles and clear operations.

The GPU test target submits an actual guest-memory GPFIFO descriptor and pushbuffer containing Maxwell 3D method packets, then verifies the clear and RGB triangle output.

**ABI and compatibility boundary:** the ioctl identifiers and compact argument layouts in this stage are a documented HorizonLite HLE contract, not a byte-for-byte implementation of every retail `nvhost` ioctl structure. The reported GM20B characteristics are compatibility values, not hardware discovery. The renderer currently supports a narrow linear RGBA8888 target and a fixed float3+float4 vertex stream; it does not implement Tegra GOB/block-linear render targets, shaders, texture sampling, depth/stencil, blending, raster rules, syncpoint timelines, preemption, cache coherency or the full Maxwell method space. The GPFIFO CTest drives the C++ GPU channel directly; it does not yet prove that an arbitrary NRO can initialize this GPU interface or that a triangle is presented through `vi:m`.


## Stage 11: SDL OpenGL renderer, indexed geometry and texture HLE

Stage 11 adds a hardware-accelerated OpenGL rendering backend while retaining the Stage 10 software path for headless tests and systems where an OpenGL 3.3 context cannot be created.

- SDL first requests an OpenGL 3.3 Core Profile context with a 24-bit depth buffer, then tries an OpenGL ES 3.0 context. OpenGL entry points are resolved dynamically through `SDL_GL_GetProcAddress`; no GLAD or GLEW dependency is added. The renderer selects desktop GLSL 3.30 or GLSL ES 3.00 and the matching depth-clear entry point. If both context paths fail, the window falls back to the existing SDL 2D renderer.
- `include/horizon/gl_renderer.hpp` and `src/gl_renderer.cpp` provide GLSL 3.30 shaders, VAO/VBO/EBO submission, an RGBA8 + depth render target FBO, viewport/scissor, depth function/write state, culling, blending, indexed and non-indexed draws, texture upload/cache and FBO-to-window blitting.
- Interactive startup attaches the SDL GL renderer to `GpuChannel`. Maxwell draw and clear methods target the OpenGL FBO when that context is available. The frame loop presents a pending GPU FBO; otherwise it preserves the existing CPU framebuffer presentation path.
- Maxwell state decodes the requested viewport/scissor, depth test/function/write, culling and blending registers. The ZETA emulation currently uses the renderer's internal GL_DEPTH_COMPONENT24 attachment.
- Indexed drawing supports U8, U16 and U32 guest index buffers and converts them to the OpenGL index stream. Primitive selection supports triangles, triangle strips and lines.
- TIC/TSC pool access reads an 8-word (32-byte) HorizonLite descriptor. Its documented subset is: word 0 GPU address low; word 1 GPU address high in bits 0-7, block-linear flag in bit 8 and block-height log2 in bits 9-11; words 2/3 width/height; word 4 format (0 = RGBA8888, 1 = RGB565); word 7 TSC descriptor index. TSC word 0 bit 0 selects linear filtering. Guest texture data is converted to RGBA8888 and cached by GPU address.

**Retail compatibility boundary:** this TIC/TSC word layout is a bounded project HLE contract, not a complete decoder of retail GM20B descriptor bitfields. The ZETA GPU address is tracked in the Maxwell register bank but the GL depth storage is a host FBO attachment rather than a guest-mapped depth surface. The renderer uses a temporary identity-MVP shader; it does not yet implement Maxwell shader decompilation, full matrix/transform state, multisampling, full blend factors, stencil operations, full texture formats, mipmaps, texture views, GOB swizzles for every format or synchronization/cache-coherency semantics.

The GPFIFO CTest now includes indexed drawing and depth-state method writes in addition to the Stage 10 triangle test. It uses the C++ GPU channel with no GL context. The guest ARM64 self-test has **not yet been extended to initialize the GPU, upload a checkerboard TIC/TSC, and animate a rotating indexed cube**; the 60 FPS cube scenario remains a subsequent integration task. SDL swap interval 1 requests display synchronization, but it does not guarantee a fixed 60 FPS on every display or device.
