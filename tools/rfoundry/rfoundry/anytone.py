# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The AnyTone update mode (boards.py family "anytone-update"): the vendor's own firmware update protocol on the
radio's USB serial port, which the bootloader runs when the radio is turned on with the board's keys held
(docs/spec/hardware-d578uv.md 8). rfoundry (cli.py), tools/anytone_update.py and the web flasher
(site/static/js/flasher/anytone.js) speak it the same way.

The radio names its model when asked (byte 02), and the name differs by MCU: a GD32 radio and an Artery radio of the
same model answer different names, as the vendor's files carry them (board.toml [flash.anytone] models, per [hardware]
variants code). As the vendor's updater does, the tools write a vendor firmware set only to a radio whose model its
.spi names, and RadioFoundry only to a variant whose image this board is (an Artery radio has none: it is refused,
while its own stock firmware can still go back).

The image the radio takes for RadioFoundry carries the DMR baseband's boot block in the board's modem boot slot,
merged from the user's own stock file on their machine (modemboot.py): never in a release.
"""
import hashlib
import os
import struct
import time

from . import modemboot

ACK, NAK = 0x06, 0x15
HELLO = b"UPDATE"
HELLO_REPLY = b"QX\x06"
IDENTIFY = b"\x02"
IDENTIFY_LEN = 16
FINISH = b"\x18"
DATA = 0x01
CHUNK = 32
FRAME_LEN = 40
BAUD = 921600
RETRIES = 3
TIMEOUT = 2.0			# seconds for each answer
# The stock firmware's PC mode (programming mode), with the radio on and running its stock firmware: the codeplug is
# read before the install, since RadioFoundry no longer reads the stock tables (docs/spec/codeplug-stock-anytone.md)
PROGRAM = b"PROGRAM"
END = b"END"
READ = 0x52			# 'R' + address (4 B big-endian) + count (1 B)
READ_REPLY = 0x57		# 'W' + address + count + data + sum + 06
READ_MAX = 16			# bytes per read, as the vendor's programming software asks for them
SPI_HEADER = 14			# chunk size, block count, payload length, 0
SPI_MODEL_LEN = 8
SPI_MODEL2 = SPI_HEADER + 12	# the second model name: after the first and its 4-byte version
CDI_ENTRY = 278			# a 256-byte name, then address, length and offset (LE words) and 10 more bytes


class UpdateError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------
# the protocol

def frame(addr, chunk):
    """One data frame: 01, the address (LE), 32 bytes (zero padded), the 16-bit LE sum of bytes 1..36, 06"""
    if len(chunk) > CHUNK:
        raise ValueError("a frame carries at most %d bytes" % CHUNK)
    body = struct.pack("<I", addr) + bytes(chunk).ljust(CHUNK, b"\x00")
    return bytes([DATA]) + body + struct.pack("<H", sum(body) & 0xFFFF) + bytes([ACK])


def frames_needed(n):
    return (n + CHUNK - 1) // CHUNK


def parse_identify(reply):
    """(model, hardware byte, version) from the 16-byte answer to 02: 'I', the model (7 bytes, NUL padded), the
    hardware byte, the version (4 bytes), 00 00 06"""
    if len(reply) != IDENTIFY_LEN or reply[0:1] != b"I" or reply[-1] != ACK:
        raise UpdateError("the radio's identity answer is malformed: %s" % (bytes(reply).hex() or "nothing"))
    model = reply[1:8].split(b"\x00")[0].decode("ascii", "replace").strip()
    return model, reply[8], reply[9:13].decode("ascii", "replace")


# ---------------------------------------------------------------------------------------------------------------
# which radio, which image

def variant_of(board, model):
    """The [hardware.variants] code whose model names include the one the radio gave, or None"""
    for code, names in board.anytone_models.items():
        if model in names:
            return code
    return None


def _named(board, code):
    return "%s (%s)" % (code, board.variant_name(code))


def check_radio(board, model, chosen=None):
    """The variant code of the radio that answered `model`. UpdateError when it isn't this board's radio, or when
    the user named a variant (chosen) and the radio is another one."""
    got = variant_of(board, model)
    if got is None:
        raise UpdateError("the radio answers model %r, which is not a %s (known: %s): refusing"
                          % (model, board.name, ", ".join(n for v in board.anytone_models.values() for n in v)))
    if chosen and chosen != got:
        raise UpdateError("you said the radio is %s, but it answers model %r: it is %s. Refusing: check the radio "
                          "and run again" % (_named(board, chosen), model, _named(board, got)))
    return got


def check_install(board, model, chosen=None):
    """check_radio, and the variant must run this board's image: an Artery radio of a GD32 board is refused (the
    vendor's updater refuses a file for the other MCU too)"""
    got = check_radio(board, model, chosen)
    runs = board.variant_boards.get(got)
    if runs is not None and runs != board.id:
        raise UpdateError("the radio is %s, which takes the %s image, not the %s one: nothing written. Run again "
                          "with board %s" % (_named(board, got), runs, board.id, runs))
    if runs != board.id:
        raise UpdateError("the radio is %s, which RadioFoundry has no image for (the %s image is for another MCU): "
                          "nothing written. Its own stock firmware can still be written back" % (_named(board, got),
                                                                                                 board.name))
    return got


