#!/usr/bin/env python3
import sys
import os

"""
Patch for Goodix FOD HAL (libgf_hal.so) on Transsion (Infinix/Tecno) MTK devices
when ported to Custom ROMs (ColorOS 17 / OxygenOS / AOSP).

Fixes:
1. Bypass kernel Netlink uevent wait in `tran_uevent_handle` (waits 30ms for HBM panel luminance and returns 1).
2. NOP out the `uiReadyStatus` check in `gfHbmEventDetectingThread` so capture fires immediately when finger is pressed.
"""

def patch_libgf_hal(filepath):
    if not os.path.isfile(filepath):
        print(f"Error: file not found: {filepath}")
        sys.exit(1)

    with open(filepath, "rb") as f:
        data = bytearray(f.read())

    # Target 1: tran_uevent_handle (file offset 0x5766c for vma 0x5b66c)
    off_uevent = 0x5766c
    orig_head = data[off_uevent : off_uevent + 4]
    if orig_head != bytes([0xfd, 0x7b, 0xba, 0xa9]) and orig_head != bytes([0xfd, 0x7b, 0xbf, 0xa9]):
        print(f"Warning: unexpected bytes at 0x{off_uevent:x}: {orig_head.hex()}")

    patch_uevent = bytes([
        0xfd, 0x7b, 0xbf, 0xa9, # stp x29, x30, [sp, #-16]!
        0x00, 0xa6, 0x8e, 0x52, # mov w0, #30000 (30ms sleep for HBM panel rise)
        0x3f, 0xb0, 0x00, 0x94, # bl <usleep@plt>
        0x20, 0x00, 0x80, 0x52, # mov w0, #1
        0xfd, 0x7b, 0xc1, 0xa8, # ldp x29, x30, [sp], #16
        0xc0, 0x03, 0x5f, 0xd6  # ret
    ])
    data[off_uevent : off_uevent + len(patch_uevent)] = patch_uevent

    # Target 2: NOP out uiReadyStatus gate at file offset 0x57f04 (vma 0x5bf04)
    off_uiready = 0x57f04
    patch_uiready = bytes.fromhex("1f 20 03 d5 1f 20 03 d5 1f 20 03 d5")
    data[off_uiready : off_uiready + len(patch_uiready)] = patch_uiready

    outpath = filepath if filepath.endswith("_patched.so") else filepath + ".patched"
    with open(outpath, "wb") as f:
        f.write(data)

    print(f"Successfully patched: {outpath}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 patch_libgf_hal.py <path_to_libgf_hal.so>")
        sys.exit(1)
    patch_libgf_hal(sys.argv[1])
