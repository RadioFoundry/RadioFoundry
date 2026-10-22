#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The board registry: one place for every fact about a radio that isn't the build itself (issue #106).

Sources, each fact in one of them:
  boards/<id>/board.toml        identity, status, build options, CI extras, flashing family, stub profile, codec route,
                                site data
  platform/<p>/platform.toml    toolchain, CPU, CI group, platform stubs, flash base and sector geometries, codec symbols
  boards/<id>/board.mk          PLATFORM, LDSCRIPT, BOOT_*, BOARD_DIRS, read through make (print-vars), so the
                                board's config.mk and its conditionals count; the build reads no TOML
  boards/<id>/*.ld              the application region (MEMORY FLASH) and the codec symbols (__codec_*)
  boards/<id>/*.c               the board_t .name values: the names the debug link reports
  boards/<id>/pins.h            the power latch and key pins the stub holds

Derived rather than typed: the flashing link from the family, the CPU from the platform, the protected range (flash
base to the application), the erasable sectors (the platform geometry from the application up), the stub's NAND flag
(the board's drivers) and its power pins (pins.h).

    tools/boards.py list [--platform P] [--release stable|testing] [--comma]   board ids
    tools/boards.py platform P [key.path]        one platform value
    tools/boards.py get dm32 [key.path]          one value (a list: one item per line), for shell scripts
    tools/boards.py json [--for odrflash|build] [--out FILE | --check FILE]   the merged, derived view; --for build
                                                  is the stable view for tools that build from a checkout
    tools/boards.py check                         registry consistency
    tools/boards.py self-test                     the loader's own tests, on fixtures

Needs Python 3.11 or later (tomllib). Scripts find one with tools/odr_python.sh. The tools that flash radios read the
generated view (tools/odrflash/odrflash/registry.json), so they run on any Python 3 and inside the odrflash executable.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LEVELS = ("bring-up", "testing", "supported")
RELEASES = ("stable", "testing")
KINDS = ("handheld", "mobile")
# The flashing routes, named by protocol (odrflash cli.py, site/static/js/flasher): the cable they need, and the
# board.toml keys a board on the route must have
FAMILIES = {
    "usb-dfu": {"link": "usb", "needs": ("flash.dfu",)},		# a USB DFU bootloader with a per-model XOR key
    "serial-loader": {"link": "serial-cable", "needs": ("flash.bootloader.id", "flash.bootloader.keys")},	# a serial
    # bootloader started by a key combination, speaking its own file format (tools/serial_loader.py)
    "serial-iap": {"link": "serial-cable", "needs": ()},	# a vendor IAP loader on the cable, through the RAM stub
}
CODEC_TOOLS = ("codec_extract", "hrc7000_codec")
ODRFLASH_JSON = "tools/odrflash/odrflash/registry.json"	# the flashing tools' view, generated and committed
APP_SLOT = "app+slot"		# [flash] sectors: one sector for the application, one for the codec slot (no geometry)
NAND_DRIVER = "drivers/flash/spi_nand"	# in BOARD_DIRS: the board's data memory is SPI NAND (the stub's nand flag)
NO_PIN = 255			# the universal stub's "no pin"
# board.mk values the registry reads, through make (conditionals and config.mk included)
MAKE_VARS = ("PLATFORM", "LDSCRIPT", "BOOT_STAGE", "BOOT_SIZE", "BOOT_SIGNATURE", "BOARD_DIRS", "BUILD_OPTIONS")


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
    "part_of": _opt(str),
    "debuglink_aliases": _opt(STR_LIST),
    "docs": _req({"hardware": _req(str), "codec": _opt(str), "install": _opt(str), "bringup": _opt(str)}),
    "hardware": _req({"cpu": _opt(str), "bands": _req(str), "power": _req(str), "display": _req(str),
                      "gallery": _req(str)}),
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
        "stub": _opt({"id": _req(int), "hold_power": _req(bool), "progmem": _req(int), "tested": _req(bool),
                      "stock_console": _opt(bool)}),
    }),
    "codec": _req({"tool": _req(CODEC_TOOLS), "stock": _opt(STR_LIST), "profile": _opt(str), "sources": _opt(STR_LIST),
                   "installed": _opt(STR_LIST)}),
    "frame": _req({"shape": _req(str), "body": _req(str), "accent": _req(str), "lens": _req(RANGE)}),
}

PLATFORM_SCHEMA = {
    "id": _req(str),
    "isa": _req(str),
    "cpu": _req(str),
    "toolchain": _req({"cc": _req(str), "build": _req(str), "version": _req(str), "sha256": _opt(str),
                       "image": _opt(str)}),
    "ci": _req({"group": _req(str), "container": _req(bool)}),
    "stubs": _req(STR_LIST),
    "board_stub": _req(bool),
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
    try:
        r = subprocess.run(["make", "-s", "--no-print-directory", "-C", root, "BOARD=" + board, *args,
                            "print-vars", "VARS=" + " ".join(names)], capture_output=True, text=True, timeout=60)
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


def ld_expr(text):
    """The value of a linker script constant expression: numbers (hex, decimal, K and M suffixes), + - * / ( )."""
    s = re.sub(r"\b(\d+)([KM])\b", lambda m: "(%s*%d)" % (m.group(1), 1024 if m.group(2) == "K" else 1048576), text)
    if not re.fullmatch(r"[0-9a-fA-FxX+\-*/() \t]+", s):
        raise RegistryError("can't evaluate linker expression %r" % text)
    return int(eval(s.replace("/", "//"), {"__builtins__": {}}, {}))


