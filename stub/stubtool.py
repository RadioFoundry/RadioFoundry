#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""stubtool: flash access for ODR-supported radios through a RAM monitor (the stub). Today: read the program flash and the data flash of an HR_C7000 radio (DM-32UV, DM-1801 Pro) through the chip's
serial boot ROM, every reply checked, both memories in one power-on. Protocol, history and the stub's source:
stub/USER-GUIDE.md in the ODR repository.

    stubtool.py PORT --progmem progmem.bin --q128 q128.bin     both memories, one boot (1 MB + 16 MB)
    stubtool.py PORT --q128 q.bin --q128-range 0x7C000:0x7C200   part of one (any byte range)
    stubtool.py PORT --verify-q128 q128.bin                      compare flash with a file by CRC, no data transferred
    stubtool.py PORT --odr-enter --progmem p.bin --q128 q.bin     the same from a radio running ODR (built with CONFIG_STUB_ENTRY=y)
    stubtool.py PORT --odr-enter --write-app odr_dm1801pro.bin   program an ODR build into the program flash (changed sectors only)
    stubtool.py PORT --attach --boot reset                      restart the radio into its firmware, no power cycle
    stubtool.py PORT --probe                                    chip IDs and a few registers, no files
    stubtool.py PORT --peek 0x14020000:16                       read radio memory (registers, SRAM, flash window)
    stubtool.py PORT out.bin --mem q128                         the old form: one memory into out.bin
    stubtool.py --self-test                                     against a simulated radio, no hardware

Radio OFF, cable in, run the script, then switch the radio ON within --wait seconds (or --odr-enter with the radio running ODR).
Reads never write. Only --write-app (the application area from 0xC000, never the bootloader) and --write-test (two reserved
test sectors per chip) write flash. The monitor the script loads into the radio's RAM is embedded below (source: the stub's own repository folder), so this file
needs only Python 3 and pyserial.
"""
import argparse
import base64
import hashlib
import sys
import time
import zlib

# --- STUB begin (stub/embed.py rewrites everything up to the end marker)
STUB_SHA256 = "053a6b645d122dc85443616ed622c3a91229d8f949fc5e07aec68d14a8e4cbe1"
STUB_B64 = (
    "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"
)
# --- STUB end
# --- STUB ODR begin (stub/embed.py odr rewrites everything up to the end marker)
STUB_ODR_SHA256 = "a014d3b1ad500448f3a26702ffc5025384339648feb61d768440e37aa3c2db7b"
STUB_ODR_B64 = (
    "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"
)
# --- STUB ODR end
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
STUB_UNIV_SHA256 = "254f2332b8e03bbe7b15980f70ec954012ae763d5e70b152fe1dd295672beff6"
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
TIMER2_VALUE = 0x14000018           # timer 2 current value (counts down; the stub runs it free)
LED_RED, LED_GREEN = 33, 32         # PTB1 = red, PTB0 = green on the DM-1801 Pro and the DM-32UV (both checked on the radio)
OK, BADSUM, BADOP, SPITIMEOUT, TOOLONG = 0x05, 0x0A, 0x0B, 0x0C, 0x0E
# memory: (SSI base, SER, GPIO chip-select pin or None, size, name). The data flash's chip select is the GPIO PTA18
# (hardware-dm32.md 10; dm1801pro-hardware.md 6.1); the SSI's own select is not wired to the flash on its own.
MEM = {"progmem": (SSI_PROG, True, None, 0x100000, "program flash"),
       "q128": (SSI_DATA, True, 18, 0x1000000, "data flash"),
       # the DB25-D's SPI NAND on the same bus and chip select (hardware-db25d.md 6.5): 2 KB pages without the spare
       # bytes, read by the universal stub's NAND path (flags bit 2), two pages per 4 KB sector; never written here
       "nand": (SSI_DATA, True, 18, 0x10000000, "NAND")}
NAND_FLAG = 4


# Writes are only allowed here (chosen 2026-10-15 from the dumps: blank in every dump, away from ODR)
WRITE_OK = {"progmem": [(0xF0000, 0xF4000)], "q128": [(0xC00000, 0xC04000)]}
PAGE = 256
# The application area of the program flash: an ODR image is linked for 0x0300C000, which is flash offset 0xC000. The
# bootloader below it (0..0xC000) is never written, so the radio can always be put in update mode by hand.
APP_START, APP_END = 0xC000, 0x100000


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


def stub_odr_image():
    """The stub linked for a running ODR on the DM-32UV / DM-1801 Pro (RAM at 0x17000, 84 MHz clock), for --odr-enter."""
    return _unpack(STUB_ODR_B64, STUB_ODR_SHA256, "ODR")


def odr_stubs():
    """The stubs embedded for --odr-enter, by the load address the running firmware reports in STUB INFO (the
    monitor is position dependent, so the image has to be linked for exactly the span ODR offers): {base: (name, image)}.
    0x17000 DM-32UV / DM-1801 Pro; 0x30000 DB25-D; 0x1D000 DB25-D built with CONFIG_DMR=y (boards/db25d/config.mk)."""
    out = {0x17000: ("DM-32UV / DM-1801 Pro", stub_odr_image())}
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
    """Refuse a stub that was not linked for the span ODR offers: jumping into one built for another base runs wild
    (the radio then needs a power cycle). The image's bss and stack have to lie inside [load, load + size)."""
    link = stub_link(image)
    if link is None:
        raise DumpError("this does not look like a stub image (no HR_C7000 monitor entry code at its start)")
    top, bss0, bss1 = link
    # the raw image is the stub's text and data and ends where its bss starts, which pins the base it was linked for
    if not (load <= bss0 <= bss1 <= top <= load + size and bss0 - load - len(image) in range(4)):
        raise DumpError("this stub image is linked for a different RAM span (its data and stack are at 0x%X-0x%X) than the "
                        "0x%X-0x%X ODR offers. Use the image built for that base (see stub/README.md)"
                        % (bss0, top, load, load + size))


