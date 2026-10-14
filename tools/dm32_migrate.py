#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""dm32_migrate: move a Baofeng DM-32UV from its stock firmware to OpenDigitalRadio, keeping the channels, zones,
contacts, RX group lists and your DMR ID, and adding the DMR ID (callsign) database. Untested on a radio.

    dm32_migrate.py all --image odr_dm32_codec.bin           the whole migration
    dm32_migrate.py backup [--from FILE]       read the stock codeplug (or convert a file) and check the conversion
    dm32_migrate.py flash --image FILE         flash ODR (or a stock BFUV32 file) through the Baofeng bootloader
    dm32_migrate.py restore --workdir DIR      write the converted codeplug and the ID database to the radio on ODR
    dm32_migrate.py package OUTDIR --image FILE   a directory to hand to a tester
    dm32_migrate.py --self-test                the pure logic, the CPS writer and the flasher, no radio, no dmrconf

Every command takes --dry-run: offline steps (file conversion, image checks) run, nothing touches the radio.

How the codeplug gets there (docs/spec/dm32-migration.md): qdmr's command line tool dmrconf (an external program,
installed separately) reads the stock radio with its DM-32UV driver and turns the codeplug into qdmr's YAML, then
encodes it offline for qdmr's open firmware family (OpenUV380), the open-firmware layout ODR's CPS shim serves. dmrconf
can't write that to the DM-32 itself: ODR keeps the open-firmware image at +0x700000 in the radio's data flash and its
shim takes physical addresses, so this tool writes dmrconf's encoded image over the programming cable with
cps_client.py, adding 0x700000 to every address. The codeplug steps (RX group list clean-up, encode, TX contacts put
back, CPS write and read back) are shared with the other radios' tools (codeplug_port.py,
docs/spec/codeplug-port.md).

Needs Python 3.8+, dmrconf (qdmr 0.15 or newer, with the dm32uv driver) on PATH and pyserial. The helpers
(dm32_flash.py, cps_client.py, codeplug_port.py, odr_debug.py, md380_migrate.py, dm32_codec.py) are loaded from this script's
directory.
"""
import argparse
import datetime
import importlib.util
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import codeplug_port as cp  # noqa: E402  (next to this script)

STOCK_DRIVER = "dm32uv"       # dmrconf --radio for the stock radio
ODR_DRIVER = "openuv380"      # dmrconf --radio for the open-firmware layout ODR serves (open-firmware-cps.md 7)
CPS_OFFSET = 0x700000         # ODR's open-firmware image in the DM-32 data flash (boards/dm32/flash_dm32.c)
WRITE_FLOOR = 0x700000        # ODR never writes below this (hardware-dm32.md 10.1)
FLASH_SIZE = 0x1000000
STOCK_PAGES = (0x000000, 0x0C9000)     # stock codeplug pages and the factory calibration page (hardware-dm32.md 10.1)
NAME_MAX = 16                 # names in the open-firmware layout (open-firmware-cps.md 7.3)
BANDS = ((136000000, 174000000), (400000000, 480000000))     # boards/dm32/board.c
CPS_BLOCK = 32                # bytes per CPS request, as qdmr
BULK_BLOCK = 512              # bytes per CPS read for the data flash backup
LIST_SECTIONS = ("radioIDs", "contacts", "groupLists", "channels", "zones", "scanLists", "positioning",
                 "roamingChannels", "roamingZones", "smsTemplates", "encryptionKeys")


MigrateError = cp.PortError     # one error type for this tool, the MD-380 tool's session code and the codeplug steps


def load_module(name, candidates=None):
    for rel in candidates or [name + ".py"]:
        path = os.path.join(HERE, rel)
        if os.path.isfile(path):
            spec = importlib.util.spec_from_file_location("dm32mig_" + name, path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    raise MigrateError("%s.py not found next to this script" % name)


_mods = {}


def mod(name):
    if name not in _mods:
        _mods[name] = load_module(name)
    return _mods[name]


# ---------------------------------------------------------------------------------------------------------------
# pure logic: DfuSe files (dmrconf's binary codeplug format)

parse_dfuse = cp.parse_dfuse
build_dfuse = cp.build_dfuse
dfu_regions = cp.dfu_regions
dfu_image = cp.dfu_image
image_to_dfu = cp.image_to_dfu


def find_cal_page(img, base=0):
    """Address of the stock factory calibration page (hardware-dm32.md 11.1: the first 4 KB page from 0x1000 whose
    byte +0xFFF is 2) in a data flash dump starting at `base`, or None."""
    for a in range(max(0x1000, base), min(0xC9000, base + len(img)), 0x1000):
        if img[a - base + 0xFFF] == 2:
            return a
    return None


# ---------------------------------------------------------------------------------------------------------------
# pure logic: what converts (qdmr YAML in, a report out)

def parse_freq(text):
    """Hz from qdmr's '441 MHz', '146.52 MHz', '441000 kHz', '441000000 Hz', or None."""
    m = re.match(r"^\s*([0-9.]+)\s*(MHz|kHz|Hz|GHz)?\s*$", text or "")
    if not m:
        return None
    mult = {"GHz": 1e9, "MHz": 1e6, "kHz": 1e3, "Hz": 1, None: 1e6}[m.group(2)]
    return int(round(float(m.group(1)) * mult))


def in_band(hz):
    return any(lo <= hz <= hi for lo, hi in BANDS)


def conversion_report(stock_text, odr_text=None):
    """What the stock codeplug has, what ODR gets and what is lost or changed on the way."""
    m = cp
    ss = m.yaml_sections(stock_text)
    summary = m.codeplug_summary(stock_text)
    rep = {"stock": {k: len(ss.get(k) or []) for k in LIST_SECTIONS if ss.get(k)},
           "radio_ids": summary["radio_ids"], "over_limits": [], "long_names": [], "non_ascii": [],
           "out_of_band": [], "lost_sections": [], "odr": None, "renamed": []}
    for key, n, lim, dropped in m.limit_report(summary):
        rep["over_limits"].append({"section": key, "count": n, "limit": lim, "dropped": dropped})
    for key in m.ODR_LIMITS:
        for name in summary["names"][key]:
            if any(ord(c) > 126 for c in name):
                rep["non_ascii"].append([key, name])
            elif len(name) > NAME_MAX:
                rep["long_names"].append([key, name, name[:NAME_MAX]])
    for item in ss.get("channels") or []:
        name = m.item_field(item, "name") or "?"
        bad = [f for f in (parse_freq(m.item_field(item, "rxFrequency")), parse_freq(m.item_field(item, "txFrequency")))
               if f is not None and not in_band(f)]
        if bad:
            rep["out_of_band"].append([name, "%.5f MHz" % (bad[0] / 1e6)])
    if odr_text is not None:
        so = m.yaml_sections(odr_text)
        rep["odr"] = {k: len(so.get(k) or []) for k in LIST_SECTIONS if so.get(k) or ss.get(k)}
        for k, n in rep["stock"].items():
            got = rep["odr"].get(k, 0)
            if got < n and k not in m.ODR_LIMITS:
                rep["lost_sections"].append([k, n, got])
        oname = m.codeplug_summary(odr_text)["names"]
        for key in m.ODR_LIMITS:
            for a, b in zip(summary["names"][key], oname[key]):
                if a != b:
                    rep["renamed"].append([key, a, b])
    return rep


