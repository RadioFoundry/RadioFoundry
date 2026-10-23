# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""odrflash command line (tools/odrflash/README.md)."""
import argparse
import json
import os
import random
import re
import sys
import threading
import time
import urllib.request
import uuid

from . import __version__, boards, bootkey, codecsave, debuglink, manifest as mf, plan as planmod, state, stockfw
from . import dfu as dfumod
from . import release as relmod
from . import serialflash
from . import k22fcodec, hidboot, stubroute, updatefile

EPILOG = """\
examples:
  odrflash flash --release latest                       download the newest release and flash it
  odrflash flash odr-dm1701-v0.1.0.bin                  a downloaded image (its .json manifest next to it)
  odrflash flash --release v0.1.0 --dry-run             print exactly which sectors would be erased and written
  odrflash flash --release latest --stock "DM-1701(CSV)-V02.06.bin"
                                                        also write the codec from your stock firmware file
  odrflash probe                                        ask the radio (running ODR) whether its codec can be kept
  odrflash key --stock "DM-1701(CSV)-V02.06.bin" --save derive the bootloader key from the stock file, keep it
  odrflash key --save                                   read it from the radio instead (running ODR)
  odrflash flash odr-dm1801pro-v0.1.0.bin --port /dev/ttyUSB0
                                                        a DM-1801 Pro over its programming cable (the stub when it
                                                        runs ODR with stub entry, else Baofeng's update mode)

entering the bootloader (DM-1701): turn the radio off, hold PTT and SK1 (the side key just above PTT), turn it on.
The screen stays dark.
"""

STOCK_HINT = ("the DM-1701 stock firmware V02.06 CSV file, DM-1701(CSV)-V02.06.bin (sha256 0314efd4...): an archive "
              "is at github.com/daniel5600/BAOFENG-DM1701-Original-Firmware")


class UsageError(Exception):
    pass


# minimal non-invasive reporting for helping the ODR project track programming failures; opt out with
# --no-reporting or env REPORTING=0
FLASH_LOG_URL = "https://odr.spencerfowler.com/v1/flashes"


def _send_in_background(method, url, body):
    """Send in the background; never delays or fails a flash."""
    def go():
        try:
            req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"},
                                         method=method)
            urllib.request.urlopen(req, timeout=5, context=relmod._ssl_context()).close()
        except Exception:
            pass
    t = threading.Thread(target=go, daemon=True)
    t.start()
    return t


def flash_started(args, board, man):
    if args.no_reporting or os.environ.get("REPORTING") == "0":
        return None
    fid = str(uuid.uuid4())
    sent = _send_in_background("POST", FLASH_LOG_URL, {
        "id": fid, "tool": "odrflash", "tool_version": __version__, "task": "install",
        "source": "release" if args.release else "own", "radio": board.id,
        "firmware": str(man.get("version", "")), "variant": str(man.get("variant", "")),
        "features": str(man.get("features", ""))})
    return fid, sent


def flash_finished(started, error=None):
    if not started:
        return
    fid, sent = started
    sent.join(5)	# the start must arrive first
    body = {"result": "success"}
    if error is not None:
        # file paths can hold user names
        msg = re.sub(r"(?:[A-Za-z]:)?[\\/][^\s'\"]+", "<path>", str(error))
        body = {"result": "fail", "reason": ("%s: %s" % (type(error).__name__, msg))[:300]}
    _send_in_background("PATCH", "%s/%s" % (FLASH_LOG_URL, fid), body).join(5)


def say(msg=""):
    print(msg, flush=True)


def warn(msg):
    print("warning: " + msg, file=sys.stderr, flush=True)


def ask_yes(question, default=False):
    if not sys.stdin.isatty():
        return default
    try:
        a = input(question + (" [Y/n] " if default else " [y/N] ")).strip().lower()
    except EOFError:
        return default
    return default if not a else a in ("y", "yes")


def pick_board(board_id, what, candidates=None):
    """The board for a command when nothing it was given names one: --board, else the only candidate, else ask on a
    terminal. There is no default radio: flashing the wrong model's image is the one mistake odrflash must not make."""
    if board_id:
        return boards.get(board_id)
    cands = boards.by() if candidates is None else candidates
    if len(cands) == 1:
        return cands[0]
    ids = ", ".join(b.id for b in cands)
    if not sys.stdin.isatty():
        raise UsageError("%s: which radio? Give --board (%s)" % (what, ids))
    for i, b in enumerate(cands, 1):
        say("  %d  %s (%s)" % (i, b.name, b.id))
    try:
        a = input("%s: which radio? [1-%d] " % (what, len(cands))).strip()
    except EOFError:
        a = ""
    if a.isdigit() and 1 <= int(a) <= len(cands):
        return cands[int(a) - 1]
    try:
        return boards.get(a)
    except KeyError:
        raise UsageError("%s: no radio chosen. Give --board (%s)" % (what, ids))


def parse_options(board, given):
    """--option NAME=VALUE values, checked against the board's build options."""
    out = {}
    for opt in given or []:
        name, _, value = opt.partition("=")
        if board.build_option(name, value) is None:
            raise UsageError("%s has no build option %s=%s (its options: %s)" % (board.name, name, value, ", ".join(
                "%s=%s" % (o["name"], "|".join(v["id"] for v in o["values"])) for o in board.build_options) or "none"))
        out[name] = value
    return out


def resolve_options(args, board, probe, need_all=False):
    """The hardware build options for this radio (the MD-380's band): --option, and the board name a running radio
    reports, which must agree. need_all (choosing a release image): every option must be known, from those two or
    asked on a terminal; a wrong band's image would refuse the radio's own frequencies."""
    opts = parse_options(board, getattr(args, "option", None))
    named = board.options_for_name((probe or {}).get("board") or "") or {}
    for k, v in named.items():
        if opts.get(k, v) != v:
            raise UsageError("the radio reports %s (%s=%s), but --option says %s=%s: refusing"
                             % (probe.get("board"), k, v, k, opts[k]))
        opts[k] = v
    if need_all:
        for o in board.build_options:
            if o["name"] in opts:
                continue
            ids = [v["id"] for v in o["values"]]
            if not sys.stdin.isatty():
                raise UsageError("which %s %s? Give --option %s=%s (%s)" % (board.name, o["title"].lower(), o["name"],
                                                                           "|".join(ids), o["help"]))
            say("%s: %s" % (o["title"], o["help"]))
            for i, v in enumerate(o["values"], 1):
                say("  %d  %s (%s)" % (i, v["title"], v["text"]))
            try:
                a = input("which one? [1-%d] " % len(ids)).strip()
            except EOFError:
                a = ""
            if not (a.isdigit() and 1 <= int(a) <= len(ids)):
                raise UsageError("no %s chosen: give --option %s=%s" % (o["title"].lower(), o["name"], "|".join(ids)))
            opts[o["name"]] = ids[int(a) - 1]
    return opts


