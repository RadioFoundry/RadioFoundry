#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""f4stub: the host side of the STM32F4 flash stub (DM-1701, MD-380, MD-9600). docs/spec/stub-stm32f4.md.

The stub (stub/stm32f4, built with `make -C stub/stm32f4`) is loaded by a running ODR built with CONFIG_STUB_ENTRY=y
through the debug link's STUB command, over the radio's USB serial port. It then answers on the same port (or, if it
had to enumerate again, on a port with the same serial number). stubtool.py hands STM32 radios to this module:

    stubtool.py PORT --odr-enter --write-app build/dm1701/odr_dm1701.bin   install a build
    stubtool.py PORT --odr-enter --write-app build/dm1701/odr_dm1701_codec.bin   a build merged with the codec
    stubtool.py PORT --odr-enter --progmem flash.bin                      back up the whole internal flash (1 MB)
    stubtool.py PORT --odr-enter --verify-progmem flash.bin               compare by CRC
    stubtool.py PORT --odr-enter --q128 spi.bin                           back up the external SPI flash (16 MB)
    stubtool.py PORT --odr-enter --write-q128 spi.bin                     put it back (changed 4 KB sectors only)
    stubtool.py PORT --attach --board md9600 --boot reset                 restart into the firmware
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

One stub image serves every radio. The board facts (SPI flash wiring, crystal, bootloader end) are in BOARDS below and
go to the stub as a CONFIG record (config_record) right after HELLO; the stub writes nothing until it has one, and the
tool checks its echo in the next HELLO. The board comes from the running ODR's VERSION reply, or from --board.
"""
import base64
import hashlib
import os
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stubtool  # noqa: E402  (the shared framing, LZ4 decoder and debug link loader)

# --- F4STUB begin (f4stub.py --embed rewrites everything up to the end marker)
F4STUB_SHA256 = "3fb649554c4e608dd1c25aa0fd9a67952cc8511489584d451c86bc88aeb01a97"
F4STUB_B64 = (
    "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"
)
# --- F4STUB end

MAGIC = 0x46345342                     # 'F4SB': ERASE and RESET (stub/stm32f4/f4stub.h)
OP_HELLO, OP_PEEK, OP_SUMS, OP_READ, OP_RESET, OP_ERASE, OP_PROGRAM = 0x01, 0x02, 0x07, 0x09, 0x0D, 0x10, 0x11
OP_XERASE, OP_XPROGRAM, OP_CONFIG = 0x12, 0x13, 0x14
MEM_INTERNAL, MEM_SPI = 0, 1           # the memory byte SUMS and READ end with (stub version 2)
OK, BADSUM, BADOP, BADARG, TOOLONG, REFUSED, FLASHERR, NOCONFIG = 0x05, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11
VERSION = 3                            # the embedded stub's protocol: 3 added CONFIG and HELLO's record echo
HELLO_CONFIGURED = 0x80                # HELLO flags: a board record is in force
HELLO_LEN = 26                         # HELLO v3 without the record echo
REC_LEN, REC_LAYOUT = 24, 1            # the board record (stub/stm32f4/f4stub.h)
BLOCK = 4096
FLASH_BASE = 0x08000000
APP_BASE = 0x0800C000                  # sector 3: the bootloader starts the application here; 0-2 are never written
SUM_MAX = 800                          # SUMS records per request (the stub allows 820)
READ_PACK = 16                         # blocks per READ request
ERASE_TIMEOUT = 6.0                    # 128 KB: 2 s worst case in the data sheet, the stub gives up at 4 s
RUN_ADDR = 0x20000000                  # the stub's link address, which ODR reports as STUB INFO's load_addr
SRAM_SIZE = 128 * 1024                 # stub/stm32f4/f4stub.ld: .bss and the stack use all of it after the jump

# Flash the firmware owns on each radio (docs/spec/hardware-dm1701.md 1.4, hardware-md380.md 1.3, hardware-md9600.md
# 1.3). Everything else in a sector that is rewritten is read back from the radio and written again unchanged (no
# radio shares a sector with the codec any more, since it moved; the rule stays for one that does). Sectors with no
# owned byte are never erased, except "codec": the codec's own sectors (the DM-1701's 10 and 11, the MD-380's and the
# MD-9600's 8 to 11), written only from an image merged with the codec (docs/spec/codec-placement.md), which carries
# all of them.
#
# The rest of a profile is the stub's board record (config_record): "id" (echoed by HELLO), the crystal, the
# bootloader's end (APP_BASE on all three), and the SPI flash: SPI block, mode, the fastest clock, SCK / MISO / MOSI and
# their alternate function, the chip select, and other chip selects on that bus to hold high.
SPI1_PD7 = {"bus": 1, "mode": 0, "hz": 4500000, "sck": "PB3", "miso": "PB4", "mosi": "PB5", "af": 5, "cs": "PD7",
            "idle": ()}
BOARDS = {
    # hardware-dm1701.md 2, the W25Q128 on SPI1 (mode 0, 4.5 MHz as ODR runs it)
    "dm1701": {"id": 1, "name": "Baofeng DM-1701", "names": ("DM-1701", "dm1701"), "hse": 8000000, "spi": SPI1_PD7,
               "owned": [(0x0800C000, 0x080C0000)], "codec": (0x080C0000, 0x08100000)},
    # hardware-md380.md 12.1: the same wiring
    "md380": {"id": 2, "name": "TYT MD-380", "names": ("MD-380", "MD-380 VHF", "md380"), "hse": 8000000,
              "spi": SPI1_PD7, "owned": [(0x0800C000, 0x08080000)], "codec": (0x08080000, 0x08100000)},
    # hardware-md9600.md 12.1 (unverified on a radio): SPI2 shared with the LCD, mode 3 as ODR runs both; the LCD's chip
    # select PD14 held high. SPI1 / PD7 is the HR_C6000's voice port on this radio, which is why the wiring is a record
    "md9600": {"id": 3, "name": "TYT MD-9600", "names": ("MD-9600", "MD9600", "md9600", "RT90"), "hse": 8000000,
               "spi": {"bus": 2, "mode": 3, "hz": 4500000, "sck": "PB13", "miso": "PB14", "mosi": "PB15", "af": 5,
                       "cs": "PB12", "idle": ("PD14",)},
               "owned": [(0x0800C000, 0x08080000)], "codec": (0x08080000, 0x08100000)},
}


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


def load_segments(path, board_id):
    """[(addr, bytes)] for a build: the application .bin at 0x0800C000, and for an image merged with the codec
    (odr_<board>_codec.bin, which runs to the end of flash) the codec sectors as a second segment."""
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
        self.board = None          # the BOARDS key the stub is configured for (version 3), or None
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
        if self.version >= 3:
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
        (an --attach without --board), else stay unconfigured (reads of the internal flash only). A version 2 stub has
        no CONFIG: its wiring is fixed to the DM-1701 / MD-380 one, so it is refused for any other radio."""
        if board_id is not None and board_id not in BOARDS:
            raise F4Error("%s is not an STM32F4 radio this stub knows (%s)" % (board_id, ", ".join(BOARDS)))
        if self.version < 3:
            if board_id not in (None, "dm1701", "md380"):
                raise F4Error("this stub (protocol %d) drives the DM-1701 / MD-380 SPI flash wiring and can't serve "
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
             "not configured yet" if self.version >= 3 else "no SPI flash"),
            "adopted from ODR" if self.adopted else "enumerated again"))

    def peek(self, addr, n):
        return self.request(OP_PEEK, addr.to_bytes(4, "big") + n.to_bytes(2, "big"))

    @staticmethod
    def mem_byte(mem):
        return b"" if mem == MEM_INTERNAL else bytes([mem])

    def need_spi(self):
        if not self.spi_size:
            raise F4Error("the stub found no SPI flash (stub version %d%s)" % (
                self.version, ", not configured: pass --board" if self.version >= 3 and self.board is None else ""))

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

    def erase(self, sector):
        self.request(OP_ERASE, bytes([sector]) + MAGIC.to_bytes(4, "big"), retries=0, timeout=ERASE_TIMEOUT)

    def program(self, addr, data):
        """Program data (a multiple of 4 bytes) at addr in pieces of up to 4 KB, skipping all-0xFF pieces (erased)."""
        if addr % 4 or len(data) % 4:
            raise F4Error("program 0x%08X+%d: words only" % (addr, len(data)))
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
        self.request(OP_XERASE, addr.to_bytes(4, "big") + MAGIC.to_bytes(4, "big"), retries=0, timeout=2.0)

    def spi_write(self, data, start=0, progress=None):
        """Write data (whole 4 KB sectors) into the SPI flash from start: only the sectors whose CRC differs are erased
        and programmed (blank pieces skipped), each read back by the stub, then all checked again. Returns (written,
        unchanged)."""
        self.need_spi()
        self.need_board()
        if start % BLOCK or len(data) % BLOCK or start + len(data) > self.spi_size:
            raise F4Error("SPI flash writes are whole 4 KB sectors inside the chip's %d KB" % (self.spi_size // 1024))
        todo = self.verify_file(data, start, MEM_SPI)
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
    """The board id from a running ODR's debug link VERSION reply (board=...), or None."""
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
    """Refuse an image that is not the STM32F4 stub, or a span ODR reports that is not where the stub runs. ODR stages
    the image in an 8 KB buffer and copies it to its link address before the jump (docs/spec/debuglink.md 3.18), so
    the HR_C7000 rule (bss and stack inside the span) does not apply: after the jump the stub owns all of SRAM."""
    top = stub_stack_top(image)
    if top is None or b"F4STUB" not in image:
        raise F4Error("this does not look like the STM32F4 stub image (no f4stub_entry code at its start)")
    if load != RUN_ADDR:
        raise F4Error("ODR reports its stub span at 0x%X; the STM32F4 stub runs at 0x%X" % (load, RUN_ADDR))
    if not RUN_ADDR + len(image) < top <= RUN_ADDR + SRAM_SIZE:
        raise F4Error("this stub image's stack top 0x%X is outside the SRAM it runs in (0x%X-0x%X)"
                      % (top, RUN_ADDR, RUN_ADDR + SRAM_SIZE))


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
    if a.odr_enter:
        found = board_from_version(radio)
        if board is None:
            board = found
        elif found is not None and found != board:
            raise F4Error("the radio says it is a %s, not a %s" % (found, board))
        image = open(a.odr_stub, "rb").read() if a.odr_stub else stub_image()
        radio.odr_enter(image, RUN_ADDR, board=board)
    else:
        radio.attach()
        if board is None and radio.board is not None:
            board = radio.board            # a stub configured by an earlier run
    radio.check_board()
    radio.configure(board)
    if a.write_app:
        if board is None:
            raise F4Error("which radio? pass --board (%s)" % ", ".join(BOARDS))
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
        self.record = b"" if version >= 3 else config_record("dm1701")     # version 2: wiring built in
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
        return self.flash, addr - FLASH_BASE

    def w(self, addr):
        o = addr - FLASH_BASE
        return int.from_bytes(self.flash[o:o + 4], "little")

    def handle(self, op, p):
        if op in (OP_ERASE, OP_PROGRAM, OP_XERASE, OP_XPROGRAM) and not self.record:
            return self.frame(NOCONFIG)
        if op == OP_HELLO:
            rec = self.record
            on = bool(rec) and self.version >= 3
            body = (b"F4STUB" + bytes([self.version]) + (4104).to_bytes(2, "big") + self.flash_kb.to_bytes(2, "big")
                    + bytes([0x55, 1 | (HELLO_CONFIGURED if on else 0)])
                    + (int.from_bytes(rec[8:12], "big") if rec else 0).to_bytes(4, "big")
                    + (len(self.spi) if self.spi and rec else 0).to_bytes(4, "big") + bytes([0xEF, 0x40, 0x18]))
            if self.version >= 3:
                body += bytes([rec[0] if on else 0, len(rec)]) + rec
            self.frame(OK, body)
        elif op == OP_CONFIG and self.version >= 3:
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

    # MD-9600: the same application sectors as the MD-380, its own SPI flash wiring in the board record
    rec = config_record("md9600")
    check(rec.hex() == "03010203007a12000800c0000044aa201d1e1f051c3eff00", "md9600 board record %s" % rec.hex())
    check(config_record("dm1701")[2:4] == b"\x01\x00" and config_record("dm1701")[16:24] == bytes(
        [0x13, 0x14, 0x15, 5, 0x37, 0xFF, 0xFF, 0]), "dm1701 board record: SPI1 mode 0, PB3-PB5, PD7")
    f = stock(11)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.write_image("md9600", [(APP_BASE, app(5000, 3))], backup_dir=tmp)
        check(False, "md9600: no write before the board record")
    except F4Error:
        check(fake.erased == [] and fake.configs == 0, "md9600: refused by the tool before anything is sent")
    r.configure("md9600")
    check(r.board == "md9600" and r.record == rec and fake.record == rec, "md9600: configured, the echo checked")
    segs = [(APP_BASE, app(400000, 7))]
    nw, nk = r.write_image("md9600", segs, backup_dir=tmp)
    check(sorted(fake.erased) == [3, 4, 5, 6, 7] and bytes(fake.flash[0x80000:]) == bytes(f[0x80000:]),
          "md9600: sectors 3-7 written, the codec sectors 8-11 untouched %s" % fake.erased)
    check(bytes(fake.flash[0xC000:0xC000 + 400000]) == segs[0][1], "md9600: application in place")
    try:
        r.write_image("dm1701", segs, backup_dir=tmp)
        check(False, "a stub configured for the MD-9600 is not used for a DM-1701 image")
    except F4Error:
        pass
    r.configure("md9600")
    check(fake.configs == 1, "the same record twice is sent once")
    r2 = F4Radio(fake, log=quiet)          # --attach without --board: the stub's own record says which radio
    r2.hello()
    check(r2.board == "md9600", "attach: the configured board read back from HELLO")
    mp = os.path.join(tmp, "odr_md9600_codec.bin")
    merged = bytearray(b"\xff" * (0x100000 - 0xC000))
    merged[:400000] = app(400000, 7)
    merged[0xE0000 - 0xC000:0xE0000 - 0xC000 + 0x9000] = app(0x9000, 5)
    open(mp, "wb").write(merged)
    check([(a, len(d)) for a, d in load_segments(mp, "md9600")] == [(APP_BASE, 400000), (0x08080000, 0x80000)],
          "md9600: a merged image splits at sector 8")

    # A stub that answers CONFIG but doesn't echo the record: nothing written
    fake = FakeF4(stock(11))
    fake.handle_orig = fake.handle
    fake.handle = lambda op, p: (fake.frame(OK) if op == OP_CONFIG else fake.handle_orig(op, p))
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.configure("md9600")
        check(False, "a missing echo is refused")
    except F4Error:
        check(r.board is None, "a missing echo leaves the tool unconfigured")

    # A protocol 2 stub (attached from an older run): fixed DM-1701 / MD-380 wiring, so no MD-9600
    fake = FakeF4(stock(11), version=2)
    r = F4Radio(fake, log=quiet)
    r.hello()
    try:
        r.configure("md9600")
        check(False, "a protocol 2 stub is refused for the MD-9600")
    except F4Error:
        pass
    r.configure("md380")
    nw, _ = r.write_image("md380", [(APP_BASE, app(5000, 3))], backup_dir=tmp)
    check(nw == 5 and fake.configs == 0, "a protocol 2 stub still installs on the MD-380 (%d)" % nw)

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
