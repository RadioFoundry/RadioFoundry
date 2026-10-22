#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""dm32_flash: write a firmware image to a Baofeng DM-32UV or DM-1801 Pro through its own bootloader (update mode),
over the programming cable. The protocol is docs/spec/hardware-dm32.md 13.2; both radios speak it, with their own ID
string (--model). Untested on a radio.

    dm32_flash.py odr_dm32_codec.bin                      a raw application image (linked for 0x0300C000)
    dm32_flash.py odr_dm32_codec_BFUV32.bin               a Baofeng firmware file (the 256-byte header is stripped)
    dm32_flash.py --stock DM32.01.01.049.bin              back to the stock firmware (same code path)
    dm32_flash.py IMAGE --port /dev/ttyUSB0 --dry-run     check the image and print the plan, touch nothing
    dm32_flash.py --list-ports                            the USB-serial cables found
    dm32_flash.py --model dm1801pro odr_dm1801pro.bin     the DM-1801 Pro (default model: dm32)
    dm32_flash.py --self-test                             against a simulated bootloader, no radio

Update mode: radio off, hold PTT and SK1 (the side key just below PTT), switch on; the DM-32UV's green LED lights.
The DM-1801 Pro's loader reads PTT (PTB11) and the key on PTB26; which key that is isn't known yet. The bootloader at
0x03000000..0x0300C000 is never written, so update mode stays available whatever is flashed.

Needs Python 3.8+ and pyserial (pip install pyserial). Runs on macOS, Linux and Windows.
"""
import argparse
import hashlib
import os
import struct
import sys
import time

APP_BASE = 0x0300C000
FLASH_END = 0x03100000
APP_MAX = FLASH_END - APP_BASE          # 0xF4000: everything the bootloader may write
SLOT_BASE = 0x030E8000                  # ODR's codec slot (boards/dm32/dm32.ld, tools/hrc7000_codec.py)
SLOT_MAGIC = 0x43524444                 # "DDRC", first word of the slot header
FILE_MAGIC = b"BFUV32-V2"               # the DM-32UV's; MODELS has the others
FILE_END = b"END"
FILE_HEADER = 0x100
BAUD = 115200
BLOCK = 1024
HEADER_BLOCK = 128

class Model:
    """What differs between the radios that share the Baofeng bootloader: the ID the radio answers at step 3 (also
    the magic of its firmware files), the key combination, and whether ODR keeps a codec slot in the image."""

    def __init__(self, id, name, magic, keys, codec_slot):
        self.id = id
        self.name = name
        self.magic = magic
        self.keys = keys
        self.codec_slot = codec_slot

    @property
    def tag(self):
        return self.magic.decode("ascii")


MODELS = {m.id: m for m in (
    Model("dm32", "DM-32UV", FILE_MAGIC, "PTT and SK1 (the side key just below PTT)", True),
    # BF1801-V1 is the ID the loader answers (its length, 9, is the step 2 answer too). A firmware file's header is
    # assumed to be the DM-32's layout with this magic. The loader stays in update mode while PTB11 (PTT) and PTB26
    # both read low; which key PTB26 is, isn't known.
    Model("dm1801pro", "DM-1801 Pro", b"BF1801-V1", "PTT and the key on PTB26 (probably SK1)", False),
)}
MODEL_DM32 = MODELS["dm32"]

ACK, NAK, CRC_REQ, SOH, STX, EOT = 0x06, 0x15, 0x43, 0x01, 0x02, 0x04
STEP_TIMEOUT = 2.0                      # steps 1-6 and every 1 KB block
ERASE_TIMEOUT = 30.0                    # step 7: the radio erases the application area (up to about 15 s)
RETRIES = 4                             # resends of a block the radio refuses (NAK) or doesn't answer

# USB-serial bridges used in DM-32 programming cables: (VID, PID, name)
USB_SERIAL = [
    (0x1A86, 0x7523, "CH340"), (0x1A86, 0x5523, "CH341"), (0x1A86, 0x55D4, "CH9102"),
    (0x0403, 0x6001, "FTDI FT232R"), (0x0403, 0x6015, "FTDI FT231X"), (0x0403, 0x6010, "FTDI FT2232"),
    (0x0403, 0x6014, "FTDI FT232H"), (0x10C4, 0xEA60, "CP210x"), (0x067B, 0x2303, "PL2303"),
]

def update_mode_hint(model=MODEL_DM32):
    return ("is the radio in update mode? Switch it off, hold %s and switch it on%s. Check the cable is pushed fully "
            "into the radio" % (model.keys, ": the green LED lights" if model is MODEL_DM32 else ""))


class FlashError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# pure logic

def crc16_xmodem(data, crc=0):
    """CRC-16/XMODEM: polynomial 0x1021, initial value 0, no reflection, no final XOR."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def unwrap(data, model=MODEL_DM32):
    """(raw application image, 'bfuv32' or 'raw') from a file's bytes."""
    magic = model.magic
    if data.startswith(magic):
        if len(data) <= FILE_HEADER or data[FILE_HEADER - len(FILE_END):FILE_HEADER] != FILE_END:
            raise FlashError("starts with %s but the 256-byte header has no END at 0xFD: damaged file" % model.tag)
        return data[FILE_HEADER:], "bfuv32"
    for other in MODELS.values():
        if other is not model and data.startswith(other.magic):
            raise FlashError("a firmware file for the %s (%s), not the %s" % (other.name, other.tag, model.name))
    if data[:3] == b"BFU":
        raise FlashError("a Baofeng file of another version (%r), not %s" % (data[:9], model.tag))
    if data.startswith(b"\x7fELF"):
        raise FlashError("this is an ELF file: use the .bin")
    return data, "raw"


