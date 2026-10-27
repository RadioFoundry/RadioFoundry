#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""f4stub: the host side of the STM32F4 flash stub (every STM32F4 board). docs/spec/stub-stm32f4.md.

The stub (stub/stm32f4, built with `make -C stub/stm32f4`) is loaded by a running RadioFoundry built with CONFIG_STUB_ENTRY=y
through the debug link's STUB command, over the radio's USB serial port. It then answers on the same port (or, if it
had to enumerate again, on a port with the same serial number). stubtool.py hands STM32 radios to this module:

    stubtool.py PORT --rfoundry-enter --write-app build/dm1701/rfoundry_dm1701.bin   install a build
    stubtool.py PORT --rfoundry-enter --write-app build/dm1701/rfoundry_dm1701_codec.bin   a build merged with the codec
    stubtool.py PORT --rfoundry-enter --progmem flash.bin                      back up the whole internal flash (1 MB)
    stubtool.py PORT --rfoundry-enter --verify-progmem flash.bin               compare by CRC
    stubtool.py PORT --rfoundry-enter --q128 spi.bin                           back up the external SPI flash (16 MB)
    stubtool.py PORT --rfoundry-enter --write-q128 spi.bin                     put it back (changed 4 KB sectors only)
    stubtool.py PORT --rfoundry-enter --secreg secreg.bin                      back up the SPI flash's security registers
    stubtool.py PORT --attach --write-secreg secreg.bin --confirm-secreg       put them back (changed registers only)
    stubtool.py PORT --attach --board md380 --boot reset                  restart into the firmware
    f4stub.py --self-test                                                 against a simulated radio, no hardware
    f4stub.py --embed build/stub-stm32f4/f4stub.bin                       put a new stub build into this file
    f4stub.py --check-embedded build/stub-stm32f4/f4stub.bin              exit 1 when the build differs from it

The tool sends the stub embedded below (checked by its SHA-256), so the host tools and the web flasher send the same
bytes; `--rfoundry-stub FILE` sends another build instead. After a change to stub/stm32f4, rebuild and run --embed; CI
fails while the embedded copy and a fresh build differ.

Framing and the debug link loader are stubtool.Radio's; what differs is here: the ops (ERASE / PROGRAM / RESET are
new, SUMS and READ take a flash address), 4 KB blocks inside coarse erase sectors (16 to 128 KB), the sector 3
interlock and the update order that goes with it, and the bytes a sector shares with other data, which are read back
and kept (no radio has such a sector since the codec moved).

