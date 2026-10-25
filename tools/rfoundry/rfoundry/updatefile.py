# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The SGL update file (.sgl) a vendor loader takes over USB HID (hidboot.py): read one into the flat flash image, and
write an image into a copy of a file the user already has (docs/spec/update-gd77.md).

An update file is a 1 KB header, then the flash image and one zero byte, byte-scrambled. The header names the
company, model and version the radio's loader compares, the address and length fields, and the sum check. A new
image keeps the header of the file it is written into (the donor): same names, same tag, same stream; only the body
and the sum check change. A donor is any update file of the same radio; its own tail, which is zero before
scrambling, shows the stream, so no key is needed.

Unverified on a radio: whether the loader accepts a body written this way (docs/spec/update-gd77.md section 5).
"""
import struct

PERIOD = 32767
KEY = b"SGL!"
INFO_LEN = 103
BODY_AT = 0x400
FLASH_BASE = 0x4000


class UpdateFileError(Exception):
    pass


def _ror3(b):
    return ((b >> 3) | (b << 5)) & 255


def _rol3(b):
    return ((b << 3) | (b >> 5)) & 255


class UpdateFile:
    def __init__(self, data):
        self.data = d = bytes(data)
        if len(d) < BODY_AT + 64 or d[:4] != b"SGL!":
            raise UpdateFileError("not an SGL update file (no \"SGL!\" header)")
        h = bytearray(d[:16])
        for i in range(4, 16):
            h[i] ^= KEY[i % 4]
        if bytes(h[4:11]) != b"ENCV001":
            raise UpdateFileError("an update file of another version (header %r)" % bytes(h[4:11]))
        self.pad, self.info_at, self.k0, self.k1 = h[11], h[12] | (h[13] << 8), h[14], h[15]
        self.body_at = BODY_AT + self.pad
        if self.info_at + INFO_LEN > BODY_AT or self.body_at >= len(d):
            raise UpdateFileError("damaged update file (header fields out of range)")
        self.info = bytes(d[self.info_at + i] ^ (self.k0 if i % 2 == 0 else self.k1) for i in range(INFO_LEN))
        self.body = d[self.body_at:]
        if struct.unpack_from("<I", self.info, 10)[0] != (~sum(self.body)) & 0xFFFFFFFF:
            raise UpdateFileError("the file's sum check does not match its contents")

    start = property(lambda s: struct.unpack_from("<I", s.info, 2)[0])
    length = property(lambda s: struct.unpack_from("<I", s.info, 6)[0])	# bytes the loader writes, the zero byte included
    company = property(lambda s: bytes(s.info[50:66]))
    model = property(lambda s: bytes(s.info[66:74]))
    version = property(lambda s: bytes(s.info[74:82]))
    tag = property(lambda s: bytes(s.info[95:103]))
    addr_width = property(lambda s: s.info[91] >> 4)
    len_width = property(lambda s: (s.info[92] >> 3) & 3)
    block_size = property(lambda s: struct.unpack_from("<H", s.info, 93)[0])
    use_fprog = property(lambda s: bool(s.info[91] & 1))
    send_tag = property(lambda s: bool(s.info[92] & 0x20))

    @property
    def room(self):
        """Bytes of flash image this file holds; the last 32 body bytes are zero (they show the stream), so an image
        longer than room - 32 must end in zeros."""
        return self.length - 1 - self.start

    def label(self):
        t = lambda b: bytes(c for c in b if 32 <= c < 127).decode()
        return "%s %s %s" % (t(self.company), t(self.model), t(self.version))


def _forward(seed, n):
    out = list(seed)
    while len(out) < n:
        j = len(out)
        out.append(out[j - 1] ^ out[j - 8] ^ out[j - 9] ^ out[j - 15] ^ out[j - 16])
    return out


def _stream(u):
    """(anchor, stream): the scrambling bytes, from the file's own zero tail."""
    tail = u.data[-32:]
    if _forward(tail[:16], 32)[16:] != list(tail[16:]):
        raise UpdateFileError("the end of this file is not a clean byte stream: it cannot be used as a donor")
    return len(u.data) - 32, _forward(tail[:16], PERIOD)


def unpack(data):
    """The flat flash image (flash address = offset + FLASH_BASE) of an update file."""
    u = data if isinstance(data, UpdateFile) else UpdateFile(data)
    anchor, k = _stream(u)
    out = bytes(_rol3(u.data[i] ^ k[(i - anchor) % PERIOD]) for i in range(u.body_at, len(u.data)))
    return out[:-1] if out.endswith(b"\0") else out


def pack(image, donor):
    """An update file with `image` (flat, from FLASH_BASE) as its body, in the header of `donor`."""
    u = donor if isinstance(donor, UpdateFile) else UpdateFile(donor)
    anchor, k = _stream(u)
    total = len(u.data) - u.body_at
    if len(image) > total - 1:
        raise UpdateFileError("the image is %d bytes, the donor file has room for %d" % (len(image), total - 1))
    if any(image[total - 32:]):
        raise UpdateFileError("the image reaches into the last 32 bytes of the donor file, which must stay zero")
    plain = bytes(image) + b"\xff" * max(0, total - 32 - len(image))
    plain += b"\0" * (total - len(plain))
    body = bytes(_ror3(plain[j]) ^ k[(u.body_at + j - anchor) % PERIOD] for j in range(total))
    info = bytearray(u.info)
    struct.pack_into("<I", info, 10, (~sum(body)) & 0xFFFFFFFF)
    head = bytearray(u.data[:u.body_at])
    for i in range(INFO_LEN):
        head[u.info_at + i] = info[i] ^ (u.k0 if i % 2 == 0 else u.k1)
    return bytes(head) + body
