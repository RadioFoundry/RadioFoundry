#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The board registry: one place for every fact about a radio that isn't the build itself (issue #106).

Sources, each fact in one of them:
  boards/<id>/board.toml        identity, status, build options, CI extras, flashing family, stub profile, codec route,
                                site data
  platform/<p>/platform.toml    toolchain, CPU, CI group, platform stubs, flash base and sector geometries, codec symbols
  boards/<id>/board.mk          PLATFORM, APP_BASE, LDSCRIPT, BOOT_*, BOARD_DIRS, read through make (print-vars), so
                                the board's and the platform's config.mk and their conditionals count; the build reads
                                no TOML
  boards/<id>/*.ld              the application region (MEMORY FLASH of memory.ld) and the codec symbols (__codec_*)
  boards/<id>/*.c               the board_t .name values (the names the debug link reports) and hal_flash_map_t
                                (where the codeplug lives in data flash)
  boards/<id>/pins.h            the pins a stub needs: the power latch and key it holds (HR_C7000), the SPI flash's
                                pins and the chip selects it keeps high (STM32F4)

Derived rather than typed: the flashing link from the family, the CPU from the platform, the protected range (flash
base to the application), the erasable sectors (the platform geometry from the application up), the stub's NAND flag
(the board's drivers), its pins (pins.h), and on the STM32F4 the crystal (HSE_VALUE) and the bootloader's end (APP_BASE)
for the stub's board record.

    tools/boards.py list [--platform P[,Q]] [--toolchain T[,U]] [--release stable|testing] [--comma]   board ids (an
                                                  unknown platform or toolchain is an error, so a typo never selects
                                                  no board)
    tools/boards.py platform P [key.path]        one platform value
    tools/boards.py get <id> [key.path]          one value (a list: one item per line), for shell scripts
    tools/boards.py json [--for odrflash|site|build] [--out FILE | --check FILE]   the merged, derived view; --for
                                                  build is the stable view for tools that build from a checkout
    tools/boards.py check                         registry consistency
    tools/boards.py docs [--check] [--floor REF]  the docs blocks generated from the registry (tools/registry_docs.py);
                                                  --floor: fail when a radio reads lower than the README at REF
    tools/boards.py self-test                     the loader's own tests, on fixtures

Needs Python 3.11 or later (tomllib). Scripts find one with tools/odr_python.sh. The tools that flash radios read the
generated view (tools/odrflash/odrflash/registry.json), so they run on any Python 3 and inside the odrflash executable.
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import registry_docs  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEVELS = ("bring-up", "testing", "supported")
RELEASES = ("stable", "testing")
KINDS = ("handheld", "mobile")
# [hardware] clock rtc: a real-time clock that keeps time through power-off (a backup supply), one that keeps it only
# while the radio has power, none, or not known yet
CLOCKS = ("backed", "unbacked", "none", "unknown")
# The flashing routes, named by protocol (odrflash cli.py, site/static/js/flasher): the cable they need, and the
# board.toml keys a board on the route must have
# web: the web flasher's backend for the family (site/static/js/flasher/backends.js); update_mode: the vendor loader's
# update mode the page can write through when nothing else answers (iap.js), or None
FAMILIES = {
    "usb-dfu": {"link": "usb", "needs": ("flash.dfu",), "web": "anyroad-dfu", "update_mode": None},	# a USB DFU
    # bootloader with a per-model XOR key
    "serial-loader": {"link": "serial-cable", "needs": ("flash.bootloader.id", "flash.bootloader.keys"),
                      "web": "c7000-stub", "update_mode": None},	# a serial bootloader started by a key
    # combination, speaking its own file format (tools/serial_loader.py)
    "serial-iap": {"link": "serial-cable", "needs": (), "web": "c7000-stub", "update_mode": "iap"},	# a vendor IAP
    # loader on the cable, through the RAM stub
    "usb-hid-sgl": {"link": "usb", "needs": (), "web": "sgl-hid", "update_mode": None},	# a vendor loader's USB HID
    # update mode, taking the image inside a stock update file (.sgl) as its envelope (odrflash hidboot.py, updatefile.py)
}
# [flash.stub] stock_console: the debug console a stock firmware answers on the cable, by protocol (the web flasher
# loads the stub through it before ODR is installed: site/static/js/flasher/usmart.js)
STOCK_CONSOLES = ("usmart",)
CODEC_TOOLS = ("codec_extract", "hrc7000_codec", "k22f_codec")
ODRFLASH_JSON = "tools/odrflash/odrflash/registry.json"	# the flashing tools' view, generated and committed
# hal_flash_map_t fields the site reads (the codeplug editor): the open-firmware image's data flash address, the end of
# the room for it and the DMR ID database, the TFTP staging area. The open-firmware flash image follows the EEPROM image
# at a fixed offset (docs/spec/cps-fast.md)
FLASH_MAP_FIELDS = ("openfw_eeprom", "openfw_flash", "openfw_end", "staging_addr")
OPENFW_FLASH_OFFSET = 0x20000
APP_SLOT = "app+slot"		# [flash] sectors: one sector for the application, one for the codec slot (no geometry)
NAND_DRIVER = "drivers/flash/spi_nand"	# in BOARD_DIRS: the board's data memory is SPI NAND (the stub's nand flag)
NO_PIN = 255			# the universal stub's "no pin"
# board.mk values the registry reads, through make (conditionals and config.mk included)
MAKE_VARS = ("PLATFORM", "APP_BASE", "LDSCRIPT", "BOOT_STAGE", "BOOT_SIZE", "BOOT_SIGNATURE", "BOARD_DIRS",
             "BUILD_OPTIONS", "BOARD_UNSUPPORTED", "BOARD_NOT_WRITTEN", "HSE_VALUE", "MCU_DEFS")
# The frame buffer core/ui/gfx.h builds when a board's MCU_DEFS don't size it (RGB565, 160x128, at most 160 rows)
GFX_DEFAULTS = {"GFX_FB_BYTES": 160 * 128 * 2, "GFX_MAX_ROWS": 160, "GFX_INDEXED": 0}
# The [flash.stub] id every stub keeps for its tools' test profiles (stub/f4stub.py test_board()), so a test record
# never names a real board
STUB_TEST_ID = 0xFE
# The pins.h names the STM32F4 stub's board record reads: the SPI flash's clock, data and chip select (stub-stm32f4.md 5.2)
F4_SPI_PINS = ("PIN_FLASH_SCK", "PIN_FLASH_MISO", "PIN_FLASH_MOSI", "PIN_FLASH_CS")


class RegistryError(Exception):
    pass


def _tomllib():
    try:
        import tomllib
    except ImportError:
        raise RegistryError("tools/boards.py needs Python 3.11 or later (tomllib); this is %d.%d. Run it with "
                            "$(tools/odr_python.sh) or set ODR_PYTHON" % sys.version_info[:2])
    return tomllib


# ---------------------------------------------------------------------------------------------------------------
# schema: every key a board.toml or platform.toml may have. A table is {key: (spec, required)}; ("list", spec) is a
# list of spec; ("map", spec) a table with free keys; a tuple of values an enumeration (strings)

def _opt(spec):
    return (spec, False)


def _req(spec):
    return (spec, True)


STR_LIST = ("list", str)
RANGE = ("list", int)

BOARD_SCHEMA = {
    "id": _req(str),
    "name": _req(str),
    "maker": _req(str),
    "model": _req(str),
    "also": _req(STR_LIST),
    "kind": _req(KINDS),
    "order": _req(int),		# where the radio appears in the site's lists; the lowest is the home page's radio
    "part_of": _opt(str),
    "debuglink_aliases": _opt(STR_LIST),
    "docs": _req({"hardware": _req(str), "codec": _opt(str), "install": _opt(str), "bringup": _opt(str)}),
    # unconfirmed: a hardware switch (tests/hil/features.toml hardware) that is neither built nor in
    # BOARD_UNSUPPORTED or BOARD_NOT_WRITTEN because no source says yet whether the radio has the part: switch ->
    # what is missing (docs/build-config.md, "Unsupported, not written, unconfirmed")
    "hardware": _req({"cpu": _opt(str), "bands": _req(str), "power": _req(str), "display": _req(str),
                      "gallery": _req(str), "unconfirmed": _opt(("map", str)),
                      # the real-time clock (CLOCKS) and the source of that fact; no driver uses it yet
                      "clock": _req({"rtc": _req(CLOCKS), "note": _req(str)})}),
    # summary: what works, then at most one short clause of what is open; plain text, no links (the README, the
    # user manual's radio table and the site show it)
    "status": _req({"level": _req(LEVELS), "summary": _req(str), "release": _req(RELEASES)}),
    "build_options": _opt(("list", {"name": _req(str), "title": _req(str), "help": _req(str),
                                    "values": _req(("list", {"id": _req(str), "title": _req(str), "text": _req(str),
                                                             "board_name": _req(str)}))})),
    "ci": _opt({"extra": _req(("list", {"name": _req(str), "make": _req(STR_LIST)}))}),
    "flash": _req({
        "family": _req(tuple(FAMILIES)),
        "keep_codec": _req(bool),
        "sectors": _opt(str),
        "stock_download": _opt(str),
        "stock_versions": _req(STR_LIST),
        "backup": _req({"stock": _req(("none", "full")), "odr": _req(("none", "full")), "spi_size": _req(int)}),
        "bootloader": _req({"keys": _opt(str), "steps": _req(STR_LIST), "id": _opt(str), "sign": _opt(str)}),
        "dfu": _opt({"vid": _req(int), "pid": _req(int), "string1": _req(str), "product": _req(str),
                     "key_sha256": _req(str), "key_prefix": _req(str)}),
        # a board profile of its platform's stub (platform.toml board_stub): id and tested always, the keys the
        # platform lists in board_stub.keys, nothing another platform's stub takes
        "stub": _opt({"id": _req(int), "tested": _req(bool),
                      "hold_power": _opt(bool), "progmem": _opt(int),	# the HR_C7000 universal stub
                      "stock_console": _opt(STOCK_CONSOLES), "read_rate": _opt(int),
                      # the STM32F4 stub: the SPI flash's bus (SPIn), mode and fastest clock; the pins come from pins.h
                      # (F4_SPI_PINS), and idle_cs names other chip selects on that bus to hold high
                      "spi": _opt({"bus": _req(int), "mode": _req(int), "hz": _req(int)}),
                      "idle_cs": _opt(STR_LIST)}),
    }),
    # stock_version: the stock firmware version whose file carries the codec, as the web flasher asks for the file
    "codec": _req({"tool": _req(CODEC_TOOLS), "stock": _opt(str), "profile": _opt(str), "sources": _opt(STR_LIST),
                   "installed": _opt(STR_LIST), "stock_version": _opt(str)}),
    "frame": _req({"shape": _req(str), "body": _req(str), "accent": _req(str), "lens": _req(RANGE)}),
    # tests/hil: what the health checks expect (test_health.py), and the features built here but not working yet
    # (tools/bringup.py, which checks the feature ids; what the hardware lacks is config.mk's BOARD_UNSUPPORTED, code
    # not written for the radio its BOARD_NOT_WRITTEN)
    "hil": _opt({"display": _opt(str), "features": _opt(STR_LIST), "no_features": _opt(STR_LIST),
                 "flash_jedec": _opt(STR_LIST), "calibration": _opt(bool), "battery_mv": _opt(RANGE),
                 "supply_mv": _opt(RANGE),
                 "codec": _opt(STR_LIST)}),
    "features": _opt(("map", {"state": _req(("unsupported",)), "reason": _req(str)})),
    # the open-firmware CPS radio types the board's hal_cps_t reports (docs/spec/cps-protocol.md 2.2, test_cps.py)
    "cps": _opt({"radio_types": _req(("list", int))}),
}

PLATFORM_SCHEMA = {
    "id": _req(str),
    "isa": _req(str),
    "cpu": _req(str),
    # id and name: the toolchain's CI job; every platform with the same id pins the same toolchain
    "toolchain": _req({"id": _req(str), "name": _req(str), "cc": _req(str), "build": _req(str), "version": _req(str),
                       "sha256": _opt(str), "image": _opt(str)}),
    "stubs": _req(STR_LIST),
    # the stub that takes board profiles ([flash.stub]): its tool, the profile keys every board must give, the keys
    # it takes besides id and tested, and the SoC's SPI alternate function per bus (STM32F4); absent: no board profiles
    "board_stub": _opt({"tool": _req(str), "keys": _req(STR_LIST), "optional": _req(STR_LIST),
                        "spi_af": _opt(("list", int))}),
    "codec_symbols": _req(STR_LIST),
    "flash_base": _req(int),
    "default_sectors": _req(str),
    "sectors": _req(("map", ("list", RANGE))),
}


def validate(value, spec, where):
    """Raise RegistryError on a missing key, an unknown key or a value of the wrong type."""
    if isinstance(spec, dict):
        if not isinstance(value, dict):
            raise RegistryError("%s: a table expected" % where)
        for k in value:
            if k not in spec:
                raise RegistryError("%s: unknown key %r (known: %s)" % (where, k, ", ".join(sorted(spec))))
        for k, (sub, required) in spec.items():
            if k in value:
                validate(value[k], sub, "%s.%s" % (where, k))
            elif required:
                raise RegistryError("%s: missing key %r" % (where, k))
    elif isinstance(spec, tuple) and spec and spec[0] in ("list", "map"):
        if spec[0] == "list":
            if not isinstance(value, list):
                raise RegistryError("%s: a list expected" % where)
            for i, v in enumerate(value):
                validate(v, spec[1], "%s[%d]" % (where, i))
        else:
            if not isinstance(value, dict):
                raise RegistryError("%s: a table expected" % where)
            for k, v in value.items():
                validate(v, spec[1], "%s.%s" % (where, k))
    elif isinstance(spec, tuple):
        if value not in spec:
            raise RegistryError("%s: %r is not one of %s" % (where, value, ", ".join(spec)))
    elif spec is int:
        if not isinstance(value, int) or isinstance(value, bool):
            raise RegistryError("%s: an integer expected" % where)
    elif not isinstance(value, spec):
        raise RegistryError("%s: %s expected" % (where, spec.__name__))


# ---------------------------------------------------------------------------------------------------------------
# the build's own files, read, never changed

def make_vars(root, board, names, args=()):
    """{name: value} of make variables as the build sees them for BOARD (board.mk, config.mk and their conditionals):
    the Makefile's print-vars target. args: more make arguments (BAND=vhf, VARIANT=amateur)."""
    import subprocess
    # Without the calling make's variables: run from a build's registry rule, MAKEFLAGS would carry its BUILD and
    # CONFIG_ overrides into this make, which then read another build's values and touched its build directory
    env = {k: v for k, v in os.environ.items() if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES")}
    try:
        r = subprocess.run(["make", "-s", "--no-print-directory", "-C", root, "BOARD=" + board, *args,
                            "print-vars", "VARS=" + " ".join(names)], capture_output=True, text=True, timeout=60,
                           env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RegistryError("make print-vars for %s: %s" % (board, e))
    if r.returncode != 0:
        raise RegistryError("make print-vars for %s failed: %s" % (board, (r.stderr or r.stdout).strip()[-300:]))
    out = {}
    for line in r.stdout.splitlines():
        k, sep, v = line.partition("=")
        if sep and k in names:
            out[k] = " ".join(v.split())
    return out


def ld_expr(text, names=None):
    """The value of a linker script constant expression: numbers (hex, decimal, K and M suffixes), + - * / ( ), and the
    symbols in names (the --defsym values and the script's own constants)."""
    names = names or {}
    s = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: "(%d)" % names[m.group(0)] if m.group(0) in names else m.group(0), text)
    s = re.sub(r"\b(\d+)([KM])\b", lambda m: "(%s*%d)" % (m.group(1), 1024 if m.group(2) == "K" else 1048576), s)
    if not re.fullmatch(r"[0-9a-fA-FxX+\-*/() \t]+", s):
        raise RegistryError("can't evaluate linker expression %r" % text)
    return int(eval(s.replace("/", "//"), {"__builtins__": {}}, {}))


