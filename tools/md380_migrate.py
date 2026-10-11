#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""md380_migrate: move a TYT MD-380 / MD-390 / Retevis RT3 from stock or md380tools (KD4Z toolkit) firmware to
OpenDigitalRadio, keeping the channels, zones, contacts, RX group lists and the DMR ID (callsign) database.

    md380_migrate.py all --image odr_md380_codec.bin --stock "MD-380-D2.32(AD).bin"     the whole migration
    md380_migrate.py all --image odr_md380_codec.bin --key md380.key --from my.rdt      codeplug from a CPS file
    md380_migrate.py backup [--from FILE]       read the codeplug from the stock radio (or convert a file) and check it
    md380_migrate.py flash --image FILE ...     flash ODR (or any TYT updater file) through the TYT bootloader
    md380_migrate.py restore --workdir DIR      write the converted codeplug and the ID database to the radio on ODR
    md380_migrate.py package OUTDIR --image FILE   a directory to hand to a tester
    md380_migrate.py --self-test                the pure conversion logic, no radio, no dmrconf

Every command takes --dry-run: offline steps (file conversion, image checks) run, nothing touches the radio.

The codeplug conversion is done by qdmr's command line tool dmrconf (an external program, installed separately):
the stock codeplug is read with dmrconf's TyT MD-390 driver (the MD-380, MD-390 and RT3 share one codeplug format),
turned into qdmr's radio-independent YAML, and written to ODR with the driver for qdmr's open firmware family
(OpenUV380), which is what ODR's CPS compatibility layer speaks. docs/spec/md380-migration.md has the details,
docs/user/18-moving-from-stock-md380.md the user guide.