def parse_ld(text):
    """{"symbols": {__codec_*: value}, "memory": {region: (origin, length)}} of a linker script."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    syms = {m.group(1): ld_expr(m.group(2)) for m in re.finditer(r"(?m)^\s*(__codec_\w+)\s*=\s*([^;]+);", text)}
    mem = {}
    block = re.search(r"MEMORY\s*\{(.*?)\}", text, re.S)
    if block:
        for m in re.finditer(r"(?m)^\s*(\w+)\s*(?:\([^)]*\))?\s*:\s*ORIGIN\s*=\s*([^,]+),\s*LENGTH\s*=\s*(.+?)\s*$",
                             block.group(1)):
            mem[m.group(1)] = (ld_expr(m.group(2)), ld_expr(m.group(3)))
    return {"symbols": syms, "memory": mem}


def _strip_c_comments(text):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)


def board_t_names(board_dir):
    """What board_t .name is set to in a board's sources: [(literal or None, macro or None)], one per board_t
    definition. The one parser of it (bringup.py uses it too): any whitespace, comments ignored; a macro is returned by
    name (such a board names its values in [[build_options]] board_name)."""
    out = []
    for f in sorted(os.listdir(board_dir)):
        if not f.endswith(".c"):
            continue
        with open(os.path.join(board_dir, f), encoding="utf-8") as fh:
            text = _strip_c_comments(fh.read())
        for m in re.finditer(r"\bboard_t\s+\w+\s*=\s*\{", text):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            body = text[m.end():i]
            n = re.search(r'\.name\s*=\s*(?:"([^"]*)"|([A-Za-z_]\w*))', body)
            if n:
                out.append((n.group(1), n.group(2)))
    return out


def pin_defines(path):
    """{NAME: value} of the integer #defines in a pins.h."""
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as f:
        text = _strip_c_comments(f.read())
    return {m.group(1): int(m.group(2), 0)
            for m in re.finditer(r"(?m)^\s*#\s*define\s+(\w+)\s+\(?((?:0[xX][0-9a-fA-F]+|\d+))[uU]?\)?\s*$", text)}


# ---------------------------------------------------------------------------------------------------------------
# the loader

class Platform:
    def __init__(self, data):
        self.data = data
        self.id = data["id"]


class Board:
    """A board's TOML fields (data) and what is derived from its build files."""

    def __init__(self, data, platform, mk, layout, names, pins):
        self.data = data
        self.id = data["id"]
        self.platform = platform
        self.mk = mk				# the make variables of MAKE_VARS and the build options
        self.symbols = layout["symbols"]	# __codec_* of every linker script in the board directory
        self.app_region = layout["flash"]	# (start, end) of MEMORY FLASH in the build's LDSCRIPT
        self.source_names = names		# board_t .name: [(literal, macro)]
        self.pins = pins			# pins.h integer defines
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
        """The universal stub's profile with its pins: the power latch and key from pins.h when the stub must hold
        the radio on (a soft power key), else none."""
        s = self.data["flash"].get("stub")
        if s is None:
            return None
        hold = s["hold_power"]
        return {"id": s["id"], "latch": self.pins.get("PIN_POWER_LATCH", NO_PIN) if hold else NO_PIN,
                "key": self.pins.get("PIN_POWER_KEY", NO_PIN) if hold else NO_PIN, "nand": self.nand,
                "progmem": s["progmem"], "tested": s["tested"], "stock_console": s.get("stock_console", False)}

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
        raise RegistryError("boards/%s/board.mk: no LDSCRIPT" % bid)
    with open(os.path.join(root, ld), encoding="utf-8") as f:
        main = parse_ld(f.read())
    if "FLASH" not in main["memory"]:
        raise RegistryError("%s: no MEMORY FLASH region" % ld)
    org, length = main["memory"]["FLASH"]
    syms = {}
    for f in sorted(os.listdir(bdir)):
        if f.endswith(".ld"):
            with open(os.path.join(bdir, f), encoding="utf-8") as fh:
                for k, v in parse_ld(fh.read())["symbols"].items():
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
                         pin_defines(os.path.join(bdir, "pins.h")))
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