def check_image(raw, model=MODEL_DM32):
    """Problems that stop a flash (raises) and facts worth printing (returned as lines)."""
    if not raw:
        raise FlashError("the image is empty")
    if len(raw) > APP_MAX:
        raise FlashError("the image is %d bytes: it does not fit 0x%08X..0x%08X (%d bytes)"
                         % (len(raw), APP_BASE, FLASH_END, APP_MAX))
    if raw[:16] == b"\xff" * 16 or raw[:16] == bytes(16):
        raise FlashError("the image starts with 16 blank bytes: not an application")
    notes = ["%d bytes, 0x%08X..0x%08X, %d blocks of 1 KB, sha256 %s"
             % (len(raw), APP_BASE, APP_BASE + len(raw), blocks_needed(len(raw)), sha256(raw))]
    if model.codec_slot and has_codec_slot(raw):
        notes.append("ODR codec slot at 0x%08X: present (DMR voice)" % SLOT_BASE)
    return notes


def has_codec_slot(raw):
    off = SLOT_BASE - APP_BASE
    return len(raw) >= off + 4 and struct.unpack_from("<I", raw, off)[0] == SLOT_MAGIC


def blocks_needed(size):
    return (size + BLOCK - 1) // BLOCK


def header_block(name, size):
    """Step 7's 128-byte block: name, NUL, decimal size in ASCII, zero padded (the radio uses neither)."""
    n = os.path.basename(name).encode("ascii", "replace")[:HEADER_BLOCK - 16]
    body = n + b"\0" + str(size).encode("ascii")
    return body + bytes(HEADER_BLOCK - len(body))


def data_frame(n, chunk):
    """Step 8's frame after the 0x02 / 0x43 exchange: block number, its complement, 1024 bytes, CRC (big endian)."""
    chunk = chunk + bytes(BLOCK - len(chunk))
    return bytes([n & 0xFF, 0xFF - (n & 0xFF)]) + chunk + struct.pack(">H", crc16_xmodem(chunk))


def hexs(b):
    return " ".join("%02X" % x for x in b) if b else "nothing"