def check_options(board, man, opts):
    """Refuse an image built for other hardware than the radio's (its manifest's build_options against opts)."""
    if not board.build_options or not opts:
        return
    have = board.full_options((man or {}).get("build_options"))
    for k, v in opts.items():
        if have.get(k) != v:
            raise UsageError("the image is the %s=%s build, the radio needs %s=%s: refusing" % (k, have.get(k), k, v))


def stock_board(stock, board_id):
    """The board a stock file is for (its codec profile), checked against --board when both are given."""
    try:
        prof = stockfw.find_profile(stock)
    except stockfw.StockError:
        return pick_board(board_id, "an unknown stock file")
    board = boards.get(prof["board"])
    if board_id and boards.get(board_id).id != board.id:
        raise UsageError("the stock file is %s firmware, but --board says %s: refusing"
                         % (board.name, boards.get(board_id).name))
    return board


# ---------------------------------------------------------------------------------------------------------------
# inputs: image + manifest, key, stock file

def load_image(args, board_hint):
    """Returns (board, manifest, application image bytes, merged): merged is (stock profile, codec segments) for an
    image with the codec merged in (odrflash merge, or a build with the stock file in codecs/), else None."""
    if args.release:
        if args.image:
            raise UsageError("give an image file or --release, not both")
        board = pick_board(board_hint, "--release")
        probe = get_probe(args, log=lambda *a: None) if board.build_options else None
        opts = resolve_options(args, board, probe, need_all=True)
        img_path, man_path = relmod.fetch(board.image_id(opts), args.release, args.repo, log=say)
    elif args.image:
        img_path = args.image
        man_path = args.manifest or os.path.splitext(img_path)[0] + ".json"
    else:
        raise UsageError("which firmware? Give an image file (with its .json manifest next to it) or --release "
                         "VERSION (or --release latest)")
    with open(os.path.expanduser(img_path), "rb") as f:
        image = f.read()
    if image.startswith(stockfw.MAGIC):
        raise UsageError("%s is an official Baofeng/TYT firmware file, not an OpenDigitalRadio image. Use it with "
                         "--stock to provide the codec" % img_path)
    if os.path.exists(man_path):
        m = mf.load(man_path)
        board = mf.verify(m, image, board_hint)
        say("image    %s: %s %s (git %s), %d bytes, SHA-256 verified against %s"
            % (os.path.basename(img_path), board.name, m.get("version"), m.get("git"), len(image),
               os.path.basename(man_path)))
        return board, m, image, None
    hint = boards.get(board_hint) if board_hint else None
    if hint is not None and hint.family == "serial-loader":
        # a local HR_C7000 build: odr_<board>.bin, or the build's _codec.bin with the codec slot merged in
        app, slot = serialflash.split_slot(hint, image)
        m = mf.for_dev_build(hint, app)
        mf.verify(m, app, hint.id)
        say("image    %s: a local %s build, application %d bytes%s" % (
            os.path.basename(img_path), hint.name, len(app), ", codec slot merged in (%d bytes)" % len(slot)
            if slot else ""))
        return hint, m, app, slot
    try:
        merged = stockfw.split_merged(image)
    except stockfw.StockError as e:
        raise UsageError("%s: %s" % (img_path, e))
    if merged:
        app, prof, codec = merged
        board = boards.get(prof["board"])
        if board_hint and boards.get(board_hint).id != board.id:
            raise UsageError("%s holds the %s codec, but --board says %s: refusing"
                             % (img_path, board.name, boards.get(board_hint).name))
        m = mf.for_dev_build(board, app)
        mf.verify(m, app, board.id)
        say("image    %s: a %s image with the codec merged in (%s, every window verified by SHA-256), "
            "application %d bytes" % (os.path.basename(img_path), board.name, prof["codec_profile"], len(app)))
        return board, m, app, (prof, codec)
    if not board_hint:
        raise UsageError("no manifest %s next to the image. Release images come with one; for a local build say "
                         "which radio it is for with --board (%s)" % (man_path, ", ".join(sorted(boards.BOARDS))))
    board = boards.get(board_hint)
    m = mf.for_dev_build(board, image)
    mf.verify(m, image, board.id)
    warn("no manifest: treating %s as a local %s build (nothing to verify it against)" % (img_path, board.name))
    return board, m, image, None


def load_stock(args):
    if not args.stock:
        return None
    if args.simulate and args.stock == "synthetic":
        return "synthetic"
    with open(os.path.expanduser(args.stock), "rb") as f:
        return f.read()


def get_key(args, board, stock, sim_key=None):
    """The bootloader key: --key FILE, else derived from the stock file, else the saved key, else read from the radio
    if it runs ODR."""
    if sim_key is not None:
        return sim_key
    if args.key:
        return dfumod.load_key(args.key, board)
    if isinstance(stock, bytes):
        say("key      deriving the %s bootloader key from the stock file" % board.name)
        try:
            return dfumod.check_key(stockfw.derive_key(stock, board.key_sha256), board)
        except stockfw.StockError as e:
            raise UsageError(str(e))
    saved = state.key_path(board.id)
    if os.path.exists(saved):
        say("key      %s" % saved)
        return dfumod.load_key(saved, board)
    if not args.simulate:
        key = bootkey.from_radio(board, args.port)
        if key:
            say("key      read from the radio's bootloader (ODR, debug link)")
            return dfumod.check_key(key, board)
    raise UsageError("the %s bootloader needs its 1024-byte key. Start the radio with ODR and connect it (odrflash reads "
                     "the key from it), or give --stock with %s (odrflash derives the key from it), or --key FILE, or "
                     "save it once with: odrflash key --stock FILE --save"
                     % (board.name, STOCK_HINT))


