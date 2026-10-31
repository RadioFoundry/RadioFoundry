#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""stubtool: flash access for RadioFoundry-supported radios through a RAM monitor (the stub). Today: read the program flash and the data flash of an HR_C7000 radio (DM-32UV, DM-1801 Pro) through the chip's
serial boot ROM, every reply checked, both memories in one power-on. Protocol, history and the stub's source:
stub/USER-GUIDE.md in the RadioFoundry repository.

    stubtool.py PORT --progmem progmem.bin --q128 q128.bin     both memories, one boot (1 MB + 16 MB)
    stubtool.py PORT --q128 q.bin --q128-range 0x7C000:0x7C200   part of one (any byte range)
    stubtool.py PORT --verify-q128 q128.bin                      compare flash with a file by CRC, no data transferred
    stubtool.py PORT --rfoundry-enter --progmem p.bin --q128 q.bin     the same from a radio running RadioFoundry (built with CONFIG_STUB_ENTRY=y)
    stubtool.py PORT --rfoundry-enter --write-app rfoundry_dm1801pro.bin   program an RadioFoundry build into the program flash (changed sectors only)
    stubtool.py PORT --attach --boot reset                      restart the radio into its firmware, no power cycle
    stubtool.py PORT --probe                                    chip IDs and a few registers, no files
    stubtool.py PORT --peek 0x14020000:16                       read radio memory (registers, SRAM, flash window)
    stubtool.py PORT out.bin --mem q128                         the old form: one memory into out.bin
    stubtool.py --self-test                                     against a simulated radio, no hardware

Radio OFF, cable in, run the script, then switch the radio ON within --wait seconds (or --rfoundry-enter with the radio running RadioFoundry).
Reads never write. Only --write-app (the application area from 0xC000, never the bootloader) and --write-test (two reserved
test sectors per chip) write flash. The monitor the script loads into the radio's RAM is embedded below (source: the stub's own repository folder), so this file
needs only Python 3 and pyserial.
"""
import argparse
import base64
import hashlib
import os
import re
import sys
import time
import zlib

# the board registry (tools/boards.py generates it): the boards with a stub profile, their pins and memories
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools", "rfoundry"))
from rfoundry import boards as rfoundryboards  # noqa: E402
from rfoundry import protect  # noqa: E402  (protected flash regions, board.toml [[flash.protect]])

# --- STUB begin (stub/embed.py rewrites everything up to the end marker)
STUB_SHA256 = "053a6b645d122dc85443616ed622c3a91229d8f949fc5e07aec68d14a8e4cbe1"
STUB_B64 = (
    "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"
)
# --- STUB end
# --- STUB RadioFoundry begin (stub/embed.py rfoundry rewrites everything up to the end marker)
STUB_RFOUNDRY_SHA256 = "a014d3b1ad500448f3a26702ffc5025384339648feb61d768440e37aa3c2db7b"
STUB_RFOUNDRY_B64 = (
    "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"
)
# --- STUB RadioFoundry end
# --- STUB DB25D begin (stub/embed_stub.py rewrites everything up to the end marker)
STUB_DB25D_SHA256 = "08a1b669c42ec028131f2011c70b47e1867107e0d3058f3d8148559d54a9b9e4"
STUB_DB25D_B64 = (
    "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"
    "o+mlHVlKADTKB5DKED8eQBpEiiMNI11MYI+5tGEjjZqXwm9wjXhLGnVfE/ZxJvTf9glyZ4d/nXzcXRtWY73jeYnafy5K9vW4gJUZ"
    "6SdhlU9GLuBIxq0RrPPJyBkcSU6MSDtI/9Q4KCYg1pH8UbEu7LG7HElo0/pwDmIXsQKbsKiDIcjwLbuYjjVlbCv3z5VfvZAs2FCC"
    "07hjCdj8L8qauAo18qv9yYLYTpv3SJUNU/rk8KuYrSNb1VgL2uL8xKuYH/cPqVA5CsZkv5lrqpvHewpXlS7JyEZp8iY0chVG3npn"
    "SWdTHYZQZ4tEd5Z11vkZ66u1f9I7/PP8ROrN40Sys8mt/rM6h/wnlkh4ntVmXGaJp+nhpI5Cf5/U70P8YbBq+EFfWkaZK+qu659M"
    "18TvA1K1nXQvrk3nGxHLpGNgrzmGdfYNBjFhQ7r4Wkl77y5e1B7bcyxPZ26T2nT9NPqZ2CHKtW3Ce64O8v1LWykzJVxkv5BHE7Y/"
    "rucwOj4ZF4bUPNq9tj8ZcpR/V5OVsPqjDaXwUX0jegZVVBt+X38n+t+/jhNrNoRr/dVhyd8is2ExXJjW4J+Vths1Oz5uQp57p6S4"
    "aqgJZzftxS8Okhsa8Lkl0gIL0xzx7UKruDq8vfDALp9ojuZZVq/QIfZqomPon59PddTt5KiRfKGygZxQ43eFZxslvxguFhv85X6d"
    "H78dx7/2b7f37Tojbg3X9C/H/PvB+GZZg/kpbapxSAebZS2+66f0+I2yGTWvm8rdQZ6aqa3+zTIdHebV2NRNfEOu8ZsvoVRpt+vN"
    "U+ZbNafzNl4T3x7LJP5dM7Rd+KXxVPUVWR9djCdsDZMcv9UPlvIiEv8/6Lfb+RdMrkj8K/5KU2v8Bl986H0pne+oYzgicwij8t/G"
    "a/wddTUJzewwwLB26or7LbbYt1muPi0AIBIcvbkWv4pKb38VlY2TnTEtJmSQU2k4i3SPgYaXkdYbUTakp9IxDpGeQ3oG+3Ek8pOM"
    "H/6sV2gqej+/33qgZOCZ7x9sbKxydd2/8p+064WW8DdlXsO14XfOnt0fdJ0MfCK9eeYvX7Q1X07umPPXjz5S8UfhHoSE98qgYz/6"
    "DvmjgPSRPO+mU/8Hwb0j5Q=="
)
# --- STUB DB25D end
# --- STUB DB25D_DMR begin (stub/embed_stub.py rewrites everything up to the end marker)
STUB_DB25D_DMR_SHA256 = "bcff18981ddf6e08a4a8002dff6224000353a8438bba094d416f66fb95292dba"
STUB_DB25D_DMR_B64 = (
    "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"
    "vkP+KCBjbJ53Z1D/D0CrRrU="
)
# --- STUB DB25D_DMR end
# --- STUB UNIV begin (stub/embed_stub.py rewrites everything up to the end marker)
STUB_UNIV_SHA256 = "eae5b98615d67ba465061c84a5fb209a902333dc700671ce4ca69a05eb4aaee9"
STUB_UNIV_B64 = (
    "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"
)
# --- STUB UNIV end

BLOCK = 256
CHUNK = 1024                        # bytes per read request
SECTOR = 4096                       # the unit the stub checksums and the host fetches
WINDOW = 16                         # sectors per checksum request
PACK = 16                           # sectors per read request; the stub streams one frame per sector
BLANK_CRC = zlib.crc32(b"\xff" * SECTOR)
LOAD_ADDR = 0x10000                 # where the ROM puts the stub and where it starts it
SSI_PROG = 0x04000000               # the SSI that talks to the program flash (chip select is the SSI's own, SER)
SSI_DATA = 0x140A0000               # SPI0, the data flash
GPIOA = 0x14020000
IO_DIPLEX1 = 0x11000038
SYS_SOFT_RSTN = 0x11000000
QUAD_ENABLE = 0x1100005C
LCSFC_BAUDR = 0x11000030            # XIP flash SCLK divider; a soft reset keeps it
WDT_LOCK = 0x14010000               # watchdog: unlock key, then EN = 0, then lock (hardware-dm32.md 3.5)
WDT_EN = 0x14010010
WDT_KEY = 0x5ADA7200
CRASH_SOFT_MARK = 0x18007EF4        # RadioFoundry's restart-cause words (crash-log.md 1.3): HRC_CRASH_BOOT + 4, crash_boot_t.soft
CRASH_BOOT_SOFT = 0x54464F53        # "SOFT": the reset was asked for, so RadioFoundry's next start doesn't take it for the watchdog
TIMER2_VALUE = 0x14000018           # timer 2 current value (counts down; the stub runs it free)
LED_RED, LED_GREEN = 33, 32         # PTB1 = red, PTB0 = green on the DM-1801 Pro and the DM-32UV (both checked on the radio)
OK, BADSUM, BADOP, SPITIMEOUT, TOOLONG, NOWEL = 0x05, 0x0A, 0x0B, 0x0C, 0x0E, 0x0F
WRITE_VERSION = 7                   # the stub has op 0E (WRITE): a sector erased, programmed and read back in one request,
                                    # and goes back to its home rate by itself when a raised rate's link goes quiet
# Raised rates (protocol 7): the UART has whole divisors only, so the stub offers clock / (16 * d). Auto tries those in
# this span, a binary search for the fastest that carries full frames both ways, and remembers the result per port and
# radio. LINK_WINDOW: the provisional window, and how long a raised link may stay quiet before the stub goes home
FAST_MIN, FAST_MAX, LINK_WINDOW = 200000, 1100000, 2.5
FAST_FALLBACK = 30                  # link score at which a raised rate is given up: +10 per damaged frame, -1 per clean one
PROOF_ROUNDS = 4                    # set_baud's full frames each way before a rate is kept
BAUD_CACHE = os.path.join(os.path.expanduser("~"), ".cache", "rfoundry", "stub-baud.json")
# memory: (SSI base, SER, GPIO chip-select pin or None, size, name). The data flash's chip select is the GPIO PTA18
# (hardware-dm32.md 10; dm1801pro-hardware.md 6.1); the SSI's own select is not wired to the flash on its own.
MEM = {"progmem": (SSI_PROG, True, None, 0x100000, "program flash"),
       "q128": (SSI_DATA, True, 18, 0x1000000, "data flash"),
       # the DB25-D's SPI NAND on the same bus and chip select (hardware-db25d.md 6.5): 2 KB pages without the spare
       # bytes, read by the universal stub's NAND path (flags bit 2), two pages per 4 KB sector; never written here
       "nand": (SSI_DATA, True, 18, 0x10000000, "NAND")}
NAND_FLAG = 4


# Writes are only allowed here (chosen 2026-10-15 from the dumps: blank in every dump, away from RadioFoundry)
WRITE_OK = {"progmem": [(0xF0000, 0xF4000)], "q128": [(0xC00000, 0xC04000)]}
PAGE = 256
# The application area of the program flash: an RadioFoundry image is linked for 0x0300C000, which is flash offset 0xC000. The
# bootloader below it (0..0xC000) is never written, so the radio can always be put in update mode by hand.
APP_START, APP_END = 0xC000, 0x100000
# The first word of RadioFoundry's first stage (boot/README.md), at APP_START on a radio that has one (the DB25-D). Its
# application is linked for 0x0300D000, so the image written at APP_START is the install image, which starts with it
STAGE_MAGIC = bytes.fromhex("2c1101c0")


class DumpError(Exception):
    pass


class FrameError(DumpError):
    """A damaged, short or missing frame, or a sector that failed its CRC: worth asking again."""


def stub_image():
    blob = zlib.decompress(base64.b64decode("".join(STUB_B64)))
    if hashlib.sha256(blob).hexdigest() != STUB_SHA256:
        raise DumpError("the embedded stub fails its own checksum")
    return blob


def _unpack(b64, sha, what):
    blob = zlib.decompress(base64.b64decode("".join(b64)))
    if hashlib.sha256(blob).hexdigest() != sha:
        raise DumpError("the embedded %s stub fails its own checksum" % what)
    return blob


def stub_rfoundry_image():
    """The stub linked for a running RadioFoundry on the DM-32UV / DM-1801 Pro (RAM at 0x17000, 84 MHz clock), for --rfoundry-enter."""
    return _unpack(STUB_RFOUNDRY_B64, STUB_RFOUNDRY_SHA256, "RadioFoundry")


def rfoundry_stubs():
    """The stubs embedded for --rfoundry-enter, by the load address the running firmware reports in STUB INFO (the
    monitor is position dependent, so the image has to be linked for exactly the span RadioFoundry offers): {base: (name, image)}.
    0x17000 DM-32UV / DM-1801 Pro; 0x30000 DB25-D; 0x1D000 DB25-D built with CONFIG_DMR=y (boards/db25d/config.mk)."""
    out = {0x17000: ("DM-32UV / DM-1801 Pro", stub_rfoundry_image())}
    for base, name, b64, sha in ((0x30000, "DB25-D", "STUB_DB25D_B64", "STUB_DB25D_SHA256"),
                                 (0x1D000, "DB25-D with DMR", "STUB_DB25D_DMR_B64", "STUB_DB25D_DMR_SHA256")):
        if b64 in globals():
            out[base] = (name, _unpack(globals()[b64], globals()[sha], name))
    return out


def stub_link(image):
    """(stack top, bss start, bss end) a stub image was linked with, read from the literal pool after its entry code
    (hrc7000/start.S: psrclr ie first, the three addresses at 0x28..0x30), or None if this is not such an image."""
    if len(image) < 0x34 or image[:4] != b"\x80\xc0\x20\x70":
        return None
    return tuple(int.from_bytes(image[o:o + 4], "little") for o in (0x28, 0x2C, 0x30))


def check_stub_fits(image, load, size):
    """Refuse a stub that was not linked for the span RadioFoundry offers: jumping into one built for another base runs wild
    (the radio then needs a power cycle). The image's bss and stack have to lie inside [load, load + size)."""
    link = stub_link(image)
    if link is None:
        raise DumpError("this does not look like a stub image (no HR_C7000 monitor entry code at its start)")
    top, bss0, bss1 = link
    # the raw image is the stub's text and data and ends where its bss starts, which pins the base it was linked for
    if not (load <= bss0 <= bss1 <= top <= load + size and bss0 - load - len(image) in range(4)):
        raise DumpError("this stub image is linked for a different RAM span (its data and stack are at 0x%X-0x%X) than the "
                        "0x%X-0x%X RadioFoundry offers. Use the image built for that base (see stub/README.md)"
                        % (bss0, top, load, load + size))


SRAM = (0x10000, 0x58000)               # the HR_C7000's main SRAM, 288 KB (hardware-dm32.md 2; the same on the DB25-D)


def stub_univ_image():
    """The universal stub: one image for every HR_C7000 radio and every way in (boot ROM, the DB25-D's first stage, a
    running RadioFoundry, a stock DB25-D through its console). It copies itself to its run address and asks the host for the
    board's values (CONFIG) after HELLO. stub/README.md has the RAM table."""
    return _unpack(STUB_UNIV_B64, STUB_UNIV_SHA256, "universal")


def univ_layout(image):
    """(run address, end of its RAM) of a universal stub image, read from its parameter block ('UCFG' at offset 0x80,
    layout 2 or later), or None for a position-dependent stub. The stub uses exactly [run, end): image, .bss, stack."""
    if len(image) < 0xC0 or image[0x80:0x84] != b"UCFG":
        return None
    ver, size = int.from_bytes(image[0x84:0x86], "little"), int.from_bytes(image[0x86:0x88], "little")
    if ver < 2 or size < 0x40:
        return None
    return int.from_bytes(image[0xB8:0xBC], "little"), int.from_bytes(image[0xBC:0xC0], "little")


def check_univ_load(image, load, window=None):
    """Refuse a load of the universal stub that could go wrong, before anything is sent: its run area [run, end) must be
    inside SRAM; the image must land 4-byte aligned inside SRAM (and inside `window` = (base, size) when the firmware
    offers one); and unless it is loaded at its run address, the loaded copy must not overlap the copy the entry code
    makes (the stub would stop at its entry rather than copy over itself)."""
    lay = univ_layout(image)
    if lay is None:
        raise DumpError("this is not a universal stub image (no parameter block at offset 0x80)")
    run, end = lay
    n = len(image)
    if not (SRAM[0] <= run and run + n <= end <= SRAM[1]):
        raise DumpError("the stub's RAM 0x%X-0x%X is not inside SRAM 0x%X-0x%X" % (run, end, SRAM[0], SRAM[1]))
    if load % 4 or not (SRAM[0] <= load and load + n <= SRAM[1]):
        raise DumpError("load address 0x%X: the %d-byte stub must land 4-byte aligned inside SRAM" % (load, n))
    if window and not (window[0] <= load and load + n <= window[0] + window[1]):
        raise DumpError("the %d-byte stub does not fit the 0x%X-0x%X the firmware offers" % (n, window[0], window[0] + window[1]))
    if load != run and load < run + n and run < load + n:
        raise DumpError("load address 0x%X overlaps the stub's run address 0x%X: load it at 0x%X or at least %d bytes away"
                        % (load, run, run, n))
    return run, end


# Board profiles for the universal stub (CONFIG, op 0x0D), from each board's [flash.stub] (boards/<id>/board.toml):
# board id echoed by HELLO, power latch / power key pins (255 = none), whether the data memory is the SPI NAND, the
# program flash size. VERSION's board= names them on the RadioFoundry route; --board at power-on. Unknown radio: GENERIC (no
# pins, nothing echoed but 0).
C7_BOARDS = {b.id: {"id": b.stub["id"], "name": b.model, "names": tuple(b.debuglink_names), "latch": b.stub["latch"],
                    "key": b.stub["key"], "nand": b.stub["nand"], "progmem": b.stub["progmem"], "protect": b.protect}
             for b in rfoundryboards.BOARDS.values() if b.stub and b.stub["tool"] == "stubtool"}
GENERIC = {"id": 0, "name": "HR_C7000 radio", "names": (), "latch": 255, "key": 255, "nand": False, "progmem": 0x100000,
           "protect": []}
# This tool's memory names as board.toml [[flash.protect]] names them
PROTECT_MEM = {"progmem": "progmem", "q128": "data", "nand": "nand"}
CFG_UART_THIS = 3                        # CONFIG uart field: answer only on the UART the CONFIG came in on


def image_board_check(image, board):
    """Why a RadioFoundry image is not for the C7_BOARDS board `board`, or None. Every RadioFoundry build carries its
    board's name (board_t .name, the debug link's board=) as a C string, so an image without the target's is another
    radio's. Images that aren't RadioFoundry (a stock firmware being put back) and unknown boards are not judged."""
    prof = C7_BOARDS.get(board)
    if prof is None or not prof["names"] or b"RadioFoundry" not in image:
        return None
    if any(n.encode() + b"\0" in image for n in prof["names"]):
        return None
    other = [b["name"] for k, b in sorted(C7_BOARDS.items()) if k != board and any(n.encode() + b"\0" in image for n in b["names"])]
    return "this RadioFoundry image is %s, not for the %s" % ("for the " + ", ".join(other) if other else "for another radio",
                                                              prof["name"])


