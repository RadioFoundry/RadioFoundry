#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""split_duplex_host: the host side of the split duplex repeater (docs/spec/split-duplex.md): two OpenDigitalRadio
radios, each an MMDVM modem on its own USB port, run as one duplex repeater; the TX half sends the continuous BS
outbound, the RX half receives both inbound slots and labels them from the TX half's slot clock, which this host
relays (time sync, epoch). Reference implementation of the RadioDesk contract (split-duplex.md 8) and bench tool.

    split_duplex_host.py --tx PORT --rx PORT --freq-in MHZ --freq-out MHZ [--cc N] [--power LEVEL] [--seconds S]
        Start both halves (8.1), keep the time sync, the epoch and the AT relay running, print a status line every
        2 s. Upstream (MMDVMHost) is not attached: this is the pair's bring-up and the timing readout.
    split_duplex_host.py --tx PORT --exp VARIANT [--freq-out MHZ] [--cc N] [--seconds 10] [--reg R=V ...]
        BS framing experiment (10.1): sets the variant over the debug link (same port), takes the TX half role,
        keys the outbound with idle bursts and a short LC, toggles SET_AT every 2 s, prints STATUS and the split view.
    split_duplex_host.py --rx PORT --rx-only [--freq-in MHZ] [--cc N] [--seconds S] [--flip-at S]
        RX half alone: an epoch made from its own clock (period nominal), every burst heard printed with its label;
        --flip-at moves the epoch by one slot at that time (every label must flip).
    split_duplex_host.py --info PORT
        GET_VERSION, INFO and STATUS of one radio.

Only the standard MMDVM commands and the vendor command 0xD6 (ODR_SPLIT) are used; a radio without " SPLIT1" in
its GET_VERSION description is refused. Needs pyserial for the radios (pip install pyserial); the classes below
(ClockSync, epoch_for_rx, VirtualStatus, the frame codecs) have no dependencies and are what tests/host/sim/
splitPairTest.py runs against two simulated radios.
"""
import argparse
import os
import re
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench"))
import dmr_cach  # noqa: E402  (tools/bench/dmr_cach.py: the short LC coding)

FRAME_START = 0xE0
GET_VERSION, GET_STATUS, SET_CONFIG, SET_MODE, SET_FREQ = 0x00, 0x01, 0x02, 0x03, 0x04
DMR_DATA1, DMR_LOST1, DMR_DATA2, DMR_LOST2 = 0x18, 0x19, 0x1A, 0x1B
DMR_SHORTLC, DMR_START, DMR_ABORT = 0x1C, 0x1D, 0x1E
ACK, NAK = 0x70, 0x7F
ODR_SPLIT = 0xD6
SUB_INFO, SUB_SET_ROLE, SUB_TIME, SUB_GET_EPOCH, SUB_SET_EPOCH, SUB_SET_AT, SUB_STATUS = range(7)
ROLE_NORMAL, ROLE_TX, ROLE_RX = 0, 1, 2
ROLE_NAMES = {0: "normal", 1: "tx", 2: "rx"}
RESULTS = {0: "ok", 1: "unknown sub-command", 2: "wrong state", 4: "invalid"}
FLAG_EPOCH, FLAG_KEYED, FLAG_COOLDOWN, FLAG_CLOCK = 0x01, 0x02, 0x04, 0x08
CACH_METHODS = {0: "none", 1: "ours", 2: "chip", 3: "unknown"}
SLOT_NS = 30_000_000
WRAP = 1 << 32
SUFFIX_RE = re.compile(rb"\bSPLIT(\d+)\b")


# ── frames ──

def frame(cmd, payload=b""):
    return bytes([FRAME_START, len(payload) + 3, cmd]) + bytes(payload)


def split_frame(sub, data=b""):
    return frame(ODR_SPLIT, bytes([sub]) + bytes(data))


def config_payload(cc, simplex=False):
    """SET_CONFIG: DMR on, duplex flag clear (split-duplex.md 4.1), the colour code"""
    cfg = bytearray(23)
    cfg[0], cfg[1], cfg[2], cfg[3] = (0x80 if simplex else 0x00), 0x02, 10, 2
    cfg[4], cfg[5], cfg[6], cfg[7], cfg[8], cfg[10], cfg[13], cfg[14] = 128, 128, cc, 0, 128, 128, 128, 128
    return bytes(cfg)


def freq_payload(rx_hz, tx_hz, level):
    return b"\x00" + rx_hz.to_bytes(4, "little") + tx_hz.to_bytes(4, "little") + bytes([level]) + \
        rx_hz.to_bytes(4, "little")


def role_payload(role, lease_s=0, power_cap=0, prebuffer=0, max_key_10s=0, cooldown_s=0):
    return bytes([role, lease_s, power_cap, prebuffer, max_key_10s, cooldown_s])


def parse_info(data):
    """The INFO block (3.1) from a reply's data (after sub and result)"""
    if len(data) < 10:
        raise ValueError("INFO block too short")
    return {"version": data[0], "offered": data[1], "role": data[2], "buffer": data[3], "cach": data[4],
            "clock_us": struct.unpack_from("<I", data, 5)[0], "flags": data[9]}


