# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""A fake AnyRoad bootloader with a model of the STM32F405 flash, for tests and rfoundry's --simulate.

It answers the DFU requests of hardware-dm1701.md 15.2, decrypts each block with the key, and programs a 1 MB flash
image the way the chip does: erase sets a whole sector to 0xFF, programming can only clear bits (writing over
programmed bytes is recorded as a violation). Tests check what ends up in flash, which sectors were erased, and that
the codec sectors kept their bytes.
"""
import random
import struct

from . import dfu as dfumod
from .plan import BLOCK, FIRST_BLOCK

FLASH_BASE = 0x08000000
FLASH_SIZE = 0x100000


class FakeAnyRoad:
    def __init__(self, board, key, flash=None, strings=None):
        self.board = board
        self.key = key
        self.flash = bytearray(flash if flash is not None else b"\xFF" * FLASH_SIZE)
        assert len(self.flash) == FLASH_SIZE
        self.state = dfumod.ST_IDLE
        self.busy = 0
        self.addr = None
        self.erased = []
        self.vendor = []
        self.blocks = 0
        self.violations = []
        self.exited = False
        self.strings = strings or {"string1": board.dfu_string1, "manufacturer": "STMicroelectronics",
                                   "product": "STM32  BOOTLOADER", "serial": "FAKE0001",
                                   "interfaces": ["@Internal Flash  /0x08000000/03*016Kg,01*016Kg,01*064Kg,07*128Kg"]}

    # flash helpers
    def read(self, addr, n):
        return bytes(self.flash[addr - FLASH_BASE:addr - FLASH_BASE + n])

    def _program(self, addr, data):
        off = addr - FLASH_BASE
        for i, b in enumerate(data):
            old = self.flash[off + i]
            if old != 0xFF and b != 0xFF and old != b:
                self.violations.append(addr + i)
            self.flash[off + i] = old & b

    # pyusb-like
    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        if rtype == 0xA1 and req == dfumod.DFU_GETSTATUS:
            if self.busy:
                self.busy -= 1
                st = dfumod.ST_DNBUSY
            else:
                st = self.state
            return bytes([0, 1, 0, 0, st, 0])
        assert rtype == 0x21, rtype
        if req in (dfumod.DFU_CLRSTATUS, dfumod.DFU_ABORT):
            self.state = dfumod.ST_IDLE
            return 0
        assert req == dfumod.DFU_DNLOAD
        data = bytes(data or b"")
        self.state = dfumod.ST_DNIDLE
        self.busy = 1
        if value == 0:
            if not data:
                assert self.addr == dfumod.EXIT_ADDR, "exit without set address 0x08000000"
                self.exited = True
            elif data[0] == 0x91:
                self.vendor.append(data[1])
                self.busy = 0
            elif data[0] == 0x41:
                a = struct.unpack("<I", data[1:5])[0]
                sec = self.board.sector_at(a)
                assert sec is not None and sec.addr == a, "erase of 0x%08X, not a sector start" % a
                assert self.vendor == [0x01, 0x31], "erase before the vendor commands"
                self.flash[a - FLASH_BASE:sec.end - FLASH_BASE] = b"\xFF" * sec.size
                self.erased.append(a)
            elif data[0] == 0x21:
                self.addr = struct.unpack("<I", data[1:5])[0]
            else:
                raise AssertionError("unknown command %r" % data)
        else:
            assert value >= FIRST_BLOCK and len(data) == BLOCK, (value, len(data))
            dest = self.addr + (value - FIRST_BLOCK) * BLOCK
            sec = self.board.sector_at(self.addr)
            assert sec is not None and sec.addr == self.addr, "write region 0x%08X is not a sector" % self.addr
            assert dest + BLOCK <= sec.end, "block past the end of its sector"
            off = dest - self.board.app_base
            plain = bytes(b ^ self.key[(off + i) % len(self.key)] for i, b in enumerate(data))
            self._program(dest, plain)
            self.blocks += 1
        return len(data)


def stock_flash(board, rng, codec_segments):
    """Flash as stock firmware leaves it: random 'stock code' over the whole application area, the codec windows
    holding the given codec bytes, the bootloader area random too (never touched)."""
    flash = bytearray(rng.getrandbits(8 * FLASH_SIZE).to_bytes(FLASH_SIZE, "little"))
    for addr, data in codec_segments:
        flash[addr - FLASH_BASE:addr - FLASH_BASE + len(data)] = data
    return flash


def synthetic_key(seed=1701):
    rng = random.Random(seed)
    return bytes(rng.randrange(256) for _ in range(dfumod.KEY_LEN))


class FakeUsb:
    """Just enough of pyusb's module surface for cli's device code paths (find, strings, claim)."""

    def __init__(self, devices):
        self._devices = devices

        class _Core:
            class USBError(Exception):
                errno = None

            class NoBackendError(Exception):
                pass

            @staticmethod
            def find(find_all=True, idVendor=None, idProduct=None, backend=None):
                return list(devices)

        class _Util:
            @staticmethod
            def get_string(dev, index):
                return dev.string_by_index(index)

            @staticmethod
            def claim_interface(dev, intf):
                pass

            @staticmethod
            def dispose_resources(dev):
                pass

        self.core = _Core
        self.util = _Util


class FakeUsbDevice(FakeAnyRoad):
    """A FakeAnyRoad that also looks like a pyusb device (descriptors, strings, one DFU interface)."""
    iManufacturer, iProduct, iSerialNumber = 2, 3, 4

    class _Intf:
        bInterfaceClass, bInterfaceSubClass, bInterfaceNumber, iInterface = 0xFE, 0x01, 0, 5

    def __iter__(self):
        return iter([[self._Intf()]])

    def string_by_index(self, i):
        return {1: self.strings["string1"], 2: self.strings["manufacturer"], 3: self.strings["product"],
                4: self.strings["serial"], 5: self.strings["interfaces"][0]}.get(i)

    def is_kernel_driver_active(self, intf):
        return False

    def set_configuration(self):
        pass
