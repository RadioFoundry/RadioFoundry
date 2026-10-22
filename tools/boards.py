#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The board registry: one place for every fact about a radio that isn't the build itself (issue #106).

Sources, each fact in one of them:
  boards/<id>/board.toml        identity, status, build options, CI extras, flashing, stub profile, codec route, site data
  platform/<p>/platform.toml    toolchain, CI group, platform stubs, flash sector geometries
  boards/<id>/board.mk          PLATFORM, LDSCRIPT, BOOT_* (read here, unchanged; the build reads no TOML)
  boards/<id>/*.ld              the application region (MEMORY FLASH) and the codec symbols (__codec_*)
  boards/<id>/*.c               board_t .name, checked against debuglink_names

    tools/boards.py list [--platform P] [--release stable|testing] [--comma]   board ids
    tools/boards.py get dm32 [key.path]          one value (a list: one item per line), for shell scripts
    tools/boards.py json [--for odrflash] [--out FILE | --check FILE]   the merged, derived view
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
# odrflash's flashing routes (tools/odrflash/odrflash/cli.py) and the web flasher's (site/static/js/flasher)
FAMILIES = ("anyroad-dfu", "bfuv32-serial", "hrc7000-iap")
BACKENDS = ("anyroad-dfu", "c7000-stub", "none")
CODEC_TOOLS = ("codec_extract", "hrc7000_codec")
ODRFLASH_JSON = "tools/odrflash/odrflash/registry.json"	# the flashing tools' view, generated and committed
APP_SLOT = "app+slot"		# [flash] sectors: one sector for the application, one for the codec slot (no geometry)


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
RANGES = ("list", RANGE)

BOARD_SCHEMA = {
    "id": _req(str),
    "name": _req(str),
    "maker": _req(str),
    "model": _req(str),
    "also": _req(STR_LIST),
    "kind": _req(KINDS),
    "debuglink_names": _req(STR_LIST),
    "docs": _req({"hardware": _req(str), "codec": _opt(str), "install": _opt(str), "bringup": _opt(str)}),
    "hardware": _req({"cpu": _req(str), "bands": _req(str), "power": _req(str), "display": _req(str),
                      "gallery": _req(str)}),
    "status": _req({"level": _req(LEVELS), "summary": _req(str), "release": _req(RELEASES)}),
    "build_options": _opt(("list", {"name": _req(str), "title": _req(str), "help": _req(str),
                                    "values": _req(("list", {"id": _req(str), "title": _req(str), "text": _req(str),
                                                             "board_name": _req(str)}))})),
    "ci": _opt({"extra": _req(("list", {"name": _req(str), "make": _req(STR_LIST)}))}),
    "flash": _req({
        "backend": _req(BACKENDS),
        "family": _req(FAMILIES),
        "link": _req(("usb", "serial-cable")),
        "keep_codec": _req(bool),
        "sectors": _req(str),
        "protected": _req(RANGES),
        "stock_download": _opt(str),
        "stock_versions": _req(STR_LIST),
        "backup": _req({"stock": _req(("none", "full")), "odr": _req(("none", "full")), "spi_size": _req(int)}),
        "bootloader": _req({"keys": _opt(str), "steps": _req(STR_LIST), "id": _opt(str), "sign": _opt(str)}),
        "dfu": _opt({"vid": _req(int), "pid": _req(int), "string1": _req(str), "product": _req(str),
                     "key_sha256": _req(str), "key_prefix": _req(str)}),
        "stub": _opt({"id": _req(int), "latch": _req(int), "key": _req(int), "nand": _req(bool),
                      "progmem": _req(int), "tested": _req(bool), "stock_console": _opt(bool)}),
    }),
    "codec": _req({"tool": _req(CODEC_TOOLS), "stock": _opt(str), "profile": _opt(str), "sources": _opt(STR_LIST),
                   "installed": _opt(STR_LIST)}),
    "frame": _req({"shape": _req(str), "body": _req(str), "accent": _req(str), "lens": _req(RANGE)}),
}

PLATFORM_SCHEMA = {
    "id": _req(str),
    "isa": _req(str),
    "toolchain": _req({"cc": _req(str), "build": _req(str)}),
    "ci": _req({"group": _req(str), "container": _req(bool)}),
    "stubs": _req(STR_LIST),
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

def parse_mk(text):
    """The plain assignments of a makefile outside any conditional: {name: value}. := and = replace, ?= sets once,
    += appends. Enough for board.mk's PLATFORM, LDSCRIPT and BOOT_*; anything computed is make's business."""
    out = {}
    depth = 0
    text = re.sub(r"\\\n", " ", text)
    for line in text.splitlines():
        s = line.split("#", 1)[0].strip()
        if re.match(r"^(ifeq|ifneq|ifdef|ifndef)\b", s):
            depth += 1
            continue
        if s == "endif":
            depth -= 1
            continue
        if depth or not s:
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*(:=|\?=|\+=|=)\s*(.*)$", s)
        if not m:
            continue
        name, op, val = m.group(1), m.group(2), " ".join(m.group(3).split())
        if op == "?=":
            out.setdefault(name, val)
        elif op == "+=":
            out[name] = (out.get(name, "") + " " + val).strip()
        else:
            out[name] = val
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


def board_t_names(board_dir):
    """The string literals board_t .name is set to in a board's sources (a macro name is not seen; such a board names
    its values in [[build_options]] board_name)."""
    names = []
    for f in sorted(os.listdir(board_dir)):
        if f.endswith(".c"):
            with open(os.path.join(board_dir, f), encoding="utf-8") as fh:
                names += re.findall(r'(?m)^\t\.name\s*=\s*"([^"]+)"', fh.read())
    return names


def _stockfw(root):
    """odrflash's stock-file tables (the STM32 codec placements live with the stock files they come from)."""
    path = os.path.join(root, "tools", "odrflash")
    if path not in sys.path:
        sys.path.insert(0, path)
    from odrflash import stockfw
    return stockfw


# ---------------------------------------------------------------------------------------------------------------
# the loader

class Platform:
    def __init__(self, data):
        self.data = data
        self.id = data["id"]


class Board:
    """A board's TOML fields (data) and what is derived from its build files."""

    def __init__(self, data, platform, mk, layout, names):
        self.data = data
        self.id = data["id"]
        self.platform = platform
        self.mk = mk				# board.mk's plain assignments
        self.symbols = layout["symbols"]	# __codec_* of every linker script in the board directory
        self.app_region = layout["flash"]	# (start, end) of MEMORY FLASH in board.mk's LDSCRIPT
        self.source_names = names		# board_t .name literals
        boot = mk.get("BOOT_STAGE")
        self.first_stage = None
        if boot:
            if "BOOT_SIZE" not in mk or "BOOT_SIGNATURE" not in mk:
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
    def codec_slot(self):
        s = self.symbols
        if "__codec_slot" in s and "__codec_slot_end" in s:
            return (s["__codec_slot"], s["__codec_slot_end"])
        return None

    def window(self, lo, hi):
        s = self.symbols
        return (s[lo], s[hi]) if lo in s and hi in s else None

    def sectors(self):
        """[(index, address, size)] the bootloader can erase, in its order."""
        name = self.data["flash"]["sectors"]
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
        return [tuple(s) for s in geo]

    def flash_end(self):
        s = self.sectors()
        return s[-1][1] + s[-1][2]

    def codec(self, root=ROOT):
        """{"profile", "windows": [(addr, length)], "sources", "installed", "code"}: where a merged codec lives.
        code: the address of the codec's first word (STM32: the moved code; HR_C7000: the slot)."""
        c = self.data["codec"]
        if c.get("stock"):
            sfw = _stockfw(root)
            prof = sfw.PROFILES.get(c["stock"])
            if prof is None:
                raise RegistryError("%s: [codec] stock %s... is not a stockfw.PROFILES entry" % (self.id, c["stock"][:12]))
            if prof.get("board") != self.id:
                raise RegistryError("%s: [codec] stock is the %s's stock file" % (self.id, prof.get("board")))
            lay = sfw.layout(prof)
            return {"profile": prof["codec_profile"], "windows": [(a, n) for a, n, _ in lay],
                    "sources": c.get("sources", [prof["name"]]), "installed": c.get("installed", []), "code": lay[0][0]}
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
    """The board's linker facts: board.mk's LDSCRIPT for the application region, every *.ld in its directory for the
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
    end = org + length
    syms = {}
    for f in sorted(os.listdir(bdir)):
        if f.endswith(".ld"):
            with open(os.path.join(bdir, f), encoding="utf-8") as fh:
                for k, v in parse_ld(fh.read())["symbols"].items():
                    if syms.setdefault(k, v) != v:
                        raise RegistryError("boards/%s: %s differs between the linker scripts" % (bid, k))
    # a board whose codec build switches to another script (db25d: CONFIG_DMR) reaches the slot only without the codec;
    # the image odrflash writes alongside a codec stops at the slot
    if "__codec_slot" in syms:
        end = min(end, syms["__codec_slot"])
    return {"flash": (org, end), "symbols": syms}


def load(root=ROOT):
    """{id: Board} for every board directory with a board.mk. Fails on a missing or malformed board.toml."""
    platforms = load_platforms(root)
    out = {}
    for bid in board_dirs(root):
        bdir = os.path.join(root, "boards", bid)
        path = os.path.join(bdir, "board.toml")
        if not os.path.isfile(path):
            raise RegistryError("boards/%s has a board.mk but no board.toml (docs/dev/adding-a-radio.md)" % bid)
        data = _read_toml(path)
        validate(data, BOARD_SCHEMA, "boards/%s/board.toml" % bid)
        if data["id"] != bid:
            raise RegistryError("boards/%s/board.toml: id %r must be the directory name" % (bid, data["id"]))
        with open(os.path.join(bdir, "board.mk"), encoding="utf-8") as f:
            mk = parse_mk(f.read())
        plat = platforms.get(mk.get("PLATFORM", ""))
        if plat is None:
            raise RegistryError("boards/%s: PLATFORM %r has no platform/<p>/platform.toml" % (bid, mk.get("PLATFORM")))
        out[bid] = Board(data, plat, mk, _layout(root, bid, mk), board_t_names(bdir))
    return out


# ---------------------------------------------------------------------------------------------------------------
# consistency

GALLERY_RE = re.compile(r"^\d+x\d+(i\d+(x\d+)?|m)?$")	# a display class tests/host/sim/ui_sim.c sim_geom_find() takes


def check(root=ROOT):
    """Every problem found, as strings (empty: the registry is consistent)."""
    problems = []
    try:
        boards = load(root)
    except RegistryError as e:
        return [str(e)]
    stub_ids = {}
    for bid, b in boards.items():
        d = b.data
        where = "boards/%s/board.toml" % bid

        def bad(msg):
            problems.append("%s: %s" % (where, msg))
        for k, p in d["docs"].items():
            if not os.path.isfile(os.path.join(root, p)):
                bad("docs.%s %s doesn't exist" % (k, p))
        names = set(d["debuglink_names"])
        for n in b.source_names + b.option_board_names():
            if n not in names:
                bad("board_t .name %r (boards/%s/*.c or a build option's board_name) is not in debuglink_names" % (n, bid))
        if bid not in names:
            bad("debuglink_names must hold the id %r (VERSION board= of older builds)" % bid)
        if not GALLERY_RE.match(d["hardware"]["gallery"]):
            bad("hardware.gallery %r is not a display class (WxH, WxHiN, WxHiNxM, WxHm)" % d["hardware"]["gallery"])
        fl = d["flash"]
        try:
            secs = b.sectors()
            codec = b.codec(root)
        except RegistryError as e:
            problems.append(str(e))
            continue
        for (_, a0, s0), (_, a1, _) in zip(secs, secs[1:]):
            if a0 + s0 > a1:
                bad("flash sectors overlap or are out of order at 0x%08X" % a1)
        if secs[0][1] != b.app_base:
            bad("the first sector starts at 0x%08X, the image (linker FLASH origin%s) at 0x%08X"
                % (secs[0][1], " minus the first stage" if b.first_stage else "", b.app_base))
        if not fl["protected"] or fl["protected"][-1][1] != b.app_base:
            bad("flash.protected must end where the image starts, 0x%08X" % b.app_base)
        free_lo = b.app_region[1] if fl["sectors"] != APP_SLOT else b.codec_slot[0]
        for a, n in codec["windows"]:
            if a < free_lo or a + n > b.flash_end():
                bad("codec window 0x%08X+0x%X is outside the flash the linker script keeps free (0x%08X..0x%08X)"
                    % (a, n, free_lo, b.flash_end()))
        if b.app_region[1] > free_lo and fl["sectors"] != APP_SLOT:
            bad("the application region ends past the codec sectors")
        if not os.path.isfile(os.path.join(root, "tools", d["codec"]["tool"] + ".py")):
            bad("codec.tool tools/%s.py doesn't exist" % d["codec"]["tool"])
        if b.platform.id == "hrc7000":
            for lo, hi in (("__codec_run_lo", "__codec_run_hi"), ("__codec_tab_lo", "__codec_tab_hi")):
                if b.window(lo, hi) is None:
                    bad("the linker scripts define no %s / %s" % (lo, hi))
        if fl["family"] == "anyroad-dfu":
            dfu = fl.get("dfu")
            if dfu is None:
                bad("family anyroad-dfu needs [flash.dfu]")
            elif not re.fullmatch(r"[0-9a-f]{64}", dfu["key_sha256"]) or not re.fullmatch(r"[0-9A-F]{8}", dfu["key_prefix"]):
                bad("flash.dfu key_sha256 is 64 lower-case hex digits, key_prefix 8 upper-case")
            elif d["codec"].get("stock"):
                prof = _stockfw(root).PROFILES[d["codec"]["stock"]]
                if prof.get("key_sha256") != dfu["key_sha256"]:
                    bad("flash.dfu key_sha256 is not the key of the codec's stock file (stockfw.PROFILES)")
        elif "dfu" in fl:
            bad("[flash.dfu] belongs to family anyroad-dfu")
        if fl["family"] == "bfuv32-serial" and not (fl["bootloader"].get("id") and fl["bootloader"].get("keys")):
            bad("family bfuv32-serial needs flash.bootloader id and keys (the loader's ID string, the key combination)")
        stub = fl.get("stub")
        if stub is not None:
            if b.platform.id != "hrc7000":
                bad("[flash.stub] is the universal HR_C7000 stub's profile: only for boards on platform hrc7000")
            elif stub["id"] in stub_ids:
                bad("flash.stub id %d is also the %s's" % (stub["id"], stub_ids[stub["id"]]))
            else:
                stub_ids[stub["id"]] = bid
        elif fl["backend"] == "c7000-stub":
            bad("backend c7000-stub needs [flash.stub]")
        if b.first_stage and len(b.first_stage[1]) != 4:
            bad("board.mk BOOT_SIGNATURE is the first stage's first word: 4 bytes")
        for o in d.get("build_options", []):
            if len(o["values"]) < 2:
                bad("build option %s needs at least two values (the first is the default)" % o["name"])
    if not problems:
        path = os.path.join(root, ODRFLASH_JSON)
        try:
            with open(path, encoding="utf-8") as f:
                current = f.read() == dumps(odrflash_view(boards, root))
        except OSError:
            current = False
        if not current:
            problems.append("%s is out of date: run tools/boards.py json --for odrflash --out %s"
                            % (ODRFLASH_JSON, ODRFLASH_JSON))
    return problems


# ---------------------------------------------------------------------------------------------------------------
# derived views

def odrflash_view(boards, root=ROOT):
    """What the flashing tools read (tools/odrflash/odrflash/registry.json): plain JSON, every address a number."""
    out = {}
    for bid, b in sorted(boards.items()):
        d, fl = b.data, b.data["flash"]
        codec = b.codec(root)
        e = {
            "id": bid, "name": d["name"], "model": d["model"], "platform": b.platform.id,
            "platform_stubs": list(b.platform.data["stubs"]), "family": fl["family"],
            "release": d["status"]["release"], "debuglink_names": list(d["debuglink_names"]),
            "app_base": b.app_base, "flash_end": b.flash_end(), "app_region": list(b.app_region),
            "sectors": [list(s) for s in b.sectors()], "protected": [list(p) for p in fl["protected"]],
            "keep_codec": fl["keep_codec"],
            "first_stage": {"size": b.first_stage[0], "head": b.first_stage[1].hex()} if b.first_stage else None,
            "codec": {"tool": d["codec"]["tool"], "profile": codec["profile"], "stock": d["codec"].get("stock"),
                      "windows": [list(w) for w in codec["windows"]], "code": codec["code"],
                      "sources": codec["sources"], "installed": codec["installed"],
                      "slot": list(b.codec_slot) if b.codec_slot else None,
                      "run": list(b.window("__codec_run_lo", "__codec_run_hi") or ()) or None,
                      "tables": list(b.window("__codec_tab_lo", "__codec_tab_hi") or ()) or None},
            "bootloader": {k: fl["bootloader"][k] for k in ("keys", "id", "sign") if k in fl["bootloader"]},
            "dfu": dict(fl["dfu"]) if "dfu" in fl else None,
            "stub": dict(fl["stub"], stock_console=fl["stub"].get("stock_console", False)) if "stub" in fl else None,
            "build_options": [{"name": o["name"], "values": [{"id": v["id"], "board_name": v["board_name"]}
                                                              for v in o["values"]]}
                              for o in d.get("build_options", [])],
        }
        out[bid] = e
    return {"format": "odr-board-registry/1",
            "_about": "Generated by tools/boards.py json --for odrflash from boards/*/board.toml, the boards' board.mk "
                      "and linker scripts. Don't edit: change the sources and run it again.",
            "boards": out}