STATUS_FIELDS = ("role", "flags", "inbound_busy", "at_on_air", "buf1", "buf2")
STATUS_U16 = ("underruns1", "underruns2", "idle", "cach", "slc_taken", "labeled1", "labeled2", "unlabeled",
              "residual_us", "epoch_age_s", "keyed_s", "slc_bad", "lease_expiries")


def parse_status(data):
    """The STATUS block (3.6)"""
    if len(data) < 32:
        raise ValueError("STATUS block too short")
    out = dict(zip(STATUS_FIELDS, data[:6]))
    vals = struct.unpack_from("<HHHHHHHHhHHHH", data, 6)
    out.update(zip(STATUS_U16, vals))
    return out


def parse_epoch(data):
    counter, t_us, ts, flags = struct.unpack_from("<IIBB", data, 0)
    return {"counter": counter, "t_us": t_us, "ts": ts, "flags": flags}


def wrap_diff(a, b):
    """a - b for two u32 clock values, as a signed number"""
    d = (a - b) % WRAP
    return d - WRAP if d >= WRAP // 2 else d


# ── time sync (split-duplex.md 6) ──

class ClockSync:
    """One radio's modem clock against the host's (split-duplex.md 6): TIME samples (t1, t4 host µs; t_rx, t_tx modem
    µs). NTP's clock filter: of every 4 successive samples the one with the smallest round trip (the least
    asymmetric) is a point; a least-squares line through the points of the last 120 s gives the offset and the rate
    (with 1-3 ms of USB asymmetry the rate is good to about 20 ppm after 30 s and 5 ppm after 120 s; it only scales
    period_ns and extrapolations of a few seconds, so either is far inside the 5 ms budget).
    Before there are 3 points over 5 s, the smallest round trip of the last 16 samples gives the offset and the rate
    is 0. offset(h) = modem clock - host clock at host time h (µs, unwrapped)."""

    KEEP = 16
    GROUP = 4
    FIT_S = 120.0

    def __init__(self):
        self.samples = []       # (rtt, host mid, offset), the last KEEP
        self.group = []
        self.points = []        # (host mid, offset): one per GROUP samples
        self.base = None        # unwrapping reference: the first offset
        self.last_rtt = None
        self._fit = None

    def add(self, t1, t4, t_rx, t_tx):
        rtt = (t4 - t1) - wrap_diff(t_tx, t_rx)
        raw = ((t_rx - t1) + (t_tx - t4)) / 2.0
        if self.base is None:
            self.base = raw
        off = self.base + wrap_diff(int(round(raw)) % WRAP, int(round(self.base)) % WRAP)
        mid = (t1 + t4) / 2.0
        s = (rtt, mid, off)
        self.samples = (self.samples + [s])[-self.KEEP:]
        self.group.append(s)
        if len(self.group) >= self.GROUP:
            best = min(self.group, key=lambda x: x[0])
            self.points.append((best[1], best[2]))
            self.group = []
            horizon = mid - self.FIT_S * 1e6
            self.points = [p for p in self.points if p[0] >= horizon]
        self.last_rtt = rtt
        self._fit = None
        return rtt

    def ready(self, n=8):
        return len(self.samples) >= min(n, self.KEEP)

    def fit(self):
        """(rate, offset at h0, h0): offset(h) = icpt + rate (h - h0)"""
        if self._fit is not None:
            return self._fit
        pts = self.points
        if len(pts) < 3 or pts[-1][0] - pts[0][0] < 5e6:
            best = min(self.samples, key=lambda x: x[0])
            self._fit = (0.0, best[2], best[1])
            return self._fit
        n = len(pts)
        mh = sum(p[0] for p in pts) / n
        mo = sum(p[1] for p in pts) / n
        sxx = sum((p[0] - mh) ** 2 for p in pts)
        sxy = sum((p[0] - mh) * (p[1] - mo) for p in pts)
        rate = sxy / sxx if sxx > 0 else 0.0
        self._fit = (rate, mo, mh)
        return self._fit

    def offset(self, h):
        rate, icpt, h0 = self.fit()
        return icpt + rate * (h - h0)

    def rate(self):
        return self.fit()[0]

    def to_modem(self, h):
        return int(round(h + self.offset(h))) % WRAP

    def to_host(self, m, near_h):
        """Host time of modem clock value m, the one nearest host time near_h"""
        guess = self.to_modem(near_h)
        return near_h + wrap_diff(m, guess) / (1.0 + self.rate())