# ---------------------------------------------------------------------------------------------------------------
# the protocol

class Flasher:
    """Runs 13.2 over `port`: any object with write(bytes), read(n) returning what arrived (possibly nothing) within
    its own short timeout, and reset_input_buffer()."""

    def __init__(self, port, log=print, progress=None, step_timeout=STEP_TIMEOUT, erase_timeout=ERASE_TIMEOUT,
                 retries=RETRIES, model=MODEL_DM32):
        self.model = model
        self.port = port
        self.log = log
        self.progress = progress
        self.step_timeout = step_timeout
        self.erase_timeout = erase_timeout
        self.retries = retries

    def read_exact(self, n, timeout):
        end = time.monotonic() + timeout
        out = b""
        while len(out) < n:
            chunk = self.port.read(n - len(out))
            if chunk:
                out += chunk
            elif time.monotonic() >= end:
                break
        return out

    def exchange(self, step, send, want, timeout=None, what=""):
        if send:
            self.port.write(send)
        got = self.read_exact(len(want), self.step_timeout if timeout is None else timeout)
        if got == want:
            return
        if not got:
            raise FlashError("step %d%s: no answer within %.0f s: %s"
                             % (step, what, self.step_timeout if timeout is None else timeout, update_mode_hint(self.model)))
        raise FlashError("step %d%s: the radio answered %s, expected %s" % (step, what, hexs(got), hexs(want)))

    def handshake(self):
        self.port.reset_input_buffer()
        # step 1, a few times: the radio may still be starting when the user presses Enter
        for attempt in range(3):
            self.port.write(bytes([0x52]))
            got = self.read_exact(1, self.step_timeout)
            if got == bytes([ACK]):
                break
            if got:
                raise FlashError("step 1: the radio answered %s, expected 06. If the screen is on, the radio runs "
                                 "its firmware, not the bootloader: %s" % (hexs(got), update_mode_hint(self.model)))
        else:
            raise FlashError("step 1: no answer: " + update_mode_hint(self.model))
        self.exchange(2, bytes([0x4D, 0, 0, 0, 1]), bytes([0x4D, 0x01, 0x09]))
        self.exchange(3, bytes([ACK]), self.model.magic, what=" (model)")
        self.exchange(4, bytes([0x45, 0, 0, 0, 0]), bytes([ACK]))
        self.exchange(5, bytes([0x31]), bytes([CRC_REQ]))
        self.exchange(6, bytes([SOH]), bytes([CRC_REQ]))

    def probe(self):
        """Steps 1-3 only, the loader's identification (13.2): (step 2 answer, step 3 answer) as received, whatever
        they are. Stops before step 4; nothing in the radio changes before the erase at step 7. For checking whether
        another HR_C7000 radio's update loader speaks this protocol and which model string it reports."""
        self.port.reset_input_buffer()
        for attempt in range(3):
            self.port.write(bytes([0x52]))
            got = self.read_exact(1, self.step_timeout)
            if got:
                break
        if got != bytes([ACK]):
            raise FlashError("step 1: %s: not this loader protocol, or not in update mode"
                             % ("answered " + hexs(got) if got else "no answer"))
        self.port.write(bytes([0x4D, 0, 0, 0, 1]))
        step2 = self.read_exact(3, self.step_timeout)
        self.port.write(bytes([ACK]))
        step3 = self.read_exact(32, self.step_timeout)  # up to 32 bytes, as many as arrive (the DM-32 sends 9)
        return step2, step3

    def send_header(self, name, size):
        blk = header_block(name, size)
        self.log("   step 7: header sent, the radio erases the application area (up to about 15 s) ...")
        t0 = time.monotonic()
        self.exchange(7, bytes([0x00, 0xFF]) + blk + struct.pack(">H", crc16_xmodem(blk)), bytes([ACK, CRC_REQ]),
                      timeout=self.erase_timeout, what=" (erase)")
        self.log("   erased in %.1f s" % (time.monotonic() - t0))

    def send_block(self, n, chunk):
        frame = data_frame(n, chunk)
        last = None
        for attempt in range(1 + self.retries):
            if attempt:
                self.log("   block %d: %s, sending it again (%d of %d)" % (n, last, attempt, self.retries))
            self.port.write(bytes([STX]))
            got = self.read_exact(1, self.step_timeout)
            if got != bytes([CRC_REQ]):
                last = "answered %s to 02 instead of 43" % hexs(got)
                self.port.reset_input_buffer()
                continue
            self.port.write(frame)
            got = self.read_exact(1, self.step_timeout)
            if got == bytes([ACK]):
                return
            last = "refused (NAK)" if got == bytes([NAK]) else "answered %s" % hexs(got)
            self.port.reset_input_buffer()
        raise FlashError("step 8, block %d: %s after %d tries. The application area is erased now: the radio "
                         "won't start until a flash completes. Enter update mode again and rerun" % (n, last,
                                                                                                   1 + self.retries))

    def flash(self, raw, name="firmware.bin"):
        total = blocks_needed(len(raw))
        self.handshake()
        self.log("   bootloader found (%s)" % self.model.tag)
        self.send_header(name, len(raw))
        for i in range(total):
            self.send_block((i + 1) & 0xFF, raw[i * BLOCK:(i + 1) * BLOCK])
            if self.progress:
                self.progress(i + 1, total)
        self.port.write(bytes([EOT]))
        self.read_exact(1, 0.5)     # the radio may acknowledge before it restarts; not required
        self.log("   done: the radio restarts")