def format_report(rep):
    out = ["Conversion report: stock DM-32UV codeplug -> ODR (open-firmware UV380 layout)", ""]
    out.append("%-16s %8s %8s" % ("section", "stock", "ODR"))
    for k in sorted(set(rep["stock"]) | set(rep["odr"] or {})):
        out.append("%-16s %8d %8s" % (k, rep["stock"].get(k, 0), "-" if rep["odr"] is None else rep["odr"].get(k, 0)))
    out.append("")
    out.append("radio IDs in the codeplug: %s" % (", ".join(map(str, rep["radio_ids"])) or "none"))
    for o in rep["over_limits"]:
        shown = ", ".join(o["dropped"][:12]) + (" and %d more" % (len(o["dropped"]) - 12) if len(o["dropped"]) > 12
                                               else "")
        out.append("over ODR's limit: %s %d of %d kept, left out: %s" % (o["section"], o["limit"], o["count"], shown))
    for k, n, got in rep["lost_sections"]:
        out.append("not carried over: %s (%d in the stock codeplug, %d for ODR)" % (k, n, got))
    if rep["long_names"]:
        out.append("names longer than %d characters (%d), cut to:" % (NAME_MAX, len(rep["long_names"])))
        out += ["  %s: %r -> %r" % tuple(x) for x in rep["long_names"]]
    if rep["non_ascii"]:
        out.append("names with characters outside ASCII (%d), shown differently on ODR:" % len(rep["non_ascii"]))
        out += ["  %s: %r" % tuple(x) for x in rep["non_ascii"]]
    if rep["renamed"]:
        out.append("names that differ after the conversion (%d):" % len(rep["renamed"]))
        out += ["  %s: %r -> %r" % tuple(x) for x in rep["renamed"][:50]]
    if rep["out_of_band"]:
        out.append("channels outside the DM-32's bands 136-174 / 400-480 MHz (%d; they won't work on ODR):"
                   % len(rep["out_of_band"]))
        out += ["  %s (%s)" % tuple(x) for x in rep["out_of_band"]]
    out.append("")
    out.append("Not carried over in any case: the stock menu, key and display settings, scan lists, roaming, GPS /")
    out.append("APRS systems, encryption keys, the stock contact database. ODR's own settings start at defaults.")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------------------------
# session

def make_session(args):
    base = mod("md380_migrate").Session

    class Dm32Session(base):
        def open_workdir(self, path=None, create=True):
            if path is None:
                stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
                path = os.path.join(os.path.expanduser("~"), "ODR-DM32-migration", stamp)
            base.open_workdir(self, path, create)

        def dmr(self, *a, radio, check=True, capture=True):
            cmd = [self.dmrconf] + (["--device", self.port] if self.port and radio else []) + list(a)
            rc, out = self.run(cmd, radio, check=False, capture=capture)
            if self.workdir and not (radio and self.dry):
                with open(self.path("dmrconf.log"), "a", encoding="utf-8") as f:
                    f.write("$ %s\n%s(exit status %d)\n\n" % (" ".join(cmd), out, rc))
            if check and rc != 0:
                raise MigrateError("dmrconf %s failed (exit status %d); its output is in %s"
                                   % (a[0], rc, self.path("dmrconf.log")))
            return rc, out

        def odr_debug(self, *a, check=True, capture=True):
            cmd = [sys.executable, os.path.join(HERE, "odr_debug.py")] + (["--port", self.port] if self.port else [])
            return self.run(cmd + list(a), True, check, capture)

    return Dm32Session(args)


def prepare_users(sess, args):
    """A users CSV to users.json, checked with encode-db (md380_migrate's code), once per run."""
    if getattr(args, "users", None) and not sess.cache.get("users_done"):
        mod("md380_migrate").prepare_users(sess, args)
        sess.cache["users_done"] = True


def need_file(sess, path, what):
    if sess.dry and not os.path.exists(path):
        return
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        raise MigrateError("%s was not produced (%s)" % (what, path))