def c7_board(name):
    """The C7_BOARDS key for a board id or a VERSION board= name, or None."""
    for bid, b in C7_BOARDS.items():
        if name == bid or name in b["names"]:
            return bid
    return None


def crc16_ccitt(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


STUB_MAGIC = 0x42533743                 # 'C7SB': the guard word of RadioFoundry's STUB command (docs/spec/debuglink.md 3.18)
DL_STUB = 0x1A


def lz4_block(src, size):
    """Decode one LZ4 block (the open LZ4 block format) to exactly `size` bytes. Our own decoder, no library."""
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0 or off > len(out):
            raise DumpError("corrupt LZ4 block")
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:
            for k in range(ml):
                out.append(out[start + k])
    if len(out) != size:
        raise DumpError("LZ4 block decodes to %d bytes, wanted %d" % (len(out), size))
    return bytes(out)


def parse_led_spec(spec):
    """"255:0:300,0:255:300" = red 255 + green 0 for 300 ms, then red 0 + green 255 for 300 ms. Prefix `repeat;` or
    `hold;` for those flags. Returns (steps, repeat, hold)."""
    flags, _, body = spec.rpartition(";")
    steps = []
    for item in body.split(","):
        try:
            r, g, ms = (int(x, 0) for x in item.split(":"))
        except ValueError:
            raise DumpError("LED step %r: want RED:GREEN:MS, e.g. 255:0:300" % item)
        if not (0 <= r <= 255 and 0 <= g <= 255 and 0 <= ms <= 65535):
            raise DumpError("LED step %r out of range" % item)
        steps.append((r, g, ms))
    return steps, "repeat" in flags, "hold" in flags


# played when a run finishes (--done-led): three green flashes, then the yellow status blink resumes
DONE_LED = "0:255:150,0:0:150,0:255:150,0:0:150,0:255:150,0:0:150"


def sum8(b):
    return sum(b) & 0xFF


ROM_ACK = b"\x02\x05\x00\x03"
ROM_HELLO, ROM_REPLY = b"\x02\x24\x00\x03", b"\x02\x14\x00\x03"
# The boot ROM stops waiting (about 48 ms after its greeting when no answer came) and starts the firmware; its NAK is
# also how it answers a repeated greeting reply once it took the first one (docs/spec/hardware-dm32.md 13.3)
ROM_GAVE_UP, ROM_NAK = b"\x00\x10\x00\x03", b"\x02\x0a\x00\x03"
# --slow-cable: the greeting reply goes out this often while the radio is switched on, and on for ROM_REPEAT_AFTER once
# the greeting is seen: the ROM drops what reaches it before its greeting, so only the answers sent after that count
ROM_REPEAT, ROM_REPEAT_AFTER = 0.03, 0.15


def rom_cmd(op, payload=b""):
    """02 op chk 03 payload; chk = sum of the payload. The ROM's own framing."""
    return bytes([0x02, op, sum8(payload), 0x03]) + payload


class Radio:
    """port: write(bytes), read(n) (what arrived within its timeout), reset_input_buffer(), a timeout attribute."""

    def __init__(self, port, log=print, retries=6, cable=""):
        self.port = port
        self.log = log
        self.cable = cable              # the programming cable's chip, named in a boot ROM error
        self.echo = False               # the cable sends back what the host sends (seen in the boot ROM load)
        self.retries = retries
        self.retried = 0
        self.stub_head = b""
        self.stub_base = LOAD_ADDR
        self.v6 = None                  # the universal stub's HELLO fields (protocol 6), None for an older stub
        self.sector_op = False          # protocol 7: whole-sector writes (op 0E); else one SPI request per step
        self.home_baud = None           # the rate before the first switch: where a lost raised link comes back to
        self.link_window = LINK_WINDOW  # the stub's quiet limit at a raised rate, as last sent with BAUD
        self.link_score = 0
        self.bad_rates = set()          # raised rates given up in this session
        self.uart_hz = None             # the clock CONFIG sent (the stub's rates are worked out from it)
        self.configured = None          # the board profile the stub confirmed after CONFIG
        self.profile = GENERIC
        self.slow = False               # --slow-cable: the boot ROM's greeting is answered ahead (wait_for_radio)

    def set_timeout(self, t):
        """Only when it changes: on macOS pyserial re-applies the terminal settings on every timeout assignment, which
        drops a custom baud rate (found 2026-10-15: every rate that is not a standard one stopped working)."""
        if self.port.timeout != t:
            self.port.timeout = t

    def read_exact(self, n, timeout):
        end = time.monotonic() + timeout
        out = b""
        self.set_timeout(0.02)
        while len(out) < n:
            chunk = self.port.read(n - len(out))
            if chunk:
                out += chunk
            elif time.monotonic() >= end:
                break
        return out

    # ---- boot ROM: wait for it, load the stub, start it
    def rom_ack(self, sent, timeout):
        """The boot ROM's acknowledgement of `sent`: (ok, got). A cable that echoes (some USB-serial cables loop TX back
        to RX) puts an exact copy of the frame first: it is dropped and the reading goes on. Other bytes ahead of the ack (a
        damaged echo, seen on a CH340 cable) are passed over: the ack is the last thing the ROM sends, so it counts when
        the bytes end with it. A NAK or the ROM giving up ends the wait. got: what came instead."""
        end = time.monotonic() + timeout
        buf = b""
        while True:
            if buf[:4] == ROM_ACK:
                return True, b""
            n = min(len(buf), len(sent))
            echo = n > 0 and buf[:n] == sent[:n]
            if echo and len(buf) >= len(sent):
                buf = buf[len(sent):]
                if not self.echo:
                    self.log("the cable echoes what the host sends; the echo is ignored")
                self.echo = True
                continue
            if not echo and buf[-4:] == ROM_ACK:
                self.log("before the boot ROM's acknowledgement: %s" % buf[:-4][:16].hex(" "))
                return True, b""
            if not echo and (buf[-4:] == ROM_NAK or ROM_GAVE_UP in buf):
                return False, buf[:16]
            if time.monotonic() >= end:
                return False, buf[:16]
            want = len(sent) - len(buf) if echo else 4 - len(buf)
            buf += self.read_exact(max(1, want), max(0.0, min(0.05, end - time.monotonic())))

    def rom_error(self, what, got):
        bytes_ = "received %s" % got.hex(" ") if got else "nothing received"
        cable = "the %s programming cable" % self.cable if self.cable else "the programming cable"
        return DumpError("%s (%s%s). The radio's boot ROM did not take the loader over %s. Try a cable with an FTDI, "
                         "CP210x or PL2303 chip, %s, or use the stock firmware file instead of reading the radio"
                         % (what, bytes_, "; the cable echoes" if self.echo else "", cable,
                            "the radio's factory update mode" if self.slow else "--slow-cable"))

    def rom_late(self, got):
        return DumpError("the radio's boot ROM stopped waiting before the answer to its greeting reached it (received %s). %s"
                         % (got[:16].hex(" "), "It did so even with --slow-cable" if self.slow
                            else "The cable or a USB hub carries it too slowly: run again with --slow-cable"))

    def wait_for_radio(self, wait, slow=False):
        """Wait for the boot ROM's greeting. The ROM waits only about 48 ms for the answer; slow (--slow-cable): send the
        answer every ROM_REPEAT s all along, and on for ROM_REPEAT_AFTER after the greeting, so the ROM's window is
        covered however late the greeting shows here. Only with the radio off at the start: the firmware of a radio left
        on would get it too. True when the ROM took one of those (a NAK of a later one shows it, or the greeting and
        ROM_REPEAT_AFTER of answers after it without the ROM giving up)."""
        end = time.monotonic() + wait
        nxt = 0.0
        heard = 0.0
        last = b""
        self.set_timeout(0.005 if slow else 0.1)
        while time.monotonic() < end:
            if slow and time.monotonic() >= nxt:
                nxt = time.monotonic() + ROM_REPEAT
                self.port.write(ROM_REPLY)
            if heard and time.monotonic() - heard >= ROM_REPEAT_AFTER:
                return True
            b = self.port.read(1)
            if not b:
                continue
            last = (last + b)[-4:]
            if last == ROM_HELLO:
                if not slow:
                    return False
                heard = time.monotonic()
            elif slow and last == ROM_NAK:
                return True
            if last == ROM_GAVE_UP:
                raise DumpError("the radio started its firmware%s" % (" without taking the loader, even with --slow-cable" if slow
                                else ": its boot ROM's greeting did not come through. Run again with --slow-cable"))
        raise DumpError("the radio did not start its serial boot ROM within %d s: radio off and cable in before the "
                        "script starts, then switch it on" % wait)

    def attach(self):
        """Talk to a stub that is already running (a previous run left the radio on): no ROM load, no power cycle."""
        info = None
        for rate in (None, 921600, 460800, 230400, 750000, 500000, 375000, 250000, 115200):       # a stub left at a faster rate by an earlier run
            try:
                if rate:
                    self.set_timeout(0.02)
                    self.port.baudrate = rate
                    time.sleep(0.05)
                self.drain(0.3)
                info = self.hello()
                break
            except DumpError:
                continue
        if info is None:
            raise DumpError("no stub answers: power-cycle the radio and run without --attach")
        self.log("attached to stub v%d (max payload %d, SSI burst %d / %d bytes)"
                 % (info[0], info[1], self.info >> 8 & 0xFF, self.info & 0xFF))

    def start(self, wait=120, stub=None, load=LOAD_ADDR, slow=False):
        """Boot a stub through the serial boot ROM at power-on, written to and started at load (the DB25-D's first
        stage answers the same way). Default: the universal stub at 0x10000, which copies itself to its run address;
        a position-dependent stub (--legacy-stub, --stub FILE) must be linked for `load` (--load-base)."""
        stub = stub_univ_image() if stub is None else stub
        if univ_layout(stub):
            check_univ_load(stub, load)
            self.stub_head = stub[:0x80]            # the entry code: the same at the load address and the run address
        else:
            self.stub_head = stub[:CHUNK]
        stub += b"\xFF" * (-len(stub) % BLOCK)
        self.stub_base = load
        self.slow = slow
        answered = self.wait_for_radio(wait, slow)
        if not answered:
            self.port.write(ROM_REPLY)
        # the ROM's reply to it isn't needed; an echo shows the cable loops back. After --slow-cable the NAKs of the answers
        # still on their way come in too: they must be gone before block 0. The ROM has no timeout once it took the answer
        time.sleep(0.2 if answered else 0.05)
        reply = self.read_exact(256, 0.02)
        if ROM_GAVE_UP in reply:
            raise self.rom_late(reply)
        if ROM_REPLY in reply:
            self.echo = True
            self.log("the cable echoes what the host sends; the echo is ignored")
        if reply:
            self.log("after the boot ROM greeting: %s" % reply[:16].hex(" "))
        blocks = len(stub) // BLOCK
        self.log("boot ROM answered; loading %d-byte stub (%d blocks)" % (len(stub), blocks))
        for i in range(blocks):
            payload = (load + i * BLOCK).to_bytes(4, "big") + b"\x00\x00\x00\x40" + stub[i * BLOCK:(i + 1) * BLOCK]
            got = b""
            for attempt in range(3):             # a block is a write to a fixed address: sending it again is safe
                self.port.reset_input_buffer()
                frame = rom_cmd(0x18, payload)
                self.port.write(frame)
                ok, got = self.rom_ack(frame, 1.0)
                if ok:
                    break
                if ROM_GAVE_UP in got:
                    raise self.rom_late(got)
                self.log("stub block %d not acknowledged (%s), sending it again"
                         % (i, "received %s" % got.hex(" ") if got else "nothing received"))
            else:
                raise self.rom_error("stub block %d was not acknowledged" % i, got)
        self.port.reset_input_buffer()
        go = rom_cmd(0x1B, load.to_bytes(4, "big"))
        self.port.write(go)
        ok, got = self.rom_ack(go, 1.0)
        if not ok:
            raise self.rom_error("the start command was not acknowledged", got)
        end = time.monotonic() + 3
        while True:
            try:
                info = self.hello()
                break
            except DumpError:
                if time.monotonic() > end:
                    raise DumpError("the stub was started but does not answer")
        self.log("stub v%d running (max payload %d, SSI burst %d / %d bytes)"
                 % (info[0], info[1], self.info >> 8 & 0xFF, self.info & 0xFF))

    # ---- stub frames: 02 op len(2) chk 03 payload  ->  02 status len(2) chk 03 payload
    def exchange(self, op, payload=b"", timeout=1.0):
        """One frame out, one reply in: (status, body), or (None, why) when the link damaged or lost it."""
        frame = bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, sum8(payload), 0x03]) + payload
        self.port.reset_input_buffer()
        self.port.write(frame)
        hdr = self.read_exact(6, timeout + len(frame) * 10 / self.rate())
        if len(hdr) != 6 or hdr[0] != 0x02 or hdr[5] != 0x03:
            return None, "no or malformed reply header"
        n = (hdr[2] << 8) | hdr[3]
        body = self.read_exact(n, 0.5 + n * 0.0002)
        if len(body) != n:
            return None, "reply short (%d of %d bytes)" % (len(body), n)
        if sum8(body) != hdr[4]:
            return None, "reply checksum"
        if hdr[1] == BADSUM:
            return None, "the radio saw a bad request checksum"
        return hdr[1], body

    def rate(self):
        return getattr(self.port, "baudrate", 0) or 115200

    def raised(self):
        """At a raised rate the stub goes home by itself when the link is lost (protocol 7), so this side can too. Not
        while set_baud is proving a rate: a failure there has its own way back (the provisional window)."""
        return (self.sector_op and self.home_baud is not None and self.rate() != self.home_baud
                and not getattr(self, "switching", False))

    def request(self, op, payload=b"", retries=None, timeout=1.0):
        retries = self.retries if retries is None else retries
        why = ""
        for attempt in range(retries + 1):
            if self.raised() and self.link_score >= FAST_FALLBACK:
                self.fall_back(why or "damaged frames")
            status, body = self.exchange(op, payload, timeout)
            if status == OK:
                self.link_score = max(0, self.link_score - 1)
                return body
            if status is not None:
                raise DumpError("stub op 0x%02X refused: status 0x%02X" % (op, status))
            why = body
            self.retried += 1
            if self.raised():
                self.link_score += 10
            if attempt < retries:
                self.log("op 0x%02X: %s, again (%d of %d)" % (op, why, attempt + 1, retries))
                time.sleep(0.02)
        if self.raised():                       # the whole budget spent at a raised rate: go home and try once more there
            self.fall_back(why)
            return self.request(op, payload, retries, timeout)
        raise DumpError("op 0x%02X: no clean reply after %d tries (%s)" % (op, retries + 1, why))

    def stream_damaged(self, why):
        """A streamed read (op 09) cut short or damaged, once the rest of it is drained: scored like a damaged reply, so a
        raised rate that stops holding is given up here too (that path writes its request itself, not through request()).
        True when the link went home, and the caller's retry budget starts again there."""
        if not self.raised():
            return False
        self.link_score += 10
        if self.link_score < FAST_FALLBACK:
            return False
        self.fall_back(why)
        return True

    def fall_back(self, why):
        """Give up a raised rate with nothing but silence: the stub goes back to its home rate on its own once the link
        has been quiet for its window (protocol 7), whatever state the link was in. Then carry on at the home rate."""
        bad = self.rate()
        self.bad_rates.add(bad)
        self.log("%d baud is not holding (%s): back to %d" % (bad, why, self.home_baud))
        self.set_timeout(0.02)
        self.port.baudrate = self.home_baud
        time.sleep(self.link_window + 0.5)
        self.port.reset_input_buffer()
        self.link_score = 0
        forget_baud(self.baud_key(), bad)
        for _ in range(3):
            status, body = self.exchange(0x01)
            if status == OK:
                return
        raise DumpError("the stub does not answer at %d baud after leaving %d: power-cycle the radio" % (self.home_baud, bad))

    def hello(self, retries=0):
        r = self.request(0x01, retries=retries)
        if r[:6] != b"C7STUB":
            raise DumpError("unexpected hello %r" % r)
        self.info = int.from_bytes(r[13:17], "big") if len(r) >= 17 else 0      # the port's facts (HR_C7000: SSI bursts)
        self.sector_op = r[6] >= WRITE_VERSION
        if len(r) >= 43 and r[6] >= 6:          # the universal stub: board, flags and its RAM footprint
            g = lambda o: int.from_bytes(r[o:o + 4], "big")
            self.v6 = {"load": g(9), "board": r[17], "flags": r[18], "run": g(19), "image": g(23), "bss_end": g(27),
                       "stack_lo": g(31), "stack_top": g(35), "stack_used": g(39)}
            self.stub_base = self.v6["run"]     # set_baud's read-back: the running copy
        else:
            self.v6 = None                      # an older stub (HELLO shorter than 19 bytes): no CONFIG
        return r[6], (r[7] << 8) | r[8]

    def footprint(self):
        """One line on the universal stub's RAM use, from HELLO."""
        v = self.v6
        return ("loaded at 0x%X, runs at 0x%X-0x%X: image %d B, .bss to 0x%X, stack %d B with %d B used at most"
                % (v["load"], v["run"], v["stack_top"], v["image"], v["bss_end"], v["stack_top"] - v["stack_lo"], v["stack_used"]))

    def configure(self, board=None):
        """Send the board's profile to the universal stub (CONFIG, op 0x0D) and check the echo in the next HELLO; writes
        are refused until this has succeeded. The timer and UART clock is the APB clock, measured here (24 MHz under the
        boot ROM, 36 MHz under the DB25-D's loader, its stock firmware and RadioFoundry's hand-over there, 84 MHz under RadioFoundry on the
        DM-32UV / DM-1801 Pro). An older stub has no CONFIG: nothing is sent."""
        if self.v6 is None:
            self.log("stub protocol before 6: no board configuration")
            if board:			# the stub can't take it, but its protected regions still apply here
                self.profile = C7_BOARDS[board]
            return
        if board is None and self.v6["board"]:     # attached to a stub configured earlier: keep its profile
            prof = next((b for b in C7_BOARDS.values() if b["id"] == self.v6["board"]), None)
            if prof is not None and self.v6["flags"] == (1 if prof["nand"] else 0):
                self.configured = self.profile = prof
                self.log("stub already configured for the %s; %s" % (prof["name"], self.footprint()))
                return
        prof = C7_BOARDS[board] if board else GENERIC
        hz = self.timer_hz(0.3)
        if not 1e6 < hz < 4e8:
            raise DumpError("the stub's timer runs at %.0f Hz, which can't be right: not configuring it" % hz)
        hz = int(round(hz / 1e6) * 1e6)         # every APB clock here is whole MHz: rates then come out the same each time
        self.uart_hz = hz
        payload = (hz.to_bytes(4, "big") + hz.to_bytes(4, "big")
                   + bytes([prof["latch"], prof["key"], LED_RED, LED_GREEN, prof["id"], 1 if prof["nand"] else 0, CFG_UART_THIS, 0]))
        self.request(0x0D, payload)
        self.hello(self.retries)                # a damaged reply here is asked for again
        want = (prof["id"], 1 if prof["nand"] else 0)
        if self.v6 is None or (self.v6["board"], self.v6["flags"]) != want:
            raise DumpError("the stub did not take the %s configuration (HELLO says board %s, flags %s): nothing written"
                            % (prof["name"], self.v6 and self.v6["board"], self.v6 and self.v6["flags"]))
        self.configured = prof
        self.profile = prof
        self.log("stub configured for the %s (clock %.1f MHz); %s" % (prof["name"], hz / 1e6, self.footprint()))

    def check_writable(self):
        """The universal stub writes nothing before its board configuration is confirmed (HELLO echo after CONFIG)."""
        if self.v6 is not None and self.configured is None:
            raise DumpError("the stub's board configuration is not confirmed: refusing to write")

    def mem_size(self, mem):
        if mem == "progmem":
            return self.profile["progmem"] if self.v6 is not None else MEM[mem][3]
        return MEM[mem][3]

    def mem_flags(self, mem):
        base, ser, cs, _, _ = MEM[mem]
        if mem == "nand" and not (self.v6 and self.v6["flags"] & 1):
            raise DumpError("the NAND is read through the universal stub on a radio configured with one (--board %s)"
                           % " or ".join(sorted(b for b, p in C7_BOARDS.items() if p["nand"])))
        return (1 if ser else 0) | (2 if cs is not None else 0) | (NAND_FLAG if mem == "nand" else 0)

    # ---- entering the stub from a running RadioFoundry (debug link frames: A5 5A cmd seq len(2 LE) payload crc16(2 LE))
    def check_stub(self, image, load, size):
        """Refuse an image that is not this SoC's stub or is linked for another span. A method so that the STM32 radios
        (f4stub.F4Radio), whose stub has another entry code and runs at the SRAM base, check their own image."""
        if univ_layout(image):
            run, end = check_univ_load(image, load, (load, size))
            self.log("universal stub: loaded at 0x%X, runs at 0x%X-0x%X" % (load, run, end))
            self.stub_head = image[:0x80]
        else:
            check_stub_fits(image, load, size)

    def dl_request(self, cmd, payload, timeout=1.0, retries=3):
        self.dl_seq = (getattr(self, "dl_seq", 0) + 1) & 0xFF
        for attempt in range(retries + 1):
            seq = self.dl_seq = (self.dl_seq + 1) & 0xFF
            body = bytes([cmd, seq]) + len(payload).to_bytes(2, "little") + bytes(payload)
            self.port.reset_input_buffer()
            self.port.write(b"\xA5\x5A" + body + crc16_ccitt(body).to_bytes(2, "little"))
            buf = b""
            end = time.monotonic() + timeout
            self.set_timeout(0.02)
            while time.monotonic() < end:
                buf += self.port.read(256)
                k = buf.find(b"\xA5\x5A")
                if k < 0 or len(buf) < k + 8:
                    continue
                n = buf[k + 4] | buf[k + 5] << 8
                if len(buf) < k + 6 + n + 2:
                    continue
                f = buf[k + 2:k + 6 + n]
                if crc16_ccitt(f) != int.from_bytes(buf[k + 6 + n:k + 8 + n], "little"):
                    buf = buf[k + 1:]
                    continue
                if f[1] == seq and f[0] == (cmd | 0x80):
                    return f[4:]
                buf = buf[k + 8 + n:]
        raise DumpError("no answer from RadioFoundry's debug link to command 0x%02X (is RadioFoundry running with CONFIG_STUB_ENTRY=y and "
                        "the programming cable on UART0 at 115200?)" % cmd)

    def rfoundry_enter(self, image=None, entry=None, board=None, force=False):
        """Load the stub into a running RadioFoundry's free RAM over its debug link and jump to it (radio ON, RadioFoundry running). Without
        `image` the embedded stub whose base is the span RadioFoundry reports in STUB INFO is used; a given image must be linked
        for that span (checked). Afterwards the stub answers on the same port. Recover the radio with a power cycle
        (or --boot reset)."""
        name = version_board(self)
        if board is None:                       # the profile the stub gets: from VERSION's board= (a library caller)
            board = c7_board(name) if name else None
        elif name and board in C7_BOARDS and name not in C7_BOARDS[board]["names"]:
            # Two cables with one serial number can swap names when they re-enumerate: a GD-88 image once went into a
            # DM-32UV this way, because --board was trusted over what the radio said it is
            why = ("the radio on this port runs RadioFoundry for the %s, not the %s that --board names"
                   % (name, C7_BOARDS[board]["name"]))
            if not force:
                raise DumpError(why + ": wrong port? Nothing was sent (--force goes ahead anyway)")
            self.log("warning: %s; going ahead (--force)" % why)
        r = self.dl_request(DL_STUB, b"\x00")
        if r[0] != 0 or len(r) < 19 or int.from_bytes(r[1:5], "little") != STUB_MAGIC:
            raise DumpError("RadioFoundry's STUB info refused or malformed (%s): firmware built without CONFIG_STUB_ENTRY?" % r.hex())
        load, size = int.from_bytes(r[5:9], "little"), int.from_bytes(r[9:13], "little")
        chunk = min(int.from_bytes(r[13:15], "little"), 59)
        if image == "legacy":                   # the position-dependent stubs, one per span (the fallback)
            known = rfoundry_stubs()
            if load not in known:
                raise DumpError("RadioFoundry offers RAM at 0x%X and no legacy stub is linked for that base (have %s): give the "
                                "matching image with --rfoundry-stub" % (load, ", ".join("0x%X %s" % (k, v[0]) for k, v in sorted(known.items()))))
            self.log("the firmware's stub span 0x%X: using the legacy stub for %s" % (load, known[load][0]))
            image = known[load][1]
        elif image is None:
            image = stub_univ_image()
        self.check_stub(image, load, size)
        if len(image) > size:
            raise DumpError("stub image (%d bytes) does not fit RadioFoundry's free RAM (%d bytes)" % (len(image), size))
        entry = load if entry is None else entry
        self.log("RadioFoundry offers %d bytes of RAM at 0x%X; sending the %d-byte stub in %d-byte chunks" % (size, load, len(image), chunk))
        r = self.dl_request(DL_STUB, b"\x01" + STUB_MAGIC.to_bytes(4, "little") + len(image).to_bytes(4, "little")
                            + (zlib.crc32(image) & 0xFFFFFFFF).to_bytes(4, "little") + entry.to_bytes(4, "little"))
        if r[0] != 0:
            raise DumpError("RadioFoundry refused the stub transfer (status %d)" % r[0])
        for off in range(0, len(image), chunk):
            r = self.dl_request(DL_STUB, b"\x02" + off.to_bytes(4, "little") + image[off:off + chunk])
            if r[0] != 0:
                raise DumpError("RadioFoundry refused the chunk at offset %d (status %d)" % (off, r[0]))
        r = self.dl_request(DL_STUB, b"\x03" + STUB_MAGIC.to_bytes(4, "little"))
        if r[0] == 3:
            raise DumpError("RadioFoundry refused to enter the stub: the radio is transmitting. Release PTT and run again")
        if r[0] == 7 and len(r) > 1 and r[1] == 1:
            raise DumpError("RadioFoundry refused to enter this stub: its watchdog runs and the stub doesn't feed it, so the radio "
                            "would reset part way through. Use the stub from a current stubtool.py / f4stub.py")
        if r[0] != 0:
            raise DumpError("RadioFoundry refused to enter the stub (status %d): image incomplete or CRC mismatch" % r[0])
        self.log("RadioFoundry is jumping to the stub")
        time.sleep(0.3)
        self.attach()
        self.configure(board)

    def send_noreply(self, op, payload=b""):
        """A request the radio will not answer because it is about to reset or jump away."""
        self.port.reset_input_buffer()
        frame = bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, sum8(payload), 0x03]) + payload
        self.port.write(frame)
        self.port.flush()                       # nothing comes back to wait on: without this the program could exit and
        time.sleep(0.05 + len(frame) * 10 / self.rate())    # close the port before the frame left (the reset never ran)

    def reset_chip(self):
        """Soft-reset the SoC (hardware-dm32.md 1.1: XIP quad off and the reset divider, then SYS_SOFT_RSTN = 0). The boot
        ROM runs again, offers its serial window briefly, then starts the firmware from flash: the same as a power cycle
        without the cycle. The soft reset keeps the divider, and the ROM can't read flash at RadioFoundry's divider 2. The watchdog
        goes off first: RadioFoundry leaves it running while the stub feeds it, a soft reset may keep it, and the bootloader never
        feeds it (its update mode waits for the PC). RadioFoundry starts it again. The restart-cause mark goes first, so RadioFoundry
        reports a software restart (it counts only when the memory was kept: a stub loaded at power-on leaves it)."""
        self.poke(CRASH_SOFT_MARK, CRASH_BOOT_SOFT.to_bytes(4, "little"))
        self.poke(WDT_LOCK, WDT_KEY.to_bytes(4, "little"))
        self.poke(WDT_EN, (0).to_bytes(4, "little"))
        self.poke(WDT_LOCK, (0).to_bytes(4, "little"))
        self.poke(QUAD_ENABLE, (0).to_bytes(4, "little"))
        self.poke(LCSFC_BAUDR, (4).to_bytes(4, "little"))
        self.send_noreply(0x03, SYS_SOFT_RSTN.to_bytes(4, "big") + (0).to_bytes(4, "little"))

    def jump(self, addr):
        """Call/jump to `addr` (op CALL); no reply is expected if it never returns."""
        self.send_noreply(0x04, addr.to_bytes(4, "big") + bytes(16))

    def peek(self, addr, n):
        return self.request(0x02, addr.to_bytes(4, "big") + n.to_bytes(2, "big"))

    def poke(self, addr, data):
        self.request(0x03, addr.to_bytes(4, "big") + bytes(data))

    def spi(self, base, tx, rx_len, ser=True, cs=None, retries=None):
        flags = (1 if ser else 0) | (2 if cs is not None else 0)
        payload = (base.to_bytes(4, "big") + bytes([flags, cs or 0]) + len(tx).to_bytes(2, "big")
                   + rx_len.to_bytes(2, "big") + bytes(tx))
        r = self.request(0x05, payload, retries)
        if len(r) != rx_len:
            raise DumpError("SPI reply is %d bytes, wanted %d" % (len(r), rx_len))
        return r

    # ---- flash
    def jedec(self, mem):
        base, ser, cs, _, _ = MEM[mem]
        if mem == "nand":                       # 0x9F, a dummy byte, then maker and device (hardware-db25d.md 6.5)
            return self.spi(base, b"\x9F\x00", 2, ser, cs)
        return self.spi(base, b"\x9F", 3, ser, cs)

    def read_flash(self, mem, addr, n):
        base, ser, cs, _, _ = MEM[mem]
        return self.spi(base, bytes([0x03, addr >> 16 & 0xFF, addr >> 8 & 0xFF, addr & 0xFF]), n, ser, cs)

    # ---- flash writes (generic SPI ops: write enable 06, status 05, sector erase 20, page program 02)
    def _status(self, mem):
        base, ser, cs, _, _ = MEM[mem]
        return self.spi(base, b"\x05", 1, ser, cs)[0]

    def _wait_ready(self, mem, what, timeout):
        end = time.monotonic() + timeout
        while True:
            st = self._status(mem)
            if not st & 1:
                return st
            if time.monotonic() > end:
                raise DumpError("%s: flash still busy after %.1f s (status 0x%02X)" % (what, timeout, st))

    def _write_cmd(self, mem, tx, what, timeout):
        self._guard(mem, tx)
        base, ser, cs, _, _ = MEM[mem]
        self.spi(base, b"\x06", 0, ser, cs)                     # write enable
        if not self._status(mem) & 2:
            raise DumpError("%s: write enable did not stick (status 0x%02X): chip write-protected?" % (what, self._status(mem)))
        self.spi(base, tx, 0, ser, cs)
        self._wait_ready(mem, what, timeout)

    def _guard(self, mem, tx):
        """The last check before a flash command goes out, whoever built it: a sector erase (0x20) or page program
        (0x02) must miss the board's protected regions, a chip erase (0x60, 0xC7) is refused on a memory that has any,
        and a 64 KB or 32 KB block erase (0xD8, 0x52) is checked over its block. Nothing here sends the last three."""
        op = tx[0]
        pm = PROTECT_MEM[mem]
        try:
            if op in (0x60, 0xC7):
                protect.check_whole(self.profile, pm)
            elif op in (0x20, 0x52, 0xD8, 0x02) and len(tx) >= 4:
                a = int.from_bytes(tx[1:4], "big")
                n = {0x20: SECTOR, 0x52: 0x8000, 0xD8: 0x10000}.get(op, len(tx) - 4)
                a -= a % n if op != 0x02 else 0
                protect.check(self.profile, pm, a, a + max(n, 1), "erase" if op != 0x02 else "program")
        except protect.ProtectedError as e:
            raise DumpError(str(e))

    def check_write_range(self, mem, start, end, app=False):
        """Refuse a write or erase of [start, end) before anything is sent: whole sectors, never the NAND, only the
        allowed regions, and never a protected region of the board the stub is configured for (board.toml
        [[flash.protect]], rfoundry/protect.py)."""
        if start % SECTOR or end % SECTOR or start >= end:
            raise DumpError("writes are whole 4 KB sectors")
        try:
            protect.check(self.profile, PROTECT_MEM[mem], start, end)
        except protect.ProtectedError as e:
            raise DumpError(str(e))
        if mem == "nand":
            raise DumpError("the NAND is read only here")
        ok = WRITE_OK.get(mem, []) + ([(APP_START, APP_END)] if app and mem == "progmem" else [])
        if not any(a <= start and end <= b for a, b in ok):
            raise DumpError("%s 0x%X-0x%X is outside the allowed write regions %s" %
                            (MEM[mem][4], start, end, ", ".join("0x%X-0x%X" % r for r in WRITE_OK.get(mem, []))))

    def erase_sectors(self, mem, start, end):
        self.check_writable()
        self.check_write_range(mem, start, end)
        self.check_id(mem)
        for a in range(start, end, SECTOR):
            if self.sector_op:
                self.write_sector(mem, a, None)
            else:
                self._write_cmd(mem, bytes([0x20, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF]), "erase 0x%X" % a, 3.0)

    def write_sector(self, mem, a, sec):
        """Erase the sector at `a` and program `sec` (4096 bytes, None = leave it erased) into it. A protocol 7 stub does all
        of it in one request (op 0E) and replies with the CRC-32 it reads back; an older one takes write enable, status,
        command and busy polls as separate SPI requests, four or more per page. Returns the radio's CRC of the sector, or
        None when only the final verify will tell. The caller has checked the range."""
        if not self.sector_op:
            self._write_cmd(mem, bytes([0x20, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF]), "erase 0x%X" % a, 3.0)
            for p in range(0, SECTOR, PAGE) if sec else ():
                page = sec[p:p + PAGE]
                if page != b"\xff" * PAGE:
                    q = a + p
                    self._write_cmd(mem, bytes([0x02, q >> 16 & 0xFF, q >> 8 & 0xFF, q & 0xFF]) + page, "program 0x%X" % q, 1.0)
            return None
        base, ser, cs, _, _ = MEM[mem]
        flags = (1 if ser else 0) | (2 if cs is not None else 0)
        payload = base.to_bytes(4, "big") + bytes([flags, cs or 0]) + a.to_bytes(4, "big") + (bytes(sec) if sec else b"")
        try:                                    # the frame's time on the wire, then erase and program (worst case about 2 s)
            r = self.request(0x0E, payload, timeout=4.0 + len(payload) * 10 / (getattr(self.port, "baudrate", 0) or 115200))
        except DumpError as e:
            if "status 0x%02X" % NOWEL in str(e):
                raise DumpError("write 0x%X: write enable did not stick: chip write-protected?" % a)
            if "status 0x%02X" % SPITIMEOUT in str(e):
                raise DumpError("write 0x%X: the flash stayed busy past its erase or program time" % a)
            raise
        if len(r) != 4:
            raise DumpError("write 0x%X: reply is %d bytes, wanted 4" % (a, len(r)))
        return int.from_bytes(r, "big")

    def write_sectors(self, mem, data, start, todo, progress=None):
        """Write the sectors at the addresses `todo` from `data` (which starts at `start`). Each is checked against the
        radio's read-back CRC at once where the stub sends one, and written again up to 3 times."""
        for n, a in enumerate(todo):
            if progress:
                progress(n, len(todo))
            sec = data[a - start:a - start + SECTOR]
            want = zlib.crc32(sec) & 0xFFFFFFFF
            for attempt in range(1, 4):
                crc = self.write_sector(mem, a, None if sec == b"\xff" * SECTOR else sec)
                if crc is None or crc == want:
                    break
                self.retried += 1
                if attempt == 3:
                    raise DumpError("0x%X: the sector reads back wrong after 3 writes" % a)
                self.log("0x%X: read back differs, writing it again (%d of 3)" % (a, attempt))

    def write_image(self, image, progress=None, replace_stage=False):
        """Program an RadioFoundry application image into the program flash at 0xC000 (where it is linked to run): only the 4 KB
        sectors whose CRC differs from the flash are erased and written, then everything is verified by CRC. The
        bootloader below 0xC000 is never touched. Returns (sectors written, sectors left alone)."""
        mem = "progmem"
        image = bytes(image)
        end = APP_START + (len(image) + SECTOR - 1) // SECTOR * SECTOR
        if not image or end > APP_END:
            raise DumpError("image is %d bytes: it must fit 0x%X-0x%X of the program flash" % (len(image), APP_START, APP_END))
        if image[:4] == b"\x00" * 4 or len(set(image[:64])) < 2:
            raise DumpError("this does not look like an application image (starts with %s)" % image[:8].hex())
        padded = image + b"\xff" * (end - APP_START - len(image))
        self.check_writable()
        self.check_write_range(mem, APP_START, end, app=True)
        self.check_id(mem)
        # The application alone over a first stage leaves the radio in its update loader (white screen, red LED)
        if not replace_stage and self.read_flash(mem, APP_START, 4) == STAGE_MAGIC and image[:4] != STAGE_MAGIC:
            raise DumpError("this radio has RadioFoundry's first stage at 0x%X and the image does not start with it: write the "
                            "install image (rfoundry_<board>_install.bin), not the application alone (--replace-stage "
                            "writes it anyway)" % APP_START)
        todo = self.verify(mem, padded, APP_START)
        self.write_sectors(mem, padded, APP_START, todo, progress)
        bad = self.verify(mem, padded, APP_START) if todo else []
        if bad:
            raise DumpError("verify after write: sectors differ at %s" % ", ".join("0x%X" % x for x in bad))
        return len(todo), (end - APP_START) // SECTOR - len(todo)

    def write_flash(self, mem, start, data):
        """Erase and program whole sectors at `start`, then check by CRC against the radio's own read of them."""
        end = start + len(data)
        self.check_writable()
        self.check_write_range(mem, start, end)
        self.check_id(mem)
        self.write_sectors(mem, data, start, range(start, end, SECTOR))
        bad = self.verify(mem, data, start)
        if bad:
            raise DumpError("verify after write: sectors differ at %s" % ", ".join("0x%X" % x for x in bad))

    def sums(self, mem, addr, count):
        """[(crc32, blank)] for `count` sectors from addr, computed by the radio."""
        base, ser, cs, _, _ = MEM[mem]
        flags = self.mem_flags(mem)
        payload = (base.to_bytes(4, "big") + bytes([flags, cs or 0]) + addr.to_bytes(4, "big")
                   + count.to_bytes(2, "big"))
        r = self.request(0x07, payload, timeout=1.0 + count * 0.15)
        if len(r) != count * 5:
            raise DumpError("sector sums reply is %d bytes, wanted %d" % (len(r), count * 5))
        return [(int.from_bytes(r[i * 5:i * 5 + 4], "big"), r[i * 5 + 4] == 1) for i in range(count)]

    def set_baud(self, baud, window=4.0, tries=None):
        """Ask the stub to switch its UART to `baud`, prove the new rate with full-size reads, then confirm. Provisional on
        the radio's side: unless the confirm arrives inside `window` seconds it goes back to the old rate by itself, and
        this side goes back too. Returns True if the new rate is now in force, False (still at the old rate) if not."""
        self.switching = True
        try:
            return self._set_baud(baud, window, PROOF_ROUNDS if tries is None else tries)
        finally:
            self.switching = False

    def _set_baud(self, baud, window, tries):
        old = self.rate()
        hz = self.timer_hz(0.2)
        if self.sector_op:                      # the proof is full frames both ways (prove_link), and the window is also
            window = max(LINK_WINDOW, 2.5 * tries * (SECTOR + CHUNK + 40) * 10 / baud + 0.5)     # the stub's quiet limit
        else:                                   # older stubs: about 9 * tries full-size reads, with room to spare
            window = max(window, 2 * 9 * tries * (CHUNK + 20) * 10 / baud + 1.0)
        try:
            reply = self.request(0x0B, baud.to_bytes(4, "big") + int(window * hz).to_bytes(4, "big"), retries=0)
        except DumpError as e:
            self.log("%d baud not offered by the stub (%s)" % (baud, e))
            return False
        actual = int.from_bytes(reply, "big")
        t0 = time.monotonic()
        ok = False
        try:
            self.set_timeout(0.02)
            self.port.flush()
            self.port.baudrate = actual
            time.sleep(0.05)
            self.port.reset_input_buffer()
            ref = None
            if self.sector_op:
                self.prove_link(tries)
            for _ in range(0 if self.sector_op else tries):
                self.hello()
                for _ in range(8):            # a sustained stretch of full frames: a marginal link shows up here, not in one frame
                    got = self.peek(self.stub_base, CHUNK)
                    if got != (ref or got):
                        raise DumpError("read-back differs")
                    ref = got
                got = self.peek(self.stub_base, CHUNK)    # the stub's own first bytes: same every time, and a full-size frame
                ref = got if ref is None else ref
                if got != ref or (self.stub_head and got[:len(self.stub_head)] != self.stub_head):
                    raise DumpError("read-back differs")
            self.request(0x0C, retries=3)
            ok = True
            if self.home_baud is None:
                self.home_baud = old
            self.link_window = window
            self.link_score = 0
        except (DumpError, ValueError, OSError) as e:
            self.log("%d baud failed (%s); going back to %d" % (actual, e, old))
        if not ok:
            self.bad_rates.add(actual)
            try:
                self.port.baudrate = old
            except (ValueError, OSError):
                pass
            time.sleep(max(0.0, window + 0.3 - (time.monotonic() - t0)))      # the stub reverts when its window ends
            self.port.reset_input_buffer()
            try:
                self.hello()
            except DumpError:
                if self.home_baud is None or old == self.home_baud:
                    raise
                self.switching = False          # back at a raised rate that no longer answers: go home by silence
                self.fall_back("no answer after a failed try")
            return False
        self.log("serial now %d baud" % actual)
        return True

    def prove_link(self, rounds):
        """Full-size frames both ways at the new rate, each of which must arrive whole: a WRITE-size frame to the radio
        (op 0E with a flag no stub takes, so it is refused with 0B after its checksum is checked and nothing is written),
        then a full-size read of the stub's own image. A cable that manages the replies may still damage long frames
        towards the radio (seen at 276644 baud on a DM-32UV: the PEEK-only proof passed, then WRITE frames failed)."""
        ref = None
        for i in range(rounds):
            junk = os.urandom(SECTOR)
            status, body = self.exchange(0x0E, bytes(4) + b"\x80\x00" + bytes(4) + junk, 1.0)
            if status != BADOP:
                raise DumpError("a full frame to the radio: %s" % (body if status is None else "status 0x%02X" % status))
            status, body = self.exchange(0x02, self.stub_base.to_bytes(4, "big") + CHUNK.to_bytes(2, "big"), 1.0)
            if status != OK or len(body) != CHUNK or body != (ref or body) or (self.stub_head and body[:len(self.stub_head)] != self.stub_head):
                raise DumpError("a full frame from the radio: %s" % (body if status is None else "read-back differs"))
            ref = body

    def baud_key(self):
        """The cable and the radio a remembered rate belongs to: the port's device (or URL) and the board."""
        return "%s|%s" % (getattr(self.port, "port", None) or getattr(self.port, "name", "?"), self.profile["name"])

    def baud_candidates(self):
        """Raised rates this stub can make (whole divisors of its UART clock) in FAST_MIN..FAST_MAX, fastest first."""
        hz = self.uart_hz or self.timer_hz(0.3)
        out = []
        for d in range(1, 4096):
            r = int(hz / (16 * d))
            if r < FAST_MIN:
                break
            if r <= FAST_MAX and r > self.rate() * 1.5:
                out.append(r)
        return out

    def auto_baud(self):
        """The fastest rate that proves itself, from a remembered one or by a binary search over baud_candidates (the
        failures are a cable's bandwidth edge, so above a failing rate nothing is tried). Every step falls back by
        itself; the link ends at the home rate when nothing faster holds. Protocol 7 only. Returns the rate."""
        if not self.sector_op:
            return self.rate()
        cands = [r for r in self.baud_candidates() if r not in self.bad_rates]
        if not cands:
            return self.rate()
        key = self.baud_key()
        known = recall_baud(key)
        if known == 0:
            self.log("no raised rate held on this cable lately: staying at %d baud" % self.rate())
            return self.rate()
        if known:
            near = min(cands, key=lambda r: abs(r - known))
            if abs(near - known) <= known * 0.02 and self.set_baud(near):
                return self.rate()                      # what the stub made, not this side's estimate of it
            forget_baud(key, known)
        lo, hi, best = 0, len(cands) - 1, None         # cands[lo..hi] still open; fastest first
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.set_baud(cands[mid]):              # from a working raised rate a failed try comes back to it
                best = self.rate()                      # the rate the stub made
                hi = mid - 1                            # try faster
            else:
                lo = mid + 1                            # slower
        if best is None:
            remember_baud(key, None)
            return self.rate()
        if self.rate() != best and not self.set_baud(best):
            return self.rate()
        remember_baud(key, self.rate())
        return self.rate()

    def timer_hz(self, span=0.5):
        """Tick rate of timer 2 (it counts down), measured against this computer's clock. Each sample's time is taken as
        its request goes out: the stub reads the timer as soon as the request arrives, while the reply waits up to the
        cable's latency timer (16 ms on an FTDI) on its way back. Timing to the second reply put that wait into the span,
        and over 0.3 s an 84 MHz clock read about 80 MHz: the UART divisor for every rate but the first was 5 % off."""
        t0 = time.monotonic()
        v0 = int.from_bytes(self.peek(TIMER2_VALUE, 4), "little")
        time.sleep(span)
        t1 = time.monotonic()
        v1 = int.from_bytes(self.peek(TIMER2_VALUE, 4), "little")
        return ((v0 - v1) & 0xFFFFFFFF) / (t1 - t0)

    def setup_led(self, red=LED_RED, green=LED_GREEN, status="both"):
        """Status blink on the chosen LED(s): 500 ms per phase while waiting, 125 ms while data moves. Pin 255 = absent;
        status is "red", "green" or "both"."""
        hz = self.timer_hz()
        if not 1e6 < hz < 4e8:
            self.log("timer rate %.0f Hz looks wrong, assuming 24 MHz" % hz)
            hz = 24e6
        mask = {"red": 1, "green": 2, "both": 3, "off": 0}[status]
        self.led_per_ms = int(hz / 1000)
        self.request(0x08, bytes([red & 0xFF, green & 0xFF, mask]) + int(hz * 0.5).to_bytes(4, "big")
                     + int(hz * 0.125).to_bytes(4, "big") + self.led_per_ms.to_bytes(4, "big"))

    def led_sequence(self, steps, repeat=False, hold=False):
        """Play [(red, green, ms)] (brightness 0-255 each; PWM mixes them) on the radio by itself; it keeps playing
        after this program exits. Without repeat or hold the status blink resumes at the end; [] cancels."""
        if len(steps) > 32:
            raise DumpError("at most 32 LED steps")
        payload = bytes([(1 if repeat else 0) | (2 if hold else 0), len(steps)])
        for r, g, ms in steps:
            payload += min(int(ms), 65535).to_bytes(2, "big") + bytes([r & 0xFF, g & 0xFF])
        self.request(0x0A, payload)

    def read_frame(self, timeout):
        hdr = self.read_exact(6, timeout)
        if len(hdr) != 6 or hdr[0] != 0x02 or hdr[5] != 0x03:
            raise FrameError("no or malformed frame header")
        n = (hdr[2] << 8) | hdr[3]
        body = self.read_exact(n, 0.5 + n * 0.0002)
        if len(body) != n:
            raise FrameError("frame short (%d of %d bytes)" % (len(body), n))
        if sum8(body) != hdr[4]:
            raise FrameError("frame checksum")
        return hdr[1], body

    def drain(self, quiet=0.4):
        """Read and drop what the stub is still streaming, until the line has been quiet for `quiet` seconds."""
        self.set_timeout(0.05)
        last = time.monotonic()
        while time.monotonic() - last < quiet:
            if self.port.read(4096):
                last = time.monotonic()
        self.port.reset_input_buffer()

    def sectors(self, mem, addr, count, compress=True):
        """Generator of (crc32, method, data) for `count` sectors from addr. The stub reads each sector, CRCs it and
        LZ4-compresses it when that is smaller (method 1; 0 raw, 2 blank), sending one frame per sector and preparing the
        next sector while the previous one is on the wire. A frame error raises FrameError; the rest of the stream
        must then be drained."""
        base, ser, cs, _, _ = MEM[mem]
        flags = self.mem_flags(mem)
        payload = (base.to_bytes(4, "big") + bytes([flags, cs or 0]) + addr.to_bytes(4, "big")
                   + bytes([count, 1 if compress else 0]))
        self.port.reset_input_buffer()
        self.port.write(bytes([0x02, 0x09, 0, len(payload), sum8(payload), 0x03]) + payload)
        for i in range(count):
            status, r = self.read_frame(3.0 if i == 0 else 1.5)
            if status != OK:
                if status in (BADOP, TOOLONG):
                    raise DumpError("stub op 0x09 refused: status 0x%02X" % status)
                raise FrameError("stub op 0x09: status 0x%02X" % status)
            if len(r) < 7 or len(r) != 7 + ((r[5] << 8) | r[6]):
                raise FrameError("sector record malformed")
            yield int.from_bytes(r[:4], "big"), r[4], r[7:]

    def unpack(self, crc, method, data):
        """The sector's 4096 bytes if they match the radio's CRC-32 of it, else None."""
        try:
            if method == 2:
                sec = b"\xff" * SECTOR
            elif method == 1:
                sec = lz4_block(data, SECTOR)
            else:
                sec = data
        except (DumpError, IndexError):
            return None
        return sec if len(sec) == SECTOR and zlib.crc32(sec) == crc else None

    def check_id(self, mem):
        id_ = self.jedec(mem)
        self.log("%s JEDEC ID %s" % (MEM[mem][4], id_.hex()))
        if id_ in (b"\xff" * len(id_), b"\x00" * len(id_)):
            raise DumpError("%s does not answer the ID command (%s): chip select or pin mux wrong, nothing was dumped"
                            % (MEM[mem][4], id_.hex()))

    def windows(self, mem, start, end):
        size = self.mem_size(mem)
        if not 0 <= start < end <= size:
            raise DumpError("%s range must lie within 0..0x%X" % (mem, size))
        for w in range(start // SECTOR * SECTOR, end, WINDOW * SECTOR):
            yield w, min(WINDOW, (end - w + SECTOR - 1) // SECTOR)

    def dump(self, out, mem, start, end, progress=None, compress=True):
        """The radio reads each sector, CRCs it and sends it blank (a flag), LZ4-compressed or raw; every sector must
        match the radio's CRC-32. A sector that doesn't is asked for again, uncompressed from the second try. A record
        carries no address, so one left over from a stream cut short could pass for another sector: at the end every
        sector is checked against the radio's sector CRCs (op 07) and read again until it matches."""
        self.check_id(mem)
        size = self.mem_size(mem)
        if not 0 <= start < end <= size:
            raise DumpError("%s range must lie within 0..0x%X" % (mem, size))
        out.seek(0)
        out.truncate(end - start)
        self.skipped = self.packed = 0
        self.wire = 0
        total = fails = 0
        sa = lo = start // SECTOR * SECTOR
        full = bytearray((end + SECTOR - 1) // SECTOR * SECTOR - lo)
        want = compress
        while sa < end:
            count = min(PACK, (end - sa + SECTOR - 1) // SECTOR)
            try:
                for crc, method, data in self.sectors(mem, sa, count, want):
                    sec = self.unpack(crc, method, data)
                    if sec is None:
                        raise FrameError("0x%X: sector does not match the radio's CRC" % sa)
                    self.skipped += method == 2
                    self.packed += method == 1
                    self.wire += len(data)
                    self.put(out, full, lo, start, end, sa, sec)
                    sa += SECTOR
                    total += 1
                    fails = 0
                    self.link_score = max(0, self.link_score - 1)
                    want = compress
                    if progress and total % 64 == 0:
                        progress(sa, end)
            except FrameError as e:
                fails += 1
                self.retried += 1
                self.drain()
                if self.stream_damaged(str(e)):
                    fails = 0                       # at the home rate now: a fresh budget there
                elif fails > self.retries:
                    raise DumpError("0x%X: no clean read after %d tries (%s)" % (sa, fails, e))
                else:
                    self.log("0x%X: %s, resuming here (%d of %d)" % (sa, e, fails, self.retries))
                want = compress and fails < 2       # a second failure at the same sector: ask for it uncompressed
        for attempt in range(self.retries + 1):
            bad = self.verify(mem, bytes(full), lo)
            if not bad:
                break
            if attempt == self.retries:
                raise DumpError("0x%X: the read does not match the radio's sector CRC after %d tries" % (bad[0], attempt + 1))
            self.log("%d sector(s) read wrong (first 0x%X), reading them again" % (len(bad), bad[0]))
            self.retried += len(bad)
            for sa in bad:
                try:
                    for crc, method, data in self.sectors(mem, sa, 1, False):
                        sec = self.unpack(crc, method, data)
                        if sec is not None:
                            self.put(out, full, lo, start, end, sa, sec)
                except FrameError as e:
                    self.log("0x%X: %s" % (sa, e))
                    self.drain()
                    self.stream_damaged(str(e))
        out.flush()
        if total == self.skipped:
            self.log("warning: every sector was blank (0xFF)")

    @staticmethod
    def put(out, full, lo, start, end, sa, sec):
        """A sector read at sa: into the whole-sector copy (for the final check) and the part of it in [start, end) out"""
        full[sa - lo:sa - lo + SECTOR] = sec
        a, b = max(start, sa), min(end, sa + SECTOR)
        out.seek(a - start)
        out.write(sec[a - sa:b - sa])

    def verify(self, mem, data, start=0):
        """Sector addresses where the flash differs from `data` (a multiple of 4096 bytes at a 4096 boundary), by
        comparing CRC-32s: no flash data crosses the cable."""
        if start % SECTOR or len(data) % SECTOR:
            raise DumpError("verify needs whole 4 KB sectors")
        self.check_id(mem)
        bad = []
        for w, count in self.windows(mem, start, start + len(data)):
            for i, (crc, _) in enumerate(self.sums(mem, w, count)):
                off = w + i * SECTOR - start
                if zlib.crc32(data[off:off + SECTOR]) != crc:
                    bad.append(w + i * SECTOR)
        return bad


def _baud_cache():
    try:
        import json
        with open(BAUD_CACHE) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


BAUD_NONE_DAYS = 7                  # "nothing faster held" is believed this long, then auto searches again


def recall_baud(key):
    """The raised rate that last held for this cable and radio; 0 when none did (lately); None when not known."""
    v = _baud_cache().get(key)
    if not isinstance(v, dict) or not isinstance(v.get("rate"), int):
        return None
    if v["rate"] == 0 and time.time() - v.get("t", 0) > BAUD_NONE_DAYS * 86400:
        return None
    return v["rate"]


def remember_baud(key, rate):
    """Keep `rate` (None: nothing faster held) for the next run; a cache that can't be written is only slower."""
    import json
    d = _baud_cache()
    d[key] = {"rate": rate or 0, "t": int(time.time())}
    try:
        os.makedirs(os.path.dirname(BAUD_CACHE), exist_ok=True)
        with open(BAUD_CACHE, "w") as f:
            json.dump(d, f, indent=1, sort_keys=True)
    except OSError:
        pass


def forget_baud(key, rate):
    """A remembered rate that failed is not tried first again (auto searches next time)."""
    if recall_baud(key) == rate:
        import json
        d = _baud_cache()
        d.pop(key, None)
        try:
            with open(BAUD_CACHE, "w") as f:
                json.dump(d, f, indent=1, sort_keys=True)
        except OSError:
            pass


def fingerprint(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------------------------------------------
# simulated radio (self-test): the ROM handshake and loader upload, then the stub's frames over two simulated flashes.
# The data flash answers only while its GPIO chip select is low, as on the radio (a read without it returns 0xFF).
# Faults: `drop` = {request number: (offset, count)} bytes removed from a reply, `flip` = request numbers with a flipped
# bit in the data, `silent` = request numbers with no reply, `misplace` = request numbers whose first sector record
# carries the next sector's data with its own CRC (as a record left over from a stream cut short would).

def lz4_encode(src):
    """A greedy LZ4 block encoder, for the simulated radio only (the real one is stub/lz4enc.h)."""
    out = bytearray()
    seen = {}
    ip = anchor = 0
    n = len(src)

    def ext(v):
        while v >= 255:
            out.append(255)
            v -= 255
        out.append(v)

    while n >= 13 and ip <= n - 12:
        key = src[ip:ip + 4]
        ref = seen.get(key)
        seen[key] = ip
        if ref is not None and ip - ref < 65536:
            ml = 4
            while ip + ml < n - 5 and src[ref + ml] == src[ip + ml]:
                ml += 1
            lit = ip - anchor
            out.append((min(lit, 15) << 4) | min(ml - 4, 15))
            if lit >= 15:
                ext(lit - 15)
            out += src[anchor:ip]
            out += (ip - ref).to_bytes(2, "little")
            if ml - 4 >= 15:
                ext(ml - 19)
            ip += ml
            anchor = ip
            continue
        ip += 1
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        ext(lit - 15)
    out += src[anchor:]
    return bytes(out)


class FakeRadio:
    def __init__(self, images, drop=None, flip=(), silent=(), swap=(), misplace=(), stub_len=1536, load=LOAD_ADDR,
                 rom_echo=False, rom_nak=False, rom_late=False):
        self.images = images                # {"progmem": bytes, "q128": bytes}
        self.rom_echo = rom_echo            # the cable sends back every byte written while the boot ROM listens
        self.rom_nak = rom_nak              # the boot ROM answers the load with something other than its ack
        self.rom_late = rom_late            # the answer to the greeting comes too late: the ROM stops waiting
        self.rom_taken = False              # the ROM took the answer to its greeting (a second one is NAKed)
        self.drop, self.flip, self.silent, self.swap = drop or {}, set(flip), set(silent), set(swap)
        self.misplace = set(misplace)
        self.data_bytes = 0                 # flash bytes sent over the cable
        self.sectors_sent = 0
        self.pokes = []
        self.led = self.led_seq = None
        self.out = bytearray(b"\x02\x24\x00\x03" + (ROM_GAVE_UP if rom_late else b""))
        self.inb = bytearray()
        self.loaded = bytearray()
        self.running = False
        self.requests = 0
        self.timeout = 0.02
        self.stub_len = stub_len
        self.load = load                    # where the ROM (or the DB25-D's first stage) expects the stub
        self.errors = []
        self.mem = {0x14020000: 0, 0x11000038: 0xFFFFFE07}
        self.t_hz = 24e6
        self.baudrate = self.stub_baud = 115200   # the host's setting and the stub's; the "cable" only works if they agree
        self.max_baud = None                      # the fastest rate the cable manages
        self.max_in = None                        # the fastest at which long frames to the radio still arrive whole
        self.home = self.idle = None              # protocol 7: the stub's home rate, and how long it stays away unheard
        self.last_ok = 0.0
        self.pending = None                       # (old rate, deadline) of a provisional baud change
        self.baud_log = []
        self.switch_to = None
        self.wel = False
        self.writes = []
        self.univ = None                    # a universal stub: {"run", "end", "board", "flags", "honor"} (protocol 6)
        self.write_op = False               # protocol 7: op 0E writes a whole sector
        self.bad_write = 0                  # this many of the next op 0E writes read back with a bit wrong
        self.write_status = 0               # every op 0E answers this status instead (0F protected, 0C busy), 0 = none
        self.configs = []

    def revert_check(self):
        if self.pending and time.monotonic() >= self.pending[1]:
            self.stub_baud = self.pending[0]
            self.pending = None
            self.last_ok = time.monotonic()             # the stub's quiet limit counts from the revert
            self.baud_log.append("reverted")
        if (self.write_op and self.home and not self.pending and self.stub_baud != self.home
                and time.monotonic() - self.last_ok >= self.idle):
            self.stub_baud = self.home
            self.baud_log.append("home")

    def linked(self):
        self.revert_check()
        return self.baudrate == self.stub_baud and (self.max_baud is None or self.baudrate <= self.max_baud)

    def flush(self):
        self.flushed = len(self.inb) + self.requests      # what had been written when the host last waited for it to go

    def reset_input_buffer(self):
        self.out.clear()

    def read(self, n):
        if not self.linked():
            return b""
        r = bytes(self.out[:n])
        del self.out[:n]
        if not self.out and self.switch_to:          # the BAUD reply has left at the old rate: now the stub switches
            self.stub_baud, self.switch_to = self.switch_to, None
        return r

    def write(self, b):
        if self.running and not self.linked():
            return                              # garbled on the wire: the stub sees nothing it can use
        if self.rom_echo and not self.running:
            self.out += b
        if self.running and self.max_in and self.stub_baud > self.max_in and len(b) > 1100:
            b = bytes(b[:600]) + bytes([b[600] ^ 0x20]) + bytes(b[601:])
        self.inb += b
        while self.step():
            pass

    def frame(self, status, body):
        return bytes([0x02, status, len(body) >> 8, len(body) & 0xFF, sum8(body), 0x03]) + bytes(body)

    def step(self):
        ib = self.inb
        if not self.running:
            if len(ib) < 4:
                return False
            if self.rom_late:                   # it stopped waiting: the firmware runs, and ignores all of it
                ib.clear()
                return False
            if bytes(ib[:4]) == b"\x02\x14\x00\x03":
                del ib[:4]
                if self.rom_taken:
                    self.out += ROM_NAK
                self.rom_taken = True
                return True
            op, chk = ib[1], ib[2]
            n = {0x18: 4 + 8 + BLOCK, 0x1B: 4 + 4}.get(op)
            if n is None:
                self.errors.append("unexpected %s" % bytes(ib[:8]).hex())
                ib.clear()
                return False
            if len(ib) < n:
                return False
            payload = bytes(ib[4:n])
            del ib[:n]
            if sum8(payload) != chk:
                self.errors.append("rom checksum")
                return True
            if op == 0x18:
                addr = int.from_bytes(payload[:4], "big")
                if addr != self.load + len(self.loaded) or payload[4:8] != b"\x00\x00\x00\x40":
                    self.errors.append("load block address")
                self.loaded += payload[8:]
            else:
                if int.from_bytes(payload, "big") != self.load or len(self.loaded) != self.stub_len:
                    self.errors.append("start command")
                self.running = True
            self.out += b"\x02\x0a\x00\x03" if self.rom_nak else ROM_ACK
            return True
        if len(ib) < 6:
            return False
        if ib[0] != 0x02:
            del ib[0]
            return True
        op, ln, chk = ib[1], (ib[2] << 8) | ib[3], ib[4]
        if len(ib) < 6 + ln:
            return False
        payload = bytes(ib[6:6 + ln])
        del ib[:6 + ln]
        if sum8(payload) != chk:
            self.out += self.frame(BADSUM, b"")
            return True
        self.requests += 1
        self.last_ok = time.monotonic()
        n = self.requests
        if n in self.silent:
            return True
        status, body = OK, b""
        if op == 0x01:
            body = b"C7STUB" + bytes([1, 1032 >> 8, 1032 & 255]) + LOAD_ADDR.to_bytes(4, "big")
            u = self.univ
            if u:
                body = (b"C7STUB" + bytes([7 if self.write_op else 6, 1032 >> 8, 1032 & 255]) + self.load.to_bytes(4, "big") + (0x0808).to_bytes(4, "big")
                        + bytes([u["board"], u["flags"]]) + u["run"].to_bytes(4, "big") + len(self.loaded).to_bytes(4, "big")
                        + (u["end"] - 0x1004).to_bytes(4, "big") + (u["end"] - 0x1000).to_bytes(4, "big")
                        + u["end"].to_bytes(4, "big") + (600).to_bytes(4, "big"))
        elif op == 0x0E and self.write_op:
            base, flags, cs, a = int.from_bytes(payload[:4], "big"), payload[4], payload[5], int.from_bytes(payload[6:10], "big")
            mem = "progmem" if base == SSI_PROG else "q128"
            if len(payload) not in (10, 10 + SECTOR) or a % SECTOR or flags & ~3:
                status = BADOP
            elif not (flags & 1 and (mem == "progmem" or (flags & 2 and cs == 18))):
                status = NOWEL                                    # nothing selected: the status reads 0xFF... or 0
            elif self.write_status:
                status = self.write_status
            else:
                img = bytearray(self.images[mem])
                img[a:a + SECTOR] = payload[10:] or b"\xff" * SECTOR
                if self.bad_write:
                    self.bad_write -= 1
                    img[a + 100] ^= 0x10
                self.images[mem] = bytes(img)
                self.writes.append((mem, 0x0E, a))
                body = zlib.crc32(bytes(img[a:a + SECTOR])).to_bytes(4, "big")
        elif op == 0x0D and self.univ:
            if len(payload) != 16 or payload[14] > 3:
                status = BADOP
            else:
                self.configs.append(bytes(payload))
                if self.univ["honor"]:
                    self.univ["board"], self.univ["flags"] = payload[12], payload[13]
        elif op == 0x02:
            a = int.from_bytes(payload[:4], "big")
            c = int.from_bytes(payload[4:6], "big")
            if a == TIMER2_VALUE:
                self.mem[a] = (-int(time.monotonic() * self.t_hz)) & 0xFFFFFFFF
            run = self.univ["run"] if self.univ else LOAD_ADDR
            if run <= a < run + len(self.loaded):
                body = bytes(self.loaded[a - run:a - run + c])
            else:
                body = b"".join(self.mem.get(a + i, 0).to_bytes(4, "little") for i in range(0, c, 4))[:c]
        elif op == 0x0B:
            want = int.from_bytes(payload[:4], "big")
            body = want.to_bytes(4, "big")
            self.pending = (self.stub_baud, time.monotonic() + int.from_bytes(payload[4:8], "big") / self.t_hz)
            self.home = self.home or self.stub_baud
            self.idle = int.from_bytes(payload[4:8], "big") / self.t_hz
            self.out += self.frame(OK, body)           # goes out at the old rate, then the stub switches
            self.switch_to = want
            self.baud_log.append("to %d" % want)
            return True
        elif op == 0x0C:
            self.revert_check()
            self.pending = None
            self.baud_log.append("confirmed")
        elif op == 0x07:
            base = int.from_bytes(payload[:4], "big")
            flags, cs = payload[4], payload[5]
            addr, cnt = int.from_bytes(payload[6:10], "big"), int.from_bytes(payload[10:12], "big")
            mem = "progmem" if base == SSI_PROG else "nand" if flags & NAND_FLAG else "q128"
            sel = bool(flags & 1) and (mem == "progmem" or (flags & 2 and cs == 18))
            for i in range(cnt):
                sec = self.images[mem][addr + i * SECTOR:addr + (i + 1) * SECTOR] if sel else b"\xff" * SECTOR
                body += zlib.crc32(sec).to_bytes(4, "big") + bytes([1 if sec == b"\xff" * SECTOR else 0])
        elif op == 0x03:
            a = int.from_bytes(payload[:4], "big")
            for i in range(0, len(payload) - 4, 4):
                self.mem[a + i] = int.from_bytes(payload[4 + i:8 + i], "little")
                self.pokes.append((a + i, self.mem[a + i]))
        elif op == 0x08:
            if len(payload) != 15:
                status = BADOP
            self.led = tuple(payload[:3])
            self.led_ticks = (int.from_bytes(payload[3:7], "big"), int.from_bytes(payload[7:11], "big"),
                              int.from_bytes(payload[11:15], "big"))
        elif op == 0x0A:
            if len(payload) < 2 or len(payload) != 2 + 4 * payload[1]:
                status = BADOP
            self.led_seq = (payload[0], [(int.from_bytes(payload[2 + 4 * i:4 + 4 * i], "big"), payload[4 + 4 * i], payload[5 + 4 * i])
                                         for i in range(payload[1])])
        elif op == 0x09:
            base = int.from_bytes(payload[:4], "big")
            flags, cs = payload[4], payload[5]
            addr, cnt, mode = int.from_bytes(payload[6:10], "big"), payload[10], payload[11]
            mem = "progmem" if base == SSI_PROG else "nand" if flags & NAND_FLAG else "q128"
            sel = bool(flags & 1) and (mem == "progmem" or (flags & 2 and cs == 18))
            frames = bytearray()
            for i in range(cnt):
                at = addr + (i + 1 if i == 0 and n in self.misplace else i) * SECTOR
                sec = self.images[mem][at:at + SECTOR] if sel else b"\xff" * SECTOR
                crc = zlib.crc32(sec).to_bytes(4, "big")
                if sec == b"\xff" * SECTOR:
                    rec = crc + bytes([2, 0, 0])
                else:
                    z = lz4_encode(sec) if mode else sec
                    m = 1 if mode and len(z) < SECTOR - 16 else 0
                    z = z if m else sec
                    rec = crc + bytes([m]) + len(z).to_bytes(2, "big") + z
                    self.sectors_sent += 1
                fr = bytearray(self.frame(OK, rec))
                if i == 0 and n in self.flip:
                    fr[6 + len(rec) // 2] ^= 0x04
                if i == 0 and n in self.drop:
                    off, c = self.drop[n]
                    del fr[6 + off:6 + off + c]
                if i == 0 and n in self.swap:
                    k = next((j for j in range(7 + 5, len(rec) - 1) if rec[j] != rec[j + 1]), None)
                    if k is not None:
                        fr[6 + k], fr[7 + k] = fr[7 + k], fr[6 + k]     # frame sum unchanged, sector CRC not
                frames += fr
            self.data_bytes += len(frames)
            self.out += frames
            return True
        elif op == 0x05:
            base = int.from_bytes(payload[:4], "big")
            flags, cs = payload[4], payload[5]
            txn, rxn = int.from_bytes(payload[6:8], "big"), int.from_bytes(payload[8:10], "big")
            tx = payload[10:10 + txn]
            mem = "progmem" if base == SSI_PROG else "q128"
            selected = bool(flags & 1) and (mem == "progmem" or (flags & 2 and cs == 18))
            if not selected:
                body = b"\xff" * rxn
            elif tx[:1] == b"\x03":
                a = int.from_bytes(tx[1:4], "big")
                body = self.images[mem][a:a + rxn]
            elif tx[:1] == b"\x06":
                self.wel = True
            elif tx[:1] == b"\x05":
                body = bytes([2 if self.wel else 0])
            elif tx[:1] in (b"\x20", b"\x02"):
                a = int.from_bytes(tx[1:4], "big")
                img = bytearray(self.images[mem])
                if not self.wel:
                    pass                                          # ignored without write enable
                elif tx[0] == 0x20:
                    img[a:a + SECTOR] = b"\xff" * SECTOR
                else:
                    for i, b in enumerate(tx[4:]):
                        img[a + i] &= b                           # programming only clears bits
                self.images[mem] = bytes(img)
                self.wel = False
                self.writes.append((mem, tx[0], a))
            elif tx[:1] == b"\x9F":
                body = bytes([0xC8, 0x40, 0x14 if mem == "progmem" else 0x18])[:rxn]
            else:
                self.errors.append("unknown flash command %s" % tx.hex())
        else:
            status = BADOP
        if n in self.swap and body:        # two bytes swapped: the frame's 8-bit sum can't see it, the sector CRC can
            b = bytearray(body)
            i = next((k for k in range(len(b) - 1) if b[k] != b[k + 1]), None)
            if i is not None:
                b[i], b[i + 1] = b[i + 1], b[i]
            frame = bytearray(self.frame(status, bytes(b)))
            frame[4] = sum8(body)
            body = bytes(b)
        else:
            frame = bytearray(self.frame(status, body))
        if op == 0x05 and rxn_big(payload):
            self.data_bytes += len(body)
        if n in self.flip and body:
            frame[6 + len(body) // 2] ^= 0x04
        if n in self.drop and body:
            off, cnt = self.drop[n]
            del frame[6 + off:6 + off + cnt]
        self.out += frame
        return True


class FakeRFoundry:
    """A running RadioFoundry's debug link STUB command (docs/spec/debuglink.md 3.18) for the self-tests: offers `span`,
    takes the image, enters if its CRC matches. Also used by f4stub.py's self-test."""

    def __init__(self, span=(0x17000, 0x18000)):
        self.span = span                            # (base, size) the "firmware" offers in STUB INFO
        self.timeout = 0.02
        self.out = b""
        self.mem = bytearray(span[1])
        self.total = self.crc = None
        self.entered = False

    def reset_input_buffer(self):
        self.out = b""

    def read(self, n):
        r, self.out = self.out[:n], self.out[n:]
        return r

    def write(self, b):
        assert b[:2] == b"\xA5\x5A" and crc16_ccitt(b[2:-2]) == int.from_bytes(b[-2:], "little")
        cmd, seq, n = b[2], b[3], b[4] | b[5] << 8
        p = b[6:6 + n]
        if cmd == 0x02:                             # VERSION: the board rfoundry_enter picks the stub profile by
            rep = bytes([cmd | 0x80, seq]) + (14).to_bytes(2, "little") + b"\0board=DB25-D\n"
            self.out = b"\xA5\x5A" + rep + crc16_ccitt(rep).to_bytes(2, "little")
            return
        assert cmd == DL_STUB
        op, st, body = p[0], 0, b""
        if op == 0:
            body = (STUB_MAGIC.to_bytes(4, "little") + self.span[0].to_bytes(4, "little") + self.span[1].to_bytes(4, "little")
                    + (59).to_bytes(2, "little") + b"\0" + (0).to_bytes(4, "little"))
        elif op == 1:
            self.total, self.crc = int.from_bytes(p[5:9], "little"), int.from_bytes(p[9:13], "little")
            self.mem = bytearray(self.span[1])
        elif op == 2:
            off = int.from_bytes(p[1:5], "little")
            self.mem[off:off + len(p) - 5] = p[5:]
        elif op == 3:
            st = 0 if zlib.crc32(bytes(self.mem[:self.total])) == self.crc else 4
            self.entered = st == 0
        rep = bytes([cmd | 0x80, seq]) + (1 + len(body)).to_bytes(2, "little") + bytes([st]) + body
        self.out = b"\xA5\x5A" + rep + crc16_ccitt(rep).to_bytes(2, "little")


def rxn_big(payload):
    return int.from_bytes(payload[8:10], "big") >= 256


def open_port(serial, path):
    """A device or a pyserial URL: rfc2217://HOST:PORT reaches a radio on a networked serial server."""
    return serial.serial_for_url(path, 115200, timeout=0.1)


def self_test():
    import io
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
    try:
        import serial
    except ImportError:
        serial = None
    if serial is not None:	# the port argument takes pyserial URLs (rfc2217:// through a serial server)
        p = open_port(serial, "loop://")
        p.write(b"\x5a")
        check(p.read(1) == b"\x5a", "a pyserial URL opens as the port")
        p.close()
    def image(seed, blank_from):
        d = bytearray(bytes((i * seed + (i >> 8) * 13 + 1) & 0xFF for i in range(64 * SECTOR)))
        d[blank_from * SECTOR:] = b"\xff" * (len(d) - blank_from * SECTOR)          # erased tail
        d[5 * SECTOR:7 * SECTOR] = b"\xff" * (2 * SECTOR)                           # and a hole
        return bytes(d)
    prog, data = image(7, 40), image(5, 20)
    images = {"progmem": prog, "q128": data}
    stub = bytes(range(256)) * 7
    # faults land on the requests after the hello: drops, flips, silence (frame checks) and a byte swap (sector CRC)
    radio = FakeRadio(images, drop={4: (124, 18), 12: (0, 1)}, flip={5, 13}, silent={6, 14}, swap={7, 15, 21},
                      stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    for mem, img in (("progmem", prog), ("q128", data)):     # both memories in the one boot
        out = io.BytesIO()
        r.dump(out, mem, 0, len(img))
        check(out.getvalue() == img, "%s image exact despite drops, flips, swaps and silent replies" % mem)
        check(r.skipped >= 24, "%s blank sectors skipped (%d)" % (mem, r.skipped))
        check(r.packed > 0, "%s sectors arrived LZ4-compressed (%d)" % (mem, r.packed))
    check(r.retried >= 6, "retries counted (%d)" % r.retried)
    check(not radio.errors, "radio saw errors: %s" % radio.errors)
    check(radio.data_bytes < (len(prog) + len(data)) * 0.7, "blank sectors stay off the cable (%d bytes)" % radio.data_bytes)

    # a cable that echoes what the host sends: the echo is dropped and the load goes on
    radio = FakeRadio(images, stub_len=len(stub), rom_echo=True)
    lines = []
    r = Radio(radio, log=lines.append)
    r.start(wait=2, stub=stub)
    check(radio.running and r.echo and not radio.errors, "the boot ROM load works through an echoing cable")
    check(sum("echoes what" in l for l in lines) == 1, "the echo is logged once")
    # the ROM answers with something else: the error names the bytes, the cable and the way out
    radio = FakeRadio(images, stub_len=len(stub), rom_nak=True)
    r = Radio(radio, log=quiet, cable="CH340")
    try:
        r.start(wait=2, stub=stub)
        check(False, "a refused boot ROM load fails")
    except DumpError as e:
        m = str(e)
        check("received 02 0a 00 03" in m and "CH340 programming cable" in m and "stock firmware file" in m,
              "a refused boot ROM load says what came back, the cable and the way out: %s" % m)
    # the answer to the greeting came too late: the ROM says it stopped waiting, and the error names --slow-cable
    radio = FakeRadio(images, stub_len=len(stub), rom_late=True)
    try:
        Radio(radio, log=quiet).start(wait=2, stub=stub)
        check(False, "a boot ROM that stopped waiting fails the load")
    except DumpError as e:
        check("stopped waiting" in str(e) and "--slow-cable" in str(e), "a late answer is reported as one: %s" % e)
    # --slow-cable: the answer goes out before the greeting is seen; the load goes on, the NAKs of the extra answers dropped
    radio = FakeRadio(images, stub_len=len(stub))
    sent = []
    write = radio.write
    radio.write = lambda b: (sent.append(bytes(b)), write(b))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub, slow=True)
    check(radio.running and not radio.errors and sent[0] == ROM_REPLY, "the boot ROM load works with --slow-cable")
    radio = FakeRadio(images, stub_len=len(stub))
    radio.out = bytearray(ROM_GAVE_UP)
    try:
        Radio(radio, log=quiet).start(wait=2, stub=stub)
        check(False, "a radio that started its firmware fails the wait")
    except DumpError as e:
        check("started its firmware" in str(e) and "--slow-cable" in str(e), "the ROM giving up with no greeting seen: %s" % e)
    # a near-echo is not an echo: it stays in the error, and only an ack after it counts (a damaged echo on a CH340 cable)
    r = Radio(io.BytesIO(), log=quiet)
    r.read_exact = lambda n, t, q=[b"\x02\x1b\x01\x03\x00\x01\x00\x07"]: q.pop() if q else b""
    ok, got = r.rom_ack(b"\x02\x1b\x01\x03\x00\x01\x00\x00", 0.1)
    check(not ok and got[:4] == b"\x02\x1b\x01\x03" and not r.echo, "a frame that differs from the one sent is not taken for an echo")
    r = Radio(io.BytesIO(), log=quiet)
    r.read_exact = lambda n, t, q=[ROM_ACK, b"\x47\x02\x00\x40\x00\x00"]: q.pop() if q else b""
    ok, got = r.rom_ack(b"\x02\x1b\x01\x03\x00\x01\x00\x00", 0.5)
    check(ok, "bytes ahead of the acknowledgement are passed over")
    r = Radio(io.BytesIO(), log=quiet)
    r.read_exact = lambda n, t, q=[ROM_NAK, b"\x00\x00"]: q.pop() if q else b""
    t0 = time.monotonic()
    ok, got = r.rom_ack(b"\x02\x1b\x01\x03\x00\x01\x00\x00", 5)
    check(not ok and got.endswith(ROM_NAK) and time.monotonic() - t0 < 1, "a NAK ends the wait at once")
    # an echo that arrives in pieces, then the acknowledgement; and silence
    frame = b"\x02\x1b\x01\x03\x00\x01\x00\x00"
    r = Radio(io.BytesIO(), log=quiet)
    r.read_exact = lambda n, t, q=[ROM_ACK, frame[5:], frame[:5]]: q.pop() if q else b""
    ok, got = r.rom_ack(frame, 0.5)
    check(ok and r.echo, "an echo in pieces is dropped and the acknowledgement after it taken")
    r = Radio(io.BytesIO(), log=quiet)
    r.read_exact = lambda n, t: b""
    check(r.rom_ack(frame, 0.1) == (False, b""), "silence: not acknowledged, nothing received")

    # a record that belongs to the next sector passes its own CRC: the final check against the radio's sector CRCs
    # catches it and reads the sector again
    radio = FakeRadio(images, misplace={4}, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    out = io.BytesIO()
    r.dump(out, "progmem", 0, len(prog))
    check(out.getvalue() == prog, "a misplaced sector record is caught by the sector CRCs and read again")
    check(r.retried >= 1, "the misplaced sector was read again")
    out = io.BytesIO()
    r.dump(out, "q128", 0, len(data), compress=False)
    check(out.getvalue() == data and r.packed == 0, "uncompressed mode works")
    radio.t_hz = 84e6
    r.setup_led(32, 33, "red")
    check(radio.led == (32, 33, 1) and abs(radio.led_ticks[0] / 84e6 / 0.5 - 1) < 0.05
          and abs(radio.led_ticks[1] / 84e6 / 0.125 - 1) < 0.05 and abs(radio.led_ticks[2] / 84e3 - 1) < 0.05,
          "LED set up from the measured timer rate (%s)" % (radio.led_ticks,))
    steps, rep, hold = parse_led_spec("hold;255:0:300,0:255:300,128:64:50")
    r.led_sequence(steps, rep, hold)
    check(radio.led_seq == (2, [(300, 255, 0), (300, 0, 255), (50, 128, 64)]), "LED sequence sent (%s)" % (radio.led_seq,))
    r.led_sequence(parse_led_spec(DONE_LED)[0])
    check(radio.led_seq[0] == 0 and len(radio.led_seq[1]) == 6, "default done sequence")
    r.led_sequence([])
    check(radio.led_seq == (0, []), "LED sequence cancelled")
    # baud switching: a cable that manages 500000 keeps it; one that tops out lower falls back by itself
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    radio.max_baud = 600000
    check(r.set_baud(500000, window=0.3) and radio.baudrate == 500000 and radio.stub_baud == 500000
          and radio.pending is None, "baud switch kept when the link works (%s)" % radio.baud_log)
    radio.baud_log.clear()
    check(not r.set_baud(750000, window=0.3) and radio.baudrate == 500000 and radio.stub_baud == 500000
          and "reverted" in radio.baud_log, "baud switch refused and reverted when the cable cannot do it (%s)" % radio.baud_log)
    check(r.hello()[0] > 0 and radio.pending is None, "link still works after the revert")
    radio = FakeRadio({"progmem": prog + b"\xff" * (0x100000 - len(prog)), "q128": data + b"\xff" * (0x1000000 - len(data))},
                      stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    lo, hi = WRITE_OK["q128"][0]
    pat = bytes((i * 3 + 1) & 0xFF for i in range(hi - lo))
    r.write_flash("q128", lo, pat)
    check(radio.images["q128"][lo:hi] == pat, "write_flash programs and verifies a test region")
    r.erase_sectors("q128", lo, lo + SECTOR)
    check(radio.images["q128"][lo:lo + SECTOR] == b"\xff" * SECTOR and radio.images["q128"][lo + SECTOR:hi] == pat[SECTOR:],
          "erase touches only its sector")
    for bad_args in (("q128", 0x7C000, b"\0" * SECTOR), ("progmem", 0xF0000, b"\0" * (SECTOR + 1)), ("progmem", 0x1000, b"\0" * SECTOR)):
        try:
            r.write_flash(*bad_args)
            check(False, "write outside the test regions refused %s" % (bad_args[:2],))
        except DumpError:
            pass
    appimg = bytes((i * 7 + 3) & 0xFF for i in range(5 * SECTOR + 100))
    nw, nk = r.write_image(appimg)
    check(radio.images["progmem"][APP_START:APP_START + len(appimg)] == appimg and nw == 6, "write_image programs an app image (%d sectors)" % nw)
    check(radio.images["progmem"][:APP_START] == (prog + b"\xff" * (0x100000 - len(prog)))[:APP_START], "bootloader area untouched")
    radio.writes.clear()
    appimg2 = appimg[:2 * SECTOR] + b"\x55" + appimg[2 * SECTOR + 1:]
    nw, nk = r.write_image(appimg2)
    check(nw == 1 and nk == 5 and len(radio.writes) >= 2, "second image writes only the changed sector (%d written, %d kept)" % (nw, nk))
    try:
        r.write_image(b"\x01" * (APP_END - APP_START + 1))
        check(False, "oversized image refused")
    except DumpError:
        pass
    # protected regions (board.toml [[flash.protect]]): refused before anything is erased, whoever builds the command
    r.profile = dict(r.profile, protect=[
        {"mem": "progmem", "start": APP_START + 2 * SECTOR, "end": APP_START + 3 * SECTOR, "what": "test (x.md 1)"},
        {"mem": "data", "start": lo + SECTOR, "end": lo + 2 * SECTOR, "what": "test (x.md 1)"}])
    radio.writes.clear()
    for what, call in (("write_image over a protected sector", lambda: r.write_image(appimg)),
                       ("write_flash over a protected sector", lambda: r.write_flash("q128", lo, pat)),
                       ("erase of a protected sector", lambda: r.erase_sectors("q128", lo + SECTOR, lo + 2 * SECTOR)),
                       ("a raw sector erase in a protected region", lambda: r._write_cmd(
                           "q128", bytes([0x20, (lo + SECTOR) >> 16 & 0xFF, 0x10, 0]), "erase", 1.0)),
                       ("a chip erase of a memory with a protected region", lambda: r._write_cmd(
                           "q128", b"\x60", "chip erase", 1.0))):
        try:
            call()
            check(False, "%s refused" % what)
        except DumpError as e:
            check("protected region" in str(e), "%s refused with the reason (%s)" % (what, e))
    check(radio.writes == [], "nothing written by a refused write (%s)" % radio.writes[:3])
    r.erase_sectors("q128", lo, lo + SECTOR)
    check(radio.images["q128"][lo:lo + SECTOR] == b"\xff" * SECTOR, "the sector beside a protected one still erases")
    r.profile = GENERIC
    r.setup_led(32, 33, "red")
    r.reset_chip()
    check(getattr(radio, "flushed", None) == len(radio.inb) + radio.requests,
          "reset_chip waits for its last frame to leave: nothing answers it, and the program exits next")
    check(radio.pokes[-3:] == [(QUAD_ENABLE, 0), (LCSFC_BAUDR, 4), (SYS_SOFT_RSTN, 0)],
          "reset_chip pokes XIP quad off and the reset divider, then the soft reset (%s)" % (radio.pokes[-3:],))
    check(radio.pokes[-7] == (CRASH_SOFT_MARK, CRASH_BOOT_SOFT),
          "reset_chip marks the restart as software's first (%s)" % (radio.pokes[-7:],))
    hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "platform", "hrc7000", "platform.h")
    if os.path.exists(hdr):                     # in the repository: the mark's address is RadioFoundry's crash_boot_t.soft
        m = re.search(r"#define HRC_CRASH_BOOT\s+0x([0-9A-Fa-f]+)u", open(hdr).read())
        check(m is not None and int(m.group(1), 16) + 4 == CRASH_SOFT_MARK, "CRASH_SOFT_MARK is HRC_CRASH_BOOT + 4")
    try:
        parse_led_spec("255:0")
        check(False, "bad LED spec refused")
    except DumpError:
        pass
    check(r.verify("q128", data) == [], "verify: identical image")
    bad = bytearray(data)
    bad[9 * SECTOR + 17] ^= 1
    bad[30 * SECTOR] ^= 0xFF
    check(r.verify("q128", bytes(bad)) == [9 * SECTOR, 30 * SECTOR], "verify names the differing sectors")
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    out = io.BytesIO()
    r.dump(out, "q128", 0x123, 0x123 + 300)
    check(out.getvalue() == data[0x123:0x123 + 300], "unaligned range, two chunks")
    check(r.peek(0x11000038, 4) == b"\x07\xfe\xff\xff", "peek, little-endian words")
    # a data flash read without its GPIO chip select returns 0xFF: the dump must refuse, not write a file of FF
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    saved = MEM["q128"]
    MEM["q128"] = (SSI_DATA, True, None) + saved[3:]
    try:
        r.dump(io.BytesIO(), "q128", 0, 256)
        check(False, "a dead chip select stops the dump")
    except DumpError as e:
        check("does not answer" in str(e), "dead-select message: %s" % e)
    MEM["q128"] = saved
    # a request that never gets a reply ends the run and names the op
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet, retries=2)
    r.start(wait=2, stub=stub)
    radio.silent = set(range(radio.requests, radio.requests + 100))
    try:
        r.sums("q128", 0, 1)
        check(False, "a block that never answers stops the dump")
    except DumpError as e:
        check("0x07" in str(e), "error names the op: %s" % e)
    try:
        r.dump(io.BytesIO(), "progmem", 0, 0x200000)
        check(False, "range beyond the chip refused")
    except DumpError:
        pass
    # entering the stub from RadioFoundry (FakeRFoundry: the debug link STUB command)
    def fake_stub(base, length):                       # an image with the entry code's literal pool, linked at `base`
        return (b"\x80\xc0\x20\x70" + bytes(0x24) + (base + length).to_bytes(4, "little") + (base + 0x34 + 1024).to_bytes(4, "little")
                + (base + 0x632C).to_bytes(4, "little") + bytes(range(256)) * 4)

    spans = {0x17000: (0x18000, "DM-32UV"), 0x30000: (0x20000, "DB25-D"), 0x1D000: (0x12000, "DB25-D with DMR")}
    known = rfoundry_stubs()
    check(set(known) == set(spans), "embedded RadioFoundry stubs cover %s" % sorted(hex(k) for k in spans))
    for base, (size, name) in spans.items():
        link = stub_link(known[base][1])
        check(link is not None and base <= link[1] <= link[2] <= link[0] <= base + size,
              "embedded %s stub is linked inside its span (%s)" % (name, link and [hex(x) for x in link]))
        rfoundry = FakeRFoundry((base, size))                    # no image given: chosen by the span the firmware reports
        ro = Radio(rfoundry, log=quiet)
        ro.attach = lambda: None
        ro.rfoundry_enter("legacy")
        check(rfoundry.entered and bytes(rfoundry.mem[:len(known[base][1])]) == known[base][1],
              "rfoundry_enter picks the legacy stub for the %s span 0x%X" % (name, base))
        rfoundry = FakeRFoundry((base, size))                    # default: the universal stub, at the span's base
        ro = Radio(rfoundry, log=quiet)
        ro.attach = lambda: None
        ro.rfoundry_enter()
        univ = stub_univ_image()
        check(rfoundry.entered and bytes(rfoundry.mem[:len(univ)]) == univ, "rfoundry_enter loads the universal stub into the %s span 0x%X" % (name, base))
        for other, (_, img) in known.items():           # every stub linked for another base is refused, nothing is sent
            if other == base:
                continue
            rfoundry = FakeRFoundry((base, size))
            ro = Radio(rfoundry, log=quiet)
            ro.attach = lambda: None
            try:
                ro.rfoundry_enter(img)
                check(False, "a stub linked for 0x%X is refused by the 0x%X span" % (other, base))
            except DumpError:
                check(not rfoundry.entered and rfoundry.total is None, "stub for 0x%X refused before BEGIN on the 0x%X span" % (other, base))
    rfoundry = FakeRFoundry((0x40000, 0x10000))
    try:
        Radio(rfoundry, log=quiet).rfoundry_enter("legacy")
        check(False, "an unknown span with no legacy image is refused")
    except DumpError as e:
        check("--rfoundry-stub" in str(e), "unknown span message names --rfoundry-stub")
    rfoundry = FakeRFoundry((0x17000, 0x18000))
    try:
        Radio(rfoundry, log=quiet).rfoundry_enter(stub)           # not a stub at all (no entry code)
        check(False, "an image without the monitor's entry code is refused")
    except DumpError:
        pass
    ro = Radio(rfoundry, log=quiet)
    ro.attach = lambda: None
    ro.rfoundry_enter(fake_stub(0x17000, 0x18000))
    check(rfoundry.entered, "rfoundry_enter loads a given image that fits the span")
    mute = FakeRadio(images)
    mute.out.clear()
    try:
        Radio(mute, log=quiet).start(wait=0.3, stub=stub)
        check(False, "no radio: the wait ends")
    except DumpError as e:
        check("switch it on" in str(e), "no-radio message: %s" % e)
    # --load-base: the DB25-D's first stage takes the stub at its own RAM base, not the boot ROM's
    at = FakeRadio(images, stub_len=len(stub), load=0x30000)
    Radio(at, log=quiet).start(wait=2, stub=stub, load=0x30000)
    check(at.running and not at.errors, "a stub written and started at a given base: %s" % at.errors)
    try:
        check(len(stub_image()) > 0, "embedded stub decodes")
    except DumpError as e:
        check(False, "embedded stub: %s" % e)
    # ---- the universal stub (stub/README.md: RAM placement)
    univ = stub_univ_image()
    lay = univ_layout(univ)
    check(lay is not None and lay[0] == 0x30000 and lay[0] + len(univ) <= lay[1] <= 0x40000,
          "universal stub runs at 0x30000 and stays below 0x40000 (%s)" % (lay and [hex(x) for x in lay]))
    check(univ_layout(stub_image()) is None and all(univ_layout(v[1]) is None for v in rfoundry_stubs().values()),
          "legacy stubs are told apart from the universal one")
    # every way in: ROM / first stage at 0x10000, RadioFoundry's spans, run address itself, a stock DB25-D's console window
    for load, win in ((0x10000, None), (0x17000, (0x17000, 0x18000)), (0x1D000, (0x1D000, 0x12000)), (0x30000, (0x30000, 0x20000)),
                      (0x54000, (0x54000, 0x4000))):
        try:
            check_univ_load(univ, load, win)
        except DumpError as e:
            check(False, "universal stub loads at 0x%X: %s" % (load, e))
    for load, win, why in ((0x30800, None, "overlapping its run copy"), (0x2F000, None, "overlapping from below"),
                           (0x17002, None, "unaligned"), (0x57000, None, "past the end of SRAM"), (0x8000, None, "below SRAM"),
                           (0x1D000, (0x1D000, 0x1000), "a window smaller than the image")):
        try:
            check_univ_load(univ, load, win)
            check(False, "universal stub refused %s (0x%X)" % (why, load))
        except DumpError:
            pass
    rfoundry = FakeRFoundry((0x44000, 0x14000))                   # it says board=DB25-D: a --board for another radio is refused
    other = next(b for b in sorted(C7_BOARDS) if "DB25-D" not in C7_BOARDS[b]["names"])
    try:
        ro = Radio(rfoundry, log=quiet)
        ro.attach = lambda: None
        ro.rfoundry_enter(board=other)
        check(False, "rfoundry_enter refuses a --board the running firmware does not report")
    except DumpError as e:
        check(rfoundry.total is None and "wrong port" in str(e), "--board %s on a DB25-D refused before BEGIN: %s" % (other, e))
    rfoundry = FakeRFoundry((0x44000, 0x14000))
    ro = Radio(rfoundry, log=quiet)
    ro.attach = lambda: None
    ro.rfoundry_enter(board=other, force=True)
    check(rfoundry.entered, "--force enters anyway")
    nm = {k: C7_BOARDS[k]["names"][0].encode() + b"\0" for k in C7_BOARDS if C7_BOARDS[k]["names"]}
    a_, b_ = sorted(nm)[:2]
    img = b"\0" * 64 + b"RadioFoundry\0" + nm[a_] + b"\xff" * 64
    check(image_board_check(img, a_) is None, "an image naming its own board passes")
    why = image_board_check(img, b_)
    check(why is not None and C7_BOARDS[a_]["name"] in why, "an image for the %s refused for the %s: %s" % (a_, b_, why))
    check(image_board_check(b"stock firmware" + nm[a_], b_) is None, "a stock image is not judged")
    check(image_board_check(img, None) is None, "no target board: not judged")
    rfoundry = FakeRFoundry((0x44000, 0x14000))
    ro = Radio(rfoundry, log=quiet)
    ro.attach = lambda: None
    ro.rfoundry_enter(board=c7_board("DB25-D"))
    check(rfoundry.entered, "rfoundry_enter takes a --board the running firmware reports")
    rfoundry = FakeRFoundry((0x2F000, 0x4000))                    # a span that would overlap the run copy: refused before BEGIN
    try:
        Radio(rfoundry, log=quiet).rfoundry_enter()
        check(False, "rfoundry_enter refuses a span overlapping the universal stub's run address")
    except DumpError:
        check(rfoundry.total is None, "overlapping span refused before BEGIN")
    # power-on with the universal stub, CONFIG, the HELLO echo, writes gated on it, the NAND path
    nand = bytearray(images["q128"][:16 * SECTOR])
    nand[SECTOR:2 * SECTOR] = b"\xff" * SECTOR
    for board in sorted(C7_BOARDS) + [None]:
        b = rfoundryboards.get(board) if board else None
        imgs = dict(images, nand=bytes(nand))
        radio = FakeRadio(imgs, stub_len=(len(univ) + BLOCK - 1) // BLOCK * BLOCK)
        radio.univ = {"run": 0x30000, "end": lay[1], "board": 0, "flags": 0, "honor": True}
        r = Radio(radio, log=quiet)
        r.start(wait=2)
        check(radio.running and not radio.errors and bytes(radio.loaded[:len(univ)]) == univ,
              "the universal stub is the default power-on image, at 0x10000 (%s)" % radio.errors)
        check(r.v6 is not None and r.v6["run"] == 0x30000 and r.v6["load"] == 0x10000, "HELLO v6 parsed")
        try:
            r.write_image(images["progmem"][APP_START:APP_START + 4 * SECTOR])
            check(False, "a write before CONFIG is refused")
        except DumpError as e:
            check("configuration" in str(e), "write before CONFIG refused: %s" % e)
        r.configure(board)
        prof = C7_BOARDS[board] if board else GENERIC
        cfg = radio.configs[-1]
        check(cfg[8:10] == bytes([prof["latch"], prof["key"]]) and cfg[12] == prof["id"] and cfg[14] == CFG_UART_THIS,
              "CONFIG carries the %s profile" % prof["name"])
        check(23e6 < int.from_bytes(cfg[:4], "big") < 25e6, "CONFIG carries the measured clock")
        check(r.configured is prof, "the %s profile is confirmed by the HELLO echo" % prof["name"])
        nw, nk = r.write_image(images["progmem"][APP_START:APP_START + 4 * SECTOR])
        check(nw == 0 and nk == 4, "writing the same image after CONFIG changes nothing")
        if b and b.first_stage:		# a first stage in the radio: only an image that starts with one goes over it
            pm = bytearray(radio.images["progmem"])
            pm[APP_START:APP_START + 4] = STAGE_MAGIC
            radio.images["progmem"] = bytes(pm)
            try:
                r.write_image(images["progmem"][APP_START:APP_START + 4 * SECTOR])
                check(False, "the application alone over a first stage is refused")
            except DumpError as e:
                check("install image" in str(e), "application over a first stage refused: %s" % e)
            app = images["progmem"][APP_START:APP_START + 4 * SECTOR]
            nw, nk = r.write_image(app, replace_stage=True)
            check(nw >= 1, "--replace-stage writes the application over the first stage")
            radio.images["progmem"] = bytes(pm)
            nw, nk = r.write_image(bytes(pm[APP_START:APP_START + 4 * SECTOR]))
            check(nk == 4, "an install image over a first stage is written")
        check(r.mem_size("progmem") == prof["progmem"], "the %s's program flash is %d KB" % (prof["name"], prof["progmem"] >> 10))
        if prof["nand"]:
            out = io.BytesIO()
            r.dump(out, "nand", 0, 16 * SECTOR)
            check(out.getvalue() == bytes(nand), "NAND read through the stub's NAND path")
        else:
            try:
                r.dump(io.BytesIO(), "nand", 0, SECTOR)
                check(False, "NAND refused on a radio without one")
            except DumpError:
                pass
    radio = FakeRadio(images, stub_len=(len(univ) + BLOCK - 1) // BLOCK * BLOCK)
    radio.univ = {"run": 0x30000, "end": lay[1], "board": 0, "flags": 0, "honor": False}
    r = Radio(radio, log=quiet)
    r.start(wait=2)
    try:
        r.configure(sorted(C7_BOARDS)[0])
        check(False, "a stub that ignores CONFIG is caught by the HELLO echo")
    except DumpError:
        try:
            r.write_image(images["progmem"][APP_START:APP_START + SECTOR])
            check(False, "no write after a failed CONFIG")
        except DumpError:
            pass
    # protocol 7: one WRITE request per changed sector, its read-back CRC checked at once; a v6 stub keeps the SPI path
    for wop in (True, False):
        pm = images["progmem"] + b"\xff" * (0x100000 - len(images["progmem"]))
        radio = FakeRadio(dict(images, progmem=pm), stub_len=(len(univ) + BLOCK - 1) // BLOCK * BLOCK)
        radio.univ = {"run": 0x30000, "end": lay[1], "board": 0, "flags": 0, "honor": True}
        radio.write_op = wop
        r = Radio(radio, log=quiet)
        r.start(wait=2)
        r.configure(sorted(C7_BOARDS)[0])
        check(r.sector_op == wop, "HELLO v%d: whole-sector writes %s" % (7 if wop else 6, "on" if wop else "off"))
        app = bytes((i * 13 + 5) & 0xFF for i in range(5 * SECTOR)) + b"\xff" * SECTOR + bytes(range(256)) * 4
        radio.images["progmem"] = radio.images["progmem"][:APP_START + 5 * SECTOR] + b"\x00" * SECTOR + radio.images["progmem"][APP_START + 6 * SECTOR:]
        nw, nk = r.write_image(app)
        ok = radio.images["progmem"][APP_START:APP_START + len(app)] == app
        ops = [w[1] for w in radio.writes]
        if wop:
            check(ok and nw == 7 and ops == [0x0E] * 7, "WRITE: each changed sector is one request (%d written, %s)" % (nw, ops))
        else:
            check(ok and nw == 7 and 0x0E not in ops and ops.count(0x20) == 7, "v6 stub: written by SPI requests as before")
        if wop:
            app2 = app[:SECTOR] + bytes(SECTOR) + app[2 * SECTOR:]
            radio.bad_write = 1
            n0 = r.retried
            nw, _ = r.write_image(app2)
            check(nw == 1 and radio.images["progmem"][APP_START:APP_START + len(app2)] == app2 and r.retried == n0 + 1,
                  "WRITE: a sector that reads back wrong is written again (%d retried)" % (r.retried - n0))
            radio.bad_write = 3
            try:
                r.write_image(app)
                check(False, "a sector that never reads back right fails the write")
            except DumpError as e:
                check("3 writes" in str(e), "WRITE: gives up after 3 tries: %s" % e)
            for st, why in ((NOWEL, "write-protected"), (SPITIMEOUT, "stayed busy")):
                radio.write_status = st
                before = radio.images["progmem"]
                try:
                    r.write_image(app2)
                    check(False, "WRITE status 0x%02X stops the write" % st)
                except DumpError as e:
                    check(why in str(e) and radio.images["progmem"] == before, "WRITE status 0x%02X: %s" % (st, e))
            radio.write_status = 0
            lo, hi = WRITE_OK["progmem"][0]
            r.write_flash("progmem", lo, bytes(range(256)) * 16 * ((hi - lo) // SECTOR))
            r.erase_sectors("progmem", lo, hi)
            check(radio.images["progmem"][lo:hi] == b"\xff" * (hi - lo), "WRITE without data erases the sector")
            try:
                r.write_flash("progmem", 0x1000, b"\0" * SECTOR)
                check(False, "WRITE outside the allowed regions refused")
            except DumpError:
                pass
    # raised rates (protocol 7): auto finds the fastest rate that carries full frames both ways and remembers it; a rate
    # that stops holding is given up by silence alone, because the stub goes home when its link goes quiet
    import io
    import shutil
    import tempfile
    saved = {k: globals()[k] for k in ("LINK_WINDOW", "PROOF_ROUNDS", "BAUD_CACHE")}
    tmp = tempfile.mkdtemp()
    globals().update(LINK_WINDOW=0.2, PROOF_ROUNDS=1, BAUD_CACHE=os.path.join(tmp, "baud.json"))

    def v7(max_baud, max_in=None):
        pm = images["progmem"] + b"\xff" * (0x100000 - len(images["progmem"]))
        radio = FakeRadio(dict(images, progmem=pm), stub_len=(len(univ) + BLOCK - 1) // BLOCK * BLOCK)
        radio.univ = {"run": 0x30000, "end": lay[1], "board": 0, "flags": 0, "honor": True}
        radio.write_op, radio.t_hz, radio.max_baud, radio.max_in = True, 84e6, max_baud, max_in
        r = Radio(radio, log=quiet)
        r.start(wait=2)
        r.configure(sorted(C7_BOARDS)[0])
        return radio, r

    try:
        radio, r = v7(600000, max_in=400000)
        check(not r.set_baud(437500) and r.rate() == 115200 and 437500 in r.bad_rates and r.hello()[0] == 7,
              "a rate whose replies hold but whose long frames to the radio are damaged is refused")
        radio, r = v7(600000, max_in=400000)
        got = r.auto_baud()
        check(got == 375000 and radio.stub_baud == 375000 and r.home_baud == 115200 and recall_baud(r.baud_key()) == 375000,
              "auto: the fastest rate that holds both ways, remembered (%s, tried %s)" % (got, radio.baud_log))
        radio, r = v7(600000, max_in=400000)
        check(r.auto_baud() == 375000 and [x for x in radio.baud_log if x.startswith("to")] == ["to 375000"],
              "auto: the remembered rate is tried first and alone (%s)" % radio.baud_log)
        want = r.sums("progmem", APP_START, 2)
        radio.max_baud = 300000                         # the cable gets worse mid-session
        check(r.sums("progmem", APP_START, 2) == want and r.rate() == 115200 and radio.stub_baud == 115200
              and "home" in radio.baud_log and recall_baud(r.baud_key()) is None,
              "a raised rate that stops holding: both sides go home by silence, the request is answered, the rate "
              "is forgotten (%s)" % radio.baud_log)
        radio, r = v7(600000, max_in=400000)
        check(r.auto_baud() == 375000, "auto again after the rate was forgotten")
        check_id = r.check_id                           # worse once the streamed read (op 09) is under way, not before

        def worse(mem):
            check_id(mem)
            radio.max_baud = 300000
        r.check_id = worse
        out = io.BytesIO()
        r.dump(out, "progmem", APP_START, APP_START + 2 * PACK * SECTOR)
        check(out.getvalue() == radio.images["progmem"][APP_START:APP_START + 2 * PACK * SECTOR] and r.rate() == 115200
              and radio.stub_baud == 115200, "a raised rate that stops holding during a streamed read: the read goes home "
              "and finishes there (%s)" % radio.baud_log)
        radio, r = v7(150000)
        check(r.auto_baud() == 115200 and r.rate() == 115200 and recall_baud(r.baud_key()) == 0 and r.hello()[0] == 7,
              "auto: nothing faster holds, so the link stays at the home rate and that is remembered")
        radio, r = v7(150000)
        check(r.auto_baud() == 115200 and not radio.baud_log, "auto: a cable known to hold nothing faster is not tried again")
    finally:
        globals().update(saved)
        shutil.rmtree(tmp, ignore_errors=True)
    # an old stub (HELLO without the v6 fields): no CONFIG, writes allowed as before
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    r.configure(sorted(C7_BOARDS)[0])
    check(r.v6 is None and not radio.configs, "an old stub gets no CONFIG")
    r.write_image(images["progmem"][APP_START:APP_START + SECTOR])
    # the board named still brings its protected regions
    for p in (x for x in C7_BOARDS[sorted(C7_BOARDS)[0]]["protect"] if x["mem"] in PROTECT_MEM.values()):
        mem = next(m for m, n in PROTECT_MEM.items() if n == p["mem"])
        try:
            r.check_write_range(mem, p["start"] - p["start"] % SECTOR, p["start"] - p["start"] % SECTOR + SECTOR)
            check(False, "an old stub: a protected region still refused")
        except DumpError as e:
            check("protected region" in str(e), "an old stub: a protected region still refused (%s)" % e)
    print("stubtool self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                    "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


def parse_range(s, size):
    if not s:
        return 0, size
    a, _, b = s.partition(":")
    return int(a, 0) if a else 0, int(b, 0) if b else size


class UdpPort:
    """The debug link over UDP (port 3334 of the radio's address in USB network mode, docs/spec/usb-network.md), shaped
    like the pyserial calls this tool makes. Used only to send the hand-over to an STM32 radio."""

    def __init__(self, addr):
        import socket
        host, _, port = addr.partition(":")
        self.addr = (socket.gethostbyname(host), int(port or 3334))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.timeout = 0.1
        self.pending = bytearray()

    def read(self, n=1):
        if not self.pending:
            self.sock.settimeout(self.timeout or 0.001)
            try:
                self.pending += self.sock.recvfrom(2048)[0]
            except OSError:
                return b""
        out = bytes(self.pending[:n])
        del self.pending[:n]
        return out

    def write(self, data):
        self.sock.sendto(bytes(data), self.addr)
        return len(data)

    def reset_input_buffer(self):
        self.pending.clear()
        self.sock.setblocking(False)
        try:
            while self.sock.recv(2048):
                pass
        except OSError:
            pass

    def flush(self):
        pass

    def close(self):
        self.sock.close()


def version_board(radio):
    """The board= name from a running RadioFoundry's debug link VERSION reply, or None."""
    try:
        r = radio.dl_request(0x02, b"")
    except DumpError:
        return None
    if not r or r[0] != 0:
        return None
    for line in r[1:].decode("ascii", "replace").splitlines():
        if line.startswith("board="):
            return line[6:].strip()
    return None


def find_rfoundry_port():
    """The one serial port with RadioFoundry's USB identity (1FC9:0094), which the STM32 stub also uses"""
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x1FC9, 0x0094)]
    if len(ports) != 1:
        raise OSError("%d serial ports with USB ID 1FC9:0094: give --stub-port" % len(ports))
    return ports[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("port", nargs="?", help="serial port or pyserial URL (rfc2217://HOST:PORT), or udp:ADDR[:PORT] for an "
                    "STM32 radio in USB network mode")
    ap.add_argument("--stub-port", metavar="PORT", help="with a udp: port: the serial port the stub comes up on "
                    "(default: the one USB device 1FC9:0094)")
    ap.add_argument("out", nargs="?", help="old form: output file for the memory chosen with --mem")
    ap.add_argument("--mem", choices=sorted(MEM), default="progmem", help="memory for the old form")
    ap.add_argument("--progmem", metavar="FILE", help="read the 1 MB program flash into FILE")
    ap.add_argument("--q128", metavar="FILE", help="read the 16 MB data flash into FILE")
    ap.add_argument("--progmem-range", metavar="START:END", help="byte range, any alignment (default all)")
    ap.add_argument("--q128-range", metavar="START:END")
    ap.add_argument("--nand", metavar="FILE", help="DB25-D (--board db25d or found from RadioFoundry): read the SPI NAND into FILE "
                    "(2 KB pages without the spare bytes; give --nand-range, the chip is 256 MB)")
    ap.add_argument("--nand-range", metavar="START:END")
    ap.add_argument("--verify-nand", metavar="FILE", help="compare the NAND from --nand-range's start (default 0) with FILE by CRC")
    ap.add_argument("--start", type=lambda s: int(s, 0), help="old form: first byte")
    ap.add_argument("--end", type=lambda s: int(s, 0), help="old form: end (exclusive)")
    ap.add_argument("--verify-progmem", metavar="FILE", help="compare the program flash with FILE by CRC (no data transfer)")
    ap.add_argument("--verify-q128", metavar="FILE", help="compare the data flash with FILE by CRC")
    ap.add_argument("--write-q128", metavar="FILE", help="STM32 radios: write FILE into the SPI flash from --q128-range's "
                    "start (default 0), changed 4 KB sectors only, each verified by CRC")
    ap.add_argument("--secreg", metavar="FILE", help="STM32 radios: read the SPI flash's three 256-byte security registers "
                    "(the DM-1701 family's factory calibration and flags) into FILE (768 bytes), and print their lock bits")
    ap.add_argument("--write-secreg", metavar="FILE", help="STM32 radios: write FILE (768 bytes, registers 1-3) into the "
                    "security registers, only those that differ, each read back; the old content is saved to --backup-dir "
                    "first. Refused for a locked register. Needs --confirm-secreg")
    ap.add_argument("--confirm-secreg", action="store_true", help="confirm --write-secreg: it rewrites factory data")
    ap.add_argument("--probe", action="store_true", help="chip IDs, a first block of each flash and some registers")
    ap.add_argument("--peek", metavar="ADDR:LEN", help="print LEN bytes of radio memory (hex dump)")
    ap.add_argument("--retries", type=int, default=6)
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the radio to be switched on")
    ap.add_argument("--slow-cable", action="store_true", help="boot ROM: send the answer to its greeting every 30 ms "
                    "from the start, for a cable or USB hub too slow for the ROM's 48 ms window. Radio OFF before the "
                    "tool starts: a running firmware would get the answers too")
    ap.add_argument("--led-red", type=int, default=LED_RED, help="GPIO pin index of the red LED (default %d, 255 = none)" % LED_RED)
    ap.add_argument("--led-green", type=int, default=LED_GREEN, help="GPIO pin index of the green LED (default %d, 255 = none)" % LED_GREEN)
    ap.add_argument("--led-status", choices=("red", "green", "both", "off"), default="both",
                    help="which LED blinks while the stub is up: 500 ms phases waiting, 125 ms while data moves")
    ap.add_argument("--led-seq", metavar="SPEC", help="play an LED sequence now, e.g. 255:0:300,0:255:300 (RED:GREEN:MS "
                    "per step, brightness 0-255; prefix repeat; or hold;)")
    ap.add_argument("--done-led", metavar="SPEC", default=DONE_LED,
                    help="sequence to play when the run finishes (default: three green flashes; `none` to skip)")
    ap.add_argument("--led-test", action="store_true", help="walk the LEDs through red, green, both and PWM mixes (watch the radio)")
    ap.add_argument("--boot", metavar="reset|jump:ADDR", help="when done, restart the radio into its firmware: `reset` soft-resets the chip, "
                    "`jump:0x0300C000` jumps to an address")
    ap.add_argument("--baud", metavar="N|auto|off", help="serial link rate after the boot. Default with a protocol 7 stub "
                    "and a read or --write-app: auto, the fastest rate that carries full frames both ways (the UART has "
                    "whole divisors only: clock / (16 * d)), remembered per port and radio; if it stops holding the link "
                    "goes back to the starting rate by itself. N asks for one rate (tested before it is kept); off stays")
    ap.add_argument("--write-test", choices=("progmem", "q128", "both"), help="erase, program a test pattern into, read back and erase "
                    "again the sectors reserved for write tests (see WRITE_OK); nothing else is touched")
    ap.add_argument("--rfoundry-enter", action="store_true", help="the radio is ON and runs RadioFoundry (built with CONFIG_STUB_ENTRY=y): load "
                    "the stub into its RAM over the debug link and jump to it, instead of the boot-ROM load")
    ap.add_argument("--rfoundry-stub", metavar="FILE", help="with --rfoundry-enter: this stub image instead of the embedded one (which is chosen by the RAM span the "
                    "firmware reports); refused unless it is linked for that span")
    ap.add_argument("--write-app", metavar="IMAGE", help="program an RadioFoundry application image (raw .bin linked for 0x0300C000) into the "
                    "program flash at 0xC000: only changed 4 KB sectors are written, then verified; the bootloader is never touched. "
                    "Use with --rfoundry-enter (radio running RadioFoundry) or the boot-ROM load; finish with --boot reset. On a radio with "
                    "RadioFoundry's first stage at 0xC000 (DB25-D) give the _install.bin, which starts with it")
    ap.add_argument("--replace-stage", action="store_true", help="let --write-app put an image that does not start with "
                    "RadioFoundry's first stage over one (the radio then starts only what the image holds at 0xC000)")
    ap.add_argument("--attach", action="store_true", help="the stub from an earlier run is still up: skip the boot")
    ap.add_argument("--stub", metavar="FILE", help="load this stub image instead of the embedded universal one (a universal "
                    "image, or a position-dependent one linked for the load address)")
    ap.add_argument("--legacy-stub", action="store_true", help="use the position-dependent stubs embedded before the universal "
                    "one (one per RAM span: the boot ROM's 0x10000, RadioFoundry's 0x17000, the DB25-D's 0x30000 / 0x1D000). Fallback only")
    ap.add_argument("--load-base", metavar="ADDR", type=lambda s: int(s, 0),
                    help="power-on load: write and start the stub at ADDR (default 0x%X). The universal stub runs from any "
                         "base that doesn't overlap its run area; a legacy stub must be linked for it (the DB25-D's first stage "
                         "took the legacy 0x30000 stub at 0x30000)" % LOAD_ADDR)
    ap.add_argument("--force", action="store_true",
                    help="write even when the radio's firmware or the image names another board than the one targeted. Two "
                         "cables with one serial number can swap port names, so a refusal usually means the wrong port")
    import f4stub
    ap.add_argument("--board", choices=sorted(b.id for b in rfoundryboards.BOARDS.values()
                                              if b.stub or b.id in f4stub.BOARDS),
                    help="the radio: a Cortex-M one (%s: stub/f4stub.py, the STM32F4, GD32F30x or AT32F43x stub) "
                         "or an HR_C7000 one (%s: the board profile the "
                         "universal stub gets). Found from the radio's VERSION reply with --rfoundry-enter; give it at "
                         "power-on and with --attach (without it the stub gets a profile that drives no board pins)"
                         % (", ".join(sorted(f4stub.BOARDS)), ", ".join(sorted(C7_BOARDS))))
    ap.add_argument("--backup-dir", metavar="DIR", help="STM32 --write-app: where a sector that also holds codec bytes "
                    "is saved before it is erased (default the home directory, never a git tree)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        import f4stub
        return self_test() | f4stub.self_test()
    if not a.port:
        ap.error("give PORT")
    jobs = []                                   # (mem, file, start, end, twice)
    if a.out:
        size = MEM[a.mem][3]
        jobs.append((a.mem, a.out, a.start or 0, a.end if a.end is not None else size))
    for mem, f, rng in (("progmem", a.progmem, a.progmem_range), ("q128", a.q128, a.q128_range), ("nand", a.nand, a.nand_range)):
        if f:
            if mem == "nand" and not rng:
                ap.error("--nand needs --nand-range (the chip is 256 MB)")
            jobs.append((mem, f, rng))
    if not (jobs or a.probe or a.peek or a.verify_progmem or a.verify_q128 or a.verify_nand or a.led_test or a.led_seq or a.boot or a.write_test or a.write_app or a.write_q128
            or a.secreg or a.write_secreg):
        ap.error("nothing to do: give --progmem, --q128, --verify-*, --probe, --peek or OUT")
    if (a.secreg or a.write_secreg) and a.board in C7_BOARDS:
        ap.error("--secreg and --write-secreg are for the STM32 radios; the HR_C7000 stub has no security register ops")
    if a.write_secreg:
        # checked before the radio is touched
        if not a.confirm_secreg:
            ap.error("--write-secreg rewrites the radio's factory calibration: add --confirm-secreg")
        if os.path.getsize(a.write_secreg) != 768:
            ap.error("%s is %d bytes: --write-secreg takes the 768 bytes --secreg reads (registers 1-3)"
                     % (a.write_secreg, os.path.getsize(a.write_secreg)))
    try:
        import serial
    except ImportError:
        print("pyserial is missing: pip install pyserial", file=sys.stderr)
        return 1
    try:
        stub = open(a.stub, "rb").read() if a.stub else None
        if a.port.startswith("udp:"):
            # a radio in USB network mode: the hand-over goes over UDP; the stub then enumerates as a serial port
            port = UdpPort(a.port[4:])
            reopen = lambda: open_port(serial, a.stub_port or find_rfoundry_port())
        else:
            port = open_port(serial, a.port)
            import f4stub
            reopen = f4stub.PortFollower(serial, a.port)
        radio = Radio(port, retries=a.retries)
        # The Cortex-M radios (STM32F4, GD32F30x, AT32F43x) run their own stub over USB: stub/f4stub.py
        if a.board is None and a.rfoundry_enter:
            name = version_board(radio)
            a.board = f4stub.board_for(name) if name else None
            if a.board is None and name:
                a.board = c7_board(name)
                if a.board is None:
                    print("RadioFoundry reports board %r: not one with a stub profile, using the generic one" % name)
        if a.board in f4stub.BOARDS:
            return f4stub.run(a, port, reopen)
        if a.write_app:                         # before the stub goes in: a refusal leaves the radio as it was
            why = image_board_check(open(a.write_app, "rb").read(), a.board)
            if why and not a.force:
                raise DumpError(why + ": wrong image or wrong port? Nothing was sent (--force writes it anyway)")
            if why:
                print("warning: %s; writing it anyway (--force)" % why)
        if a.write_q128:
            raise DumpError("--write-q128 is for the STM32 radios (DM-1701, MD-380)")
        if a.secreg or a.write_secreg:
            raise DumpError("--secreg and --write-secreg are for the STM32 radios (DM-1701, MD-380); the HR_C7000 stub has "
                            "no security register ops")
        if a.rfoundry_enter:
            radio.rfoundry_enter(open(a.rfoundry_stub, "rb").read() if a.rfoundry_stub else ("legacy" if a.legacy_stub else None),
                                 board=a.board, force=a.force)
        elif a.attach:
            radio.attach()
            radio.configure(a.board)
        else:
            load = LOAD_ADDR if a.load_base is None else a.load_base
            if stub is None and a.legacy_stub:
                if load == LOAD_ADDR:
                    stub = stub_image()
                elif load in rfoundry_stubs():
                    stub = rfoundry_stubs()[load][1]
                else:
                    raise DumpError("no legacy stub is linked for 0x%X: give --stub" % load)
            print("radio OFF? switch it on now (waiting up to %d s)" % a.wait)
            radio.start(a.wait, stub, load, slow=a.slow_cable)
            radio.configure(a.board)
        jobs = [(mem, f) + parse_range(rng, radio.mem_size(mem)) for mem, f, rng in jobs]
        if a.baud == "off":
            pass
        elif radio.sector_op and (a.baud == "auto" or (a.baud is None and (jobs or a.write_app))):
            b = radio.auto_baud()               # protocol 7: proven both ways, and it falls back by itself if it stops holding
            print("serial link at %d baud" % b)
        elif a.baud == "auto":                  # an older stub: no way back from a raised rate that stops holding
            for b in sorted((r for r in (radio.baud_candidates() if radio.uart_hz else [750000, 500000, 375000, 250000])
                             if r <= 750000), reverse=True)[:4]:
                if radio.set_baud(b):
                    break
        elif a.baud:
            radio.set_baud(int(a.baud))
        radio.setup_led(a.led_red, a.led_green, a.led_status)
        if a.led_test:
            for what, steps in (("red only", [(255, 0, 1500)]), ("green only", [(0, 255, 1500)]),
                                ("both full", [(255, 255, 1500)]), ("red 255 + green 40", [(255, 40, 1500)]),
                                ("red 40 + green 255", [(40, 255, 1500)]), ("red 90 + green 90", [(90, 90, 1500)]),
                                ("fade red to green", [(255 - i * 16, i * 16, 100) for i in range(16)])):
                print("LED:", what)
                radio.led_sequence(steps)
                time.sleep(sum(x[2] for x in steps) / 1000 + 0.3)
        if a.led_seq:
            steps, rep, hold = parse_led_spec(a.led_seq)
            radio.led_sequence(steps, rep, hold)
        if a.write_app:
            image = open(a.write_app, "rb").read()
            if a.board is None:                 # the profile a stub attached to already had: checked before any write
                why = image_board_check(image, next((k for k, b in C7_BOARDS.items() if b is radio.profile), None))
                if why and not a.force:
                    raise DumpError(why + ": wrong image or wrong port? Nothing was written (--force writes it anyway)")
                if why:
                    print("warning: %s; writing it anyway (--force)" % why)
            t0 = time.monotonic()
            nw, nk = radio.write_image(image, replace_stage=a.replace_stage, progress=lambda i, n: print("\rwriting sector %d of %d changed" % (i + 1, n), end="", flush=True))
            print("\n%s: %d bytes, %d sectors written, %d unchanged, verified by CRC, %.0f s" % (a.write_app, len(image), nw, nk, time.monotonic() - t0))
            print("sha256", fingerprint(a.write_app))
            if not a.boot:
                print("run again with --attach --boot reset (or power-cycle) to start the new firmware")
        if a.write_test:
            for mem in (("progmem", "q128") if a.write_test == "both" else (a.write_test,)):
                lo, hi = WRITE_OK[mem][0]
                before = b"".join(radio.read_flash(mem, x, CHUNK) for x in range(lo, hi, CHUNK))
                if before != b"\xff" * (hi - lo):
                    raise DumpError("%s 0x%X-0x%X is not blank any more: something else uses it, pick another region" % (MEM[mem][4], lo, hi))
                pattern = bytes((i * 131 + (i >> 8) * 7 + 5) & 0xFF for i in range(hi - lo))
                radio.write_flash(mem, lo, pattern)
                back = b"".join(radio.read_flash(mem, x, CHUNK) for x in range(lo, hi, CHUNK))
                print("%s 0x%X-0x%X: written, verified by CRC, read back %s" % (MEM[mem][4], lo, hi, "identical" if back == pattern else "DIFFERENT"))
                if back != pattern:
                    raise DumpError("read-back differs from what was written")
                radio.erase_sectors(mem, lo, hi)
                after = b"".join(radio.read_flash(mem, x, CHUNK) for x in range(lo, hi, CHUNK))
                print("%s: erased again, %s" % (MEM[mem][4], "blank" if after == b"\xff" * (hi - lo) else "NOT BLANK"))
                if after != b"\xff" * (hi - lo):
                    raise DumpError("region not blank after the final erase")
        if a.probe:
            for mem in sorted(MEM):
                print("%-8s JEDEC %s, first 16 bytes %s" % (mem, radio.jedec(mem).hex(), radio.read_flash(mem, 0, 16).hex()))
            for name, addr in (("GPIOA DR", GPIOA), ("GPIOA DDR", GPIOA + 4), ("IO_DIPLEX1", IO_DIPLEX1)):
                print("%-10s %s" % (name, radio.peek(addr, 4)[::-1].hex()))
        if a.peek:
            addr, n = parse_range(a.peek, 0)
            data = b"".join(radio.peek(addr + i, min(CHUNK, n - i)) for i in range(0, n, CHUNK))
            for i in range(0, len(data), 16):
                print("%08X  %s" % (addr + i, data[i:i + 16].hex(" ")))
        for mem, path in (("progmem", a.verify_progmem), ("q128", a.verify_q128), ("nand", a.verify_nand)):
            if path:
                data = open(path, "rb").read()
                data += b"\xff" * (-len(data) % SECTOR)
                start = parse_range(a.nand_range, radio.mem_size(mem))[0] if mem == "nand" and a.nand_range else 0
                bad = radio.verify(mem, data, start)
                print("%s vs %s: %s" % (MEM[mem][4], path, "identical" if not bad else
                      "%d sectors differ, first at 0x%X" % (len(bad), bad[0])))
        if radio.v6 is not None and (jobs or a.write_app or a.verify_progmem or a.verify_nand):
            print("stub RAM:", radio.footprint())
        for mem, path, start, end in jobs:
            with open(path, "wb") as f:
                t0 = time.monotonic()
                radio.dump(f, mem, start, end,
                           progress=lambda x, e: print("\r%s 0x%X / 0x%X" % (mem, x, e), end="", flush=True))
            print()
            print("%s: %d bytes in %.0f s, %d retries; %d blank sectors not sent, %d LZ4-compressed, %d bytes on the cable"
                  % (path, end - start, time.monotonic() - t0, radio.retried, radio.skipped, radio.packed, radio.wire))
            print("sha256", fingerprint(path))
        if jobs and a.done_led != "none" and not a.boot:
            radio.led_sequence(*parse_led_spec(a.done_led))
        if a.boot:
            if a.boot == "reset":
                radio.reset_chip()
                print("chip reset sent: the radio restarts into its firmware")
            elif a.boot.startswith("jump:"):
                radio.jump(int(a.boot[5:], 0))
                print("jump sent")
            else:
                raise DumpError("--boot wants reset or jump:ADDR")
        return 0
    except (DumpError, OSError) as e:
        print("\nstubtool: %s" % e, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nstubtool: stopped", file=sys.stderr)
        return 1


if __name__ == "__main__":
    # f4stub imports this file as "stubtool": without this it would load a second copy, and its DumpError would be a
    # different class from the one main() catches (an STM32 error printed as a traceback)
    sys.modules.setdefault("stubtool", sys.modules[__name__])
    sys.exit(main())