# ---------------------------------------------------------------------------------------------------------------
# serial ports

def list_cables():
    """[(device, description)] of the USB-serial bridges known from the radios' programming cables."""
    try:
        from serial.tools import list_ports
    except ImportError:
        raise FlashError("pyserial is missing: pip install pyserial")
    out = []
    for p in list_ports.comports():
        for vid, pid, name in USB_SERIAL:
            if p.vid == vid and p.pid == pid:
                out.append((p.device, "%s %04X:%04X %s" % (name, vid, pid, p.description or "")))
    return out


def find_port(port=None):
    """The port to use: `port` if given, else the only known USB-serial cable."""
    if port:
        return port
    cables = list_cables()
    if len(cables) == 1:
        return cables[0][0]
    if not cables:
        raise FlashError("no USB-serial programming cable found (CH340, FTDI, CP210x, PL2303): plug it in, or give "
                         "--port (dm32_flash.py --list-ports shows what was found)")
    raise FlashError("several USB-serial cables found (%s): give --port" % ", ".join(c[0] for c in cables))


class SerialPort:
    def __init__(self, device):
        try:
            import serial
        except ImportError:
            raise FlashError("pyserial is missing: pip install pyserial")
        try:
            self.s = serial.Serial(device, BAUD, bytesize=8, parity="N", stopbits=1, timeout=0.05,
                                   write_timeout=5)
        except serial.SerialException as e:
            raise FlashError("can't open %s: %s (close the CPS, qdmr and any terminal using it)" % (device, e))

    def write(self, b):
        self.s.write(b)
        self.s.flush()

    def read(self, n):
        return self.s.read(n)

    def reset_input_buffer(self):
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()


# ---------------------------------------------------------------------------------------------------------------
# simulated bootloader (self-test)