def check_restore(board, model, spi_models, chosen=None):
    """check_radio, and the vendor set's .spi must name the radio's model, as the vendor's updater checks"""
    got = check_radio(board, model, chosen)
    if model not in spi_models:
        raise UpdateError("this firmware is for %s, the radio answers %r (%s): refusing, as the vendor's updater "
                          "does" % (" or ".join(sorted(spi_models)) or "no model", model, _named(board, got)))
    return got


def spi_models(spi):
    """The model names a vendor .spi file names (its two 8-byte fields), without blanks"""
    out = set()
    for at in (SPI_HEADER, SPI_MODEL2):
        name = bytes(spi[at:at + SPI_MODEL_LEN]).split(b"\x00")[0].decode("ascii", "replace").strip()
        if name:
            out.add(name)
    return out


def stock_set(cdd, cdi, spi):
    """(address, image, model names) of a vendor firmware set's three files' bytes"""
    if len(cdi) != CDI_ENTRY:
        raise UpdateError("the .CDI holds %s, not one firmware image" % ("%d entries" % (len(cdi) // CDI_ENTRY)
                                                                        if len(cdi) % CDI_ENTRY == 0 else
                                                                        "%d bytes" % len(cdi)))
    addr, length, offset = struct.unpack_from("<III", cdi, 256)
    data = bytes(cdd[offset:offset + length])
    if len(data) != length or not length:
        raise UpdateError("the .CDD is shorter than its .CDI says (%d bytes from %d)" % (length, offset))
    models = spi_models(spi)
    if not models:
        raise UpdateError("the .spi names no radio model")
    return addr, data, models


def sibling(path, ext):
    """The file beside `path` with extension ext (either case)"""
    base = os.path.splitext(path)[0]
    for e in (ext.lower(), ext.upper()):
        if os.path.isfile(base + e):
            return base + e
    raise UpdateError("%s needs its %s file beside it (the vendor's firmware comes as .CDD, .CDI and .spi)"
                      % (path, ext.upper()))


def read_stock(path):
    """(address, image, model names) of a vendor firmware set: the .CDD, with the .CDI (its load address) and the .spi
    (the model names it is for) of the same name beside it"""
    with open(path, "rb") as f:
        cdd = f.read()
    with open(sibling(path, ".cdi"), "rb") as f:
        cdi = f.read()
    with open(sibling(path, ".spi"), "rb") as f:
        spi = f.read()
    return stock_set(cdd, cdi, spi)


def check_fits(board, addr, data):
    if addr != board.app_base or not data or addr + len(data) > board.flash_end:
        raise UpdateError("the image (%d bytes at 0x%08X) doesn't fit the application area 0x%08X..0x%08X"
                          % (len(data), addr, board.app_base, board.flash_end))


def boot_slot(board):
    """(start, end) of the board's modem boot slot"""
    if not board.codec_slot:
        raise UpdateError("%s has no modem boot slot" % board.name)
    return board.codec_slot


def split_image(board, image):
    """(application, boot block or None) of an image from the application base: one with the boot block merged in
    (tools/modem_boot.py) carries it, checked by its hash, at the slot"""
    slot = boot_slot(board)[0] - board.app_base
    blk = bytes(image[slot:slot + modemboot.BOOT_LEN])
    if len(blk) == modemboot.BOOT_LEN and hashlib.sha256(blk).hexdigest() == modemboot.BOOT_SHA256:
        return bytes(image[:slot]).rstrip(b"\xff"), blk
    return bytes(image), None


def install_image(board, app, block):
    """What the update mode writes for RadioFoundry: the application, 0xFF up to the slot, the boot block"""
    start, end = boot_slot(board)
    try:
        return modemboot.merge(app, board.app_base, start, end, block)
    except modemboot.ModemBootError as e:
        raise UpdateError(str(e))


# ---------------------------------------------------------------------------------------------------------------
# the radio

class Radio:
    """The update mode on a serial port (or anything with write, read and reset_input_buffer)"""

    def __init__(self, port, log=print, timeout=None, retries=RETRIES):
        self.port = port
        self.log = log
        self.timeout = TIMEOUT if timeout is None else timeout
        self.retries = retries

    def _read(self, n):
        end = time.monotonic() + self.timeout
        out = b""
        while len(out) < n and time.monotonic() < end:
            out += self.port.read(n - len(out))
        return out

    def hello(self):
        self.port.reset_input_buffer()
        self.port.write(HELLO)
        r = self._read(len(HELLO_REPLY))
        if r != HELLO_REPLY:
            raise UpdateError("no answer to UPDATE (%s): is the radio in update mode (%s)?"
                              % (r.hex() or "nothing", "the board's keys held at power-on"))

    def identify(self):
        self.port.write(IDENTIFY)
        return parse_identify(self._read(IDENTIFY_LEN))

    def write(self, addr, data, progress=None, cancelled=None):
        n = frames_needed(len(data))
        for i in range(n):
            if cancelled and cancelled():
                raise UpdateError("stopped after %d of %d frames: the radio stays in update mode, write again" % (i, n))
            f = frame(addr + i * CHUNK, data[i * CHUNK:(i + 1) * CHUNK])
            r = b""
            for _ in range(self.retries):
                self.port.write(f)
                r = self._read(1)
                if r == bytes([ACK]):
                    break
            else:
                raise UpdateError("the radio refused the frame at 0x%08X %d times (%s): the radio stays in update "
                                  "mode, write again" % (addr + i * CHUNK, self.retries,
                                                         "NAK" if r == bytes([NAK]) else r.hex() or "no answer"))
            if progress:
                progress(i + 1, n)
        self.port.write(FINISH)
        if self._read(1) != bytes([ACK]):
            raise UpdateError("the radio didn't confirm the end of the write")
        return n


def read_request(addr, n):
    """One read: 'R', the memory address (big-endian), the count"""
    if not 0 < n <= READ_MAX:
        raise ValueError("a read asks for 1 to %d bytes" % READ_MAX)
    return bytes([READ]) + struct.pack(">IB", addr, n)


def read_sum(b):
    """The 8-bit sum of the bytes after the command byte (address, count, data)"""
    return sum(b) & 0xFF


def read_reply(addr, data):
    """The radio's answer to a read (the fake's, and the tests')"""
    body = struct.pack(">IB", addr, len(data)) + bytes(data)
    return bytes([READ_REPLY]) + body + bytes([read_sum(body), ACK])


def parse_read(addr, n, r):
    """The data of the answer to read_request(addr, n), or UpdateError"""
    if len(r) != n + 8 or r[0] != READ_REPLY or r[-1] != ACK:
        raise UpdateError("the radio's answer to a read at 0x%08X is malformed: %s" % (addr, r.hex() or "nothing"))
    got, cnt = struct.unpack_from(">IB", r, 1)
    if got != addr or cnt != n:
        raise UpdateError("the radio answered a read at 0x%08X with 0x%08X" % (addr, got))
    if read_sum(r[1:-2]) != r[-2]:
        raise UpdateError("bad sum in the answer to a read at 0x%08X" % addr)
    return r[6:-2]


class Program(Radio):
    """The stock firmware's PC mode on a serial port: read its memory, then END (the radio restarts)"""

    def hello(self):
        self.port.reset_input_buffer()
        self.port.write(PROGRAM)
        r = self._read(len(HELLO_REPLY))
        if r != HELLO_REPLY:
            raise UpdateError("no answer to PROGRAM (%s): is the radio on, running its original firmware?"
                              % (r.hex() or "nothing"))

    def read(self, addr, n, progress=None):
        out = bytearray()
        while len(out) < n:
            k = min(READ_MAX, n - len(out))
            a = addr + len(out)
            r = b""
            for _ in range(self.retries):
                self.port.reset_input_buffer()
                self.port.write(read_request(a, k))
                r = self._read(k + 8)
                try:
                    out += parse_read(a, k, r)
                    break
                except UpdateError:
                    continue
            else:
                parse_read(a, k, r)
            if progress:
                progress(len(out), n)
        return bytes(out)

    def end(self):
        """Leave the PC mode; the radio answers, then restarts"""
        self.port.write(END)
        return self._read(len(HELLO_REPLY)) == HELLO_REPLY


class FakeRadio:
    """A simulated update mode for tests: answers the handshake and the identity, keeps what is written. nak_every:
    refuse every n-th frame once; silent_after: stop answering after that many frames"""

    def __init__(self, model="MODEL1", nak_every=0, silent_after=None, memory=None):
        self.model = model
        self.memory = memory or {}	# the PC mode's memory: {address: bytes}, unset bytes read 0xFF
        self.program = False
        self.ended = False
        self.out = b""
        self.inbuf = b""
        self.mem = {}
        self.finished = False
        self.nak_every = nak_every
        self.silent_after = silent_after
        self.frames = 0
        self.bad = 0

    def reset_input_buffer(self):
        self.out = b""

    def read(self, n):
        r, self.out = self.out[:n], self.out[n:]
        return r

    def image(self):
        """The bytes written, from the lowest address, gaps 0xFF"""
        if not self.mem:
            return b""
        lo = min(self.mem)
        out = bytearray(b"\xff" * (max(self.mem) + CHUNK - lo))
        for a, d in self.mem.items():
            out[a - lo:a - lo + CHUNK] = d
        return bytes(out)

    def peek(self, addr, n):
        out = bytearray(b"\xff" * n)
        for a, d in self.memory.items():
            lo, hi = max(a, addr), min(a + len(d), addr + n)
            if lo < hi:
                out[lo - addr:hi - addr] = d[lo - a:hi - a]
        return bytes(out)

    def write(self, b):
        self.inbuf += bytes(b)
        while self.inbuf:
            if self.inbuf.startswith(PROGRAM):
                self.inbuf = self.inbuf[len(PROGRAM):]
                self.program = True
                self.out += HELLO_REPLY
            elif self.program and self.inbuf.startswith(END):
                self.inbuf = self.inbuf[len(END):]
                self.ended = True
                self.out += HELLO_REPLY
            elif self.program and self.inbuf[:1] == bytes([READ]):
                if len(self.inbuf) < 6:
                    return
                addr, n = struct.unpack_from(">IB", self.inbuf, 1)
                self.inbuf = self.inbuf[6:]
                self.out += read_reply(addr, self.peek(addr, n))
            elif self.inbuf.startswith(HELLO):
                self.inbuf = self.inbuf[len(HELLO):]
                self.out += HELLO_REPLY
            elif self.inbuf[:1] == IDENTIFY:
                self.inbuf = self.inbuf[1:]
                self.out += b"I" + self.model.encode().ljust(7, b"\x00")[:7] + b"\x01V100\x00\x00\x06"
            elif self.inbuf[:1] == FINISH:
                self.inbuf = self.inbuf[1:]
                self.finished = True
                self.out += bytes([ACK])
            elif self.inbuf[:1] == bytes([DATA]):
                if len(self.inbuf) < FRAME_LEN:
                    return
                f, self.inbuf = self.inbuf[:FRAME_LEN], self.inbuf[FRAME_LEN:]
                self.frames += 1
                if self.silent_after is not None and self.frames > self.silent_after:
                    continue
                if self.nak_every and self.frames % self.nak_every == 0:
                    self.out += bytes([NAK])
                    continue
                ok = struct.unpack_from("<H", f, 37)[0] == sum(f[1:37]) & 0xFFFF and f[39] == ACK
                if ok:
                    self.mem[struct.unpack_from("<I", f, 1)[0]] = f[5:37]
                else:
                    self.bad += 1
                self.out += bytes([ACK if ok else NAK])
            else:
                self.inbuf = self.inbuf[1:]	# noise: the update mode ignores a byte it doesn't know


def open_port(port):
    """The radio's USB serial port at the update mode's rate: `port`, or the only USB serial port there is"""
    try:
        import serial
    except ImportError:
        raise UpdateError("needs pyserial (pip install pyserial)")
    if port is None:
        from serial.tools import list_ports
        cands = [p.device for p in list_ports.comports() if p.vid is not None]
        if len(cands) != 1:
            raise UpdateError("pass --port: %s" % (", ".join(cands) if cands else "no USB serial port found"))
        port = cands[0]
    return serial.Serial(port, BAUD, timeout=0.05)
