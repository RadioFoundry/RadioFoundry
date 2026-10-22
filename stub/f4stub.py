#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""f4stub: the host side of the STM32F4 flash stub (every STM32F4 board). docs/spec/stub-stm32f4.md.

The stub (stub/stm32f4, built with `make -C stub/stm32f4`) is loaded by a running ODR built with CONFIG_STUB_ENTRY=y
through the debug link's STUB command, over the radio's USB serial port. It then answers on the same port (or, if it
had to enumerate again, on a port with the same serial number). stubtool.py hands STM32 radios to this module:

    stubtool.py PORT --odr-enter --write-app build/dm1701/odr_dm1701.bin   install a build
    stubtool.py PORT --odr-enter --write-app build/dm1701/odr_dm1701_codec.bin   a build merged with the codec
    stubtool.py PORT --odr-enter --progmem flash.bin                      back up the whole internal flash (1 MB)
    stubtool.py PORT --odr-enter --verify-progmem flash.bin               compare by CRC
    stubtool.py PORT --odr-enter --q128 spi.bin                           back up the external SPI flash (16 MB)
    stubtool.py PORT --odr-enter --write-q128 spi.bin                     put it back (changed 4 KB sectors only)
    stubtool.py PORT --attach --board md380 --boot reset                  restart into the firmware
    f4stub.py --self-test                                                 against a simulated radio, no hardware
    f4stub.py --embed build/stub-stm32f4/f4stub.bin                       put a new stub build into this file
    f4stub.py --check-embedded build/stub-stm32f4/f4stub.bin              exit 1 when the build differs from it

The tool sends the stub embedded below (checked by its SHA-256), so the host tools and the web flasher send the same
bytes; `--odr-stub FILE` sends another build instead. After a change to stub/stm32f4, rebuild and run --embed; CI
fails while the embedded copy and a fresh build differ.

Framing and the debug link loader are stubtool.Radio's; what differs is here: the ops (ERASE / PROGRAM / RESET are
new, SUMS and READ take a flash address), 4 KB blocks inside coarse erase sectors (16 to 128 KB), the sector 3
interlock and the update order that goes with it, and the bytes a sector shares with other data, which are read back
and kept (no radio has such a sector since the codec moved).

One stub image serves every radio. The board facts (SPI flash wiring, crystal, bootloader end) come from the board
registry (board.toml [flash.stub], pins.h, board.mk; tools/boards.py) into BOARDS below and go to the stub as a CONFIG
record (config_record) right after HELLO; the stub writes nothing until it has one, and the
tool checks its echo in the next HELLO. The board comes from the running ODR's VERSION reply, or from --board.
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
from stubtool import odrboards  # noqa: E402  (the board registry, tools/odrflash/odrflash/registry.json)

# --- F4STUB begin (f4stub.py --embed rewrites everything up to the end marker)
F4STUB_SHA256 = "793a115995dbc0ef7b183e7b344d01f3fc10a17e85fef4aaba60929f56c33467"
F4STUB_B64 = (
    "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"
)
# --- F4STUB end

MAGIC = 0x46345342                     # 'F4SB': ERASE and RESET (stub/stm32f4/f4stub.h)
OP_HELLO, OP_PEEK, OP_SUMS, OP_READ, OP_RESET, OP_ERASE, OP_PROGRAM = 0x01, 0x02, 0x07, 0x09, 0x0D, 0x10, 0x11
OP_XERASE, OP_XPROGRAM, OP_CONFIG = 0x12, 0x13, 0x14
MEM_INTERNAL, MEM_SPI = 0, 1           # the memory byte SUMS and READ end with (stub version 2)
OK, BADSUM, BADOP, BADARG, TOOLONG, REFUSED, FLASHERR, NOCONFIG = 0x05, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11
VERSION = 4                            # the embedded stub's protocol: 3 added the image header and the IWDG feed,
                                       # 4 CONFIG and HELLO's record echo