Needs Python 3.8+, dmrconf (qdmr 0.12 or newer) on PATH, pyusb with libusb (flashing) and pyserial (talking to the
radio once it runs ODR). The flash and debug link helpers (stm32_dfu.py, odr_debug.py, odrflash's stockfw.py) are
loaded from this script's directory or the repository layout.
"""
import argparse
import csv
import datetime
import hashlib
import importlib.util
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# TYT MD-380 / MD-390 CPS codeplug file (.rdt): a 549-byte header, the 256 KB codeplug image, a 16-byte trailer. The
# sizes are the published ones (the TYT CPS and the md380tools documentation); dmrconf's manufacturer reader for
# the md390 checks the total size (262709 bytes)
CODEPLUG_SIZE = 0x40000
RDT_HEADER = 549
RDT_TRAILER = 16
RDT_SIZE = RDT_HEADER + CODEPLUG_SIZE + RDT_TRAILER

STOCK_DRIVER = "md390"        # dmrconf --radio for the stock codeplug (MD-380, MD-390, RT3)
ODR_DRIVER = "openuv380"      # dmrconf --radio for ODR (open-firmware-cps.md 4: radio type 7, OpenUV380 family)

# What ODR's codeplug layout holds (docs/spec/open-firmware-cps.md 7.2-7.3, docs/user/15-programming-qdmr.md).
# dmrconf's OpenUV380 encoder drops what doesn't fit without an error, so these are checked before writing
ODR_LIMITS = {"channels": 1024, "zones": 68, "contacts": 1024, "groupLists": 76}
SECTION_NAMES = {"channels": "channels", "zones": "zones", "contacts": "contacts", "groupLists": "RX group lists"}

APP_BASE = 0x0800C000
FLASH_END = 0x08100000
CODEC_SIG = 0x4FF0E92D
CODEC_SIG_ADDR = 0x08050C44   # first word of the MD-380 D002.032 codec code window (docs/spec/codec-md380.md)
CONTAINER_MAGIC = b"OutSecurityBin"
CONTAINER_HEADER = 0x100
MD380_KEY_SHA256 = "5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd"


class MigrateError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# pure logic (covered by --self-test)

def classify_codeplug(data, name=""):
    """What a codeplug file is: 'rdt' (TYT CPS / KD4Z toolkit), 'raw' (a 256 KB image, as md380tools dumps it),
    'dfu' (dmrconf's binary format), 'yaml' (qdmr). Raises MigrateError for anything else."""
    low = name.lower()
    if data.startswith(b"DfuSe"):
        return "dfu"
    if len(data) == RDT_SIZE:
        return "rdt"
    if len(data) == CODEPLUG_SIZE:
        return "raw"
    if low.endswith((".yaml", ".yml")) or data.lstrip()[:8] in (b"version:", b"---\nvers"):
        return "yaml"
    if len(data) >= 0x100000:
        raise MigrateError("%s is %d bytes: that looks like a whole SPI flash or firmware dump, not a codeplug. Save "
                           "the codeplug with the TYT CPS (.rdt) or read it from the radio instead" % (name, len(data)))
    raise MigrateError("%s: %d bytes is neither a TYT .rdt codeplug (%d bytes), a raw 256 KB codeplug image (%d bytes), "
                       "a dmrconf .dfu file nor qdmr YAML" % (name, len(data), RDT_SIZE, CODEPLUG_SIZE))


def raw_to_rdt(raw):
    """Wrap a raw 256 KB codeplug image in the .rdt frame dmrconf's manufacturer reader takes. The header and trailer
    are zero: dmrconf reads only the image (a file for qdmr, not for the TYT CPS)."""
    if len(raw) != CODEPLUG_SIZE:
        raise MigrateError("a raw codeplug image is %d bytes, not %d" % (CODEPLUG_SIZE, len(raw)))
    return bytes(RDT_HEADER) + raw + bytes(RDT_TRAILER)


def yaml_sections(text):
    """Split qdmr's block-style YAML into {top-level key: [item text, ...]} for list sections ('  - ' items at indent
    2) and {key: ''} for the others. Enough for counting and naming: qdmr writes its files in this regular form."""
    out, key, items = {}, None, None
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = re.match(r"^([A-Za-z_][\w]*):(.*)$", line)
        if m:
            key = m.group(1)
            rest = m.group(2).strip()
            items = out.setdefault(key, [])
            if rest.startswith("[") and rest.endswith("]") and rest[1:-1].strip():
                items.extend(x.strip() for x in rest[1:-1].split(","))
            continue
        if key is None:
            continue
        if re.match(r"^  - ", line) or re.match(r"^- ", line):
            items.append(line.split("- ", 1)[1] + "\n")
        elif items:
            items[-1] += line.strip() + "\n"
    return out


def item_field(item, field):
    """The value of `field:` in one YAML item (inline {...} or block form), or None."""
    m = re.search(r"(?:^|[{,\s])" + re.escape(field) + r":\s*(\"[^\"]*\"|'[^']*'|[^,}\n]*)", item)
    if not m:
        return None
    v = m.group(1).strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
        v = v[1:-1]
    return v


def codeplug_summary(text):
    """Counts and names of what ODR uses, plus the radio's own DMR ID(s), from qdmr YAML."""
    sec = yaml_sections(text)
    s = {"counts": {}, "names": {}, "radio_ids": []}
    for key in ODR_LIMITS:
        items = sec.get(key) or []
        s["counts"][key] = len(items)
        s["names"][key] = [item_field(i, "name") or "?" for i in items]
    for item in sec.get("radioIDs") or []:
        n = item_field(item, "number")
        if n and n.isdigit():
            s["radio_ids"].append(int(n))
    return s


def limit_report(summary, limits=ODR_LIMITS):
    """[(key, count, limit, [dropped names])] for every section that doesn't fit ODR."""
    out = []
    for key, lim in limits.items():
        n = summary["counts"].get(key, 0)
        if n > lim:
            out.append((key, n, lim, summary["names"][key][lim:]))
    return out


def compare_counts(expected, got):
    """[(key, expected, got)] where a read back differs from what was written (after ODR's limits)."""
    return [(k, min(expected.get(k, 0), lim), got.get(k, 0)) for k, lim in ODR_LIMITS.items()
            if min(expected.get(k, 0), lim) != got.get(k, 0)]


USER_FIELDS = {
    "id": ("radio_id", "radioid", "id", "dmr_id", "dmrid", "dmr id", "radio id", "radio_id"),
    "callsign": ("callsign", "call", "call_sign", "call sign"),
    "fname": ("first_name", "fname", "firstname", "first name", "name"),
    "surname": ("last_name", "surname", "lastname", "last name"),
    "city": ("city",),
    "state": ("state", "province"),
    "country": ("country",),
}
# Columns of a users file without a header row. Override with --users-columns when your file differs: the tool shows
# the first records before it uses them
DEFAULT_USER_COLUMNS = ("id", "callsign", "fname", "surname", "city", "state", "country")


def parse_users_csv(text, columns=None):
    """DMR users from a CSV (radioid.net's user.csv, or the users file of md380tools / the KD4Z toolkit). A header row
    is recognized by its column names; otherwise `columns` (default DEFAULT_USER_COLUMNS) names them. Lines whose
    first field is not a DMR ID (a size line, comments, blanks) are skipped. Returns a list sorted by ID, one entry per
    ID (the first one wins)."""
    rows = list(csv.reader(io.StringIO(text)))
    names = None
    if rows and not (rows[0] and rows[0][0].strip().isdigit()):
        head = [c.strip().lower() for c in rows[0]]
        mapped = []
        for c in head:
            mapped.append(next((f for f, aliases in USER_FIELDS.items() if c in aliases), None))
        if "id" in mapped and "callsign" in mapped:
            names = mapped
            rows = rows[1:]
    if names is None:
        names = list(columns or DEFAULT_USER_COLUMNS)
    seen, users = set(), []
    for r in rows:
        if len(r) < 2:
            continue
        rec = {}
        for i, f in enumerate(names):
            if f and i < len(r):
                rec[f] = r[i].strip()
        rid = rec.get("id", "")
        if not rid.isdigit() or not rec.get("callsign"):
            continue
        rid = int(rid)
        if not 1 <= rid <= 0xFFFFFF or rid in seen:
            continue
        seen.add(rid)
        u = {"id": rid, "callsign": rec["callsign"].upper()}
        for f in ("fname", "surname", "city", "state", "country"):
            if rec.get(f):
                u[f] = rec[f]
        users.append(u)
    users.sort(key=lambda u: u["id"])
    return users


def users_json(users):
    """The custom user database for `dmrconf write-db --database`: {"users": [{"id", "callsign", "fname", "surname",
    "city", "state", "country"}]}. Checked with dmrconf 0.15.1 encode-db (records 'CALL Name City State Country')."""
    return json.dumps({"users": users}, ensure_ascii=False, indent=0) + "\n"


def classify_image(data):
    """('container', [(addr, len)]) for a TYT updater file, ('plain', has_codec) for an ODR application image."""
    if data.startswith(CONTAINER_MAGIC):
        if len(data) < CONTAINER_HEADER + 8:
            raise MigrateError("truncated TYT updater file")
        count = struct.unpack_from("<I", data, 0x7C)[0]
        if count == 0xFFFFFFFF:
            count = 1
        if not 1 <= count <= 15:
            raise MigrateError("TYT updater file: implausible segment count %d" % count)
        segs = [struct.unpack_from("<II", data, 0x80 + 8 * i) for i in range(count)]
        return "container", segs
    if data.startswith(b"\x7fELF"):
        raise MigrateError("this is an ELF file: use the .bin")
    if not data or APP_BASE + len(data) > FLASH_END:
        raise MigrateError("image size %d does not fit 0x%08X..0x%08X" % (len(data), APP_BASE, FLASH_END))
    off = CODEC_SIG_ADDR - APP_BASE
    has_codec = len(data) >= off + 4 and struct.unpack_from("<I", data, off)[0] == CODEC_SIG
    return "plain", has_codec


def container_stream(data, segs):
    """The encrypted stream the bootloader takes, straight out of a TYT updater file. The file's payload is XORed with
    the key indexed from the payload start, and the bootloader stream with the key indexed from 0x0800C000
    (hardware-md380.md 14.2-14.3), so when the segments start at 0x0800C000 and are contiguous the payload is sent as
    it is, without the key."""
    if segs[0][0] != APP_BASE:
        raise MigrateError("the updater file starts at 0x%08X, not 0x%08X: not an MD-380 application file"
                           % (segs[0][0], APP_BASE))
    addr = APP_BASE
    for a, n in segs:
        if a != addr:
            raise MigrateError("the updater file's segments are not contiguous (0x%08X after 0x%08X)" % (a, addr))
        addr += n
    if addr > FLASH_END:
        raise MigrateError("the updater file runs past the end of flash")
    total = addr - APP_BASE
    if CONTAINER_HEADER + total > len(data):
        raise MigrateError("the updater file is shorter than its segment table says")
    return data[CONTAINER_HEADER:CONTAINER_HEADER + total]


def xor_key(data, key):
    reps = (len(data) + len(key) - 1) // len(key)
    return bytes(a ^ b for a, b in zip(data, (key * reps)[:len(data)]))


# ---------------------------------------------------------------------------------------------------------------
# helpers: modules, messages, external programs

def load_module(name, candidates):
    for rel in candidates:
        path = os.path.join(HERE, rel)
        if os.path.isfile(path):
            spec = importlib.util.spec_from_file_location("md380mig_" + name, path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    raise MigrateError("%s.py not found next to this script (looked in: %s)" % (name, ", ".join(candidates)))


def stm32_dfu():
    return load_module("stm32_dfu", ["stm32_dfu.py"])


def stockfw():
    return load_module("stockfw", ["stockfw.py", os.path.join("odrflash", "odrflash", "stockfw.py")])


def odr_debug_path():
    p = os.path.join(HERE, "odr_debug.py")
    if not os.path.isfile(p):
        raise MigrateError("odr_debug.py not found next to this script")
    return p


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class Session:
    """Messages, prompts, the dry-run switch, a log in the work directory and the state file."""

    def __init__(self, args):
        self.dry = args.dry_run
        self.yes = args.yes
        self.port = getattr(args, "port", None)
        self.dmrconf = getattr(args, "dmrconf", None) or shutil.which("dmrconf") or "dmrconf"
        self.workdir = None
        self.cache = {}
        self.log = None
        self.state = {}

    def open_workdir(self, path=None, create=True):
        if path is None:
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            path = os.path.join(os.path.expanduser("~"), "ODR-MD380-migration", stamp)
        path = os.path.abspath(os.path.expanduser(path))
        if create:
            os.makedirs(path, exist_ok=True)
        elif not os.path.isdir(path):
            raise MigrateError("work directory %s does not exist" % path)
        self.workdir = path
        self.log = open(os.path.join(path, "migration.log"), "a", encoding="utf-8")
        self.state = self.load_state()
        self.say("work directory: %s (nothing in it is ever deleted)" % path)

    def path(self, name):
        return os.path.join(self.workdir, name)

    def load_state(self):
        try:
            with open(self.path("state.json"), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def save_state(self, **kv):
        self.state.update(kv)
        if self.workdir:
            with open(self.path("state.json"), "w", encoding="utf-8") as f:
                json.dump(self.state, f, indent=1, sort_keys=True)

    def say(self, msg=""):
        print(msg, flush=True)
        if self.log:
            self.log.write(msg + "\n")
            self.log.flush()

    def step(self, title):
        self.say("")
        self.say("== " + title)

    def ask(self, question):
        if self.yes:
            self.say(question + " [yes: --yes]")
            return True
        if self.dry:
            self.say(question + " [dry run: yes]")
            return True
        if not sys.stdin.isatty():
            raise MigrateError("needs an answer (%s); run in a terminal or pass --yes" % question)
        while True:
            a = input(question + " [y/n] ").strip().lower()
            if a in ("y", "yes"):
                return True
            if a in ("n", "no"):
                return False

    def wait_user(self, what):
        """A physical step (power, keys, cable): the user says when it's done."""
        self.say("")
        for line in what.strip().splitlines():
            self.say("   " + line.strip())
        if self.dry or self.yes:
            self.say("   [%s: not waiting]" % ("dry run" if self.dry else "--yes"))
            return
        if not sys.stdin.isatty():
            raise MigrateError("needs you at the radio; run in a terminal")
        input("   Press Enter when done (Ctrl-C stops; your backup stays in %s) " % self.workdir)

    def run(self, cmd, radio, check=True, capture=False):
        """Run an external command. radio=True: it talks to the radio, so a dry run only prints it."""
        shown = " ".join('"%s"' % c if " " in c else c for c in cmd)
        if radio and self.dry:
            self.say("   [dry run] would run: " + shown)
            return 0, ""
        self.say("   $ " + shown)
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE if capture else None, stderr=subprocess.STDOUT
                               if capture else None, universal_newlines=True)
        except FileNotFoundError:
            raise MigrateError("%s not found: install qdmr (it brings dmrconf) or give --dmrconf PATH" % cmd[0])
        out = p.stdout or ""
        if capture and out:
            for line in out.rstrip().splitlines():
                self.say("     " + line)
        if check and p.returncode != 0:
            raise MigrateError("%s failed (exit status %d)" % (os.path.basename(cmd[0]), p.returncode))
        return p.returncode, out

    def dmr(self, *args, radio, check=True, capture=False):
        cmd = [self.dmrconf]
        if self.port and radio:
            cmd += ["--device", self.port]
        return self.run(cmd + list(args), radio, check, capture)

    def odr_debug(self, *args, check=True, capture=True):
        cmd = [sys.executable, odr_debug_path()]
        if self.port:
            cmd += ["--port", self.port]
        return self.run(cmd + list(args), True, check, capture)


def need_file(sess, path, what):
    if sess.dry and not os.path.exists(path):
        return
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        raise MigrateError("%s was not produced (%s)" % (what, path))


def check_dmrconf(sess):
    if sess.cache.get("dmrconf_ok"):
        return
    try:
        p = subprocess.run([sess.dmrconf, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True)
    except FileNotFoundError:
        raise MigrateError("dmrconf not found. Install qdmr (macOS: brew install qdmr; Linux: your distribution's "
                           "qdmr package; https://dm3mat.darc.de/qdmr/) or give --dmrconf PATH")
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", p.stdout or "")
    ver = tuple(int(x) for x in m.groups()) if m else None
    sess.say("dmrconf: %s (%s)" % (sess.dmrconf, ".".join(map(str, ver)) if ver else "version unknown"))
    if ver and ver < (0, 12, 0):
        raise MigrateError("dmrconf %s is too old: qdmr 0.12 or newer is needed (custom user database, MD-390 "
                           "codeplug files, the OpenUV380 driver)" % ".".join(map(str, ver)))
    sess.cache["dmrconf_ok"] = True


# ---------------------------------------------------------------------------------------------------------------
# step 1: backup and convert

def print_summary(sess, title, summary):
    c = summary["counts"]
    sess.say("   %s: %d channels, %d zones, %d contacts, %d RX group lists; radio ID %s"
             % (title, c["channels"], c["zones"], c["contacts"], c["groupLists"],
                ", ".join(map(str, summary["radio_ids"])) or "none"))


def step_backup(sess, args):
    sess.step("1. Back up the codeplug and convert it for ODR")
    check_dmrconf(sess)
    if args.source:
        src = os.path.abspath(args.source)
        with open(src, "rb") as f:
            data = f.read()
        kind = classify_codeplug(data, src)
        sess.say("   codeplug file %s: %s, %d bytes, sha256 %s" % (src, kind, len(data), sha256(data)))
        keep = sess.path("original" + os.path.splitext(src)[1].lower())
        shutil.copyfile(src, keep)
    else:
        sess.wait_user("""
            Radio: switch it on normally with its stock or md380tools firmware, and connect the programming
            cable. Close the TYT CPS and any other program that uses the radio.""")
        sess.dmr("detect", radio=True, check=False, capture=True)
        keep = sess.path("backup-stock.dfu")
        sess.dmr("read", "--radio", args.stock_radio, keep, radio=True)
        need_file(sess, keep, "the codeplug read from the radio")
        kind = "dfu"
        if os.path.exists(keep):
            with open(keep, "rb") as f:
                sess.say("   saved %s, sha256 %s" % (keep, sha256(f.read())))
    yaml_path = sess.path("backup-stock.yaml")
    if kind == "yaml":
        shutil.copyfile(keep, yaml_path)
    elif kind == "raw":
        rdt = sess.path("backup-stock.rdt")
        with open(keep, "rb") as f:
            raw = f.read()
        with open(rdt, "wb") as f:
            f.write(raw_to_rdt(raw))
        sess.say("   wrapped the raw image as %s for dmrconf" % rdt)
        sess.dmr("decode", "--manufacturer", "--radio", args.stock_radio, rdt, yaml_path, radio=False)
    elif kind == "rdt":
        sess.dmr("decode", "--manufacturer", "--radio", args.stock_radio, keep, yaml_path, radio=False)
    else:
        sess.dmr("decode", "--radio", args.stock_radio, keep, yaml_path, radio=args.source is None)
    need_file(sess, yaml_path, "the codeplug in qdmr YAML")
    sess.save_state(stock_backup=keep, stock_yaml=yaml_path)

    # the conversion check: encode for ODR offline, decode again, so what restore writes is known before flashing
    check_bin = sess.path("odr-codeplug.dfu")
    odr_yaml = sess.path("odr-codeplug.yaml")
    offline = os.path.exists(yaml_path)
    sess.dmr("encode", "--radio", ODR_DRIVER, yaml_path, check_bin, radio=not offline)
    sess.dmr("decode", "--radio", ODR_DRIVER, check_bin, odr_yaml, radio=not offline)
    if not offline:
        sess.say("   [dry run] the conversion check runs once the codeplug has been read")
        return True
    need_file(sess, odr_yaml, "the codeplug converted for ODR")
    with open(yaml_path, encoding="utf-8") as f:
        before = codeplug_summary(f.read())
    with open(odr_yaml, encoding="utf-8") as f:
        after = codeplug_summary(f.read())
    print_summary(sess, "stock codeplug", before)
    print_summary(sess, "for ODR       ", after)
    over = limit_report(before)
    for key, n, lim, dropped in over:
        shown = ", ".join(dropped[:8]) + (" and %d more" % (len(dropped) - 8) if len(dropped) > 8 else "")
        sess.say("   ! ODR holds %d %s, the codeplug has %d: these are left out: %s"
                 % (lim, SECTION_NAMES[key], n, shown))
    if over:
        sess.say("     To choose yourself, open %s in qdmr, remove what you don't need, save it, and run" % yaml_path)
        sess.say("     this step again with --from on the saved file.")
        if not args.accept_trim and not sess.ask("Continue with the items that fit?"):
            raise MigrateError("stopped before flashing; the backup is in %s" % sess.workdir)
    if not before["radio_ids"]:
        sess.say("   ! the codeplug has no DMR ID: set yours in qdmr after the migration (or pass --id for the ID "
                 "database)")
    sess.save_state(odr_yaml=odr_yaml, expected=after["counts"], radio_ids=before["radio_ids"])
    prepare_users(sess, args)
    sess.say("   backup done: %s" % sess.workdir)
    return True


def prepare_users(sess, args):
    if not getattr(args, "users", None):
        return
    with open(args.users, encoding="utf-8", errors="replace") as f:
        text = f.read()
    cols = args.users_columns.split(",") if args.users_columns else None
    users = parse_users_csv(text, cols)
    if not users:
        raise MigrateError("no DMR users found in %s (check --users-columns)" % args.users)
    sess.say("   users file %s: %d DMR IDs. First records as read:" % (args.users, len(users)))
    for u in users[:3]:
        sess.say("     %d %s %s %s" % (u["id"], u["callsign"], u.get("fname", ""), u.get("country", "")))
    if not sess.ask("Do these look right (ID, callsign, name, country)?"):
        raise MigrateError("give the column order with --users-columns, e.g. id,callsign,fname,surname,city,state,"
                           "country")
    path = sess.path("users.json")
    with open(path, "w", encoding="utf-8") as f:
        f.write(users_json(users))
    # offline check that dmrconf takes the file: encode it the way write-db will
    ids = [args.id] if getattr(args, "id", None) else (sess.state.get("radio_ids") or [users[0]["id"]])
    check_dmrconf(sess)
    sess.dmr("encode-db", "--radio", ODR_DRIVER, "--database", path, "--id", str(ids[0]), "--limit", "16",
             sess.path("users-check.dfu"), radio=False)
    sess.save_state(users_json=path, users_count=len(users))


# ---------------------------------------------------------------------------------------------------------------
# step 2: flash

def load_key(sess, args):
    if args.key:
        with open(args.key, "rb") as f:
            key = f.read()
        if len(key) != 1024 or sha256(key) != MD380_KEY_SHA256:
            raise MigrateError("%s is not the MD-380 bootloader key (1024 bytes, sha256 %s...)"
                               % (args.key, MD380_KEY_SHA256[:16]))
        sess.say("   key: %s (checked)" % args.key)
        return key
    if args.stock:
        sfw = stockfw()
        with open(args.stock, "rb") as f:
            stock = f.read()
        sess.say("   deriving the bootloader key from %s ..." % args.stock)
        try:
            key = sfw.derive_key(stock, want_sha256=MD380_KEY_SHA256)
        except sfw.StockError as e:
            raise MigrateError(str(e))
        sess.say("   key: derived and checked by its SHA-256")
        return key
    return None


def prepare_stream(sess, args):
    """The encrypted stream for the bootloader (None on a dry run without a key). Worked out once per run."""
    if "stream" in sess.cache:
        return sess.cache["stream"]
    with open(args.image, "rb") as f:
        data = f.read()
    kind, info = classify_image(data)
    sess.say("   image %s: %d bytes, sha256 %s" % (args.image, len(data), sha256(data)))
    key = load_key(sess, args)
    if kind == "container":
        stream = container_stream(data, info)
        sess.say("   a TYT updater file (already encrypted), %d bytes from 0x%08X: sent as it is"
                 % (len(stream), APP_BASE))
        if key is not None:
            sp, reset = struct.unpack_from("<II", xor_key(stream[:8], key))
            if not (0x20000000 <= sp <= 0x20020000 or 0x10000000 <= sp <= 0x10010000) or \
                    not APP_BASE <= (reset & ~1) < FLASH_END:
                raise MigrateError("the updater file doesn't decrypt to an MD-380 application with this key")
            sess.say("   checked with the key: a valid vector table")
        sess.cache["stream"] = stream
        return stream
    dfu = stm32_dfu()
    if not info:
        if args.stock:
            sfw = stockfw()
            with open(args.stock, "rb") as f:
                stock = f.read()
            try:
                prof, segs = sfw.extract(stock, key)
                data = sfw.merge(data, prof, segs)
            except sfw.StockError as e:
                raise MigrateError(str(e))
            sess.say("   merged the codec from %s: %d bytes" % (args.stock, len(data)))
        else:
            sess.say("   ! this image has no codec: the radio will work on FM only (no DMR voice). Use the merged")
            sess.say("     image (odr_md380_codec.bin) or give --stock \"MD-380-D2.32(AD).bin\" to merge it.")
            if not sess.ask("Flash it anyway?"):
                raise MigrateError("stopped before flashing")
    try:
        warnings = dfu.check_image(data, "md380")
    except dfu.DfuError as e:
        raise MigrateError(str(e))
    for w in warnings:
        sess.say("   ! " + w)
    if warnings and not args.force:
        raise MigrateError("the image has warnings (above); fix them or pass --force")
    if key is None:
        if not sess.dry:
            raise MigrateError("flashing an ODR image needs the bootloader key: --key FILE, or --stock with TYT's "
                               "\"MD-380-D2.32(AD).bin\" to derive it")
        sess.say("   [dry run] no --key or --stock: plan only")
        stream = None
    else:
        stream = dfu.xor_image(data, key)
    sess.cache["stream"] = stream
    sess.cache["plain_len"] = len(data)
    return stream


def preflight(sess, args):
    """Everything 'all' needs, checked before the radio leaves its stock firmware."""
    sess.step("0. Check the tools and the image")
    check_dmrconf(sess)
    missing = []
    try:
        dfu = stm32_dfu()
        dfu.import_usb()
    except Exception as e:      # DfuError, or the module missing
        missing.append("pyusb + libusb (flashing): %s" % e)
    try:
        import serial  # noqa: F401  (odr_debug.py needs it once the radio runs ODR)
    except ImportError:
        missing.append("pyserial (talking to the radio on ODR): pip install pyserial")
    odr_debug_path()
    for m in missing:
        sess.say("   ! missing: " + m)
    if missing and not sess.dry:
        raise MigrateError("install what is missing first; nothing has been done to the radio")
    prepare_stream(sess, args)


def wait_dfu(sess, dfu, timeout):
    usb = dfu.import_usb()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        devs = dfu.find_devices(usb)
        if len(devs) == 1:
            return usb
        if len(devs) > 1:
            raise MigrateError("%d devices 0483:DF11 are connected: connect only the radio" % len(devs))
        time.sleep(1)
    raise MigrateError("the bootloader did not appear within %d s (USB 0483:DF11)" % timeout)


def step_flash(sess, args):
    sess.step("2. Flash OpenDigitalRadio")
    stream = prepare_stream(sess, args)
    dfu = stm32_dfu()
    shown = stream if stream is not None else bytes(sess.cache.get("plain_len", 0))
    if shown:
        for line in dfu.describe_plan(dfu.plan_blocks(shown), len(shown)):
            sess.say("   " + line)
    if sess.dry:
        sess.say("   [dry run] would wait for the bootloader and write the image")
        return True
    sess.wait_user("""
        Radio: switch it OFF. Hold PTT and the UPPER side key (the one just above PTT), and switch it on while
        holding both. The screen stays dark and the LED blinks: that is the bootloader. Release the keys.
        Leave the cable connected.""")
    try:
        usb = wait_dfu(sess, dfu, args.wait)
        dev, intf = dfu.open_device(usb, args.force)
        man = dfu.get_string(usb, dev, dev.iManufacturer)
        prod = dfu.get_string(usb, dev, dev.iProduct)
    except dfu.DfuError as e:
        raise MigrateError(str(e))
    sess.say("   found 0483:DF11: manufacturer %r, product %r" % (man, prod))
    sess.say("   The radio in normal programming mode uses the same USB ID. If the screen is lit, it is NOT in the")
    sess.say("   bootloader: answer no, and enter the bootloader as above.")
    if not sess.ask("Is the screen dark (bootloader) and should the firmware be written now?"):
        raise MigrateError("stopped before flashing; nothing was written")

    def progress(done, total):
        sys.stdout.write("\r   writing %d/%d blocks (%d%%)" % (done, total, done * 100 // total))
        sys.stdout.flush()
        if done == total:
            sys.stdout.write("\n")

    try:
        dfu.Dfu(dev, intf, log=lambda *a: sess.say("   " + " ".join(str(x) for x in a))).flash(
            dfu.plan_blocks(stream), progress)
    except Exception as e:
        raise MigrateError("flashing failed: %s. The bootloader is never erased: enter it again and rerun "
                           "'flash'. Your codeplug backup is in %s" % (e, sess.workdir))
    sess.say("   written; the radio restarts into ODR")
    sess.save_state(flashed=sha256(stream))
    return True


# ---------------------------------------------------------------------------------------------------------------
# step 3: make the flash area ODR's, write the codeplug and the ID database

def wait_odr(sess, timeout):
    if sess.dry:
        sess.say("   [dry run] would wait for the radio on ODR (USB serial 1FC9:0094)")
        return
    cmd = [sys.executable, odr_debug_path()] + (["--port", sess.port] if sess.port else []) + ["ping"]
    sess.say("   waiting for the radio on ODR (USB serial 1FC9:0094) ...")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
        if p.returncode == 0:
            sess.say("   " + p.stdout.strip())
            return
        if "pyserial" in (p.stdout or ""):
            raise MigrateError("pyserial is missing: pip install pyserial")
        time.sleep(2)
    raise MigrateError("the radio did not answer on its USB serial port within %d s. Switch it off and on; if the "
                       "screen stays dark, flash again (the bootloader is never erased)" % timeout)


def step_prepare_odr(sess, args):
    sess.step("3. Check ODR on the radio and give it its settings area")
    sess.wait_user("""
        Radio: it should be running ODR now (the ODR home screen). If the screen is dark, switch it off and on
        normally. Keep the cable connected.""")
    wait_odr(sess, args.wait)
    rc, out = sess.odr_debug("version")
    if not sess.dry and "MD-380" not in out:
        raise MigrateError("the radio does not report board=MD-380 over its debug link (see above)")
    rc, out = sess.odr_debug("storage", "info")
    region = re.search(r"region\s+(\w+)", out or "")
    region = region.group(1) if region else ("foreign" if sess.dry else "?")
    if region == "none":
        raise MigrateError("ODR found no usable SPI flash: this radio has the small (1 MB) flash part, or the flash "
                           "was not detected. ODR runs on this radio but keeps nothing (no codeplug, no settings), "
                           "so there is nothing to restore. Your backup is in %s" % sess.workdir)
    if region == "foreign":
        sess.say("   The flash area ODR keeps its settings in still holds the stock firmware's data. ODR doesn't")
        sess.say("   overwrite it on its own. Your stock codeplug is already backed up; this saves the area again")
        sess.say("   (raw) into the work directory and then gives it to ODR.")
        if not sess.ask("Give the settings area to ODR now?"):
            raise MigrateError("stopped: codeplug writes are refused until the area is ODR's")
        sess.odr_debug("storage", "claim", "--out", sess.path("storage-before-claim"))
    elif region != "ours":
        raise MigrateError("unexpected storage state %r" % region)
    sess.say("   the settings area is ODR's")
    return True


def step_restore(sess, args):
    sess.step("4. Write the codeplug to the radio")
    check_dmrconf(sess)
    src = args.yaml or sess.state.get("odr_yaml")
    if not src:
        raise MigrateError("no converted codeplug: run 'backup' first or give --yaml FILE")
    if not sess.dry and not os.path.isfile(src):
        raise MigrateError("%s not found" % src)
    expected = sess.state.get("expected")
    if args.yaml or expected is None:
        if os.path.isfile(src):
            with open(src, encoding="utf-8") as f:
                expected = codeplug_summary(f.read())["counts"]
    # --init-codeplug: start from qdmr's defaults instead of decoding what is in the radio's flash first, which after
    # a stock firmware is the stock codeplug's bytes, not an OpenUV380 image
    sess.dmr("write", "--radio", ODR_DRIVER, "--init-codeplug", src, radio=True)
    sess.say("   written; the radio restarts")
    time.sleep(0 if sess.dry else 3)
    wait_odr(sess, args.wait)
    back = sess.path("readback-odr.yaml")
    sess.dmr("read", "--radio", ODR_DRIVER, back, radio=True)
    if not sess.dry:
        need_file(sess, back, "the read-back codeplug")
        with open(back, encoding="utf-8") as f:
            got = codeplug_summary(f.read())
        print_summary(sess, "read back", got)
        diff = compare_counts(expected or {}, got["counts"])
        if diff:
            for k, want, have in diff:
                sess.say("   ! %s: wrote %d, read back %d" % (SECTION_NAMES[k], want, have))
            raise MigrateError("the codeplug read back differs from what was written; the files are in %s"
                               % sess.workdir)
        sess.say("   read back matches")
    return True


def step_iddb(sess, args):
    sess.step("5. Write the DMR ID (callsign) database")
    if args.no_db:
        sess.say("   skipped (--no-db)")
        return True
    ids = [args.id] if args.id else (sess.state.get("radio_ids") or [])
    if not ids:
        raise MigrateError("no DMR ID to choose the database around: give --id YOUR_ID, or --no-db")
    cmd = ["write-db", "--radio", ODR_DRIVER, "--id", ",".join(str(i)[:3] if args.id_prefix else str(i) for i in ids)]
    users = args.users_json or sess.state.get("users_json")
    if users:
        cmd += ["--database", users]
        sess.say("   from your users file (%s records)" % sess.state.get("users_count", "?"))
    else:
        sess.say("   dmrconf downloads the public DMR user database (internet needed)")
    if args.db_limit:
        cmd += ["--limit", str(args.db_limit)]
    sess.say("   a large database takes several minutes over USB; don't unplug the radio")
    wait_odr(sess, args.wait)
    sess.dmr(*cmd, radio=True)
    sess.say("   done: names show on the receive screen for IDs that are not contacts")
    return True


# ---------------------------------------------------------------------------------------------------------------
# package for a tester

PACKAGE_README = """\
OpenDigitalRadio for the TYT MD-380 / MD-390 / Retevis RT3: migration kit (TESTING ONLY)

This kit moves a radio from its stock or md380tools firmware to OpenDigitalRadio and keeps your channels, zones,
contacts, RX group lists and the DMR ID database. The firmware has not been proven on every radio yet: keep the
TYT updater and your stock firmware file at hand to go back.

Not for redistribution: the merged firmware image contains the codec from TYT's firmware.
The image is built for one band: odr_md380_codec.bin from a UHF build for a UHF radio, from a VHF build
(MD380_BAND=vhf) for a VHF radio. The band is on the label under the battery.

Install once
  - Python 3.8 or newer.
  - qdmr 0.12 or newer, which brings the dmrconf command (macOS: brew install qdmr; Linux: the qdmr package of
    your distribution; Windows: the qdmr installer from https://dm3mat.darc.de/qdmr/).
  - pip install -r requirements.txt   (pyusb and pyserial)
  - libusb: macOS brew install libusb; Linux apt install libusb-1.0-0 (and a udev rule for 0483:df11, which
    the tool prints when access is denied; add yourself to the dialout group for the serial port).
  - Windows (untested): bind "STM32 BOOTLOADER" / 0483:DF11 to WinUSB with Zadig for flashing. That binding also
    replaces the driver the TYT CPS uses; run the backup step first.

Run
  python3 md380_migrate.py all --image {image}{keyarg} --dry-run     (see every step, touches nothing)
  python3 md380_migrate.py all --image {image}{keyarg}

The tool asks before each step that needs you at the radio (switching on, the bootloader keys). It keeps every file
in ~/ODR-MD380-migration/<date-time>/ and never deletes them. If a step fails it stops and says how to go on;
'restore --workdir DIR' continues after the flash.
"""


def cmd_package(sess, args):
    out = os.path.abspath(args.outdir)
    files = {os.path.abspath(__file__): "md380_migrate.py",
             os.path.join(HERE, "stm32_dfu.py"): "stm32_dfu.py",
             os.path.join(HERE, "odr_debug.py"): "odr_debug.py"}
    sfw = next((p for p in (os.path.join(HERE, "stockfw.py"), os.path.join(HERE, "odrflash", "odrflash", "stockfw.py"))
                if os.path.isfile(p)), None)
    if sfw:
        files[sfw] = "stockfw.py"
    for img in args.image:
        files[os.path.abspath(img)] = os.path.basename(img)
    for extra in args.include or []:
        files[os.path.abspath(extra)] = os.path.basename(extra)
    for src in files:
        if not os.path.isfile(src):
            raise MigrateError("%s not found" % src)
    sess.say("package %s:" % out)
    for src, dst in files.items():
        sess.say("   %s <- %s" % (dst, src))
    if sess.dry:
        return True
    os.makedirs(out, exist_ok=True)
    for src, dst in files.items():
        shutil.copyfile(src, os.path.join(out, dst))
    keyarg = ' --stock "MD-380-D2.32(AD).bin"'
    if any(os.path.basename(x).endswith(".key") for x in args.include or []):
        keyarg = " --key " + next(os.path.basename(x) for x in args.include if x.endswith(".key"))
    with open(os.path.join(out, "README.txt"), "w", encoding="utf-8") as f:
        f.write(PACKAGE_README.format(image=os.path.basename(args.image[0]), keyarg=keyarg))
    with open(os.path.join(out, "requirements.txt"), "w", encoding="utf-8") as f:
        f.write("pyusb>=1.2\npyserial>=3.4\n")
    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8") as f:
        for dst in sorted(set(files.values())):
            with open(os.path.join(out, dst), "rb") as g:
                f.write("%s  %s\n" % (sha256(g.read()), dst))
    sess.say("   done: hand the directory over as it is (README.txt says what to install)")
    return True


# ---------------------------------------------------------------------------------------------------------------
# self-test

def self_test():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL: " + what)

    raw = bytes(range(256)) * (CODEPLUG_SIZE // 256)
    rdt = raw_to_rdt(raw)
    check(len(rdt) == 262709 and rdt[RDT_HEADER:RDT_HEADER + CODEPLUG_SIZE] == raw, "raw image wrapped as .rdt")
    check(classify_codeplug(rdt, "x.rdt") == "rdt", "classify rdt")
    check(classify_codeplug(raw, "x.img") == "raw", "classify raw")
    check(classify_codeplug(b"DfuSe\x01" + bytes(300), "x.dfu") == "dfu", "classify dfu")
    check(classify_codeplug(b"version: 0.15.1\n", "x.yaml") == "yaml", "classify yaml")
    for bad in (bytes(1000), bytes(0x1000000)):
        try:
            classify_codeplug(bad, "bad.bin")
            check(False, "classify rejects %d bytes" % len(bad))
        except MigrateError:
            pass

    y = """version: 0.15.1
settings:
  defaultID: id1
  boot:
    message1: ODR
radioIDs:
  - dmr: {id: id1, name: N0CALL, number: 3100001}
contacts:
  - dmr: {id: cont1, name: Local, ring: false, type: GroupCall, number: 9}
  - dmr: {id: cont2, name: "Alice, A", ring: false, type: PrivateCall, number: 3100002}
groupLists:
  - id: grp1
    name: Local
    contacts: [cont1]
channels:
  - dmr:
      id: ch1
      name: Simplex DMR
      rxFrequency: 441 MHz
  - fm:
      id: ch2
      name: Simplex FM
zones:
""" + "".join("  - id: zone%d\n    name: Z%d\n    A: [ch1, ch2]\n" % (i, i) for i in range(1, 71)) + \
        "scanLists: []\npositioning: []\n"
    s = codeplug_summary(y)
    check(s["counts"] == {"channels": 2, "zones": 70, "contacts": 2, "groupLists": 1}, "section counts %r" % s["counts"])
    check(s["names"]["channels"] == ["Simplex DMR", "Simplex FM"], "channel names")
    check(s["names"]["contacts"][1] == "Alice, A", "quoted name with a comma")
    check(s["radio_ids"] == [3100001], "radio ID")
    rep = limit_report(s)
    check(len(rep) == 1 and rep[0][:3] == ("zones", 70, 68) and rep[0][3] == ["Z69", "Z70"], "zone limit report")
    check(yaml_sections(y).get("scanLists") == [], "empty flow list")
    exp = {"channels": 2, "zones": 70, "contacts": 2, "groupLists": 1}
    check(compare_counts(exp, {"channels": 2, "zones": 68, "contacts": 2, "groupLists": 1}) == [],
          "read back after the zone limit matches")
    check(compare_counts(exp, {"channels": 1, "zones": 68, "contacts": 2, "groupLists": 1}) == [("channels", 2, 1)],
          "a lost channel is found")

    radioid = ("RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY,REMARKS\n"
               "3100002,k1abc,Bob,Jones,Austin,Texas,United States,\n"
               "3100001,N0CALL,Alice,Smith,Denver,Colorado,United States,x\n"
               "3100001,DUPE,Dup,,,,,\n"
               "notanid,X,,,,,,\n")
    u = parse_users_csv(radioid)
    check([x["id"] for x in u] == [3100001, 3100002], "radioid.net csv: sorted, deduplicated")
    check(u[1] == {"id": 3100002, "callsign": "K1ABC", "fname": "Bob", "surname": "Jones", "city": "Austin",
                   "state": "Texas", "country": "United States"}, "radioid.net csv fields %r" % (u[1],))
    plain = "123456\n3100003,W1AW,Hiram,Maxim,Newington,CT,United States\n\n7,X\n"
    u = parse_users_csv(plain)
    check(len(u) == 2 and u[1]["callsign"] == "W1AW" and u[1]["city"] == "Newington", "headerless, size line skipped")
    u = parse_users_csv("3100004,Jane,KA1XYZ\n", ["id", "fname", "callsign"])
    check(u == [{"id": 3100004, "callsign": "KA1XYZ", "fname": "Jane"}], "custom columns")
    j = json.loads(users_json(u))
    check(j == {"users": [{"id": 3100004, "callsign": "KA1XYZ", "fname": "Jane"}]}, "users json")

    img = struct.pack("<II", 0x20020000, APP_BASE + 0x1001) + bytes(0x100)
    check(classify_image(img) == ("plain", False), "plain image without codec")
    merged = bytearray(b"\xFF" * (CODEC_SIG_ADDR - APP_BASE + 0x100))
    merged[0:8] = img[:8]
    struct.pack_into("<I", merged, CODEC_SIG_ADDR - APP_BASE, CODEC_SIG)
    check(classify_image(bytes(merged)) == ("plain", True), "merged image")
    key = bytes((i * 37 + 11) & 0xFF for i in range(1024))
    payload = bytes((i * 7) & 0xFF for i in range(5000))
    enc = xor_key(payload, key)
    cont = bytearray(CONTAINER_MAGIC + bytes(CONTAINER_HEADER - len(CONTAINER_MAGIC)))
    struct.pack_into("<I", cont, 0x7C, 0xFFFFFFFF)
    struct.pack_into("<II", cont, 0x80, APP_BASE, len(enc))
    cont = bytes(cont) + enc + bytes(0x100)
    kind, segs = classify_image(cont)
    check(kind == "container" and segs == [(APP_BASE, len(enc))], "single-segment updater file")
    stream = container_stream(cont, segs)
    check(stream == enc and xor_key(stream, key) == payload, "updater payload is the bootloader stream")
    try:
        container_stream(cont, [(0x00060000, 16)])
        check(False, "a file not at 0x0800C000 is refused")
    except MigrateError:
        pass

    print("self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                          "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


# ---------------------------------------------------------------------------------------------------------------

EPILOG = """\
examples:
  %(prog)s all --image odr_md380_codec.bin --stock "MD-380-D2.32(AD).bin" --dry-run
  %(prog)s all --image odr_md380_codec.bin --key md380.key --users user.csv
  %(prog)s backup --from KD4Z-codeplug.rdt
  %(prog)s flash --image odr_md380.bin --stock "MD-380-D2.32(AD).bin"      (merges the codec)
  %(prog)s flash --image "MD-380-D2.32(AD).bin"                            (back to stock: a TYT updater file)
  %(prog)s restore --workdir ~/ODR-MD380-migration/20261011-101500

The steps of 'all': backup (radio on stock firmware) -> flash (bootloader) -> settings area -> codeplug -> ID
database. Each asks before anything happens at the radio; it stops at the first failure and never deletes a file.
"""


def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--dry-run", action="store_true", help="run the offline steps; print what would touch the radio")
    common.add_argument("--yes", "-y", action="store_true", help="don't ask (physical steps are still announced)")
    common.add_argument("--workdir", help="where backups and converted files go (default ~/ODR-MD380-migration/DATE)")
    common.add_argument("--port", help="the radio's serial port once it runs ODR (found by USB ID otherwise)")
    common.add_argument("--dmrconf", help="path of dmrconf (default: from PATH)")
    common.add_argument("--wait", type=int, default=120, help="seconds to wait for the radio (default 120)")

    u = argparse.ArgumentParser(add_help=False)
    u.add_argument("--users", metavar="CSV", help="DMR users file (radioid.net user.csv, md380tools / KD4Z users "
                                                 "file) for the ID database; default: dmrconf downloads it")
    u.add_argument("--users-columns", help="column order of a users file without a header, e.g. "
                                           "id,callsign,fname,surname,city,state,country")

    b = argparse.ArgumentParser(add_help=False)
    b.add_argument("--from", dest="source", metavar="FILE",
                   help="convert this codeplug file instead of reading the radio: TYT CPS / KD4Z toolkit .rdt, a raw "
                        "256 KB md380tools image, a dmrconf .dfu, or qdmr YAML")
    b.add_argument("--stock-radio", default=STOCK_DRIVER, choices=("md390", "rt8"),
                   help="dmrconf driver for the stock codeplug (default md390; the MD-380 and RT3 use the same format)")
    b.add_argument("--accept-trim", action="store_true", help="continue without asking when zones or RX group lists "
                                                              "don't all fit ODR")

    f = argparse.ArgumentParser(add_help=False)
    f.add_argument("--image", required=True, help="the ODR image (odr_md380_codec.bin, or odr_md380.bin with --stock), "
                                                  "or a TYT updater file")
    f.add_argument("--key", help="the 1024-byte MD-380 bootloader key file (checked by SHA-256)")
    f.add_argument("--stock", metavar="FILE", help="TYT's \"MD-380-D2.32(AD).bin\": gives the key and, for an image "
                                                   "without it, the codec")
    f.add_argument("--force", action="store_true", help="flash although the image has warnings")

    r = argparse.ArgumentParser(add_help=False)
    r.add_argument("--yaml", help="write this qdmr YAML instead of the converted backup")
    r.add_argument("--id", type=int, help="your DMR ID, for choosing the ID database (default: from the codeplug)")
    r.add_argument("--id-prefix", action="store_true", help="choose the database by the ID's 3-digit prefix "
                                                            "(your country) instead of nearest IDs")
    r.add_argument("--users-json", help="a user database in dmrconf's JSON form (default: from backup --users)")
    r.add_argument("--db-limit", type=int, help="at most this many database records")
    r.add_argument("--no-db", action="store_true", help="don't write the ID database")

    ap = argparse.ArgumentParser(description="Move a TYT MD-380 / MD-390 / Retevis RT3 from stock or md380tools "
                                             "firmware to OpenDigitalRadio, keeping the codeplug and the ID database.",
                                 epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="test the conversion logic (no radio, no dmrconf)")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("backup", parents=[common, b, u], help="read (or import) the codeplug and convert it for ODR")
    sub.add_parser("flash", parents=[common, f], help="flash ODR through the TYT bootloader")
    sub.add_parser("restore", parents=[common, r, u], help="settings area, codeplug and ID database to the radio on ODR")
    sub.add_parser("all", parents=[common, b, f, r, u], help="backup, flash, restore")
    p = sub.add_parser("package", parents=[common], help="a directory for a tester")
    p.add_argument("outdir")
    p.add_argument("--image", action="append", required=True, help="image(s) to include")
    p.add_argument("--include", action="append", help="further files (a key file, the stock firmware file)")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.cmd:
        ap.print_help()
        return 2
    sess = Session(args)
    try:
        if args.cmd == "package":
            return 0 if cmd_package(sess, args) else 1
        if args.cmd == "restore":
            sess.open_workdir(args.workdir, create=args.workdir is None or args.yaml is not None)
            if not args.yaml and not sess.state.get("odr_yaml"):
                raise MigrateError("no converted codeplug in %s: give --workdir of an earlier backup, or --yaml"
                                   % sess.workdir)
            prepare_users(sess, args)
            step_prepare_odr(sess, args)
            step_restore(sess, args)
            step_iddb(sess, args)
        else:
            sess.open_workdir(args.workdir)
            if args.cmd == "all":
                preflight(sess, args)
            if args.cmd in ("backup", "all"):
                step_backup(sess, args)
            if args.cmd in ("flash", "all"):
                if args.cmd == "all":
                    if not sess.ask("The codeplug is backed up. Flash ODR now?"):
                        raise MigrateError("stopped before flashing; the backup is in %s" % sess.workdir)
                step_flash(sess, args)
            if args.cmd == "all":
                step_prepare_odr(sess, args)
                step_restore(sess, args)
                step_iddb(sess, args)
        sess.say("")
        sess.say("finished%s. Files: %s" % (" (dry run: nothing was written to the radio)" if sess.dry else "",
                                            sess.workdir))
        return 0
    except MigrateError as e:
        sess.say("")
        sess.say("stopped: %s" % e)
        if sess.state.get("flashed") and sess.state.get("odr_yaml") and args.cmd in ("all", "restore"):
            sess.say("to continue once the cause is fixed: %s restore --workdir \"%s\""
                     % (os.path.basename(sys.argv[0]), sess.workdir))
        return 1
    except KeyboardInterrupt:
        sess.say("")
        sess.say("stopped by you. Files: %s" % (sess.workdir or "-"))
        return 1


if __name__ == "__main__":
    sys.exit(main())
