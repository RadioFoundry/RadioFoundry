#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""f4stub: the host side of the STM32F4 flash stub (DM-1701 first, then the MD-380). docs/spec/stub-stm32f4.md.

The stub (stub/stm32f4, built with `make -C stub/stm32f4`) is loaded by a running ODR built with CONFIG_STUB_ENTRY=y
through the debug link's STUB command, over the radio's USB serial port. It then answers on the same port (or, if it
had to enumerate again, on a port with the same serial number). stubtool.py hands STM32 radios to this module:

    stubtool.py PORT --odr-enter --write-app build/dm1701/odr_dm1701.bin   install a build (sector 9 part found next to it)
    stubtool.py PORT --odr-enter --progmem flash.bin                      back up the whole internal flash (1 MB)
    stubtool.py PORT --odr-enter --verify-progmem flash.bin               compare by CRC
    stubtool.py PORT --odr-enter --q128 spi.bin                           back up the external SPI flash (16 MB)
    stubtool.py PORT --odr-enter --write-q128 spi.bin                     put it back (changed 4 KB sectors only)
    stubtool.py PORT --attach --boot reset                                restart into the firmware
    f4stub.py --self-test                                                 against a simulated radio, no hardware
    f4stub.py --embed build/stub-stm32f4/f4stub.bin                       put a new stub build into this file
    f4stub.py --check-embedded build/stub-stm32f4/f4stub.bin              exit 1 when the build differs from it

The tool sends the stub embedded below (checked by its SHA-256), so the host tools and the web flasher send the same
bytes; `--odr-stub FILE` sends another build instead. After a change to stub/stm32f4, rebuild and run --embed; CI
fails while the embedded copy and a fresh build differ.

