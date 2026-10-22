# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The serial route for the HR_C7000 radios (DM-1801 Pro, DM-32UV; boards.py family "bfuv32-serial"): flashing over
the programming cable, through one of two existing tools that odrflash wraps instead of speaking their protocols again:

  stub        a radio that runs ODR built with CONFIG_STUB_ENTRY=y: stub/stubtool.py loads its RAM monitor over the
              debug link (--odr-enter), writes the 4 KB sectors that differ (--write-app) and restarts the radio
              (--boot reset). No keys to hold. Only the sectors the image covers are written, so the codec slot
              stays when the image has none.
  bootloader  Baofeng's update mode (PTT and SK1 at power-on): tools/serial_loader.py. The loader erases the whole
              application area, the codec slot included, so the codec must be in what is written.

auto picks stub when the radio answers the debug link and offers stub entry, else the bootloader. The codec slot is
built by tools/hrc7000_codec.py from the user's own file (DM-1801 Pro: a program flash read, stubtool.py --progmem;
DM-32UV: the stock firmware file). docs/dev/release.md "The serial route".

The tools are loaded from the checkout (tools/, stub/) or, in the odrflash executable, from the copies PyInstaller
bundles (--hidden-import).
"""
import importlib
import importlib.util
import os
import struct
import time

from . import debuglink

AUTO, STUB, BOOTLOADER = "auto", "stub", "bootloader"
ROUTES = (AUTO, STUB, BOOTLOADER)

SLOT_MAGIC = 0x43524444		# "DDRC": the first word of a codec slot (platform/hrc7000/vocoder.c, hrc7000_codec.py)
BAUD = 115200

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
_PLACES = {"serial_loader": "tools", "hrc7000_codec": "tools", "stubtool": "stub"}
_loaded = {}


class SerialError(Exception):
    pass


def tool(name):
    """The module of an ODR tool script (serial_loader, hrc7000_codec, stubtool)."""
    if name in _loaded:
        return _loaded[name]
    path = os.path.join(_ROOT, _PLACES[name], name + ".py")
    if os.path.isfile(path):
        spec = importlib.util.spec_from_file_location("odrflash_tool_" + name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    else:
        try:
            mod = importlib.import_module(name)
        except ImportError:
            raise SerialError("the serial route needs %s/%s.py from an OpenDigitalRadio checkout (run odrflash from the "
                              "checkout, or use the odrflash executable, which carries it)" % (_PLACES[name], name))
    _loaded[name] = mod
    return mod


def slot_offset(board):
    """Offset of the codec slot from the application base (the board's one codec window)."""
    return board.codec.windows[0][0] - board.app_base


def split_slot(board, image):
    """(application, slot bytes or None) of a contiguous image from the application base. An image with the codec
    merged in (the build's _codec.bin, tools/hrc7000_codec.py) carries "DDRC" at the slot; everything between the
    application and the slot must be 0xFF, as hrc7000_codec.merge() pads it."""
    off = slot_offset(board)
    if len(image) < off + 4 or struct.unpack_from("<I", image, off)[0] != SLOT_MAGIC:
        return image, None
    app = image[:off].rstrip(b"\xFF")
    return app, image[off:]


def contiguous(board, app, slot=None):
    """What a serial route writes from the application base: the application, then 0xFF and the slot."""
    off = slot_offset(board)
    if len(app) > off:
        raise SerialError("the application (%d bytes) runs into the codec slot at 0x%08X"
                          % (len(app), board.codec.windows[0][0]))
    if slot is None:
        return bytes(app)
    if len(slot) > board.flash_end - board.codec.windows[0][0]:
        raise SerialError("the codec slot (%d bytes) does not fit before the end of flash" % len(slot))
    return bytes(app) + b"\xFF" * (off - len(app)) + bytes(slot)


def slot_from_file(board, app, data):
    """The codec slot made from the user's file by tools/hrc7000_codec.py (checked by SHA-256): (slot bytes, name)."""
    dc = tool("hrc7000_codec")
    try:
        prof, code, tables = dc.extract(data, board=board.id)
        merged = dc.merge(app, prof, code, tables)
    except dc.ExtractError as e:
        raise SerialError("%s (DM-1801 Pro: a program flash read, stubtool.py PORT --progmem FILE; DM-32UV: the stock "
                          "firmware file)" % e)
    return merged[slot_offset(board):], prof["name"]


# ---------------------------------------------------------------------------------------------------------------
# the cable and the radio on it

def find_cable(port=None):
    """The programming cable's serial port: --port, else the only known USB-serial cable."""
    df = tool("serial_loader")
    try:
        return df.find_port(port)
    except df.FlashError as e:
        raise SerialError(str(e))


def probe(dev, log=None):
    """VERSION over the cable (debuglink.py): the running ODR's board, git and codec, or None."""
    return debuglink.probe(dev, log=log)


def open_port(dev):
    try:
        import serial
    except ImportError:
        raise SerialError("pyserial is not installed (pip install pyserial, or use the odrflash executable)")
    try:
        return serial.Serial(dev, BAUD, timeout=0.1)
    except (serial.SerialException, OSError) as e:
        raise SerialError("can't open %s: %s (close the CPS, qdmr and any terminal using it)" % (dev, e))


def stub_offered(dev, opener=None):
    """Whether the running ODR offers stub entry: the debug link's STUB info (op 0, read only; debuglink.md 3.18)."""
    st = tool("stubtool")
    port = (opener or open_port)(dev)
    try:
        r = st.Radio(port, log=lambda *a: None).dl_request(st.DL_STUB, b"\x00", retries=1)
    except (st.DumpError, OSError):
        return False
    finally:
        try:
            port.close()
        except Exception:
            pass
    return len(r) >= 19 and r[0] == 0 and int.from_bytes(r[1:5], "little") == st.STUB_MAGIC


def choose_route(requested, probe_result, offered):
    """(route, why). requested: auto, stub or bootloader."""
    if requested == BOOTLOADER:
        return BOOTLOADER, "asked for: Baofeng's update mode"
    if offered:
        return STUB, "ODR runs on the radio and offers stub entry: no keys to hold"
    if requested == STUB:
        if probe_result:
            raise SerialError("ODR on the radio answers, but was built without CONFIG_STUB_ENTRY=y: use the bootloader "
                              "(--route bootloader, or leave --route out)")
        raise SerialError("no ODR answers on the cable, so the stub can't be entered: start the radio with ODR (built "
                          "with CONFIG_STUB_ENTRY=y) and connect the programming cable, or use --route bootloader")
    if probe_result:
        return BOOTLOADER, "ODR runs on the radio but was built without CONFIG_STUB_ENTRY=y, so the update goes " \
                           "through Baofeng's update mode"
    return BOOTLOADER, "no ODR answers on the cable, so the update goes through Baofeng's update mode"


def decide_codec(board, route, requested, slot_in_image, have_file, probe_result):
    """The codec on the serial route: (mode, why, risk, expect_codec, source). mode keep or merge (plan.KEEP/MERGE
    spelling); source "image" or "file" for merge. Raises SerialError when neither is possible."""
    hint = ("give --stock FILE with your codec source (DM-1801 Pro: a program flash read, stubtool.py PORT --progmem "
            "FILE; DM-32UV: the stock firmware file)")
    if requested == "merge" and not (slot_in_image or have_file):
        raise SerialError("--codec merge needs the codec: " + hint)
    if requested != "keep":
        if slot_in_image:
            return "merge", "the image carries the codec slot, so it is written from the image", None, True, "image"
        if have_file:
            return "merge", "the codec slot is built from your file and written", None, True, "file"
    if route == BOOTLOADER:
        raise SerialError("Baofeng's update mode erases the whole application area, the codec slot included, so the "
                          "codec can't be kept that way; %s, or update a radio that runs ODR with stub entry "
                          "(--route stub)" % hint)
    codec = (probe_result or {}).get("codec")
    if codec == board.codec.id:
        return "keep", "ODR on the radio checked its codec (%s); the stub writes only the image's sectors, so the " \
                       "codec slot stays" % codec, None, True, None
    return "keep", "the stub writes only the image's sectors, so the codec slot is left as it is", \
        "the radio reports codec=%s: DMR voice stays off (FM works) until you flash with --stock" % codec, False, None


def describe(board, route, raw, mode):
    """The plan's lines."""
    lo, hi = board.app_base, board.app_base + len(raw)
    kb = (len(raw) + 1023) // 1024
    slot = board.codec.windows[0][0]
    lines = ["board %s, route: %s, codec mode: %s" % (board.name, route, mode)]
    if route == STUB:
        lines.append("ODR loads the stub into RAM; the stub compares 0x%08X..0x%08X (%d KB) with the flash by CRC and "
                     "erases and writes only the 4 KB sectors that differ, then checks every one" % (lo, hi - 1, kb))
        if mode == "keep":
            lines.append("not written: the codec slot 0x%08X..0x%08X" % (slot, board.flash_end - 1))
        lines.append("then the stub restarts the radio into the new firmware")
    else:
        lines.append("the update mode loader erases the whole application area 0x%08X..0x%08X, codec slot included"
                     % (board.app_base, board.flash_end - 1))
        lines.append("then writes 0x%08X..0x%08X: %d blocks of 1 KB" % (lo, hi - 1, (len(raw) + 1023) // 1024))
    for plo, phi in board.protected:
        lines.append("never erased or written: the bootloader 0x%08X..0x%08X" % (plo, phi - 1))
    return lines


# ---------------------------------------------------------------------------------------------------------------
# the two routes

def flash_stub(dev, raw, log=print, progress=None, opener=None):
    """stubtool.py PORT --odr-enter --write-app IMAGE --boot reset, as a library call. Returns (sectors written,
    sectors unchanged)."""
    st = tool("stubtool")
    port = (opener or open_port)(dev)
    try:
        radio = st.Radio(port, log=log)
        radio.odr_enter()
        try:
            written, same = radio.write_image(raw, progress=progress)
        except st.DumpError as e:
            raise SerialError("%s. The bootloader is never written: run the same command again, or put the radio in "
                              "update mode (PTT and SK1 at power-on) and use --route bootloader" % e)
        radio.reset_chip()
    except st.DumpError as e:
        raise SerialError(str(e))
    finally:
        try:
            port.close()
        except Exception:
            pass
    return written, same


def flash_bootloader(dev, raw, board, log=print, progress=None, wait_s=120, opener=None, name="odr.bin"):
    """tools/serial_loader.py --model BOARD IMAGE, as a library call. Waits up to wait_s for the loader to answer
    (the user switches the radio on in update mode meanwhile)."""
    df = tool("serial_loader")
    model = df.MODELS.get(board.id)
    if model is None:
        raise SerialError("tools/serial_loader.py has no model %s" % board.id)
    try:
        for line in df.check_image(raw, model):
            log("         " + line)
    except df.FlashError as e:
        raise SerialError(str(e))
    port = (opener or df.SerialPort)(dev)
    end = time.monotonic() + wait_s
    try:
        while True:
            try:
                df.Flasher(port, log=lambda m: log("      " + m), progress=progress, model=model).flash(raw, name)
                return
            except df.FlashError as e:
                # only "nobody answers yet" is worth waiting for: anything else is a real answer from the radio
                if not str(e).startswith("step 1: no answer") or time.monotonic() >= end:
                    raise SerialError(str(e))
    finally:
        try:
            port.close()
        except Exception:
            pass


def update_mode_steps(board):
    df = tool("serial_loader")
    model = df.MODELS.get(board.id)
    keys = model.keys if model else "the update mode keys"
    return ["Put the radio in update mode now: switch it off, hold %s, and switch it on while holding them." % keys,
            "The screen stays dark. Release the keys and keep the cable in."]


def wait_for_odr(dev, wait_s=25, probe_fn=None, sleep=time.sleep):
    """After the restart: the radio's VERSION over the cable, or None."""
    probe_fn = probe_fn or probe
    end = time.monotonic() + wait_s
    while True:
        sleep(1.0)
        p = probe_fn(dev)
        if p or time.monotonic() >= end:
            return p
