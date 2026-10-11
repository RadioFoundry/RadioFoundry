# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Just enough of the debug link (docs/spec/debuglink.md) to ask a running ODR radio what it is: VERSION (board,
git, codec=) and INFO_CHIP (the chip UID, to tell radios apart), and READ_MEM for the bootloader key (bootkey.py).
Read only: no command here changes the radio.
Needs pyserial; without it, probing is skipped.
"""
import struct
import time

VID, PID = 0x1FC9, 0x0094
SOF1, SOF2 = 0xA5, 0x5A
CMD_VERSION, CMD_INFO_CHIP, CMD_READ_MEM = 0x02, 0x0B, 0x0C


class LinkError(Exception):
    pass


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE"""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(cmd, seq, payload=b""):
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF1, SOF2]) + body + struct.pack("<H", crc16(body))


def parse_frames(buf):
    """Yield (cmd, seq, payload) for each complete, CRC-valid frame in buf (bytes)."""
    i = 0
    while i + 8 <= len(buf):
        if buf[i] != SOF1 or buf[i + 1] != SOF2:
            i += 1
            continue
        n = struct.unpack_from("<H", buf, i + 4)[0]
        end = i + 6 + n + 2
        if end > len(buf):
            return
        body = buf[i + 2:i + 6 + n]
        if struct.unpack_from("<H", buf, i + 6 + n)[0] == crc16(body):
            yield buf[i + 2], buf[i + 3], bytes(buf[i + 6:i + 6 + n])
            i = end
        else:
            i += 1


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def find_ports():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    return [p.device for p in list_ports.comports() if p.vid == VID and p.pid == PID]


class Link:
    def __init__(self, port, timeout=1.0):
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.timeout = timeout
        self.seq = 0x40

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def request(self, cmd, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.s.reset_input_buffer()
        self.s.write(frame(cmd, self.seq, payload))
        buf = bytearray()
        end = time.monotonic() + self.timeout
        while time.monotonic() < end:
            buf += self.s.read(1100)
            for rcmd, rseq, data in parse_frames(bytes(buf)):
                if rseq == self.seq and rcmd in (cmd | 0x80, 0xFF):
                    if not data or data[0] != 0:
                        raise LinkError("command 0x%02X: status %s" % (cmd, data[0] if data else "?"))
                    return data[1:]
        raise LinkError("no answer to command 0x%02X" % cmd)

    def version(self):
        return parse_kv(self.request(CMD_VERSION).decode("ascii", "replace"))

    def read_mem(self, addr, size):
        """READ_MEM in 1000-byte pieces (internal flash, the stock bootloader included)."""
        out = bytearray()
        while len(out) < size:
            n = min(1000, size - len(out))
            r = self.request(CMD_READ_MEM, struct.pack("<IH", addr + len(out), n))
            if len(r) != 4 + n or struct.unpack_from("<I", r)[0] != addr + len(out):
                raise LinkError("READ_MEM answer doesn't match 0x%08X" % (addr + len(out)))
            out += r[4:]
        return bytes(out)

    def chip_uid(self):
        """The 96-bit chip UID as hex, or None (INFO_CHIP: idcode u32, flash_kb u16, uid 12 bytes, ...)."""
        try:
            r = self.request(CMD_INFO_CHIP)
        except LinkError:
            return None
        return r[6:18].hex() if len(r) >= 18 else None


def probe(port=None, log=None):
    """Ask a running ODR radio for its identity. Returns a dict (board, git, codec, features, uid, port) or None when
    no radio answers. Never raises for a missing radio or a missing pyserial."""
    ports = [port] if port else find_ports()
    if not ports:
        return None
    for p in ports:
        try:
            link = Link(p)
        except Exception as e:
            if log:
                log("could not open %s: %s" % (p, e))
            continue
        try:
            v = link.version()
            uid = link.chip_uid()
        except (LinkError, OSError) as e:
            if log:
                log("%s: no debug link answer (%s)" % (p, e))
            continue
        finally:
            link.close()
        return {"port": p, "board": v.get("board"), "git": v.get("git"), "codec": v.get("codec"),
                "features": v.get("features", ""), "uid": uid}
    return None