def get_saved_codec(args, board, probe):
    """The codec read back from the radio, or the copy saved from it earlier (codecsave.py). A radio running ODR with
    this codec hands over a copy: before every update where an update erases it, once on the other boards. Else the
    copy an earlier run saved (an update that stopped halfway, a radio back on stock firmware or in its bootloader)."""
    saved = codecsave.load(board)
    has_codec = bool(probe) and probe.get("codec") == board.codec.id
    if has_codec and not args.no_probe and (not board.keep_codec_supported or saved is None):
        got = codecsave.from_radio(board, args.port)
        if got:
            say("codec    read back from the radio (debug link) and checked; saved in %s" % codecsave.save(board, got[1]))
            return got
    if saved and not (has_codec and board.keep_codec_supported):
        say("codec    the copy saved from this radio earlier, checked: %s" % codecsave.path(board))
    return saved


# ---------------------------------------------------------------------------------------------------------------
# the codec decision

class Decision:
    def __init__(self, mode, why, risk=None, expect_codec=True, source="stock"):
        self.mode = mode
        self.why = why
        self.risk = risk
        self.expect_codec = expect_codec
        self.source = source		# a merge's codec: "stock" (the stock file) or "saved" (read back from the radio)


def decide_codec(board, manifest, requested, have_stock, probe, stock_installed, ask=None, have_saved=False):
    """Pick keep or merge (docs/dev/release.md "Codec handling"). probe: a debug-link probe of the radio (this run or
    remembered), or None. stock_installed: what the user says the radio runs ("V02.06 CSV"), or None. ask: a yes/no
    callable for the interactive question, or None (non-interactive). have_saved: the codec read back from this radio
    (codecsave.py), used where an update can't keep it or the radio's codec can't be checked, before the stock file.
    Raises UsageError when neither is safe."""
    profile = (manifest.get("codec") or {}).get("profile") or board.codec.id
    merge_hint = "flash with --stock FILE to write the codec from your stock firmware (%s)" % STOCK_HINT
    if requested == planmod.MERGE:
        if not have_stock:
            raise UsageError("--codec merge needs --stock: " + STOCK_HINT)
        return Decision(planmod.MERGE, "asked for: the codec is written from your stock file")
    if not board.keep_codec_supported:
        if have_saved:
            return Decision(planmod.MERGE, "on the %s, ODR's firmware shares a flash sector with the codec, so every "
                            "update erases part of it; the copy read back from the radio is written again"
                            % board.name, source="saved")
        if have_stock:
            return Decision(planmod.MERGE, "on the %s, ODR's firmware shares a flash sector with the codec, so every "
                            "update erases part of it; it is written again from your stock file" % board.name)
        raise UsageError("on the %s, ODR's firmware shares a flash sector with the codec, so every update erases part "
                         "of it; start the radio with ODR and connect it (odrflash reads the codec back and saves a "
                         "copy), or %s" % (board.name, merge_hint))
    if probe:
        pb = boards.from_debuglink_name(probe.get("board") or "")
        if pb is None or pb.id != board.id:
            raise UsageError("the radio reported board %r over its debug link, but this image is for the %s: refusing"
                             % (probe.get("board"), board.name))
        codec = probe.get("codec")
        if codec == profile:
            return Decision(planmod.KEEP, "ODR on the radio checked its codec (%s: where it is and every window's CRC) "
                            "and the codec sectors are left alone" % codec)
        if codec is None:
            what = "firmware too old to report its codec"
        else:
            what = "codec=%s" % codec
        if have_saved and requested != planmod.KEEP:
            return Decision(planmod.MERGE, "the radio has no codec to keep (%s), so the copy saved from it earlier is "
                            "written" % what, source="saved")
        if have_stock and requested != planmod.KEEP:
            return Decision(planmod.MERGE, "the radio has no codec to keep (%s), so it is written from your stock "
                            "file" % what)
        if requested == planmod.KEEP:
            return Decision(planmod.KEEP, "asked for, although the radio reported %s" % what,
                            risk="DMR voice will stay off (FM works) until you flash with --stock", expect_codec=False)
        raise UsageError("the radio reported %s: there is no matching codec in it to keep; %s" % (what, merge_hint))
    if have_saved and requested != planmod.KEEP:
        return Decision(planmod.MERGE, "the radio's codec can't be checked, so the copy saved from it earlier is "
                        "written", source="saved")
    if stock_installed:
        norm = stock_installed.upper().replace("-", " ").replace("_", " ")
        if any(norm == v.upper() for v in board.codec.installed):
            return Decision(planmod.KEEP, "you said the radio runs stock %s, which holds this codec at the right "
                            "addresses" % stock_installed,
                            risk="if the radio runs anything else, DMR voice stays off (FM works); the firmware checks "
                                 "the codec before it uses it, and flashing again with --stock fixes it")
        if have_stock:
            return Decision(planmod.MERGE, "stock %s keeps its codec at other addresses, so it is written from your "
                            "stock file" % stock_installed)
        if not board.codec.installed:
            raise UsageError("no stock firmware leaves the codec where ODR keeps it on the %s (stock %s holds its code "
                             "in flash ODR's application uses); %s" % (board.name, stock_installed, merge_hint))
        raise UsageError("stock %s keeps its codec at other addresses than %s; %s"
                         % (stock_installed, ", ".join(board.codec.installed), merge_hint))
    if have_stock and requested != planmod.KEEP:
        return Decision(planmod.MERGE, "the radio's codec is unknown, so it is written from your stock file")
    risk = ("if the radio doesn't hold that codec, DMR voice stays off (FM works) until you flash again with --stock; "
            "the firmware checks the codec before using it, so nothing worse happens")
    if requested == planmod.KEEP:
        return Decision(planmod.KEEP, "asked for; the radio's codec could not be checked", risk=risk)
    if ask is not None and board.codec.installed:
        say("")
        say("odrflash can keep the codec that is already in the radio instead of writing one, if the radio holds")
        say("the right one: it is running Baofeng's stock firmware %s, or ODR with working DMR voice."
            % " or ".join(board.codec.installed))
        say("(Start the radio normally with ODR and run 'odrflash probe' first to have it checked instead.)")
        if ask("Is the radio running stock %s (or ODR with DMR voice working)?" % " or ".join(board.codec.installed)):
            return Decision(planmod.KEEP, "you confirmed the radio holds the codec", risk=risk)
    if not board.codec.installed:	# no stock firmware holds it where ODR does: only ODR's own check can tell
        raise UsageError("odrflash can't tell whether the radio holds the codec where this firmware keeps it. Either "
                         "start the radio with ODR and run 'odrflash probe' first, or %s" % merge_hint)
    raise UsageError("odrflash can't tell whether the radio holds the codec. Either start the radio with ODR and run "
                     "'odrflash probe' first, or say --stock-installed \"%s\" if it runs that stock firmware, or %s"
                     % (board.codec.installed[0], merge_hint))


