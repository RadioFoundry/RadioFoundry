#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Read and write the radio's codeplug over the open-firmware CPS protocol (docs/spec/open-firmware-cps.md, our shim in
docs/spec/cps-protocol.md 2), the way qdmr's open-firmware UV380 driver does it.

  cps_client.py info                                 radio info (type, git, build date, flash)
  cps_client.py read image.bin [--regions all]       read into a flat image (file offset = flash address)
  cps_client.py write image.bin [--no-reboot]        write the regions back (prepare / data / commit per sector)
  cps_client.py verify image.bin                     read back and compare
  cps_client.py test-channels image.bin              put an FM and a DMR test channel (and a zone) into an image

Transport: the radio's USB serial port (--port, $ODR_PORT, or the only 1FC9:0094 device), or a command speaking
the protocol on stdin / stdout (--exec "tests/host/cpsSim --populate", for tests). The DM-32UV and the DB25-D PRO have
no USB: give the programming cable's port with --port (for example /dev/ttyUSB1; needs pyserial). The DB25-D's image
sits at the STM32 addresses, so it takes no --offset.

--offset ADDR adds ADDR to every flash address on the wire. The DM-32UV keeps the open-firmware image at +0x700000 in
its data flash (boards/dm32/flash_dm32.c, open-firmware-cps.md 7.4) and its shim takes physical addresses, so it
needs --offset 0x700000; image files keep the STM32 addresses either way.

Regions (wire addresses = flash addresses on the STM32 radios):
  qdmr      what qdmr transfers: EEPROM image 0x80-0x6060 and 0x7500-0xB000, custom data 0x20000-0x211A0,
            codeplug 0x9B000-0xAEE60 (default)
  codeplug  the EEPROM image 0x0-0x10000 and the flash image's codeplug 0x9B000-0xAEE60
  all       the EEPROM image and the whole open-firmware flash image 0x20000-0x120000 (DMR ID database and voice prompts
            included; slow)
