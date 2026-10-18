# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Board descriptions for flashing: flash sectors, where the application and the codec live, how the bootloader is
reached. One source of truth for odrflash and tools/release.py (docs/dev/release.md "Sector maps").
"""
from . import stockfw


class Sector:
    __slots__ = ("index", "addr", "size")

    def __init__(self, index, addr, size):
        self.index, self.addr, self.size = index, addr, size

    @property
    def end(self):
        return self.addr + self.size

    def overlaps(self, lo, hi):
        return lo < self.end and hi > self.addr

    def __repr__(self):
        return "Sector(%d, 0x%08X, 0x%X)" % (self.index, self.addr, self.size)


class CodecProfile:
    """A stock codec at fixed flash addresses. windows: [(addr, length)]. installed: the stock firmware versions
    that leave exactly this codec at these addresses when they are installed on the radio (so "keep" is safe)."""

    def __init__(self, id, windows, installed, sources):
        self.id = id
        self.windows = windows
        self.installed = installed
        self.sources = sources		# stock files odrflash can merge it from (names, for messages)


class Board:
    def __init__(self, id, name, family, app_base, flash_end, sectors, codec, debuglink_names, releasable,
                 keep_codec_supported, key_sha256=None, dfu_ids=None, dfu_string1=None, dfu_product=None, protected=(),
                 data_regions=(), first_stage=None):
        self.id = id
        self.name = name
        self.family = family
        self.app_base = app_base
        self.flash_end = flash_end
        self.sectors = sectors			# the sectors the bootloader can erase, in its order
        self.codec = codec			# CodecProfile
        self.debuglink_names = debuglink_names	# VERSION board= values that mean this board
        self.releasable = releasable
        self.keep_codec_supported = keep_codec_supported
        self.key_sha256 = key_sha256
        self.dfu_ids = dfu_ids
        self.dfu_string1 = dfu_string1
        self.dfu_product = dfu_product		# the bootloader's USB product name (what Zadig lists)
        self.protected = protected		# address ranges never written (bootloader)
        self.data_regions = list(data_regions)	# [(addr, size)] flash outside the application the firmware image may
        # also fill (the DM-1701's sector 9: read-only data, docs/dev/release.md "Sector 9"); never a codec sector
        self.first_stage = first_stage		# (size, first bytes): the image at app_base starts with a first stage
        # this long, then the application (make's odr_<board>_install.bin); None: the application starts at app_base

    def sector_at(self, addr):
        for s in self.sectors:
            if s.addr <= addr < s.end:
                return s
        return None

    def sectors_for(self, lo, hi):
        """Sectors overlapping [lo, hi)."""
        return [s for s in self.sectors if s.overlaps(lo, hi)]

    def codec_sectors(self):
        """Sectors that hold any byte of a codec window (never erased when the codec is kept)."""
        out = []
        for lo, length in self.codec.windows:
            for s in self.sectors_for(lo, lo + length):
                if s not in out:
                    out.append(s)
        return sorted(out, key=lambda s: s.addr)


def _f405_sectors():
    """STM32F405 1 MB: sectors 0-3 16 KB, 4 64 KB, 5-11 128 KB (reference manual, flash module organization).
    Sectors 0-2 hold the stock bootloader and are not in the list."""
    out = [Sector(3, 0x0800C000, 0x4000), Sector(4, 0x08010000, 0x10000)]
    for i in range(5, 12):
        out.append(Sector(i, 0x08020000 + (i - 5) * 0x20000, 0x20000))
    return out


_V0206 = stockfw.PROFILES["0314efd4c62625661a2d693261ae682dff8a1b075a30620f3dbda4be065de32d"]

DM1701 = Board(
    id="dm1701",
    name="Baofeng DM-1701",
    family="anyroad-dfu",
    app_base=0x0800C000,
    flash_end=0x08100000,
    sectors=_f405_sectors(),
    codec=CodecProfile(
        id=_V0206["codec_profile"],
        windows=[(_V0206["code"][0], _V0206["code"][1]), (_V0206["tables"][0], _V0206["tables"][1])],
        installed=["V02.06 CSV"],
        sources=[_V0206["name"]],
    ),
    debuglink_names=["DM-1701", "dm1701"],
    releasable=True,
    keep_codec_supported=True,
    key_sha256=stockfw.DM1701_KEY_SHA256,
    dfu_ids=(0x0483, 0xDF11),
    dfu_string1="AnyRoad Technology",
    dfu_product="Digital Radio in USB mode",
    protected=[(0x08000000, 0x0800C000)],
    data_regions=_V0206["app_data"],
)

# Not released yet (docs/testing/dm32-bringup.md). Program flash 0x03000000..0x03100000; the Baofeng bootloader at
# 0x03000000..0x0300C000; the codec slot 0x030E8000..0x03100000 (boards/dm32/dm32.ld, tools/dm32_codec.py). The
# Baofeng serial updater erases the whole application area before it writes (hardware-dm32.md 13.2 step 7), so the
# codec can't be kept across an update: every flash merges it again. Modeled as one "sector" for the app and one for
# the slot so manifests can say where the codec goes.
DM32 = Board(
    id="dm32",
    name="Baofeng DM-32UV",
    family="bfuv32-serial",
    app_base=0x0300C000,
    flash_end=0x03100000,
    sectors=[Sector(0, 0x0300C000, 0x030E8000 - 0x0300C000), Sector(1, 0x030E8000, 0x03100000 - 0x030E8000)],
    codec=CodecProfile(id="dm32-049", windows=[(0x030E8000, 0x03100000 - 0x030E8000)],
                       installed=[], sources=["DM32.01.01.049.bin", "DM32.00.01.046HRVocoder.bin"]),
    debuglink_names=["DM-32UV", "DM-32", "dm32"],
    releasable=False,
    keep_codec_supported=False,
    protected=[(0x03000000, 0x0300C000)],
)

# Not released yet (docs/testing/dm1801pro-bringup.md). The same Baofeng serial updater as the DM-32UV, with its own ID
# string (BF1801-V1); the same flash map, the same codec slot and the same erase of the whole application area. The
# codec comes from the radio's own program flash (read with stub/stubtool.py), not from a stock file
# (docs/spec/codec-dm1801pro.md, tools/dm32_codec.py --board dm1801pro). tools/dm32_flash.py --model dm1801pro does
# the writing.
DM1801PRO = Board(
    id="dm1801pro",
    name="Baofeng DM-1801 Pro",
    family="bfuv32-serial",
    app_base=0x0300C000,
    flash_end=0x03100000,
    sectors=[Sector(0, 0x0300C000, 0x030E8000 - 0x0300C000), Sector(1, 0x030E8000, 0x03100000 - 0x030E8000)],
    codec=CodecProfile(id="dm1801pro-049", windows=[(0x030E8000, 0x03100000 - 0x030E8000)],
                       installed=[], sources=["a program flash read of the radio"]),
    debuglink_names=["DM-1801 Pro", "DM-1801Pro", "dm1801pro"],
    releasable=False,
    keep_codec_supported=False,
    protected=[(0x03000000, 0x0300C000)],
)

# Not released yet (docs/testing/md380-bringup.md). The TYT bootloader takes the same DFU protocol and sector order as
# the DM-1701's, with its own key (tools/stm32_dfu.py --model md380). The stock D002.032 codec runs in place at its
# stock addresses (docs/spec/codec-md380.md): the code window starts at 0x08050C44, inside sector 6, so the application
# ends there (boards/md380/md380.ld) and shares sector 6 with the codec. An update erases sector 6 and with it part of
# the codec, so the codec can't be kept: every flash merges it from the stock file again.
_D2032 = stockfw.PROFILES["e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43"]

MD380 = Board(
    id="md380",
    name="TYT MD-380",
    family="anyroad-dfu",
    app_base=0x0800C000,
    flash_end=0x08100000,
    sectors=_f405_sectors(),
    codec=CodecProfile(id=_D2032["codec_profile"], windows=[(a, n) for a, n, _ in stockfw.windows(_D2032)],
                       installed=[], sources=[_D2032["name"]]),
    debuglink_names=["MD-380", "MD-380 VHF", "md380"],
    releasable=False,
    keep_codec_supported=False,
    key_sha256=stockfw.MD380_KEY_SHA256,
    dfu_ids=(0x0483, 0xDF11),
    dfu_string1="AnyRoad Technology",
    dfu_product="Digital Radio in USB mode",
    protected=[(0x08000000, 0x0800C000)],
)

# Bring-up, not released (docs/spec/hardware-db25d.md 2, 7, 8). The vendor IAP loader (0x03000000..0x0300BFFF) is never
# written. The first stage fills the sector at 0x0300C000, ODR starts at 0x0300D000, the codec slot is the same as the
# DM-32UV's. No tool here flashes it: the web flasher does (IAP loader, or the stub from ODR), and stub/stubtool.py.
DB25D = Board(
    id="db25d",
    name="Radioddity DB25-D PRO",
    family="hrc7000-iap",
    app_base=0x0300C000,
    flash_end=0x03100000,
    sectors=[Sector(0, 0x0300C000, 0x030E8000 - 0x0300C000), Sector(1, 0x030E8000, 0x03100000 - 0x030E8000)],
    codec=CodecProfile(id="db25d-018", windows=[(0x030E8000, 0x03100000 - 0x030E8000)],
                       installed=[], sources=["DB25-D PRO 909E.Dx.EARSAB.018 stock files", "a program flash read of the radio"]),
    debuglink_names=["DB25-D", "DB25-D PRO", "db25d"],
    releasable=False,
    keep_codec_supported=False,
    protected=[(0x03000000, 0x0300C000)],
    first_stage=(0x1000, bytes([0x2C, 0x11, 0x01, 0xC0])),
)

BOARDS = {b.id: b for b in (DM1701, DM32, DM1801PRO, MD380, DB25D)}


def get(board_id):
    b = BOARDS.get((board_id or "").lower().replace("-", "").replace("_", ""))
    if b is None:
        raise KeyError("unknown board %r (known: %s)" % (board_id, ", ".join(sorted(BOARDS))))
    return b


def from_debuglink_name(name):
    """The board for a VERSION board= value, or None."""
    for b in BOARDS.values():
        if name in b.debuglink_names:
            return b
    return None
