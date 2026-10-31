# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The DMR baseband's boot block: the 2048 bytes an SCT3258 baseband takes from the MCU at every power-up
(docs/spec/hardware-d578uv.md 4.3). It is the radio maker's, so it is never in this repository or a release: it is
found in the user's own stock firmware file by its SHA-256 and placed in the image's modem boot slot on their machine.

The stock file is a flat application image (the vendor's .CDD, from its first byte at the application address), or a
read of the radio's program flash. The block sits at a different offset in each build, so it is found by its first
bytes and checked by its hash."""
import hashlib

BOOT_LEN = 2048
# SHA-256 of the block, and its first 8 bytes to find it by (every 578 build checked carries the same block: GD and
# Artery builds of 2.08 and PRO 1.21)
BOOT_SHA256 = "743515b9f7cd90737c1fff29b652b7558bc5a56487ec38df4fdea8877e7a619a"
BOOT_PREFIX = bytes.fromhex("448a0920f83015a6")


class ModemBootError(Exception):
    pass


def find(stock):
    """The boot block in a stock image or flash read, checked by its hash. ModemBootError when it isn't there."""
    at = stock.find(BOOT_PREFIX)
    while at >= 0:
        blk = stock[at:at + BOOT_LEN]
        if len(blk) == BOOT_LEN and hashlib.sha256(blk).hexdigest() == BOOT_SHA256:
            return blk
        at = stock.find(BOOT_PREFIX, at + 1)
    raise ModemBootError("no DMR baseband boot block in this file: pick the radio's stock firmware (.CDD) or a read "
                         "of its program flash")


def merge(image, app_base, slot, slot_end, block):
    """image (built from app_base) padded with 0xFF up to the slot, then the block. ModemBootError when the image
    reaches into the slot or the block doesn't fit it."""
    off = slot - app_base
    if len(image) > off:
        raise ModemBootError("the image (%d bytes) runs into the modem boot slot at 0x%08X" % (len(image), slot))
    if slot + len(block) > slot_end:
        raise ModemBootError("the boot block doesn't fit the slot 0x%08X..0x%08X" % (slot, slot_end))
    return bytes(image) + b"\xff" * (off - len(image)) + bytes(block)
