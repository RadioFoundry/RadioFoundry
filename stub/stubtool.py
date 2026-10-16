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
STUB_SHA256 = "07ce3abe3aaa7ac9789316dbe8894c85820c19a113620ed47bd3cf9759fd74ae"
STUB_B64 = (
    "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"
)
# --- STUB end
# --- STUB ODR begin (stub/embed.py odr rewrites everything up to the end marker)
STUB_ODR_SHA256 = "2bd5eb955901c190f52ec67935e5b33fe745c3392387e8eb4c1673332cde3125"
STUB_ODR_B64 = (
    "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"
)
# --- STUB ODR end

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
LED_RED, LED_GREEN = 33, 32         # PTB1 = red, PTB0 = green, seen on a DM-1801 Pro 2026-10-15 (ODR pins.h has them the other way; DM-32UV unchecked)
OK, BADSUM, BADOP, SPITIMEOUT, TOOLONG = 0x05, 0x0A, 0x0B, 0x0C, 0x0E
# memory: (SSI base, SER, GPIO chip-select pin or None, size, name). The data flash's chip select is the GPIO PTA18
# (hardware-dm32.md 10; dm1801pro-hardware.md 6.1); the SSI's own select is not wired to the flash on its own.
MEM = {"progmem": (SSI_PROG, True, None, 0x100000, "program flash"),
       "q128": (SSI_DATA, True, 18, 0x1000000, "data flash")}


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


def stub_odr_image():
    """The stub linked for a running ODR (RAM at 0x17000, 84 MHz clock), for --odr-enter."""
    blob = zlib.decompress(base64.b64decode("".join(STUB_ODR_B64)))
    if hashlib.sha256(blob).hexdigest() != STUB_ODR_SHA256:
        raise DumpError("the embedded ODR stub fails its own checksum")
    return blob


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