Framing and the debug link loader are stubtool.Radio's; what differs is here: the ops (ERASE / PROGRAM / RESET are
new, SUMS and READ take a flash address), 4 KB blocks inside coarse erase sectors (16 to 128 KB), the sector 3
interlock and the update order that goes with it, and the bytes a sector shares with the codec (the MD-380's sector 6),
which are read back and kept.
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
F4STUB_SHA256 = "5330a97316614b4d25d3eb6591aa54ed43f45f364a586c4242ea3e7969c80c0f"
F4STUB_B64 = (
    "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"
)
# --- F4STUB end

MAGIC = 0x46345342                     # 'F4SB': ERASE and RESET (stub/stm32f4/f4stub.h)
OP_HELLO, OP_PEEK, OP_SUMS, OP_READ, OP_RESET, OP_ERASE, OP_PROGRAM = 0x01, 0x02, 0x07, 0x09, 0x0D, 0x10, 0x11
OP_XERASE, OP_XPROGRAM = 0x12, 0x13
MEM_INTERNAL, MEM_SPI = 0, 1           # the memory byte SUMS and READ end with (stub version 2)
OK, BADSUM, BADOP, BADARG, TOOLONG, REFUSED, FLASHERR = 0x05, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10
BLOCK = 4096
FLASH_BASE = 0x08000000
APP_BASE = 0x0800C000                  # sector 3: the bootloader starts the application here; 0-2 are never written
SUM_MAX = 800                          # SUMS records per request (the stub allows 820)
READ_PACK = 16                         # blocks per READ request
ERASE_TIMEOUT = 6.0                    # 128 KB: 2 s worst case in the data sheet, the stub gives up at 4 s

# Flash the firmware owns on each radio (docs/spec/hardware-dm1701.md 1.4, hardware-md380.md 1.3). Everything else in
# a sector that is rewritten is read back from the radio and written again unchanged: the codec windows, and the
# MD-380's sector 6 tail. Sectors with no owned byte are never erased (the DM-1701's codec sectors 7, 8, 10 and 11).
BOARDS = {
    "dm1701": {"name": "Baofeng DM-1701", "names": ("DM-1701", "dm1701"),
               "owned": [(0x0800C000, 0x08060000), (0x080A0000, 0x080C0000)], "rodata2": 0x080A0000},
    "md380": {"name": "TYT MD-380", "names": ("MD-380", "MD-380 VHF", "md380"),
              "owned": [(0x0800C000, 0x08050C44)], "rodata2": None},
}


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
        return self.serial.Serial(path, self.baud, timeout=0.1)


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


def load_segments(path, board_id, no_rodata2=False):
    """[(addr, bytes)] for a build: the application .bin at 0x0800C000 and, on the DM-1701, the sector 9 part
    (<name>_rodata2.bin next to it). A merged _codec.bin is refused by write_image (it reaches past the owned flash)."""
    segs = [(APP_BASE, open(path, "rb").read())]
    r2 = BOARDS[board_id]["rodata2"]
    if r2 is not None:
        base, ext = os.path.splitext(path)
        side = base + "_rodata2" + ext
        if os.path.exists(side):
            segs.append((r2, open(side, "rb").read()))
        elif not no_rodata2:
            raise F4Error("%s has no %s next to it: the DM-1701 image has two parts (hardware-dm1701.md 1.4); "
                          "pass --no-rodata2 if the application is meant to go alone" % (path, os.path.basename(side)))
    return segs


class F4Radio(stubtool.Radio):
    """A radio running the STM32F4 stub. port: a pyserial-like object; reopen: a callable that opens the port again
    (after the stub enumerated afresh) and returns it, or None."""

    def __init__(self, port, log=print, retries=3, reopen=None):
        super().__init__(port, log=log, retries=retries)
        self.reopen = reopen
        self.flash_kb = 0
        self.rdp = 0
        self.adopted = False

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
        if self.app_base != APP_BASE:
            raise F4Error("the stub reports the application at 0x%08X, this tool knows 0x%08X" % (self.app_base, APP_BASE))
        return self.version, self.max_payload

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
            "SPI flash %d KB (JEDEC %s)" % (self.spi_size // 1024, self.spi_id) if self.spi_size else "no SPI flash",
            "adopted from ODR" if self.adopted else "enumerated again"))

    def peek(self, addr, n):
        return self.request(OP_PEEK, addr.to_bytes(4, "big") + n.to_bytes(2, "big"))

    @staticmethod
    def mem_byte(mem):
        return b"" if mem == MEM_INTERNAL else bytes([mem])

    def need_spi(self):
        if not self.spi_size:
            raise F4Error("the stub found no SPI flash (stub version %d)" % self.version)

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
            raise F4Error("the chip reports %d KB of flash; the DM-1701 and MD-380 have 1024" % self.flash_kb)

    def plan(self, board_id, segments):
        """{sector index: new content} for every sector that holds firmware-owned flash, and which of those differ
        from the radio. Owned bytes come from the segments (0xFF where they have none); the rest is read back."""
        owned = BOARDS[board_id]["owned"]
        for addr, data in segments:
            if not any(lo <= addr and addr + len(data) <= hi for lo, hi in owned):
                raise F4Error("image data at 0x%08X..0x%08X is outside the flash the firmware owns on the %s (%s): a "
                              "codec-merged image or the wrong board?" % (addr, addr + len(data) - 1, BOARDS[board_id]["name"],
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
        new, changed, kept = self.plan(board_id, segments)
        if not changed:
            return 0, len(new)
        sec = {i: (a, s) for i, a, s in f405_sectors(self.flash_kb)}
        for idx in changed:
            if idx in kept:			# bytes that are not ours (the MD-380's codec in sector 6): a copy first
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
        radio.odr_enter(image, 0x20000000)
    else:
        radio.attach()
    radio.check_board()
    if a.write_app:
        if board is None:
            raise F4Error("which radio? pass --board dm1701 or --board md380")
        segs = load_segments(a.write_app, board, a.no_rodata2)
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
    """The stub's protocol and safety rules (stub/stm32f4/f4stub_proto.c) on a simulated 1 MB flash."""

    def __init__(self, flash, flash_kb=1024, fail_after=None, spi=None):
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
        if op == OP_HELLO:
            self.frame(OK, b"F4STUB" + bytes([2]) + (4104).to_bytes(2, "big") + self.flash_kb.to_bytes(2, "big")
                       + bytes([0x55, 1]) + APP_BASE.to_bytes(4, "big")
                       + (len(self.spi) if self.spi else 0).to_bytes(4, "big") + bytes([0xEF, 0x40, 0x18]))
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

    # DM-1701: application + sector 9; codec sectors 7, 8, 10, 11 and the bootloader never touched
    f = stock(5)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    check(r.flash_kb == 1024 and r.adopted, "hello parsed")
    segs = [(APP_BASE, app(300000, 3)), (0x080A0000, app(50000, 11))]
    nw, nk = r.write_image("dm1701", segs, backup_dir=tmp)
    fl = fake.flash
    check(nw == 5 and nk == 0, "dm1701: sectors 3-6 and 9 written (%d, %d)" % (nw, nk))
    check(fake.erased[0] == 3 and sorted(fake.erased) == [3, 4, 5, 6, 9], "dm1701: sector 3 erased first, only owned sectors %s" % fake.erased)
    check(bytes(fl[:0xC000]) == boot, "dm1701: bootloader untouched")
    check(bytes(fl[0x60000:0xA0000]) == bytes(f[0x60000:0xA0000]) and bytes(fl[0xC0000:]) == bytes(f[0xC0000:]),
          "dm1701: codec sectors untouched")
    check(bytes(fl[0xC000:0xC000 + 300000]) == segs[0][1], "dm1701: application in place")
    check(bytes(fl[0xC000 + 300000:0x60000]) == b"\xff" * (0x60000 - 0xC000 - 300000), "dm1701: the old tail is cleared")
    check(bytes(fl[0xA0000:0xA0000 + 50000]) == segs[1][1], "dm1701: sector 9 part in place")
    fake.erased = []
    nw, nk = r.write_image("dm1701", segs, backup_dir=tmp)
    check(nw == 0 and nk == 5 and fake.erased == [], "dm1701: the same image again writes nothing")
    segs2 = [(APP_BASE, app(300000, 3)[:200000] + app(100000, 9)), segs[1]]
    fake.erased = []
    nw, _ = r.write_image("dm1701", segs2, backup_dir=tmp)
    check(sorted(fake.erased) == [3, 5, 6] and nw == 3, "dm1701: only changed sectors (plus 3) %s" % fake.erased)

    # A merged codec image and a bad vector table are refused before anything is erased
    fake.erased = []
    for bad, what in (([(APP_BASE, app(0x80000, 3))], "reaches the codec sectors"),
                      ([(APP_BASE, b"\x00" * 64)], "no stack pointer"),
                      ([(0x08004000, app(64, 3))], "bootloader")):
        try:
            r.write_image("dm1701", bad, backup_dir=tmp)
            check(False, "refused: " + what)
        except F4Error:
            check(fake.erased == [], "refused before erasing: " + what)

    # Interrupted after a few programs: the first word is still erased, so the bootloader stays in DFU
    f = stock(7)
    fake = FakeF4(f, fail_after=10)
    r = F4Radio(fake, log=quiet)
    r.hello()
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

    # MD-380: sector 6 shared with the codec; the codec bytes from 0x08050C44 are read back and kept, and saved
    f = stock(13)
    fake = FakeF4(f)
    r = F4Radio(fake, log=quiet)
    r.hello()
    segs = [(APP_BASE, app(0x08050C44 - APP_BASE - 1000, 17))]
    nw, nk = r.write_image("md380", segs, backup_dir=tmp)
    fl = fake.flash
    check(sorted(fake.erased) == [3, 4, 5, 6], "md380: sectors 3-6 %s" % fake.erased)
    check(bytes(fl[0x50C44:]) == bytes(f[0x50C44:]), "md380: codec bytes in sector 6 and after kept")
    check(bytes(fl[0xC000:0xC000 + len(segs[0][1])]) == segs[0][1], "md380: application in place")
    saved = os.path.join(tmp, "md380_sector6_0x08040000.bin")
    check(os.path.exists(saved) and open(saved, "rb").read() == bytes(f[0x40000:0x60000]), "md380: sector 6 saved first")
    try:
        r.write_image("md380", [(APP_BASE, app(0x08050C44 - APP_BASE + 4, 3))], backup_dir=tmp)
        check(False, "md380: an image into the codec window is refused")
    except F4Error:
        pass

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
    try:
        r.spi_write(b"\xff" * BLOCK)
        check(False, "no SPI flash: refused")
    except F4Error:
        pass

    check(len(stub_image()) > 4096 and stub_image()[:4] != b"\xff" * 4, "the embedded stub decodes and checks")

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

        class Serial:
            def __init__(self, path, baud, timeout):
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