def full_view(boards, root=ROOT):
    """Every board's TOML fields with the derived ones under "derived"."""
    out = {}
    for bid, b in sorted(boards.items()):
        e = json.loads(json.dumps(b.data))
        e["platform"] = b.platform.id
        e["derived"] = odrflash_view({bid: b}, root)["boards"][bid]
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

    mk = parse_mk("A := 1\nB ?= x\nB ?= y\nifeq ($(X),1)\nA := 2\nelse\nA := 3\nendif\nC := a \\\n\tb\nC += c\n# D := 4\n")
    ok(mk == {"A": "1", "B": "x", "C": "a b c"}, "parse_mk: plain assignments, conditionals skipped %r" % mk)
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
        for top in ("boards", "platform"):
            shutil.copytree(os.path.join(ROOT, top), os.path.join(tmp, top),
                            ignore=lambda d, names: [n for n in names if not (n.endswith((".toml", ".mk", ".ld", ".c"))
                                                                              or os.path.isdir(os.path.join(d, n)))])
        shutil.copytree(os.path.join(ROOT, "tools", "odrflash", "odrflash"), os.path.join(tmp, "tools", "odrflash", "odrflash"))
        for t in CODEC_TOOLS:
            open(os.path.join(tmp, "tools", t + ".py"), "w").close()
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
        broken(re.sub(r'(?m)^debuglink_names = \[.*\]$', 'debuglink_names = ["%s"]' % one, good),
               "a board_t name missing from debuglink_names", "is not in debuglink_names")
        broken(good.replace('hardware = "', 'hardware = "missing/', 1), "a missing docs page", "doesn't exist")
        broken(re.sub(r"(?m)^protected = .*$", "protected = [[0, 1]]", good), "protected not ending at the image",
               "flash.protected")
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
    p = sub.add_parser("builds", help="a board's images besides the default ('name make-args', one per line)")
    p.add_argument("board")
    p.add_argument("--extra", action="store_true", help="the [ci] extra configurations instead")
    p = sub.add_parser("json", help="the merged view")
    p.add_argument("--for", dest="view", choices=("all", "odrflash"), default="all")
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
        boards = load()
        if a.cmd == "list":
            ids = [b for b, x in boards.items() if (not a.platform or x.platform.id == a.platform)
                   and (not a.release or x.data["status"]["release"] == a.release)]
            print(",".join(ids) if a.comma else "\n".join(ids))
        elif a.cmd == "get":
            if a.board not in boards:
                raise RegistryError("unknown board %r (known: %s)" % (a.board, ", ".join(boards)))
            v = full_view({a.board: boards[a.board]})["boards"][a.board]
            try:
                v = lookup(v, a.path)
            except KeyError:
                raise RegistryError("%s has no %s" % (a.board, a.path))
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
            text = dumps(odrflash_view(boards) if a.view == "odrflash" else full_view(boards))
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
