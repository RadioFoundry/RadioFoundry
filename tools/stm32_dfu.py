#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Flash a plain application image to a TYT/Baofeng/Retevis STM32 radio (DM-1701, RT84, MD-UV380/390, MD-2017,
MD-380, MD-9600) through the stock AnyRoad DFU bootloader.

Written from docs/spec/hardware-dm1701.md section 15:
  15.1  the radio enumerates as VID 0x0483 PID 0xDF11 (string 1 "AnyRoad Technology") when powered on with
        PTT + side key 1 held;
  15.2  wait idle, vendor commands 0x91 0x01 and 0x91 0x31, erase the sector list, write each region as 1024-byte
        blocks numbered from 2, pad the last block with 0xFF, exit with set-address 0x08000000 + zero-length DNLOAD;
  15.3  the image is XORed with a 1024-byte per-model key, indexed from the image start (0x0800C000).

The key is never stored in this repository. You supply it as a 1024-byte file and the tool checks its SHA-256
against the table below before using it.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time

# the board registry (tools/boards.py generates it): the ODR boards on this bootloader, their sectors, keys and codecs
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "odrflash"))
from odrflash import boards  # noqa: E402

ODR_BOARDS = [b for b in boards.BOARDS.values() if b.key_sha256]
BLOCK = 1024
FIRST_BLOCK = 2
KEY_LEN = 1024

DFU_VID, DFU_PID = ODR_BOARDS[0].dfu_ids

# (sector address, size) in the order the bootloader expects them (15.2 step 4/5), the same on every radio it runs
# on. The loader also lists 0x08100000, which is past the end of the 1 MB flash; it is never erased or written here.
REGIONS = [(s.addr, s.size) for s in ODR_BOARDS[0].sectors]
APP_BASE = ODR_BOARDS[0].app_base
FLASH_END = ODR_BOARDS[0].flash_end
assert all([(s.addr, s.size) for s in b.sectors] == REGIONS and (b.app_base, b.flash_end) == (APP_BASE, FLASH_END)
           and b.dfu_ids == (DFU_VID, DFU_PID) for b in ODR_BOARDS), "the registry's DFU boards disagree on the flash map"

# Per-model key identity (15.3). The key bytes are not reproduced; only their hash and first four bytes. The ODR
# boards' come from the registry ([flash.dfu]); STOCK_ONLY are radios with this bootloader and no ODR board yet
# (planned, docs/spec/radios.md), for flashing their stock firmware back.
STOCK_ONLY = {
    "mduv380": ("739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991", bytes.fromhex("00AA8989")),
    "md9600": ("c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae", bytes.fromhex("A2FABB4B")),
}
KEYS = dict({b.id: (b.key_sha256, b.key_prefix) for b in ODR_BOARDS}, **STOCK_ONLY)
ALIASES = dict({b.id: b.id for b in ODR_BOARDS}, **{
    "rt84": "dm1701",      # spec: same key as the DM-1701, unverified
    "mduv380": "mduv380",
    "mduv390": "mduv380",
    "md2017": "mduv380",
    "md9600": "md9600",
})

# Where a merged image holds the codec (docs/spec/codec-placement.md: the tables at their stock address, the code moved
# into the top sector), and where its code starts: the board's codec windows (the union over its stock files) and the
# code address of each stock file's placement. An image that reaches into a window must carry a merged codec,
# recognized by its first word at one of those addresses.
CODEC_WINDOWS = {b.id: sorted((a, a + n) for a, n in b.codec.windows) for b in ODR_BOARDS}
CODEC_SIG_ADDRS = {b.id: b.codec_codes for b in ODR_BOARDS}
CODEC_SIG = 0x4FF0E92D

STOCK_MAGIC = b"OutSecurityBin"

# DFU requests and states
DFU_DNLOAD, DFU_GETSTATUS, DFU_CLRSTATUS, DFU_ABORT = 1, 3, 4, 6
ST_IDLE, ST_DNBUSY, ST_DNIDLE, ST_ERROR = 2, 4, 5, 10
STATE_NAMES = {0: "appIDLE", 1: "appDETACH", 2: "dfuIDLE", 3: "dfuDNLOAD-SYNC", 4: "dfuDNBUSY",
               5: "dfuDNLOAD-IDLE", 6: "dfuMANIFEST-SYNC", 7: "dfuMANIFEST", 8: "dfuMANIFEST-WAIT-RESET",
               9: "dfuUPLOAD-IDLE", 10: "dfuERROR"}

