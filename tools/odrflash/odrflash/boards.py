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

from . import names

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


def _tuple(v):
    return tuple(v) if v else None


class Board:
    """One registry entry. e: the board's object in registry.json (tools/boards.py odrflash_view), whose keys are this
    class's attributes; only the ones that are richer than JSON are converted here."""

    def __init__(self, e):
        self.__dict__.update(e)
        self.app_region = tuple(e["app_region"])	# the application's flash (the linker script's MEMORY FLASH)
        self.sectors = [Sector(*s) for s in e["sectors"]]	# the sectors the bootloader can erase, in its order
        self.protected = [tuple(p) for p in e["protected"]]	# address ranges never written (the factory loader)
        self.codec_slot, self.codec_run, self.codec_tables = (_tuple(e[k]) for k in ("codec_slot", "codec_run",
                                                                                      "codec_tables"))
        self.dfu_ids = _tuple(e["dfu_ids"])
        self.key_prefix = bytes.fromhex(e["key_prefix"]) if e["key_prefix"] else None
        fs = e["first_stage"]
        # (size, first bytes): the image at app_base starts with a first stage this long, then the application (make's
        # odr_<board>_install.bin); None: the application starts at app_base
        self.first_stage = (fs[0], bytes.fromhex(fs[1])) if fs else None
        c = dict(e["codec"])
        self.codec_layouts = {}
        self.codec_codes = [self.codec_code] if self.codec_code is not None else []
        if c["id"] is None:
            # a stock-file placement (STM32): each listed stock file puts its codec where its own layout says. The board's
            # windows are the union of them all: what the image must stay out of and what keeping the codec protects. A
            # merge plans against the layout of the file it merges (merge_windows)
            from . import stockfw
            profs = [stockfw.PROFILES[h] for h in self.codec_stock]
            ids = {p["codec_profile"] for p in profs}
            if len(ids) != 1:
                raise ValueError("%s: the stock files hold different codecs %s" % (self.id, sorted(ids)))
            wins = []
            for h, prof in zip(self.codec_stock, profs):
                lay = [(a, n) for a, n, _ in stockfw.layout(prof)]
                self.codec_layouts[h] = lay
                wins += [w for w in lay if w not in wins]
            c.update(id=profs[0]["codec_profile"], windows=wins, sources=c["sources"] or [p["name"] for p in profs])
            # where each file's codec code starts (its first word is the codec signature), first file first
            self.codec_codes = [self.codec_layouts[h][0][0] for h in self.codec_stock]
            self.codec_code = self.codec_codes[0]
        self.codec = CodecProfile(c["id"], [tuple(w) for w in c["windows"]], c["installed"], c["sources"] or [])

    @property
    def releasable(self):
        return self.release == "stable"

    def merge_windows(self, prof=None):
        """The windows a merge of this stock profile writes, [(addr, length)]: its own layout, never the union of every
        stock file's. Without a profile (a codec in ODR's slot), the board's windows."""
        if prof is None or not self.codec_layouts:
            return list(self.codec.windows)
        from . import stockfw
        return [(a, n) for a, n, _ in stockfw.layout(prof)]

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

    def options_for_name(self, name):
        """The build option values a debug link board name stands for ({"BAND": "vhf"} for "<model> VHF"); {} for a
        name of this board that says nothing about them, None for another board's name."""
        if name not in self.debuglink_names:
            return None
        out = {}
        for o in self.build_options:
            v = next((v for v in o["values"] if v["board_name"] == name), None)
            if v:
                out[o["name"]] = v["id"]
        return out

    def option_values(self):
        """[(name, [value, ...])], each option's default first: what names.py takes."""
        return [(o["name"], [v["id"] for v in o["values"]]) for o in self.build_options]

    def full_options(self, given):
        """Every build option's value: the given ones, the defaults (each option's first value) for the rest."""
        return names.full(self.option_values(), given)

    def image_id(self, options):
        """The board id with each non-default option value, as the Makefile and the release name images: <id>-vhf."""
        return names.image_id(self.id, self.option_values(), options)

    def build_image(self, options, variant=None):
        """The image make writes for these option values, from the source tree (names.build_image)."""
        return names.build_image(self.id, self.option_values(), options, variant, bool(self.first_stage))

    def release_id(self, options, variant=None):
        """The name id of the release files for these option values (names.release_id)."""
        return names.release_id(self.id, self.option_values(), options, variant)


FORMAT = "odr-board-registry/2"


def _load():
    with open(REGISTRY, encoding="utf-8") as f:
        data = json.load(f)
    if data.get("format") != FORMAT:
        raise RuntimeError("%s is format %r, not %r: regenerate it (tools/boards.py json --for odrflash)"
                           % (REGISTRY, data.get("format"), FORMAT))
    _warn_if_stale()
    return {bid: Board(e) for bid, e in data["boards"].items()}


def _warn_if_stale():
    """From a source tree: warn when a registry source is newer than registry.json (it was edited and not regenerated;
    the build regenerates it, tools/boards.py check fails on it)."""
    import glob
    import sys
    root = os.path.normpath(os.path.join(os.path.dirname(REGISTRY), "..", "..", ".."))
    if not os.path.isfile(os.path.join(root, "tools", "boards.py")):
        return		# the odrflash executable or an installed package: no sources
    try:
        built = os.path.getmtime(REGISTRY)
        srcs = [p for pat in ("boards/*/board.toml", "boards/*/board.mk", "boards/*/*.ld", "boards/*/pins.h",
                              "platform/*/platform.toml") for p in glob.glob(os.path.join(root, pat))]
        newer = [os.path.relpath(p, root) for p in srcs if os.path.getmtime(p) > built + 1]
    except OSError:
        return
    if newer:
        sys.stderr.write("odrflash: warning: %s is older than %s; run make or tools/boards.py json --for odrflash --out "
                         "%s\n" % (os.path.relpath(REGISTRY, root), newer[0], os.path.relpath(REGISTRY, root)))


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