# played when a run finishes (--done-led): three green flashes, then the yellow status blink resumes
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

    def start(self, wait=120, stub=None):
        stub = stub_image() if stub is None else stub
        stub += b"\xFF" * (-len(stub) % BLOCK)
        self.stub_head = stub[:CHUNK]
        self.wait_for_radio(wait)
        self.port.write(b"\x02\x14\x00\x03")
        time.sleep(0.05)
        blocks = len(stub) // BLOCK
        self.log("boot ROM answered; loading %d-byte stub (%d blocks)" % (len(stub), blocks))
        for i in range(blocks):
            payload = (LOAD_ADDR + i * BLOCK).to_bytes(4, "big") + b"\x00\x00\x00\x40" + stub[i * BLOCK:(i + 1) * BLOCK]
            for attempt in range(3):             # a block is a write to a fixed address: sending it again is safe
                self.port.reset_input_buffer()
                self.port.write(rom_cmd(0x18, payload))
                if self.read_exact(4, 1.0) == b"\x02\x05\x00\x03":
                    break
                self.log("stub block %d not acknowledged, sending it again" % i)
            else:
                raise DumpError("stub block %d was not acknowledged" % i)
        self.port.write(rom_cmd(0x1B, LOAD_ADDR.to_bytes(4, "big")))
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

    def hello(self):
        r = self.request(0x01, retries=0)
        if r[:6] != b"C7STUB":
            raise DumpError("unexpected hello %r" % r)
        self.info = int.from_bytes(r[13:17], "big") if len(r) >= 17 else 0      # the port's facts (HR_C7000: SSI bursts)
        return r[6], (r[7] << 8) | r[8]

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

    def odr_enter(self, image=None, entry=None):
        """Load the stub into a running ODR's free RAM over its debug link and jump to it (radio ON, ODR running). Afterwards
        the stub answers on the same port. Recover the radio with a power cycle (or --boot reset)."""
        image = stub_odr_image() if image is None else image
        r = self.dl_request(DL_STUB, b"\x00")
        if r[0] != 0 or len(r) < 19 or int.from_bytes(r[1:5], "little") != STUB_MAGIC:
            raise DumpError("ODR's STUB info refused or malformed (%s): firmware built without CONFIG_STUB_ENTRY?" % r.hex())
        load, size = int.from_bytes(r[5:9], "little"), int.from_bytes(r[9:13], "little")
        chunk = min(int.from_bytes(r[13:15], "little"), 59)
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
        if r[0] != 0:
            raise DumpError("ODR refused to enter the stub (status %d): image incomplete or CRC mismatch" % r[0])
        self.log("ODR is jumping to the stub")
        time.sleep(0.3)
        self.attach()

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
        ok = WRITE_OK.get(mem, []) + ([(APP_START, APP_END)] if app and mem == "progmem" else [])
        if not any(a <= start and end <= b for a, b in ok):
            raise DumpError("%s 0x%X-0x%X is outside the allowed write regions %s" %
                            (MEM[mem][4], start, end, ", ".join("0x%X-0x%X" % r for r in WRITE_OK.get(mem, []))))

    def erase_sectors(self, mem, start, end):
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
        flags = (1 if ser else 0) | (2 if cs is not None else 0)
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
                    got = self.peek(LOAD_ADDR, CHUNK)
                    if got != (ref or got):
                        raise DumpError("read-back differs")
                    ref = got
                got = self.peek(LOAD_ADDR, CHUNK)         # the stub's own first bytes: same every time, and a full-size frame
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
        flags = (1 if ser else 0) | (2 if cs is not None else 0)
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
        if id_ in (b"\xff\xff\xff", b"\x00\x00\x00"):
            raise DumpError("%s does not answer the ID command (%s): chip select or pin mux wrong, nothing was dumped"
                            % (MEM[mem][4], id_.hex()))

    def windows(self, mem, start, end):
        size = MEM[mem][3]
        if not 0 <= start < end <= size:
            raise DumpError("%s range must lie within 0..0x%X" % (mem, size))
        for w in range(start // SECTOR * SECTOR, end, WINDOW * SECTOR):
            yield w, min(WINDOW, (end - w + SECTOR - 1) // SECTOR)

    def dump(self, out, mem, start, end, progress=None, compress=True):
        """The radio reads each sector, CRCs it and sends it blank (a flag), LZ4-compressed or raw; every sector must
        match the radio's CRC-32. A sector that doesn't is asked for again, uncompressed from the second try."""
        self.check_id(mem)
        size = MEM[mem][3]
        if not 0 <= start < end <= size:
            raise DumpError("%s range must lie within 0..0x%X" % (mem, size))
        out.seek(0)
        out.truncate(end - start)
        self.skipped = self.packed = 0
        self.wire = 0
        total = fails = 0
        sa = start // SECTOR * SECTOR
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
                    lo, hi = max(start, sa), min(end, sa + SECTOR)
                    out.seek(lo - start)
                    out.write(sec[lo - sa:hi - sa])
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
        out.flush()
        if total == self.skipped:
            self.log("warning: every sector was blank (0xFF)")

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
# bit in the data, `silent` = request numbers with no reply.

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
    def __init__(self, images, drop=None, flip=(), silent=(), swap=(), stub_len=1536):
        self.images = images                # {"progmem": bytes, "q128": bytes}
        self.drop, self.flip, self.silent, self.swap = drop or {}, set(flip), set(silent), set(swap)
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
                if addr != LOAD_ADDR + len(self.loaded) or payload[4:8] != b"\x00\x00\x00\x40":
                    self.errors.append("load block address")
                self.loaded += payload[8:]
            else:
                if int.from_bytes(payload, "big") != LOAD_ADDR or len(self.loaded) != self.stub_len:
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
        elif op == 0x02:
            a = int.from_bytes(payload[:4], "big")
            c = int.from_bytes(payload[4:6], "big")
            if a == TIMER2_VALUE:
                self.mem[a] = (-int(time.monotonic() * self.t_hz)) & 0xFFFFFFFF
            if LOAD_ADDR <= a < LOAD_ADDR + len(self.loaded):
                body = bytes(self.loaded[a - LOAD_ADDR:a - LOAD_ADDR + c])
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
            mem = "progmem" if base == SSI_PROG else "q128"
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
            mem = "progmem" if base == SSI_PROG else "q128"
            sel = bool(flags & 1) and (mem == "progmem" or (flags & 2 and cs == 18))
            frames = bytearray()
            for i in range(cnt):
                sec = self.images[mem][addr + i * SECTOR:addr + (i + 1) * SECTOR] if sel else b"\xff" * SECTOR
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
    class FakeODR:
        def __init__(self):
            self.timeout = 0.02
            self.out = b""
            self.mem = bytearray(0x18000)
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
            assert cmd == DL_STUB
            op, st, body = p[0], 0, b""
            if op == 0:
                body = (STUB_MAGIC.to_bytes(4, "little") + (0x17000).to_bytes(4, "little") + (0x18000).to_bytes(4, "little")
                        + (59).to_bytes(2, "little") + b"\0" + (0).to_bytes(4, "little"))
            elif op == 1:
                self.total, self.crc = int.from_bytes(p[5:9], "little"), int.from_bytes(p[9:13], "little")
                self.mem = bytearray(0x18000)
            elif op == 2:
                off = int.from_bytes(p[1:5], "little")
                self.mem[off:off + len(p) - 5] = p[5:]
            elif op == 3:
                st = 0 if zlib.crc32(bytes(self.mem[:self.total])) == self.crc else 4
                self.entered = st == 0
            rep = bytes([cmd | 0x80, seq]) + (1 + len(body)).to_bytes(2, "little") + bytes([st]) + body
            self.out = b"\xA5\x5A" + rep + crc16_ccitt(rep).to_bytes(2, "little")
    odr = FakeODR()
    ro = Radio(odr, log=quiet)
    ro.attach = lambda: None
    ro.odr_enter(stub)
    check(odr.entered and bytes(odr.mem[:len(stub)]) == stub, "odr_enter loads the image in chunks and enters after the CRC check")
    mute = FakeRadio(images)
    mute.out.clear()
    try:
        Radio(mute, log=quiet).start(wait=0.3, stub=stub)
        check(False, "no radio: the wait ends")
    except DumpError as e:
        check("switch it on" in str(e), "no-radio message: %s" % e)
    try:
        check(len(stub_image()) > 0, "embedded stub decodes")
    except DumpError as e:
        check(False, "embedded stub: %s" % e)
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
    ap.add_argument("--odr-stub", metavar="FILE", help="with --odr-enter: this stub image instead of the embedded one")
    ap.add_argument("--write-app", metavar="IMAGE", help="program an ODR application image (raw .bin linked for 0x0300C000) into the "
                    "program flash at 0xC000: only changed 4 KB sectors are written, then verified; the bootloader is never touched. "
                    "Use with --odr-enter (radio running ODR) or the boot-ROM load; finish with --boot reset")
    ap.add_argument("--attach", action="store_true", help="the stub from an earlier run is still up: skip the boot")
    ap.add_argument("--stub", metavar="FILE", help="load this stub image instead of the embedded one")
    ap.add_argument("--board", choices=("dm1701", "md380"), help="an STM32 radio (stub/f4stub.py); found from the "
                    "radio's VERSION reply with --odr-enter, needed with --attach")
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
    for mem, f, rng in (("progmem", a.progmem, a.progmem_range), ("q128", a.q128, a.q128_range)):
        if f:
            jobs.append((mem,) + (f,) + parse_range(rng, MEM[mem][3]))
    if not (jobs or a.probe or a.peek or a.verify_progmem or a.verify_q128 or a.led_test or a.led_seq or a.boot or a.write_test or a.write_app or a.write_q128):
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
            reopen = lambda: serial.Serial(a.port, 115200, timeout=0.1)
        radio = Radio(port, retries=a.retries)
        # The STM32 radios (DM-1701, MD-380) run their own stub over USB: stub/f4stub.py
        import f4stub
        if a.board is None and a.odr_enter:
            a.board = f4stub.board_from_version(radio)
        if a.board in f4stub.BOARDS:
            return f4stub.run(a, port, reopen)
        if a.odr_enter:
            radio.odr_enter(open(a.odr_stub, "rb").read() if a.odr_stub else None)
        elif a.attach:
            radio.attach()
        else:
            print("radio OFF? switch it on now (waiting up to %d s)" % a.wait)
            radio.start(a.wait, stub)
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
        for mem, path in (("progmem", a.verify_progmem), ("q128", a.verify_q128)):
            if path:
                data = open(path, "rb").read()
                bad = radio.verify(mem, data)
                print("%s vs %s: %s" % (MEM[mem][4], path, "identical" if not bad else
                      "%d sectors differ, first at 0x%X" % (len(bad), bad[0])))
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
