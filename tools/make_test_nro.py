#!/usr/bin/env python3
"""Create a tiny NRO-layout fixture for testing HorizonLite's parser and CPU."""

from pathlib import Path
import struct

OUTPUT = Path("test_homebrew.nro")
IMAGE_SIZE = 0x88

image = bytearray(IMAGE_SIZE)

# NroStart: branch from offset 0 to the first test instruction at 0x80.
# AArch64 B #0x80 has imm26 = 0x20.
struct.pack_into("<I", image, 0x00, 0x14000020)
struct.pack_into("<I", image, 0x04, 0)

# NroHeader starts at 0x10. The fixed NRO0 signature is at file offset 0x10.
image[0x10:0x14] = b"NRO0"
struct.pack_into("<I", image, 0x14, 0)          # version / reserved
struct.pack_into("<I", image, 0x18, IMAGE_SIZE) # total NRO image size
struct.pack_into("<I", image, 0x1C, 0)          # flags

# Segment descriptors: .text starts at file offset 0 and includes the header.
struct.pack_into("<II", image, 0x20, 0, IMAGE_SIZE)
struct.pack_into("<II", image, 0x28, 0, 0)       # .rodata empty
struct.pack_into("<II", image, 0x30, 0, 0)       # .data empty
struct.pack_into("<I", image, 0x38, 0)           # BSS size

# MOVZ X0, #42 ; RET X30. The test runner initializes X30 to its stop address.
struct.pack_into("<I", image, 0x80, 0xD2800540)
struct.pack_into("<I", image, 0x84, 0xD65F03C0)

OUTPUT.write_bytes(image)
print(f"Wrote {OUTPUT} ({len(image)} bytes)")