CONFIG_VERSION = 4                     # the first protocol with CONFIG
HELLO_CONFIGURED = 0x80                # HELLO flags: a board record is in force
HELLO_LEN = 26                         # HELLO v4 without the record echo
REC_LEN, REC_LAYOUT = 24, 1            # the board record (stub/stm32f4/f4stub.h)
BLOCK = 4096
FLASH_BASE = 0x08000000
APP_BASE = 0x0800C000                  # sector 3: the bootloader starts the application here; 0-2 are never written
SUM_MAX = 800                          # SUMS records per request (the stub allows 820)
READ_PACK = 16                         # blocks per READ request
ERASE_TIMEOUT = 6.0                    # 128 KB: 2 s worst case in the data sheet, the stub gives up at 4 s
RUN_ADDR = 0x20000000                  # the stub's link address, which ODR reports as STUB INFO's load_addr
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
                 "boot_end": b.stub["boot_end"], "spi": b.stub["spi"]}
          for b in odrboards.BOARDS.values() if b.stub and b.stub["tool"] == "f4stub"}
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


# A profile with other wiring than any board's, for the tests here, in tests/host/f4stub_wire_check.py and in the
# web flasher's (site/tests/fixtures.py): the SPI flash on SPI2 in mode 3 with chip select PB12, a second chip select
# on that bus (PD14) held high, and the MD-380's application sectors 3-7 with the codec in 8-11
TEST_BOARD = "spi2test"


def test_board():
    return {"name": "SPI2 test board", "names": (), "owned": [(APP_BASE, 0x08080000)], "codec": (0x08080000, 0x08100000),
            "id": 3, "hse": 8000000, "boot_end": APP_BASE,
            "spi": {"bus": 2, "mode": 3, "hz": 4500000, "sck": "PB13", "miso": "PB14", "mosi": "PB15", "af": 5,
                    "cs": "PB12", "idle": ["PD14"]}}


def config_record(board_id):
    """The board record of one of BOARDS"""
    return record(BOARDS[board_id])


def board_by_id(num):
    return next((bid for bid, b in BOARDS.items() if b["id"] == num), None)


def pin_byte(name):
    """'PB12' -> 0x1C (port << 4 | pin), None -> 0xFF"""
    if name is None:
        return 0xFF
    if len(name) < 3 or name[0] != "P" or not "A" <= name[1] <= "I" or not 0 <= int(name[2:]) <= 15:
        raise ValueError("pin %r" % name)
    return (ord(name[1]) - ord("A")) << 4 | int(name[2:])


def config_record(board_id):
    """The 24-byte board record the stub's CONFIG takes (stub/stm32f4/f4stub.h)"""
    b = BOARDS[board_id]
    spi = b["spi"]
    idle = list(spi["idle"]) + [None] * (2 - len(spi["idle"])) if spi else [None, None]
    rec = bytes([b["id"], REC_LAYOUT, spi["bus"] if spi else 0, spi["mode"] if spi else 0])
    rec += b["hse"].to_bytes(4, "big") + APP_BASE.to_bytes(4, "big") + (spi["hz"] if spi else 0).to_bytes(4, "big")
    rec += bytes([pin_byte(spi and spi["sck"]), pin_byte(spi and spi["miso"]), pin_byte(spi and spi["mosi"]),
                  spi["af"] if spi else 0, pin_byte(spi and spi["cs"]), pin_byte(idle[0]), pin_byte(idle[1]), 0])
    assert len(rec) == REC_LEN
    return rec


def board_by_id(num):
    return next((bid for bid, b in BOARDS.items() if b["id"] == num), None)