EPILOG = """\
examples:
  %(prog)s --list
  %(prog)s --model dm1701 --key ~/keys/dm1701.key build/dm1701/odr_dm1701.bin
  %(prog)s --model dm1701 --key ~/keys/dm1701.key --dry-run -o enc.bin build/dm1701/odr_dm1701.bin
  %(prog)s --self-test

models: dm1701 (also rt84), mduv380 (also mduv390, md2017), md380, md9600

entering the bootloader: hold PTT + side key 1 (just above PTT) while switching the radio on. The screen stays dark.

requirements: Python 3 and pyusb (pip install pyusb) on top of libusb.
  macOS:  brew install libusb
  Linux:  apt install libusb-1.0-0   (and a udev rule for 0483:df11, see the error message if access is denied)
  Windows: bind the device to WinUSB with Zadig.

The key file is yours to supply (1024 bytes); it is checked by SHA-256 and never stored in this repository.
Never flash an official "OutSecurityBin" firmware file with this tool; it is refused.
"""


class DfuError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# image handling (no USB)

def load_key(path, model):
    with open(os.path.expanduser(path), "rb") as f:
        key = f.read()
    if len(key) != KEY_LEN:
        raise DfuError("key file %s is %d bytes, expected %d" % (path, len(key), KEY_LEN))
    want_hash, want_head = KEYS[model]
    got = hashlib.sha256(key).hexdigest()
    if got != want_hash:
        hint = ""
        for other, (h, _) in KEYS.items():
            if h == got:
                hint = " (this is the %s key)" % other
        raise DfuError("key SHA-256 mismatch for model %s%s\n  got      %s\n  expected %s"
                       % (model, hint, got, want_hash))
    if key[:4] != want_head:  # cannot happen when the hash matches; kept as a table consistency check
        raise DfuError("key first bytes do not match the table for %s" % model)
    return key


def xor_image(data, key):
    """encrypted[n] = plain[n] XOR key[n mod 1024], n from the image start (0x0800C000). Symmetric."""
    if len(key) != KEY_LEN:
        raise ValueError("key must be %d bytes" % KEY_LEN)
    reps = (len(data) + KEY_LEN - 1) // KEY_LEN
    stream = (key * reps)[:len(data)]
    return bytes(a ^ b for a, b in zip(data, stream))


def check_image(data, model):
    """Return a list of warnings; raise DfuError for images that must never be flashed."""
    warnings = []
    if data.startswith(STOCK_MAGIC):
        raise DfuError("this is an official TYT firmware file (starts with \"OutSecurityBin\"); "
                       "it is already encrypted and must not be flashed as an application image")
    if data.startswith(b"\x7fELF"):
        raise DfuError("this is an ELF file; flash the raw .bin (objcopy -O binary)")
    if not data:
        raise DfuError("image is empty")
    end = APP_BASE + len(data)
    if end > FLASH_END:
        raise DfuError("image is %d bytes and would end at 0x%08X, past the end of flash 0x%08X (max %d bytes)"
                       % (len(data), end, FLASH_END, FLASH_END - APP_BASE))
    if len(data) >= 8:
        sp, reset = struct.unpack_from("<II", data, 0)
        if not (0x20000000 <= sp <= 0x20020000 or 0x10000000 <= sp <= 0x10010000):
            warnings.append("initial stack pointer 0x%08X is not in SRAM/CCM; is this an application image "
                            "linked for 0x%08X?" % (sp, APP_BASE))
        if not (APP_BASE <= (reset & ~1) < FLASH_END) or not (reset & 1):
            warnings.append("reset vector 0x%08X is not a Thumb address inside 0x%08X..0x%08X"
                            % (reset, APP_BASE, FLASH_END))
    windows = CODEC_WINDOWS.get(model, [])
    hit = [(a, b) for a, b in windows if end > a]
    if hit:
        words = {}
        for sig_addr in CODEC_SIG_ADDRS[model]:
            off = sig_addr - APP_BASE
            words[sig_addr] = struct.unpack_from("<I", data, off)[0] if off + 4 <= len(data) else None
        if CODEC_SIG not in words.values():
            spans = ", ".join("0x%08X..0x%08X" % w for w in hit)
            found = ", ".join("0x%08X is %s" % (a, "missing" if w is None else "0x%08X" % w) for a, w in words.items())
            warnings.append("image extends to 0x%08X, into the codec window(s) %s, but the word at %s, not the "
                            "codec signature 0x%08X: the application overlaps the codec window "
                            "(DMR voice will not work and the app may be corrupt if a codec is merged later)"
                            % (end, spans, found, CODEC_SIG))
    return warnings


