#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Wraps a raw DM-32 application image (linked for 0x0300C000, where the Baofeng bootloader puts it) in the Baofeng firmware file format the DM-32 CPS
and bootloader accept: a 256-byte header, "BFUV32-V2", 0xFF padding, "END" at 0xFD, then the image. Layout from the
stock DM32.01.01.049 file and the OpenDM32 binary releases (no checksum in either).

    dm32_pack.py firmware.bin firmware_BFUV32.bin
    dm32_pack.py --unpack DM32.01.01.049.bin app.bin
"""
import sys

HEADER_SIZE = 0x100
MAGIC = b"BFUV32-V2"
END = b"END"


def pack(image):
    header = bytearray(b"\xff" * HEADER_SIZE)
    header[:len(MAGIC)] = MAGIC
    header[HEADER_SIZE - len(END):] = END
    return bytes(header) + image


def unpack(data):
    if not data.startswith(MAGIC) or data[HEADER_SIZE - len(END):HEADER_SIZE] != END:
        raise SystemExit("not a BFUV32-V2 firmware file")
    return data[HEADER_SIZE:]


def main(argv):
    if len(argv) == 4 and argv[1] == "--unpack":
        out = unpack(open(argv[2], "rb").read())
    elif len(argv) == 3:
        out = pack(open(argv[1], "rb").read())
    else:
        raise SystemExit(__doc__)
    open(argv[-1], "wb").write(out)


if __name__ == "__main__":
    main(sys.argv)