SRAM = (0x10000, 0x58000)               # the HR_C7000's main SRAM, 288 KB (hardware-dm32.md 2; the same on the DB25-D)


def stub_univ_image():
    """The universal stub: one image for every HR_C7000 radio and every way in (boot ROM, the DB25-D's first stage, a
    running ODR, a stock DB25-D through its console). It copies itself to its run address and asks the host for the
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


# Board profiles for the universal stub (CONFIG, op 0x0D): board id echoed by HELLO, power latch / power key pins (255 =
# none), whether the data memory is the SPI NAND, the program flash size. VERSION's board= names them on the ODR route;
# --board at power-on. Unknown radio: GENERIC (no pins, nothing echoed but 0).
C7_BOARDS = {
    "dm32": {"id": 1, "name": "DM-32UV", "names": ("DM-32UV", "dm32", "dm32uv"), "latch": 255, "key": 255, "nand": False,
             "progmem": 0x100000},
    "dm1801pro": {"id": 2, "name": "DM-1801 Pro", "names": ("DM-1801 Pro", "dm1801pro"), "latch": 255, "key": 255,
                  "nand": False, "progmem": 0x100000},
    # PTB13 latch, PTB12 power key (hardware-db25d.md 3.3); the same pins under ODR, its first stage and the stock firmware
    "db25d": {"id": 3, "name": "DB25-D", "names": ("DB25-D", "db25d"), "latch": 45, "key": 44, "nand": True,
              "progmem": 0x200000},
}
GENERIC = {"id": 0, "name": "HR_C7000 radio", "names": (), "latch": 255, "key": 255, "nand": False, "progmem": 0x100000}
CFG_UART_THIS = 3                        # CONFIG uart field: answer only on the UART the CONFIG came in on


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


STUB_MAGIC = 0x42533743                 # 'C7SB': the guard word of ODR's STUB command (docs/spec/debuglink.md 3.18)
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


# played when a run finishes (--done-led): three green flashes, then the orange status blink resumes
DONE_LED = "0:255:150,0:0:150,0:255:150,0:0:150,0:255:150,0:0:150"


def sum8(b):
    return sum(b) & 0xFF


def rom_cmd(op, payload=b""):
    """02 op chk 03 payload; chk = sum of the payload. The ROM's own framing."""
    return bytes([0x02, op, sum8(payload), 0x03]) + payload