def plan_blocks(data):
    """Split the (already encrypted) image into the regions it covers.

    Returns [(region_addr, region_size, [(block_number, 1024 bytes), ...]), ...]. The stream is contiguous from
    0x0800C000; the last block is padded with 0xFF.
    """
    plan = []
    pos = 0
    for addr, size in REGIONS:
        if pos >= len(data):
            break
        chunk = data[pos:pos + size]
        blocks = []
        for i in range(0, len(chunk), BLOCK):
            blk = chunk[i:i + BLOCK]
            if len(blk) < BLOCK:
                blk = blk + b"\xFF" * (BLOCK - len(blk))
            blocks.append((FIRST_BLOCK + i // BLOCK, blk))
        plan.append((addr, size, blocks))
        pos += size
    if pos < len(data):
        raise DfuError("image does not fit the region list")
    return plan


def describe_plan(plan, nbytes):
    lines = ["erase %d sectors: %s" % (len(REGIONS), ", ".join("0x%08X" % a for a, _ in REGIONS))]
    for addr, size, blocks in plan:
        lines.append("write 0x%08X (region 0x%05X): blocks 0x%X..0x%X (%d x %d bytes)"
                     % (addr, size, blocks[0][0], blocks[-1][0], len(blocks), BLOCK))
    total = sum(len(b) for _, _, b in plan)
    lines.append("total %d bytes in %d blocks (image %d bytes, end 0x%08X)"
                 % (total * BLOCK, total, nbytes, APP_BASE + nbytes))
    lines.append("exit: set address 0x08000000, zero-length DNLOAD (radio resets)")
    return lines


# ---------------------------------------------------------------------------------------------------------------
# USB / DFU

def import_usb():
    try:
        import usb.core
        import usb.util
    except ImportError:
        raise DfuError("pyusb is not installed: pip install pyusb (it also needs libusb: "
                       "'brew install libusb' on macOS, 'apt install libusb-1.0-0' on Linux)")
    return usb


def permission_hint():
    if sys.platform == "darwin":
        return ("on macOS: close any other program using the radio (STM32CubeProgrammer, CPS in a VM with USB "
                "passthrough), unplug and replug the cable, and make sure libusb is from Homebrew "
                "(brew install libusb); as a last resort run with sudo")
    if sys.platform.startswith("linux"):
        return ("on Linux: add a udev rule, e.g.\n"
                "  echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0483\", ATTRS{idProduct}==\"df11\", "
                "MODE=\"0666\"' | sudo tee /etc/udev/rules.d/50-stm32-dfu.rules\n"
                "  sudo udevadm control --reload-rules && sudo udevadm trigger\n"
                "then replug the radio (or run with sudo once to confirm)")
    return "on Windows: install the WinUSB driver for 0483:DF11 with Zadig"


def find_devices(usb):
    try:
        return list(usb.core.find(find_all=True, idVendor=DFU_VID, idProduct=DFU_PID))
    except usb.core.NoBackendError:
        raise DfuError("pyusb found no libusb backend: 'brew install libusb' on macOS, "
                       "'apt install libusb-1.0-0' on Linux")


def get_string(usb, dev, index):
    if not index:
        return None
    try:
        return usb.util.get_string(dev, index)
    except (usb.core.USBError, ValueError) as e:
        if getattr(e, "errno", None) == 13:
            raise DfuError("access denied reading the device descriptors\n" + permission_hint())
        return None


def dfu_interfaces(dev):
    out = []
    for cfg in dev:
        for intf in cfg:
            if intf.bInterfaceClass == 0xFE and intf.bInterfaceSubClass == 0x01:
                out.append(intf)
    return out


def list_devices():
    usb = import_usb()
    devs = find_devices(usb)
    if not devs:
        print("no DFU device 0483:DF11 found. Power the radio on while holding PTT + side key 1 "
              "(the key just above PTT); the screen stays dark.")
        return 1
    for dev in devs:
        print("bus %s address %s: %04X:%04X" % (dev.bus, dev.address, dev.idVendor, dev.idProduct))
        print("  string 1:      %r" % get_string(usb, dev, 1))
        print("  manufacturer:  %r" % get_string(usb, dev, dev.iManufacturer))
        print("  product:       %r" % get_string(usb, dev, dev.iProduct))
        print("  serial:        %r" % get_string(usb, dev, dev.iSerialNumber))
        for intf in dfu_interfaces(dev):
            print("  interface %d alt %d: %r" % (intf.bInterfaceNumber, intf.bAlternateSetting,
                                                 get_string(usb, dev, intf.iInterface)))
        if "AnyRoad" not in (get_string(usb, dev, 1) or ""):
            print("  note: string 1 is not \"AnyRoad Technology\"; this may be ST's ROM bootloader, "
                  "not the radio's bootloader")
    return 0


class Dfu:
    """DfuSe-style sequence from 15.2 over any object with a pyusb-like ctrl_transfer()."""

    def __init__(self, dev, intf, log=print, sleep=time.sleep):
        self.dev = dev
        self.intf = intf
        self.log = log
        self.sleep = sleep

    def _out(self, req, value, data):
        return self.dev.ctrl_transfer(0x21, req, value, self.intf, data, 5000)

    def get_status(self):
        r = bytes(self.dev.ctrl_transfer(0xA1, DFU_GETSTATUS, 0, self.intf, 6, 5000))
        if len(r) != 6:
            raise DfuError("short GETSTATUS reply (%d bytes)" % len(r))
        status = r[0]
        poll_ms = r[1] | (r[2] << 8) | (r[3] << 16)
        return status, poll_ms, r[4]

    def wait_idle(self):
        """15.2 step 1: until state == dfuIDLE, CLRSTATUS and retry (ABORT as a fallback)."""
        for attempt in range(20):
            status, _, state = self.get_status()
            if state == ST_IDLE:
                return
            if attempt < 10:
                self._out(DFU_CLRSTATUS, 0, None)
            else:
                self._out(DFU_ABORT, 0, None)
        raise DfuError("device stays in state %s (status %d), not dfuIDLE; power-cycle the radio into the "
                       "bootloader and retry" % (STATE_NAMES.get(state, state), status))

    def wait_dnload_done(self, what):
        """After a DNLOAD: expect dnBUSY then dnIDLE (busy may be skipped on a fast device)."""
        for _ in range(2000):
            status, poll_ms, state = self.get_status()
            if state == ST_ERROR or status != 0:
                raise DfuError("%s failed: status %d, state %s" % (what, status, STATE_NAMES.get(state, state)))
            if state == ST_DNIDLE:
                return
            if state != ST_DNBUSY:
                raise DfuError("%s: unexpected state %s (expected dfuDNBUSY/dfuDNLOAD-IDLE)"
                               % (what, STATE_NAMES.get(state, state)))
            self.sleep(max(poll_ms, 1) / 1000.0)
        raise DfuError("%s: device stayed busy" % what)

    def vendor(self, b0, b1):
        what = "vendor command 0x%02X 0x%02X" % (b0, b1)
        self._out(DFU_DNLOAD, 0, bytes([b0, b1]))
        self.get_status()
        self.sleep(0.1)
        self.get_status()
        self.wait_idle()

    def command(self, cmd, addr, what):
        self._out(DFU_DNLOAD, 0, bytes([cmd]) + struct.pack("<I", addr))
        self.wait_dnload_done(what)

    def flash(self, plan, progress=None):
        self.wait_idle()
        self.vendor(0x91, 0x01)
        self.vendor(0x91, 0x31)
        for addr, _ in REGIONS:
            self.log("erase 0x%08X" % addr)
            self.command(0x41, addr, "erase 0x%08X" % addr)
        total = sum(len(b) for _, _, b in plan)
        done = 0
        for addr, _, blocks in plan:
            self.command(0x21, addr, "set address 0x%08X" % addr)
            for num, blk in blocks:
                self._out(DFU_DNLOAD, num, blk)
                self.wait_dnload_done("block 0x%X at 0x%08X" % (num, addr + (num - FIRST_BLOCK) * BLOCK))
                done += 1
                if progress:
                    progress(done, total)
        self.command(0x21, 0x08000000, "set address 0x08000000")
        self._out(DFU_DNLOAD, 0, None)
        try:
            self.get_status()  # the device resets; it may not answer
        except Exception:
            pass


def open_device(usb, force):
    devs = find_devices(usb)
    if not devs:
        raise DfuError("no DFU device 0483:DF11 found. Power the radio on while holding PTT + side key 1 "
                       "(the key just above PTT); the screen stays dark. Check the cable carries data.")
    if len(devs) > 1:
        raise DfuError("%d DFU devices found; connect only the radio" % len(devs))
    dev = devs[0]
    s1 = get_string(usb, dev, 1) or ""
    if "AnyRoad" not in s1 and not force:
        raise DfuError("string 1 is %r, not \"AnyRoad Technology\": this does not look like the radio's "
                       "bootloader (ST's ROM loader would write the encrypted image as-is). Use --force to "
                       "override." % s1)
    intfs = dfu_interfaces(dev)
    if not intfs:
        raise DfuError("device has no DFU interface")
    intf = intfs[0].bInterfaceNumber
    try:
        try:
            if dev.is_kernel_driver_active(intf):
                dev.detach_kernel_driver(intf)
        except (NotImplementedError, usb.core.USBError):
            pass
        dev.set_configuration()
        usb.util.claim_interface(dev, intf)
    except usb.core.USBError as e:
        if e.errno == 13 or "Access denied" in str(e):
            raise DfuError("access denied opening the device\n" + permission_hint())
        if e.errno == 16 or "busy" in str(e).lower():
            raise DfuError("device is busy (another program has it open)\n" + permission_hint())
        raise DfuError("cannot open the device: %s" % e)
    return dev, intf


def do_flash(enc, force):
    usb = import_usb()
    dev, intf = open_device(usb, force)
    plan = plan_blocks(enc)
    for line in describe_plan(plan, len(enc)):
        print(line)

    def progress(done, total):
        sys.stdout.write("\rwriting %d/%d blocks (%d%%)" % (done, total, done * 100 // total))
        sys.stdout.flush()
        if done == total:
            sys.stdout.write("\n")

    try:
        Dfu(dev, intf, log=print).flash(plan, progress)
    except usb.core.USBError as e:
        if e.errno == 13:
            raise DfuError("access denied during transfer\n" + permission_hint())
        raise DfuError("USB error during flashing: %s (the radio may need to be re-entered into the "
                       "bootloader and flashed again; the bootloader itself is never erased)" % e)
    finally:
        try:
            usb.util.dispose_resources(dev)
        except Exception:
            pass
    print("done; the radio resets into the new firmware")


# ---------------------------------------------------------------------------------------------------------------
# self-test

class FakeBootloader:
    """Minimal model of the bootloader for the self-test: decrypts and records flash writes."""

    def __init__(self, key):
        self.key = key
        self.state = ST_IDLE
        self.busy = 0
        self.addr = None
        self.flash = {}
        self.erased = []
        self.vendor = []
        self.exited = False

    def ctrl_transfer(self, rtype, req, value, intf, data, timeout=None):
        if rtype == 0xA1 and req == DFU_GETSTATUS:
            if self.busy:
                self.busy -= 1
                st = ST_DNBUSY
            else:
                st = self.state
            return bytes([0, 1, 0, 0, st, 0])
        assert rtype == 0x21
        if req == DFU_CLRSTATUS or req == DFU_ABORT:
            self.state = ST_IDLE
            return 0
        assert req == DFU_DNLOAD
        data = bytes(data or b"")
        self.state = ST_DNIDLE
        self.busy = 1
        if value == 0:
            if not data:
                assert self.addr == 0x08000000
                self.exited = True
            elif data[0] == 0x91:
                self.vendor.append(data[1])
                self.busy = 0
            elif data[0] == 0x41:
                self.erased.append(struct.unpack("<I", data[1:5])[0])
            elif data[0] == 0x21:
                self.addr = struct.unpack("<I", data[1:5])[0]
            else:
                raise AssertionError("unknown command %r" % data)
        else:
            assert value >= FIRST_BLOCK and len(data) == BLOCK
            dest = self.addr + (value - FIRST_BLOCK) * BLOCK
            off = dest - APP_BASE
            plain = bytes(b ^ self.key[(off + i) % KEY_LEN] for i, b in enumerate(data))
            assert dest not in self.flash
            self.flash[dest] = plain
        return len(data)


def self_test():
    import random
    rng = random.Random(1701)
    key = bytes(rng.randrange(256) for _ in range(KEY_LEN))
    fails = []

    def check(cond, msg):
        print(("ok    " if cond else "FAIL  ") + msg)
        if not cond:
            fails.append(msg)

    # encryption
    img = bytes(rng.randrange(256) for _ in range(5000))
    enc = xor_image(img, key)
    check(xor_image(enc, key) == img, "XOR round trip")
    check(enc[0] == img[0] ^ key[0] and enc[1024] == img[1024] ^ key[0] and enc[1500] == img[1500] ^ key[476],
          "key index = n mod 1024 from the image start")
    check(hashlib.sha256(key).hexdigest() not in [h for h, _ in KEYS.values()], "synthetic key is not a real key")

    # key file check
    import tempfile
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(key)
        kpath = f.name
    try:
        try:
            load_key(kpath, "dm1701")
            check(False, "wrong key refused")
        except DfuError:
            check(True, "wrong key refused")
        with open(kpath, "wb") as f:
            f.write(key[:1000])
        try:
            load_key(kpath, "dm1701")
            check(False, "short key refused")
        except DfuError:
            check(True, "short key refused")
    finally:
        os.unlink(kpath)

    # block planning
    full = 0x4000 + 0x10000 + 0x20000 + 1
    plan = plan_blocks(b"\x00" * full)
    check([p[0] for p in plan] == [0x0800C000, 0x08010000, 0x08020000, 0x08040000], "regions covered in order")
    check([(b[0][0], b[-1][0]) for _, _, b in plan[:3]] == [(2, 0x11), (2, 0x41), (2, 0x81)],
          "block numbers 2..0x11, 2..0x41, 2..0x81")
    check(len(plan[3][2]) == 1 and plan[3][2][0][1] == b"\x00" + b"\xFF" * 1023, "last block padded with 0xFF")
    plan = plan_blocks(b"\x01" * 100)
    check(len(plan) == 1 and len(plan[0][2]) == 1 and plan[0][2][0][0] == 2, "tiny image is one block 2")
    plan = plan_blocks(b"\x00" * (FLASH_END - APP_BASE))
    check(len(plan) == len(REGIONS) and all(len(b) == s // BLOCK for _, s, b in plan), "full flash plan")

    # image checks
    vt = struct.pack("<II", 0x20020000, 0x0800C1C1)
    for name, data, expect in [
        ("OutSecurityBin refused", b"OutSecurityBin\0\0" + b"\0" * 64, "refuse"),
        ("ELF refused", b"\x7fELF" + b"\0" * 64, "refuse"),
        ("image past 0x08100000 refused", vt + b"\0" * (FLASH_END - APP_BASE), "refuse"),
        ("image filling flash exactly accepted", vt + b"\0" * (FLASH_END - APP_BASE - 8), "ok"),
    ]:
        try:
            check_image(data, "md380")
            check(expect == "ok", name)
        except DfuError:
            check(expect == "refuse", name)
    small = vt + b"\0" * 1000
    check(check_image(small, "dm1701") == [], "small dm1701 image: no warnings")
    over = bytearray(vt + b"\0" * (0x080E3B00 - APP_BASE))
    check(any("codec window" in w for w in check_image(bytes(over), "dm1701")),
          "dm1701 image into the codec window without signature warns")
    struct.pack_into("<I", over, 0x080E3A74 - APP_BASE, CODEC_SIG)
    check(not any("codec window" in w for w in check_image(bytes(over), "dm1701")),
          "dm1701 image with merged codec signature does not warn")
    check(not check_image(bytes(over[:0x080C0000 - APP_BASE]), "dm1701"), "dm1701 image filling sectors 3-9: no warnings")
    check(any("codec window" in w for w in check_image(bytes(over[:0x080E3A76 - APP_BASE]), "dm1701")),
          "image ending inside the signature word warns")
    check(any("stack pointer" in w for w in check_image(b"\xFF" * 64, "md380")), "bad vector table warns")

    # protocol against a fake bootloader
    img = bytes(rng.randrange(256) for _ in range(0x4000 + 0x10000 + 3000))
    fake = FakeBootloader(key)
    Dfu(fake, 0, log=lambda *a: None, sleep=lambda s: None).flash(plan_blocks(xor_image(img, key)))
    check(fake.vendor == [0x01, 0x31], "vendor commands 0x91 0x01 then 0x91 0x31")
    check(fake.erased == [a for a, _ in REGIONS], "erase list")
    written = b"".join(fake.flash[a] for a in sorted(fake.flash))
    check(written[:len(img)] == img and set(written[len(img):]) <= {0xFF ^ k for k in key},
          "fake bootloader decrypts the stream back to the image")
    check(sorted(fake.flash) == [APP_BASE + i * BLOCK for i in range(len(fake.flash))], "contiguous from 0x0800C000")
    check(fake.exited, "exit: set address 0x08000000 + zero-length DNLOAD")

    print("self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                          "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


# ---------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="Flash an application image to a TYT/Baofeng STM32 radio through the "
                                             "stock AnyRoad DFU bootloader.",
                                 epilog=EPILOG, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="plain application image (.bin, linked for 0x0800C000)")
    ap.add_argument("--model", help="radio model: " + ", ".join(sorted(ALIASES)))
    ap.add_argument("--key", help="1024-byte key file for the model (checked by SHA-256)")
    ap.add_argument("--dry-run", action="store_true", help="check, encrypt and print the plan; no USB")
    ap.add_argument("-o", "--output", help="with --dry-run: write the encrypted image here")
    ap.add_argument("--list", action="store_true", help="list DFU devices 0483:DF11 and their strings")
    ap.add_argument("--self-test", action="store_true", help="run the built-in tests (no USB, synthetic key)")
    ap.add_argument("--force", action="store_true",
                    help="flash even if string 1 is not \"AnyRoad Technology\" or the image has warnings")
    args = ap.parse_args(argv)

    try:
        if args.self_test:
            return self_test()
        if args.list:
            return list_devices()
        if not args.image or not args.model:
            ap.error("an image and --model are required (or --list / --self-test)")
        model = ALIASES.get(args.model.lower().replace("-", "").replace("_", ""))
        if not model:
            ap.error("unknown model %r; known: %s" % (args.model, ", ".join(sorted(ALIASES))))
        if not args.key and not args.dry_run:
            ap.error("--key is required to flash")
        if args.output and not args.dry_run:
            ap.error("--output is only used with --dry-run")

        with open(args.image, "rb") as f:
            data = f.read()
        warnings = check_image(data, model)
        man = os.path.splitext(args.image)[0] + ".json"
        if os.path.exists(man):
            # a DM-1701 release from before the codec moved packed two flash ranges (application + sector 9 data) into
            # the one file; this tool writes one contiguous application, so it would put the data over sectors 3-6
            try:
                with open(man) as f:
                    nranges = len(json.load(f).get("ranges", []))
            except (OSError, ValueError):
                nranges = 0
            if nranges > 1:
                raise DfuError("%s lists %d flash ranges (application and sector 9 data in one file); this tool writes "
                               "one contiguous image. Use tools/odrflash" % (os.path.basename(man), nranges))
        for w in warnings:
            print("warning: " + w, file=sys.stderr)
        print("image %s: %d bytes, 0x%08X..0x%08X, model %s, SHA-256 %s"
              % (args.image, len(data), APP_BASE, APP_BASE + len(data), model, hashlib.sha256(data).hexdigest()))

        if args.key:
            key = load_key(args.key, model)
            enc = xor_image(data, key)
        else:
            print("no --key: plan only, nothing encrypted")
            enc = data
        if args.dry_run:
            for line in describe_plan(plan_blocks(enc), len(enc)):
                print(line)
            if args.output:
                if not args.key:
                    ap.error("--output needs --key")
                with open(args.output, "wb") as f:
                    f.write(enc)
                print("wrote encrypted image to %s" % args.output)
            return 0
        if warnings and not args.force:
            raise DfuError("image has warnings (see above); fix them or pass --force")
        do_flash(enc, args.force)
        return 0
    except DfuError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    except OSError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