Writes never touch bytes outside the regions; the radio refuses its own areas.
"""
import argparse
import os
import select
import shlex
import subprocess
import sys
import time

IMAGE_SIZE = 0x120000
SECTOR = 4096
BLOCK = 32                      # qdmr's block size for reads and writes

REGIONS = {
    "qdmr": [(0x00080, 0x06060), (0x07500, 0x0B000), (0x20000, 0x211A0), (0x9B000, 0xAEE60)],
    "codeplug": [(0x00000, 0x10000), (0x9B000, 0xAEE60)],
    "all": [(0x00000, 0x10000), (0x20000, 0x120000)],
}

REPLY_MAX = 32768      # the longest read reply (cps-protocol.md 2.2)

RADIO_TYPES = {0: "GD-77", 1: "GD-77S", 2: "DM-1801", 3: "RD-5R", 4: "DM-1801A", 5: "MD-9600", 6: "MD-UV380",
               7: "MD-380", 8: "DM-1701 (BGR panel)", 9: "MD-2017", 10: "DM-1701 (RGB panel)", 11: "DM-32UV"}


class CpsError(Exception):
    pass


class SerialTransport:
    def __init__(self, port):
        import serial  # pyserial
        self.s = serial.serial_for_url(port, 115200, timeout=0.05)	# a device, or a URL such as rfc2217://host:port
        self.s.reset_input_buffer()

    def write(self, b):
        self.s.write(b)

    def read(self, n, timeout):
        end = time.monotonic() + timeout
        out = b""
        while len(out) < n and time.monotonic() < end:
            out += self.s.read(n - len(out))
        return out

    def close(self):
        self.s.close()


class ExecTransport:
    def __init__(self, cmd):
        self.p = subprocess.Popen(shlex.split(cmd), stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        os.set_blocking(self.p.stdout.fileno(), False)

    def write(self, b):
        self.p.stdin.write(b)
        self.p.stdin.flush()

    def read(self, n, timeout):
        end = time.monotonic() + timeout
        out = b""
        fd = self.p.stdout.fileno()
        while len(out) < n:
            left = end - time.monotonic()
            if left <= 0 or not select.select([fd], [], [], left)[0]:
                break
            try:
                chunk = os.read(fd, n - len(out))
            except BlockingIOError:
                continue
            if not chunk:
                break
            out += chunk
        return out

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)


def find_port():
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = [p.device for p in list_ports.comports() if p.vid == 0x1FC9 and p.pid == 0x0094]
    return ports[0] if len(ports) == 1 else None


class Cps:
    def __init__(self, transport, timeout=2.0, offset=0):
        if offset % SECTOR:
            raise CpsError(f"offset {offset:#x} is not a multiple of the {SECTOR}-byte sector")
        self.t = transport
        self.timeout = timeout
        self.offset = offset    # added to flash addresses on the wire (DM-32UV: 0x700000)

    def close(self):
        self.t.close()

    def _gap(self):
        time.sleep(0.004)       # the radio ends header-less requests after 3 ms of quiet

    # ── control (the reply is always '-', meaning done) ──
    def control(self, sub, args=b""):
        self.t.write(bytes([0x43, sub]) + args)
        r = self.t.read(1, self.timeout)
        if r != b"-":
            raise CpsError(f"C {sub}: reply {r!r}")

    def text(self, y, s, font=1, align=1, retries=2):
        """Screen text; sent again when the reply is missing (line noise on a programming cable, as for reads):
        drawing it twice is harmless."""
        for _ in range(retries + 1):
            self.t.write(bytes([0x43, 2, 0, y, font, align, 0]) + s.encode("ascii")[:16] + b"\0")
            if self.t.read(1, self.timeout) == b"-":
                return
            self.t.read(64, 0.2)        # drain the rest of a garbled reply
        raise CpsError("C 2: no reply")

    def opening(self, writing):
        self.control(0)
        self.control(1)
        self.text(0, "OpenDigitalRadio")
        self.text(16, "Writing" if writing else "Reading")
        self.control(3)
        self.control(6, bytes([4 if writing else 3]))
        self.control(6, bytes([2]))     # the radio saves its settings / VFOs first

    # ── reads ──
    def read(self, area, addr, n, retries=2):
        """A read is repeated (after the rest of a bad reply has drained) when its reply is missing or malformed, or
        '-': on a programming cable (the DM-1801 Pro's) a burst of line noise now and then spoils a request, and the
        radio refuses the garbled address. A real refusal is refused again and raised."""
        for attempt in range(retries + 1):
            try:
                return self._read_once(area, addr, n)
            except CpsError:
                if attempt == retries:
                    raise
                self._drain()

    def _read_once(self, area, addr, n):
        self.t.write(bytes([0x52, area]) + addr.to_bytes(4, "big") + n.to_bytes(2, "big"))
        h = self.t.read(1, self.timeout)
        if h != b"R":
            raise CpsError(f"R {area} {addr:#x} {n}: reply {h!r}")
        ln = int.from_bytes(self.t.read(2, self.timeout), "big")
        if area in (1, 2) and ln != n and n <= REPLY_MAX:   # flash, EEPROM: as asked (longer: clipped)
            raise CpsError(f"R {area} {addr:#x} {n}: reply length {ln}")
        d = self.t.read(ln, self.timeout)
        if len(d) != ln:
            raise CpsError(f"R {area} {addr:#x}: short reply {len(d)} of {ln}")
        return d

    def _drain(self):
        """Let the radio give up on a spoiled request (CPS_STALL_MS) and throw away what is still coming"""
        time.sleep(0.1)
        while self.t.read(4096, 0.1):
            pass

    def info(self):
        d = self.read(9, 0, 0x20)
        v = {"struct": int.from_bytes(d[0:4], "little"), "radio_type": int.from_bytes(d[4:8], "little"),
             "git": d[8:24].split(b"\0")[0].decode("ascii", "replace"),
             "build": d[24:40].split(b"\0")[0].decode("ascii", "replace"),
             "flash_id": int.from_bytes(d[40:44], "little")}
        if len(d) >= 46:
            v["features"] = int.from_bytes(d[44:46], "little")
        v["model"] = RADIO_TYPES.get(v["radio_type"], "?")
        return v

    def read_image(self, regions, image=None, block=BLOCK, progress=None):
        img = image if image is not None else bytearray(b"\xff" * IMAGE_SIZE)
        self.opening(False)
        total = sum(e - s for s, e in regions)
        done = 0
        for start, end in regions:
            a = start
            while a < end:
                n = min(block, end - a)
                img[a:a + n] = self.read(1, a + self.offset, n)
                a += n
                done += n
                if progress:
                    progress(done, total)
        self.control(5)
        return img

    # ── writes ──
    def _ack(self, what, sub):
        r = self.t.read(2, self.timeout * 3)   # a commit erases and programs (and journals) a sector
        if r != bytes([0x58, sub]):
            raise CpsError(f"{what}: reply {r!r}")

    def write_sector(self, img, sector, start, end, block=BLOCK, retries=2):
        """Write one sector's part of img, read it back and compare; on a bad reply or a difference the whole sector
        is drained and done again (a burst of line noise on the DM-1801 Pro's cable spoils a request now and then, and
        noise inside a data block can change its bytes without an error)"""
        for attempt in range(retries + 1):
            try:
                self._write_sector_once(img, sector, start, end, block)
                a, b = max(sector * SECTOR, start), min((sector + 1) * SECTOR, end)
                if same_but_vfo(self.read(1, a + self.offset, b - a), img[a:b], a):
                    return
                err = CpsError(f"sector {sector:#x}: read back differs")
            except CpsError as e:
                err = e
            if attempt == retries:
                raise err
            self._drain()

    def _write_sector_once(self, img, sector, start, end, block):
        base = sector * SECTOR
        wire = sector + self.offset // SECTOR
        prepare = bytes([0x58, 1]) + wire.to_bytes(3, "big")
        self.t.write(prepare)
        r = self.t.read(2, self.timeout)
        if r == b"-":                   # a sector left prepared by a spoiled attempt: that X 1 dropped it, try again
            self.t.write(prepare)
            r = self.t.read(2, self.timeout)
        if r != bytes([0x58, 1]):
            raise CpsError(f"prepare sector {wire:#x}: reply {r!r}")
        a = max(base, start)
        while a < min(base + SECTOR, end):
            n = min(block, min(base + SECTOR, end) - a)
            self.t.write(bytes([0x58, 2]) + (a + self.offset).to_bytes(4, "big") + n.to_bytes(2, "big") +
                         bytes(img[a:a + n]))
            self._ack(f"data {a:#x}", 2)
            a += n
        self.t.write(bytes([0x58, 3]))
        self._ack(f"commit sector {sector:#x}", 3)

    def write_image(self, img, regions, reboot=True, block=BLOCK, progress=None, base=None):
        """Write the regions of img; with base (what the radio holds now) only the sectors that differ"""
        self.opening(True)
        jobs = plan_write(img, regions, base)
        for i, (sector, start, end) in enumerate(jobs):
            self.write_sector(img, sector, start, end, block)
            if progress:
                progress(i + 1, len(jobs))
        self.control(5)
        if reboot:
            self.control(6, bytes([0]))


VFO_A = (0x7590, 0x7590 + 56)   # the VFO A record: a read shows the live VFO there (cps-protocol.md 2.2)


def same_but_vfo(got, want, at):
    """got == want for the bytes at image address at.., the VFO A record aside"""
    return all(got[i] == want[i] for i in range(len(want)) if not VFO_A[0] <= at + i < VFO_A[1])


def plan_write(img, regions, base=None):
    """The (sector, region start, region end) jobs write_image sends for img: every sector part of a region, or with
    base only those whose bytes differ from it. Each job writes img's bytes of that sector inside the region."""
    jobs = []
    for start, end in regions:
        for sector in range(start // SECTOR, (end + SECTOR - 1) // SECTOR):
            lo, hi = max(start, sector * SECTOR), min(end, (sector + 1) * SECTOR)
            if base is None or bytes(img[lo:hi]) != bytes(base[lo:hi]):
                jobs.append((sector, start, end))
    return jobs


def apply_plan(target, img, jobs):
    """What the radio would hold after write_image's jobs: img's bytes copied into target (a dry run)."""
    for sector, start, end in jobs:
        lo, hi = max(start, sector * SECTOR), min(end, (sector + 1) * SECTOR)
        target[lo:hi] = img[lo:hi]
    return target


# ── the open-firmware channel records (docs/spec/open-firmware-cps.md), enough for tests to find and edit a channel ──
CHANNEL_SIZE = 56
CHANNELS_PER_BANK = 128


def channel_addr(number):
    """Flash address of channel number (1-1024): bank 0 in the EEPROM image, banks 1-7 in the flash image"""
    i = number - 1
    bank = i // CHANNELS_PER_BANK
    bitmap = 0x3780 if bank == 0 else 0x9B1B0 + (bank - 1) * (16 + CHANNELS_PER_BANK * CHANNEL_SIZE)
    return bitmap, bitmap + 16 + (i % CHANNELS_PER_BANK) * CHANNEL_SIZE


def _bcd(b):
    v = 0
    for byte in reversed(b):
        hi, lo = byte >> 4, byte & 15
        if hi > 9 or lo > 9:
            return None
        v = v * 100 + hi * 10 + lo
    return v


def decode_channel(img, number):
    """{name, rx_hz, tx_hz} of a channel in an image, None when its bit in the bank bitmap is clear"""
    bitmap, a = channel_addr(number)
    i = (number - 1) % CHANNELS_PER_BANK
    if not img[bitmap + i // 8] & (1 << (i % 8)):
        return None
    rec = bytes(img[a:a + CHANNEL_SIZE])
    rx, tx = _bcd(rec[0x10:0x14]), _bcd(rec[0x14:0x18])
    return {"name": rec[0:16].split(b"\xff")[0].split(b"\0")[0].decode("ascii", "replace"),
            "rx_hz": rx * 10 if rx is not None else None, "tx_hz": tx * 10 if tx is not None else None}


def set_channel_name(img, number, name):
    _, a = channel_addr(number)
    raw = name.encode("ascii")[:16]
    img[a:a + 16] = raw + b"\xff" * (16 - len(raw))


CH_FLAG1 = 0x30                 # talker alias per timeslot: bits 1 (TS1 text) and 3 (TS2 text) (core/codeplug)


def set_channel_alias(img, number, ts1=True, ts2=False):
    """Turn on the channel's talker alias TX (our radio name as text) on TS1 / TS2."""
    _, a = channel_addr(number)
    img[a + CH_FLAG1] = (img[a + CH_FLAG1] & ~0x0A) | (0x02 if ts1 else 0) | (0x08 if ts2 else 0)


def set_channel_tx(img, number, hz):
    """Set the channel's TX frequency (a split channel when it differs from RX)."""
    _, a = channel_addr(number)
    img[a + 0x14:a + 0x18] = _bcd_freq(hz)


# ── a small test codeplug: one FM and one DMR channel, a contact, a zone ──
CONTACT_BASE = 0xA7620          # contact 1 (flash image 0x87620 + the 128 KB offset), 24 bytes each
ZONE_BITMAP = 0x8010
ZONE_DATA = 0x8030              # slot 0, 80-channel layout: 16 name bytes, 80 channel numbers (LE)
GENERAL = 0xE0                  # callsign (8) at +0, DMR ID (8 BCD digits, big endian) at +8
BANK0_BITMAP = 0x3780


def _bcd_freq(hz):
    """A frequency as the channel record holds it: BCD of 10 Hz units, a little endian 32-bit word"""
    units = hz // 10
    if not 0 < units < 10 ** 8:
        raise CpsError(f"frequency {hz} Hz out of range")
    return int(str(units), 16).to_bytes(4, "little")


def _bcd_id(n):
    return int(f"{n:08d}", 16).to_bytes(4, "big")


def _channel_record(name, rx_hz, tx_hz, dmr, cc=1, ts=1, contact=0, wide=False):
    r = bytearray(CHANNEL_SIZE)
    raw = name.encode("ascii")[:16]
    r[0:16] = raw + b"\xff" * (16 - len(raw))
    r[0x10:0x14] = _bcd_freq(rx_hz)
    r[0x14:0x18] = _bcd_freq(tx_hz)
    r[0x18] = 1 if dmr else 0
    r[0x20:0x24] = b"\xff\xff\xff\xff"      # no tones
    r[0x28] = 0x16                          # the vendor default bytes (open-firmware-cps.md 7.3.3)
    r[0x2C] = cc if dmr else 0
    r[0x2E:0x30] = contact.to_bytes(2, "little")
    r[0x31] = 0x40 if dmr and ts == 2 else 0
    r[0x33] = 0x02 if wide else 0           # bandwidth bit: set = 25 kHz
    return r


def make_test_codeplug(img, rx_mhz=461.4625, tg=201, cc=2, ts=1, dmr_id=None, callsign=None):
    """Edit img (a flat image, file offset = flash address) so the radio has channel 1 (FM, 12.5 kHz, simplex), channel
    2 (DMR on the same frequency, color code cc, timeslot ts, talk target group tg = contact 1) and zone 1 holding both.
    Everything else in img stays; zone slot 0 and contact 1 are replaced. dmr_id and callsign set the radio's own."""
    hz = round(rx_mhz * 1e6)
    for number, rec in ((1, _channel_record(f"ODR FM {rx_mhz:.4f}", hz, hz, False)),
                        (2, _channel_record(f"ODR DMR TG{tg}", hz, hz, True, cc, ts, contact=1))):
        _, a = channel_addr(number)
        img[a:a + CHANNEL_SIZE] = rec
        i = number - 1
        img[BANK0_BITMAP + i // 8] |= 1 << (i % 8)
    name = f"TG {tg}".encode("ascii")[:16]
    img[CONTACT_BASE:CONTACT_BASE + 24] = (name + b"\xff" * (16 - len(name)) + _bcd_id(tg) +
                                           bytes([0, 0, 0, 0xFF]))
    zone = b"ODR test".ljust(16, b"\xff") + (1).to_bytes(2, "little") + (2).to_bytes(2, "little") + bytes(156)
    img[ZONE_DATA:ZONE_DATA + len(zone)] = zone     # the zeros after the list also select the 80-channel layout
    img[ZONE_BITMAP] |= 1
    if callsign:
        raw = callsign.encode("ascii")[:8]
        img[GENERAL:GENERAL + 8] = raw + b"\xff" * (8 - len(raw))
    if dmr_id:
        img[GENERAL + 8:GENERAL + 12] = _bcd_id(dmr_id)
    return img


def open_cps(port=None, exec_cmd=None, timeout=2.0, offset=0):
    if exec_cmd:
        return Cps(ExecTransport(exec_cmd), timeout, offset)
    port = port or os.environ.get("ODR_PORT") or find_port()
    if not port:
        raise CpsError("no radio found (1FC9:0094): give --port")
    return Cps(SerialTransport(port), timeout, offset)


def load_image(path):
    with open(path, "rb") as f:
        d = bytearray(f.read())
    if len(d) < IMAGE_SIZE:
        d += b"\xff" * (IMAGE_SIZE - len(d))
    return d


def compare(a, b, regions, skip=()):
    """Differences of b against a inside the regions, as (address, a byte, b byte); the ranges in skip left out"""
    diffs = []
    for start, end in regions:
        for addr in range(start, end):
            if any(s <= addr < e for s, e in skip):
                continue
            if a[addr] != b[addr]:
                diffs.append((addr, a[addr], b[addr]))
    return diffs


def bar(done, total):
    sys.stderr.write(f"\r{done * 100 // max(total, 1):3d}%")
    if done >= total:
        sys.stderr.write("\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--exec", dest="exec_cmd", help="command speaking the protocol on stdin / stdout (tests)")
    ap.add_argument("--regions", choices=sorted(REGIONS), default="qdmr")
    ap.add_argument("--block", type=int, default=BLOCK, help="bytes per read / data request (qdmr: 32)")
    ap.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                    help="added to flash addresses on the wire (DM-32UV: 0x700000)")
    ap.add_argument("--quiet", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    r = sub.add_parser("read")
    r.add_argument("image")
    w = sub.add_parser("write")
    w.add_argument("image")
    w.add_argument("--no-reboot", action="store_true", help="don't send save-and-reboot at the end")
    v = sub.add_parser("verify")
    v.add_argument("image")
    t = sub.add_parser("test-channels", help="edit an image (a new blank one if the file is missing): an FM and a DMR "
                       "test channel, a talk group contact and a zone")
    t.add_argument("image")
    t.add_argument("--rx", type=float, default=461.4625, help="MHz, simplex (default 461.4625)")
    t.add_argument("--tg", type=int, default=201)
    t.add_argument("--cc", type=int, default=2)
    t.add_argument("--ts", type=int, choices=(1, 2), default=1)
    t.add_argument("--id", type=int, help="the radio's DMR ID")
    t.add_argument("--call", help="the radio's name / callsign")
    a = ap.parse_args(argv)

    if a.cmd == "test-channels":
        img = load_image(a.image) if os.path.exists(a.image) else bytearray(b"\xff" * IMAGE_SIZE)
        make_test_codeplug(img, a.rx, a.tg, a.cc, a.ts, a.id, a.call)
        with open(a.image, "wb") as f:
            f.write(img)
        print(f"{a.image}: channel 1 FM and channel 2 DMR (TG {a.tg}, CC {a.cc}, TS {a.ts}) on {a.rx:.4f} MHz, zone 1")
        return 0
    regions = REGIONS[a.regions]
    progress = None if a.quiet else bar
    try:
        cps = open_cps(a.port, a.exec_cmd, offset=a.offset)
    except CpsError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    try:
        if a.cmd == "info":
            for k, val in cps.info().items():
                print(f"{k}={val:#x}" if k == "flash_id" else f"{k}={val}")
        elif a.cmd == "read":
            img = cps.read_image(regions, block=a.block, progress=progress)
            with open(a.image, "wb") as f:
                f.write(img)
            print(f"read {sum(e - s for s, e in regions)} bytes into {a.image}")
        elif a.cmd == "write":
            img = load_image(a.image)
            cps.write_image(img, regions, reboot=not a.no_reboot, block=a.block, progress=progress)
            print(f"wrote {a.image}" + ("" if a.no_reboot else "; the radio restarts"))
        elif a.cmd == "verify":
            img = load_image(a.image)
            back = cps.read_image(regions, block=a.block, progress=progress)
            d = compare(img, back, regions)
            for addr, x, y in d[:20]:
                print(f"{addr:#08x}: file {x:#04x} radio {y:#04x}")
            print("match" if not d else f"{len(d)} bytes differ")
            return 0 if not d else 1
    except CpsError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        cps.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