def parse_ld(text, defs=None):
    """{"symbols": {__codec_*: value}, "memory": {region: (origin, length)}} of a linker script (a board's memory.ld).
    defs: the symbols the Makefile defines (--defsym __app_base, __app_start). The script's own constant assignments
    before MEMORY count too; anything after it (INCLUDE common.ld, SECTIONS) is layout, not read."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    block = re.search(r"MEMORY\s*\{(.*?)\}", text, re.S)
    names = dict(defs or {})
    for m in re.finditer(r"(?m)^\s*(\w+)\s*=\s*([^;]+);", text[:block.start()] if block else text):
        try:
            names[m.group(1)] = ld_expr(m.group(2), names)
        except RegistryError:
            pass				# not a constant (ORIGIN(), LOADADDR()): layout, not a fact the tools read
    syms = {m.group(1): ld_expr(m.group(2), names)
            for m in re.finditer(r"(?m)^\s*(__codec_\w+)\s*=\s*([^;]+);", text)}
    mem = {}
    if block:
        for m in re.finditer(r"(?m)^\s*(\w+)\s*(?:\([^)]*\))?\s*:\s*ORIGIN\s*=\s*([^,]+),\s*LENGTH\s*=\s*(.+?)\s*$",
                             block.group(1)):
            mem[m.group(1)] = (ld_expr(m.group(2), names), ld_expr(m.group(3), names))
    return {"symbols": syms, "memory": mem}


def _strip_c_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def _braced(text, start):
    """The text from start (just after an opening brace) to its closing brace."""
    depth, i = 1, start
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[start:i]


def _c_sources(board_dir):
    for f in sorted(os.listdir(board_dir)):
        if f.endswith(".c"):
            with open(os.path.join(board_dir, f), encoding="utf-8") as fh:
                yield f, _strip_c_comments(fh.read())


def source_volts(board_dir):
    """The voltage a board measures, from its sources as hal/board.h board_volts() decides it: "battery" unless board_t
    .caps has CAP_NO_BATTERY, then "supply" when a hal_power_t sets .supply_mv, else "none"."""
    texts = [t for _, t in _c_sources(board_dir)]
    if not any(re.search(r"\.caps\s*=[^;}]*\bCAP_NO_BATTERY\b", t) for t in texts):
        return "battery"
    return "supply" if any(re.search(r"\.supply_mv\s*=", t) for t in texts) else "none"


def board_t_names(board_dir):
    """What board_t .name is set to in a board's sources: [(literal or None, macro or None)], one per board_t
    definition. The one parser of it (bringup.py uses it too): any whitespace, comments ignored; a macro is returned by
    name (such a board names its values in [[build_options]] board_name)."""
    out = []
    for _, text in _c_sources(board_dir):
        for m in re.finditer(r"\bboard_t\s+\w+\s*=\s*\{", text):
            body = _braced(text, m.end())
            n = re.search(r'\.name\s*=\s*(?:"([^"]*)"|([A-Za-z_]\w*))', body)
            if n:
                out.append((n.group(1), n.group(2)))
    return out


def flash_map(board_dir):
    """The FLASH_MAP_FIELDS of the board's hal_flash_map_t definition, {field: address}. A board has one; a second one
    that disagrees is an error."""
    out, where = {}, None
    for f, text in _c_sources(board_dir):
        for m in re.finditer(r"\bhal_flash_map_t\s+\w+\s*=\s*\{", text):
            found = {k: int(v, 0) for k, v in re.findall(r"\.(\w+)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*",
                                                        _braced(text, m.end())) if k in FLASH_MAP_FIELDS}
            if where is not None and found != out:
                raise RegistryError("%s: hal_flash_map_t in %s and %s differ" % (os.path.relpath(board_dir, ROOT),
                                                                                 where, f))
            out, where = found, f
    return out


UNSUPPORTED_RE = re.compile(r"^\s*BOARD_UNSUPPORTED\s*[:+]?=([^#]*)(?:#(.*))?$")


def unsupported_reasons(config_mk):
    """{switch: reason} from config.mk's BOARD_UNSUPPORTED lines, one switch a line with its reason in the comment
    (BOARD_UNSUPPORTED += AM_RX\t# no air band receiver). A line with several switches gives each the reason ""."""
    out = {}
    try:
        lines = open(config_mk, encoding="utf-8").read().splitlines()
    except OSError:
        return out
    for line in lines:
        m = UNSUPPORTED_RE.match(line)
        if m:
            sws = m.group(1).split()
            for s in sws:
                out[s] = (m.group(2) or "").strip() if len(sws) == 1 else ""
    return out


NOT_WRITTEN_RE = re.compile(r"^\s*BOARD_NOT_WRITTEN\s*[:+]?=([^#]*)(?:#(.*))?$")


def not_written_reasons(config_mk):
    """{switch: reason} from config.mk's BOARD_NOT_WRITTEN lines, in BOARD_UNSUPPORTED's form: the switches the
    radio's hardware can do but whose driver or glue isn't written yet (docs/build-config.md)."""
    out = {}
    try:
        lines = open(config_mk, encoding="utf-8").read().splitlines()
    except OSError:
        return out
    for line in lines:
        m = NOT_WRITTEN_RE.match(line)
        if m:
            sws = m.group(1).split()
            for s in sws:
                out[s] = (m.group(2) or "").strip() if len(sws) == 1 else ""
    return out


def not_written_problems(bid, b):
    """config.mk's BOARD_NOT_WRITTEN of a board: each switch with its reason, and none also in BOARD_UNSUPPORTED (the
    hardware either can or can't)."""
    out = []
    for s, why in b.not_written.items():
        if not why:
            out.append("boards/%s/config.mk: BOARD_NOT_WRITTEN %s has no reason: one switch a line, "
                       "BOARD_NOT_WRITTEN += %s  # <what isn't written>" % (bid, s, s))
        if s in b.unsupported:
            out.append("boards/%s: %s is in both BOARD_UNSUPPORTED and BOARD_NOT_WRITTEN: UNSUPPORTED only when the "
                       "hardware can't do it (docs/build-config.md)" % (bid, s))
    return out


def pin_defines(path):
    """{NAME: value} of the pin #defines in a pins.h: an integer (the HR_C7000's port << 4 | pin), or "PB3" for the
    STM32 form `GPIOB, GPIO_PIN_3`."""
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as f:
        text = _strip_c_comments(f.read())
    out = {m.group(1): int(m.group(2), 0)
           for m in re.finditer(r"(?m)^\s*#\s*define\s+(\w+)\s+\(?((?:0[xX][0-9a-fA-F]+|\d+))[uU]?\)?\s*$", text)}
    out.update({m.group(1): "P%s%d" % (m.group(2), int(m.group(3)))
                for m in re.finditer(r"(?m)^\s*#\s*define\s+(\w+)\s+GPIO([A-K])\s*,\s*GPIO_PIN_(\d+)\s*$", text)})
    return out


# ---------------------------------------------------------------------------------------------------------------
# the loader

class Platform:
    def __init__(self, data):
        self.data = data
        self.id = data["id"]


class Board:
    """A board's TOML fields (data) and what is derived from its build files."""

    def __init__(self, data, platform, mk, layout, names, pins, unsupported=None, fmap=None):
        self.data = data
        self.id = data["id"]
        self.platform = platform
        self.mk = mk				# the make variables of MAKE_VARS and the build options
        self.symbols = layout["symbols"]	# __codec_* of every linker script in the board directory
        self.app_region = layout["flash"]	# (start, end) of MEMORY FLASH in the build's LDSCRIPT
        self.source_names = names		# board_t .name: [(literal, macro)]
        self.pins = pins			# pins.h integer defines
        self.flash_map = fmap or {}		# hal_flash_map_t: FLASH_MAP_FIELDS
        # config.mk BOARD_UNSUPPORTED: {switch: reason}, in the build's order (make's value decides the set)
        reasons = unsupported or {}
        self.unsupported = {s: reasons.get(s, "") for s in mk.get("BOARD_UNSUPPORTED", "").split()}
        # config.mk BOARD_NOT_WRITTEN, the same way (load() fills it)
        self.not_written = {}
        boot = mk.get("BOOT_STAGE")
        self.first_stage = None
        if boot:
            if not mk.get("BOOT_SIZE") or not mk.get("BOOT_SIGNATURE"):
                raise RegistryError("%s: BOOT_STAGE without BOOT_SIZE and BOOT_SIGNATURE in board.mk" % self.id)
            self.first_stage = (int(mk["BOOT_SIZE"], 0), bytes.fromhex(mk["BOOT_SIGNATURE"]))
        # the image odrflash writes starts at app_base: the first stage, when there is one, then the application
        self.app_base = self.app_region[0] - (self.first_stage[0] if self.first_stage else 0)

    def get(self, path, default=None):
        v = self.data
        for k in path.split("."):
            if not isinstance(v, dict) or k not in v:
                return default
            v = v[k]
        return v

    @property
    def family(self):
        return FAMILIES[self.data["flash"]["family"]]

    @property
    def link(self):
        return self.family["link"]

    @property
    def cpu(self):
        return self.data["hardware"].get("cpu", self.platform.data["cpu"])

    @property
    def protected(self):
        """The flash below the image, never written: the factory loader's (platform flash_base to app_base)."""
        return [(self.platform.data["flash_base"], self.app_base)]

    @property
    def nand(self):
        return NAND_DRIVER in self.mk.get("BOARD_DIRS", "").split()

    def debuglink_names(self):
        """The VERSION board= values that mean this board: board_t's name (or each build option value's), the
        older names in debuglink_aliases, and the id."""
        names = [lit for lit, _ in self.source_names if lit] + self.option_board_names()
        names += self.data.get("debuglink_aliases", []) + [self.id]
        return list(dict.fromkeys(names))

    def stub(self):
        """The board's profile for its platform's stub (platform.toml board_stub.tool), with what other files say.
        stubtool (the HR_C7000 universal stub): the power latch and key from pins.h when the stub must hold the radio
        on (a soft power key), else none. f4stub: the board record's facts, the SPI flash pins from pins.h, the crystal
        from HSE_VALUE and the bootloader's end from APP_BASE. None without a profile."""
        s = self.data["flash"].get("stub")
        bs = self.platform.data.get("board_stub")
        if s is None or bs is None:
            return None
        out = {"tool": bs["tool"], "id": s["id"], "tested": s["tested"]}
        if "spi" in s:
            spi, af = s["spi"], bs.get("spi_af") or []
            pin = {k: self.pins.get(k) for k in F4_SPI_PINS}
            out.update(hse=int((self.mk.get("HSE_VALUE") or "0").rstrip("uUlL") or "0", 0), boot_end=self.app_base,
                       spi=dict(spi, af=af[spi["bus"] - 1] if 0 < spi["bus"] <= len(af) else None,
                                sck=pin["PIN_FLASH_SCK"], miso=pin["PIN_FLASH_MISO"], mosi=pin["PIN_FLASH_MOSI"],
                                cs=pin["PIN_FLASH_CS"], idle=[self.pins.get(n) for n in s.get("idle_cs", [])]))
        if "hold_power" in s:
            hold = s["hold_power"]
            out.update(latch=self.pins.get("PIN_POWER_LATCH", NO_PIN) if hold else NO_PIN,
                       key=self.pins.get("PIN_POWER_KEY", NO_PIN) if hold else NO_PIN, nand=self.nand,
                       progmem=s["progmem"], stock_console=s.get("stock_console"))
        return out

    def codeplug(self):
        """Where the codeplug lives, for the codeplug editor: the data flash address of the open-firmware image (the
        editor adds it on the wire), the end of the room for it and the DMR ID database, whether the radio takes a
        codeplug over TFTP (a staging area), and the names the radio reports on the debug link. None without a flash
        map."""
        m = self.flash_map
        if "openfw_eeprom" not in m or "openfw_end" not in m:
            return None
        return {"base": m["openfw_eeprom"], "end": m["openfw_end"], "tftp": "staging_addr" in m,
                "names": sorted(set(self.debuglink_names()))}

    @property
    def codec_slot(self):
        s = self.symbols
        if "__codec_slot" in s and "__codec_slot_end" in s:
            return (s["__codec_slot"], s["__codec_slot_end"])
        return None

    def window(self, lo, hi):
        s = self.symbols
        return (s[lo], s[hi]) if lo in s and hi in s else None

    def sector_name(self):
        return self.data["flash"].get("sectors", self.platform.data["default_sectors"])

    def sectors(self):
        """[(index, address, size)] the bootloader can erase, in its order: the platform geometry's sectors from the
        image up, or the application and the codec slot."""
        name = self.sector_name()
        if name == APP_SLOT:
            slot = self.codec_slot
            if slot is None:
                raise RegistryError("%s: [flash] sectors = %r needs __codec_slot and __codec_slot_end in a linker script"
                                    % (self.id, APP_SLOT))
            return [(0, self.app_base, slot[0] - self.app_base), (1, slot[0], slot[1] - slot[0])]
        geo = self.platform.data["sectors"].get(name)
        if geo is None:
            raise RegistryError("%s: [flash] sectors = %r is neither %r nor a geometry of platform/%s/platform.toml"
                                % (self.id, name, APP_SLOT, self.platform.id))
        return [tuple(s) for s in geo if s[1] >= self.app_base]

    def flash_end(self):
        s = self.sectors()
        return s[-1][1] + s[-1][2]

    def codec(self):
        """{"profile", "windows": [(addr, length)], "sources", "installed", "code"} for a codec in a linker slot, or
        None for a stock-file placement ([codec] stock, the SHA-256 of each stock file the codec merges from: odrflash
        reads each file's windows from its stock-file tables, and the board's are their union)."""
        c = self.data["codec"]
        if c.get("stock"):
            return None
        slot = self.codec_slot
        if slot is None or not c.get("profile"):
            raise RegistryError("%s: [codec] needs stock (a stock-file placement) or profile with a linker codec slot"
                                % self.id)
        return {"profile": c["profile"], "windows": [(slot[0], slot[1] - slot[0])], "sources": c.get("sources", []),
                "installed": c.get("installed", []), "code": slot[0]}

    def option_board_names(self):
        return [v["board_name"] for o in self.data.get("build_options", []) for v in o["values"]]

    def builds(self):
        """The images besides the default build: [(name, [make args])], one per non-default build option value."""
        out = []
        for o in self.data.get("build_options", []):
            for v in o["values"][1:]:
                out.append((v["id"], ["%s=%s" % (o["name"], v["id"])]))
        return out

    def ci_extra(self):
        return [(e["name"], list(e["make"])) for e in self.data.get("ci", {}).get("extra", [])]


def _read_toml(path):
    tl = _tomllib()
    with open(path, "rb") as f:
        try:
            return tl.load(f)
        except tl.TOMLDecodeError as e:
            raise RegistryError("%s: %s" % (os.path.relpath(path, ROOT), e))


def board_dirs(root=ROOT):
    """Every boards/<id>/ with a board.mk (a board the build knows)."""
    base = os.path.join(root, "boards")
    return sorted(d for d in os.listdir(base) if os.path.isfile(os.path.join(base, d, "board.mk")))


def load_platforms(root=ROOT):
    out = {}
    base = os.path.join(root, "platform")
    for p in sorted(os.listdir(base)):
        path = os.path.join(base, p, "platform.toml")
        if not os.path.isfile(path):
            continue
        data = _read_toml(path)
        validate(data, PLATFORM_SCHEMA, "platform/%s/platform.toml" % p)
        if data["id"] != p:
            raise RegistryError("platform/%s/platform.toml: id %r must be the directory name" % (p, data["id"]))
        out[p] = Platform(data)
    return out


def _layout(root, bid, mk):
    """The board's linker facts: the build's LDSCRIPT for the application region, every *.ld in its directory for the
    codec symbols (a configuration may switch scripts: they must agree)."""
    bdir = os.path.join(root, "boards", bid)
    ld = mk.get("LDSCRIPT")
    if not ld:
        raise RegistryError("boards/%s: no LDSCRIPT (the platform's default is boards/<id>/memory.ld)" % bid)
    if not mk.get("APP_BASE"):
        raise RegistryError("boards/%s/board.mk: no APP_BASE" % bid)
    # the Makefile's --defsym values: the image address, and the application's first byte after any first stage
    base = int(mk["APP_BASE"], 0)
    start = base + (int(mk["BOOT_SIZE"], 0) if mk.get("BOOT_STAGE") else 0)
    defs = {"__app_base": base, "__app_start": start}
    with open(os.path.join(root, ld), encoding="utf-8") as f:
        main = parse_ld(f.read(), defs)
    if "FLASH" not in main["memory"]:
        raise RegistryError("%s: no MEMORY FLASH region" % ld)
    org, length = main["memory"]["FLASH"]
    syms = {}
    for f in sorted(os.listdir(bdir)):
        if f.endswith(".ld"):
            with open(os.path.join(bdir, f), encoding="utf-8") as fh:
                for k, v in parse_ld(fh.read(), defs)["symbols"].items():
                    if syms.setdefault(k, v) != v:
                        raise RegistryError("boards/%s: %s differs between the linker scripts" % (bid, k))
    return {"flash": (org, org + length), "symbols": syms}


def load(root=ROOT, only=None):
    """{id: Board} for every board directory with a board.mk (or the ids in only). Fails on a missing or malformed
    board.toml."""
    platforms = load_platforms(root)
    out = {}
    dirs = board_dirs(root)
    for bid in only or ():
        if bid not in dirs:
            raise RegistryError("unknown board %r (known: %s)" % (bid, ", ".join(dirs)))
    for bid in (only or dirs):
        bdir = os.path.join(root, "boards", bid)
        path = os.path.join(bdir, "board.toml")
        if not os.path.isfile(path):
            raise RegistryError("boards/%s has a board.mk but no board.toml (docs/dev/adding-a-radio.md)" % bid)
        data = _read_toml(path)
        validate(data, BOARD_SCHEMA, "boards/%s/board.toml" % bid)
        if data["id"] != bid:
            raise RegistryError("boards/%s/board.toml: id %r must be the directory name" % (bid, data["id"]))
        mk = make_vars(root, bid, MAKE_VARS + tuple(o["name"] + "_VALUES" for o in data.get("build_options", [])))
        plat = platforms.get(mk.get("PLATFORM", ""))
        if plat is None:
            raise RegistryError("boards/%s: PLATFORM %r has no platform/<p>/platform.toml" % (bid, mk.get("PLATFORM")))
        out[bid] = Board(data, plat, mk, _layout(root, bid, mk), board_t_names(bdir),
                         pin_defines(os.path.join(bdir, "pins.h")),
                         # the platform's config.mk adds its own switches (no USB on a platform without it); the
                         # board's reason wins for a switch both list
                         {**unsupported_reasons(os.path.join(root, "platform", plat.id, "config.mk")),
                          **unsupported_reasons(os.path.join(bdir, "config.mk"))}, flash_map(bdir))
        out[bid].volts = source_volts(bdir)
        nw = {**not_written_reasons(os.path.join(root, "platform", plat.id, "config.mk")),
              **not_written_reasons(os.path.join(bdir, "config.mk"))}
        out[bid].not_written = {s: nw.get(s, "") for s in mk.get("BOARD_NOT_WRITTEN", "").split()}
    return out


# ---------------------------------------------------------------------------------------------------------------
# consistency

GALLERY_RE = re.compile(r"^\d+x\d+(i\d+(x\d+)?|m)?$")	# a display class tests/host/sim/ui_sim.c sim_geom_find() takes
UI_SIM = "tests/host/sim/ui_sim.c"


def gallery_classes(root=ROOT):
    """The display classes `make ui-gallery` renders (the names in ui_sim.c's sim_geoms table), or None without it."""
    path = os.path.join(root, UI_SIM)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        m = re.search(r"sim_geoms\[\]\s*=\s*\{(.*?)\n\};", f.read(), re.S)
    return set(re.findall(r'\{\s*"([^"]+)"', m.group(1))) if m else set()


def hardware_switches(root=ROOT):
    """{switch: what it needs}: the features of tests/hil/features.toml with a hardware field."""
    path = os.path.join(root, "tests", "hil", "features.toml")
    if not os.path.isfile(path):
        return {}
    feats = _read_toml(path).get("feature", {})
    return {e["switch"]: e["hardware"] for e in feats.values() if e.get("switch") and e.get("hardware")}


def hardware_problems(bid, unsupported, unconfirmed, built, hw):
    """A board's hardware switches that read "off by default": each must be built (built: switch -> "y"/"n"), listed in
    BOARD_UNSUPPORTED or BOARD_NOT_WRITTEN (unsupported: both, with their reasons), or, while the fact is unknown, in
    board.toml [hardware] unconfirmed. One in either list must also be off by default."""
    out = []
    for s, what in sorted(hw.items()):
        on, gone = built.get(s) == "y", s in unsupported
        if s in unconfirmed:
            if on or gone:
                out.append("boards/%s/board.toml: hardware.unconfirmed %s, but the board %s: drop the entry"
                           % (bid, s, "builds it" if on else "lists it in BOARD_UNSUPPORTED or BOARD_NOT_WRITTEN"))
        elif on and gone:
            out.append("boards/%s: %s is in BOARD_UNSUPPORTED or BOARD_NOT_WRITTEN (it needs %s) but built by default: "
                       "add CONFIG_%s ?= n to its config.mk" % (bid, s, what, s))
        elif not on and not gone:
            out.append("boards/%s: %s needs %s and is off by default: build it (config.mk CONFIG_%s ?= y) if the "
                       "radio has it and the code is written; BOARD_NOT_WRITTEN += %s  # <what isn't written> if "
                       "the radio has it but the code isn't; else BOARD_UNSUPPORTED += %s  # <the reason, from the "
                       "hardware spec>. A fact no source has yet goes in board.toml [hardware] unconfirmed"
                       % (bid, s, what, s, s, s))
    for s in sorted(set(unconfirmed) - set(hw)):
        out.append("boards/%s/board.toml: hardware.unconfirmed %s is not a hardware switch (tests/hil/features.toml)"
                   % (bid, s))
    return out


def check(root=ROOT):
    """Every problem found, as strings (empty: the registry is consistent)."""
    problems = []
    try:
        boards = load(root)
    except RegistryError as e:
        return [str(e)]
    hw = hardware_switches(root)
    stub_ids, orders = {}, {}
    classes = gallery_classes(root)
    for bid, b in boards.items():
        d = b.data
        where = "boards/%s/board.toml" % bid

        def bad(msg):
            problems.append("%s: %s" % (where, msg))
        for s, why in b.unsupported.items():
            if not why:
                problems.append("boards/%s/config.mk: BOARD_UNSUPPORTED %s has no reason: one switch a line, "
                                "BOARD_UNSUPPORTED += %s  # <reason>" % (bid, s, s))
        problems += not_written_problems(bid, b)
        if hw:
            try:
                built = {s: v for s, v in ((k[len("CONFIG_"):], v) for k, v in
                                           make_vars(root, bid, ["CONFIG_" + s for s in hw]).items())}
                # not written yet counts as left off on purpose, as unsupported does
                problems += hardware_problems(bid, {**b.unsupported, **b.not_written},
                                              d["hardware"].get("unconfirmed", {}), built, hw)
            except RegistryError as e:
                problems.append(str(e))
        for k, p in d["docs"].items():
            if not os.path.isfile(os.path.join(root, p)):
                bad("docs.%s %s doesn't exist" % (k, p))
        if d.get("part_of") is not None and d["part_of"] not in boards:
            bad("part_of %r is not a board" % d["part_of"])
        if not b.source_names:
            bad("no board_t definition with .name in boards/%s/*.c" % bid)
        if d["order"] in orders:
            bad("order %d is also the %s's" % (d["order"], orders[d["order"]]))
        orders.setdefault(d["order"], bid)
        fm = b.flash_map
        if b.codeplug() is None:
            bad("no hal_flash_map_t with openfw_eeprom and openfw_end in boards/%s/*.c" % bid)
        elif fm.get("openfw_flash", fm["openfw_eeprom"] + OPENFW_FLASH_OFFSET) - fm["openfw_eeprom"] \
                != OPENFW_FLASH_OFFSET:
            bad("hal_flash_map_t openfw_flash isn't openfw_eeprom + 0x%X (the codeplug editor assumes it)"
                % OPENFW_FLASH_OFFSET)
        for lit, macro in b.source_names:
            if macro and not d.get("build_options"):
                bad("board_t .name is the macro %s: name its values in [[build_options]] board_name" % macro)
        if not GALLERY_RE.match(d["hardware"]["gallery"]):
            bad("hardware.gallery %r is not a display class (WxH, WxHiN, WxHiNxM, WxHm)" % d["hardware"]["gallery"])
        elif classes is not None and d["hardware"]["gallery"] not in classes:
            # the site links docs/ux/screens/<class>/, which only the classes in sim_geoms get
            bad("hardware.gallery %r is not in %s sim_geoms (make ui-gallery renders only those)"
                % (d["hardware"]["gallery"], UI_SIM))
        fl = d["flash"]
        try:
            secs = b.sectors()
            codec = b.codec()
        except RegistryError as e:
            problems.append(str(e))
            continue
        for (_, a0, s0), (_, a1, _) in zip(secs, secs[1:]):
            if a0 + s0 > a1:
                bad("flash sectors overlap or are out of order at 0x%08X" % a1)
        if not secs or secs[0][1] != b.app_base:
            bad("no sector of %s starts where the image does (linker FLASH origin%s), 0x%08X"
                % (b.sector_name(), " minus the first stage" if b.first_stage else "", b.app_base))
        if b.app_base <= b.platform.data["flash_base"]:
            bad("the image starts at 0x%08X, at or below the platform's flash_base: nothing is protected" % b.app_base)
        if codec is not None:
            for a, n in codec["windows"]:
                if a < b.app_region[1] or a + n > b.flash_end():
                    bad("codec window 0x%08X+0x%X is outside the flash the linker script keeps free (0x%08X..0x%08X)"
                        % (a, n, b.app_region[1], b.flash_end()))
        cst = d["codec"].get("stock")
        if cst is not None:
            if not cst or len(set(cst)) != len(cst):
                bad("codec.stock lists each stock file once, at least one")
            if any(not re.fullmatch(r"[0-9a-f]{64}", h) for h in cst):
                bad("codec.stock entries are SHA-256 digests (64 lowercase hex digits)")
            for k in ("profile", "sources"):
                if k in d["codec"]:
                    bad("codec.%s comes from the stock files' profiles when codec.stock is set" % k)
        if not os.path.isfile(os.path.join(root, "tools", d["codec"]["tool"] + ".py")):
            bad("codec.tool tools/%s.py doesn't exist" % d["codec"]["tool"])
        if codec is not None and not codec["profile"].startswith(bid + "-"):
            bad("codec.profile %r must start with %r: the firmware reports <id>-<stock tag> (ODR_BOARD_ID)"
                % (codec["profile"], bid + "-"))
        for sym in b.platform.data["codec_symbols"]:
            if sym not in b.symbols:
                bad("the linker scripts define no %s (platform/%s/platform.toml codec_symbols)" % (sym, b.platform.id))
        for need in b.family["needs"]:
            if b.get(need) is None:
                bad("family %s needs %s" % (fl["family"], need))
        if "dfu" in fl and "flash.dfu" not in b.family["needs"]:
            bad("[flash.dfu] is for a family that needs it (%s)"
                % ", ".join(f for f, v in FAMILIES.items() if "flash.dfu" in v["needs"]))
        if "dfu" in fl and (not re.fullmatch(r"[0-9a-f]{64}", fl["dfu"]["key_sha256"])
                            or not re.fullmatch(r"[0-9A-F]{8}", fl["dfu"]["key_prefix"])):
            bad("flash.dfu key_sha256 is 64 lower-case hex digits, key_prefix 8 upper-case")
        stub = fl.get("stub")
        bs = b.platform.data.get("board_stub")
        if stub is not None:
            if bs is None:
                bad("[flash.stub] is a board profile of its platform's stub: platform %s takes none (board_stub)"
                    % b.platform.id)
            else:
                for k in bs["keys"]:
                    if k not in stub:
                        bad("flash.stub needs %s on platform %s (platform.toml board_stub.keys)" % (k, b.platform.id))
                for k in set(stub) - {"id", "tested"} - set(bs["keys"]) - set(bs["optional"]):
                    bad("flash.stub %s is not a key of the %s stub (platform.toml board_stub)" % (k, bs["tool"]))
                # ids are per stub: each stub has its own table of boards
                if stub["id"] == STUB_TEST_ID:
                    bad("flash.stub id 0x%02X is reserved for the stub tools' test profiles" % STUB_TEST_ID)
                elif (bs["tool"], stub["id"]) in stub_ids:
                    bad("flash.stub id %d is also the %s's" % (stub["id"], stub_ids[(bs["tool"], stub["id"])]))
                else:
                    stub_ids[(bs["tool"], stub["id"])] = bid
            if stub.get("hold_power") and not {"PIN_POWER_LATCH", "PIN_POWER_KEY"} <= set(b.pins):
                bad("flash.stub hold_power needs PIN_POWER_LATCH and PIN_POWER_KEY in boards/%s/pins.h" % bid)
            if "spi" in stub:
                prof = b.stub()
                for k in F4_SPI_PINS + tuple(stub.get("idle_cs", [])):
                    if not isinstance(b.pins.get(k), str):
                        bad("flash.stub spi needs %s in boards/%s/pins.h as GPIOx, GPIO_PIN_n" % (k, bid))
                if prof["spi"]["af"] is None:
                    bad("flash.stub spi bus %d has no alternate function in platform.toml board_stub.spi_af"
                        % stub["spi"]["bus"])
                if prof["hse"] <= 0:
                    bad("flash.stub spi: the board record needs the crystal, HSE_VALUE in board.mk")
                if stub["spi"]["mode"] not in range(4):
                    bad("flash.stub spi mode is 0 to 3")
        if b.first_stage and len(b.first_stage[1]) != 4:
            bad("board.mk BOOT_SIGNATURE is the first stage's first word: 4 bytes")
        # board.mk declares the options and their values (the build checks a value against them); board.toml adds
        # only the words: both must name the same values in the same order (the first is the default)
        if [o["name"] for o in d.get("build_options", [])] != (b.mk.get("BUILD_OPTIONS") or "").split():
            bad("board.mk BUILD_OPTIONS is %r; [[build_options]] names %s"
                % (b.mk.get("BUILD_OPTIONS"), [o["name"] for o in d.get("build_options", [])]))
        for o in d.get("build_options", []):
            ids, mk_ids = [v["id"] for v in o["values"]], (b.mk.get(o["name"] + "_VALUES") or "").split()
            if len(ids) < 2:
                bad("build option %s needs at least two values (the first is the default)" % o["name"])
            elif ids != mk_ids:
                bad("build option %s: board.toml has the values %s, board.mk %s_VALUES %s"
                    % (o["name"], ids, o["name"], mk_ids))
    if not problems:
        # the build view's names are the Makefile's: every build in full, the first one also in the first variant
        bv = build_view(boards, root)
        for bid, e in bv["boards"].items():
            for i, bl in enumerate(e["builds"]):
                for var in ["full"] + bv["variants"][1:2] * (i == 0):
                    vs = "" if var == "full" else "-" + var
                    args = bl["make"][1:] + ([] if var == "full" else ["VARIANT=" + var])
                    try:
                        mk = make_vars(root, bid, ["BUILD", "TARGET"], args)
                    except RegistryError as err:
                        problems.append(str(err))
                        continue
                    want = (bl["build_dir"].replace("{variant}", vs),
                            bl["image"].replace("{variant}", vs).replace("_install.bin", ".bin"))
                    if (mk.get("BUILD"), mk.get("TARGET", "") + ".bin") != want:
                        problems.append("%s %s: make names the build %s, %s.bin; the build view says %s, %s"
                                        % (bid, " ".join(args), mk.get("BUILD"), mk.get("TARGET"), *want))
    # check 6: one pin per toolchain, and no copy of it elsewhere that differs
    problems += pin_problems(root, load_platforms(root))
    # check 5: the radio survey's Port cells (a fixture tree without the survey has none to check)
    if os.path.isfile(os.path.join(root, registry_docs.SURVEY)):
        try:
            problems += registry_docs.survey(root, boards)[1]
        except registry_docs.DocsError as e:
            problems.append(str(e))
    if not problems:
        path = os.path.join(root, ODRFLASH_JSON)
        try:
            with open(path, encoding="utf-8") as f:
                current = f.read() == dumps(odrflash_view(boards))
        except OSError:
            current = False
        if not current:
            problems.append("%s is out of date: run tools/boards.py json --for odrflash --out %s"
                            % (ODRFLASH_JSON, ODRFLASH_JSON))
    return problems


# ---------------------------------------------------------------------------------------------------------------
# derived views

def odrflash_view(boards):
    """What the flashing tools read (tools/odrflash/odrflash/registry.json): plain JSON, every address a number.
    Its keys are the attribute names of odrflash.boards.Board, which reads it as is."""
    out = {}
    for bid, b in sorted(boards.items()):
        d, fl = b.data, b.data["flash"]
        codec = b.codec()
        dfu = fl.get("dfu")
        e = {
            "id": bid, "name": d["name"], "model": d["model"], "part_of": d.get("part_of"),
            "platform": b.platform.id, "platform_stubs": list(b.platform.data["stubs"]), "family": fl["family"],
            "link": b.link, "release": d["status"]["release"], "debuglink_names": b.debuglink_names(),
            "app_base": b.app_base, "flash_end": b.flash_end(), "app_region": list(b.app_region),
            "sectors": [list(s) for s in b.sectors()], "protected": [list(p) for p in b.protected],
            "keep_codec_supported": fl["keep_codec"],
            "first_stage": [b.first_stage[0], b.first_stage[1].hex()] if b.first_stage else None,
            "codec_tool": d["codec"]["tool"], "codec_stock": list(d["codec"].get("stock", [])),
            # a stock-file placement has no id or windows here: odrflash reads them from its stock-file tables
            "codec": {"id": codec["profile"] if codec else None,
                      "windows": [list(w) for w in codec["windows"]] if codec else None,
                      "installed": d["codec"].get("installed", []), "sources": d["codec"].get("sources")},
            "codec_code": codec["code"] if codec else None,
            "codec_slot": list(b.codec_slot) if b.codec_slot else None,
            "codec_run": list(b.window("__codec_run_lo", "__codec_run_hi") or ()) or None,
            "codec_tables": list(b.window("__codec_tab_lo", "__codec_tab_hi") or ()) or None,
            "loader_id": fl["bootloader"].get("id"), "loader_keys": fl["bootloader"].get("keys"),
            "loader_sign": fl["bootloader"].get("sign", ""),
            "key_sha256": dfu["key_sha256"] if dfu else None, "key_prefix": dfu["key_prefix"] if dfu else None,
            "dfu_ids": [dfu["vid"], dfu["pid"]] if dfu else None,
            "dfu_string1": dfu["string1"] if dfu else None, "dfu_product": dfu["product"] if dfu else None,
            "stub": b.stub(),
            "build_options": d.get("build_options", []),		# odrflash asks with the same words as the site
        }
        out[bid] = e
    return {"format": "odr-board-registry/2",
            "_about": "Generated by tools/boards.py json --for odrflash from boards/*/board.toml, platform/*/platform.toml "
                      "and the boards' build files. Don't edit: change the sources and run it again.",
            "boards": out}


BUILD_FORMAT = "odr-build-matrix/1"


def _names():
    """odrflash/names.py, the one naming rule for build directories, images and release files. It imports nothing, so
    loading it doesn't load the generated registry."""
    sys.path.insert(0, os.path.join(ROOT, "tools", "odrflash"))
    try:
        from odrflash import names
    finally:
        sys.path.pop(0)
    return names


def build_view(boards, root=ROOT):
    """What a tool that builds from a checkout needs (docs/dev/board-registry.md "The build view"): every board with its
    platform, build options and release state, each build's make arguments and file names, and each platform's
    toolchain. A stable interface: a change that breaks a reader bumps BUILD_FORMAT."""
    names = _names()
    vdir = os.path.join(root, "config", "variants")
    variants = sorted(f[:-3] for f in os.listdir(vdir) if f.endswith(".mk")) if os.path.isdir(vdir) else []
    plats = {}
    for pid, pl in sorted(load_platforms(root).items()):
        tc = pl.data["toolchain"]
        plats[pid] = {"isa": pl.data["isa"], "cpu": pl.data["cpu"], "toolchain": tc["id"], "cc": tc["cc"],
                      "version": tc["version"],
                      "image": tc.get("image"), "install": tc["build"], "sha256": tc.get("sha256")}
    out = {}
    for bid, b in sorted(boards.items()):
        d = b.data
        opts = [(o["name"], [v["id"] for v in o["values"]]) for o in d.get("build_options", [])]
        builds = []
        for combo in _product(opts):
            chosen = dict(combo)
            builds.append({
                "options": chosen,
                "make": ["BOARD=" + bid] + ["%s=%s" % kv for kv in combo],
                "release": ["--board", bid] + [x for n, v in combo if v != dict(opts)[n][0]
                                               for x in ("--build-option", "%s=%s" % (n, v))],
                "image_id": names.image_id(bid, opts, chosen),
                "build_dir": names.build_dir(bid, opts, chosen, names.VARIANT),
                "image": names.build_image(bid, opts, chosen, names.VARIANT, bool(b.first_stage)),
                "release_image": names.release_name(names.release_id(bid, opts, chosen, names.VARIANT), "{version}"),
                "release_manifest": names.release_name(names.release_id(bid, opts, chosen, names.VARIANT), "{version}",
                                                       "json"),
            })
        out[bid] = {
            "name": d["name"], "platform": b.platform.id, "release": d["status"]["release"],
            "released": d["status"]["release"] == "stable", "first_stage": bool(b.first_stage),
            "build_options": [{"name": n, "default": vals[0], "values": vals} for n, vals in opts],
            "builds": builds,
        }
    return {"format": BUILD_FORMAT, "variants": ["full"] + variants, "platforms": plats, "boards": out}


def _product(opts):
    """Every combination of option values, [(name, value), ...] each, the all-default one first."""
    combos = [[]]
    for n, vals in opts:
        combos = [c + [(n, v)] for c in combos for v in vals]
    return combos
def site_view(boards, root=ROOT):
    """What the web site and the web flasher read (site/odrsite/repo.py), in the boards' order. The radio entries keep
    the keys the pages' scripts use (site/data/radios.json)."""
    out = []
    for bid, b in sorted(boards.items(), key=lambda kv: kv[1].data["order"]):
        d, fl, hw = b.data, b.data["flash"], b.data["hardware"]
        stub = fl.get("stub") or {}
        flash = {"backend": b.family["web"], "board": bid, "family": fl["family"], "link": b.link,
                 "bootloader": list(fl["bootloader"]["steps"]), "stock_versions": list(fl["stock_versions"]),
                 # the image starts with a first stage of this size, its first word this signature (board.mk BOOT_*)
                 "first_stage": {"size": b.first_stage[0], "head": b.first_stage[1].hex()} if b.first_stage else None,
                 "stock_console": stub.get("stock_console"), "update_mode": b.family["update_mode"],
                 "read_rate": stub.get("read_rate"), "codec_version": d["codec"].get("stock_version"),
                 "build_options": json.loads(json.dumps(d.get("build_options", [])))}
        if "stock_download" in fl:
            flash["stock_download"] = fl["stock_download"]
        if stub:
            flash["stub_tested"] = stub["tested"]
        out.append({
            "id": bid, "name": d["name"], "maker": d["maker"], "model": d["model"], "also": list(d["also"]),
            "kind": d["kind"], "order": d["order"], "level": d["status"]["level"], "release": d["status"]["release"],
            "summary": d["status"]["summary"], "bands": hw["bands"], "power": hw["power"],
            "cpu": b.cpu, "display": hw["display"], "gallery": hw["gallery"], "docs": dict(d["docs"]), "flash": flash,
            "backup": dict(fl["backup"]), "frame": json.loads(json.dumps(d["frame"])), "codeplug": b.codeplug(),
            "unsupported": dict(b.unsupported), "not_written": dict(b.not_written), "volts": b.volts,
            "clock": hw["clock"]["rtc"]})
    # the survey's radios without a board, planned or in progress (docs/spec/radios.md Port cells)
    rows, problems = registry_docs.survey(root, boards)
    if problems:
        raise RegistryError("; ".join(problems))
    planned = [{"radio": r["radio"], "status": r["status"], "pr": r["pr"], "hardware_needed": r["hardware_needed"],
                "note": registry_docs.plain(r["note"])} for r in registry_docs.planned(rows)]
    return {"format": "odr-site-boards/1", "boards": out, "planned": planned}


def full_view(boards, root=ROOT):
    """Every board's TOML fields with the derived ones under "derived"."""
    out = {}
    for bid, b in sorted(boards.items()):
        e = json.loads(json.dumps(b.data))
        e["platform"] = b.platform.id
        e["derived"] = odrflash_view({bid: b})["boards"][bid]
        e["derived"]["cpu"] = b.cpu
        out[bid] = e
    return {"boards": out, "platforms": {p: pl.data for p, pl in sorted(load_platforms(root).items())}}


def select(boards, platforms, platform=None, release=None, toolchain=None):
    """The ids `list` prints: the boards on any of the comma separated platforms, built with any of the comma
    separated toolchains ([toolchain] id), with that release. platforms is {id: Platform}. An unknown platform or
    toolchain is an error, so a misspelled one in CI never selects no board and passes."""
    def wanted(arg, known, what):
        want = [p for p in (arg or "").split(",") if p]
        unknown = [p for p in want if p not in known]
        if unknown:
            raise RegistryError("unknown %s %s (known: %s)" % (what, ", ".join(map(repr, unknown)), ", ".join(known)))
        return want
    plats = wanted(platform, list(platforms), "platform")
    tcs = wanted(toolchain, sorted({x.data["toolchain"]["id"] for x in platforms.values()}), "toolchain")
    return [b for b, x in boards.items() if (not plats or x.platform.id in plats)
            and (not tcs or x.platform.data["toolchain"]["id"] in tcs)
            and (not release or x.data["status"]["release"] == release)]


# Shell, Dockerfile and workflow variables that would hold a copy of a toolchain's pin: <ID>_GCC_VERSION and
# <ID>_GCC_SHA256 for the toolchain id. Set to a literal, the copy must equal platform.toml's
PIN_VARS = {"VERSION": "version", "SHA256": "sha256"}
PIN_DIRS = ("tools", ".github")


def pin_problems(root, platforms):
    """Every literal copy of a toolchain pin under tools/ or .github/ that differs from platform.toml: a Dockerfile's
    ARG ARM_GCC_VERSION=..., a script's or a workflow's ARM_GCC_SHA256: ... A value taken from a variable or a
    command ($...) is not a copy. Also every pair of platforms with the same toolchain id whose [toolchain] differ."""
    problems, pins = [], {}
    for pid, pl in sorted(platforms.items()):
        tc = pl.data["toolchain"]
        first = pins.setdefault(tc["id"], (pid, tc))
        if first[1] != tc:
            problems.append("platform/%s/platform.toml: [toolchain] differs from platform/%s/platform.toml's, which "
                            "has the same id %r (one toolchain, one pin)" % (pid, first[0], tc["id"]))
    want = {}
    for tid, (pid, tc) in pins.items():
        for suffix, key in PIN_VARS.items():
            if key in tc:
                want["%s_GCC_%s" % (tid.upper(), suffix)] = (tc[key], pid, key)
    if not want:
        return problems
    pat = re.compile(r"\b(%s)\b\s*[:=]\s*[\"']?([A-Za-z0-9][A-Za-z0-9._-]*)" % "|".join(map(re.escape, want)))
    for top in PIN_DIRS:
        for dirpath, dirs, files in os.walk(os.path.join(root, top)):
            dirs[:] = [d for d in dirs if d not in ("__pycache__", "node_modules")]
            for fn in files:
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, encoding="utf-8") as f:
                        text = f.read()
                except (OSError, UnicodeDecodeError):
                    continue
                for m in pat.finditer(text):
                    value, pid, key = want[m.group(1)]
                    if m.group(2) != value:
                        line = text.count("\n", 0, m.start()) + 1
                        problems.append("%s:%d: %s=%s, but platform/%s/platform.toml [toolchain] %s is %s: take it "
                                        "from there" % (os.path.relpath(path, root), line, m.group(1), m.group(2),
                                                        pid, key, value))
    return problems