class Chip:
    """One stub target, as the host sees it: HELLO's name, the op magic, the flash map and program unit, where the
    stub runs, its embedded image and its radios' profiles. F4 (below) is the STM32F4 stub; stub/k22stub.py has the
    Kinetis K22F one. F4Radio, load_segments, run and the fake stub take one."""
    title = "STM32F4"
    name = b"F4STUB"
    magic = MAGIC
    flash_base = FLASH_BASE
    flash_kb = 1024                    # every radio's part
    prog_unit = 4
    app_base = APP_BASE
    run_addr = RUN_ADDR
    sram_size = SRAM_SIZE              # the span the stub's stack top must be in, from run_addr
    erase_timeout = ERASE_TIMEOUT
    min_version = 2                    # protocol 2 stubs (no CONFIG) are still served

    @property
    def boards(self):
        return BOARDS

    def sectors(self, flash_kb):
        return f405_sectors(flash_kb)

    def config_record(self, board_id):
        return config_record(board_id)

    def parse_record(self, rec):
        """{"app_base", "app_end", "xfloor"} from a board record, or None when the stub would refuse it"""
        if len(rec) != REC_LEN or rec[0] == 0 or rec[1] != REC_LAYOUT:
            return None
        return {"app_base": int.from_bytes(rec[8:12], "big"), "app_end": None, "xfloor": 0}

    def stub_image(self):
        return stub_image()

    def sp_ok(self, sp):
        """A vector table's stack pointer the radio's bootloader accepts (in SRAM)"""
        return sp & 0x2FFE0000 == 0x20000000

    def board_for(self, name):
        return board_for(name)

    def board_by_id(self, num):
        return next((bid for bid, b in self.boards.items() if b["id"] == num), None)


F4 = Chip()


class F4Error(stubtool.DumpError):
    pass


