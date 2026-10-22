#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Read and write the radio's codeplug over the fast codeplug protocol (docs/spec/cps-fast.md): the debug link's
CODEPLUG command, in 4 KB blocks, blank blocks skipped, only blocks whose CRC-32 differs moved, LZ4 both ways.

  cps_fast.py info                                   layout, limits, staging
  cps_fast.py read image.bin [--cache old.bin]       read into a flat image; with a cache only the changed blocks move
  cps_fast.py write image.bin [--direct]             write the blocks that differ (staged and committed where the
                                                     radio has a staging area); the radio re-reads its codeplug
  cps_fast.py verify image.bin                       compare by CRC-32, block by block

Image files are the ones tools/cps_client.py keeps (file offset = image offset = the STM32 wire address). Regions
(--regions) as cps_client.py's: qdmr (default), codeplug, all. Whole blocks travel: a write keeps the radio's bytes
of a block outside the regions (it reads such a block first). Transport as cps_client.py: --port (the radio's USB
serial port or a programming cable), $ODR_PORT, the only 1FC9:0094 device, or --exec "tests/host/cpsSim --populate".
A radio without the protocol (CONFIG_CPS_FAST off, older firmware) answers unavailable or unknown: use
cps_client.py.
"""
import argparse
import os
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "stub"))
import cps_client  # noqa: E402  (transports, regions, image files)
import odr_debug  # noqa: E402  (debug link framing)
import stubtool  # noqa: E402  (LZ4 block encoder and decoder)

CMD = 0x1C
OP_INFO, OP_SUMS, OP_READ, OP_BEGIN, OP_BLOCK, OP_DATA, OP_END, OP_COMMIT, OP_STATUS, OP_ABORT = range(10)
FMT_LZ4, FMT_BLANK = 1, 2
MODE_DIRECT, MODE_STAGED = 0, 1
ST_IDLE, ST_OPEN, ST_BLOCK, ST_COMMITTING, ST_DONE, ST_FAILED = range(6)
BLOCK = 4096
BLANK = b"\xff" * BLOCK
REASONS = {1: "outside the codeplug, or a protected block", 2: "the flash can't be written", 3: "no staging area",
           4: "the staging area is full", 5: "the staging area is in use", 6: "wrong state", 7: "CRC mismatch",
           8: "corrupt stream", 9: "flash error"}
VFO_A = cps_client.VFO_A


class FastError(Exception):
    def __init__(self, msg, status=None, reason=None):
        super().__init__(msg)
        self.status = status
        self.reason = reason


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def lz4_encode(block):
    return bytes(stubtool.lz4_encode(bytes(block)))


def blocks_of(regions):
    """Block addresses the regions touch, in order"""
    out = set()
    for s, e in regions:
        out.update(range(s - s % BLOCK, e, BLOCK))
    return sorted(out)


def covers(regions, a):
    """The regions cover all of the block at a"""
    return any(s <= a and a + BLOCK <= e for s, e in regions)


class FastLink:
    def __init__(self, transport, timeout=2.0):
        self.t = transport
        self.timeout = timeout
        self.seq = 0x30
        self.buf = bytearray()
        self.info_ = None
        self.requests = 0
        self.wire = 0           # bytes both ways

    def close(self):
        self.t.close()

    # ── debug link frames ──
    def _frame(self, deadline):
        while True:
            i = self.buf.find(bytes([odr_debug.SOF1, odr_debug.SOF2]))
            if i < 0:
                del self.buf[:-1]
            else:
                del self.buf[:i]
                if len(self.buf) >= 8:
                    n = self.buf[4] | (self.buf[5] << 8)
                    if n > 1024:            # not a frame of ours: look past this start
                        del self.buf[:1]
                        continue
                    if len(self.buf) >= 8 + n:
                        f = bytes(self.buf[:8 + n])
                        if odr_debug.crc16(f[2:6 + n]) == (f[6 + n] | (f[7 + n] << 8)):
                            del self.buf[:8 + n]
                            return f[2], f[3], f[6:6 + n]
                        del self.buf[:1]
                        continue
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.buf += self._read_some(min(left, 0.05))

    def _read_some(self, timeout):
        """The first byte (waiting up to timeout), then whatever else is already there"""
        b = self.t.read(1, timeout)
        if not b:
            return b
        port = getattr(self.t, "s", None)          # pyserial: no waiting for bytes that aren't coming
        if port is not None and hasattr(port, "in_waiting"):
            n = port.in_waiting
            return b + (port.read(n) if n else b"")
        return b + self.t.read(4096, 0.002)

    def request(self, op, payload=b"", timeout=None, tries=3):
        """(status, data after the status) of one CODEPLUG request. Repeated when no answer comes (every op is safe to
        repeat: READ and SUMS change nothing, DATA takes a repeat as a repeat, BLOCK starts the block again)"""
        body = bytes([op]) + bytes(payload)
        for attempt in range(tries):
            self.seq = (self.seq + 1) & 0xFF
            f = odr_debug.frame(CMD, self.seq, body)
            self.t.write(f)
            self.requests += 1
            self.wire += len(f)
            deadline = time.monotonic() + (timeout or self.timeout)
            while True:
                r = self._frame(deadline)
                if r is None:
                    break
                cmd, seq, data = r
                if seq != self.seq:
                    continue
                self.wire += 8 + len(data)
                if cmd == odr_debug.CMD_ERROR:
                    raise FastError(f"op {op}: frame error {data[:1].hex()}", data[0] if data else None)
                if cmd != (CMD | 0x80) or not data:
                    raise FastError(f"op {op}: unexpected reply {cmd:#x}")
                return data[0], bytes(data[1:])
        raise FastError(f"op {op}: no answer")

    def ok(self, op, payload=b"", timeout=None, busy_wait=5.0):
        """The data of a request that must succeed; waits out busy answers, raises on a refusal"""
        end = time.monotonic() + busy_wait
        while True:
            st, d = self.request(op, payload, timeout)
            if st == 3 and time.monotonic() < end:
                time.sleep(0.02)
                continue
            if st == 0:
                return d
            why = d[0] if st == 7 and d else None
            text = odr_debug.STATUS.get(st, st) if why is None else REASONS.get(why, why)
            raise FastError(f"op {op}: {text}", st, why)

    # ── operations ──
    def info(self):
        if self.info_ is None:
            d = self.ok(OP_INFO)
            if len(d) < 26:
                raise FastError("INFO too short")
            v = {"version": d[0], "staging": bool(d[1] & 1), "writable": bool(d[1] & 2),
                 "block": struct.unpack_from("<H", d, 2)[0], "sums_max": d[4], "data_max": d[5],
                 "read_max": struct.unpack_from("<H", d, 6)[0]}
            v["ee_size"], v["flash_start"], v["end"], v["base"] = struct.unpack_from("<IIII", d, 8)
            v["stage_max"] = struct.unpack_from("<H", d, 24)[0]
            if v["block"] != BLOCK:
                raise FastError(f"block size {v['block']}")
            self.info_ = v
        return self.info_

    def sums(self, addrs):
        """{addr: (crc, blank)} for block addresses, asked in runs of up to sums_max"""
        out = {}
        n = self.info()["sums_max"]
        i = 0
        while i < len(addrs):
            k = 1
            while k < n and i + k < len(addrs) and addrs[i + k] == addrs[i] + k * BLOCK:
                k += 1
            d = self.ok(OP_SUMS, struct.pack("<IB", addrs[i], k))
            mask = struct.unpack_from("<H", d, 5)[0]
            for j in range(k):
                out[addrs[i] + j * BLOCK] = (struct.unpack_from("<I", d, 7 + 4 * j)[0], bool(mask >> j & 1))
            i += k
        return out

    def read_block(self, addr):
        stream = bytearray()
        total, fmt, crc = None, None, None
        while total is None or len(stream) < total:
            d = self.ok(OP_READ, struct.pack("<IH", addr, len(stream)))
            a, fmt_, total_, crc_, off = struct.unpack_from("<IBHIH", d, 0)
            if a != addr or off != len(stream) or (total is not None and (crc_, total_) != (crc, total)):
                raise FastError(f"READ {addr:#x}: the block changed while being read")
            fmt, total, crc = fmt_, total_, crc_
            if fmt == FMT_BLANK:
                break
            if len(d) == 14 and len(stream) < total:
                raise FastError(f"READ {addr:#x}: empty window")
            stream += d[14:]
        data = BLANK if fmt == FMT_BLANK else bytes(stubtool.lz4_block(bytes(stream), BLOCK))
        if crc32(data) != crc:
            raise FastError(f"READ {addr:#x}: CRC mismatch")
        return data

    def begin(self, mode):
        return struct.unpack("<H", self.ok(OP_BEGIN, bytes([mode])))[0]

    def write_block(self, addr, data, tries=3):
        """One block: BLOCK, DATA pieces, END. A block that fails on the way (no answer, a corrupt piece) is sent again
        from its start"""
        blank = data == BLANK
        stream = b"" if blank else lz4_encode(data)
        n = self.info()["data_max"]
        for attempt in range(tries):
            try:
                self.ok(OP_BLOCK, struct.pack("<IBHI", addr, FMT_BLANK if blank else FMT_LZ4, len(stream), crc32(data)))
                for off in range(0, len(stream), n):
                    self.ok(OP_DATA, struct.pack("<H", off) + stream[off:off + n])
                self.ok(OP_END, timeout=5.0)
                return len(stream)
            except FastError as e:
                if e.reason in (1, 2, 3, 4, 5, 9) or attempt == tries - 1:
                    raise
        return 0

    def status(self):
        d = self.ok(OP_STATUS)
        return {"state": d[0], "mode": d[1], "blocks": struct.unpack_from("<H", d, 2)[0],
                "copied": struct.unpack_from("<H", d, 4)[0], "reason": d[6]}

    def commit(self, timeout=60.0):
        self.ok(OP_COMMIT)
        end = time.monotonic() + timeout
        while True:
            s = self.status()
            if s["state"] == ST_DONE:
                return s
            if s["state"] != ST_COMMITTING or time.monotonic() > end:
                raise FastError(f"commit failed: {REASONS.get(s['reason'], s['reason'])} "
                                "(a staged commit is finished at the next power on)", reason=s["reason"])
            time.sleep(0.02)

    def abort(self):
        self.request(OP_ABORT)

    # ── images ──
    def read_image(self, regions, cache=None, image=None, progress=None):
        """The blocks the regions touch, into a flat image; blocks blank on the radio aren't sent, and with cache (an
        image read earlier) neither are those whose CRC-32 is the cache's"""
        img = image if image is not None else bytearray(BLANK * (cps_client.IMAGE_SIZE // BLOCK))
        addrs = blocks_of(regions)
        sums = self.sums(addrs)
        moved = 0
        for i, a in enumerate(addrs):
            crc, blank = sums[a]
            if blank:
                img[a:a + BLOCK] = BLANK
            elif cache is not None and len(cache) >= a + BLOCK and crc32(bytes(cache[a:a + BLOCK])) == crc:
                img[a:a + BLOCK] = cache[a:a + BLOCK]
            else:
                img[a:a + BLOCK] = self.read_block(a)
                moved += 1
            if progress:
                progress(i + 1, len(addrs))
        self.moved = moved
        return img

    def plan_write(self, img, regions):
        """[(addr, block)] of what a write sends: each block the regions touch, img's bytes inside the regions over the
        radio's outside them, where its CRC-32 differs from the radio's"""
        addrs = blocks_of(regions)
        sums = self.sums(addrs)
        jobs = []
        for a in addrs:
            if covers(regions, a):
                want = bytes(img[a:a + BLOCK])
            else:
                have = BLANK if sums[a][1] else self.read_block(a)
                merged = bytearray(have)
                for s, e in regions:
                    lo, hi = max(s, a), min(e, a + BLOCK)
                    if lo < hi:
                        merged[lo - a:hi - a] = img[lo:hi]
                want = bytes(merged)
            if crc32(want) != sums[a][0]:
                jobs.append((a, want))
        return jobs

    def write_image(self, img, regions, staged=None, progress=None):
        """Write the blocks that differ. staged None: staged when the radio has a staging area big enough. Returns
        (blocks written, stream bytes)"""
        info = self.info()
        jobs = self.plan_write(img, regions)
        if not jobs:
            return 0, 0
        if staged is None:
            staged = info["staging"] and len(jobs) <= info["stage_max"]
        self.begin(MODE_STAGED if staged else MODE_DIRECT)
        sent = 0
        try:
            for i, (a, data) in enumerate(jobs):
                sent += self.write_block(a, data)
                if progress:
                    progress(i + 1, len(jobs))
        except FastError:
            self.abort()
            raise
        self.commit(timeout=10.0 + 2.0 * len(jobs))
        return len(jobs), sent

    def verify(self, img, regions):
        """Block addresses whose bytes inside the regions differ from img (the VFO A record aside)"""
        bad = []
        sums = self.sums(blocks_of(regions))
        for a, (crc, blank) in sums.items():
            want = bytes(img[a:a + BLOCK])
            if crc32(want) == crc:
                continue
            got = BLANK if blank else self.read_block(a)
            for s, e in regions:
                lo, hi = max(s, a), min(e, a + BLOCK)
                if lo < hi and not cps_client.same_but_vfo(got[lo - a:hi - a], want[lo - a:hi - a], lo):
                    bad.append(a)
                    break
        return bad


def open_link(port=None, exec_cmd=None, timeout=2.0):
    if exec_cmd:
        return FastLink(cps_client.ExecTransport(exec_cmd), timeout)
    port = port or os.environ.get("ODR_PORT") or cps_client.find_port()
    if not port:
        raise FastError("no radio found (1FC9:0094): give --port")
    return FastLink(cps_client.SerialTransport(port), timeout)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--exec", dest="exec_cmd", help="command speaking the protocol on stdin / stdout (tests)")
    ap.add_argument("--regions", choices=sorted(cps_client.REGIONS), default="qdmr")
    ap.add_argument("--quiet", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    r = sub.add_parser("read")
    r.add_argument("image")
    r.add_argument("--cache", help="an image read earlier: blocks with the same CRC-32 are taken from it")
    w = sub.add_parser("write")
    w.add_argument("image")
    w.add_argument("--direct", action="store_true", help="write in place through the journal, no staging")
    v = sub.add_parser("verify")
    v.add_argument("image")
    a = ap.parse_args(argv)
    regions = cps_client.REGIONS[a.regions]
    progress = None if a.quiet else cps_client.bar
    try:
        link = open_link(a.port, a.exec_cmd)
    except (FastError, cps_client.CpsError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    try:
        t0 = time.monotonic()
        if a.cmd == "info":
            for k, val in link.info().items():
                print(f"{k}={val:#x}" if k in ("ee_size", "flash_start", "end", "base") else f"{k}={val}")
        elif a.cmd == "read":
            cache = cps_client.load_image(a.cache) if a.cache else None
            img = link.read_image(regions, cache=cache, progress=progress)
            with open(a.image, "wb") as f:
                f.write(img)
            print(f"read {a.image}: {link.moved} block(s) moved, {link.wire} bytes on the wire, "
                  f"{time.monotonic() - t0:.1f} s")
        elif a.cmd == "write":
            n, sent = link.write_image(cps_client.load_image(a.image), regions, staged=False if a.direct else None,
                                       progress=progress)
            print(f"wrote {a.image}: {n} block(s), {sent} stream bytes, {time.monotonic() - t0:.1f} s")
        elif a.cmd == "verify":
            bad = link.verify(cps_client.load_image(a.image), regions)
            for addr in bad[:20]:
                print(f"block {addr:#08x} differs")
            print("match" if not bad else f"{len(bad)} block(s) differ")
            return 0 if not bad else 1
    except FastError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