class FakeBootloader:
    """The radio side of 13.2 as a serial port object. Checks every step, the header CRC, each block's number (with
    the 0xFF -> 0x00 wrap), complement and CRC, and keeps what was written. Faults: `corrupt` = block numbers whose
    first transfer arrives damaged (the radio NAKs), `silent` = never answers, `bad_step2` = a wrong model answer,
    `erase_reads` = empty reads before the erase answer, `model` = the step 3 answer."""

    def __init__(self, corrupt=(), silent=False, bad_step2=False, erase_reads=5, model=MODEL_DM32):
        self.magic = model.magic
        self.inbuf = bytearray()
        self.out = bytearray()
        self.state = 1
        self.corrupt = set(corrupt)
        self.silent = silent
        self.bad_step2 = bad_step2
        self.erase_reads = erase_reads
        self.delay = 0
        self.expect_n = 1
        self.blocks = []
        self.header = None
        self.naks = 0
        self.done = False
        self.errors = []

    def reset_input_buffer(self):
        self.out.clear()

    def read(self, n):
        if self.delay:
            self.delay -= 1
            return b""
        r = bytes(self.out[:n])
        del self.out[:n]
        return r

    def write(self, b):
        if self.silent:
            return
        self.inbuf += b
        while self.step():
            pass

    def take(self, want):
        if len(self.inbuf) < len(want) or bytes(self.inbuf[:len(want)]) != want:
            return False
        del self.inbuf[:len(want)]
        return True

    def step(self):
        s, ib = self.state, self.inbuf
        if not ib:
            return False
        fixed = {1: (b"\x52", b"\x06"), 2: (b"\x4D\x00\x00\x00\x01", b"\x4D\x01\x09"), 3: (b"\x06", self.magic),
                 4: (b"\x45\x00\x00\x00\x00", b"\x06"), 5: (b"\x31", b"\x43"), 6: (b"\x01", b"\x43")}
        if s in fixed:
            want, answer = fixed[s]
            if len(ib) < len(want):
                return False
            if not self.take(want):
                self.errors.append("step %d: got %s" % (s, hexs(bytes(ib[:len(want)]))))
                ib.clear()
                return False
            if s == 2 and self.bad_step2:
                answer = b"\x4D\x01\x08"
            self.out += answer
            self.state += 1
            return True
        if s == 7:
            if len(ib) < 2 + HEADER_BLOCK + 2:
                return False
            frame = bytes(ib[:2 + HEADER_BLOCK + 2])
            del ib[:len(frame)]
            blk = frame[2:2 + HEADER_BLOCK]
            if frame[:2] != b"\x00\xFF" or struct.unpack(">H", frame[-2:])[0] != crc16_xmodem(blk):
                self.errors.append("header block")
                self.out.append(NAK)
                return True
            name, rest = blk.split(b"\0", 1)
            self.header = (name.decode("ascii"), int(rest.split(b"\0")[0]))
            self.delay = self.erase_reads
            self.out += bytes([ACK, CRC_REQ])
            self.state = 8
            return True
        if s == 8:              # waiting for STX or EOT
            c = ib[0]
            del ib[0]
            if c == STX:
                self.out.append(CRC_REQ)
                self.state = 9
            elif c == EOT:
                self.out.append(ACK)
                self.done = True
                self.state = 10
            else:
                self.errors.append("expected 02 or 04, got %02X" % c)
            return True
        if s == 9:              # one data frame
            if len(ib) < 2 + BLOCK + 2:
                return False
            frame = bytearray(ib[:2 + BLOCK + 2])
            del ib[:len(frame)]
            n = frame[0]
            if n in self.corrupt:
                self.corrupt.discard(n)
                frame[10] ^= 0x40           # line noise on the first transfer
            data = bytes(frame[2:2 + BLOCK])
            ok = (frame[1] == 0xFF - n and struct.unpack(">H", bytes(frame[-2:]))[0] == crc16_xmodem(data))
            if ok and n != (self.expect_n & 0xFF):
                self.errors.append("block number %d, expected %d" % (n, self.expect_n & 0xFF))
                ok = False
            if ok:
                self.blocks.append(data)
                self.expect_n += 1
                self.out.append(ACK)
            else:
                self.naks += 1
                self.out.append(NAK)
            self.state = 8
            return True
        ib.clear()
        return False

    def image(self):
        return b"".join(self.blocks)[:self.header[1]] if self.header else b""