def epoch_for_rx(tx, rx, epoch, now_h):
    """SET_EPOCH data for the RX half (6.2): the TX half's GET_EPOCH reply (dict) translated through both clocks.
    Returns (t_us in the RX clock of an outbound TS1 boundary, period_ns in RX clock units)."""
    t_tx = epoch["t_us"] + (30_000 if epoch["ts"] == 2 else 0)
    h = tx.to_host(t_tx % WRAP, now_h)
    t_rx = rx.to_modem(h)
    period = int(round(SLOT_NS * (1.0 + rx.rate() - tx.rate())))
    return t_rx, period


def epoch_payload(t_us, period_ns=SLOT_NS):
    return struct.pack("<II", t_us % WRAP, period_ns)


# ── the virtual duplex modem (split-duplex.md 8.2, 8.3) ──

ROUTE = {GET_VERSION: "host", GET_STATUS: "host", SET_FREQ: "both", SET_CONFIG: "both", SET_MODE: "both",
         DMR_DATA1: "tx", DMR_DATA2: "tx", DMR_START: "tx", DMR_SHORTLC: "tx", DMR_ABORT: "tx"}


def route(cmd):
    """Where an upstream (MMDVMHost) command goes: host (answered by the host), both, tx, or nak (NAK 2)"""
    return ROUTE.get(cmd, "nak")


class VirtualStatus:
    """The virtual modem's GET_STATUS (8.3) from both halves' latest status frames"""

    def __init__(self):
        self.tx = None
        self.rx = None
        self.state = 0

    def reply(self):
        flags = 0
        space1 = space2 = 0
        if self.tx is not None and len(self.tx) >= 9:
            flags |= self.tx[5] & 0x09
            space1, space2 = self.tx[7], self.tx[8]
        if self.rx is not None and len(self.rx) >= 9:
            flags |= self.rx[5] & 0x44
        return frame(GET_STATUS, bytes([0x02, self.state, flags, 0, space1, space2, 0, 0, 0, 0]))


def busy_mask_from_uplink(open_calls, cmd, control):
    """Inbound busy from the uplink frames (8.4): a call open on a slot is busy from its header or voice until its
    terminator (control 42) or DMR_LOST. open_calls: set of slots, updated in place; returns the mask (bit 0 TS1,
    bit 1 TS2). Data bursts are covered by the RX half's STATUS byte 2 (busy 360 ms after each)."""
    slot = 1 if cmd in (DMR_DATA1, DMR_LOST1) else 2
    if cmd in (DMR_LOST1, DMR_LOST2) or control == 0x42:
        open_calls.discard(slot)
    elif control == 0x41 or (control & 0x40) == 0:
        open_calls.add(slot)
    return (1 if 1 in open_calls else 0) | (2 if 2 in open_calls else 0)


# ── a radio on a serial port ──

