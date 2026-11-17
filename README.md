# HorizonLite — Stage 7: cooperative threads, ARM64 atomics and scalar FPU

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

## Retained Stage 6 display and GPU-memory HLE

Stage 6 functionality remains available:

- `nvdrv` Open/Initialize/SetAruid and reduced Ioctl/Ioctl2 routing for `/dev/nvmap`, `/dev/nvhost-ctrl` and `/dev/nvhost-gpu`
- nvmap CREATE, ALLOC, GET_ID, FROM_ID and PARAM records, including validated guest virtual ranges
- `vi:m`, Application Root/Display Service, Binder session and NativeWindow-style Parcel subset
- Binder CONNECT, SET_PREALLOCATED_BUFFER, DEQUEUE_BUFFER, REQUEST_BUFFER and QUEUE_BUFFER transactions
- Tegra 16Bx2 GOB block-linear RGBA untile helper and linear fallback, followed by SDL2 `SDL_UpdateTexture`

The display path is host-backed HLE, not Tegra GPU command execution. It does not implement NVHOST syncpoints/channels, GPU command buffers, shaders, GPU fences, or all Android Binder object relocation and Parcel semantics.

## Tests

The CTest suite contains five targets:

1. `horizonlite_headless_self_test`: guest heap, nvdrv/vi:m Binder display, worker thread, mutex contention, moving sprite and audio initialization
2. `horizonlite_cpu_memory_test`: existing ARM64 integer/memory and SDL2 audio tests
3. `horizonlite_thread_atomic_fpu_test`: 32/64-bit exclusive load/store, reservation invalidation, CLREX, scalar single/double FP conversions/arithmetic/FMOV/FCMP
4. `horizonlite_romfs_asset_test`: NRO ASET/RomFS parsing and asset building
5. `horizonlite_display_gpu_test`: Tegra GOB conversion and linear framebuffer fallback

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