class Radio:
    """port: write(bytes), read(n) (what arrived within its timeout), reset_input_buffer(), a timeout attribute."""

    def __init__(self, port, log=print, retries=6):
        self.port = port
        self.log = log
        self.retries = retries
        self.retried = 0
        self.stub_head = b""
        self.stub_base = LOAD_ADDR
        self.v6 = None                  # the universal stub's HELLO fields (protocol 6), None for an older stub
        self.configured = None          # the board profile the stub confirmed after CONFIG
        self.profile = GENERIC

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
    def wait_for_radio(self, wait):
        want = b"\x02\x24\x00\x03"
        pos = 0
        end = time.monotonic() + wait
        self.set_timeout(0.1)
        while time.monotonic() < end:
            b = self.port.read(1)
            if not b:
                continue
            if b[0] == want[pos]:
                pos += 1
                if pos == len(want):
                    return
            else:
                pos = 1 if b[0] == want[0] else 0
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

    def start(self, wait=120, stub=None, load=LOAD_ADDR):
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
        self.wait_for_radio(wait)
        self.port.write(b"\x02\x14\x00\x03")
        time.sleep(0.05)
        blocks = len(stub) // BLOCK
        self.log("boot ROM answered; loading %d-byte stub (%d blocks)" % (len(stub), blocks))
        for i in range(blocks):
            payload = (load + i * BLOCK).to_bytes(4, "big") + b"\x00\x00\x00\x40" + stub[i * BLOCK:(i + 1) * BLOCK]
            for attempt in range(3):             # a block is a write to a fixed address: sending it again is safe
                self.port.reset_input_buffer()
                self.port.write(rom_cmd(0x18, payload))
                if self.read_exact(4, 1.0) == b"\x02\x05\x00\x03":
                    break
                self.log("stub block %d not acknowledged, sending it again" % i)
            else:
                raise DumpError("stub block %d was not acknowledged" % i)
        self.port.write(rom_cmd(0x1B, load.to_bytes(4, "big")))
        if self.read_exact(4, 1.0) != b"\x02\x05\x00\x03":
            raise DumpError("the start command was not acknowledged")
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
    def request(self, op, payload=b"", retries=None, timeout=1.0):
        retries = self.retries if retries is None else retries
        frame = bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, sum8(payload), 0x03]) + payload
        why = ""
        for attempt in range(retries + 1):
            self.port.reset_input_buffer()
            self.port.write(frame)
            n_expect = None
            hdr = self.read_exact(6, timeout)
            if len(hdr) != 6 or hdr[0] != 0x02 or hdr[5] != 0x03:
                why = "no or malformed reply header"
            else:
                n_expect = (hdr[2] << 8) | hdr[3]
                body = self.read_exact(n_expect, 0.5 + n_expect * 0.0002)
                if len(body) != n_expect:
                    why = "reply short (%d of %d bytes)" % (len(body), n_expect)
                elif sum8(body) != hdr[4]:
                    why = "reply checksum"
                elif hdr[1] == OK:
                    return body
                elif hdr[1] == BADSUM:
                    why = "the radio saw a bad request checksum"
                else:
                    raise DumpError("stub op 0x%02X refused: status 0x%02X" % (op, hdr[1]))
            self.retried += 1
            if attempt < retries:
                self.log("op 0x%02X: %s, again (%d of %d)" % (op, why, attempt + 1, retries))
                time.sleep(0.02)
        raise DumpError("op 0x%02X: no clean reply after %d tries (%s)" % (op, retries + 1, why))

    def hello(self, retries=0):
        r = self.request(0x01, retries=retries)
        if r[:6] != b"C7STUB":
            raise DumpError("unexpected hello %r" % r)
        self.info = int.from_bytes(r[13:17], "big") if len(r) >= 17 else 0      # the port's facts (HR_C7000: SSI bursts)
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
        boot ROM, 36 MHz under the DB25-D's loader, its stock firmware and ODR's hand-over there, 84 MHz under ODR on the
        DM-32UV / DM-1801 Pro). An older stub has no CONFIG: nothing is sent."""
        if self.v6 is None:
            self.log("stub protocol before 6: no board configuration")
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
        hz = int(round(hz / 1e5) * 1e5)
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
            raise DumpError("the NAND is read through the universal stub on a radio configured with one (--board db25d)")
        return (1 if ser else 0) | (2 if cs is not None else 0) | (NAND_FLAG if mem == "nand" else 0)

    # ---- entering the stub from a running ODR (debug link frames: A5 5A cmd seq len(2 LE) payload crc16(2 LE))
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
        raise DumpError("no answer from ODR's debug link to command 0x%02X (is ODR running with CONFIG_STUB_ENTRY=y and "
                        "the programming cable on UART0 at 115200?)" % cmd)

    def odr_enter(self, image=None, entry=None, board=None):
        """Load the stub into a running ODR's free RAM over its debug link and jump to it (radio ON, ODR running). Without
        `image` the embedded stub whose base is the span ODR reports in STUB INFO is used; a given image must be linked
        for that span (checked). Afterwards the stub answers on the same port. Recover the radio with a power cycle
        (or --boot reset)."""
        if board is None:                       # the profile the stub gets: from VERSION's board= (a library caller)
            name = version_board(self)
            board = c7_board(name) if name else None
        r = self.dl_request(DL_STUB, b"\x00")
        if r[0] != 0 or len(r) < 19 or int.from_bytes(r[1:5], "little") != STUB_MAGIC:
            raise DumpError("ODR's STUB info refused or malformed (%s): firmware built without CONFIG_STUB_ENTRY?" % r.hex())
        load, size = int.from_bytes(r[5:9], "little"), int.from_bytes(r[9:13], "little")
        chunk = min(int.from_bytes(r[13:15], "little"), 59)
        if image == "legacy":                   # the position-dependent stubs, one per span (the fallback)
            known = odr_stubs()
            if load not in known:
                raise DumpError("ODR offers RAM at 0x%X and no legacy stub is linked for that base (have %s): give the "
                                "matching image with --odr-stub" % (load, ", ".join("0x%X %s" % (k, v[0]) for k, v in sorted(known.items()))))
            self.log("the firmware's stub span 0x%X: using the legacy stub for %s" % (load, known[load][0]))
            image = known[load][1]
        elif image is None:
            image = stub_univ_image()
        if univ_layout(image):
            run, end = check_univ_load(image, load, (load, size))
            self.log("universal stub: loaded at 0x%X, runs at 0x%X-0x%X" % (load, run, end))
            self.stub_head = image[:0x80]
        else:
            check_stub_fits(image, load, size)
        if len(image) > size:
            raise DumpError("stub image (%d bytes) does not fit ODR's free RAM (%d bytes)" % (len(image), size))
        entry = load if entry is None else entry
        self.log("ODR offers %d bytes of RAM at 0x%X; sending the %d-byte stub in %d-byte chunks" % (size, load, len(image), chunk))
        r = self.dl_request(DL_STUB, b"\x01" + STUB_MAGIC.to_bytes(4, "little") + len(image).to_bytes(4, "little")
                            + (zlib.crc32(image) & 0xFFFFFFFF).to_bytes(4, "little") + entry.to_bytes(4, "little"))
        if r[0] != 0:
            raise DumpError("ODR refused the stub transfer (status %d)" % r[0])
        for off in range(0, len(image), chunk):
            r = self.dl_request(DL_STUB, b"\x02" + off.to_bytes(4, "little") + image[off:off + chunk])
            if r[0] != 0:
                raise DumpError("ODR refused the chunk at offset %d (status %d)" % (off, r[0]))
        r = self.dl_request(DL_STUB, b"\x03" + STUB_MAGIC.to_bytes(4, "little"))
        if r[0] == 3:
            raise DumpError("ODR refused to enter the stub: the radio is transmitting. Release PTT and run again")
        if r[0] != 0:
            raise DumpError("ODR refused to enter the stub (status %d): image incomplete or CRC mismatch" % r[0])
        self.log("ODR is jumping to the stub")
        time.sleep(0.3)
        self.attach()
        self.configure(board)

    def send_noreply(self, op, payload=b""):
        """A request the radio will not answer because it is about to reset or jump away."""
        self.port.reset_input_buffer()
        self.port.write(bytes([0x02, op, len(payload) >> 8, len(payload) & 0xFF, sum8(payload), 0x03]) + payload)

    def reset_chip(self):
        """Soft-reset the SoC (hardware-dm32.md 1.1: XIP quad off and the reset divider, then SYS_SOFT_RSTN = 0). The boot
        ROM runs again, offers its serial window briefly, then starts the firmware from flash: the same as a power cycle
        without the cycle. The soft reset keeps the divider, and the ROM can't read flash at ODR's divider 2."""
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
        base, ser, cs, _, _ = MEM[mem]
        self.spi(base, b"\x06", 0, ser, cs)                     # write enable
        if not self._status(mem) & 2:
            raise DumpError("%s: write enable did not stick (status 0x%02X): chip write-protected?" % (what, self._status(mem)))
        self.spi(base, tx, 0, ser, cs)
        self._wait_ready(mem, what, timeout)

    def check_write_range(self, mem, start, end, app=False):
        if start % SECTOR or end % SECTOR or start >= end:
            raise DumpError("writes are whole 4 KB sectors")
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
            self._write_cmd(mem, bytes([0x20, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF]), "erase 0x%X" % a, 3.0)

    def write_image(self, image, progress=None):
        """Program an ODR application image into the program flash at 0xC000 (where it is linked to run): only the 4 KB
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
        todo = self.verify(mem, padded, APP_START)
        for n, a in enumerate(todo):
            if progress:
                progress(n, len(todo))
            off = a - APP_START
            self._write_cmd(mem, bytes([0x20, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF]), "erase 0x%X" % a, 3.0)
            for p in range(a, a + SECTOR, PAGE):
                page = padded[p - APP_START:p - APP_START + PAGE]
                if page != b"\xff" * PAGE:
                    self._write_cmd(mem, bytes([0x02, p >> 16 & 0xFF, p >> 8 & 0xFF, p & 0xFF]) + page, "program 0x%X" % p, 1.0)
        bad = self.verify(mem, padded, APP_START) if todo else []
        if bad:
            raise DumpError("verify after write: sectors differ at %s" % ", ".join("0x%X" % x for x in bad))
        return len(todo), (end - APP_START) // SECTOR - len(todo)

    def write_flash(self, mem, start, data):
        """Erase and program whole sectors at `start`, then check by CRC against the radio's own read of them."""
        end = start + len(data)
        self.check_writable()
        self.check_write_range(mem, start, end)
        self.erase_sectors(mem, start, end)
        for a in range(start, end, PAGE):
            page = data[a - start:a - start + PAGE]
            if page != b"\xff" * PAGE:                           # erased already
                self._write_cmd(mem, bytes([0x02, a >> 16 & 0xFF, a >> 8 & 0xFF, a & 0xFF]) + page, "program 0x%X" % a, 1.0)
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

    def set_baud(self, baud, window=4.0, tries=4):
        """Ask the stub to switch its UART to `baud`, prove the new rate with full-size reads, then confirm. Provisional on
        the radio's side: unless the confirm arrives inside `window` seconds it goes back to the old rate by itself, and
        this side goes back too. Returns True if the new rate is now in force, False (still at the old rate) if not."""
        old = getattr(self.port, "baudrate", 115200)
        hz = self.timer_hz(0.2)
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
            for _ in range(tries):
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
        except (DumpError, ValueError, OSError) as e:
            self.log("%d baud failed (%s); going back to %d" % (actual, e, old))
        if not ok:
            try:
                self.port.baudrate = old
            except (ValueError, OSError):
                pass
            time.sleep(max(0.0, window + 0.3 - (time.monotonic() - t0)))      # the stub reverts when its window ends
            self.port.reset_input_buffer()
            self.hello()
            return False
        self.log("serial now %d baud" % actual)
        return True

    def timer_hz(self, span=0.5):
        """Tick rate of timer 2 (it counts down), measured against this computer's clock."""
        t0 = time.monotonic()
        v0 = int.from_bytes(self.peek(TIMER2_VALUE, 4), "little")
        time.sleep(span)
        v1 = int.from_bytes(self.peek(TIMER2_VALUE, 4), "little")
        t1 = time.monotonic()
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
                    want = compress
                    if progress and total % 64 == 0:
                        progress(sa, end)
            except FrameError as e:
                fails += 1
                self.retried += 1
                if fails > self.retries:
                    raise DumpError("0x%X: no clean read after %d tries (%s)" % (sa, fails, e))
                self.log("0x%X: %s, resuming here (%d of %d)" % (sa, e, fails, self.retries))
                self.drain()
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
    def __init__(self, images, drop=None, flip=(), silent=(), swap=(), misplace=(), stub_len=1536, load=LOAD_ADDR):
        self.images = images                # {"progmem": bytes, "q128": bytes}
        self.drop, self.flip, self.silent, self.swap = drop or {}, set(flip), set(silent), set(swap)
        self.misplace = set(misplace)
        self.data_bytes = 0                 # flash bytes sent over the cable
        self.sectors_sent = 0
        self.pokes = []
        self.led = self.led_seq = None
        self.out = bytearray(b"\x02\x24\x00\x03")
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
        self.pending = None                       # (old rate, deadline) of a provisional baud change
        self.baud_log = []
        self.switch_to = None
        self.wel = False
        self.writes = []
        self.univ = None                    # a universal stub: {"run", "end", "board", "flags", "honor"} (protocol 6)
        self.configs = []

    def revert_check(self):
        if self.pending and time.monotonic() >= self.pending[1]:
            self.stub_baud = self.pending[0]
            self.pending = None
            self.baud_log.append("reverted")

    def linked(self):
        self.revert_check()
        return self.baudrate == self.stub_baud and (self.max_baud is None or self.baudrate <= self.max_baud)

    def flush(self):
        pass

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
            if bytes(ib[:4]) == b"\x02\x14\x00\x03":
                del ib[:4]
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
            self.out += b"\x02\x05\x00\x03"
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
        n = self.requests
        if n in self.silent:
            return True
        status, body = OK, b""
        if op == 0x01:
            body = b"C7STUB" + bytes([1, 1032 >> 8, 1032 & 255]) + LOAD_ADDR.to_bytes(4, "big")
            u = self.univ
            if u:
                body = (b"C7STUB" + bytes([6, 1032 >> 8, 1032 & 255]) + self.load.to_bytes(4, "big") + (0x0808).to_bytes(4, "big")
                        + bytes([u["board"], u["flags"]]) + u["run"].to_bytes(4, "big") + len(self.loaded).to_bytes(4, "big")
                        + (u["end"] - 0x1004).to_bytes(4, "big") + (u["end"] - 0x1000).to_bytes(4, "big")
                        + u["end"].to_bytes(4, "big") + (600).to_bytes(4, "big"))
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