class Radio:
    """One radio's MMDVM port: a reader thread, replies matched by command (and 0xD6 sub-command)."""

    def __init__(self, port, name="radio", log=None):
        import serial
        self.name = name
        self.ser = serial.Serial(port, 115200, timeout=0.02)
        self.buf = bytearray()
        self.lock = threading.Condition()
        self.replies = []
        self.uplink = []        # DMR_DATA / DMR_LOST frames heard, (host µs, frame)
        self.status = None
        self.log = log or (lambda *a: None)
        self.stop = False
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    @staticmethod
    def host_us():
        return time.monotonic_ns() // 1000

    def _frames(self):
        while True:
            while self.buf and self.buf[0] != FRAME_START:
                del self.buf[0]
            if len(self.buf) >= 2 and len(self.buf) >= self.buf[1] >= 3:
                n = self.buf[1]
                f = bytes(self.buf[:n])
                del self.buf[:n]
                yield f
            else:
                return

    def _loop(self):
        while not self.stop:
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if not chunk:
                continue
            t = self.host_us()
            self.buf += chunk
            for f in self._frames():
                with self.lock:
                    if f[2] in (DMR_DATA1, DMR_DATA2, DMR_LOST1, DMR_LOST2):
                        self.uplink.append((t, f))
                    else:
                        if f[2] == GET_STATUS:
                            self.status = f
                        self.replies.append((t, f))
                    self.lock.notify_all()

    def send(self, f):
        self.ser.write(f)

    def request(self, f, match, timeout=1.0):
        """Send f, wait for a frame for which match(frame) is true; (host µs at send, at reply, frame)"""
        with self.lock:
            self.replies.clear()
        t1 = self.host_us()
        self.send(f)
        end = time.monotonic() + timeout
        with self.lock:
            while True:
                for i, (t, r) in enumerate(self.replies):
                    if match(r):
                        del self.replies[i]
                        return t1, t, r
                left = end - time.monotonic()
                if left <= 0:
                    raise TimeoutError(f"{self.name}: no reply to {f.hex()}")
                self.lock.wait(left)

    def split(self, sub, data=b"", timeout=1.0):
        """0xD6 request: (t1, t4, result, data)"""
        t1, t4, r = self.request(split_frame(sub, data),
                                 lambda r: (r[2] == ODR_SPLIT and len(r) >= 5 and r[3] == sub) or
                                 (r[2] == NAK and len(r) >= 4 and r[3] == ODR_SPLIT), timeout)
        if r[2] == NAK:
            raise RuntimeError(f"{self.name}: 0xD6 NAKed: firmware without the split extension")
        return t1, t4, r[4], r[5:]

    def command(self, cmd, payload=b"", timeout=1.0):
        """A standard command answered by ACK / NAK; returns the NAK reason or 0"""
        _, _, r = self.request(frame(cmd, payload),
                               lambda r: r[2] in (ACK, NAK) and len(r) >= 4 and r[3] == cmd, timeout)
        return r[4] if r[2] == NAK else 0

    def version(self):
        _, _, r = self.request(frame(GET_VERSION), lambda r: r[2] == GET_VERSION)
        return r

    def time_sample(self, sync, token=None):
        tok = struct.pack("<Q", self.host_us()) if token is None else token
        t1, t4, res, d = self.split(SUB_TIME, tok)
        if res != 0 or d[:8] != tok:
            raise RuntimeError(f"{self.name}: TIME result {res}")
        t_rx, t_tx = struct.unpack_from("<II", d, 8)
        return sync.add(t1, t4, t_rx, t_tx)

    def close(self):
        self.stop = True
        self.t.join(timeout=1)
        self.ser.close()


def require_split(radio):
    v = radio.version()
    m = SUFFIX_RE.search(v[4:])
    if not m:
        raise SystemExit(f"{radio.name}: no SPLIT suffix in {v[4:]!r}: not a split duplex firmware")
    _, _, res, d = radio.split(SUB_INFO)
    info = parse_info(d)
    return int(m.group(1)), info


def setup_half(radio, rx_hz, tx_hz, level, cc):
    for cmd, p in ((SET_FREQ, freq_payload(rx_hz, tx_hz, level)), (SET_CONFIG, config_payload(cc)),
                   (SET_MODE, b"\x02")):
        nak = radio.command(cmd, p)
        if nak:
            raise SystemExit(f"{radio.name}: command 0x{cmd:02X} NAK {nak}")


def set_role(radio, role, **kw):
    _, _, res, d = radio.split(SUB_SET_ROLE, role_payload(role, **kw))
    if res != 0:
        raise SystemExit(f"{radio.name}: SET_ROLE {role}: {RESULTS.get(res, res)}")
    return parse_info(d)