One stub image serves every radio. The board facts (SPI flash wiring, crystal, bootloader end) come from the board
registry (board.toml [flash.stub], pins.h, board.mk; tools/boards.py) into BOARDS below and go to the stub as a CONFIG
record (config_record) right after HELLO; the stub writes nothing until it has one, and the
tool checks its echo in the next HELLO. The board comes from the running RadioFoundry's VERSION reply, or from --board.
"""
import base64
import hashlib
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stubtool  # noqa: E402  (the shared framing, LZ4 decoder and debug link loader)
from stubtool import rfoundryboards  # noqa: E402  (the board registry, tools/rfoundry/rfoundry/registry.json)
from stubtool import protect  # noqa: E402  (protected flash regions, board.toml [[flash.protect]])

# --- F4STUB begin (f4stub.py --embed rewrites everything up to the end marker)
F4STUB_SHA256 = "8141381cf472d079dd0b4bea6d187b1351c8a71535956fc3a7561c2dcfada083"
F4STUB_B64 = (
    "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"
)
# --- F4STUB end

MAGIC = 0x46345342                     # 'F4SB': ERASE and RESET (stub/stm32f4/f4stub.h)
OP_HELLO, OP_PEEK, OP_SUMS, OP_READ, OP_RESET, OP_ERASE, OP_PROGRAM = 0x01, 0x02, 0x07, 0x09, 0x0D, 0x10, 0x11
OP_XERASE, OP_XPROGRAM, OP_CONFIG = 0x12, 0x13, 0x14
OP_SREAD, OP_SWRITE = 0x15, 0x16       # the SPI flash's security registers (stub version 5)
MEM_INTERNAL, MEM_SPI = 0, 1           # the memory byte SUMS and READ end with (stub version 2)
OK, BADSUM, BADOP, BADARG, TOOLONG, REFUSED, FLASHERR, NOCONFIG = 0x05, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11
VERSION = 5                            # the embedded stub's protocol: 3 added the image header and the IWDG feed,
                                       # 4 CONFIG and HELLO's record echo, 5 SREAD and SWRITE
CONFIG_VERSION = 4                     # the first protocol with CONFIG
SREG_VERSION = 5                       # the first protocol with the security registers
SREG_LEN, SREG_COUNT = 256, 3          # registers 1-3 at 0x001000, 0x002000, 0x003000 (stub/common/stubeng.h)
SREG_MFR = 0xEF                        # the JEDEC manufacturer the stub sends their commands to (Winbond)
SR2_LB1 = 0x08                         # status register 2: LB1; LB2 and LB3 are the next bits up (one-time locks)
SREG_TIMEOUT = 3.0                     # an erase (400 ms worst case) and a page program
HELLO_CONFIGURED = 0x80                # HELLO flags: a board record is in force
HELLO_LEN = 26                         # HELLO v4 without the record echo
REC_LEN, REC_LAYOUT = 24, 1            # the board record (stub/stm32f4/f4stub.h)
BLOCK = 4096
FLASH_BASE = 0x08000000
APP_BASE = 0x0800C000                  # sector 3: the bootloader starts the application here; 0-2 are never written
SUM_MAX = 800                          # SUMS records per request (the stub allows 820)
READ_PACK = 16                         # blocks per READ request
ERASE_TIMEOUT = 6.0                    # 128 KB: 2 s worst case in the data sheet, the stub gives up at 4 s
RUN_ADDR = 0x20000000                  # the stub's link address, which RadioFoundry reports as STUB INFO's load_addr
SRAM_SIZE = 128 * 1024                 # stub/stm32f4/f4stub.ld: .bss and the stack use all of it after the jump

# Flash the firmware owns on each radio: the application region of its linker script (the board registry,
# tools/boards.py). Everything else in a sector that is rewritten is read back from the radio and written again
# unchanged (no radio shares a sector with the codec any more, since it moved; the rule stays for one that does).
# Sectors with no owned byte are never erased, except "codec": the flash from the application's end to the end of
# flash, the codec's own sectors, written only from an image merged with the codec (docs/spec/codec-placement.md),
# which carries all of them. The radios: every board whose platform builds this stub.
#
# The rest of a profile is the stub's board record (record()), from the registry's [flash.stub] (tools/boards.py
# Board.stub): "id" (echoed by HELLO), the crystal ("hse"), the bootloader's end ("boot_end"), and the SPI flash ("spi"):
# SPI block, mode, the fastest clock, SCK / MISO / MOSI and their alternate function, the chip select, and other chip
# selects on that bus to hold high ("idle").
BOARDS = {b.id: {"name": b.name, "names": tuple(b.debuglink_names), "owned": [b.app_region],
                 "codec": (b.app_region[1], b.flash_end), "id": b.stub["id"], "hse": b.stub["hse"],
                 "boot_end": b.stub["boot_end"], "spi": b.stub["spi"], "protect": b.protect}
          for b in rfoundryboards.BOARDS.values() if b.stub and b.stub["tool"] == "f4stub"}
# The tool installs at one application base, which every profile's record must give
for _bid, _b in BOARDS.items():
    if _b["boot_end"] != APP_BASE:
        raise ImportError("f4stub: the %s's image starts at 0x%08X, the stub's tools know 0x%08X only"
                          % (_bid, _b["boot_end"], APP_BASE))
# The wiring a stub before protocol 4 has built in, as the record after its board id byte: SPI1 mode 0 at 4.5 MHz,
# PB3-PB5 (AF5), chip select PD7, nothing held high, an 8 MHz crystal. Such a stub serves only a board whose record
# matches it
V2_WIRING = (bytes([REC_LAYOUT, 1, 0]) + (8000000).to_bytes(4, "big") + APP_BASE.to_bytes(4, "big")
             + (4500000).to_bytes(4, "big") + bytes([0x13, 0x14, 0x15, 5, 0x37, 0xFF, 0xFF, 0]))


def pin_byte(name):
    """'PB12' -> 0x1C (port << 4 | pin), None -> 0xFF"""
    if name is None:
        return 0xFF
    if len(name) < 3 or name[0] != "P" or not "A" <= name[1] <= "I" or not 0 <= int(name[2:]) <= 15:
        raise ValueError("pin %r" % name)
    return (ord(name[1]) - ord("A")) << 4 | int(name[2:])


def record(b):
    """The 24-byte board record the stub's CONFIG takes (stub/stm32f4/f4stub.h), from a profile's fields (BOARDS)"""
    spi = b["spi"]
    if spi and len(spi["idle"]) > 2:
        raise ValueError("the board record holds at most two chip selects to keep high")
    idle = list(spi["idle"]) + [None] * (2 - len(spi["idle"])) if spi else [None, None]
    rec = bytes([b["id"], REC_LAYOUT, spi["bus"] if spi else 0, spi["mode"] if spi else 0])
    rec += b["hse"].to_bytes(4, "big") + b["boot_end"].to_bytes(4, "big") + (spi["hz"] if spi else 0).to_bytes(4, "big")
    rec += bytes([pin_byte(spi and spi["sck"]), pin_byte(spi and spi["miso"]), pin_byte(spi and spi["mosi"]),
                  spi["af"] if spi else 0, pin_byte(spi and spi["cs"]), pin_byte(idle[0]), pin_byte(idle[1]), 0])
    assert len(rec) == REC_LEN
    return rec


# A profile with the stub id the registry keeps for tests (0xFE, tools/boards.py STUB_TEST_ID), so HELLO never names a
# real board for it, for the tests here, in tests/host/f4stub_wire_check.py and in the web flasher's
# (site/tests/fixtures.py): the SPI flash on SPI2 in mode 3 with chip select PB12, a second chip select on that bus
# (PD14) held high, and the MD-380's application sectors 3-7 with the codec in 8-11
TEST_BOARD = "spi2test"


def test_board():
    return {"name": "SPI2 test board", "names": (), "owned": [(APP_BASE, 0x08080000)], "codec": (0x08080000, 0x08100000),
            "id": 0xFE, "hse": 8000000, "boot_end": APP_BASE,
            "spi": {"bus": 2, "mode": 3, "hz": 4500000, "sck": "PB13", "miso": "PB14", "mosi": "PB15", "af": 5,
                    "cs": "PB12", "idle": ["PD14"]}}


def config_record(board_id):
    """The board record of one of BOARDS"""
    return record(BOARDS[board_id])


def board_by_id(num):
    return next((bid for bid, b in BOARDS.items() if b["id"] == num), None)


class F4Error(stubtool.DumpError):
    pass


STUB_USB_ID = (0x1FC9, 0x0094)	# RadioFoundry's and the stub's USB identity (same serial number too)
PORT_FALLBACK_S = 10.0		# how long the original port name gets before a look-alike is tried
PORT_WAIT_S = 20.0		# how long attach() waits in all for the stub's port after a fresh enumeration


class PortInfo:
    def __init__(self, device, vid=None, pid=None, serial=None, location=None):
        self.device, self.vid, self.pid, self.serial, self.location = device, vid, pid, serial, location


def usb_ports():
    """The serial ports pyserial can see, as PortInfo."""
    from serial.tools import list_ports
    return [PortInfo(p.device, p.vid, p.pid, p.serial_number, p.location) for p in list_ports.comports()]


def pick_port(ports, orig):
    """The port the stub came back on, when the one it left is gone or stale. ports: PortInfo list; orig: the PortInfo
    of the port RadioFoundry was on (None if it's unknown). The radio keeps its USB serial number, but a re-enumeration can give
    it the next ttyACM number while Linux still holds the old one, so the name is no proof. When the original's USB
    location is known (the physical port, what /dev/serial/by-path encodes) only a port there counts. Otherwise the
    same serial number if only one port has it, else the only port with the stub's USB ID. None when that is not
    unique: a second radio must not be picked up."""
    cands = [p for p in ports if (p.vid, p.pid) == STUB_USB_ID
             and not (orig and os.path.realpath(p.device) == os.path.realpath(orig.device))]
    if orig is not None and orig.location:
        # a known physical port is decisive: the same radio re-enumerates on the same one
        same = [p for p in cands if p.location == orig.location]
        return same[0] if len(same) == 1 else None
    if orig is not None and orig.serial:
        same = [p for p in cands if p.serial == orig.serial]
        if len(same) == 1:
            return same[0]
    return cands[0] if len(cands) == 1 else None


class PortFollower:
    """F4Radio's `reopen`: opens the stub's port again after a fresh enumeration. The original name first; once it
    has had PORT_FALLBACK_S, every other try goes to a look-alike (pick_port), so a radio that came back as ttyACM1 is
    found while a slow ttyACM0 still works."""

    def __init__(self, serial_mod, path, baud=115200, ports=usb_ports, clock=time.monotonic, log=print):
        self.serial, self.path, self.baud, self.ports, self.clock, self.log = serial_mod, path, baud, ports, clock, log
        self.tries = 0
        self.start = None
        self.said = None
        self.orig = None
        try:
            real = os.path.realpath(path)
            self.orig = next((p for p in ports() if os.path.realpath(p.device) == real), None)
        except (ImportError, OSError):
            pass

    def __call__(self):
        now = self.clock()
        if self.start is None:
            self.start = now
        self.tries += 1
        path = self.path
        if now - self.start >= PORT_FALLBACK_S and self.tries % 2 == 0:
            try:
                alt = pick_port(self.ports(), self.orig)
            except (ImportError, OSError):
                alt = None
            if alt is not None:
                if alt.device != self.path and self.said != alt.device:
                    self.said = alt.device
                    self.log("%s did not answer; trying %s (same USB device)" % (self.path, alt.device))
                path = alt.device
        return self.serial.serial_for_url(path, self.baud, timeout=0.1)


def f405_sectors(flash_kb=1024):
    """[(index, addr, size)] of a single-bank STM32F4: 4 x 16 KB, 64 KB, then 128 KB each."""
    out = []
    addr = FLASH_BASE
    i = 0
    while addr < FLASH_BASE + flash_kb * 1024 and i < 12:
        size = 0x4000 if i < 4 else (0x10000 if i == 4 else 0x20000)
        out.append((i, addr, size))
        addr += size
        i += 1
    return out


def board_for(name):
    for bid, b in BOARDS.items():
        if name == bid or name in b["names"]:
            return bid
    return None


def inside_git_tree(path):
    """True when path's directory is inside a git work tree: flash dumps hold the proprietary bootloader and the codec,
    and are never written there (docs/spec/debuglink.md 3.10)."""
    d = os.path.dirname(os.path.abspath(path)) or "."
    while not os.path.isdir(d):
        d = os.path.dirname(d)
    try:
        r = subprocess.run(["git", "-C", d, "rev-parse", "--is-inside-work-tree"], capture_output=True, text=True)
    except OSError:
        return False
    return r.returncode == 0 and r.stdout.strip() == "true"


def load_segments(path, board_id):
    """[(addr, bytes)] for a build: the application .bin at 0x0800C000, and for an image merged with the codec
    (rfoundry_<board>_codec.bin, which runs to the end of flash) the codec sectors as a second segment."""
    data = open(path, "rb").read()
    codec = BOARDS[board_id]["codec"]
    if codec is None or APP_BASE + len(data) <= codec[0]:
        return [(APP_BASE, data)]
    if APP_BASE + len(data) != codec[1]:
        raise F4Error("%s runs to 0x%08X: a build merged with the codec runs to 0x%08X, a plain build ends before "
                      "0x%08X" % (path, APP_BASE + len(data), codec[1], codec[0]))
    cut = codec[0] - APP_BASE
    return [(APP_BASE, data[:cut].rstrip(b"\xff")), (codec[0], data[cut:])]


class F4Radio(stubtool.Radio):
    """A radio running the STM32F4 stub. port: a pyserial-like object; reopen: a callable that opens the port again
    (after the stub enumerated afresh) and returns it, or None."""

    def __init__(self, port, log=print, retries=3, reopen=None):
        super().__init__(port, log=log, retries=retries)
        self.reopen = reopen
        self.flash_kb = 0
        self.rdp = 0
        self.adopted = False
        self.version = 0           # from HELLO
        self.board = None          # the BOARDS key the stub is configured for (version 4), or None
        self.record = b""          # the board record HELLO echoes

    def check_stub(self, image, load, size):
        check_stub_fits(image, load, size)

    # ---- talking to the stub
    def hello(self):
        r = self.request(OP_HELLO, retries=0, timeout=0.5)
        if r[:6] != b"F4STUB" or len(r) < 17:
            raise F4Error("unexpected hello %r: not the STM32F4 stub" % r[:16])
        self.version = r[6]
        self.max_payload = (r[7] << 8) | r[8]
        self.flash_kb = (r[9] << 8) | r[10]
        self.rdp = r[11]
        self.adopted = bool(r[12] & 1)
        self.app_base = int.from_bytes(r[13:17], "big")
        self.spi_size = int.from_bytes(r[17:21], "big") if len(r) >= 24 else 0
        self.spi_id = r[21:24].hex() if len(r) >= 24 else ""
        if self.version >= CONFIG_VERSION:
            if len(r) < HELLO_LEN or len(r) != HELLO_LEN + r[25]:
                raise F4Error("HELLO v%d is %d bytes: malformed" % (self.version, len(r)))
            self.record = bytes(r[HELLO_LEN:])
            self.board = board_by_id(r[24]) if r[12] & HELLO_CONFIGURED else None
        elif self.app_base != APP_BASE:
            raise F4Error("the stub reports the application at 0x%08X, this tool knows 0x%08X" % (self.app_base, APP_BASE))
        return self.version, self.max_payload

    def configure(self, board_id):
        """Send the board's record (CONFIG) and check the echo in the next HELLO; the stub writes nothing before. Also
        what stubtool.Radio.rfoundry_enter calls once the stub answers (board_id None there unless run() passed one). A stub
        already configured with exactly this record is left as it is. board_id None: keep what a configured stub has
        (an --attach without --board), else stay unconfigured (reads of the internal flash only). A stub before version
        4 (2, or 3 with only the image header) has no CONFIG: its wiring is built in (V2_WIRING), so it is refused for a
        radio whose record differs."""
        if board_id is not None and board_id not in BOARDS:
            raise F4Error("%s is not an STM32F4 radio this stub knows (%s)" % (board_id, ", ".join(BOARDS)))
        if self.version < CONFIG_VERSION:
            if board_id is not None and config_record(board_id)[1:] != V2_WIRING:
                raise F4Error("this stub (protocol %d) drives the SPI flash wiring built into it and can't serve "
                              "the %s: restart the radio and enter again" % (self.version, BOARDS[board_id]["name"]))
            self.board = board_id
            return
        if board_id is None:
            if self.board is not None:
                self.log("stub already configured for the %s" % BOARDS[self.board]["name"])
            return
        rec = config_record(board_id)
        if self.board == board_id and self.record == rec:
            return
        self.request(OP_CONFIG, rec, retries=0)
        self.hello()
        if self.board != board_id or self.record != rec or self.app_base != APP_BASE:
            self.board = None
            raise F4Error("the stub did not take the %s's board record (HELLO echoes %s): nothing written"
                          % (BOARDS[board_id]["name"], self.record.hex() or "none"))
        self.log("stub configured for the %s: %s" % (BOARDS[board_id]["name"], "SPI flash %d KB (JEDEC %s)" % (
            self.spi_size // 1024, self.spi_id) if self.spi_size else "no SPI flash answered"))

    def need_board(self, board_id=None):
        """Writes need the stub configured, for board_id when given"""
        if self.board is None or (board_id is not None and self.board != board_id):
            raise F4Error("the stub is not configured for %s: nothing written" % (
                BOARDS[board_id]["name"] if board_id else "a radio (pass --board)"))

    def attach(self):
        """After ENTER: the stub either adopted RadioFoundry's USB device (same port, answers at once) or enumerated again (the
        port goes away and comes back with the same serial number)."""
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            try:
                self.drain(0.05)
                self.hello()
                break
            except (stubtool.DumpError, OSError):
                time.sleep(0.1)
        else:
            if self.reopen is None:
                raise F4Error("the stub does not answer on this port, and no way to open it again was given")
            self.log("waiting for the stub's USB port to come back")
            try:
                self.port.close()
            except OSError:
                pass
            end = time.monotonic() + PORT_WAIT_S
            while True:
                try:
                    self.port = self.reopen()
                    self.hello()
                    break
                except (stubtool.DumpError, OSError):
                    if time.monotonic() > end:
                        raise F4Error("the stub's USB port did not come back within %d s: power-cycle the radio "
                                      "(RadioFoundry starts again) and check the cable" % PORT_WAIT_S)
                    time.sleep(0.3)
        self.log("STM32F4 stub v%d: %d KB flash, RDP byte 0x%02X, %s, USB %s" % (
            self.version, self.flash_kb, self.rdp,
            "configured for the %s" % BOARDS[self.board]["name"] if self.board else
            ("SPI flash %d KB (JEDEC %s)" % (self.spi_size // 1024, self.spi_id) if self.spi_size else
             "not configured yet" if self.version >= CONFIG_VERSION else "no SPI flash"),
            "adopted from RadioFoundry" if self.adopted else "enumerated again"))

    def peek(self, addr, n):
        return self.request(OP_PEEK, addr.to_bytes(4, "big") + n.to_bytes(2, "big"))

    @staticmethod
    def mem_byte(mem):
        return b"" if mem == MEM_INTERNAL else bytes([mem])

    def need_spi(self):
        if not self.spi_size:
            raise F4Error("the stub found no SPI flash (stub version %d%s)" % (
                self.version, ", not configured: pass --board" if self.version >= CONFIG_VERSION and self.board is None else ""))

    def sums(self, addr, count, mem=MEM_INTERNAL):
        """[(crc32, blank)] per 4 KB block from addr (internal flash, or the SPI flash from 0)."""
        out = []
        while count > 0:
            k = min(count, SUM_MAX)
            # internal flash reads at memory speed; the SPI flash at about 0.5 MB/s
            r = self.request(OP_SUMS, addr.to_bytes(4, "big") + k.to_bytes(2, "big") + self.mem_byte(mem),
                             timeout=1.0 + k * (0.01 if mem == MEM_INTERNAL else 0.02))
            if len(r) != 5 * k:
                raise F4Error("SUMS reply is %d bytes, wanted %d" % (len(r), 5 * k))
            out += [(int.from_bytes(r[i * 5:i * 5 + 4], "big"), r[i * 5 + 4] == 1) for i in range(k)]
            addr += k * BLOCK
            count -= k
        return out

    def read_flash(self, addr, n, mem=MEM_INTERNAL):
        """n bytes (a multiple of 4 KB) from addr, every block checked against the stub's CRC-32."""
        if addr % BLOCK or n % BLOCK:
            raise F4Error("reads are whole 4 KB blocks")
        out = bytearray()
        a = addr
        fails = 0
        while a < addr + n:
            count = min(READ_PACK, (addr + n - a) // BLOCK)
            try:
                self.port.reset_input_buffer()
                payload = a.to_bytes(4, "big") + bytes([count, 0]) + self.mem_byte(mem)
                self.port.write(bytes([0x02, OP_READ, 0, len(payload), stubtool.sum8(payload), 0x03]) + payload)
                for i in range(count):
                    status, r = self.read_frame(2.0)
                    if status != OK:
                        raise F4Error("READ at 0x%08X refused: status 0x%02X" % (a, status))
                    if len(r) < 7 or len(r) != 7 + ((r[5] << 8) | r[6]):
                        raise stubtool.FrameError("READ record malformed")
                    blk = self.unpack(int.from_bytes(r[:4], "big"), r[4], r[7:])
                    if blk is None:
                        raise stubtool.FrameError("0x%08X: block does not match the stub's CRC" % a)
                    out += blk
                    a += BLOCK
                fails = 0
            except stubtool.FrameError as e:
                fails += 1
                if fails > self.retries:
                    raise F4Error("0x%08X: no clean read after %d tries (%s)" % (a, fails, e))
                self.log("0x%08X: %s, reading again" % (a, e))
                self.drain()
        return bytes(out)

    def protect(self):
        """The protected regions (board.toml [[flash.protect]], rfoundry/protect.py) of the board the stub is configured
        for. Every erase and program below checks them first, whoever calls it."""
        return BOARDS[self.board].get("protect", []) if self.board in BOARDS else []

    def guard(self, mem, start, end, action="write"):
        try:
            protect.check(self.protect(), mem, start, end, action)
        except protect.ProtectedError as e:
            raise F4Error(str(e))

    def erase(self, sector):
        sec = {i: (a, n) for i, a, n in f405_sectors(self.flash_kb)}
        if sector in sec:
            self.guard("progmem", sec[sector][0] - FLASH_BASE, sum(sec[sector]) - FLASH_BASE, "erase")
        self.request(OP_ERASE, bytes([sector]) + MAGIC.to_bytes(4, "big"), retries=0, timeout=ERASE_TIMEOUT)

    def program(self, addr, data):
        """Program data (a multiple of 4 bytes) at addr in pieces of up to 4 KB, skipping all-0xFF pieces (erased)."""
        if addr % 4 or len(data) % 4:
            raise F4Error("program 0x%08X+%d: words only" % (addr, len(data)))
        self.guard("progmem", addr - FLASH_BASE, addr + len(data) - FLASH_BASE)
        for off in range(0, len(data), BLOCK):
            piece = data[off:off + BLOCK]
            if piece != b"\xff" * len(piece):
                self.request(OP_PROGRAM, (addr + off).to_bytes(4, "big") + piece, retries=0, timeout=2.0)

    def reset_chip(self):
        """System reset: the bootloader runs, then the application (or stays in DFU if sector 3 is blank)."""
        self.request(OP_RESET, MAGIC.to_bytes(4, "big"), retries=0)

    # ---- whole-image work
    def check_board(self):
        if self.flash_kb != 1024:
            raise F4Error("the chip reports %d KB of flash; every radio in BOARDS has 1024" % self.flash_kb)

    def plan(self, board_id, segments):
        """{sector index: new content} for every sector that holds firmware-owned flash, and which of those differ
        from the radio. Owned bytes come from the segments (0xFF where they have none); the rest is read back. A
        segment that covers the codec sectors whole (load_segments) makes them owned too."""
        owned = list(BOARDS[board_id]["owned"])
        codec = BOARDS[board_id]["codec"]
        if codec and any(a == codec[0] and len(d) == codec[1] - codec[0] for a, d in segments):
            owned.append(codec)
        for addr, data in segments:
            if not any(lo <= addr and addr + len(data) <= hi for lo, hi in owned):
                raise F4Error("image data at 0x%08X..0x%08X is outside the flash the firmware owns on the %s (%s): a "
                              "wrong board?" % (addr, addr + len(data) - 1, BOARDS[board_id]["name"],
                              ", ".join("0x%08X..0x%08X" % (lo, hi - 1) for lo, hi in owned)))
        if not segments or segments[0][0] != APP_BASE or len(segments[0][1]) < 8:
            raise F4Error("the image must start at 0x%08X with the vector table" % APP_BASE)
        sp = int.from_bytes(segments[0][1][:4], "little")
        if sp & 0x2FFE0000 != 0x20000000:
            raise F4Error("the image's first word 0x%08X is not a RAM stack pointer: the bootloader would not start it" % sp)
        new, changed, kept = {}, [], {}
        for idx, saddr, size in f405_sectors(self.flash_kb):
            parts = [(max(lo, saddr), min(hi, saddr + size)) for lo, hi in owned if lo < saddr + size and hi > saddr]
            if not parts:
                continue
            whole = parts == [(saddr, saddr + size)]
            buf = bytearray(b"\xff" * size) if whole else bytearray(self.read_flash(saddr, size))
            if not whole:
                kept[idx] = bytes(buf)
            for lo, hi in parts:
                buf[lo - saddr:hi - saddr] = b"\xff" * (hi - lo)
            for addr, data in segments:
                lo, hi = max(addr, saddr), min(addr + len(data), saddr + size)
                if lo < hi:
                    buf[lo - saddr:hi - saddr] = data[lo - addr:hi - addr]
            new[idx] = bytes(buf)
            have = self.sums(saddr, size // BLOCK)
            if any(zlib.crc32(buf[i * BLOCK:(i + 1) * BLOCK]) != crc for i, (crc, _) in enumerate(have)):
                changed.append(idx)
        return new, changed, kept

    def write_image(self, board_id, segments, backup_dir=None, progress=None):
        """Install a build. Order (the stub's interlock enforces it): erase sector 3 first, so the bootloader stays in
        DFU from here until the end; erase and program every other changed sector; program sector 3 except its first
        word; verify everything by CRC; program the first word (the commit); verify it. Returns (written, unchanged)."""
        self.check_board()
        self.need_board(board_id)
        new, changed, kept = self.plan(board_id, segments)
        sec = {i: (a, s) for i, a, s in f405_sectors(self.flash_kb)}
        for idx in changed:			# refused before the first erase, not halfway through
            self.guard("progmem", sec[idx][0] - FLASH_BASE, sum(sec[idx]) - FLASH_BASE, "erase")
        if not changed:
            return 0, len(new)
        sec = {i: (a, s) for i, a, s in f405_sectors(self.flash_kb)}
        for idx in changed:
            if idx in kept:			# bytes that are not ours: a copy first
                if backup_dir is None:
                    raise F4Error("sector %d holds bytes that are not the firmware's (the codec): give a backup "
                                  "directory outside any git tree" % idx)
                path = os.path.join(backup_dir, "%s_sector%d_0x%08X.bin" % (board_id, idx, sec[idx][0]))
                if inside_git_tree(path):
                    raise F4Error("%s is inside a git work tree: flash contents are never written there" % path)
                with open(path, "wb") as f:
                    f.write(kept[idx])
                self.log("sector %d's current content saved to %s (it holds codec bytes)" % (idx, path))
        order = [3] + [i for i in changed if i != 3]
        steps = len(order)
        self.erase(3)
        for n, idx in enumerate(order[1:], 1):
            if progress:
                progress(n, steps)
            a, _ = sec[idx]
            self.erase(idx)
            self.program(a, new[idx])
        if progress:
            progress(steps, steps)
        self.program(APP_BASE + 4, new[3][4:])
        expect = {i: new[i] for i in order}
        expect[3] = b"\xff\xff\xff\xff" + new[3][4:]
        bad = [i for i in order if self.differs(sec[i][0], expect[i])]
        if bad:
            raise F4Error("verify before the commit: sector(s) %s differ. The application is not committed: the radio "
                          "starts in its bootloader (DFU) until an install finishes" % bad)
        self.program(APP_BASE, new[3][:4])
        if self.differs(APP_BASE, new[3][:BLOCK]):
            raise F4Error("the commit word did not verify")
        return len(order), len(new) - len(order)

    def differs(self, addr, data):
        sums = self.sums(addr, len(data) // BLOCK)
        return any(zlib.crc32(data[i * BLOCK:(i + 1) * BLOCK]) != crc for i, (crc, _) in enumerate(sums))

    def verify_file(self, data, start=FLASH_BASE, mem=MEM_INTERNAL):
        """Block addresses where the flash differs from data (a whole number of 4 KB blocks from start)."""
        sums = []
        for off in range(0, len(data) // BLOCK, 256):         # 1 MB per request: a slow SPI flash stays in its time-out
            sums += self.sums(start + off * BLOCK, min(256, len(data) // BLOCK - off), mem)
        return [start + i * BLOCK for i, (crc, _) in enumerate(sums) if zlib.crc32(data[i * BLOCK:(i + 1) * BLOCK]) != crc]

    def spi_erase(self, addr):
        self.guard("data", addr, addr + BLOCK, "erase")
        self.request(OP_XERASE, addr.to_bytes(4, "big") + MAGIC.to_bytes(4, "big"), retries=0, timeout=2.0)

    def spi_write(self, data, start=0, progress=None):
        """Write data (whole 4 KB sectors) into the SPI flash from start: only the sectors whose CRC differs are erased
        and programmed (blank pieces skipped), each read back by the stub, then all checked again. A restore: the
        board's protected regions are carved out, so the radio keeps its own factory data there whatever the file
        holds. Returns (written, unchanged); protected sectors count as unchanged."""
        self.need_spi()
        self.need_board()
        if start % BLOCK or len(data) % BLOCK or start + len(data) > self.spi_size:
            raise F4Error("SPI flash writes are whole 4 KB sectors inside the chip's %d KB" % (self.spi_size // 1024))
        keep = [p for p in protect.overlapping(self.protect(), "data", start, start + len(data))]
        if keep:
            self.log("left as the radio has them (protected): %s" % "; ".join(protect.describe(p) for p in keep))
        free = set(protect.carved_units(self.protect(), "data", start, start + len(data), BLOCK))
        todo = [a for a in self.verify_file(data, start, MEM_SPI) if a in free]
        for n, a in enumerate(todo, 1):
            sec = data[a - start:a - start + BLOCK]
            self.spi_erase(a)
            for off in range(0, BLOCK, 1024):
                piece = sec[off:off + 1024]
                if piece != b"\xff" * len(piece):
                    self.request(OP_XPROGRAM, (a + off).to_bytes(4, "big") + piece, retries=0, timeout=2.0)
            if progress:
                progress(n, len(todo))
        bad = [a for a in self.verify_file(data, start, MEM_SPI) if a in free] if todo else []
        if bad:
            raise F4Error("SPI flash verify: %d sector(s) differ, first at 0x%06X" % (len(bad), bad[0]))
        return len(todo), len(data) // BLOCK - len(todo)

    # ---- the SPI flash's security registers (docs/spec/stub-stm32f4.md 6.2): three 256-byte registers, which the
    # DM-1701 family's factory fills with calibration (1, 2) and flags (3). Each has a one-time lock bit in status
    # register 2 (LB1-LB3); nothing here, or in the stub, ever writes a status register
    def need_sreg(self):
        self.need_spi()
        if self.version < SREG_VERSION:
            raise F4Error("this stub (protocol %d) has no security register ops: restart the radio and enter again "
                          "with this tool's stub" % self.version)
        if self.spi_id[:2] != "%02x" % SREG_MFR:
            raise F4Error("the SPI flash (JEDEC %s) is not a Winbond part: its security register commands are not "
                          "known, nothing sent" % self.spi_id)

    def sr2(self):
        """Status register 2 (read only)"""
        self.need_sreg()
        r = self.request(OP_SREAD, b"\x00")
        if len(r) != 1:
            raise F4Error("SREAD 0 reply is %d bytes, wanted 1" % len(r))
        return r[0]

    @staticmethod
    def sreg_locks(sr2):
        """[locked] for registers 1-3, from status register 2's LB1-LB3"""
        return [bool(sr2 & (SR2_LB1 << i)) for i in range(SREG_COUNT)]

    def sreg_read(self, reg):
        """(status register 2, register reg's 256 bytes), checked against the stub's CRC-32"""
        self.need_sreg()
        if not 1 <= reg <= SREG_COUNT:
            raise F4Error("security registers are 1 to %d" % SREG_COUNT)
        for _ in range(self.retries + 1):
            r = self.request(OP_SREAD, bytes([reg]))
            if len(r) != 5 + SREG_LEN:
                raise F4Error("SREAD %d reply is %d bytes, wanted %d" % (reg, len(r), 5 + SREG_LEN))
            data = bytes(r[5:])
            if zlib.crc32(data) & 0xFFFFFFFF == int.from_bytes(r[1:5], "big"):
                return r[0], data
            self.log("security register %d: CRC mismatch, reading again" % reg)
        raise F4Error("security register %d: no read matching the stub's CRC after %d tries" % (reg, self.retries + 1))

    def sreg_read_all(self):
        """(status register 2, the three registers: 768 bytes)"""
        out, sr2 = b"", 0
        for reg in range(1, SREG_COUNT + 1):
            sr2, data = self.sreg_read(reg)
            out += data
        return sr2, out

    @staticmethod
    def sreg_slice(data, reg):
        return data[(reg - 1) * SREG_LEN:reg * SREG_LEN]

    def sreg_write(self, data, backup_dir):
        """Put data (768 bytes: registers 1-3) into the security registers, writing only the registers that differ.
        Refused before anything is sent when a register to write is locked, or data has another length. The current
        content is saved to backup_dir first (never inside a work tree). Each register is erased, programmed and read
        back by the stub, then all three are read again here. Returns the registers written."""
        self.need_board()
        self.need_sreg()
        if len(data) != SREG_COUNT * SREG_LEN:
            raise F4Error("security register data is %d bytes, not %d (registers 1-%d): nothing written"
                          % (len(data), SREG_COUNT * SREG_LEN, SREG_COUNT))
        sr2, have = self.sreg_read_all()
        todo = [reg for reg in range(1, SREG_COUNT + 1) if self.sreg_slice(have, reg) != self.sreg_slice(data, reg)]
        if not todo:
            return []
        for reg in todo:		# factory calibration and flags: refused, not carved (a register is all or nothing)
            self.guard("secreg", (reg - 1) * SREG_LEN, reg * SREG_LEN)
        locked = [reg for reg in todo if self.sreg_locks(sr2)[reg - 1]]
        if locked:
            raise F4Error("security register(s) %s differ but are locked for good (status register 2 = 0x%02X): "
                          "nothing written" % (", ".join(map(str, locked)), sr2))
        if backup_dir is None:
            raise F4Error("give a backup directory: the security registers' current content is saved first")
        path = os.path.join(backup_dir, "%s_secreg_%s.bin" % (self.board, time.strftime("%Y%m%d-%H%M%S")))
        if inside_git_tree(path):
            raise F4Error("%s is inside a work tree: factory calibration is never written there" % path)
        with open(path, "wb") as f:
            f.write(have)
        self.log("security registers' current content saved to %s" % path)
        for reg in todo:
            try:
                self.request(OP_SWRITE, bytes([reg]) + MAGIC.to_bytes(4, "big") + self.sreg_slice(data, reg),
                             retries=0, timeout=SREG_TIMEOUT)
            except stubtool.DumpError as e:
                raise F4Error("security register %d: %s (0x0F: refused, locked; 0x10: did not read back as written). "
                              "The old content is in %s" % (reg, e, path))
        _, now = self.sreg_read_all()
        bad = [reg for reg in range(1, SREG_COUNT + 1) if self.sreg_slice(now, reg) != self.sreg_slice(data, reg)]
        if bad:
            raise F4Error("security register(s) %s differ after the write. The old content is in %s"
                          % (", ".join(map(str, bad)), path))
        return todo

    def dump(self, path, start=FLASH_BASE, end=None, progress=None, mem=MEM_INTERNAL):
        if end is None:
            end = start + (self.spi_size if mem == MEM_SPI else self.flash_kb * 1024)
        if inside_git_tree(path):
            raise F4Error("%s is inside a git work tree: a flash dump holds the stock bootloader and the codec, which "
                          "are never committed" % path)
        with open(path, "wb") as f:
            a = start
            while a < end:
                n = min(64 * BLOCK, end - a)
                f.write(self.read_flash(a, n, mem))
                a += n
                if progress:
                    progress(a, end)


def board_from_version(radio):
    """The board id from a running RadioFoundry's debug link VERSION reply (board=...), or None."""
    r = radio.dl_request(0x02, b"")
    if not r or r[0] != 0:
        return None
    for line in r[1:].decode("ascii", "replace").splitlines():
        if line.startswith("board="):
            return board_for(line[6:].strip())
    return None


def stub_stack_top(image):
    """The initial stack pointer the image's entry code loads, or None if the image does not start with f4stub_entry
    (stub/stm32f4/f4stub_main.c): `ldr r1, [pc, #imm]` and `mov sp, r1`, the literal in the pool after the branch."""
    if len(image) < 8 or image[1] != 0x49 or image[2:4] != b"\x8d\x46":
        return None
    lit = 4 + image[0] * 4                     # Thumb LDR literal: Align(PC, 4) + imm8 * 4, PC = 4 here
    if lit + 4 > len(image):
        return None
    return int.from_bytes(image[lit:lit + 4], "little")


def check_stub_fits(image, load, size):
    """Refuse an image that is not the STM32F4 stub, or a span RadioFoundry reports that is not where the stub runs. RadioFoundry stages
    the image in an 8 KB buffer and copies it to its link address before the jump (docs/spec/debuglink.md 3.18), so
    the HR_C7000 rule (bss and stack inside the span) does not apply: after the jump the stub owns all of SRAM."""
    top = stub_stack_top(image)
    if top is None or b"F4STUB" not in image:
        raise F4Error("this does not look like the STM32F4 stub image (no f4stub_entry code at its start)")
    if load != RUN_ADDR:
        raise F4Error("RadioFoundry reports its stub span at 0x%X; the STM32F4 stub runs at 0x%X" % (load, RUN_ADDR))
    if not RUN_ADDR + len(image) < top <= RUN_ADDR + SRAM_SIZE:
        raise F4Error("this stub image's stack top 0x%X is outside the SRAM it runs in (0x%X-0x%X)"
                      % (top, RUN_ADDR, RUN_ADDR + SRAM_SIZE))


# The image header (stub/stm32f4/f4stub_header.h, docs/spec/stub-stm32f4.md 3.1): 16 bytes at offset 0x10, little
# endian. RadioFoundry started with CONFIG_WATCHDOG enters only a stub whose header has the magic, a version of at least
# HDR_VERSION_FEEDS_IWDG and the feed flag. --self-test checks these numbers against f4stub_header.h
HDR_OFFSET, HDR_MAGIC, HDR_LAYOUT, HDR_SIZE = 0x10, 0x48533446, 1, 16
HDR_VERSION_FEEDS_IWDG, HDR_FLAG_FEEDS_IWDG = 3, 0x1


def stub_header(image):
    """The image header as a dict (layout, size, version, flags, feeds_iwdg), or None when the image has none (every
    stub before version 3)."""
    if len(image) < HDR_OFFSET + HDR_SIZE:
        return None
    magic, layout, size, version, flags = struct.unpack_from("<IHHII", image, HDR_OFFSET)
    if magic != HDR_MAGIC or layout < HDR_LAYOUT or size < HDR_SIZE:
        return None
    return {"layout": layout, "size": size, "version": version, "flags": flags,
            "feeds_iwdg": version >= HDR_VERSION_FEEDS_IWDG and bool(flags & HDR_FLAG_FEEDS_IWDG)}


def describe_stub(image):
    h = stub_header(image)
    if h is None:
        return "STM32F4 stub image (%d bytes, no header: older than version %d, which a watchdog build refuses)" % (
            len(image), HDR_VERSION_FEEDS_IWDG)
    return "STM32F4 stub image v%d (%d bytes%s)" % (h["version"], len(image),
                                                    ", feeds the IWDG" if h["feeds_iwdg"] else "")


def stub_image():
    """The embedded stub build, checked against its SHA-256"""
    blob = zlib.decompress(base64.b64decode("".join(F4STUB_B64)))
    if hashlib.sha256(blob).hexdigest() != F4STUB_SHA256:
        raise F4Error("the embedded STM32F4 stub fails its own checksum")
    return blob


def embed(path):
    """Rewrite the embedded stub in this file from a build"""
    blob = open(path, "rb").read()
    b64 = base64.b64encode(zlib.compress(blob, 9)).decode()
    lines = ["# --- F4STUB begin (f4stub.py --embed rewrites everything up to the end marker)",
             'F4STUB_SHA256 = "%s"' % hashlib.sha256(blob).hexdigest(), "F4STUB_B64 = ("]
    lines += ['    "%s"' % b64[i:i + 104] for i in range(0, len(b64), 104)]
    lines += [")", "# --- F4STUB end"]
    me = os.path.abspath(__file__)
    text = open(me).read()
    a = text.index("# --- F4STUB begin")
    b = text.index("# --- F4STUB end") + len("# --- F4STUB end")
    open(me, "w").write(text[:a] + "\n".join(lines) + text[b:])
    print("embedded %s: %d bytes, sha256 %s" % (path, len(blob), hashlib.sha256(blob).hexdigest()))


def run(a, port, reopen, log=print):
    """stubtool.py's STM32 branch: a is its parsed arguments, port the open serial port."""
    radio = F4Radio(port, log=log, retries=a.retries, reopen=reopen)
    board = a.board
    if a.rfoundry_enter:
        found = board_from_version(radio)
        if board is None:
            board = found
        elif found is not None and found != board:
            raise F4Error("the radio says it is a %s, not a %s" % (found, board))
        image = open(a.rfoundry_stub, "rb").read() if a.rfoundry_stub else stub_image()
        log(describe_stub(image))
        radio.rfoundry_enter(image, RUN_ADDR, board=board)
    else:
        radio.attach()
        if board is None and radio.board is not None:
            board = radio.board            # a stub configured by an earlier run
    radio.check_board()
    radio.configure(board)
    if a.write_app:
        if board is None:
            raise F4Error("which radio? pass --board (%s)" % ", ".join(sorted(BOARDS)))
        segs = load_segments(a.write_app, board)
        t0 = time.monotonic()
        nw, nk = radio.write_image(board, segs, backup_dir=a.backup_dir or os.path.expanduser("~"),
                                   progress=lambda i, n: log("sector step %d of %d" % (i, n)))
        log("%s: %s, %d sector(s) written, %d unchanged, verified by CRC, %.0f s" % (
            a.write_app, " + ".join("%d bytes at 0x%08X" % (len(d), ad) for ad, d in segs), nw, nk, time.monotonic() - t0))
    if a.peek:
        addr, n = stubtool.parse_range(a.peek, 0)
        data = b"".join(radio.peek(addr + i, min(1024, n - i)) for i in range(0, n, 1024))
        for i in range(0, len(data), 16):
            log("%08X  %s" % (addr + i, data[i:i + 16].hex(" ")))
    if a.verify_progmem:
        data = open(a.verify_progmem, "rb").read()
        bad = radio.verify_file(data[:len(data) // BLOCK * BLOCK])
        log("internal flash vs %s: %s" % (a.verify_progmem, "identical" if not bad else
            "%d blocks differ, first at 0x%08X" % (len(bad), bad[0])))
    if a.progmem:
        lo, hi = stubtool.parse_range(a.progmem_range, radio.flash_kb * 1024)
        t0 = time.monotonic()
        radio.dump(a.progmem, FLASH_BASE + lo // BLOCK * BLOCK, FLASH_BASE + (hi + BLOCK - 1) // BLOCK * BLOCK)
        log("%s: internal flash 0x%X..0x%X in %.0f s; sha256 %s" % (a.progmem, lo, hi, time.monotonic() - t0,
                                                                    stubtool.fingerprint(a.progmem)))
    if a.q128 or a.verify_q128 or a.write_q128:
        radio.need_spi()
    if a.verify_q128:
        data = open(a.verify_q128, "rb").read()
        bad = radio.verify_file(data[:len(data) // BLOCK * BLOCK], 0, MEM_SPI)
        log("SPI flash vs %s: %s" % (a.verify_q128, "identical" if not bad else
            "%d blocks differ, first at 0x%06X" % (len(bad), bad[0])))
    if a.q128:
        lo, hi = stubtool.parse_range(a.q128_range, radio.spi_size)
        lo, hi = lo // BLOCK * BLOCK, min(radio.spi_size, (hi + BLOCK - 1) // BLOCK * BLOCK)
        t0 = time.monotonic()
        radio.dump(a.q128, lo, hi, mem=MEM_SPI)
        log("%s: SPI flash 0x%X..0x%X in %.0f s; sha256 %s" % (a.q128, lo, hi, time.monotonic() - t0,
                                                              stubtool.fingerprint(a.q128)))
    if a.write_q128:
        data = open(a.write_q128, "rb").read()
        start = stubtool.parse_range(a.q128_range, radio.spi_size)[0] if a.q128_range else 0
        t0 = time.monotonic()
        nw, nk = radio.spi_write(data, start, progress=lambda i, n: log("SPI sector %d of %d" % (i, n)) if i % 64 == 0 or i == n else None)
        log("%s: SPI flash from 0x%06X, %d sector(s) written, %d unchanged, verified by CRC, %.0f s" % (
            a.write_q128, start, nw, nk, time.monotonic() - t0))
    if a.secreg or a.write_secreg:
        def locks(sr2):
            return "status register 2 = 0x%02X, %s" % (sr2, ", ".join(
                "register %d %s" % (i + 1, "LOCKED (one-time)" if lk else "unlocked")
                for i, lk in enumerate(F4Radio.sreg_locks(sr2))))
        log("SPI flash security registers: %s" % locks(radio.sr2()))
    if a.secreg:
        if inside_git_tree(a.secreg):
            raise F4Error("%s is inside a git work tree: factory calibration is never written there" % a.secreg)
        _, data = radio.sreg_read_all()
        with open(a.secreg, "wb") as f:
            f.write(data)
        log("%s: security registers 1-3 (768 bytes, each checked by CRC); sha256 %s" % (
            a.secreg, stubtool.fingerprint(a.secreg)))
    if a.write_secreg:
        if not a.confirm_secreg:
            raise F4Error("--write-secreg rewrites the factory calibration: add --confirm-secreg")
        if board is None:
            raise F4Error("which radio? pass --board (%s)" % ", ".join(sorted(BOARDS)))
        data = open(a.write_secreg, "rb").read()
        done = radio.sreg_write(data, a.backup_dir or os.path.expanduser("~"))
        log("%s: %s" % (a.write_secreg, "security register(s) %s written, each read back" % ", ".join(map(str, done))
                        if done else "all three registers already hold it, nothing written"))
    if a.boot:
        if a.boot != "reset":
            raise F4Error("on the STM32 radios --boot takes only reset")
        radio.reset_chip()
        log("reset sent: the bootloader starts the application (or stays in DFU if none is committed)")
    return 0


# ---- self-test: the stub's rules simulated behind a fake serial port

class FakeF4:
    """The stub's protocol and safety rules (stub/common/stubeng.c) on a simulated 1 MB flash."""

    def __init__(self, flash, flash_kb=1024, fail_after=None, spi=None, version=VERSION):
        self.version = version
        self.record = b"" if version >= CONFIG_VERSION else bytes([1]) + V2_WIRING     # version 2: wiring built in
        self.configs = 0
        self.flash = bytearray(flash)
        self.spi = bytearray(spi) if spi is not None else None
        self.flash_kb = flash_kb
        self.inb = bytearray()
        self.out = bytearray()
        self.timeout = 0.02
        self.erased, self.programs, self.resets = [], 0, 0
        self.fail_after = fail_after        # programs before the "cable" is pulled
        self.jedec = bytes([0xEF, 0x40, 0x18])
        # The security registers (version 5): 3 x 256 bytes, status register 2 (QE set, as many chips ship) and the
        # registers each SWRITE erased
        self.sregs = [bytearray((i * 17 + r * 5) & 0xFF for i in range(SREG_LEN)) for r in range(SREG_COUNT)]
        self.sr2 = 0x02
        self.sreg_writes = []
        self.crc_flips = 0

    def reset_input_buffer(self):
        self.out.clear()

    def flush(self):
        pass

    def close(self):
        pass

    def read(self, n):
        r = bytes(self.out[:n])
        del self.out[:n]
        return r

    def frame(self, st, body=b""):
        self.out += bytes([0x02, st, len(body) >> 8, len(body) & 0xFF, stubtool.sum8(body), 0x03]) + bytes(body)

    def write(self, b):
        if self.fail_after is not None and self.programs >= self.fail_after:
            raise OSError("cable pulled")
        self.inb += b
        while len(self.inb) >= 6:
            n = (self.inb[2] << 8) | self.inb[3]
            if len(self.inb) < 6 + n:
                return
            op, body = self.inb[1], bytes(self.inb[6:6 + n])
            del self.inb[:6 + n]
            self.handle(op, body)

    def mem(self, p, base_len, addr):
        """(buffer, offset) for a SUMS / READ request's memory byte"""
        if len(p) > base_len and p[base_len] == MEM_SPI:
            return self.spi, addr
        return self.flash, addr - FLASH_BASE

    def w(self, addr):
        o = addr - FLASH_BASE
        return int.from_bytes(self.flash[o:o + 4], "little")

    def handle(self, op, p):
        if op in (OP_ERASE, OP_PROGRAM, OP_XERASE, OP_XPROGRAM, OP_SWRITE) and not self.record:
            return self.frame(NOCONFIG)
        if op == OP_HELLO:
            rec = self.record
            on = bool(rec) and self.version >= CONFIG_VERSION
            body = (b"F4STUB" + bytes([self.version]) + (4104).to_bytes(2, "big") + self.flash_kb.to_bytes(2, "big")
                    + bytes([0x55, 1 | (HELLO_CONFIGURED if on else 0)])
                    + (int.from_bytes(rec[8:12], "big") if rec else 0).to_bytes(4, "big")
                    + (len(self.spi) if self.spi and rec else 0).to_bytes(4, "big") + self.jedec)
            if self.version >= CONFIG_VERSION:
                body += bytes([rec[0] if on else 0, len(rec)]) + rec
            self.frame(OK, body)
        elif op == OP_CONFIG and self.version >= CONFIG_VERSION:
            self.configs += 1
            ok = len(p) == REC_LEN and p[0] != 0 and p[1] == REC_LAYOUT and int.from_bytes(p[8:12], "big") == APP_BASE
            self.record = bytes(p) if ok else b""
            self.frame(OK if ok else BADARG)
        elif op == OP_SUMS:
            a, k = int.from_bytes(p[:4], "big"), int.from_bytes(p[4:6], "big")
            mem, o = self.mem(p, 6, a)
            body = b""
            for i in range(k):
                blk = bytes(mem[o + i * BLOCK:o + (i + 1) * BLOCK])
                body += (zlib.crc32(blk) & 0xFFFFFFFF).to_bytes(4, "big") + bytes([blk == b"\xff" * BLOCK])
            self.frame(OK, body)
        elif op == OP_READ:
            a, k = int.from_bytes(p[:4], "big"), p[4]
            mem, o0 = self.mem(p, 6, a)
            for i in range(k):
                o = o0 + i * BLOCK
                blk = bytes(mem[o:o + BLOCK])
                blank = blk == b"\xff" * BLOCK
                data = b"" if blank else blk
                self.frame(OK, (zlib.crc32(blk) & 0xFFFFFFFF).to_bytes(4, "big") + bytes([2 if blank else 0])
                           + len(data).to_bytes(2, "big") + data)
        elif op == OP_PEEK:
            a, n = int.from_bytes(p[:4], "big"), int.from_bytes(p[4:6], "big")
            self.frame(OK, bytes(self.flash[a - FLASH_BASE:a - FLASH_BASE + n]))
        elif op == OP_ERASE:
            s = p[0]
            if int.from_bytes(p[1:5], "big") != MAGIC or s < 3 or (s != 3 and self.w(APP_BASE) != 0xFFFFFFFF):
                return self.frame(REFUSED)
            _, a, size = f405_sectors()[s]
            self.flash[a - FLASH_BASE:a - FLASH_BASE + size] = b"\xff" * size
            self.erased.append(s)
            self.frame(OK)
        elif op == OP_PROGRAM:
            a, data = int.from_bytes(p[:4], "big"), p[4:]
            if a < APP_BASE or (a + len(data) > 0x08010000 and self.w(APP_BASE) != 0xFFFFFFFF):
                return self.frame(REFUSED)
            o = a - FLASH_BASE
            if self.flash[o:o + len(data)] != b"\xff" * len(data):
                return self.frame(REFUSED)
            self.flash[o:o + len(data)] = data
            self.programs += 1
            self.frame(OK)
        elif op == OP_XERASE:
            a = int.from_bytes(p[:4], "big")
            if int.from_bytes(p[4:8], "big") != MAGIC:
                return self.frame(REFUSED)
            self.spi[a:a + BLOCK] = b"\xff" * BLOCK
            self.spi_erases = getattr(self, "spi_erases", 0) + 1
            self.frame(OK)
        elif op == OP_XPROGRAM:
            a, data = int.from_bytes(p[:4], "big"), p[4:]
            if self.spi[a:a + len(data)] != b"\xff" * len(data):
                return self.frame(REFUSED)
            self.spi[a:a + len(data)] = data
            self.frame(OK)
        elif op in (OP_SREAD, OP_SWRITE) and self.version >= SREG_VERSION:
            write = op == OP_SWRITE
            reg = p[0] if p else 99
            if (len(p) != (5 + SREG_LEN if write else 1) or not (1 if write else 0) <= reg <= SREG_COUNT
                    or not (self.spi and self.record) or self.jedec[0] != SREG_MFR):
                return self.frame(BADARG)
            if write and int.from_bytes(p[1:5], "big") != MAGIC:
                return self.frame(REFUSED)
            if write:
                if self.sr2 & (SR2_LB1 << (reg - 1)):
                    return self.frame(REFUSED)
                self.sregs[reg - 1][:] = p[5:]
                self.sreg_writes.append(reg)
                return self.frame(OK)
            if reg == 0:
                return self.frame(OK, bytes([self.sr2]))
            data = bytes(self.sregs[reg - 1])
            crc = zlib.crc32(data) & 0xFFFFFFFF
            if self.crc_flips:              # a reply damaged on the way (the frame sum still right)
                self.crc_flips -= 1
                crc ^= 1
            self.frame(OK, bytes([self.sr2]) + crc.to_bytes(4, "big") + data)
        elif op == OP_RESET:
            self.resets += 1
            self.frame(OK)
        else:
            self.frame(BADOP)


def self_test():
    fails = []

    def check(c, what):
        if not c:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
    # The image header: RadioFoundry with CONFIG_WATCHDOG enters only a stub whose header says it feeds the IWDG
    # (docs/spec/watchdog.md 3.1). The numbers here match f4stub_header.h, and the embedded image has the header
    import re
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        hdr_src = open(os.path.join(root, "stub", "stm32f4", "f4stub_header.h")).read()
    except OSError:
        hdr_src = None  # run outside the repository: only the image checks below
    if hdr_src is not None:
        defs = dict((m.group(1), int(m.group(2), 0)) for m in
                    re.finditer(r"#define\s+(F4STUB_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u", hdr_src))
        for name, mine in (("F4STUB_HDR_OFFSET", HDR_OFFSET), ("F4STUB_HDR_MAGIC", HDR_MAGIC),
                           ("F4STUB_HDR_LAYOUT", HDR_LAYOUT), ("F4STUB_HDR_SIZE", HDR_SIZE),
                           ("F4STUB_VERSION_FEEDS_IWDG", HDR_VERSION_FEEDS_IWDG),
                           ("F4STUB_FLAG_FEEDS_IWDG", HDR_FLAG_FEEDS_IWDG)):
            check(defs.get(name) == mine, "f4stub.py's %s matches f4stub_header.h (%r, %r)" % (name, mine, defs.get(name)))
    h = stub_header(stub_image())
    check(h is not None, "the embedded stub has the image header")
    if h is not None:
        check(h["version"] >= HDR_VERSION_FEEDS_IWDG and h["flags"] & HDR_FLAG_FEEDS_IWDG and h["feeds_iwdg"],
              "the embedded stub's header: version %d >= %d and the feed flag" % (h["version"], HDR_VERSION_FEEDS_IWDG))
        if hdr_src is not None:
            check(h["version"] == defs.get("F4STUB_VERSION"), "the embedded stub is the current version (%d)" % h["version"])
    old = bytearray(stub_image())
    old[HDR_OFFSET:HDR_OFFSET + 4] = b"\0\0\0\0"
    check(stub_header(bytes(old)) is None, "an image without the magic has no header")
    old = bytearray(stub_image())
    old[HDR_OFFSET + 8:HDR_OFFSET + 12] = (HDR_VERSION_FEEDS_IWDG - 1).to_bytes(4, "little")
    check(not stub_header(bytes(old))["feeds_iwdg"], "a version before the feeds doesn't feed")
    boot = bytes((i * 7 + 1) & 0xFF for i in range(0xC000))

    def stock(seed):
        f = bytearray(b"\xff" * 0x100000)
        f[:0xC000] = boot
        f[0xC000:] = bytes((i * seed + 3) & 0xFF for i in range(0x100000 - 0xC000))
        f[0xC000:0xC004] = (0x2001FFFC).to_bytes(4, "little")
        return f

    def app(n, seed):
        d = bytearray((i * seed + (i >> 9)) & 0xFF for i in range(n))
        d[:4] = (0x2001FFFC).to_bytes(4, "little")
        return bytes(d)

    import tempfile
    tmpdir = tempfile.TemporaryDirectory(prefix="rfoundry-f4stub-")
    tmp = tmpdir.name

    # DM-1701: the application in sectors 3-9; codec sectors 10 and 11 and the bootloader never touched
    f = stock(5)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    check(r.board is None and r.record == b"", "hello: a fresh stub is unconfigured")
    r.configure("dm1701")
    check(r.flash_kb == 1024 and r.adopted, "hello parsed")
    segs = [(APP_BASE, app(300000, 3))]
    nw, nk = r.write_image("dm1701", segs, backup_dir=tmp)
    fl = fake.flash
    check(nw == 7 and nk == 0, "dm1701: sectors 3-9 written (%d, %d)" % (nw, nk))
    check(fake.erased[0] == 3 and sorted(fake.erased) == [3, 4, 5, 6, 7, 8, 9],
          "dm1701: sector 3 erased first, only owned sectors %s" % fake.erased)
    check(bytes(fl[:0xC000]) == boot, "dm1701: bootloader untouched")
    check(bytes(fl[0xC0000:]) == bytes(f[0xC0000:]), "dm1701: codec sectors untouched")
    check(bytes(fl[0xC000:0xC000 + 300000]) == segs[0][1], "dm1701: application in place")
    check(bytes(fl[0xC000 + 300000:0xC0000]) == b"\xff" * (0xC0000 - 0xC000 - 300000), "dm1701: the old tail is cleared")
    fake.erased = []
    nw, nk = r.write_image("dm1701", segs, backup_dir=tmp)
    check(nw == 0 and nk == 7 and fake.erased == [], "dm1701: the same image again writes nothing")
    segs2 = [(APP_BASE, app(300000, 3)[:200000] + app(100000, 9))]
    fake.erased = []
    nw, _ = r.write_image("dm1701", segs2, backup_dir=tmp)
    check(sorted(fake.erased) == [3, 5, 6] and nw == 3, "dm1701: only changed sectors (plus 3) %s" % fake.erased)

    # A build merged with the codec (load_segments splits it): sectors 10 and 11 written whole, too
    merged = bytearray(b"\xff" * (0x100000 - 0xC000))
    merged[:300000] = app(300000, 3)
    merged[0xD19F0 - 0xC000:0xD19F0 - 0xC000 + 0x12082] = app(0x12082, 13)
    merged[0xE3A74 - 0xC000:0xE3A74 - 0xC000 + 0x1021C] = app(0x1021C, 17)
    merged[0xFFF80 - 0xC000:] = app(0x80, 19)
    mp = os.path.join(tmp, "rfoundry_dm1701_codec.bin")
    open(mp, "wb").write(merged)
    msegs = load_segments(mp, "dm1701")
    check([(a, len(d)) for a, d in msegs] == [(APP_BASE, 300000), (0x080C0000, 0x40000)], "merged image split in two")
    fake.erased = []
    nw, _ = r.write_image("dm1701", msegs, backup_dir=tmp)
    check(sorted(fake.erased) == [3, 5, 6, 10, 11], "merged image: the codec sectors written too %s" % fake.erased)
    check(bytes(fake.flash[0xC000:]) == bytes(merged), "merged image in place")
    pp = os.path.join(tmp, "plain.bin")
    open(pp, "wb").write(app(5000, 3))
    check(load_segments(pp, "md380") == [(APP_BASE, app(5000, 3))], "md380: a plain image is one segment")

    # An image into the codec sectors that is not a whole merged image, and a bad vector table, are refused before
    # anything is erased
    fake.erased = []
    for bad, what in (([(APP_BASE, app(0xB8000, 3))], "reaches the codec sectors"),
                      ([(APP_BASE, app(5000, 3)), (0x080C0000, app(0x1000, 3))], "part of the codec sectors"),
                      ([(APP_BASE, b"\x00" * 64)], "no stack pointer"),
                      ([(0x08004000, app(64, 3))], "bootloader")):
        try:
            r.write_image("dm1701", bad, backup_dir=tmp)
            check(False, "refused: " + what)
        except F4Error:
            check(fake.erased == [], "refused before erasing: " + what)
    short = os.path.join(tmp, "short_codec.bin")
    open(short, "wb").write(bytes(merged[:-0x1000]))
    try:
        load_segments(short, "dm1701")
        check(False, "an image ending inside the codec sectors refused")
    except F4Error:
        check(True, "an image ending inside the codec sectors refused")

    # Interrupted after a few programs: the first word is still erased, so the bootloader stays in DFU
    f = stock(7)
    fake = FakeF4(f, fail_after=10)
    r = F4Radio(fake, log=quiet)
    r.hello()
    r.configure("dm1701")
    try:
        r.write_image("dm1701", segs, backup_dir=tmp)
        check(False, "interrupted write raises")
    except OSError:
        pass
    check(fake.w(APP_BASE) == 0xFFFFFFFF, "interrupted: the application is not committed (DFU on the next start)")
    check(bytes(fake.flash[:0xC000]) == boot, "interrupted: bootloader untouched")
    fake.fail_after = None
    nw, _ = r.write_image("dm1701", segs, backup_dir=tmp)
    check(bytes(fake.flash[0xC000:0xC000 + 300000]) == segs[0][1], "a second run completes the install")

    # MD-380: sectors 3-7 for the application, the codec sectors 8-11 untouched
    f = stock(13)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    r.configure("md380")
    segs = [(APP_BASE, app(0x08080000 - APP_BASE, 17))]
    nw, nk = r.write_image("md380", segs, backup_dir=tmp)
    fl = fake.flash
    check(sorted(fake.erased) == [3, 4, 5, 6, 7], "md380: sectors 3-7 %s" % fake.erased)
    check(bytes(fl[0x80000:]) == bytes(f[0x80000:]), "md380: codec sectors untouched")
    check(bytes(fl[0xC000:0x80000]) == segs[0][1], "md380: application in place")
    try:
        r.write_image("md380", [(APP_BASE, app(0x08080000 - APP_BASE + 4, 3))], backup_dir=tmp)
        check(False, "md380: an image into the codec sectors is refused")
    except F4Error:
        pass

    # A board with other wiring: the MD-380's application sectors, the SPI flash on SPI2 in mode 3 with chip select
    # PB12, and a second chip select on that bus (PD14) held high. A fixture, so the test doesn't need such a board
    check(all(b["id"] != test_board()["id"] for b in BOARDS.values()),
          "the test profile's id 0x%02X is no board's (tools/boards.py STUB_TEST_ID)" % test_board()["id"])
    BOARDS[TEST_BOARD] = test_board()
    rec = config_record(TEST_BOARD)
    check(rec.hex() == "fe010203007a12000800c0000044aa201d1e1f051c3eff00", "test board record %s" % rec.hex())
    check(config_record("dm1701")[2:4] == b"\x01\x00" and config_record("dm1701")[16:24] == bytes(
        [0x13, 0x14, 0x15, 5, 0x37, 0xFF, 0xFF, 0]), "dm1701 board record: SPI1 mode 0, PB3-PB5, PD7")
    real = [bid for bid in BOARDS if bid != TEST_BOARD]
    check(len({BOARDS[bid]["id"] for bid in real}) == len(real) and
          all(config_record(bid)[0] == BOARDS[bid]["id"] for bid in real),
          "every board's record starts with its own stub id %s" % sorted((bid, BOARDS[bid]["id"]) for bid in real))
    f = stock(11)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.write_image(TEST_BOARD, [(APP_BASE, app(5000, 3))], backup_dir=tmp)
        check(False, "spi2test: no write before the board record")
    except F4Error:
        check(fake.erased == [] and fake.configs == 0, "spi2test: refused by the tool before anything is sent")
    r.configure(TEST_BOARD)
    check(r.board == TEST_BOARD and r.record == rec and fake.record == rec, "spi2test: configured, the echo checked")
    segs = [(APP_BASE, app(400000, 7))]
    nw, nk = r.write_image(TEST_BOARD, segs, backup_dir=tmp)
    check(sorted(fake.erased) == [3, 4, 5, 6, 7] and bytes(fake.flash[0x80000:]) == bytes(f[0x80000:]),
          "spi2test: sectors 3-7 written, the codec sectors 8-11 untouched %s" % fake.erased)
    check(bytes(fake.flash[0xC000:0xC000 + 400000]) == segs[0][1], "spi2test: application in place")
    try:
        r.write_image("dm1701", segs, backup_dir=tmp)
        check(False, "a stub configured for the SPI2 test board is not used for a DM-1701 image")
    except F4Error:
        pass
    r.configure(TEST_BOARD)
    check(fake.configs == 1, "the same record twice is sent once")
    r2 = F4Radio(fake, log=quiet)          # --attach without --board: the stub's own record says which radio
    r2.hello()
    check(r2.board == TEST_BOARD, "attach: the configured board read back from HELLO")
    mp = os.path.join(tmp, "rfoundry_spi2test_codec.bin")
    merged = bytearray(b"\xff" * (0x100000 - 0xC000))
    merged[:400000] = app(400000, 7)
    merged[0xE0000 - 0xC000:0xE0000 - 0xC000 + 0x9000] = app(0x9000, 5)
    open(mp, "wb").write(merged)
    check([(a, len(d)) for a, d in load_segments(mp, TEST_BOARD)] == [(APP_BASE, 400000), (0x08080000, 0x80000)],
          "spi2test: a merged image splits at sector 8")

    # A stub that answers CONFIG but doesn't echo the record: nothing written
    fake = FakeF4(stock(11))
    fake.handle_orig = fake.handle
    fake.handle = lambda op, p: (fake.frame(OK) if op == OP_CONFIG else fake.handle_orig(op, p))
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.configure(TEST_BOARD)
        check(False, "a missing echo is refused")
    except F4Error:
        check(r.board is None, "a missing echo leaves the tool unconfigured")

    # A protocol 2 stub (attached from an older run): the wiring built in, so not the SPI2 test board
    fake = FakeF4(stock(11), version=2)
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.configure(TEST_BOARD)
        check(False, "a protocol 2 stub is refused for the SPI2 test board")
    except F4Error:
        pass
    r.configure("md380")
    nw, _ = r.write_image("md380", [(APP_BASE, app(5000, 3))], backup_dir=tmp)
    check(nw == 5 and fake.configs == 0, "a protocol 2 stub still installs on the MD-380 (%d)" % nw)
    # Protocol 3 (the image header and the IWDG feed, no CONFIG yet) is treated the same way
    r = F4Radio(FakeF4(stock(11), version=3), log=quiet)
    r.hello()
    try:
        r.configure(TEST_BOARD)
        check(False, "a protocol 3 stub is refused for the SPI2 test board")
    except F4Error:
        pass
    del BOARDS[TEST_BOARD]

    # A sector shared with other data (no radio has one since the codec moved; the rule stays): the bytes after the
    # owned range are read back and kept, and the sector saved first
    BOARDS["shared"] = dict(BOARDS["md380"], id=99, name="test", names=(), owned=[(0x0800C000, 0x08050C44)], codec=None)
    try:
        f = stock(13)
        fake = FakeF4(f)
        r = F4Radio(fake, log=quiet)
        r.hello()
        r.configure("shared")
        segs = [(APP_BASE, app(0x08050C44 - APP_BASE - 1000, 17))]
        r.write_image("shared", segs, backup_dir=tmp)
        fl = fake.flash
        check(sorted(fake.erased) == [3, 4, 5, 6], "shared: sectors 3-6 %s" % fake.erased)
        check(bytes(fl[0x50C44:]) == bytes(f[0x50C44:]), "shared: the bytes after the owned range kept")
        check(bytes(fl[0xC000:0xC000 + len(segs[0][1])]) == segs[0][1], "shared: application in place")
        saved = os.path.join(tmp, "shared_sector6_0x08040000.bin")
        check(os.path.exists(saved) and open(saved, "rb").read() == bytes(f[0x40000:0x60000]), "shared: sector 6 saved first")
    finally:
        del BOARDS["shared"]

    # Reads check every block's CRC
    data = r.read_flash(0x08040000, 0x20000)
    check(data == bytes(fl[0x40000:0x60000]), "read_flash")
    check(r.verify_file(bytes(fl[:0x20000])) == [], "verify_file")
    check(board_for("DM-1701") == "dm1701" and board_for("MD-380 VHF") == "md380" and board_for("DM-32UV") is None,
          "board names")
    check(inside_git_tree(os.path.abspath(__file__)), "dumps are refused inside this git tree")

    # The SPI flash: a backup read back, a changed copy written (only its changed sectors), and a stub without one
    spi = bytearray((i * 13 + (i >> 12)) & 0xFF for i in range(0x100000))
    fake = FakeF4(stock(5), spi=spi)
    r = F4Radio(fake, log=quiet)
    r.hello()
    r.configure("dm1701")
    check(r.spi_size == 0x100000 and r.spi_id == "ef4018", "hello: SPI flash size and ID")
    path = os.path.join(tmp, "spi.bin")
    r.dump(path, 0, mem=MEM_SPI)
    check(open(path, "rb").read() == bytes(spi), "SPI flash dump")
    new = bytearray(spi)
    new[0x3000:0x3010] = b"\x00" * 16
    new[0x80000:0x81000] = b"\xff" * BLOCK
    fake.spi_erases = 0
    nw, nk = r.spi_write(bytes(new), 0)
    check(bytes(fake.spi) == bytes(new) and (nw, nk) == (2, 254) and fake.spi_erases == 2,
          "SPI write: only the 2 changed sectors (%d, %d)" % (nw, nk))
    check(r.verify_file(bytes(new), 0, MEM_SPI) == [], "SPI verify")
    fake = FakeF4(stock(5))
    r = F4Radio(fake, log=quiet)
    r.hello()
    r.configure("dm1701")
    try:
        r.spi_write(b"\xff" * BLOCK)
        check(False, "no SPI flash: refused")
    except F4Error:
        pass

    # Protected regions (board.toml [[flash.protect]]): the DM-1701's security registers 1-3 are its factory data. A
    # write to a differing register is refused before anything is sent; an SPI flash restore keeps the radio's bytes
    # in a protected range, writing the rest; an install never erases a protected sector, and the primitives check too
    fake = FakeF4(stock(5), spi=spi)
    r = F4Radio(fake, log=quiet)
    r.hello()
    r.configure("dm1701")
    sr2, regs = r.sreg_read_all()
    for reg in (1, 3):
        new = bytearray(regs)
        new[(reg - 1) * 256] ^= 0xFF
        try:
            r.sreg_write(bytes(new), tmp)
            check(False, "a protected security register %d: refused" % reg)
        except F4Error as e:
            check("protected region" in str(e) and fake.sreg_writes == [], "a protected security register %d: refused, "
                  "nothing sent (%s)" % (reg, e))
    keep = BOARDS["dm1701"]["protect"]
    BOARDS["dm1701"]["protect"] = keep + [{"mem": "data", "start": 0x2000, "end": 0x4000, "what": "test (x.md 1)"},
                                          {"mem": "progmem", "start": 0x60000, "end": 0x80000, "what": "test (x.md 1)"}]
    try:
        before = bytes(fake.spi)
        new = bytearray(before)
        new[0x1000:0x5000] = b"\x5a" * 0x4000
        fake.spi_erases = 0
        nw, nk = r.spi_write(bytes(new), 0)
        check(bytes(fake.spi[0x2000:0x4000]) == before[0x2000:0x4000] and bytes(fake.spi[0x1000:0x2000]) == new[0x1000:0x2000]
              and bytes(fake.spi[0x4000:0x5000]) == new[0x4000:0x5000] and (nw, fake.spi_erases) == (2, 2),
              "SPI restore: the protected sectors carved out, the rest written (%d, %d)" % (nw, nk))
        for what, call in (("an SPI sector erase", lambda: r.spi_erase(0x3000)),
                           ("an internal sector erase", lambda: r.erase(7)),
                           ("an internal program", lambda: r.program(0x08060000, b"\0" * 4))):
            try:
                call()
                check(False, "%s in a protected region: refused" % what)
            except F4Error as e:
                check("protected region" in str(e), "%s in a protected region: refused" % what)
        erased = list(fake.erased)
        try:
            r.write_image("dm1701", [(APP_BASE, app(0x80000 - 0xC000, 23))], backup_dir=tmp)
            check(False, "an install over a protected sector: refused")
        except F4Error as e:
            check("protected region" in str(e) and fake.erased == erased, "an install over a protected sector: "
                  "refused before the first erase")
    finally:
        BOARDS["dm1701"]["protect"] = keep

    # The security registers: read with CRC, written only where they differ (the old content saved first), a locked
    # register refused by the tool before anything is sent and by the stub when asked anyway. On a board without
    # protected registers (the DM-1701's own are protected)
    BOARDS["dm1701"]["protect"] = []
    fake = FakeF4(stock(5), spi=spi)
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.sreg_write(b"\xff" * 768, tmp)
        check(False, "security registers: no write before the board record")
    except F4Error:
        check(fake.sreg_writes == [], "security registers: no write before the board record")
    r.configure("dm1701")
    check(r.sr2() == 0x02 and r.sreg_locks(r.sr2()) == [False] * 3, "security registers: status register 2, no locks")
    sr2, regs = r.sreg_read_all()
    check(regs == b"".join(bytes(x) for x in fake.sregs) and len(regs) == 768, "security registers read")
    fake.crc_flips = 1
    check(r.sreg_read(2)[1] == bytes(fake.sregs[1]), "security register read again after a CRC mismatch")
    new = bytearray(regs)
    new[256 + 0x10] ^= 0xFF
    check(r.sreg_write(bytes(new), tmp) == [2] and fake.sreg_writes == [2] and bytes(fake.sregs[1]) == new[256:512],
          "security registers: only register 2 written %s" % fake.sreg_writes)
    saved = [f for f in os.listdir(tmp) if f.startswith("dm1701_secreg_")]
    check(len(saved) == 1 and open(os.path.join(tmp, saved[0]), "rb").read() == regs, "the old registers saved first")
    check(r.sreg_write(bytes(new), tmp) == [] and fake.sreg_writes == [2], "the same registers again: nothing written")
    fake.sr2 |= SR2_LB1 << 2                # LB3: register 3 locked for good
    check(r.sreg_locks(r.sr2()) == [False, False, True], "LB3 read as register 3 locked")
    new2 = bytearray(new)
    new2[0] ^= 0xFF
    new2[512] ^= 0xFF
    try:
        r.sreg_write(bytes(new2), tmp)
        check(False, "a locked register to write: refused")
    except F4Error:
        check(fake.sreg_writes == [2] and bytes(fake.sregs[0]) == new[:256], "a locked register: refused, nothing sent")
    try:
        r.request(OP_SWRITE, bytes([3]) + MAGIC.to_bytes(4, "big") + bytes(256), retries=0)
        check(False, "the stub refuses a locked register")
    except stubtool.DumpError as e:
        check("status 0x0F" in str(e) and bytes(fake.sregs[2]) == new[512:], "the stub refuses a locked register")
    check(r.sreg_write(bytes(new2[:512]) + bytes(new[512:]), tmp) == [1], "an unlocked register still written")
    for bad in (b"\xff" * 767, b"\xff" * 1024):
        try:
            r.sreg_write(bad, tmp)
            check(False, "security registers: %d bytes refused" % len(bad))
        except F4Error:
            pass
    fake.jedec = bytes([0xC2, 0x20, 0x18])  # another maker: its 0x35 / 0x48 mean other things
    r.hello()
    try:
        r.sr2()
        check(False, "a non-Winbond SPI flash: refused")
    except F4Error:
        check(True, "")
    r = F4Radio(FakeF4(stock(5), spi=spi, version=4), log=quiet)
    r.hello()
    r.configure("dm1701")
    try:
        r.sreg_read_all()
        check(False, "a protocol 4 stub has no security register ops")
    except F4Error:
        pass
    BOARDS["dm1701"]["protect"] = keep
    import contextlib
    import io
    sp = os.path.join(tmp, "secreg.bin")
    open(sp, "wb").write(bytes(new))
    for args, what in ((["X", "--write-secreg", sp], "no --confirm-secreg"),
                       (["X", "--write-secreg", pp, "--confirm-secreg"], "a file that is not 768 bytes"),
                       (["X", "--board", sorted(stubtool.C7_BOARDS)[0], "--secreg", sp], "an HR_C7000 board")):
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                stubtool.main(args)
            check(False, "stubtool refuses --write-secreg with %s" % what)
        except SystemExit as e:
            check(e.code == 2, "stubtool refuses %s before opening the port" % what)

    check(len(stub_image()) > 4096 and stub_image()[:4] != b"\xff" * 4, "the embedded stub decodes and checks")

    # Entering the stub from a running RadioFoundry: the debug link reports the SRAM base and the 8 KB staging buffer
    # (docs/spec/debuglink.md 3.18). The HR_C7000 image check must not reject the STM32F4 stub, and the reverse.
    top = stub_stack_top(stub_image())
    check(top == RUN_ADDR + SRAM_SIZE, "the embedded stub's entry code loads the stack top 0x%X" % (top or 0))
    rfoundry = stubtool.FakeRFoundry((RUN_ADDR, 8192))
    r = F4Radio(rfoundry, log=quiet)
    r.attach = lambda: None
    try:
        r.rfoundry_enter(stub_image(), RUN_ADDR, board="dm1701")
    except stubtool.DumpError as e:
        check(False, "rfoundry_enter refused the embedded STM32F4 stub: %s" % e)
    check(rfoundry.entered and bytes(rfoundry.mem[:len(stub_image())]) == stub_image(), "rfoundry_enter sends the embedded STM32F4 stub")
    c7 = stubtool.rfoundry_stubs()[0x17000][1]
    noise = bytes((i * 37 + 11) & 0xFF for i in range(4096))
    fake_top = stub_image()[:8] + (RUN_ADDR + 0x100).to_bytes(4, "little") + stub_image()[12:]
    for what, img, span in (("an HR_C7000 stub", c7, (RUN_ADDR, 8192)), ("a file that is not a stub", noise, (RUN_ADDR, 8192)),
                            ("the stub on a span that is not the SRAM base", stub_image(), (0x2001C000, 8192)),
                            ("a stub whose stack is inside its own image", fake_top, (RUN_ADDR, 8192))):
        rfoundry = stubtool.FakeRFoundry(span)
        r = F4Radio(rfoundry, log=quiet)
        r.attach = lambda: None
        try:
            r.rfoundry_enter(img, RUN_ADDR, board="dm1701")
            check(False, "STM32F4 rfoundry_enter refuses %s" % what)
        except stubtool.DumpError:
            check(not rfoundry.entered and rfoundry.total is None, "STM32F4 rfoundry_enter refuses %s before BEGIN" % what)
    rfoundry = stubtool.FakeRFoundry((0x17000, 0x18000))
    r = stubtool.Radio(rfoundry, log=quiet)
    r.attach = lambda: None
    try:
        r.rfoundry_enter(stub_image())
        check(False, "the HR_C7000 path refuses the STM32F4 stub")
    except stubtool.DumpError:
        check(rfoundry.total is None, "the HR_C7000 path refuses the STM32F4 stub before BEGIN")

    # Finding the stub's port again: it came back as ttyACM1 while ttyACM0 is still held, or not at all
    ID = STUB_USB_ID
    old = PortInfo("/dev/ttyACM0", ID[0], ID[1], "00000000010C", "1-1.3")
    new1 = PortInfo("/dev/ttyACM1", ID[0], ID[1], "00000000010C", "1-1.3")
    other = PortInfo("/dev/ttyACM2", ID[0], ID[1], "00000000010C", "1-1.5")
    ftdi = PortInfo("/dev/ttyUSB0", 0x0403, 0x6001, "A50285BI", "1-1.4")
    check(pick_port([new1, ftdi], old) is new1, "port: the same USB ID, the only one")
    check(pick_port([new1, other], old) is new1, "port: the same USB location wins over a second radio")
    check(pick_port([other, ftdi], old) is None, "port: a look-alike on another USB port is a different radio")
    check(pick_port([other, ftdi], PortInfo("/dev/ttyACM0", ID[0], ID[1], None, None)) is other,
          "port: the lone look-alike when the location is unknown")
    check(pick_port([new1, other], None) is None, "port: two look-alikes and no identity: none")
    check(pick_port([other, PortInfo("/dev/ttyACM3", ID[0], ID[1], "00000000010C", "1-1.6")],
                    PortInfo("/dev/ttyACM0", ID[0], ID[1], "00000000010C", "1-1.3")) is None,
          "port: two other radios with the same serial number are not guessed")
    check(pick_port([old, ftdi], old) is None, "port: the original itself is not a look-alike")

    class Clock:
        t = 0.0

        def __call__(self):
            return Clock.t

    class FakeSerial:
        opened = []

        @staticmethod
        def serial_for_url(path, baud, timeout):
            FakeSerial.opened.append(path)

    clock = Clock()
    pf = PortFollower(FakeSerial, "/dev/ttyACM0", ports=lambda: [old, ftdi], clock=clock, log=quiet)
    check(pf.orig is old, "follower: knows the port it started on")
    pf.ports = lambda: [new1, ftdi]
    for _ in range(4):
        pf()
        Clock.t += 1.0
    check(FakeSerial.opened == ["/dev/ttyACM0"] * 4, "follower: the original name only for the first 10 s")
    Clock.t = 10.0
    FakeSerial.opened.clear()
    for _ in range(4):
        pf()
    check("/dev/ttyACM1" in FakeSerial.opened and "/dev/ttyACM0" in FakeSerial.opened,
          "follower: after 10 s it alternates between the original and the look-alike (%s)" % FakeSerial.opened)

    tmpdir.cleanup()
    print("f4stub self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                  "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if len(sys.argv) == 3 and sys.argv[1] == "--embed":
        embed(sys.argv[2])
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == "--check-embedded":
        same = open(sys.argv[2], "rb").read() == stub_image()
        print("the embedded STM32F4 stub %s %s" % ("matches" if same else "differs from", sys.argv[2]))
        sys.exit(0 if same else 1)
    print(__doc__)
    sys.exit(2)