def rxn_big(payload):
    return int.from_bytes(payload[8:10], "big") >= 256


def self_test():
    import io
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
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
    r.setup_led(32, 33, "red")
    r.reset_chip()
    check(radio.pokes[-3:] == [(QUAD_ENABLE, 0), (LCSFC_BAUDR, 4), (SYS_SOFT_RSTN, 0)],
          "reset_chip pokes XIP quad off and the reset divider, then the soft reset (%s)" % (radio.pokes[-3:],))
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
    # entering the stub from ODR: a small debug-link responder following docs/spec/debuglink.md 3.18
    def fake_stub(base, length):                       # an image with the entry code's literal pool, linked at `base`
        return (b"\x80\xc0\x20\x70" + bytes(0x24) + (base + length).to_bytes(4, "little") + (base + 0x34 + 1024).to_bytes(4, "little")
                + (base + 0x632C).to_bytes(4, "little") + bytes(range(256)) * 4)

    class FakeODR:
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
            if cmd == 0x02:                             # VERSION: the board odr_enter picks the stub profile by
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
    spans = {0x17000: (0x18000, "DM-32UV"), 0x30000: (0x20000, "DB25-D"), 0x1D000: (0x12000, "DB25-D with DMR")}
    known = odr_stubs()
    check(set(known) == set(spans), "embedded ODR stubs cover %s" % sorted(hex(k) for k in spans))
    for base, (size, name) in spans.items():
        link = stub_link(known[base][1])
        check(link is not None and base <= link[1] <= link[2] <= link[0] <= base + size,
              "embedded %s stub is linked inside its span (%s)" % (name, link and [hex(x) for x in link]))
        odr = FakeODR((base, size))                    # no image given: chosen by the span the firmware reports
        ro = Radio(odr, log=quiet)
        ro.attach = lambda: None
        ro.odr_enter("legacy")
        check(odr.entered and bytes(odr.mem[:len(known[base][1])]) == known[base][1],
              "odr_enter picks the legacy stub for the %s span 0x%X" % (name, base))
        odr = FakeODR((base, size))                    # default: the universal stub, at the span's base
        ro = Radio(odr, log=quiet)
        ro.attach = lambda: None
        ro.odr_enter()
        univ = stub_univ_image()
        check(odr.entered and bytes(odr.mem[:len(univ)]) == univ, "odr_enter loads the universal stub into the %s span 0x%X" % (name, base))
        for other, (_, img) in known.items():           # every stub linked for another base is refused, nothing is sent
            if other == base:
                continue
            odr = FakeODR((base, size))
            ro = Radio(odr, log=quiet)
            ro.attach = lambda: None
            try:
                ro.odr_enter(img)
                check(False, "a stub linked for 0x%X is refused by the 0x%X span" % (other, base))
            except DumpError:
                check(not odr.entered and odr.total is None, "stub for 0x%X refused before BEGIN on the 0x%X span" % (other, base))
    odr = FakeODR((0x40000, 0x10000))
    try:
        Radio(odr, log=quiet).odr_enter("legacy")
        check(False, "an unknown span with no legacy image is refused")
    except DumpError as e:
        check("--odr-stub" in str(e), "unknown span message names --odr-stub")
    odr = FakeODR((0x17000, 0x18000))
    try:
        Radio(odr, log=quiet).odr_enter(stub)           # not a stub at all (no entry code)
        check(False, "an image without the monitor's entry code is refused")
    except DumpError:
        pass
    ro = Radio(odr, log=quiet)
    ro.attach = lambda: None
    ro.odr_enter(fake_stub(0x17000, 0x18000))
    check(odr.entered, "odr_enter loads a given image that fits the span")
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
    check(univ_layout(stub_image()) is None and all(univ_layout(v[1]) is None for v in odr_stubs().values()),
          "legacy stubs are told apart from the universal one")
    # every way in: ROM / first stage at 0x10000, ODR's spans, run address itself, a stock DB25-D's console window
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
    odr = FakeODR((0x2F000, 0x4000))                    # a span that would overlap the run copy: refused before BEGIN
    try:
        Radio(odr, log=quiet).odr_enter()
        check(False, "odr_enter refuses a span overlapping the universal stub's run address")
    except DumpError:
        check(odr.total is None, "overlapping span refused before BEGIN")
    # power-on with the universal stub, CONFIG, the HELLO echo, writes gated on it, the NAND path
    nand = bytearray(images["q128"][:16 * SECTOR])
    nand[SECTOR:2 * SECTOR] = b"\xff" * SECTOR
    for board in ("db25d", "dm1801pro", None):
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
        if board == "db25d":
            out = io.BytesIO()
            r.dump(out, "nand", 0, 16 * SECTOR)
            check(out.getvalue() == bytes(nand), "NAND read through the stub's NAND path")
            check(r.mem_size("progmem") == 0x200000, "the DB25-D's program flash is 2 MB")
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
        r.configure("db25d")
        check(False, "a stub that ignores CONFIG is caught by the HELLO echo")
    except DumpError:
        try:
            r.write_image(images["progmem"][APP_START:APP_START + SECTOR])
            check(False, "no write after a failed CONFIG")
        except DumpError:
            pass
    # an old stub (HELLO without the v6 fields): no CONFIG, writes allowed as before
    radio = FakeRadio(images, stub_len=len(stub))
    r = Radio(radio, log=quiet)
    r.start(wait=2, stub=stub)
    r.configure("db25d")
    check(r.v6 is None and not radio.configs, "an old stub gets no CONFIG")
    r.write_image(images["progmem"][APP_START:APP_START + SECTOR])
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
    """The board= name from a running ODR's debug link VERSION reply, or None."""
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