def check(root=ROOT):
    """Every problem found, as strings (empty: the registry is consistent)."""
    problems = []
    try:
        boards = load(root)
    except RegistryError as e:
        return [str(e)]
    stub_ids = {}
    classes = gallery_classes(root)
    for bid, b in boards.items():
        d = b.data
        where = "boards/%s/board.toml" % bid

        def bad(msg):
            problems.append("%s: %s" % (where, msg))
        for k, p in d["docs"].items():
            if not os.path.isfile(os.path.join(root, p)):
                bad("docs.%s %s doesn't exist" % (k, p))
        if d.get("part_of") is not None and d["part_of"] not in boards:
            bad("part_of %r is not a board" % d["part_of"])
        if not b.source_names:
            bad("no board_t definition with .name in boards/%s/*.c" % bid)
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
        if stub is not None:
            if not b.platform.data["board_stub"]:
                bad("[flash.stub] is a board profile of the universal stub: platform %s has none (board_stub)"
                    % b.platform.id)
            elif stub["id"] in stub_ids:
                bad("flash.stub id %d is also the %s's" % (stub["id"], stub_ids[stub["id"]]))
            else:
                stub_ids[stub["id"]] = bid
            if stub["hold_power"] and not {"PIN_POWER_LATCH", "PIN_POWER_KEY"} <= set(b.pins):
                bad("flash.stub hold_power needs PIN_POWER_LATCH and PIN_POWER_KEY in boards/%s/pins.h" % bid)
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
        plats[pid] = {"isa": pl.data["isa"], "cpu": pl.data["cpu"], "cc": tc["cc"], "version": tc["version"],
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
        broken(good + "\nnew_key = 1\n", "an unknown key", "unknown key")
        broken(good.replace('id = "%s"' % one, 'id = "other"', 1), "an id that isn't the directory", "directory name")
        broken(re.sub(r'(?m)^family = ".*"$', 'family = "usb-dfu"', good), "a family's missing table", "needs flash.dfu")
        broken(good.replace('hardware = "', 'hardware = "missing/', 1), "a missing docs page", "doesn't exist")
        broken(re.sub(r'(?m)^gallery = ".*"$', 'gallery = "200x100"', good), "a display class the gallery lacks",
               "sim_geoms")
        broken(re.sub(r"(?m)^hold_power = .*$", "", good), "a missing stub key", "missing key")
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

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="board ids, one per line")
    p.add_argument("--platform")
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
    p.add_argument("--for", dest="view", choices=("all", "odrflash", "build"), default="all",
                   help="all: every field; odrflash: the flashing tools' registry.json; build: the build view")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--out", metavar="FILE")
    g.add_argument("--check", metavar="FILE", help="fail if FILE differs from what would be written")
    sub.add_parser("check", help="registry consistency")
    sub.add_parser("self-test")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "self-test":
            return self_test()
        if a.cmd == "check":
            problems = check()
            for p in problems:
                print("boards: %s" % p, file=sys.stderr)
            if not problems:
                print("boards: %d boards, %d platforms, consistent" % (len(load()), len(load_platforms())))
            return 1 if problems else 0
        # the board's make variables cost a make run each: load only what the command reads
        boards = {} if a.cmd == "platform" else load(only=[a.board] if a.cmd in ("get", "builds") else None)
        if a.cmd == "list":
            ids = [b for b, x in boards.items() if (not a.platform or x.platform.id == a.platform)
                   and (not a.release or x.data["status"]["release"] == a.release)]
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
            text = dumps({"odrflash": odrflash_view, "build": build_view, "all": full_view}[a.view](boards))
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
    except RegistryError as e:
        print("boards: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