def self_test():
    fails = []

    def check(cond, what):
        if not cond:
            fails.append(what)
            print("FAIL: " + what)

    quiet = lambda *a: None
    check(crc16_xmodem(b"123456789") == 0x31C3, "CRC-16/XMODEM check value")
    check(crc16_xmodem(b"") == 0, "CRC of nothing")
    hb = header_block("/x/odr_dm32_codec.bin", 870000)
    check(len(hb) == 128 and hb.startswith(b"odr_dm32_codec.bin\x00870000\x00"), "header block %r" % hb[:32])
    f = data_frame(256, b"\x01\x02")
    check(f[:2] == b"\x00\xFF" and len(f) == 1028 and f[4:1026] == bytes(1022), "block number wraps, zero padding")

    raw = bytes((i * 31 + (i >> 10)) & 0xFF for i in range(300 * 1024 + 517))    # 301 blocks: numbers wrap
    packed = FILE_MAGIC + b"\xff" * (FILE_HEADER - len(FILE_MAGIC) - 3) + FILE_END + raw
    check(unwrap(packed) == (raw, "bfuv32"), "BFUV32 header stripped")
    check(unwrap(raw) == (raw, "raw"), "raw image kept")
    for bad, what in ((packed[:0xFD] + b"XYZ" + raw, "header without END"), (b"\x7fELF" + bytes(100), "ELF"),
                      (b"BFUV31-V1" + bytes(300), "other Baofeng version")):
        try:
            unwrap(bad)
            check(False, "refuses " + what)
        except FlashError:
            pass
    for bad, what in ((b"", "empty"), (b"\x01" * (APP_MAX + 1), "too big"), (b"\xff" * 4096, "blank")):
        try:
            check_image(bad)
            check(False, "check_image refuses " + what)
        except FlashError:
            pass
    check(len(check_image(b"\x01" * APP_MAX)) == 1, "an image filling the area fits")
    m18 = MODELS["dm1801pro"]
    packed18 = m18.magic + b"\xff" * (FILE_HEADER - len(m18.magic) - 3) + FILE_END + raw
    check(unwrap(packed18, m18) == (raw, "bfuv32"), "DM-1801 Pro file header stripped")
    for data, model, what in ((packed18, MODEL_DM32, "a DM-1801 Pro file for the DM-32UV"),
                              (packed, m18, "a DM-32UV file for the DM-1801 Pro")):
        try:
            unwrap(data, model)
            check(False, "refuses " + what)
        except FlashError as e:
            check("not the" in str(e), "message names both radios: %s" % e)
    slot = bytearray(b"\x01" * (SLOT_BASE - APP_BASE)) + struct.pack("<I", SLOT_MAGIC) + bytes(100)
    check(has_codec_slot(bytes(slot)) and not has_codec_slot(raw), "codec slot detection")

    fake = FakeBootloader(corrupt={3, 255, 0})
    seen = []
    Flasher(fake, log=quiet, progress=lambda d, t: seen.append(d), step_timeout=0.2).flash(raw, "odr.bin")
    check(not fake.errors, "bootloader saw errors: %s" % fake.errors)
    fake18 = FakeBootloader(model=m18)
    Flasher(fake18, log=quiet, step_timeout=0.2, model=m18).flash(raw[:5000], "odr18.bin")
    check(not fake18.errors and fake18.image() == raw[:5000], "DM-1801 Pro flash through its own ID")
    try:
        Flasher(FakeBootloader(), log=quiet, step_timeout=0.2, model=m18).flash(raw[:1024])
        check(False, "a DM-32UV answering a DM-1801 Pro flash stops")
    except FlashError as e:
        check("step 3 (model)" in str(e), "wrong model stops at step 3: %s" % e)
    check("PTB26" in update_mode_hint(m18) and "green LED" in update_mode_hint(), "update mode hints per radio")
    check(fake.header == ("odr.bin", len(raw)), "header name and size %r" % (fake.header,))
    check(fake.image() == raw, "image written byte for byte (block numbers 1..255, 0, 1..)")
    check(len(fake.blocks) == 301 and fake.naks == 3, "three damaged blocks NAKed and resent (%d NAKs)" % fake.naks)
    check(fake.done and seen[-1] == 301, "EOT sent, progress to the end")

    fake_always = FakeBootloader()
    fake_always.corrupt = type("Always", (set,), {"discard": lambda self, x: None})({2})
    try:
        Flasher(fake_always, log=quiet, step_timeout=0.2, retries=2).flash(raw[:4096])
        check(False, "a block refused every time stops the flash")
    except FlashError as e:
        check("block 2" in str(e) and "3 tries" in str(e), "message names the block: %s" % e)

    for fk, want in ((FakeBootloader(silent=True), "update mode"), (FakeBootloader(bad_step2=True), "step 2")):
        try:
            Flasher(fk, log=quiet, step_timeout=0.05).flash(raw[:1024])
            check(False, "fails: " + want)
        except FlashError as e:
            check(want in str(e), "error mentions %r: %s" % (want, e))

    fake = FakeBootloader()
    s2, s3 = Flasher(fake, log=quiet, step_timeout=0.05).probe()
    check(s2 == b"\x4D\x01\x09" and s3 == FILE_MAGIC, "probe reads the DM-32 identification: %s %r" % (hexs(s2), s3))
    check(fake.state == 4 and not fake.blocks and fake.header is None, "probe stops before step 4")
    other = Model("other", "another radio", b"OTHER-LDR", "?", False)
    s2, s3 = Flasher(FakeBootloader(model=other), log=quiet, step_timeout=0.05).probe()
    check(s3 == b"OTHER-LDR", "probe reports another model string: %r" % s3)
    try:
        Flasher(FakeBootloader(silent=True), log=quiet, step_timeout=0.05).probe()
        check(False, "probe of a silent radio fails")
    except FlashError as e:
        check("step 1" in str(e), "silent probe names step 1: %s" % e)

    fake = FakeBootloader(erase_reads=10 ** 9)
    try:
        Flasher(fake, log=quiet, step_timeout=0.05, erase_timeout=0.2).flash(raw[:1024])
        check(False, "erase timeout")
    except FlashError as e:
        check("step 7" in str(e), "erase timeout names step 7: %s" % e)

    print("dm32_flash self-test %s (%d failure%s)" % ("PASSED" if not fails else "FAILED", len(fails),
                                                    "" if len(fails) == 1 else "s"))
    return 0 if not fails else 1


