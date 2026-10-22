# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Board descriptions for flashing: flash sectors, where the application and the codec live, how the bootloader is
reached. Read from registry.json, which tools/boards.py generates from boards/*/board.toml, the boards' board.mk and
linker scripts (docs/dev/release.md "Sector maps"). The flashing tools (odrflash, tools/release.py, stub/stubtool.py,
stub/f4stub.py, tools/serial_loader.py, tools/hrc7000_codec.py, tools/stm32_dfu.py) all read it from here, so it works
on any Python 3 and inside the odrflash executable, where there is no repository.
"""
import json
import os

REGISTRY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "registry.json")


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


def _pair(v):
    return tuple(v) if v else None


class Board:
    """One registry entry. e: the board's object in registry.json (tools/boards.py odrflash_view)."""

    def __init__(self, e):
        c = e["codec"]
        dfu = e["dfu"]
        self.id = e["id"]
        self.name = e["name"]
        self.model = e["model"]			# the short name (the stub's and the serial loader's messages)
        self.platform = e["platform"]
        self.platform_stubs = e["platform_stubs"]	# the platform's flash stubs (stub/<dir>), platform.toml
        self.family = e["family"]		# the flashing route (cli.py, site/static/js/flasher)
        self.release = e["release"]		# "stable" or "testing"
        self.app_base = e["app_base"]
        self.flash_end = e["flash_end"]
        self.app_region = tuple(e["app_region"])	# the application's flash (the linker script's MEMORY FLASH)
        self.sectors = [Sector(*s) for s in e["sectors"]]	# the sectors the bootloader can erase, in its order
        self.codec = CodecProfile(c["profile"], [tuple(w) for w in c["windows"]], c["installed"], c["sources"])
        self.codec_tool = c["tool"]		# the tools/ script that extracts this radio's codec
        self.codec_stock = c["stock"]		# the stock file (sha256) the codec placement comes from, or None
        self.codec_code = c["code"]		# the address of the codec's first word in a merged image
        self.codec_slot = _pair(c["slot"])	# HR_C7000: the slot in program flash, the code's SRAM window, the
        self.codec_run = _pair(c["run"])	# modem table window (the __codec_* linker symbols)
        self.codec_tables = _pair(c["tables"])
        self.debuglink_names = e["debuglink_names"]	# VERSION board= values that mean this board
        self.keep_codec_supported = e["keep_codec"]
        self.key_sha256 = dfu["key_sha256"] if dfu else None
        self.key_prefix = bytes.fromhex(dfu["key_prefix"]) if dfu else None
        self.dfu_ids = (dfu["vid"], dfu["pid"]) if dfu else None
        self.dfu_string1 = dfu["string1"] if dfu else None
        self.dfu_product = dfu["product"] if dfu else None	# the bootloader's USB product name (what Zadig lists)
        self.protected = [tuple(p) for p in e["protected"]]	# address ranges never written (bootloader)
        fs = e["first_stage"]
        self.first_stage = (fs["size"], bytes.fromhex(fs["head"])) if fs else None
        # (size, first bytes): the image at app_base starts with a first stage this long, then the application (make's
        # odr_<board>_install.bin); None: the application starts at app_base
        self.loader_id = e["bootloader"].get("id")	# a serial loader's ID string (also its files' magic)
        self.loader_keys = e["bootloader"].get("keys")	# the key combination that starts it
        self.loader_sign = e["bootloader"].get("sign", "")	# what the radio shows in update mode
        self.stub = e["stub"]			# the universal HR_C7000 stub's profile, or None
        self.build_options = e["build_options"]	# [{"name", "values": [{"id", "board_name"}]}], default first

    @property
    def releasable(self):
        return self.release == "stable"

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

    def build_option(self, name, value):
        """The build option value's entry, or None if the board has no such option or value."""
        for o in self.build_options:
            if o["name"] == name:
                return next((v for v in o["values"] if v["id"] == value), None)
        return None


def _load():
    with open(REGISTRY, encoding="utf-8") as f:
        data = json.load(f)
    return {bid: Board(e) for bid, e in data["boards"].items()}


BOARDS = _load()


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


def by(**attrs):
    """The boards whose attributes have these values, in id order: by(platform="hrc7000"), by(family=...)."""
    return [b for _, b in sorted(BOARDS.items()) if all(getattr(b, k) == v for k, v in attrs.items())]