def get_status(radio):
    _, _, res, d = radio.split(SUB_STATUS)
    return parse_status(d)


def sync_burst(radio, sync, n=8, gap=0.02):
    for _ in range(n):
        radio.time_sample(sync)
        time.sleep(gap)


# ── modes ──

def run_pair(a):
    tx, rx = Radio(a.tx, "tx"), Radio(a.rx, "rx")
    fin, fout = round(a.freq_in * 1e6), round(a.freq_out * 1e6)
    try:
        for r in (tx, rx):
            ver, info = require_split(r)
            print(f"{r.name}: SPLIT{ver} offered {info['offered']} cach {CACH_METHODS.get(info['cach'])}")
            setup_half(r, fin, fout, a.power, a.cc)
        set_role(tx, ROLE_TX, power_cap=a.power_cap)
        set_role(rx, ROLE_RX)
        stx, srx = ClockSync(), ClockSync()
        sync_burst(tx, stx)
        sync_burst(rx, srx)
        open_calls, t_epoch = set(), 0.0
        t_end = time.monotonic() + a.seconds
        t_print = 0.0
        while time.monotonic() < t_end:
            tx.time_sample(stx)
            rx.time_sample(srx)
            now = time.monotonic()
            if now - t_epoch >= (2.0 if now - (t_end - a.seconds) < 30 else 10.0):
                _, _, res, d = tx.split(SUB_GET_EPOCH)
                if res == 0:
                    t_rx, period = epoch_for_rx(stx, srx, parse_epoch(d), Radio.host_us())
                    rx.split(SUB_SET_EPOCH, epoch_payload(t_rx, period))
                    t_epoch = now
            st_rx = get_status(rx)
            with rx.lock:
                ups, rx.uplink = rx.uplink, []
            for _, f in ups:
                busy_mask_from_uplink(open_calls, f[2], f[3] if len(f) > 3 else 0)
            mask = st_rx["inbound_busy"] | (1 if 1 in open_calls else 0) | (2 if 2 in open_calls else 0)
            tx.split(SUB_SET_AT, bytes([mask]))     # every loop (1 s): the TX half drops a mask older than 2 s
            if now - t_print >= 2.0:
                st_tx = get_status(tx)
                print(f"tx keyed {st_tx['keyed_s']}s buf {st_tx['buf1']}/{st_tx['buf2']} under "
                      f"{st_tx['underruns1']}/{st_tx['underruns2']} at {st_tx['at_on_air']:02b} | rx busy "
                      f"{st_rx['inbound_busy']:02b} labeled {st_rx['labeled1']}/{st_rx['labeled2']} unlabeled "
                      f"{st_rx['unlabeled']} residual {st_rx['residual_us']} us | rtt {stx.last_rtt / 1000:.1f}/"
                      f"{srx.last_rtt / 1000:.1f} ms rate {stx.rate() * 1e6:+.1f}/{srx.rate() * 1e6:+.1f} ppm")
                t_print = now
            time.sleep(max(0.0, 1.0 - (time.monotonic() - now)))
    finally:
        for r in (tx, rx):
            try:
                set_role(r, ROLE_NORMAL)
            except Exception:
                pass
            r.close()
    return 0


def run_exp(a):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import odr_debug
    fout = round(a.freq_out * 1e6)
    regs = [tuple(int(x, 0) for x in r.split("=", 1)) for r in a.reg]
    # The debug link shares the port (its frames start A5 5A, never E0): set the variant first, then close it
    with odr_debug.DebugLink(a.tx, 2.0) as link:
        link.split_variant(a.exp, regs)
    tx = Radio(a.tx, "tx")
    try:
        ver, info = require_split(tx)
        if not info["offered"] & 1:
            raise SystemExit("tx: this radio's modem driver offers no TX half")
        setup_half(tx, fout, fout, a.power, a.cc)
        set_role(tx, ROLE_TX, power_cap=a.power_cap)
        # Short LC: an activity update, TS1 group voice to TG 9 (split-duplex.md 5.3: the 68-bit form)
        slc = dmr_cach.slc_activity(8, 0, dmr_cach.slc_hash(9), 0)
        tx.command(DMR_SHORTLC, dmr_cach.shortlc_frame_payload(slc))
        print(f"short LC {slc:07X} ({dmr_cach.describe_slc(slc)}): look for it in sdrmon.py analyze --cach")
        tx.command(DMR_START, b"\x01")
        t0 = time.monotonic()
        mask = 0
        while time.monotonic() - t0 < a.seconds:
            mask ^= 0x01
            tx.split(SUB_SET_AT, bytes([mask]))
            for _ in range(4):
                time.sleep(0.5)
                tx.split(SUB_TIME, bytes(8))     # keeps the lease and the host-silence guard
            st = get_status(tx)
            print(f"t {time.monotonic() - t0:4.1f} s keyed {st['flags'] & FLAG_KEYED != 0} at {st['at_on_air']:02b} "
                  f"idle {st['idle']} cach {st['cach']} slc {st['slc_taken']} under "
                  f"{st['underruns1']}/{st['underruns2']}")
        tx.command(DMR_START, b"\x00")
        time.sleep(1.2)
        set_role(tx, ROLE_NORMAL)
    finally:
        tx.close()
    with odr_debug.DebugLink(a.tx, 2.0) as link:
        for k, v in link.split_view().items():
            print(f"{k}={v}")
    return 0