# ---------------------------------------------------------------------------------------------------------------

def load(path, model=MODEL_DM32):
    """(raw image, kind) from a file, checked."""
    with open(path, "rb") as f:
        data = f.read()
    raw, kind = unwrap(data, model)
    return raw, kind


def progress_bar(done, total):
    sys.stdout.write("\r   writing %d/%d blocks (%d%%)" % (done, total, done * 100 // total))
    sys.stdout.flush()
    if done == total:
        sys.stdout.write("\n")


def run(image_path, port=None, dry_run=False, yes=False, log=print, stock=False, model=MODEL_DM32):
    """Check, announce update mode, flash. Raises FlashError. Used by dm32_migrate.py too."""
    raw, kind = load(image_path, model)
    log("image %s: %s" % (image_path, "Baofeng %s file, header stripped" % model.tag if kind == "bfuv32" else
                          "raw application image"))
    for line in check_image(raw, model):
        log("   " + line)
    if stock and model.codec_slot and has_codec_slot(raw):
        log("   ! --stock, but this is an ODR image (it has the ODR codec slot)")
    if not stock and model.codec_slot and not has_codec_slot(raw):
        log("   no ODR codec slot: fine for a stock file; an ODR image without it has no DMR voice "
            "(tools/hrc7000_codec.py merges it)")
    if dry_run:
        try:
            dev = find_port(port)
        except FlashError as e:
            dev = "(%s)" % e
        log("[dry run] would use %s at 115200 8N1, wait for update mode and write %d blocks" %
            (dev, blocks_needed(len(raw))))
        return raw
    dev = find_port(port)
    log("cable: %s" % dev)
    log("")
    log("   Radio: switch it OFF. Hold %s and switch it on while" % model.keys)
    log("   holding both. %s Release the keys. Keep the cable in." %
        ("The green LED lights: that is update mode." if model is MODEL_DM32 else "The radio stays dark or shows no "
         "normal start-up: that is update mode."))
    if not yes:
        if not sys.stdin.isatty():
            raise FlashError("needs you at the radio: run in a terminal, or pass --yes")
        input("   Press Enter when the green LED is on (Ctrl-C stops; nothing is written yet) ")
    sp = SerialPort(dev)
    try:
        Flasher(sp, log=log, progress=progress_bar, model=model).flash(raw, os.path.basename(image_path))
    finally:
        sp.close()
    log("If the radio does not start: remove and refit the battery, then switch it on. Update mode still works")
    log("(%s at power-on) to flash again or go back to stock." % model.keys)
    return raw


def probe(port=None, log=print):
    """--probe: identify the update loader of the radio on the cable, write nothing."""
    dev = find_port(port)
    log("cable: %s. The radio must be in its update mode already (the DM-32: PTT + SK1 at power-on)" % dev)
    sp = SerialPort(dev)
    try:
        s2, s3 = Flasher(sp, log=log).probe()
    finally:
        sp.close()
    log("   step 2 answer: %s (the DM-32 answers 4D 01 09)" % (hexs(s2) if s2 else "none"))
    log("   step 3 answer: %r (the DM-32 answers %r)" % (s3, FILE_MAGIC))
    log("   stopped before step 4: nothing was written. Switch the radio off to leave update mode")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Flash a Baofeng DM-32UV or DM-1801 Pro through its bootloader "
                                             "(update mode at power-on) over the programming cable.",
                                 epilog=__doc__.split("\n\n", 1)[1], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="raw application image or a _BFUV32.bin file")
    ap.add_argument("--model", choices=sorted(MODELS), default="dm32", help="the radio (default: dm32)")
    ap.add_argument("--stock", metavar="FILE", help="flash Baofeng's stock firmware file back (recovery)")
    ap.add_argument("--port", help="serial port of the programming cable (default: the only CH340/FTDI/CP210x/PL2303)")
    ap.add_argument("--dry-run", action="store_true", help="check the image and print the plan; touch nothing")
    ap.add_argument("--yes", "-y", action="store_true", help="don't wait for Enter before flashing")
    ap.add_argument("--list-ports", action="store_true", help="list the USB-serial cables found")
    ap.add_argument("--probe", action="store_true", help="only identify the loader (steps 1-3): print what it "
                    "answers, write nothing. For another HR_C7000 radio in its update mode")
    ap.add_argument("--self-test", action="store_true", help="run against a simulated bootloader")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    try:
        if a.list_ports:
            cables = list_cables()
            for dev, desc in cables:
                print("%s  %s" % (dev, desc))
            if not cables:
                print("no USB-serial cable found")
            return 0
        if a.probe:
            return probe(a.port)
        path = a.stock or a.image
        if not path or (a.stock and a.image):
            ap.error("give one image, or --stock FILE")
        run(path, a.port, a.dry_run, a.yes, stock=bool(a.stock), model=MODELS[a.model])
        return 0
    except FlashError as e:
        print("dm32_flash: %s" % e, file=sys.stderr)
        return 1
    except OSError as e:
        print("dm32_flash: %s" % e, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ndm32_flash: stopped", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