STUB_USB_ID = (0x1FC9, 0x0094)	# ODR's and the stub's USB identity (same serial number too)
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
    of the port ODR was on (None if it's unknown). The radio keeps its USB serial number, but a re-enumeration can give
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


def load_segments(path, board_id, chip=None):
    """[(addr, bytes)] for a build: the application .bin at the application base (0x0800C000 on the STM32F4), and for
    an image merged with the codec (odr_<board>_codec.bin) the codec area as a second segment. The merged image runs
    to the codec area's end, or to the profile's "codec_image_end" (the GD-77's ends with the descriptor), the rest of
    the area then counted erased."""
    chip = chip or F4
    app_base = chip.app_base
    data = open(path, "rb").read()
    prof = chip.boards[board_id]
    codec = prof["codec"]
    if codec is None or app_base + len(data) <= codec[0]:
        return [(app_base, data)]
    end = prof.get("codec_image_end", codec[1])
    if app_base + len(data) != end:
        raise F4Error("%s runs to 0x%08X: a build merged with the codec runs to 0x%08X, a plain build ends before "
                      "0x%08X" % (path, app_base + len(data), end, codec[0]))
    cut = codec[0] - app_base
    return [(app_base, data[:cut].rstrip(b"\xff")), (codec[0], data[cut:] + b"\xff" * (codec[1] - end))]


class F4Radio(stubtool.Radio):
    """A radio running the STM32F4 stub, or another target of the shared engine (chip: a Chip, F4 by default). port: a
    pyserial-like object; reopen: a callable that opens the port again (after the stub enumerated afresh) and returns
    it, or None."""

    def __init__(self, port, log=print, retries=3, reopen=None, chip=None):
        super().__init__(port, log=log, retries=retries)
        self.chip = chip or F4
        self.reopen = reopen
        self.flash_kb = 0
        self.rdp = 0
        self.adopted = False
        self.version = 0           # from HELLO
        self.board = None          # the BOARDS key the stub is configured for (version 4), or None
        self.record = b""          # the board record HELLO echoes

    def check_stub(self, image, load, size):
        check_stub_fits(image, load, size, self.chip)

    # ---- talking to the stub
    def hello(self, timeout=0.5):
        """HELLO; timeout: how long the reply may take (a simulated stub just started may need longer)"""
        r = self.request(OP_HELLO, retries=0, timeout=timeout)
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
        what stubtool.Radio.odr_enter calls once the stub answers (board_id None there unless run() passed one). A stub
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
        """After ENTER: the stub either adopted ODR's USB device (same port, answers at once) or enumerated again (the
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
                                      "(ODR starts again) and check the cable" % PORT_WAIT_S)
                    time.sleep(0.3)
        self.log("STM32F4 stub v%d: %d KB flash, RDP byte 0x%02X, %s, USB %s" % (
            self.version, self.flash_kb, self.rdp,
            "configured for the %s" % BOARDS[self.board]["name"] if self.board else
            ("SPI flash %d KB (JEDEC %s)" % (self.spi_size // 1024, self.spi_id) if self.spi_size else
             "not configured yet" if self.version >= CONFIG_VERSION else "no SPI flash"),
            "adopted from ODR" if self.adopted else "enumerated again"))

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

    def read_range(self, addr, n):
        """n bytes of internal flash from addr, through the whole 4 KB blocks around them (a 2 KB sector)."""
        base = self.chip.flash_base
        lo = base + (addr - base) // BLOCK * BLOCK
        hi = base + (addr + n - base + BLOCK - 1) // BLOCK * BLOCK
        return self.read_flash(lo, hi - lo)[addr - lo:addr - lo + n]

    def erase(self, sector):
        self.request(OP_ERASE, bytes([sector]) + self.chip.magic.to_bytes(4, "big"), retries=0,
                     timeout=self.chip.erase_timeout)

    def program(self, addr, data):
        """Program data (whole program units: 4 bytes on the STM32F4) at addr in pieces of up to 4 KB, skipping
        all-0xFF pieces (erased)."""
        unit = self.chip.prog_unit
        if addr % unit or len(data) % unit:
            raise F4Error("program 0x%08X+%d: whole %d-byte units only" % (addr, len(data), unit))
        for off in range(0, len(data), BLOCK):
            piece = data[off:off + BLOCK]
            if piece != b"\xff" * len(piece):
                self.request(OP_PROGRAM, (addr + off).to_bytes(4, "big") + piece, retries=0, timeout=2.0)

    def reset_chip(self):
        """System reset: the bootloader runs, then the application (or stays in DFU if sector 3 is blank)."""
        self.request(OP_RESET, self.chip.magic.to_bytes(4, "big"), retries=0)

    # ---- whole-image work
    def check_board(self):
        if self.flash_kb != 1024:
            raise F4Error("the chip reports %d KB of flash; every radio in BOARDS has 1024" % self.flash_kb)

    def plan(self, board_id, segments):
        """{sector index: new content} for every sector that holds firmware-owned flash, and which of those differ
        from the radio. Owned bytes come from the segments (0xFF where they have none); the rest is read back. A
        segment that covers the codec sectors whole (load_segments) makes them owned too."""
        prof = self.chip.boards[board_id]
        app_base = self.chip.app_base
        owned = list(prof["owned"])
        codec = prof["codec"]
        if codec and any(a == codec[0] and len(d) == codec[1] - codec[0] for a, d in segments):
            owned.append(codec)
        merged = []                     # adjacent ranges as one (the GD-77's codec starts inside a sector)
        for lo, hi in sorted(owned):
            if merged and lo <= merged[-1][1]:
                merged[-1] = (merged[-1][0], max(hi, merged[-1][1]))
            else:
                merged.append((lo, hi))
        owned = merged
        for addr, data in segments:
            if not any(lo <= addr and addr + len(data) <= hi for lo, hi in owned):
                raise F4Error("image data at 0x%08X..0x%08X is outside the flash the firmware owns on the %s (%s): a "
                              "wrong board?" % (addr, addr + len(data) - 1, prof["name"],
                              ", ".join("0x%08X..0x%08X" % (lo, hi - 1) for lo, hi in owned)))
        if not segments or segments[0][0] != app_base or len(segments[0][1]) < 8:
            raise F4Error("the image must start at 0x%08X with the vector table" % app_base)
        sp = int.from_bytes(segments[0][1][:4], "little")
        if not self.chip.sp_ok(sp):
            raise F4Error("the image's first word 0x%08X is not a RAM stack pointer: the bootloader would not start it" % sp)
        new, kept = {}, {}
        for idx, saddr, size in self.chip.sectors(self.flash_kb):
            parts = [(max(lo, saddr), min(hi, saddr + size)) for lo, hi in owned if lo < saddr + size and hi > saddr]
            if not parts:
                continue
            whole = parts == [(saddr, saddr + size)]
            buf = bytearray(b"\xff" * size) if whole else bytearray(self.read_range(saddr, size))
            if not whole:
                kept[idx] = bytes(buf)
            for lo, hi in parts:
                buf[lo - saddr:hi - saddr] = b"\xff" * (hi - lo)
            for addr, data in segments:
                lo, hi = max(addr, saddr), min(addr + len(data), saddr + size)
                if lo < hi:
                    buf[lo - saddr:hi - saddr] = data[lo - addr:hi - addr]
            new[idx] = bytes(buf)
        return new, self.mismatched(new), kept

    def write_image(self, board_id, segments, backup_dir=None, progress=None):
        """Install a build. Order (the stub's interlock enforces it): erase the commit sector (the one at the
        application base: sector 3 on the STM32F4) first, so the bootloader finds no application from here until the
        end; erase and program every other changed sector; program the commit sector except its first program unit;
        verify everything; program that unit (the commit); verify it. Returns (written, unchanged)."""
        self.check_board()
        self.need_board(board_id)
        new, changed, kept = self.plan(board_id, segments)
        if not changed:
            return 0, len(new)
        sec = self.sector_map()
        app_base, unit = self.chip.app_base, self.chip.prog_unit
        commit = next(i for i, (a, s) in sec.items() if a <= app_base < a + s)
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
        order = [commit] + [i for i in changed if i != commit]
        steps = len(order)
        self.erase(commit)
        for n, idx in enumerate(order[1:], 1):
            if progress:
                progress(n, steps)
            a, _ = sec[idx]
            self.erase(idx)
            self.program(a, new[idx])
        if progress:
            progress(steps, steps)
        self.program(app_base + unit, new[commit][unit:])        # the application base is a sector start
        expect = {i: new[i] for i in order}
        expect[commit] = b"\xff" * unit + new[commit][unit:]
        bad = self.mismatched(expect)
        if bad:
            raise F4Error("verify before the commit: sector(s) %s differ. The application is not committed: the radio "
                          "starts in its bootloader until an install finishes" % bad)
        self.program(app_base, new[commit][:unit])
        if self.mismatched({commit: new[commit]}):
            raise F4Error("the commit word did not verify")
        return len(order), len(new) - len(order)

    def verify_file(self, data, start=None, mem=MEM_INTERNAL):
        """Block addresses where the flash differs from data (a whole number of 4 KB blocks from start: the internal
        flash's base by default)."""
        if start is None:
            start = self.chip.flash_base
        sums = []
        for off in range(0, len(data) // BLOCK, 256):         # 1 MB per request: a slow SPI flash stays in its time-out
            sums += self.sums(start + off * BLOCK, min(256, len(data) // BLOCK - off), mem)
        return [start + i * BLOCK for i, (crc, _) in enumerate(sums) if zlib.crc32(data[i * BLOCK:(i + 1) * BLOCK]) != crc]

    def spi_erase(self, addr):
        self.request(OP_XERASE, addr.to_bytes(4, "big") + self.chip.magic.to_bytes(4, "big"), retries=0, timeout=2.0)

    def spi_write(self, data, start=0, progress=None):
        """Write data (whole 4 KB sectors) into the SPI flash from start: only the sectors whose CRC differs are erased
        and programmed (blank pieces skipped), each read back by the stub, then all checked again. Returns (written,
        unchanged)."""
        self.need_spi()
        self.need_board()
        if start % BLOCK or len(data) % BLOCK or start + len(data) > self.spi_size:
            raise F4Error("SPI flash writes are whole 4 KB sectors inside the chip's %d KB" % (self.spi_size // 1024))
        todo = self.verify_file(data, start, MEM_SPI)
        floor = self.chip.boards[self.board].get("spi_floor", 0) if self.board else 0
        low = [a for a in todo if a < floor]
        if low:
            raise F4Error("the SPI flash below 0x%06X is the radio's own data and is never written; %d sector(s) there "
                          "differ from the file, first at 0x%06X: nothing written" % (floor, len(low), low[0]))
        for n, a in enumerate(todo, 1):
            sec = data[a - start:a - start + BLOCK]
            self.spi_erase(a)
            for off in range(0, BLOCK, 1024):
                piece = sec[off:off + 1024]
                if piece != b"\xff" * len(piece):
                    self.request(OP_XPROGRAM, (a + off).to_bytes(4, "big") + piece, retries=0, timeout=2.0)
            if progress:
                progress(n, len(todo))
        bad = self.verify_file(data, start, MEM_SPI) if todo else []
        if bad:
            raise F4Error("SPI flash verify: %d sector(s) differ, first at 0x%06X" % (len(bad), bad[0]))
        return len(todo), len(data) // BLOCK - len(todo)

    def dump(self, path, start=None, end=None, progress=None, mem=MEM_INTERNAL):
        if start is None:
            start = 0 if mem == MEM_SPI else self.chip.flash_base
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


def board_from_version(radio, chip=None):
    """The board id from a running ODR's debug link VERSION reply (board=...), or None."""
    r = radio.dl_request(0x02, b"")
    if not r or r[0] != 0:
        return None
    for line in r[1:].decode("ascii", "replace").splitlines():
        if line.startswith("board="):
            return (chip or F4).board_for(line[6:].strip())
    return None


def stub_stack_top(image):
    """The initial stack pointer the image's entry code loads, or None if the image does not start with the shared
    engine's entry (stub/stm32f4/f4stub_main.c, stub/k22f/k22stub_main.c): `ldr rN, [pc, #imm]` and `mov sp, rN` with
    N a low register (r1 on the STM32F4, r3 on the K22F), the literal in the pool after the branch."""
    if len(image) < 8 or image[1] & 0xF8 != 0x48:
        return None
    rt = image[1] & 7
    if image[2:4] != bytes([0x85 | rt << 3, 0x46]):
        return None
    lit = 4 + image[0] * 4                     # Thumb LDR literal: Align(PC, 4) + imm8 * 4, PC = 4 here
    if lit + 4 > len(image):
        return None
    return int.from_bytes(image[lit:lit + 4], "little")


def check_stub_fits(image, load, size, chip=None):
    """Refuse an image that is not this chip's stub, or a span ODR reports that is not where the stub runs. ODR stages
    the image in an 8 KB buffer and copies it to its link address before the jump (docs/spec/debuglink.md 3.18), so
    the HR_C7000 rule (bss and stack inside the span) does not apply: after the jump the stub owns its SRAM."""
    chip = chip or F4
    top = stub_stack_top(image)
    if top is None or chip.name not in image:
        raise F4Error("this does not look like the %s stub image (no stub entry code at its start)" % chip.title)
    run, end = chip.run_addr, chip.run_addr + chip.sram_size
    if load != run:
        raise F4Error("ODR reports its stub span at 0x%X; the %s stub runs at 0x%X" % (load, chip.title, run))
    if not run + len(image) < top <= end:
        raise F4Error("this stub image's stack top 0x%X is outside the SRAM it runs in (0x%X-0x%X)" % (top, run, end))


# The image header (stub/stm32f4/f4stub_header.h, docs/spec/stub-stm32f4.md 3.1): 16 bytes at offset 0x10, little
# endian. ODR started with CONFIG_WATCHDOG enters only a stub whose header has the magic, a version of at least
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


def embed_into(path, target, tag):
    """Rewrite the stub embedded in target (a Python file, between its `# --- TAG begin` and `# --- TAG end` lines,
    as TAG_SHA256 and TAG_B64) from a build"""
    blob = open(path, "rb").read()
    b64 = base64.b64encode(zlib.compress(blob, 9)).decode()
    tool = os.path.basename(target)
    lines = ["# --- %s begin (%s --embed rewrites everything up to the end marker)" % (tag, tool),
             '%s_SHA256 = "%s"' % (tag, hashlib.sha256(blob).hexdigest()), "%s_B64 = (" % tag]
    lines += ['    "%s"' % b64[i:i + 104] for i in range(0, len(b64), 104)]
    lines += [")", "# --- %s end" % tag]
    text = open(target).read()
    a = text.index("# --- %s begin" % tag)
    b = text.index("# --- %s end" % tag) + len("# --- %s end" % tag)
    open(target, "w").write(text[:a] + "\n".join(lines) + text[b:])
    print("embedded %s: %d bytes, sha256 %s" % (path, len(blob), hashlib.sha256(blob).hexdigest()))


def embed(path):
    """Rewrite the embedded stub in this file from a build"""
    embed_into(path, os.path.abspath(__file__), "F4STUB")


def run(a, port, reopen, log=print, chip=None):
    """stubtool.py's branch for the shared engine's stubs (the STM32 radios; with chip=k22stub.K22 the GD-77): a is its
    parsed arguments, port the open serial port."""
    chip = chip or F4
    radio = F4Radio(port, log=log, retries=a.retries, reopen=reopen, chip=chip)
    board = a.board
    if a.odr_enter:
        found = board_from_version(radio, chip)
        if board is None:
            board = found
        elif found is not None and found != board:
            raise F4Error("the radio says it is a %s, not a %s" % (found, board))
        image = open(a.odr_stub, "rb").read() if a.odr_stub else stub_image()
        log(describe_stub(image))
        radio.odr_enter(image, RUN_ADDR, board=board)
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
        base = chip.flash_base
        radio.dump(a.progmem, base + lo // BLOCK * BLOCK, base + (hi + BLOCK - 1) // BLOCK * BLOCK)
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
    if a.boot:
        if a.boot != "reset":
            raise F4Error("on the %s radios --boot takes only reset" % chip.title)
        radio.reset_chip()
        log("reset sent: the bootloader starts the application (or stays in its update mode if none is committed)")
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
        return self.flash, addr - self.chip.flash_base

    def w(self, addr):
        o = addr - self.chip.flash_base
        return int.from_bytes(self.flash[o:o + 4], "little")

    def cfg(self):
        c = self.chip.parse_record(self.record)
        if c["app_end"] is None:
            c["app_end"] = self.chip.flash_base + self.flash_kb * 1024
        return c

    def commit_end(self):
        a = self.cfg()["app_base"]
        return next(sa + size for _, sa, size in self.chip.sectors(self.flash_kb) if sa <= a < sa + size)

    def handle(self, op, p):
        if op in (OP_ERASE, OP_PROGRAM, OP_XERASE, OP_XPROGRAM) and not self.record:
            return self.frame(NOCONFIG)
        if op == OP_HELLO:
            rec = self.record
            on = bool(rec) and self.version >= CONFIG_VERSION
            body = (b"F4STUB" + bytes([self.version]) + (4104).to_bytes(2, "big") + self.flash_kb.to_bytes(2, "big")
                    + bytes([0x55, 1 | (HELLO_CONFIGURED if on else 0)])
                    + (int.from_bytes(rec[8:12], "big") if rec else 0).to_bytes(4, "big")
                    + (len(self.spi) if self.spi and rec else 0).to_bytes(4, "big") + bytes([0xEF, 0x40, 0x18]))
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
            self.frame(OK, bytes(self.flash[a - chip.flash_base:a - chip.flash_base + n]))
        elif op == OP_ERASE:
            s, c = p[0], self.cfg()
            _, a, size = chip.sectors(self.flash_kb)[s]
            commit = a <= c["app_base"] < a + size
            if (int.from_bytes(p[1:5], "big") != chip.magic or a < c["app_base"] or a >= c["app_end"]
                    or (not commit and self.w(c["app_base"]) != 0xFFFFFFFF)):
                return self.frame(REFUSED)
            self.flash[a - chip.flash_base:a - chip.flash_base + size] = b"\xff" * size
            self.erased.append(s)
            self.frame(OK)
        elif op == OP_PROGRAM:
            a, data, c = int.from_bytes(p[:4], "big"), p[4:], self.cfg()
            if (a % chip.prog_unit or len(data) % chip.prog_unit or a < c["app_base"] or a + len(data) > c["app_end"]
                    or (a + len(data) > self.commit_end() and self.w(c["app_base"]) != 0xFFFFFFFF)):
                return self.frame(REFUSED)
            o = a - chip.flash_base
            if self.flash[o:o + len(data)] != b"\xff" * len(data):
                return self.frame(REFUSED)
            self.flash[o:o + len(data)] = data
            self.programs += 1
            self.frame(OK)
        elif op == OP_XERASE:
            a = int.from_bytes(p[:4], "big")
            if int.from_bytes(p[4:8], "big") != chip.magic or a < self.cfg()["xfloor"]:
                return self.frame(REFUSED)
            self.spi[a:a + BLOCK] = b"\xff" * BLOCK
            self.spi_erases = getattr(self, "spi_erases", 0) + 1
            self.frame(OK)
        elif op == OP_XPROGRAM:
            a, data = int.from_bytes(p[:4], "big"), p[4:]
            if a < self.cfg()["xfloor"] or self.spi[a:a + len(data)] != b"\xff" * len(data):
                return self.frame(REFUSED)
            self.spi[a:a + len(data)] = data
            self.frame(OK)
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
    # The image header: ODR with CONFIG_WATCHDOG enters only a stub whose header says it feeds the IWDG
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
    tmpdir = tempfile.TemporaryDirectory(prefix="odr-f4stub-")
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
    mp = os.path.join(tmp, "odr_dm1701_codec.bin")
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
    BOARDS[TEST_BOARD] = test_board()
    rec = config_record(TEST_BOARD)
    check(rec.hex() == "03010203007a12000800c0000044aa201d1e1f051c3eff00", "test board record %s" % rec.hex())
    check(config_record("dm1701")[2:4] == b"\x01\x00" and config_record("dm1701")[16:24] == bytes(
        [0x13, 0x14, 0x15, 5, 0x37, 0xFF, 0xFF, 0]), "dm1701 board record: SPI1 mode 0, PB3-PB5, PD7")
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
    mp = os.path.join(tmp, "odr_spi2test_codec.bin")
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

    check(len(stub_image()) > 4096 and stub_image()[:4] != b"\xff" * 4, "the embedded stub decodes and checks")

    # Entering the stub from a running ODR: the debug link reports the SRAM base and the 8 KB staging buffer
    # (docs/spec/debuglink.md 3.18). The HR_C7000 image check must not reject the STM32F4 stub, and the reverse.
    top = stub_stack_top(stub_image())
    check(top == RUN_ADDR + SRAM_SIZE, "the embedded stub's entry code loads the stack top 0x%X" % (top or 0))
    odr = stubtool.FakeODR((RUN_ADDR, 8192))
    r = F4Radio(odr, log=quiet)
    r.attach = lambda: None
    try:
        r.odr_enter(stub_image(), RUN_ADDR, board="dm1701")
    except stubtool.DumpError as e:
        check(False, "odr_enter refused the embedded STM32F4 stub: %s" % e)
    check(odr.entered and bytes(odr.mem[:len(stub_image())]) == stub_image(), "odr_enter sends the embedded STM32F4 stub")
    c7 = stubtool.odr_stubs()[0x17000][1]
    noise = bytes((i * 37 + 11) & 0xFF for i in range(4096))
    fake_top = stub_image()[:8] + (RUN_ADDR + 0x100).to_bytes(4, "little") + stub_image()[12:]
    for what, img, span in (("an HR_C7000 stub", c7, (RUN_ADDR, 8192)), ("a file that is not a stub", noise, (RUN_ADDR, 8192)),
                            ("the stub on a span that is not the SRAM base", stub_image(), (0x2001C000, 8192)),
                            ("a stub whose stack is inside its own image", fake_top, (RUN_ADDR, 8192))):
        odr = stubtool.FakeODR(span)
        r = F4Radio(odr, log=quiet)
        r.attach = lambda: None
        try:
            r.odr_enter(img, RUN_ADDR, board="dm1701")
            check(False, "STM32F4 odr_enter refuses %s" % what)
        except stubtool.DumpError:
            check(not odr.entered and odr.total is None, "STM32F4 odr_enter refuses %s before BEGIN" % what)
    odr = stubtool.FakeODR((0x17000, 0x18000))
    r = stubtool.Radio(odr, log=quiet)
    r.attach = lambda: None
    try:
        r.odr_enter(stub_image())
        check(False, "the HR_C7000 path refuses the STM32F4 stub")
    except stubtool.DumpError:
        check(odr.total is None, "the HR_C7000 path refuses the STM32F4 stub before BEGIN")

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