def find_odr_port():
    """The one serial port with ODR's USB identity (1FC9:0094), which the STM32 stub also uses"""
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x1FC9, 0x0094)]
    if len(ports) != 1:
        raise OSError("%d serial ports with USB ID 1FC9:0094: give --stub-port" % len(ports))
    return ports[0]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("port", nargs="?", help="serial port, or udp:ADDR[:PORT] for an STM32 radio in USB network mode")
    ap.add_argument("--stub-port", metavar="PORT", help="with a udp: port: the serial port the stub comes up on "
                    "(default: the one USB device 1FC9:0094)")
    ap.add_argument("out", nargs="?", help="old form: output file for the memory chosen with --mem")
    ap.add_argument("--mem", choices=sorted(MEM), default="progmem", help="memory for the old form")
    ap.add_argument("--progmem", metavar="FILE", help="read the 1 MB program flash into FILE")
    ap.add_argument("--q128", metavar="FILE", help="read the 16 MB data flash into FILE")
    ap.add_argument("--progmem-range", metavar="START:END", help="byte range, any alignment (default all)")
    ap.add_argument("--q128-range", metavar="START:END")
    ap.add_argument("--nand", metavar="FILE", help="DB25-D (--board db25d or found from ODR): read the SPI NAND into FILE "
                    "(2 KB pages without the spare bytes; give --nand-range, the chip is 256 MB)")
    ap.add_argument("--nand-range", metavar="START:END")
    ap.add_argument("--verify-nand", metavar="FILE", help="compare the NAND from --nand-range's start (default 0) with FILE by CRC")
    ap.add_argument("--start", type=lambda s: int(s, 0), help="old form: first byte")
    ap.add_argument("--end", type=lambda s: int(s, 0), help="old form: end (exclusive)")
    ap.add_argument("--verify-progmem", metavar="FILE", help="compare the program flash with FILE by CRC (no data transfer)")
    ap.add_argument("--verify-q128", metavar="FILE", help="compare the data flash with FILE by CRC")
    ap.add_argument("--write-q128", metavar="FILE", help="STM32 radios: write FILE into the SPI flash from --q128-range's "
                    "start (default 0), changed 4 KB sectors only, each verified by CRC")
    ap.add_argument("--probe", action="store_true", help="chip IDs, a first block of each flash and some registers")
    ap.add_argument("--peek", metavar="ADDR:LEN", help="print LEN bytes of radio memory (hex dump)")
    ap.add_argument("--retries", type=int, default=6)
    ap.add_argument("--wait", type=int, default=120, help="seconds to wait for the radio to be switched on")
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
    ap.add_argument("--baud", metavar="N|auto", help="after the boot, switch the serial link to N baud (this UART has no fractional divider, so only 750000, 500000, 375000, 250000 are offered; 921600/460800/230400 are refused); auto tries them from fast to slow. Tested before it is kept; falls back to 115200 if the cable cannot do it")
    ap.add_argument("--write-test", choices=("progmem", "q128", "both"), help="erase, program a test pattern into, read back and erase "
                    "again the sectors reserved for write tests (see WRITE_OK); nothing else is touched")
    ap.add_argument("--odr-enter", action="store_true", help="the radio is ON and runs ODR (built with CONFIG_STUB_ENTRY=y): load "
                    "the stub into its RAM over the debug link and jump to it, instead of the boot-ROM load")
    ap.add_argument("--odr-stub", metavar="FILE", help="with --odr-enter: this stub image instead of the embedded one (which is chosen by the RAM span the "
                    "firmware reports); refused unless it is linked for that span")
    ap.add_argument("--write-app", metavar="IMAGE", help="program an ODR application image (raw .bin linked for 0x0300C000) into the "
                    "program flash at 0xC000: only changed 4 KB sectors are written, then verified; the bootloader is never touched. "
                    "Use with --odr-enter (radio running ODR) or the boot-ROM load; finish with --boot reset")
    ap.add_argument("--attach", action="store_true", help="the stub from an earlier run is still up: skip the boot")
    ap.add_argument("--stub", metavar="FILE", help="load this stub image instead of the embedded universal one (a universal "
                    "image, or a position-dependent one linked for the load address)")
    ap.add_argument("--legacy-stub", action="store_true", help="use the position-dependent stubs embedded before the universal "
                    "one (one per RAM span: the boot ROM's 0x10000, ODR's 0x17000, the DB25-D's 0x30000 / 0x1D000). Fallback only")
    ap.add_argument("--load-base", metavar="ADDR", type=lambda s: int(s, 0),
                    help="power-on load: write and start the stub at ADDR (default 0x%X). The universal stub runs from any "
                         "base that doesn't overlap its run area; a legacy stub must be linked for it (the DB25-D's first stage "
                         "took the legacy 0x30000 stub at 0x30000)" % LOAD_ADDR)
    ap.add_argument("--board", choices=("dm1701", "md380") + tuple(C7_BOARDS),
                    help="the radio: an STM32 one (stub/f4stub.py) or an HR_C7000 one (dm32, dm1801pro, db25d: the board "
                         "profile the universal stub gets). Found from the radio's VERSION reply with --odr-enter; give it at "
                         "power-on and with --attach (without it the stub gets a profile that drives no board pins)")
    ap.add_argument("--no-rodata2", action="store_true", help="STM32 --write-app: the DM-1701 application without its "
                    "sector 9 part, on purpose")
    ap.add_argument("--backup-dir", metavar="DIR", help="STM32 --write-app: where a sector that also holds codec bytes "
                    "is saved before it is erased (the MD-380's sector 6; default the home directory, never a git tree)")
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
    if not (jobs or a.probe or a.peek or a.verify_progmem or a.verify_q128 or a.verify_nand or a.led_test or a.led_seq or a.boot or a.write_test or a.write_app or a.write_q128):
        ap.error("nothing to do: give --progmem, --q128, --verify-*, --probe, --peek or OUT")
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
            reopen = lambda: serial.Serial(a.stub_port or find_odr_port(), 115200, timeout=0.1)
        else:
            port = serial.Serial(a.port, 115200, timeout=0.1)
            import f4stub
            reopen = f4stub.PortFollower(serial, a.port)
        radio = Radio(port, retries=a.retries)
        # The STM32 radios (DM-1701, MD-380) run their own stub over USB: stub/f4stub.py
        import f4stub
        if a.board is None and a.odr_enter:
            name = version_board(radio)
            a.board = f4stub.board_for(name) if name else None
            if a.board is None and name:
                a.board = c7_board(name)
                if a.board is None:
                    print("ODR reports board %r: not one with a stub profile, using the generic one" % name)
        if a.board in f4stub.BOARDS:
            return f4stub.run(a, port, reopen)
        if a.write_q128:
            raise DumpError("--write-q128 is for the STM32 radios (DM-1701, MD-380)")
        if a.odr_enter:
            radio.odr_enter(open(a.odr_stub, "rb").read() if a.odr_stub else ("legacy" if a.legacy_stub else None), board=a.board)
        elif a.attach:
            radio.attach()
            radio.configure(a.board)
        else:
            load = LOAD_ADDR if a.load_base is None else a.load_base
            if stub is None and a.legacy_stub:
                if load == LOAD_ADDR:
                    stub = stub_image()
                elif load in odr_stubs():
                    stub = odr_stubs()[load][1]
                else:
                    raise DumpError("no legacy stub is linked for 0x%X: give --stub" % load)
            print("radio OFF? switch it on now (waiting up to %d s)" % a.wait)
            radio.start(a.wait, stub, load)
            radio.configure(a.board)
        jobs = [(mem, f) + parse_range(rng, radio.mem_size(mem)) for mem, f, rng in jobs]
        if a.baud:
            for b in ([921600, 460800, 230400, 750000, 500000, 375000, 250000] if a.baud == "auto" else [int(a.baud)]):
                if radio.set_baud(b):
                    break
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
            t0 = time.monotonic()
            nw, nk = radio.write_image(image, progress=lambda i, n: print("\rwriting sector %d of %d changed" % (i + 1, n), end="", flush=True))
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
    sys.exit(main())