# ---------------------------------------------------------------------------------------------------------------
# radio discovery

def get_probe(args, log=say):
    """A probe of the running radio: now over the debug link, else a recent remembered one."""
    if args.no_probe:
        return None
    p = debuglink.probe(args.port)
    if p:
        state.remember_probe(p)
        log("radio    ODR running on %s: board %s, git %s, codec %s" % (p["port"], p["board"], p["git"], p["codec"]))
        return p
    p = state.recent_probe()
    if p:
        log("radio    remembered from %d min ago: board %s, git %s, codec %s (odrflash probe refreshes it)"
            % (p["age_s"] // 60, p.get("board"), p.get("git"), p.get("codec")))
    return p


def wait_for_dfu(usb, board, wait_s, log=say):
    devs = dfumod.find(usb, board)
    if not devs and wait_s > 0:
        log("")
        log("Put the radio in its bootloader now: turn it off, hold PTT and SK1 (the side key just above PTT), and")
        log("turn it on while holding both. The screen stays dark. Waiting up to %d s..." % wait_s)
        end = time.monotonic() + wait_s
        while not devs and time.monotonic() < end:
            time.sleep(0.5)
            devs = dfumod.find(usb, board)
    if not devs:
        raise UsageError("no radio bootloader found (USB %04X:%04X). Check that the cable carries data and that the "
                         "radio is in its bootloader (screen dark after power-on with PTT + SK1)%s" % (
                             board.dfu_ids + ("\n" + dfumod.WINDOWS_DRIVER_STEPS if sys.platform == "win32" else "",)))
    if len(devs) > 1:
        raise UsageError("%d bootloaders found; connect only one radio" % len(devs))
    return devs[0]


def check_bootloader(usb, dev, board, log=say):
    fp = dfumod.fingerprint(usb, dev)
    log("found    bootloader: %r, product %r, serial %r" % (fp["string1"], fp["product"], fp["serial"]))
    for i in fp["interfaces"]:
        log("         layout %r" % i)
    if board.dfu_string1 and board.dfu_string1.split()[0] not in (fp["string1"] or ""):
        raise UsageError("this is not the radio's own bootloader (string 1 is %r, expected %r). ST's ROM bootloader "
                         "would write the encrypted image as is: refusing" % (fp["string1"], board.dfu_string1))
    return fp


# ---------------------------------------------------------------------------------------------------------------
# commands

def cmd_flash(args):
    sim = args.simulate
    probe = None
    sim_key = None
    if sim:
        say("SIMULATION: a fake radio in state %r, nothing is sent to any device" % sim)
        from . import fakedfu
        sim_key = fakedfu.synthetic_key()
    board, man, image, merged = load_image(args, args.board)
    if board.family == "serial-loader":
        return cmd_flash_serial(args, board, man, image, merged)
    if board.family == "serial-iap":
        raise UsageError("%s is installed with the web flasher or stub/stubtool.py, not with odrflash" % board.name)
    if args.route == serialflash.STUB:
        return cmd_flash_stub(args, board, image, merged)
    if args.route != serialflash.AUTO:
        raise UsageError("--route is for the radios flashed over a programming cable (DM-1801 Pro, DM-32UV) and "
                         "--route stub (radios running ODR with stub entry)")
    stock = load_stock(args)
    if merged and stock is not None:
        say("stock    the image carries the codec, so the stock file only gives the bootloader key")

    # the codec: what the radio says, what the user says
    if sim:
        name = board.debuglink_names[0]
        probe = {"odr-codec": {"board": name, "git": "00000000", "codec": board.codec.id, "port": "sim"},
                 "odr-nocodec": {"board": name, "git": "00000000", "codec": "absent", "port": "sim"}}.get(sim)
        if probe:
            say("radio    (simulated) ODR running: codec %s" % probe["codec"])
    else:
        probe = get_probe(args)
    if probe and not args.board:
        pb = boards.from_debuglink_name(probe.get("board") or "")
        if pb and pb.id != board.id:
            raise UsageError("the radio is a %s, the image is for the %s: refusing" % (pb.name, board.name))
    check_options(board, man, resolve_options(args, board, probe))
    interactive = None if (args.yes or args.dry_run) else ask_yes
    saved = None
    if not merged and not sim:
        saved = get_saved_codec(args, board, probe)
    if merged and args.codec != planmod.KEEP:
        d = Decision(planmod.MERGE, "the image carries the codec (%s), so it is written from the image"
                     % merged[0]["codec_profile"])
    else:
        d = decide_codec(board, man, args.codec, stock is not None, probe, args.stock_installed, interactive,
                         have_saved=saved is not None)
    say("codec    %s: %s" % (d.mode.upper(), d.why))
    if d.risk:
        warn(d.risk)

    codec_segs = []
    codec_prof = None	# the stock profile whose layout the merge writes
    key = None
    if d.mode == planmod.MERGE and merged:
        codec_prof, codec_segs = merged
    elif d.mode == planmod.MERGE and d.source == "saved":
        codec_prof, codec_segs = saved[0], codecsave.segments(*saved)
    elif d.mode == planmod.MERGE:
        if stock == "synthetic":
            stock_bytes, profiles, _, _ = stockfw.build_synthetic_stock(sim_key, random.Random(7),
                                                                        stockfw.profile_for_board(board.id))
            prof, csegs = stockfw.extract(stock_bytes, sim_key, profiles)
        else:
            key = get_key(args, board, stock, sim_key)
            try:
                prof, csegs = stockfw.extract(stock, key)
            except stockfw.StockError as e:
                raise UsageError(str(e))
            if prof["codec_profile"] != board.codec.id:
                raise UsageError("the stock file holds codec %s, the image expects %s"
                                 % (prof["codec_profile"], board.codec.id))
            say("stock    %s verified (SHA-256), codec windows decrypted and checked" % prof["name"])
        codec_prof, codec_segs = prof, stockfw.codec_segments(prof, csegs)

    p = planmod.make_plan(board, mf.segments(man, image), d.mode, codec_segs,
                          board.merge_windows(codec_prof) if codec_segs else None)
    say("")
    say("plan:")
    for line in p.describe():
        say("  " + line)
    say("")
    if args.dry_run:
        say("dry run: nothing was erased or written")
        return 0

    if key is None:
        key = get_key(args, board, stock if isinstance(stock, bytes) else None, sim_key)
    if board.family != "usb-dfu":
        raise UsageError("flashing the %s isn't supported by odrflash yet" % board.name)

    if sim:
        flash = fakedfu.stock_flash(board, random.Random(3), [] if sim == "blank" else
                                    [(a, stockfw.random_bytes(random.Random(a), n))
                                     for a, n in board.codec.windows])
        dev = fakedfu.FakeUsbDevice(board, key, flash)
        usb = fakedfu.FakeUsb([dev])
        before = bytes(dev.flash)
    else:
        usb = dfumod.import_usb()
        dev = wait_for_dfu(usb, board, args.wait)
    check_bootloader(usb, dev, board)
    if not args.yes and not sim and not ask_yes("Erase and write %d sector(s) now?" % len(p.erase), default=True):
        say("nothing done")
        return 1
    intf = dfumod.open_device(usb, dev)
    started = None if sim else flash_started(args, board, man)

    tty = sys.stdout.isatty()

    def progress(done, total):
        pct = done * 100 // total
        if tty:
            sys.stdout.write("\rwriting  %d/%d blocks (%d%%)" % (done, total, pct))
            if done == total:
                sys.stdout.write("\n")
        elif done == total or pct // 25 != (done - 1) * 100 // total // 25:
            sys.stdout.write("writing  %d/%d blocks (%d%%)\n" % (done, total, pct))
        sys.stdout.flush()

    try:
        dfumod.Dfu(dev, intf, log=lambda m: say("         " + m), sleep=(lambda s: None) if sim else time.sleep) \
            .run(p, key, progress)
    except BaseException as e:
        flash_finished(started, e)
        if isinstance(e, dfumod.DfuError) or not isinstance(e, Exception):
            raise
        raise dfumod.DfuError("USB error while flashing: %s. The bootloader is never erased: put the radio in its "
                              "bootloader again (PTT + SK1 at power-on) and run the same command" % e)
    finally:
        dfumod.release(usb, dev)
    flash_finished(started)
    if not sim:
        state.forget_probe()
    say("done     the radio restarts into ODR %s" % man.get("version"))

    if sim:
        return report_simulation(board, p, dev, before, d)
    if args.no_verify:
        return 0
    return verify_after(args, board, man, d)


def _progress(unit):
    tty = sys.stdout.isatty()

    def progress(done, total):
        if not total:
            return
        pct = done * 100 // total
        if tty:
            sys.stdout.write("\rwriting  %d/%d %s (%d%%)" % (done, total, unit, pct))
            if done == total:
                sys.stdout.write("\n")
        elif done == total or pct // 25 != (done - 1) * 100 // total // 25:
            sys.stdout.write("writing  %d/%d %s (%d%%)\n" % (done, total, unit, pct))
        sys.stdout.flush()
    return progress


def gd77_update_file(args, board, image):
    """(update file bytes, donor, flat image, codec profile or None): the image with the codec merged in, written into
    a copy of the stock update file --stock (its header is the donor's)."""
    if not args.stock:
        raise UsageError("the %s update needs --stock FILE: one of the radio's own update files (.sgl). It supplies the "
                         "file header the radio's loader expects and, unless --codec keep, the AMBE codec" % board.name)
    with open(os.path.expanduser(args.stock), "rb") as f:
        donor = f.read()
    try:
        u = updatefile.UpdateFile(donor)
        flat = updatefile.unpack(u)
        spec = k22fcodec.find_spec(flat, board=board.id)
        if args.codec == planmod.KEEP:
            out, prof = image, None
        else:
            if spec is None:
                raise UsageError("%s is not one of the supported %s stock builds (%s), so its codec cannot be placed. "
                                 "Use --codec keep to flash without voice" % (u.label(), board.name, ", ".join(sorted(k22fcodec.load_specs(board=board.id)))))
            out, prof = k22fcodec.merge(image, k22fcodec.pack(flat, spec)), spec["profile"]
        if len(out) > u.room:
            raise UsageError("the image is %d bytes, the stock update file has room for %d" % (len(out), u.room))
        return updatefile.pack(out, u), u, out, prof
    except (updatefile.UpdateFileError, k22fcodec.PackError) as e:
        raise UsageError(str(e))


def cmd_flash_stub(args, board, image, merged):
    """The STM32F4 radios and the GD-77 through the flash stub (stubroute.py): ODR must be running with stub entry."""
    if board.id not in stubroute.BOARDS:
        raise UsageError("the %s has no flash stub; use the bootloader route" % board.name)
    if args.simulate:
        raise UsageError("--simulate does not cover the stub route")
    if board.family == "gd77-hid":
        if args.stock:
            _, _, image, prof = hid_update_file(args, board, image)
            say("codec    %s" % ("MERGED: %s placed at 0x%X" % (prof, k22fcodec.CODE_BASE) if prof else "none"))
        else:
            say("codec    the radio keeps the one it has (no --stock given)")
    else:
        if merged:
            warn("the codec in this image is ignored: the stub route keeps the radio's own codec")
        if args.codec == "merge" or (args.stock and args.codec == "auto"):
            warn("the stub route never writes a codec; --stock is ignored (use the bootloader route to write one)")
    say("route    stub: the radio must be ON and run ODR built with CONFIG_STUB_ENTRY=y")
    if args.dry_run:
        say("dry run: %d bytes would be written (changed sectors only)" % len(image))
        return 0
    if not args.yes and not ask_yes("Write the changed sectors now?", default=True):
        return 1
    try:
        rc = stubroute.run(board, image, args.port, log=say)
    except stubroute.StubRouteError as e:
        raise UsageError(str(e))
    return rc


def cmd_flash_hid(args, board, image):
    """A radio whose loader takes a stock update file over USB HID (hidboot.py): untested on a radio."""
    if args.route not in (serialflash.AUTO, serialflash.BOOTLOADER):
        raise UsageError("--route %s does not apply to the %s" % (args.route, board.name))
    new, donor, out, prof = gd77_update_file(args, board, image)
    say("stock    %s: header and codec source" % donor.label())
    say("codec    %s" % ("MERGED: %s placed at 0x%X" % (prof, k22fcodec.CODE_BASE) if prof else "none: the image has no voice"))
    say("image    %d bytes of flash from 0x%X written into an update file of %d bytes" % (len(out), updatefile.FLASH_BASE, len(new)))
    warn("not yet tried on a radio: if the loader refuses the file, nothing is erased before the names are checked")
    if args.dry_run:
        say("dry run: nothing was sent")
        return 0
    u2 = updatefile.UpdateFile(new)
    if args.simulate:
        link = hidboot.FakeBootloader(u2)
    else:
        say("radio    switch it off, hold both side keys, switch it on and connect the USB cable (update mode)")
        link = hidboot.HidLink()
    if not args.yes and not args.simulate and not ask_yes("Erase and write the radio's application now?", default=True):
        return 1
    last = [-1]

    def progress(done, total):
        pct = 100 * done // total
        if pct != last[0] and pct % 10 == 0:
            last[0] = pct
            say("write    %d%%" % pct)
    hidboot.Updater(link).flash(u2, progress=progress)
    if args.simulate and link.received() != u2.body[:u2.length]:
        raise UsageError("simulation: the model loader received something else than the file's body")
    say("done     the radio restarts by itself")
    return 0


def cmd_flash_serial(args, board, man, app, slot):
    """The HR_C7000 radios over the programming cable (serialflash.py): the stub when the radio runs ODR with stub
    entry, else Baofeng's update mode."""
    sf = serialflash
    if args.simulate:
        raise UsageError("--simulate runs the DM-1701 bootloader model only")
    if args.key or args.stock_installed:
        raise UsageError("the %s needs no bootloader key and no --stock-installed" % board.name)
    stock = load_stock(args)
    try:
        dev = sf.find_cable(args.port)
    except sf.SerialError as e:
        if not args.dry_run:
            raise
        dev = None
        say("cable    none found (%s)" % e)
    probe = None
    offered = False
    if dev and not args.no_probe and args.route != sf.BOOTLOADER:
        probe = sf.probe(dev)
        if probe:
            pb = boards.from_debuglink_name(probe.get("board") or "")
            if pb is None or pb.id != board.id:
                raise UsageError("the radio on %s reported board %r, the image is for the %s: refusing"
                                 % (dev, probe.get("board"), board.name))
            offered = sf.stub_offered(dev)
            say("radio    ODR running on %s: board %s, git %s, codec %s, stub entry %s"
                % (dev, probe.get("board"), probe.get("git"), probe.get("codec"), "yes" if offered else "no"))
    route, why = sf.choose_route(args.route, probe, offered)
    say("route    %s: %s" % (route.upper(), why))
    mode, cwhy, risk, expect_codec, source = sf.decide_codec(board, route, args.codec, slot is not None,
                                                             stock is not None, probe)
    if mode == planmod.MERGE and source == "file":
        slot, name = sf.slot_from_file(board, app, stock)
        say("stock    %s verified (SHA-256), codec slot built" % name)
    say("codec    %s: %s" % (mode.upper(), cwhy))
    if risk:
        warn(risk)
    raw = sf.contiguous(board, app, slot if mode == planmod.MERGE else None)

    say("")
    say("plan:")
    for line in sf.describe(board, route, raw, mode):
        say("  " + line)
    say("")
    if args.dry_run:
        say("dry run: nothing was erased or written")
        return 0

    if route == sf.BOOTLOADER:
        for line in sf.update_mode_steps(board):
            say(line)
        if not args.yes:
            if not sys.stdin.isatty():
                raise UsageError("Baofeng's update mode needs you at the radio: run in a terminal, or pass --yes")
            try:
                input("Press Enter when the radio is in update mode (Ctrl-C stops; nothing is written yet) ")
            except EOFError:
                raise UsageError("stopped: nothing was written")
        say("waiting  for the update mode loader on %s (up to %d s)..." % (dev, args.wait))
    elif not args.yes and not ask_yes("Write the changed sectors now?", default=True):
        say("nothing done")
        return 1
    started = flash_started(args, board, man)
    try:
        if route == sf.STUB:
            written, same = sf.flash_stub(dev, raw, log=lambda m: say("         " + m),
                                          progress=_progress("changed sectors"))
            say("done     %d sector(s) written, %d unchanged, every one checked by CRC; the radio restarts into ODR %s"
                % (written, same, man.get("version")))
        else:
            sf.flash_bootloader(dev, raw, board, log=say, progress=_progress("blocks"), wait_s=args.wait,
                                name=os.path.basename(args.image or "odr.bin"))
            say("done     the radio restarts into ODR %s" % man.get("version"))
    except BaseException as e:
        flash_finished(started, e)
        raise
    flash_finished(started)
    state.forget_probe()
    if args.no_verify:
        return 0
    return verify_serial(dev, board, man, expect_codec)


def verify_serial(dev, board, man, expect_codec, wait_s=25, probe_fn=None, sleep=time.sleep):
    """After the restart: ODR's VERSION over the cable, compared with what was flashed."""
    say("checking the radio after the restart (up to %d s; --no-verify skips this)..." % wait_s)
    p = serialflash.wait_for_odr(dev, wait_s, probe_fn, sleep)
    if p is None:
        say("the radio did not answer on its debug link. If its screen shows ODR, all is well. If it stays dark, "
            "flash again through update mode (--route bootloader).")
        return 0
    want_git = man.get("git")
    if want_git and want_git != "unknown" and not (p.get("git") or "").startswith(want_git):
        warn("the radio reports git %s, the image is %s" % (p.get("git"), want_git))
        return 1
    say("radio    running ODR git %s, codec %s" % (p.get("git"), p.get("codec")))
    if p.get("codec") == board.codec.id:
        say("ok       DMR voice is ready")
        return 0
    say("note     the radio has no usable codec (codec=%s): FM works, DMR voice is off. Flash again with --stock FILE "
        "(a program flash read of this radio: stubtool.py PORT --progmem FILE)" % p.get("codec"))
    return 0 if not expect_codec else 1


def report_simulation(board, p, dev, before, d):
    ok = True
    erased = [hex(a) for a in dev.erased]
    say("sim      erased %s; %d blocks written; vendor commands %s; exit %s"
        % (", ".join(erased), dev.blocks, dev.vendor, "yes" if dev.exited else "NO"))
    for s in board.codec_sectors():
        same = dev.read(s.addr, s.size) == before[s.addr - 0x08000000:s.end - 0x08000000]
        if d.mode == planmod.KEEP:
            say("sim      codec sector %d %s" % (s.index, "unchanged" if same else "CHANGED"))
            ok = ok and same
    for st in p.writes:
        want = b"".join(b for _, b in st.blocks)
        got = dev.read(st.sector.addr, len(want))
        if got != want:
            say("sim      sector %d content MISMATCH" % st.sector.index)
            ok = False
    if dev.violations:
        say("sim      %d bytes programmed over non-erased flash" % len(dev.violations))
        ok = False
    say("sim      %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def verify_after(args, board, man, d, wait_s=25):
    """After the reset: wait for ODR's debug link and compare its git and codec with what was flashed."""
    if debuglink.find_ports() is None:
        say("(install pyserial to have odrflash check the radio after flashing)")
        return 0
    say("checking the radio after the restart (up to %d s; --no-verify skips this)..." % wait_s)
    end = time.monotonic() + wait_s
    p = None
    while time.monotonic() < end and p is None:
        time.sleep(1.0)
        p = debuglink.probe(args.port)
    if p is None:
        say("the radio did not answer on its debug link. If its screen shows ODR, all is well (an FM-only build "
            "without USB has no debug link). If the screen stays dark, flash again from the bootloader.")
        return 0
    state.remember_probe(p)
    want_git = man.get("git")
    if want_git and want_git != "unknown" and not (p.get("git") or "").startswith(want_git):
        warn("the radio reports git %s, the image is %s" % (p.get("git"), want_git))
        return 1
    say("radio    running ODR git %s, codec %s" % (p.get("git"), p.get("codec")))
    if p.get("codec") == board.codec.id:
        say("ok       DMR voice is ready")
        return 0
    say("note     the radio has no usable codec (codec=%s): FM works, DMR voice is off. Flash again with --stock "
        "FILE (%s)" % (p.get("codec"), STOCK_HINT))
    return 0 if not d.expect_codec else 1


def cmd_probe(args):
    p = debuglink.probe(args.port, log=say)
    if p is None:
        if debuglink.find_ports() is None:
            raise UsageError("pyserial is not installed (pip install pyserial, or use the odrflash executable)")
        raise UsageError("no radio running ODR found on USB (1FC9:0094). Start it normally (not in the bootloader) "
                         "and connect the cable; stock firmware has no debug link")
    state.remember_probe(p)
    b = boards.from_debuglink_name(p.get("board") or "")
    say("radio    %s on %s, ODR git %s, chip %s" % (p.get("board"), p["port"], p.get("git"), p.get("uid") or "?"))
    say("codec    %s" % p.get("codec"))
    if b and p.get("codec") == b.codec.id:
        say("ok       the codec can be kept: 'odrflash flash' will leave the codec sectors alone (remembered for "
            "%d min)" % (state.PROBE_MAX_AGE_S // 60))
    elif p.get("codec") is None:
        say("note     this ODR build doesn't report its codec; flash with --stock once to be sure")
    else:
        say("note     no codec to keep; flash with --stock FILE (%s)" % STOCK_HINT)
    return 0


def cmd_detect(args):
    p = debuglink.probe(args.port, log=say) if not args.no_probe else None
    if p:
        say("ODR      %s on %s: git %s, codec %s" % (p.get("board"), p["port"], p.get("git"), p.get("codec")))
    elif debuglink.find_ports() is None:
        say("ODR      (pyserial not installed: not checked)")
    else:
        say("ODR      no radio running ODR on USB")
    usb = dfumod.import_usb()
    # every USB DFU bootloader the registry knows, each USB id once (radios of one vendor share theirs)
    cands = [boards.get(args.board)] if args.board else boards.by(family="usb-dfu")
    devs, ids = [], []
    for b in cands:
        if b.dfu_ids not in ids:
            ids.append(b.dfu_ids)
            devs += [(b, d) for d in dfumod.find(usb, b)]
    if not devs:
        say("DFU      no bootloader (%s) found" % ", ".join("%04X:%04X" % i for i in ids))
        return 0 if p else 1
    for board, dev in devs:
        fp = dfumod.fingerprint(usb, dev)
        say("DFU      %04X:%04X string 1 %r, manufacturer %r, product %r, serial %r"
            % (board.dfu_ids + (fp["string1"], fp["manufacturer"], fp["product"], fp["serial"])))
        for i in fp["interfaces"]:
            say("         layout %r" % i)
        if "AnyRoad" in (fp["string1"] or ""):
            say("         a TYT/Baofeng AnyRoad bootloader (DM-1701, MD-UV380, MD-9600 and others share it; the model "
                "isn't reported, odrflash goes by the image, the debug link probe or --board)")
        else:
            say("         not the radio's own bootloader (ST's ROM loader?): odrflash won't flash it")
    return 0


def cmd_key(args):
    if not args.stock:
        p = None if args.board else debuglink.probe(args.port, log=say)
        board = (p and boards.from_debuglink_name(p.get("board") or "")) or pick_board(args.board, "key")
        key = bootkey.from_radio(board, args.port, log=say)
        if not key:
            raise UsageError("no radio running ODR gave the %s bootloader key: connect it, or use --stock FILE"
                             % board.name)
        key = dfumod.check_key(key, board)
        say("key      read from the radio's bootloader and verified (SHA-256 %s...)" % board.key_sha256[:16])
    else:
        with open(os.path.expanduser(args.stock), "rb") as f:
            stock = f.read()
        what = stockfw.identify(stock)
        say("stock    %s" % (what or "unknown file (trying anyway)"))
        board = stock_board(stock, args.board)
        try:
            key = dfumod.check_key(stockfw.derive_key(stock, board.key_sha256), board)
        except stockfw.StockError as e:
            raise UsageError(str(e))
        say("key      derived and verified (SHA-256 %s...)" % board.key_sha256[:16])
    out = state.key_path(board.id) if args.save else args.output
    if not out:
        say("(use -o FILE or --save to keep it; flash commands also get it from --stock or from the radio each time)")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as f:
        f.write(key)
    say("wrote    %s (keep it to yourself; never commit it)" % out)
    return 0


def cmd_merge(args):
    with open(os.path.expanduser(args.image), "rb") as f:
        image = f.read()
    with open(os.path.expanduser(args.stock), "rb") as f:
        stock = f.read()
    board = stock_board(stock, args.board)
    key = (dfumod.load_key(args.key, board) if args.key
           else dfumod.check_key(stockfw.derive_key(stock, board.key_sha256), board))
    # a release image comes with its manifest: check the two belong together
    man_path = os.path.splitext(os.path.expanduser(args.image))[0] + ".json"
    if os.path.exists(man_path):
        mf.verify(mf.load(man_path), image, board.id)
    try:
        prof, segs = stockfw.extract(stock, key)
        merged = stockfw.merge(image, prof, segs)
    except stockfw.StockError as e:
        raise UsageError(str(e))
    with open(args.output, "wb") as f:
        f.write(merged)
    say("wrote    %s (%d bytes, contains the proprietary codec: keep it to yourself)" % (args.output, len(merged)))
    return 0


def cmd_download(args):
    board = pick_board(args.board, "download")
    opts = resolve_options(args, board, None, need_all=True)
    img, man = relmod.fetch(board.image_id(opts), args.release, args.repo, dest=args.output, log=say)
    with open(img, "rb") as f:
        mf.verify(mf.load(man), f.read(), board.id)
    say("ok       %s\n         %s" % (img, man))
    return 0


def build_parser():
    ap = argparse.ArgumentParser(prog="odrflash", description="Flash OpenDigitalRadio onto a radio, keeping or "
                                 "writing the radio's own AMBE codec.", epilog=EPILOG,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version="odrflash " + __version__)
    sub = ap.add_subparsers(dest="cmd")

    def common(p):
        p.add_argument("--board", help="radio model (%s); normally taken from the manifest and the radio" % ", ".join(sorted(boards.BOARDS)))
        p.add_argument("--port", help="the radio's serial port when it runs ODR (found by USB ID otherwise); DM-1801 "
                       "Pro, DM-32UV: the programming cable's port (found when it is the only USB-serial cable)")
        p.add_argument("--no-probe", action="store_true", help="don't ask a running ODR radio over its debug link")
        p.add_argument("--option", action="append", metavar="NAME=VALUE", help="a hardware build option, such as "
                       "BAND=vhf on the MD-380 (normally taken from the board name the radio reports)")

    f = sub.add_parser("flash", help="flash a release image", formatter_class=argparse.RawDescriptionHelpFormatter)
    f.add_argument("image", nargs="?", help="image file (.bin, with its .json manifest next to it, or with the codec merged in)")
    f.add_argument("--release", metavar="VERSION", help="download this release (v0.1.0, or latest) from GitHub")
    f.add_argument("--manifest", help="the manifest, when it isn't next to the image")
    f.add_argument("--repo", default=relmod.DEFAULT_REPO, help="GitHub repository (default %(default)s)")
    f.add_argument("--codec", choices=("auto", "keep", "merge"), default="auto",
                   help="auto (default): keep the radio's codec when that is known to be safe, else write it from "
                        "--stock; keep: never touch the codec sectors; merge: write the codec from --stock")
    f.add_argument("--stock", metavar="FILE", help="the radio's stock firmware file (codec source; also gives the key); "
                   "DM-1801 Pro: a program flash read of the radio (stubtool.py PORT --progmem FILE)")
    f.add_argument("--stock-installed", metavar="VERSION",
                   help="the stock firmware the radio runs now, e.g. \"V02.06 CSV\" (instead of the question)")
    f.add_argument("--key", metavar="FILE", help="the 1024-byte bootloader key (else derived from --stock)")
    f.add_argument("--dry-run", action="store_true", help="print the sectors that would be erased and written; "
                   "touch nothing")
    f.add_argument("--yes", "-y", action="store_true", help="don't ask")
    f.add_argument("--wait", type=int, default=120, help="seconds to wait for the bootloader (default 120)")
    f.add_argument("--route", choices=serialflash.ROUTES, default=serialflash.AUTO,
                   help="DM-1801 Pro, DM-32UV: auto (default): the stub when the radio runs ODR with stub entry, else "
                        "Baofeng's update mode; stub; bootloader (update mode, PTT and SK1 at power-on)")
    f.add_argument("--no-verify", action="store_true", help="don't check the radio after it restarts")
    f.add_argument("--no-reporting", action="store_true",
                   help="don't send the ODR project the radio model, build and result of this flash")
    f.add_argument("--simulate", choices=("stock", "odr-codec", "odr-nocodec", "blank"),
                   help="run against a fake radio (testing: no USB); --stock synthetic gives a synthetic stock file")
    common(f)
    f.set_defaults(fn=cmd_flash)

    p = sub.add_parser("probe", help="ask a radio running ODR whether its codec can be kept")
    p.add_argument("--port")
    p.set_defaults(fn=cmd_probe)

    d = sub.add_parser("detect", help="look for the radio: running ODR and/or in its bootloader")
    common(d)
    d.set_defaults(fn=cmd_detect)

    k = sub.add_parser("key", help="get the bootloader key from a radio running ODR, or from a stock firmware file")
    k.add_argument("--stock", help="derive it from this stock file instead of reading it from the radio")
    k.add_argument("--port")
    k.add_argument("-o", "--output")
    k.add_argument("--save", action="store_true", help="keep it in odrflash's settings directory")
    k.add_argument("--board")
    k.set_defaults(fn=cmd_key)

    m = sub.add_parser("merge", help="write an image with the codec merged in, to a file (for other tools)")
    m.add_argument("image")
    m.add_argument("--stock", required=True)
    m.add_argument("--key")
    m.add_argument("-o", "--output", required=True)
    m.add_argument("--board")
    m.add_argument("--codec", choices=("auto", "keep", "merge"), default="auto", help="GD-77: keep: no codec in the file")
    m.set_defaults(fn=cmd_merge)

    g = sub.add_parser("download", help="download a release image and manifest, verified")
    g.add_argument("--release", default="latest")
    g.add_argument("--repo", default=relmod.DEFAULT_REPO)
    g.add_argument("-o", "--output", help="directory (default: odrflash's cache)")
    g.add_argument("--board")
    g.add_argument("--option", action="append", metavar="NAME=VALUE", help="a hardware build option (BAND=vhf)")
    g.set_defaults(fn=cmd_download)
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "fn", None):
        ap.print_help()
        return 2
    try:
        return args.fn(args)
    except (UsageError, mf.ManifestError, planmod.PlanError, dfumod.DfuError, relmod.ReleaseError,
            stockfw.StockError, serialflash.SerialError, KeyError) as e:
        print("error: %s" % (e.args[0] if isinstance(e, KeyError) and e.args else e), file=sys.stderr)
        return 2
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\ninterrupted", file=sys.stderr)
        return 130
