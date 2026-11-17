# HorizonLite — Stage 5: audout:u + SDL2 PCM audio

HorizonLite is an educational C++20 HLE research prototype for limited AArch64 execution, Horizon-style SVCs, HIPC/CMIF service dispatch, HID shared memory, NRO ASET/RomFS, an SDL2 framebuffer and queued PCM audio.

It is **not a complete Nintendo Switch emulator**. It does not implement the full Horizon kernel, all AArch64 instructions, the complete retail audio ABI, GPU command processing, NSP/XCI loading, NCA decryption, or commercial-game compatibility. The HIPC descriptors and HLE services are a bounded subset for the built-in guest demo and test suite.

## Stage 5 audio output

### SDL2 audio device

- SDL audio subsystem initialization is separate from the video subsystem.
- Opens one SDL2 audio output device at 48,000 Hz, `AUDIO_S16LSB`, signed 16-bit PCM, two channels (stereo).
- Uses `SDL_QueueAudio` and `SDL_GetQueuedAudioSize`; no user callback is required.
- Headless self-test selects SDL's dummy audio backend when `SDL_AUDIODRIVER` is not already configured.
- `Stop` pauses and clears the queued output; queued guest buffers are moved to the released-buffer list.

### Kernel audio objects and synchronization

- `KEvent` is a kernel object with a `signaled` state and can be referenced through a guest handle.
- `KAudioOut` tracks active buffer headers, cumulative submitted byte offsets, released buffer pointers, and its per-session buffer event.
- Audio completion is estimated from the change in SDL's queued-byte count. When a buffer's end offset has left SDL's queue, the header pointer is added to the released list and the event is signaled.
- `svcWaitSynchronization` (`0x18`) checks a guest array of 32-bit handles at X1 with count X2, updates queued-audio progress, and returns the ready index in X1. The bounded fallback signals a valid event rather than trapping the emulated thread forever.
- `svcResetSignal` (`0x17`) clears the event state for the handle in X0.

### HLE service `audout:u`

The service manager `sm:` recognizes `audout:u` and returns an `IAudioOutManager` session.

| Interface | Command | Behavior |
| --- | ---: | --- |
| `IAudioOutManager` | 0 | `ListAudioOuts`: writes the null-terminated `DeviceOut` name to a Receive Buffer and returns count 1 |
| `IAudioOutManager` | 1 | `OpenAudioOut`: reads `DeviceOut` from Send/Auto Send and returns a Move Handle plus 48000 Hz, 2 channels, Int16 format 2, state 0 |
| `IAudioOut` | 0 | `GetAudioOutState`: returns Started (0) or Stopped (1) |
| `IAudioOut` | 1 / 2 | `Start` / `Stop`: unpauses or pauses SDL audio; Stop also clears queued audio and releases active buffer headers |
| `IAudioOut` | 3 / 7 | `AppendAudioOutBuffer` / Auto: reads the five-u64 audio buffer header, validates buffer bounds and queues guest S16LE stereo samples |
| `IAudioOut` | 4 | `RegisterBufferEvent`: returns a Copy Handle to the session's KEvent |
| `IAudioOut` | 5 / 8 | `GetReleasedAudioOutBuffers` / Auto: writes released header pointers to a Receive/Auto Receive Buffer and returns the count |

The guest buffer header is represented by five little-endian u64 fields: `next_ptr`, `sample_buffer_ptr`, `sample_buffer_capacity`, `data_size`, and `data_offset`. The service validates `data_offset + data_size` against capacity and checks that the PCM range is mapped and readable before queuing.

### Auto IPC buffers

The reduced HIPC descriptor reader recognizes Auto Send type `0x21` and Auto Receive type `0x22`, in addition to X (Pointer), A (Send), B (Receive), and W (Exchange) buffers. In this prototype, the auto-type value is carried in the low byte of the descriptor attributes word. This is a documented local encoding for the test harness, not a claim that every retail HIPC variant uses this exact encoding.

### ARM64 additions

- `MOVN Wd, #imm16` / `MOVN Xd, #imm16` for inverted immediate values.
- `STRH Wt, [Xn, #imm]` for 16-bit PCM samples.
- `EOR` is executed by the existing logical-register decoder and is now covered by the CPU regression test.

## Self-test: sprite + sound

Run without an NRO path:

    ./build/horizonlite

The built-in ARM64 guest performs the Stage 3/4 service flows, then:

1. Requests `audout:u` through `sm:`.
2. Opens `DeviceOut`, registers the buffer KEvent, and starts the audio output.
3. Fills a guest RAM buffer with 4,800 frames of stereo S16LE square-wave PCM at approximately 440 Hz (about 100 ms).
4. On an A-button edge, calls `AppendAudioOutBufferAuto` to queue the PCM buffer.
5. Queries `GetReleasedAudioOutBuffersAuto` while continuing the HID-driven RomFS sprite loop.

SDL2 keyboard mapping: Arrow keys / WASD move the sprite; Z=A, X=B, A=X, S=Y, Enter=Plus. Press Escape or close the window to exit.

Run the same guest path in CI/headless mode:

    ./build/horizonlite --headless

Headless mode uses SDL's dummy audio driver when needed, simulates an A-button press to exercise the append path, waits for the queued 100 ms buffer to complete, and verifies the sprite framebuffer output.

## Earlier stages retained

Stage 5 retains the Stage 3 TLS, CPU logical/branch extensions, handle table, CMIF and `sm:`/HID services, plus Stage 4 NRO ASET extraction, the RomFS builder, `fsp-srv`, `IStorage::Read`, and sprite rendering.

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

### NRO loading limitation

The loader currently accepts uncompressed NRO images and direct RomFS data in the optional ASET section. The emulator does not yet implement the complete NRO process startup model, every AArch64 instruction, IVFC-wrapped RomFS, full Horizon IPC/session semantics, all audio formats or all libnx service contracts.

## Project structure

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
    │   └── romfs_asset_test.cpp
    └── .github/workflows/build.yml