def check_dmrconf(sess):
    if sess.cache.get("dmrconf_ok"):
        return
    try:
        p = subprocess.run([sess.dmrconf, "--list-radios"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
        v = subprocess.run([sess.dmrconf, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
    except FileNotFoundError:
        raise MigrateError("dmrconf not found. Install qdmr 0.15 or newer (macOS: brew install qdmr; Linux: your "
                           "distribution's qdmr package; Windows: https://dm3mat.darc.de/qdmr/) or give --dmrconf PATH")
    keys = set(re.findall(r"^\s*([\w-]+)\s*\|", p.stdout or "", re.M))
    sess.say("dmrconf: %s (%s)" % (sess.dmrconf, (v.stdout or "").strip() or "version unknown"))
    missing = [k for k in (STOCK_DRIVER, ODR_DRIVER) if k not in keys]
    if missing:
        raise MigrateError("this dmrconf has no %s driver: install qdmr 0.15 or newer" % " / ".join(missing))
    sess.cache["dmrconf_ok"] = True


def resolve_port(sess, args):
    """The programming cable: --port, else the only USB-serial bridge. The stock CPS protocol, the bootloader and
    ODR's host link all use it."""
    if sess.port or sess.cache.get("port_failed"):
        return sess.port
    try:
        sess.port = mod("dm32_flash").find_port(getattr(args, "port", None))
        sess.say("programming cable: %s" % sess.port)
    except mod("dm32_flash").FlashError as e:
        if not sess.dry:
            raise MigrateError(str(e))
        sess.cache["port_failed"] = True
        sess.say("   [dry run] no cable: %s" % e)
    return sess.port


def open_cps(sess, offset):
    try:
        return mod("cps_client").open_cps(sess.port, offset=offset)
    except Exception as e:
        raise MigrateError("can't open the CPS link on %s: %s" % (sess.port, e))


def bar(done, total, _last=[-1]):
    pct = done * 100 // max(total, 1)
    if pct != _last[0] or done >= total:
        _last[0] = pct
        sys.stdout.write("\r   %d%%" % pct)
        sys.stdout.flush()
    if done >= total:
        _last[0] = -1
        sys.stdout.write("\n")


# ---------------------------------------------------------------------------------------------------------------
# step 1: backup and convert

def step_backup(sess, args):
    sess.step("1. Back up the stock codeplug and convert it for ODR")
    check_dmrconf(sess)
    if args.source:
        src = os.path.abspath(args.source)
        if not os.path.isfile(src):
            raise MigrateError("%s not found" % src)
        ext = os.path.splitext(src)[1].lower()
        if ext not in (".dfu", ".yaml", ".yml"):
            raise MigrateError("--from takes a dmrconf .dfu or a qdmr .yaml file (read with --radio dm32uv)")
        keep = sess.path("original" + ext)
        shutil.copyfile(src, keep)
        sess.say("   codeplug file %s, %d bytes" % (src, os.path.getsize(src)))
        kind = "yaml" if ext != ".dfu" else "dfu"
    else:
        resolve_port(sess, args)
        sess.wait_user("""
            Radio: switch it on normally with its stock firmware and connect the programming cable.
            Close the Baofeng CPS, qdmr and any other program that uses the cable.""")
        sess.dmr("detect", radio=True, check=False)
        keep = sess.path("backup-stock.dfu")
        sess.dmr("read", "--radio", STOCK_DRIVER, keep, radio=True)
        need_file(sess, keep, "the codeplug read from the radio")
        kind = "dfu"
        if os.path.exists(keep):
            with open(keep, "rb") as f:
                sess.say("   saved %s, sha256 %s" % (keep, mod("md380_migrate").sha256(f.read())))
    yaml_path = sess.path("backup-stock.yaml")
    if kind == "yaml":
        shutil.copyfile(keep, yaml_path)
    else:
        sess.dmr("decode", "--radio", STOCK_DRIVER, keep, yaml_path, radio=args.source is None)
    need_file(sess, yaml_path, "the codeplug in qdmr YAML")
    sess.save_state(stock_backup=keep, stock_yaml=yaml_path, port=sess.port)
    return convert(sess, args, yaml_path)


def convert(sess, args, yaml_path):
    """Clean up, encode for ODR offline, put back the TX contacts, decode again, report (codeplug_port.py). The
    encoded file is exactly what restore writes."""
    if not os.path.exists(yaml_path):
        sess.say("   [dry run] the conversion (group list clean-up, encode, TX contacts put back) runs once the "
                 "codeplug has been read")
        return True
    odr_dfu, odr_yaml, _ = cp.convert_for_odr(sess, yaml_path, ODR_DRIVER, cleanup=not args.no_cleanup)
    need_file(sess, odr_yaml, "the codeplug converted for ODR")
    with open(odr_dfu, "rb") as f:
        targets = parse_dfuse(f.read())
    with open(yaml_path, encoding="utf-8") as f:
        stock_text = f.read()
    with open(odr_yaml, encoding="utf-8") as f:
        odr_text = f.read()
    rep = conversion_report(stock_text, odr_text)
    text = format_report(rep)
    with open(sess.path("conversion-report.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    with open(sess.path("conversion-report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, indent=1)
    for line in text.rstrip().splitlines():
        sess.say("   " + line)
    sess.say("   (saved as conversion-report.txt)")
    regions = dfu_regions(targets)
    sess.say("   encoded for ODR: %d bytes in %d regions" % (sum(e - s for s, e in regions), len(regions)))
    if rep["over_limits"]:
        sess.say("   To choose yourself, open %s in qdmr, remove what you don't need, save it as YAML, and run" % yaml_path)
        sess.say("   this step again with --from on the saved file.")
        if not args.accept_trim and not sess.ask("Continue with the items that fit?"):
            raise MigrateError("stopped before flashing; the backup is in %s" % sess.workdir)
    after = cp.codeplug_summary(odr_text)
    if not rep["radio_ids"]:
        sess.say("   ! the codeplug has no DMR ID: set yours in ODR's settings after the migration (or pass --id for "
                 "the ID database)")
    sess.save_state(odr_yaml=odr_yaml, odr_dfu=odr_dfu, expected=after["counts"], radio_ids=rep["radio_ids"])
    prepare_users(sess, args)
    sess.say("   backup done: %s" % sess.workdir)
    return True


# ---------------------------------------------------------------------------------------------------------------
# step 2: flash

def prepare_image(sess, args):
    if "image" in sess.cache:
        return sess.cache["image"]
    fl = mod("dm32_flash")
    try:
        raw, kind = fl.load(args.image)
        notes = fl.check_image(raw)
    except fl.FlashError as e:
        raise MigrateError(str(e))
    sess.say("   image %s (%s)" % (args.image, "BFUV32-V2 file, header stripped" if kind == "bfuv32" else "raw"))
    for n in notes:
        sess.say("   " + n)
    if not fl.has_codec_slot(raw):
        if getattr(args, "codec_from", None):
            cd = mod("dm32_codec")
            with open(args.codec_from, "rb") as f:
                stock = f.read()
            try:
                prof, code, tables = cd.extract(stock)
                raw = cd.merge(raw, prof, code, tables)
            except cd.ExtractError as e:
                raise MigrateError(str(e))
            sess.say("   merged the codec from %s: %d bytes" % (args.codec_from, len(raw)))
        elif getattr(args, "stock_image", False):
            sess.say("   flashing a stock firmware file (--stock-image)")
        else:
            sess.say("   ! no ODR codec slot: if this is an ODR image, the radio will work on FM only (no DMR voice).")
            sess.say("     Use the merged image (odr_dm32_codec.bin) or give --codec-from DM32.01.01.049.bin.")
            sess.say("     For a stock firmware file, pass --stock-image.")
            if not sess.ask("Flash it anyway?"):
                raise MigrateError("stopped before flashing")
    sess.cache["image"] = raw
    return raw


def preflight(sess, args):
    """Everything 'all' needs, checked before the radio leaves its stock firmware."""
    sess.step("0. Check the tools, the cable and the image")
    check_dmrconf(sess)
    missing = []
    try:
        import serial  # noqa: F401
    except ImportError:
        missing.append("pyserial: pip install pyserial")
    for helper in ("dm32_flash", "cps_client", "odr_debug"):
        if not os.path.isfile(os.path.join(HERE, helper + ".py")):
            missing.append("%s.py next to this script" % helper)
    for m in missing:
        sess.say("   ! missing: " + m)
    if missing and not sess.dry:
        raise MigrateError("install what is missing first; nothing has been done to the radio")
    if not missing:
        resolve_port(sess, args)
    prepare_image(sess, args)


def step_flash(sess, args):
    sess.step("2. Flash OpenDigitalRadio")
    raw = prepare_image(sess, args)
    fl = mod("dm32_flash")
    resolve_port(sess, args)
    sess.say("   %d blocks of 1 KB to 0x%08X; the bootloader itself (0x03000000..0x0300C000) is never written"
             % (fl.blocks_needed(len(raw)), fl.APP_BASE))
    if sess.dry:
        sess.say("   [dry run] would wait for update mode and write the image")
        return True
    sess.wait_user("""
        Radio: switch it OFF. Hold PTT and SK1 (the side key just below PTT) and switch it on while holding
        both. The green LED lights and the screen stays dark: that is update mode. Release the keys.
        Leave the cable connected.""")
    if not sess.ask("Is the green LED on (update mode), and should the firmware be written now?"):
        raise MigrateError("stopped before flashing; nothing was written")
    sp = None
    try:
        sp = fl.SerialPort(sess.port)
        fl.Flasher(sp, log=sess.say, progress=fl.progress_bar).flash(raw, os.path.basename(args.image))
    except fl.FlashError as e:
        raise MigrateError("flashing failed: %s. The bootloader is never erased: enter update mode again and rerun "
                           "'flash'. Your codeplug backup is in %s" % (e, sess.workdir))
    finally:
        if sp:
            sp.close()
    sess.say("   written; the radio restarts. If it stays dark: remove and refit the battery, switch it on.")
    sess.save_state(flashed=fl.sha256(raw))
    return True


# ---------------------------------------------------------------------------------------------------------------
# step 3: ODR running, settings area, data flash backup

def wait_odr(sess, timeout):
    if sess.dry:
        sess.say("   [dry run] would wait for ODR's debug link on %s" % (sess.port or "the cable"))
        return
    cmd = [sys.executable, os.path.join(HERE, "odr_debug.py"), "--port", sess.port, "ping"]
    sess.say("   waiting for ODR on %s ..." % sess.port)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if p.returncode == 0:
            sess.say("   " + p.stdout.strip())
            return
        if "pyserial" in (p.stdout or ""):
            raise MigrateError("pyserial is missing: pip install pyserial")
        time.sleep(2)
    raise MigrateError("the radio did not answer on %s within %d s. Switch it off and on; if the screen stays dark, "
                       "remove the battery, enter update mode and flash again" % (sess.port, timeout))


def step_prepare_odr(sess, args):
    sess.step("3. Check ODR on the radio and give it its settings area")
    resolve_port(sess, args)
    sess.wait_user("""
        Radio: it should be running ODR now (the ODR Home screen). If the screen is dark, remove and refit the
        battery and switch it on. Keep the cable connected.""")
    wait_odr(sess, args.wait)
    rc, out = sess.odr_debug("version")
    if not sess.dry and "DM-32" not in out:
        raise MigrateError("the radio does not report board=DM-32UV over its debug link (see above)")
    rc, out = sess.odr_debug("storage", "info")
    region = re.search(r"region\s+(\w+)", out or "")
    region = region.group(1) if region else ("foreign" if sess.dry else "?")
    if region == "none":
        raise MigrateError("ODR found no usable data flash, so it keeps no codeplug. Your backup is in %s"
                           % sess.workdir)
    if region == "foreign":
        sess.say("   The flash area ODR keeps its settings in (0x711000..) holds the stock firmware's data. ODR")
        sess.say("   doesn't overwrite it on its own. This saves the area (raw) into the work directory and then")
        sess.say("   gives it to ODR. The stock codeplug and calibration below 0x700000 are not touched.")
        if not sess.ask("Give the settings area to ODR now?"):
            raise MigrateError("stopped: codeplug writes are refused until the area is ODR's")
        sess.odr_debug("storage", "claim", "--out", sess.path("storage-before-claim"))
    elif region != "ours":
        raise MigrateError("unexpected storage state %r" % region)
    sess.say("   the settings area is ODR's")
    return True


def step_flash_backup(sess, args):
    """Read the stock data below ODR's area through ODR's CPS reads (physical addresses), into the work directory."""
    mode = args.flash_backup
    if mode == "none":
        return True
    lo, hi = STOCK_PAGES if mode == "stock" else (0, FLASH_SIZE)
    name = "dataflash-%06X-%06X.bin" % (lo, hi)
    sess.step("3b. Save the stock data flash 0x%06X..0x%06X (read only)" % (lo, hi))
    if os.path.isfile(sess.path(name)) and os.path.getsize(sess.path(name)) == hi - lo:
        sess.say("   already saved: %s" % name)
        return True
    sess.say("   %d KB at 115200 baud: about %d minutes" % ((hi - lo) // 1024, ((hi - lo) // 9000 + 59) // 60))
    if sess.dry:
        sess.say("   [dry run] would read it with cps_client (area 1, physical addresses) into %s" % name)
        return True
    cps = open_cps(sess, 0)
    try:
        img = cps.read_image([(lo, hi)], image=bytearray(hi), block=BULK_BLOCK, progress=bar)[lo:hi]
    except Exception as e:
        raise MigrateError("reading the data flash failed: %s" % e)
    finally:
        cps.close()
    with open(sess.path(name), "wb") as f:
        f.write(img)
    cal = find_cal_page(img, lo)
    sess.say("   saved %s; factory calibration page: %s" % (name, "0x%06X" % cal if cal is not None else
                                                          "not found (look at the file before going on)"))
    return True


# ---------------------------------------------------------------------------------------------------------------
# step 4 / 5: write the codeplug and the ID database through ODR's CPS shim

def cps_write_dfu(sess, dfu_path, label, before_name, reboot=True):
    """Back up what the regions hold, write the DfuSe file's elements at +CPS_OFFSET, return (targets, image)."""
    return cp.cps_write_dfu(sess, lambda: open_cps(sess, CPS_OFFSET), dfu_path, label, before_name, offset=CPS_OFFSET,
                            floor=WRITE_FLOOR, ceiling=0xF00000, block=CPS_BLOCK, progress=bar, reboot=reboot)


def step_restore(sess, args):
    sess.step("4. Write the codeplug to the radio")
    check_dmrconf(sess)
    if args.yaml:
        convert(sess, args, os.path.abspath(args.yaml))
    src = sess.state.get("odr_dfu")
    if sess.dry and not src:
        sess.say("   [dry run] would write odr-codeplug.dfu at +0x%06X, then read it back and compare" % CPS_OFFSET)
        return True
    if not src or (not sess.dry and not os.path.isfile(src)):
        raise MigrateError("no converted codeplug: run 'backup' first or give --yaml FILE")
    expected = sess.state.get("expected") or {}
    targets, img = cps_write_dfu(sess, src, "codeplug", "openfw-window-before.bin")
    if sess.dry:
        return True
    sess.say("   written; the radio restarts")
    time.sleep(3)
    wait_odr(sess, args.wait)
    cp.check_read_back(sess, lambda: open_cps(sess, CPS_OFFSET), targets, img, expected, ODR_DRIVER,
                       block=CPS_BLOCK, progress=bar)
    sess.save_state(restored=True)
    return True


def step_iddb(sess, args):
    sess.step("5. Write the DMR ID (callsign) database")
    if args.no_db:
        sess.say("   skipped (--no-db)")
        return True
    ids = [args.id] if args.id else (sess.state.get("radio_ids") or [])
    if not ids and sess.dry:
        sess.say("   [dry run] would encode the database around the codeplug's DMR ID and write it at +0x%06X"
                 % CPS_OFFSET)
        return True
    if not ids:
        raise MigrateError("no DMR ID to choose the database around: give --id YOUR_ID, or --no-db")
    out = sess.path("iddb.dfu")
    cmd = ["encode-db", "--radio", ODR_DRIVER, "--id",
           ",".join(str(i)[:3] if args.id_prefix else str(i) for i in ids)]
    users = args.users_json or sess.state.get("users_json")
    if users:
        cmd += ["--database", users]
        sess.say("   from your users file (%s records)" % sess.state.get("users_count", "?"))
    else:
        sess.say("   dmrconf downloads the public DMR user database (internet needed)")
    if args.db_limit:
        cmd += ["--limit", str(args.db_limit)]
    if sess.dry:
        sess.say("   [dry run] would run: dmrconf %s %s" % (" ".join(cmd), out))
        sess.say("   [dry run] and write it at +0x%06X" % CPS_OFFSET)
        return True
    sess.dmr(*(cmd + [out]), radio=False)
    need_file(sess, out, "the encoded ID database")
    sess.say("   a large database takes many minutes at 115200 baud; don't unplug the cable")
    wait_odr(sess, args.wait)
    cps_write_dfu(sess, out, "ID database", "iddb-area-before.bin")
    sess.say("   done: names show on the receive screen for IDs that are not contacts")
    return True


# ---------------------------------------------------------------------------------------------------------------
# package for a tester

PACKAGE_README = """\
OpenDigitalRadio for the Baofeng DM-32UV: migration kit (TESTING ONLY, never run on a radio before)

This kit backs up your stock codeplug, flashes OpenDigitalRadio over the programming cable and writes your
channels, zones, contacts, RX group lists and the DMR ID database back. The DM-32 port has never run on a radio:
you are the first. Keep Baofeng's stock firmware file (DM32.01.01.049.bin) at hand to go back.

Not for redistribution: the firmware image contains the codec from Baofeng's firmware.

ODR never writes the data flash below 0x700000, where the stock codeplug and the factory calibration live, and never
writes the bootloader. Update mode (PTT + SK1 at power-on, green LED) stays available whatever happens.

Install once
  - Python 3.8 or newer.
  - qdmr 0.15 or newer, which brings the dmrconf command (macOS: brew install qdmr; Linux: the qdmr package of
    your distribution; Windows: the qdmr installer from https://dm3mat.darc.de/qdmr/).
  - pip install -r requirements.txt   (pyserial)
  - The programming cable's driver (CH340 / FTDI / CP210x). Linux: add yourself to the dialout group.

Run
  python3 dm32_migrate.py all --image {image} --dry-run     (see every step, touches nothing)
  python3 dm32_migrate.py all --image {image}

The tool asks before each step that needs you at the radio. It keeps every file in
~/ODR-DM32-migration/<date-time>/ and never deletes them; send that folder back with your report. If a step fails it
stops and says how to go on; 'restore --workdir DIR' continues after the flash.

Flash only (no codeplug):   python3 dm32_flash.py {image}
Back to stock:              python3 dm32_flash.py --stock DM32.01.01.049.bin
  (the stock codeplug is still in the radio; backup-stock.dfu can be written with dmrconf write --radio dm32uv)
Bring-up checklist (what to look at on the first boot): dm32-bringup.md if included, else docs/testing/dm32-bringup.md
in the source.
"""


def cmd_package(sess, args):
    out = os.path.abspath(args.outdir)
    files = {os.path.abspath(__file__): "dm32_migrate.py"}
    for helper in ("dm32_flash.py", "cps_client.py", "codeplug_port.py", "odr_debug.py", "md380_migrate.py",
                   "dm32_codec.py"):
        files[os.path.join(HERE, helper)] = helper
    bringup = os.path.join(os.path.dirname(HERE), "docs", "testing", "dm32-bringup.md")
    if os.path.isfile(bringup):
        files[bringup] = "dm32-bringup.md"
    for img in args.image:
        files[os.path.abspath(img)] = os.path.basename(img)
    for extra in args.include or []:
        files[os.path.abspath(extra)] = os.path.basename(extra)
    for src in files:
        if not os.path.isfile(src):
            raise MigrateError("%s not found" % src)
    fl = mod("dm32_flash")
    for img in args.image:
        with open(img, "rb") as f:
            raw, _ = fl.unwrap(f.read())
        try:
            fl.check_image(raw)
        except fl.FlashError as e:
            raise MigrateError("%s: %s" % (img, e))
        if not fl.has_codec_slot(raw):
            sess.say("   ! %s has no codec slot: the tester gets FM only" % img)
    sess.say("package %s:" % out)
    for src, dst in files.items():
        sess.say("   %s <- %s" % (dst, src))
    if sess.dry:
        return True
    os.makedirs(out, exist_ok=True)
    for src, dst in files.items():
        shutil.copyfile(src, os.path.join(out, dst))
    with open(os.path.join(out, "README.txt"), "w", encoding="utf-8") as f:
        f.write(PACKAGE_README.format(image=os.path.basename(args.image[0])))
    with open(os.path.join(out, "requirements.txt"), "w", encoding="utf-8") as f:
        f.write("pyserial>=3.4\n")
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as f:
        for dst in sorted(set(files.values()) | {"README.txt", "requirements.txt"}):
            with open(os.path.join(out, dst), "rb") as g:
                f.write("%s  %s\n" % (mod("md380_migrate").sha256(g.read()), dst))
    sess.say("   done: hand the directory over as it is (README.txt says what to install)")
    return True


# ---------------------------------------------------------------------------------------------------------------
# self-test

class FakeCpsRadio:
    """ODR's CPS shim on a DM-32 (cps-protocol.md 2, boards/dm32/flash_dm32.c map) as a cps_client transport:
    physical addresses, writes only inside the open-firmware EEPROM image (0x700000..0x710000) and flash image
    (0x720000..0xF00000), nothing below 0x700000."""

    def __init__(self):
        self.sectors = {}
        self.pending = None
        self.out = b""
        self.commits = 0

    def byte_range(self, a, n):
        out = bytearray()
        while n:
            s, o = divmod(a, 4096)
            k = min(n, 4096 - o)
            out += self.sectors.get(s, b"\xff" * 4096)[o:o + k]
            a, n = a + k, n - k
        return bytes(out)

    def poke(self, a, data):
        for i, b in enumerate(data):
            s, o = divmod(a + i, 4096)
            self.sectors.setdefault(s, bytearray(b"\xff" * 4096))[o] = b

    @staticmethod
    def writable(a):
        return 0x700000 <= a < 0x710000 or 0x720000 <= a < 0xF00000

    def write(self, req):
        c = req[0]
        if c == 0x52:
            area, addr, n = req[1], int.from_bytes(req[2:6], "big"), int.from_bytes(req[6:8], "big")
            if area == 9:
                d = struct.pack("<II", 3, 6) + b"abc1234".ljust(16, b"\0") + b"20261011120000".ljust(16, b"\0") + \
                    struct.pack("<IH", 0x4018, 0)
            elif area == 1 and addr + n <= FLASH_SIZE:
                d = self.byte_range(addr, n)
            else:
                self.out += b"-"
                return
            self.out += b"R" + len(d).to_bytes(2, "big") + d
        elif c == 0x58 and req[1] == 1:
            a = int.from_bytes(req[2:5], "big") * 4096
            if not self.writable(a):
                self.pending = None
                self.out += b"-"
                return
            self.pending = (a, bytearray(self.byte_range(a, 4096)))
            self.out += b"X\x01"
        elif c == 0x58 and req[1] == 2:
            a, n = int.from_bytes(req[2:6], "big"), int.from_bytes(req[6:8], "big")
            if self.pending is None:
                self.out += b"-"
                return
            base, buf = self.pending
            if base <= a and a + n <= base + 4096:
                buf[a - base:a - base + n] = req[8:8 + n]
            self.out += b"X\x02"
        elif c == 0x58 and req[1] == 3:
            if self.pending is None:
                self.out += b"-"
                return
            self.poke(*self.pending)
            self.pending = None
            self.commits += 1
            self.out += b"X\x03"
        elif c == 0x43:
            self.out += b"-"
        else:
            self.out += b"-"

    def read(self, n, timeout):
        r, self.out = self.out[:n], self.out[n:]
        return r

    def close(self):
        pass


SAMPLE_STOCK_YAML = """version: 0.15.1
settings:
  defaultID: id1
radioIDs:
  - dmr: {id: id1, name: N0CALL, number: 3100001}
  - dmr: {id: id2, name: N0CALL-2, number: 3100009}
contacts:
  - dmr: {id: cont1, name: Local, ring: false, type: GroupCall, number: 9}
  - dmr: {id: cont2, name: "Worldwide Talkgroup 91", ring: false, type: GroupCall, number: 91}
  - dmr: {id: cont3, name: "René", ring: false, type: PrivateCall, number: 3100002}
groupLists:
  - id: grp1
    name: Local
    contacts: [cont1, cont2]
channels:
  - dmr:
      id: ch1
      name: Simplex DMR
      rxFrequency: 441 MHz
      txFrequency: 441 MHz
      extended:
        dcdm: false
  - fm:
      id: ch2
      name: Simplex FM 2m calling
      rxFrequency: 146.52 MHz
      txFrequency: 146.52 MHz
  - fm:
      id: ch3
      name: Air
      rxFrequency: 118.1 MHz
      txFrequency: 118.1 MHz
  - fm:
      id: ch4
      name: 6m FM
      rxFrequency: 52525 kHz
      txFrequency: 52525 kHz
zones:
""" + "".join("  - id: zone%d\n    name: Z%d\n    A: [ch1, ch2]\n" % (i, i) for i in range(1, 71)) + """\
scanLists:
  - id: scan1
    name: All
    channels: [ch1, ch2]
roamingZones:
  - id: rz1
    name: Roam
commercial:
  encryptionKeys:
    - basic: {id: key1, name: K, key: "1234"}
"""


def sample_odr_yaml():
    """What dmrconf's OpenUV380 round trip makes of SAMPLE_STOCK_YAML (names cut, zones and scan lists dropped)."""
    y = SAMPLE_STOCK_YAML.replace("Worldwide Talkgroup 91", "Worldwide Talkgr").replace(
        "Simplex FM 2m calling", "Simplex FM 2m ca")
    y = y.split("zones:\n")[0] + "zones:\n" + "".join("  - id: zone%d\n    name: Z%d\n    A: [ch1, ch2]\n" % (i, i)
                                                       for i in range(1, 69))
    return y.replace("  - dmr: {id: id2, name: N0CALL-2, number: 3100009}\n", "")


def self_test():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL: " + what)

    # DfuSe files, laid out as dmrconf 0.15.1 writes them for openuv380 (an empty EEPROM target, four elements)
    flash = [(0x80, bytes(range(256)) * 2), (0x7500, b"\x11" * 300), (0x20000, b"\x22" * 64), (0x9B000, b"\x33" * 5000)]
    targets = [("Codeplug EEPROM", []), ("Codeplug FLASH", flash)]
    data = build_dfuse(targets)
    check(parse_dfuse(data) == targets, "DfuSe round trip")
    check(data[-16:-4] == b"\xff" * 6 + b"\x1a\x01UFD\x10", "DfuSe suffix as dmrconf writes it")
    resized = bytearray(data[:-4])
    resized[6] += 1         # a wrong image size behind a valid CRC
    resized = bytes(resized) + struct.pack("<I", (~zlib.crc32(bytes(resized))) & 0xFFFFFFFF)
    for bad, what in ((data[:-1] + bytes([data[-1] ^ 1]), "CRC"), (data[:600] + bytes([data[600] ^ 1]) + data[601:],
                      "damaged body"), (resized, "size"), (b"not a dfu" + bytes(40), "no prefix")):
        try:
            parse_dfuse(bad)
            check(False, "DfuSe %s refused" % what)
        except MigrateError:
            pass
    img, regions = dfu_image(targets)
    check(regions == [(0x80, 0x280), (0x7500, 0x762C), (0x20000, 0x20040), (0x9B000, 0x9C388)], "regions %r" % regions)
    check(img[0x80:0x280] == flash[0][1] and img[0x7F] == 0xFF and len(img) == 0x120000, "flat image")
    check(image_to_dfu(targets, img) == targets, "read back to DfuSe")

    # the conversion report
    rep = conversion_report(SAMPLE_STOCK_YAML, sample_odr_yaml())
    check(rep["stock"]["channels"] == 4 and rep["stock"]["zones"] == 70 and rep["stock"]["radioIDs"] == 2,
          "stock counts %r" % rep["stock"])
    check(rep["odr"]["zones"] == 68 and rep["odr"].get("scanLists", 0) == 0, "ODR counts %r" % rep["odr"])
    check([o["section"] for o in rep["over_limits"]] == ["zones"] and rep["over_limits"][0]["dropped"] == ["Z69", "Z70"],
          "zones over the limit")
    check(sorted(x[0] for x in rep["lost_sections"]) == ["radioIDs", "roamingZones", "scanLists"],
          "lost sections %r" % rep["lost_sections"])
    check(["contacts", "Worldwide Talkgroup 91", "Worldwide Talkgr"] in rep["long_names"] and
          ["channels", "Simplex FM 2m calling", "Simplex FM 2m ca"] in rep["long_names"], "long names")
    check(rep["non_ascii"] == [["contacts", "René"]], "non-ASCII names %r" % rep["non_ascii"])
    check(len(rep["renamed"]) == 2, "renamed after the round trip %r" % rep["renamed"])
    check(sorted(x[0] for x in rep["out_of_band"]) == ["6m FM", "Air"], "out of band %r" % rep["out_of_band"])
    check(rep["radio_ids"] == [3100001, 3100009], "radio IDs")
    text = format_report(rep)
    check("left out: Z69, Z70" in text and "scanLists" in text and "6m FM" in text, "report text")
    check(parse_freq("462562.5 kHz") == 462562500 and parse_freq("441 MHz") == 441000000 and parse_freq("x") is None,
          "frequencies")
    check(conversion_report(SAMPLE_STOCK_YAML)["odr"] is None, "report without the ODR side")

    # the CPS writer against a simulated DM-32 shim: +0x700000, nothing below it
    cc = mod("cps_client")
    radio = FakeCpsRadio()
    stock = bytearray(b"\x5a" * 0x10000)
    stock[0x3FFF] = 2           # the calibration page's ID byte (page 0x3000)
    radio.poke(0, stock)
    radio.poke(0x700000 + 0x9B000, b"\x77" * 8192)      # whatever stock left in ODR's window
    cps = cc.Cps(radio, timeout=0.1, offset=CPS_OFFSET)
    check(cps.info()["radio_type"] == 6, "radio info through the shim")
    before = cps.read_image(regions, image=bytearray(b"\xff" * len(img)))
    check(before[0x9B000:0x9B010] == b"\x77" * 16, "backup of the window before writing")
    cps.write_image(img, regions, reboot=True, base=before)
    check(radio.byte_range(0x700080, 512) == flash[0][1] and radio.byte_range(0x79B000, 5000) == flash[3][1],
          "codeplug lands at +0x700000")
    check(radio.byte_range(0x79C388, 16) == b"\x77" * 16, "bytes outside the regions kept")
    check(radio.byte_range(0, 0x10000) == bytes(stock), "stock area below 0x700000 untouched")
    back = cps.read_image(regions, image=bytearray(b"\xff" * len(img)))
    check(cc.compare(img, back, regions) == [], "read back matches")
    check(build_dfuse(image_to_dfu(targets, back)) == data, "read back makes the same DfuSe file")
    try:
        cc.Cps(radio, timeout=0.1).write_image(img, regions)
        check(False, "without the offset the shim refuses the writes")
    except cc.CpsError:
        pass
    dump = cc.Cps(radio, timeout=0.1).read_image([(0, 0x10000)], image=bytearray(0x10000), block=BULK_BLOCK)
    check(bytes(dump) == bytes(stock) and find_cal_page(dump) == 0x3000, "stock data flash backup, calibration page")

    rc = mod("dm32_flash").self_test()
    check(rc == 0, "dm32_flash self-test")
    print("dm32_migrate self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                      "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


# ---------------------------------------------------------------------------------------------------------------

EPILOG = """\
examples:
  %(prog)s all --image odr_dm32_codec.bin --dry-run
  %(prog)s all --image odr_dm32_codec.bin --port /dev/ttyUSB0 --users user.csv
  %(prog)s backup --from my-dm32.yaml                     (convert and check only)
  %(prog)s flash --image odr_dm32.bin --codec-from DM32.01.01.049.bin     (merges the codec)
  %(prog)s flash --image DM32.01.01.049.bin --stock-image                 (back to stock)
  %(prog)s restore --workdir ~/ODR-DM32-migration/20261011-101500

The steps of 'all': backup (radio on stock firmware) -> flash (update mode) -> settings area -> data flash backup
-> codeplug -> ID database. Each asks before anything happens at the radio; it stops at the first failure and never
deletes a file.
"""


def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--dry-run", action="store_true", help="run the offline steps; print what would touch the radio")
    common.add_argument("--yes", "-y", action="store_true", help="don't ask (physical steps are still announced)")
    common.add_argument("--workdir", help="where backups and converted files go (default ~/ODR-DM32-migration/DATE)")
    common.add_argument("--port", help="the programming cable's serial port (default: the only USB-serial cable)")
    common.add_argument("--dmrconf", help="path of dmrconf (default: from PATH)")
    common.add_argument("--wait", type=int, default=120, help="seconds to wait for the radio (default 120)")

    u = argparse.ArgumentParser(add_help=False)
    u.add_argument("--users", metavar="CSV", help="DMR users file (radioid.net user.csv) for the ID database; "
                                                 "default: dmrconf downloads it")
    u.add_argument("--users-columns", help="column order of a users file without a header, e.g. "
                                           "id,callsign,fname,surname,city,state,country")

    b = argparse.ArgumentParser(add_help=False)
    b.add_argument("--from", dest="source", metavar="FILE",
                   help="convert this codeplug instead of reading the radio: a dmrconf .dfu (read with --radio "
                        "dm32uv) or qdmr YAML")
    t = argparse.ArgumentParser(add_help=False)
    t.add_argument("--accept-trim", action="store_true", help="continue without asking when something doesn't fit")
    t.add_argument("--no-cleanup", action="store_true", help="convert the codeplug as it is: keep every RX group list "
                   "(by default lists that are empty or only repeat the channel's TX contact are dropped)")

    f = argparse.ArgumentParser(add_help=False)
    f.add_argument("--image", required=True, help="the ODR image (odr_dm32_codec.bin, raw or _BFUV32.bin), or a "
                                                  "stock BFUV32 file with --stock-image")
    f.add_argument("--codec-from", metavar="FILE", help="Baofeng's DM32.01.01.049.bin: merge its codec into an image "
                                                        "without one")
    f.add_argument("--stock-image", action="store_true", help="the image is a stock firmware file (no codec check)")

    r = argparse.ArgumentParser(add_help=False)
    r.add_argument("--yaml", help="write this qdmr YAML instead of the converted backup")
    r.add_argument("--flash-backup", choices=("stock", "full", "none"), default="stock",
                   help="before writing, save the data flash through ODR: 'stock' the stock codeplug and calibration "
                        "pages 0x0-0xC9000 (default, about 2 minutes), 'full' all 16 MB (about 30 minutes)")
    r.add_argument("--id", type=int, help="your DMR ID, for choosing the ID database (default: from the codeplug)")
    r.add_argument("--id-prefix", action="store_true", help="choose the database by the ID's 3-digit prefix")
    r.add_argument("--users-json", help="a user database in dmrconf's JSON form (default: from backup --users)")
    r.add_argument("--db-limit", type=int, help="at most this many database records")
    r.add_argument("--no-db", action="store_true", help="don't write the ID database")

    ap = argparse.ArgumentParser(description="Move a Baofeng DM-32UV from stock firmware to OpenDigitalRadio, keeping "
                                             "the codeplug.", epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="test the logic offline (no radio, no dmrconf)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("backup", parents=[common, b, t, u], help="read (or import) the codeplug and convert it for ODR")
    sub.add_parser("import", parents=[common, b, t, u], help="same as backup --from FILE")
    sub.add_parser("flash", parents=[common, f], help="flash through the Baofeng bootloader")
    sub.add_parser("restore", parents=[common, r, t, u], help="settings area, data flash backup, codeplug, ID database")
    sub.add_parser("all", parents=[common, b, t, f, r, u], help="backup, flash, restore")
    p = sub.add_parser("package", parents=[common], help="a directory for a tester")
    p.add_argument("outdir")
    p.add_argument("--image", action="append", required=True, help="image(s) to include (the codec-merged raw image)")
    p.add_argument("--include", action="append", help="further files")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.cmd:
        ap.print_help()
        return 2
    if args.cmd == "import" and not args.source:
        ap.error("import needs --from FILE")
    try:
        sess = make_session(args)
    except MigrateError as e:
        print("dm32_migrate: %s" % e, file=sys.stderr)
        return 1
    sess.cache["dmrconf_ok"] = False
    try:
        if args.cmd == "package":
            return 0 if cmd_package(sess, args) else 1
        if args.cmd == "restore":
            sess.open_workdir(args.workdir, create=args.workdir is None or args.yaml is not None)
            sess.port = sess.port or sess.state.get("port")
            if not args.yaml and not sess.state.get("odr_dfu"):
                raise MigrateError("no converted codeplug in %s: give --workdir of an earlier backup, or --yaml"
                                   % sess.workdir)
            prepare_users(sess, args)
            step_prepare_odr(sess, args)
            step_flash_backup(sess, args)
            step_restore(sess, args)
            step_iddb(sess, args)
        else:
            sess.open_workdir(args.workdir)
            if args.cmd == "all":
                preflight(sess, args)
            if args.cmd in ("backup", "import", "all"):
                step_backup(sess, args)
            if args.cmd in ("flash", "all"):
                if args.cmd == "all" and not sess.ask("The codeplug is backed up. Flash ODR now?"):
                    raise MigrateError("stopped before flashing; the backup is in %s" % sess.workdir)
                step_flash(sess, args)
            if args.cmd == "all":
                step_prepare_odr(sess, args)
                step_flash_backup(sess, args)
                step_restore(sess, args)
                step_iddb(sess, args)
        sess.say("")
        sess.say("finished%s. Files: %s" % (" (dry run: nothing was written to the radio)" if sess.dry else "",
                                            sess.workdir or "-"))
        return 0
    except MigrateError as e:      # the shared codeplug steps and session code raise the same type
        sess.say("")
        sess.say("stopped: %s" % e)
        if sess.state.get("flashed") and sess.state.get("odr_dfu") and args.cmd in ("all", "restore"):
            sess.say("to continue once the cause is fixed: %s restore --workdir \"%s\""
                     % (os.path.basename(sys.argv[0]), sess.workdir))
        return 1
    except OSError as e:
        sess.say("")
        sess.say("stopped: %s" % e)
        return 1
    except KeyboardInterrupt:
        sess.say("")
        sess.say("stopped by you. Files: %s" % (sess.workdir or "-"))
        return 1


if __name__ == "__main__":
    sys.exit(main())