def run_rx_only(a):
    rx = Radio(a.rx, "rx")
    fin = round(a.freq_in * 1e6)
    try:
        require_split(rx)
        setup_half(rx, fin, fin, 0, a.cc)
        set_role(rx, ROLE_RX)
        srx = ClockSync()
        sync_burst(rx, srx)
        e = srx.to_modem(Radio.host_us())
        rx.split(SUB_SET_EPOCH, epoch_payload(e))
        flipped = False
        t0 = time.monotonic()
        while time.monotonic() - t0 < a.seconds:
            rx.time_sample(srx)
            if a.flip_at and not flipped and time.monotonic() - t0 >= a.flip_at:
                e = (e + 30_000) % WRAP
                rx.split(SUB_SET_EPOCH, epoch_payload(e))
                flipped = True
                print("epoch moved by one slot: labels flip from here")
            with rx.lock:
                ups, rx.uplink = rx.uplink, []
            for t, f in ups:
                slot = 1 if f[2] in (DMR_DATA1, DMR_LOST1) else 2
                kind = "lost" if f[2] in (DMR_LOST1, DMR_LOST2) else f"control {f[3]:02X}"
                print(f"{(t / 1e6) % 1000:8.3f} TS{slot} {kind}")
            st = get_status(rx)
            print(f"status labeled {st['labeled1']}/{st['labeled2']} unlabeled {st['unlabeled']} "
                  f"residual {st['residual_us']} us")
            time.sleep(1.0)
        set_role(rx, ROLE_NORMAL)
    finally:
        rx.close()
    return 0


def run_info(port):
    r = Radio(port, "radio")
    try:
        v = r.version()
        print(f"version: {v[4:].decode('ascii', 'replace')}")
        ver, info = require_split(r)
        print("info:", info)
        print("status:", get_status(r))
    finally:
        r.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tx")
    ap.add_argument("--rx")
    ap.add_argument("--info", metavar="PORT")
    ap.add_argument("--exp", type=lambda s: int(s, 0), metavar="VARIANT")
    ap.add_argument("--reg", action="append", default=[], help="raw R=V page 4 pair for --exp 255 (up to 4)")
    ap.add_argument("--rx-only", action="store_true")
    ap.add_argument("--flip-at", type=float, default=0.0)
    ap.add_argument("--freq-in", type=float, default=433.0)
    ap.add_argument("--freq-out", type=float, default=438.0)
    ap.add_argument("--cc", type=int, default=1)
    ap.add_argument("--power", type=int, default=255, help="SET_FREQ RF level (the role's cap applies)")
    ap.add_argument("--power-cap", type=int, default=0, help="SET_ROLE power cap (0: the radio's default 64)")
    ap.add_argument("--seconds", type=float, default=10.0)
    a = ap.parse_args(argv)
    if a.info:
        return run_info(a.info)
    if a.exp is not None:
        if not a.tx:
            ap.error("--exp needs --tx")
        return run_exp(a)
    if a.rx_only:
        if not a.rx:
            ap.error("--rx-only needs --rx")
        return run_rx_only(a)
    if not (a.tx and a.rx):
        ap.error("--tx and --rx (or --exp, --rx-only, --info)")
    return run_pair(a)


if __name__ == "__main__":
    sys.exit(main())