def dumps(view):
    return json.dumps(view, indent=1, sort_keys=False, ensure_ascii=False) + "\n"


def lookup(value, path):
    """value at a dotted path; a list on the way maps the rest of the path over its items."""
    if not path:
        return value
    head, _, rest = path.partition(".")
    if isinstance(value, list):
        return [lookup(v, path) for v in value]
    if not isinstance(value, dict) or head not in value:
        raise KeyError(path)
    return lookup(value[head], rest)


def _flat(v):
    if isinstance(v, list):
        for x in v:
            yield from _flat(x)
    else:
        yield v


# ---------------------------------------------------------------------------------------------------------------
# self-test: the parsers and the checks on a fixture tree

def self_test():
    import shutil
    import tempfile
    fails = []

    def ok(cond, what):
        if not cond:
            fails.append(what)
        print("%s %s" % ("ok  " if cond else "FAIL", what))

    # list's selection (ci.sh --platform, the workflows' per-platform jobs)
    from types import SimpleNamespace as NS
    fp = {p: NS(id=p, data={"toolchain": {"id": t}}) for p, t in (("p1", "t1"), ("p2", "t2"), ("p3", "t1"))}
    fake = {"a": NS(platform=fp["p1"], data={"status": {"release": "stable"}}),
            "b": NS(platform=fp["p2"], data={"status": {"release": "testing"}}),
            "c": NS(platform=fp["p1"], data={"status": {"release": "testing"}}),
            "d": NS(platform=fp["p3"], data={"status": {"release": "stable"}})}
    ok(select(fake, fp) == ["a", "b", "c", "d"] and select(fake, fp, "p1") == ["a", "c"] and
       select(fake, fp, "p2,p1", "testing") == ["b", "c"] and select(fake, {**fp, "p4": fp["p1"]}, "p4") == [],
       "select: by platform, a comma separated list, by release; a platform without boards selects none")
    ok(select(fake, fp, toolchain="t1") == ["a", "c", "d"] and select(fake, fp, toolchain="t2,t1", release="stable")
       == ["a", "d"] and select(fake, fp, "p3", toolchain="t1") == ["d"],
       "select: by toolchain, every platform that has it; with a platform and a release")
    for args, what in ((("p1,px",), "'px'"), ((None, None, "tx"), "'tx'")):
        try:
            select(fake, fp, *args)
            ok(False, "select refuses an unknown platform or toolchain %s" % what)
        except RegistryError as e:
            ok(what in str(e), "select refuses an unknown platform or toolchain (%s)" % e)
    # one pin per toolchain: a second platform with the same id and another version, a copy that differs
    tmpp = tempfile.mkdtemp(prefix="odr-boards-pin-")
    try:
        tc = {"id": "arm", "name": "Arm", "cc": "gcc", "build": "x.sh", "version": "13.3.rel1", "sha256": "ab12"}
        pl = {"p1": NS(data={"toolchain": tc}), "p2": NS(data={"toolchain": dict(tc)})}
        os.makedirs(os.path.join(tmpp, "tools", "img"))
        v = "ARM_GCC_" + "VERSION"	# split, so this file holds no copy pin_problems would find in the tree
        good = ("ARG V\nRUN V=${V} VER=${V:-$(x)} sh x\n# V=... tools/install-arm-toolchain.sh\nARM_GCC_" +
                "SHA256: \"ab12\"\n").replace("V", v)
        with open(os.path.join(tmpp, "tools", "img", "Dockerfile"), "w", encoding="utf-8") as f:
            f.write(good)
        ok(pin_problems(tmpp, pl) == [], "pin_problems: variables, commands and an equal literal pass %r"
           % pin_problems(tmpp, pl))
        with open(os.path.join(tmpp, "tools", "img", "Dockerfile"), "a", encoding="utf-8") as f:
            f.write("ARG %s=13.2.rel1\n" % v)
        got = pin_problems(tmpp, pl)
        ok(len(got) == 1 and "Dockerfile:5: %s=13.2.rel1" % v in got[0], "pin_problems: a differing copy %r"
           % got)
        pl["p2"].data["toolchain"]["version"] = "14.2.rel1"
        ok(any("same id 'arm'" in q for q in pin_problems(tmpp, pl)), "pin_problems: two pins for one toolchain id")
    finally:
        shutil.rmtree(tmpp)

    tmpc = tempfile.mkdtemp(prefix="odr-boards-c-")
    with open(os.path.join(tmpc, "board.c"), "w", encoding="utf-8") as f:
        f.write('static const x_t other = { .name = "no" };\n// board_t b = { .name = "commented" };\n'
                'const board_t board = {\n  .caps = 1,\n    .name   =  "Radio X", /* { */ .y = { 1 },\n};\n'
                'const board_t b2 = { .name = BOARD_NAME };\n')
    with open(os.path.join(tmpc, "pins.h"), "w", encoding="utf-8") as f:
        f.write("#define PIN_A\t\t0x2D\t// note\n#define PIN_B 12u\n#define NOT_A_PIN (X + 1)\n")
    ok(board_t_names(tmpc) == [("Radio X", None), (None, "BOARD_NAME")],
       "board_t_names: only board_t, any whitespace, comments skipped, a macro %r" % board_t_names(tmpc))
    ok(pin_defines(os.path.join(tmpc, "pins.h")) == {"PIN_A": 0x2D, "PIN_B": 12}, "pin_defines: integer defines only")
    with open(os.path.join(tmpc, "flash.c"), "w", encoding="utf-8") as f:
        f.write("// hal_flash_map_t old = { .openfw_eeprom = 1 };\n"
                "static const hal_flash_map_t fmap = {\n\t.openfw_eeprom = 0x00700000u,\t/* { */\n"
                "\t.openfw_end = 0x00800000UL, .other = 5, .regions = { 1, 2 },\n\t.staging_addr = 0,\n};\n")
    ok(flash_map(tmpc) == {"openfw_eeprom": 0x700000, "openfw_end": 0x800000, "staging_addr": 0},
       "flash_map: the fields the site reads, suffixes and comments skipped %r" % flash_map(tmpc))
    with open(os.path.join(tmpc, "flash2.c"), "w", encoding="utf-8") as f:
        f.write("const hal_flash_map_t m2 = { .openfw_eeprom = 0x600000 };\n")
    try:
        flash_map(tmpc)
        ok(False, "flash_map refuses two maps that differ")
    except RegistryError:
        ok(True, "flash_map refuses two maps that differ")
    shutil.rmtree(tmpc)
    ok(ld_expr("0x030E8000 - 0x0300C000") == 0xDC000 and ld_expr("128K - 4") == 131068 and ld_expr("1M") == 1 << 20,
       "ld_expr: hex, K and M")
    try:
        ld_expr("__import__('os')")
        ok(False, "ld_expr refuses names")
    except RegistryError:
        ok(True, "ld_expr refuses names")
    lay = parse_ld("/* FLASH (rx) : ORIGIN = 0, LENGTH = 0 */\n__codec_slot = 0x030E8000;\n__stack_top = 1;\n"
                   "MEMORY\n{\n\tFLASH (rx) : ORIGIN = 0x0300C000, LENGTH = 0x030E8000 - 0x0300C000\t/* app */\n"
                   "\tRAM (rwx) : ORIGIN = 0x00010000, LENGTH = 128K\n}\n")
    ok(lay["symbols"] == {"__codec_slot": 0x030E8000} and lay["memory"]["FLASH"] == (0x0300C000, 0xDC000)
       and lay["memory"]["RAM"] == (0x10000, 0x20000), "parse_ld: codec symbols and MEMORY, comments ignored")
    for bad, what in (({"a": 1, "b": "x"}, "unknown key"), ({}, "missing key"), ({"a": "1"}, "wrong type"),
                      ({"a": True}, "bool is not an integer")):
        try:
            validate(bad, {"a": _req(int)}, "t")
            ok(False, "validate refuses: " + what)
        except RegistryError:
            ok(True, "validate refuses: " + what)
    ok(lookup({"v": [{"id": "a"}, {"id": "b"}]}, "v.id") == ["a", "b"], "lookup maps over lists")
    tmpm = tempfile.mkdtemp(prefix="odr-boards-mk-")
    with open(os.path.join(tmpm, "config.mk"), "w", encoding="utf-8") as f:
        f.write("# BOARD_UNSUPPORTED += NO\nBOARD_UNSUPPORTED += USB\t# no USB port\nBOARD_UNSUPPORTED += AM_RX\n"
                "BOARD_UNSUPPORTED += X Y\t# two on a line\n")
    ok(unsupported_reasons(os.path.join(tmpm, "config.mk")) == {"USB": "no USB port", "AM_RX": "", "X": "", "Y": ""},
       "unsupported_reasons: one switch a line with its comment, none for a bare or shared line")
    with open(os.path.join(tmpm, "config.mk"), "w", encoding="utf-8") as f:
        f.write("BOARD_UNSUPPORTED += USB\t# no USB port\nBOARD_NOT_WRITTEN += DTMF\t# no tone path yet\n")
    ok(not_written_reasons(os.path.join(tmpm, "config.mk")) == {"DTMF": "no tone path yet"}
       and unsupported_reasons(os.path.join(tmpm, "config.mk")) == {"USB": "no USB port"},
       "not_written_reasons: BOARD_NOT_WRITTEN lines only, and BOARD_UNSUPPORTED's reader skips them")
    shutil.rmtree(tmpm)
    for bad, what in (({"hil": {"battery_mv": "5V"}}, "a [hil] value of the wrong type"),
                      ({"hil": {"no_such": 1}}, "an unknown [hil] key"),
                      ({"features": {"usb": {"state": "maybe", "reason": "x"}}}, "a [features] state"),
                      ({"features": {"usb": {"state": "unsupported"}}}, "a [features] entry without a reason"),
                      ({"cps": {"radio_types": ["8"]}}, "a [cps] radio type that isn't a number")):
        try:
            validate(bad, {k: BOARD_SCHEMA[k] for k in ("hil", "features", "cps")}, "t")
            ok(False, "validate refuses " + what)
        except RegistryError:
            ok(True, "validate refuses " + what)

    # the voltage a board measures, from its sources
    tmpv = tempfile.mkdtemp(prefix="odr-boards-volts-")
    try:
        def volts_of(src):
            with open(os.path.join(tmpv, "board.c"), "w", encoding="utf-8") as f:
                f.write(src)
            return source_volts(tmpv)
        ok(volts_of("const board_t board = { .caps = CAP_KEYPAD | CAP_DMR };") == "battery", "volts: a battery")
        ok(volts_of("const board_t board = { .caps = CAP_ENCODER | CAP_NO_BATTERY, };") == "none",
           "volts: no battery, no supply reading")
        ok(volts_of("const board_t board = { .caps = CAP_NO_BATTERY };\n"
                    "static const hal_power_t p = { .supply_mv = rd };") == "supply", "volts: the supply")
        ok(volts_of("// .caps = CAP_NO_BATTERY\nconst board_t board = { .caps = 0 };") == "battery",
           "volts: a comment doesn't count")
    finally:
        shutil.rmtree(tmpv)

    # [hardware] clock: an rtc outside CLOCKS fails the schema
    clock = BOARD_SCHEMA["hardware"][0]["clock"][0]
    try:
        validate({"rtc": "maybe", "note": "x"}, clock, "hardware.clock")
        ok(False, "clock: an rtc outside CLOCKS fails")
    except RegistryError as e:
        ok("rtc" in str(e), "clock: an rtc outside CLOCKS fails")
    validate({"rtc": "backed", "note": "x"}, clock, "hardware.clock")

    # hardware switches: built, unsupported or unconfirmed, never off by default
    hw = {"VIBRATE": "a vibration motor", "NAND_STORAGE": "SPI NAND data flash"}
    hp = hardware_problems
    ok(hp("x", {"NAND_STORAGE": "NOR"}, {}, {"VIBRATE": "y", "NAND_STORAGE": "n"}, hw) == [],
       "hardware switches: one built, one in BOARD_UNSUPPORTED")
    ok(len(hp("x", {}, {}, {"VIBRATE": "y", "NAND_STORAGE": "n"}, hw)) == 1,
       "hardware switches: off by default and not in BOARD_UNSUPPORTED fails")
    ok(hp("x", {}, {"NAND_STORAGE": "unknown"}, {"VIBRATE": "y", "NAND_STORAGE": "n"}, hw) == [],
       "hardware switches: an unconfirmed fact passes")
    ok(len(hp("x", {}, {"VIBRATE": "unknown"}, {"VIBRATE": "y", "NAND_STORAGE": "y"}, hw)) == 1,
       "hardware switches: unconfirmed on a switch the board builds fails")
    ok(len(hp("x", {}, {"FONT_BIG": "?"}, {"VIBRATE": "y", "NAND_STORAGE": "y"}, hw)) == 1,
       "hardware switches: unconfirmed on a switch that isn't a hardware switch fails")
    ok(len(hp("x", {"NAND_STORAGE": "NOR"}, {}, {"VIBRATE": "y", "NAND_STORAGE": "y"}, hw)) == 1,
       "hardware switches: in BOARD_UNSUPPORTED but built by default fails")
    ok(hp("x", {"NAND_STORAGE": "the driver isn't written"}, {}, {"VIBRATE": "y", "NAND_STORAGE": "n"}, hw) == [],
       "hardware switches: one in BOARD_NOT_WRITTEN (check() passes both lists) passes off by default")

    # the survey's Port cells (check 5) and the floor, on a fixture survey
    class Stub:
        def __init__(self, bid, model, level="supported"):
            self.id, self.data = bid, {"model": model, "status": {"level": level},
                                       "docs": {"hardware": "docs/spec/hardware-%s.md" % bid}}
    tmps = tempfile.mkdtemp(prefix="odr-boards-survey-")
    try:
        os.makedirs(os.path.join(tmps, "docs", "spec"))
        def survey_of(rows):
            with open(os.path.join(tmps, registry_docs.SURVEY), "w", encoding="utf-8") as f:
                f.write("## Summary table\n\n| Radio | MCU | Port |\n|---|---|---|\n" + "".join(
                    "| %s | x | %s |\n" % r for r in rows))
            return registry_docs.survey(tmps, {"a": Stub("a", "AB-100")})
        rows, probs = survey_of([("Maker AB-100, Other", "Supported"), ("Maker CD-9", "In progress, #12. Its note"),
                                 ("Maker EF-1", "Planned, hardware needed"), ("Maker GH-2", "Not planned. No")])
        ok(probs == [] and [(r["board"], r["status"], r["pr"], r["hardware_needed"], r["note"]) for r in rows] ==
           [("a", "board", None, False, ""), (None, "in_progress", 12, False, "Its note"),
            (None, "planned", None, True, ""), (None, None, None, False, "No")],
           "survey: a board's row, in progress with its PR, planned, not planned %r" % (probs or rows))
        for cells, what in ((("Maker AB-100", "Planned"),), "a planned row for an existing board"), \
                           ((("Maker CD-9", "In progress"),), "in progress without a PR number"), \
                           ((("Maker CD-9", "Soon"),), "a Port cell outside the set"), \
                           ((("Maker AB-100", "Supported"), ("Maker AB-100 GPS", "Supported")), "two rows for a board"):
            ok(len(survey_of(cells)[1]) == 1, "survey check 5 finds " + what)
        ok(registry_docs.rebase_links("[x](hardware-a.md#k) [y](https://e/z)", "docs/spec", "") ==
           "[x](docs/spec/hardware-a.md#k) [y](https://e/z)", "rebase_links: relative links only")
        fill = registry_docs.fill
        yml = "o:\n  # generated: tools/boards.py docs r; edit old\n  - stale\n  # end generated\n  - Other\n"
        ok(fill(yml, "r", "src", ["- A", "- B"], "t", "yaml") ==
           "o:\n  # generated: tools/boards.py docs r; edit src\n  - A\n  - B\n  # end generated\n  - Other\n",
           "fill: a YAML block keeps its indent")
        bare = "<!-- x:\ngenerated: tools/boards.py docs r; edit s\nold\nend generated\n-->\n"
        ok(fill(bare, "r", "s", ["A, B"], "t", "bare") == bare.replace("old", "A, B"),
           "fill: a bare block inside a Markdown comment")
        try:
            fill(yml, "r", "s", [], "t")
            ok(False, "fill: the wrong marker style finds no block")
        except registry_docs.DocsError:
            pass
        ys = registry_docs.yaml_scalar
        ok(ys("Acme XR-100 / Other ZT-7") == "Acme XR-100 / Other ZT-7" and ys("A: B") == '"A: B"'
           and ys("No") == '"No"', "yaml_scalar: quoted only where a plain scalar would misread")
        cc = registry_docs.checked_cell	# the "Checked on each radio" cells, from tools/bringup.py's statuses
        ok(cc("built, not yet recorded", None, "") == "not recorded" and cc("pass", "run", "") == "pass"
           and cc("pass", "seed", "") == "pass (seed)" and cc("fail", "run", "") == "fail"
           and cc("needs re-run", "run", "fail at x; changed since: y") == "fail, needs re-run"
           and cc("needs re-run", "seed", "seed: z; changed since: y") == "pass (seed), needs re-run",
           "checked_cell: no record is never a pass; a stale check keeps its old result")
        old = ("## Radios and status\n\n| Radio | Builds | Host tests | On the radio | Support |\n"
               "|---|---|---|---|---|\n"
               "| [Maker AB-100](x) | ● | ● | %s | ok |\n| Maker CD-9 | ○ | ○ | ○ | %s |\n")
        claims = registry_docs.readme_claims
        ok(claims(old % ("●", "In progress (#1)")) == {"AB-100": (3, [2, 2, 2]), "CD-9": (2, [0, 0, 0])},
           "readme_claims: marks and statuses by model code %r" % claims(old % ("●", "In progress (#1)")))
        top = claims(old % ("●", "In progress (#1)"))
        ok(claims(old % ("◐", "In progress (#1)"))["AB-100"][1] < top["AB-100"][1]
           and claims(old % ("●", "Planned"))["CD-9"][0] < top["CD-9"][0],
           "readme_claims: a lower mark and a lower status rank lower")
    finally:
        shutil.rmtree(tmps)

    # a fixture tree: the real registry copied, then broken one way at a time
    tmp = tempfile.mkdtemp(prefix="odr-boards-")
    try:
        # boards and platforms with what make print-vars reads (the Makefile, config/, boot/boot.mk) and pins.h
        for top in ("boards", "platform", "config"):
            shutil.copytree(os.path.join(ROOT, top), os.path.join(tmp, top),
                            ignore=lambda d, names: [n for n in names if not (n.endswith((".toml", ".mk", ".ld", ".c"))
                                                                              or n == "pins.h"
                                                                              or os.path.isdir(os.path.join(d, n)))])
        shutil.copy(os.path.join(ROOT, "Makefile"), tmp)
        os.makedirs(os.path.join(tmp, "boot"))
        shutil.copy(os.path.join(ROOT, "boot", "boot.mk"), os.path.join(tmp, "boot"))
        shutil.copytree(os.path.join(ROOT, "tools", "odrflash", "odrflash"), os.path.join(tmp, "tools", "odrflash", "odrflash"))
        for t in CODEC_TOOLS:
            open(os.path.join(tmp, "tools", t + ".py"), "w").close()
        os.makedirs(os.path.dirname(os.path.join(tmp, UI_SIM)))
        shutil.copy(os.path.join(ROOT, UI_SIM), os.path.join(tmp, UI_SIM))
        for b in board_dirs(ROOT):
            for p in _tomllib().load(open(os.path.join(ROOT, "boards", b, "board.toml"), "rb"))["docs"].values():
                os.makedirs(os.path.dirname(os.path.join(tmp, p)), exist_ok=True)
                open(os.path.join(tmp, p), "w").close()
        ok(check(tmp) == [], "the copied registry is consistent: %s" % check(tmp)[:3])
        one = board_dirs(tmp)[0]
        path = os.path.join(tmp, "boards", one, "board.toml")
        good = open(path, encoding="utf-8").read()

        def broken(text, what, want):
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            got = check(tmp)
            ok(any(want in p for p in got), "check finds %s (%s)" % (what, got[:2]))
            with open(path, "w", encoding="utf-8") as f:
                f.write(good)
        broken("new_key = 1\n" + good, "an unknown key", "unknown key")	# first: the file may end in a table
        broken(good.replace('id = "%s"' % one, 'id = "other"', 1), "an id that isn't the directory", "directory name")
        broken(re.sub(r'(?m)^family = ".*"$', 'family = "usb-dfu"', good), "a family's missing table", "needs flash.dfu")
        broken(good.replace('hardware = "', 'hardware = "missing/', 1), "a missing docs page", "doesn't exist")
        broken(re.sub(r'(?m)^gallery = ".*"$', 'gallery = "200x100"', good), "a display class the gallery lacks",
               "sim_geoms")
        broken(re.sub(r"(?m)^hold_power = .*$", "", good), "a missing stub key", "needs hold_power")
        broken(re.sub(r"(?m)^(\[flash\.stub\]\n)id = .*$", r"\1id = %d" % STUB_TEST_ID, good),
               "the test profiles' stub id", "reserved for the stub tools' test profiles")
        broken(re.sub(r"(?m)^(hold_power = .*)$", r"\1\nspi = { bus = 1, mode = 0, hz = 1000000 }", good),
               "a key of another platform's stub", "not a key of the stubtool stub")
        two = board_dirs(tmp)[1]
        other = _tomllib().load(open(os.path.join(tmp, "boards", two, "board.toml"), "rb"))["order"]
        broken(re.sub(r"(?m)^order = .*$", "order = %d" % other, good), "an order another board has", "also the")
        fm_c = [os.path.join(tmp, "boards", one, f) for f, text in _c_sources(os.path.join(tmp, "boards", one))
                if "hal_flash_map_t" in text]
        fm_good = open(fm_c[0], encoding="utf-8").read()
        with open(fm_c[0], "w", encoding="utf-8") as f:
            f.write(fm_good.replace("hal_flash_map_t", "hal_other_t"))
        ok(any("no hal_flash_map_t" in p for p in check(tmp)), "check finds a board without a flash map")
        with open(fm_c[0], "w", encoding="utf-8") as f:
            f.write(fm_good)
        mkp = os.path.join(tmp, "boards", one, "config.mk")
        mk_good = open(mkp, encoding="utf-8").read()
        with open(mkp, "w", encoding="utf-8") as f:
            f.write(mk_good + "BOARD_UNSUPPORTED += SCAN\n")
        ok(any("SCAN has no reason" in p for p in check(tmp)), "check finds a BOARD_UNSUPPORTED switch without a reason")
        with open(mkp, "w", encoding="utf-8") as f:
            f.write(mk_good + "CONFIG_SCAN ?= n\nBOARD_NOT_WRITTEN += SCAN\n")
        ok(any("BOARD_NOT_WRITTEN SCAN has no reason" in p for p in check(tmp)),
           "check finds a BOARD_NOT_WRITTEN switch without a reason")
        with open(mkp, "w", encoding="utf-8") as f:
            f.write(mk_good + "CONFIG_SCAN ?= n\nBOARD_UNSUPPORTED += SCAN\t# a\nBOARD_NOT_WRITTEN += SCAN\t# b\n")
        ok(any("in both BOARD_UNSUPPORTED and BOARD_NOT_WRITTEN" in p for p in check(tmp)),
           "check finds a switch in both lists")
        with open(mkp, "w", encoding="utf-8") as f:
            f.write(mk_good)
        pins = os.path.join(tmp, "boards", one, "pins.h")
        if os.path.isfile(pins):
            os.remove(pins)
            ok(any("hold_power needs" in p for p in check(tmp)) or "hold_power = true" not in good,
               "check finds a hold_power stub without the pins in pins.h")
            shutil.copy(os.path.join(ROOT, "boards", one, "pins.h"), pins)
        # a build option whose board.toml values differ from board.mk's
        opt = next((b for b in board_dirs(tmp) if "[[build_options]]" in open(os.path.join(tmp, "boards", b, "board.toml"),
                                                                             encoding="utf-8").read()), None)
        if opt:
            mk = os.path.join(tmp, "boards", opt, "board.mk")
            mk_good = open(mk, encoding="utf-8").read()
            with open(mk, "w", encoding="utf-8") as f:
                f.write(re.sub(r"(?m)^(\w+_VALUES\s*:=\s*)(\S+)\s+(\S+)", r"\1\3 \2", mk_good))
            ok(any("_VALUES" in p for p in check(tmp)), "check finds build option values that differ from board.mk")
            with open(mk, "w", encoding="utf-8") as f:
                f.write(mk_good)
        # a stock-file codec: a list of SHA-256 digests, its profile and sources from them
        st = next((b for b in board_dirs(tmp) if re.search(r"(?m)^stock = \[", open(os.path.join(tmp, "boards", b,
                                                                                           "board.toml")).read())), None)
        if st:
            sp = os.path.join(tmp, "boards", st, "board.toml")
            st_good = open(sp, encoding="utf-8").read()
            for text, what, want in ((re.sub(r'(?m)^stock = \["(\w+)"\]', r'stock = "\1"', st_good),
                                      "a codec.stock that isn't a list", "a list expected"),
                                     (re.sub(r'(?m)^stock = \["(\w+)"\]', r'stock = ["\1", "\1"]', st_good),
                                      "a stock file listed twice", "once"),
                                     (re.sub(r'(?m)^stock = \[', 'stock = ["abc", ', st_good),
                                      "a codec.stock entry that isn't a SHA-256", "SHA-256"),
                                     (re.sub(r"(?m)^(stock = .*)$", r'\1\nsources = ["x"]', st_good),
                                      "codec.sources beside codec.stock", "comes from the stock files")):
                with open(sp, "w", encoding="utf-8") as f:
                    f.write(text)
                got = check(tmp)
                ok(any(want in q for q in got), "check finds %s (%s)" % (what, got[:2]))
            with open(sp, "w", encoding="utf-8") as f:
                f.write(st_good)
        # the build view's file names against the Makefile's
        mkf = os.path.join(tmp, "Makefile")
        mkf_good = open(mkf, encoding="utf-8").read()
        with open(mkf, "w", encoding="utf-8") as f:
            f.write(mkf_good.replace("TARGET := $(BUILD)/odr_", "TARGET := $(BUILD)/odr-", 1))
        ok(any("the build view says" in p for p in check(tmp)), "check finds a Makefile image name the build view lacks")
        with open(mkf, "w", encoding="utf-8") as f:
            f.write(mkf_good)
        with open(path, "w", encoding="utf-8") as f:
            f.write(good.replace('name = "', 'name = "Renamed ', 1))
        ok(any("out of date" in p for p in check(tmp)), "check finds a stale %s" % ODRFLASH_JSON)
        os.remove(path)
        ok(any("no board.toml" in p for p in check(tmp)), "check finds a board directory without board.toml")
    finally:
        shutil.rmtree(tmp)
    print("boards self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                 "" if len(fails) == 1 else "s"))
    return 1 if fails else 0


# ---------------------------------------------------------------------------------------------------------------

def generic_problems(root=ROOT):
    """AGENTS.md rule 4 over the tree: a radio's id or name outside its own places (tools/check_generic.py). Apart
    from check(), which the self-test runs on copies without git history."""
    import check_generic
    radios, docs = check_generic.registry_radios(root)
    return ["generic: %s" % p for p in check_generic.check(root, radios, docs)]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="board ids, one per line")
    p.add_argument("--platform", help="only the boards on this platform (a comma separated list: on any of them)")
    p.add_argument("--toolchain", help="only the boards whose platform builds with this toolchain ([toolchain] id; a "
                                       "comma separated list: any of them)")
    p.add_argument("--release", choices=RELEASES)
    p.add_argument("--comma", action="store_true", help="one line, comma separated")
    p = sub.add_parser("get", help="one value of one board (a list: one item per line)")
    p.add_argument("board")
    p.add_argument("path", nargs="?", default="")
    p = sub.add_parser("platform", help="one value of one platform's platform.toml (no id: the platform ids)")
    p.add_argument("id", nargs="?")
    p.add_argument("path", nargs="?", default="")
    p = sub.add_parser("builds", help="a board's images besides the default ('name make-args', one per line)")
    p.add_argument("board")
    p.add_argument("--extra", action="store_true", help="the [ci] extra configurations instead")
    p = sub.add_parser("json", help="the merged view")
    p.add_argument("--for", dest="view", choices=("all", "odrflash", "site"), default="all")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--out", metavar="FILE")
    g.add_argument("--check", metavar="FILE", help="fail if FILE differs from what would be written")
    sub.add_parser("check", help="registry consistency")
    p = sub.add_parser("docs", help="rewrite the docs blocks generated from the registry")
    p.add_argument("--check", action="store_true", help="fail if a generated block is out of date; write nothing")
    p.add_argument("--floor", metavar="REF", help="fail when the README lists a radio lower than at REF")
    sub.add_parser("self-test")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "self-test":
            return self_test()
        if a.cmd == "check":
            problems = check()
            problems += generic_problems()
            for p in problems:
                print("boards: %s" % p, file=sys.stderr)
            if not problems:
                print("boards: %d boards, %d platforms, consistent" % (len(load()), len(load_platforms())))
            return 1 if problems else 0
        if a.cmd == "docs":
            boards = load()
            problems = registry_docs.docs(ROOT, boards, check=a.check)
            if a.floor:
                new = registry_docs.generate(ROOT, boards)[0]["README.md"]
                problems += registry_docs.floor(ROOT, a.floor, new)
            for p in problems:
                print("boards: %s" % p, file=sys.stderr)
            if not problems:
                print("boards: docs %s%s" % ("current" if a.check else "written",
                                            ", nothing lower than %s" % a.floor if a.floor else ""))
            return 1 if problems else 0
        # the board's make variables cost a make run each: load only what the command reads
        boards = {} if a.cmd == "platform" else load(only=[a.board] if a.cmd in ("get", "builds") else None)
        if a.cmd == "list":
            ids = select(boards, load_platforms(), a.platform, a.release, a.toolchain)
            print(",".join(ids) if a.comma else "\n".join(ids))
        elif a.cmd in ("get", "platform"):
            if a.cmd == "platform":
                platforms = load_platforms()
                if a.id is None:
                    print("\n".join(platforms))
                    return 0
                if a.id not in platforms:
                    raise RegistryError("unknown platform %r (known: %s)" % (a.id, ", ".join(platforms)))
                what, v = a.id, platforms[a.id].data
            else:
                if a.board not in boards:
                    raise RegistryError("unknown board %r (known: %s)" % (a.board, ", ".join(boards)))
                what, v = a.board, full_view({a.board: boards[a.board]})["boards"][a.board]
            try:
                v = lookup(v, a.path)
            except KeyError:
                raise RegistryError("%s has no %s" % (what, a.path))
            if isinstance(v, dict):
                print(json.dumps(v, indent=1, ensure_ascii=False))
            else:
                for x in _flat(v if isinstance(v, list) else [v]):
                    print(json.dumps(x) if isinstance(x, (dict, bool)) or x is None else x)
        elif a.cmd == "builds":
            if a.board not in boards:
                raise RegistryError("unknown board %r (known: %s)" % (a.board, ", ".join(boards)))
            b = boards[a.board]
            for name, args in (b.ci_extra() if a.extra else b.builds()):
                print(" ".join([name] + args))
        elif a.cmd == "json":
            text = dumps({"odrflash": odrflash_view, "site": site_view}.get(a.view, full_view)(boards))
            if a.check:
                try:
                    with open(a.check, encoding="utf-8") as f:
                        same = f.read() == text
                except OSError:
                    same = False
                if not same:
                    print("boards: %s is out of date: run tools/boards.py json --for %s --out %s"
                          % (a.check, a.view, a.check), file=sys.stderr)
                    return 1
            elif a.out:
                with open(a.out, "w", encoding="utf-8") as f:
                    f.write(text)
            else:
                sys.stdout.write(text)
        return 0
    except (RegistryError, registry_docs.DocsError) as e:
        print("boards: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
