# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Protected flash regions: the one check every Python tool that erases or writes a radio's flash makes first.

A board's [[flash.protect]] (boards/<id>/board.toml, docs/dev/board-registry.md "Protected regions") lists the factory
data no tool may touch: the calibration and its copies, and whatever else can't be made again. Each entry names its
memory and an offset range in it:

    progmem   the MCU's program flash, offsets from the platform's flash_base (Board.rom_base)
    data      the SPI NOR data flash, from 0
    nand      the SPI NAND's main area, from 0 (2 KB pages without their spare bytes)
    secreg    the SPI NOR flash's security registers, 256 bytes each from register 1 at 0

A write that overlaps one is refused before anything is erased (check). A restore that puts a whole memory back keeps
the radio's own bytes there and writes around them (carve). A whole-chip erase is refused on a memory with any
protected region (check_whole). site/static/js/flasher/protect.js is the same check for the web flasher.
"""

MEMS = ("progmem", "data", "nand", "secreg")
NAMES = {"progmem": "program flash", "data": "data flash", "nand": "NAND", "secreg": "security registers"}


class ProtectedError(ValueError):
    """A write that would touch a protected region. regions: the entries it hit."""

    def __init__(self, message, regions=()):
        super().__init__(message)
        self.regions = list(regions)


def entries(protect):
    """The entry list of a board (anything with .protect, a registry entry's dict with "protect") or of a list."""
    if protect is None:
        return []
    if isinstance(protect, dict):
        return list(protect.get("protect") or [])
    return list(getattr(protect, "protect", protect) or [])


def _mem(mem):
    if mem not in MEMS:
        raise ValueError("unknown memory %r (known: %s)" % (mem, ", ".join(MEMS)))
    return mem


def describe(p):
    return "%s 0x%X-0x%X, %s" % (NAMES[p["mem"]], p["start"], p["end"], p["what"])


def overlapping(protect, mem, start, end):
    """The entries of mem that overlap [start, end)."""
    _mem(mem)
    return [p for p in entries(protect) if p["mem"] == mem and p["start"] < end and start < p["end"]]


def check(protect, mem, start, end, action="write"):
    """Refuse (ProtectedError) a write or erase of [start, end) of mem that overlaps a protected region."""
    hit = overlapping(protect, mem, start, end)
    if hit:
        raise ProtectedError("refusing to %s %s 0x%X-0x%X: it overlaps a protected region (%s). Nothing was written"
                             % (action, NAMES[mem], start, end, "; ".join(describe(p) for p in hit)), hit)


def check_absolute(protect, base, start, end, action="write"):
    """check() for program flash addresses (an absolute [start, end) on a bus where the program flash starts at base)."""
    check(protect, "progmem", start - base, end - base, action)


def check_whole(protect, mem, action="erase the whole"):
    """Refuse a whole-chip erase (SPI 0x60 / 0xC7, an STM32 mass erase) of a memory with any protected region."""
    hit = [p for p in entries(protect) if p["mem"] == _mem(mem)]
    if hit:
        raise ProtectedError("refusing to %s %s: it holds protected regions (%s). Nothing was erased"
                             % (action, NAMES[mem], "; ".join(describe(p) for p in hit)), hit)


def carve(protect, mem, start, end):
    """[start, end) without the protected regions of mem: [(lo, hi)] in order, the pieces a restore writes. The radio's
    own bytes stay in the gaps."""
    pieces, at = [], start
    for p in sorted(overlapping(protect, mem, start, end), key=lambda p: p["start"]):
        if p["start"] > at:
            pieces.append((at, p["start"]))
        at = max(at, p["end"])
    if at < end:
        pieces.append((at, end))
    return pieces


def carved_units(protect, mem, start, end, unit):
    """The unit-aligned addresses in [start, end) that carve() leaves, for a writer that works unit by unit (a 4 KB
    sector). Protected regions are on erase boundaries (tools/boards.py check), so a unit is either in or out whole."""
    return [a for lo, hi in carve(protect, mem, start, end) for a in range(lo, hi, unit)]
