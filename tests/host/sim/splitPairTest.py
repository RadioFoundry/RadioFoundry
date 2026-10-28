#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""splitPairTest: the split duplex repeater end to end on two simulated radios (docs/spec/split-duplex.md).

    python3 splitPairTest.py ./splitSim

Two splitSim processes (tests/host/sim/splitSim.c: the real MMDVM core on the simulated split modem) are the TX and
RX halves, each with its own crystal (ppm) and clock offset (one wraps through 2^32 during the run), behind a USB
model with jitter and occasional spikes in both directions. The host between them is tools/split_duplex_host.py's
logic (ClockSync's min-RTT filter and rate fit, epoch_for_rx, the AT relay from the uplink): start-up as section 8.1,
time sync every 1 s, epoch relay, then two overlapping mobile calls on inbound TS1 and TS2, timed from the TX half's
outbound as a subscriber would be, heard by the RX half, sent up labeled, repeated by the host into the TX half and
checked on its outbound: each call whole and in order on its own slot, TC right on every CACH, AT busy during the
calls, no driver underrun, the RX half's phase residual and the host's offset estimates within budget. Then the
host goes silent: the TX half unkeys and both leases expire back to normal.
"""
import heapq
import math
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import split_duplex_host as sdh  # noqa: E402

WRAP = 1 << 32
STEP_US = 5000
LC1 = bytes([0, 0, 0, 0, 0, 0x09, 0x2F, 0x5A, 0x01])     # group call TG 9 from 0x2F5A01
LC2 = bytes([0, 0, 0, 0, 0, 0x5B, 0x2F, 0x5A, 0x02])     # TG 91
KIND_VOICE, KIND_HEADER, KIND_TERM, KIND_IDLE = 0x10, 1, 2, 9

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {what}")


class SimRadio:
    def __init__(self, exe, name, ppm, offset_us, h0):
        self.name, self.ppm, self.off = name, ppm, offset_us
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.out = []           # (host µs emitted, frame)
        self.air = []           # dicts
        self.last_down = 0.0    # in-order delivery of host frames
        self.cmd(f"s {self.modem(h0)}", h0)

    def modem(self, h):
        return int(math.floor(h * (1.0 + self.ppm * 1e-6) + self.off)) % WRAP

    def host_of(self, m, near_h):
        return near_h + sdh.wrap_diff(m, self.modem(near_h)) / (1.0 + self.ppm * 1e-6)

    def cmd(self, line, h):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        while True:
            r = self.p.stdout.readline()
            if not r:
                raise RuntimeError(f"{self.name}: splitSim exited")
            r = r.rstrip("\n")
            if r == ".":
                return
            if r.startswith("o "):
                _, m, hx = r.split(" ")
                self.out.append((self.host_of(int(m), h), bytes.fromhex(hx)))
            elif r.startswith("a "):
                f = r.split(" ")
                self.air.append({"counter": int(f[1]), "t": int(f[2]), "ts": int(f[3]), "kind": int(f[4]),
                                 "index": int(f[5]), "at": int(f[6]), "tc": int(f[7]), "lcss": int(f[8]),
                                 "bits": int(f[9], 16), "underrun": int(f[10]), "payload": bytes.fromhex(f[11][:-1]),
                                 "h": self.host_of(int(f[2]), h)})

    def advance(self, h):
        self.cmd(f"t {self.modem(h)}", h)

    def frame(self, f, h):
        self.cmd("h " + f.hex(), h)

    def burst(self, kind, index, t_start_modem, seed, lc, h):
        self.cmd(f"r {kind} {index} {t_start_modem} {seed} {lc.hex()}", h)

    def close(self):
        try:
            self.p.stdin.write("q\n")
            self.p.stdin.flush()
        except BrokenPipeError:
            pass
        self.p.wait(timeout=5)


class World:
    """Lockstep virtual time: both radios advanced together, the host's frames delivered through the USB model"""

    def __init__(self, exe, seed, ppm_tx, ppm_rx, off_tx, off_rx, spike_ms):
        self.rng = random.Random(seed)
        self.h = 0.0
        self.q = []
        self.seq = 0
        self.spike_ms = spike_ms
        self.tx = SimRadio(exe, "tx", ppm_tx, off_tx, 0.0)
        self.rx = SimRadio(exe, "rx", ppm_rx, off_rx, 0.0)
        self.handlers = []      # host side: fn(radio, h, frame)

    def usb_delay(self):
        d = self.rng.uniform(400, 2500)
        if self.rng.random() < 0.05:
            d += self.rng.uniform(0, self.spike_ms * 1000)
        return d

    def at(self, t, fn):
        self.seq += 1
        heapq.heappush(self.q, (t, self.seq, fn))

    def send(self, radio, f):
        """The host writes f to radio now; it arrives after the USB delay (in order)"""
        t = max(self.h + self.usb_delay(), radio.last_down + 1)
        radio.last_down = t
        self.at(t, lambda: radio.frame(f, self.h))

    def run_until(self, t_end):
        while self.h < t_end:
            nxt = min(self.q[0][0] if self.q else t_end, self.h + STEP_US, t_end)
            self.h = nxt
            for r in (self.tx, self.rx):
                r.advance(self.h)
            while self.q and self.q[0][0] <= self.h:
                _, _, fn = heapq.heappop(self.q)
                fn()
            for r in (self.tx, self.rx):
                outs, r.out = r.out, []
                for th, f in outs:
                    tr = th + self.usb_delay()
                    self.at(tr, (lambda rr, ff: lambda: [hd(rr, self.h, ff) for hd in self.handlers])(r, f))

    def close(self):
        self.tx.close()
        self.rx.close()


class Host:
    """split_duplex_host's logic, event-driven: start-up, time sync, epoch relay, AT relay, the repeat"""

    def __init__(self, w):
        self.w = w
        self.sync = {"tx": sdh.ClockSync(), "rx": sdh.ClockSync()}
        self.pending_time = {}          # token -> (radio name, t1)
        self.token = 0
        self.replies = {"tx": [], "rx": []}
        self.uplink = []                # (h, frame) from RX
        self.open_calls = set()
        self.rx_busy = 0
        self.epochs = []                # (h, epoch dict) from TX
        self.epoch_sent = 0
        self.relay = sdh.EpochRelay()
        self.silent = False
        self.tx_keyed = False
        self.status = {"tx": None, "rx": None}
        w.handlers.append(self.on_frame)

    def radio(self, name):
        return self.w.tx if name == "tx" else self.w.rx

    def on_frame(self, r, h, f):
        name = r.name
        cmd = f[2]
        if cmd == sdh.RFOUNDRY_SPLIT and f[3] == sdh.SUB_TIME and f[4] == 0:
            tok = int.from_bytes(f[5:13], "little")
            if tok in self.pending_time:
                rn, t1 = self.pending_time.pop(tok)
                t_rx, t_tx = int.from_bytes(f[13:17], "little"), int.from_bytes(f[17:21], "little")
                self.sync[rn].add(t1, h, t_rx, t_tx)
            return
        if cmd == sdh.RFOUNDRY_SPLIT and f[3] == sdh.SUB_GET_EPOCH and f[4] == 0 and name == "tx":
            ep = sdh.parse_epoch(f[5:])
            self.epochs.append((h, ep))
            if self.sync["tx"].ready() and self.sync["rx"].ready():
                t_rx, period = sdh.epoch_for_rx(self.sync["tx"], self.sync["rx"], ep, h)
                bound = self.sync["tx"].bound_us() + self.sync["rx"].bound_us()
                if self.relay.decide(t_rx, period, bound):
                    self.w.send(self.w.rx, sdh.split_frame(sdh.SUB_SET_EPOCH, sdh.epoch_payload(t_rx, period)))
                    self.epoch_sent += 1
            return
        if cmd == sdh.RFOUNDRY_SPLIT and f[3] == sdh.SUB_STATUS and f[4] == 0:
            st = sdh.parse_status(f[5:])
            self.status[name] = st
            if name == "rx":
                self.rx_busy = st["inbound_busy"]
            return
        if name == "rx" and cmd in (sdh.DMR_DATA1, sdh.DMR_DATA2, sdh.DMR_LOST1, sdh.DMR_LOST2):
            self.uplink.append((h, f))
            control = f[3] if len(f) > 3 else 0
            mask_old = (1 if 1 in self.open_calls else 0) | (2 if 2 in self.open_calls else 0)
            mask = sdh.busy_mask_from_uplink(self.open_calls, cmd, control)
            if mask != mask_old:
                self.send_at()
            if cmd in (sdh.DMR_DATA1, sdh.DMR_DATA2) and not self.silent:
                # The duplex host repeats the RF call on its own slot (8.4), as MMDVMHost does: DMR_START, then the
                # bursts (control byte and 33 bytes, without the RSSI)
                if not self.tx_keyed:
                    self.w.send(self.w.tx, sdh.frame(sdh.DMR_START, b"\x01"))
                    self.tx_keyed = True
                self.w.send(self.w.tx, sdh.frame(cmd, f[3:37]))
            return
        self.replies[name].append((h, f))

    def send_at(self):
        mask = self.rx_busy | (1 if 1 in self.open_calls else 0) | (2 if 2 in self.open_calls else 0)
        self.w.send(self.w.tx, sdh.split_frame(sdh.SUB_SET_AT, bytes([mask])))

    def time_req(self, name):
        if self.silent:
            return
        self.token += 1
        tok = self.token.to_bytes(8, "little")
        self.pending_time[self.token] = (name, self.w.h)
        self.w.send(self.radio(name), sdh.split_frame(sdh.SUB_TIME, tok))

    def start(self, fin, fout, cc):
        w = self.w
        for r, role in ((w.tx, sdh.ROLE_TX), (w.rx, sdh.ROLE_RX)):
            for f in (sdh.frame(sdh.GET_VERSION), sdh.frame(sdh.SET_FREQ, sdh.freq_payload(fin, fout, 255)),
                      sdh.frame(sdh.SET_CONFIG, sdh.config_payload(cc)), sdh.frame(sdh.SET_MODE, b"\x02"),
                      sdh.split_frame(sdh.SUB_INFO), sdh.split_frame(sdh.SUB_SET_ROLE, sdh.role_payload(role))):
                w.send(r, f)
        # Time sync: 8 samples 20 ms apart, then every 1 s; epoch every 2 s for 30 s, then every 10 s; RX status
        # and the AT relay every 250 ms
        for name in ("tx", "rx"):
            for i in range(8):
                w.at(100_000 + i * 20_000 + (5000 if name == "rx" else 0), (lambda n: lambda: self.time_req(n))(name))
        self.periodic(400_000, 1_000_000, lambda: (self.time_req("tx"), self.time_req("rx")))
        self.periodic(500_000, 250_000, self.poll_rx)
        self.periodic(600_000, 2_000_000, self.get_epoch)

    def periodic(self, first, every, fn):
        def tick():
            if not self.silent:
                fn()
                self.w.at(self.w.h + (every if fn != self.get_epoch or self.w.h < 30e6 else 10_000_000), tick)
        self.w.at(first, tick)

    def poll_rx(self):
        self.w.send(self.w.rx, sdh.split_frame(sdh.SUB_STATUS))
        self.send_at()

    def get_epoch(self):
        self.w.send(self.w.tx, sdh.split_frame(sdh.SUB_GET_EPOCH))


def schedule_calls(w, host, t_call):
    """Two mobile calls timed from the TX half's outbound (its true clock): aligned timing, inbound TS1 runs during
    outbound TS1 and inbound TS2 during outbound TS2 (split-duplex.md 7, ETSI TS 102 361-1 5.1.1 table 5.1); a 2 ms air plus chip delay; each burst reported 31 ms after
    its slot started. Returns the expected per-slot seed sequences."""
    h_ep, ep = host.epochs[-1]
    t1 = ep["t_us"] + (30_000 if ep["ts"] == 2 else 0)       # an outbound TS1 boundary, TX clock
    h_t1 = w.tx.host_of(t1, h_ep)
    # The first outbound TS1 boundary after t_call
    k0 = math.ceil((t_call - h_t1) / (60_000 / (1 + w.tx.ppm * 1e-6)))
    calls = {1: (LC1, 0x40, 0), 2: (LC2, 0x80, 5)}           # slot: LC, seed base, start (frames later)
    for slot, (lc, base, start) in calls.items():
        n_voice = 18
        for j in range(n_voice + 2):
            k = k0 + start + j
            # Outbound boundary of frame k in TX time; inbound TS1 = the TS1 half of the frame
            t_out = (t1 + k * 60_000 + (30_000 if slot == 2 else 0)) % WRAP
            h_start = w.tx.host_of(t_out, t_call) + 2000.0
            if j == 0:
                kind, idx, seed = KIND_HEADER, 0, 0
            elif j <= n_voice:
                kind, idx, seed = KIND_VOICE, (j - 1) % 6, base + j
            else:
                kind, idx, seed = KIND_TERM, 0, 0
            t_rx = w.rx.modem(h_start)
            w.at(h_start + 31_000, (lambda a: lambda: w.rx.burst(*a, w.h))((kind, idx, t_rx, seed, lc)))
    return {1: [0x40 + j for j in range(1, 19)], 2: [0x80 + j for j in range(1, 19)]}, w.tx.host_of(
        (t1 + k0 * 60_000) % WRAP, t_call) + 2000.0


def scenario(exe, seed, ppm_tx, ppm_rx, off_tx, off_rx, spike_ms, label):
    w = World(exe, seed, ppm_tx, ppm_rx, off_tx, off_rx, spike_ms)
    host = Host(w)
    try:
        host.start(433_000_000, 438_000_000, 1)
        w.run_until(1_000_000)
        for name in ("tx", "rx"):
            v = [f for _, f in host.replies[name] if f[2] == sdh.GET_VERSION]
            check(v and sdh.SUFFIX_RE.search(v[0][4:]), f"{label} {name}: SPLIT suffix")
            roles = [f for _, f in host.replies[name] if f[2] == sdh.RFOUNDRY_SPLIT and f[3] == sdh.SUB_SET_ROLE]
            want = sdh.ROLE_TX if name == "tx" else sdh.ROLE_RX
            check(roles and roles[-1][4] == 0 and sdh.parse_info(roles[-1][5:])["role"] == want,
                  f"{label} {name}: SET_ROLE")
            naks = [f for _, f in host.replies[name] if f[2] == sdh.NAK]
            check(not naks, f"{label} {name}: no NAK during start-up ({[f.hex() for f in naks]})")

        # 34 s of time sync (the rate fit needs its 30 s), then the calls
        w.run_until(34_000_000)
        for name, r in (("tx", w.tx), ("rx", w.rx)):
            err = sdh.wrap_diff(host.sync[name].to_modem(w.h), r.modem(w.h))
            rate_err = (host.sync[name].rate() - r.ppm * 1e-6) * 1e6
            check(abs(err) < 1500, f"{label} {name}: offset estimate error {err} us")
            # 34 s of 1-3 ms USB asymmetry pins the rate to about 20 ppm (it only scales period_ns: 0.2 ms per 10 s)
            check(abs(rate_err) < 30.0, f"{label} {name}: rate estimate error {rate_err:.2f} ppm")
        check(host.epoch_sent >= 10, f"{label}: epochs relayed ({host.epoch_sent})")
        check(host.relay.rejected == 0, f"{label}: no epoch held back ({host.relay.log[-3:]})")
        mark_up = len(host.uplink)
        mark_air = len(w.tx.air)
        want, h_first = schedule_calls(w, host, w.h + 200_000)
        w.run_until(w.h + 3_500_000)
        # RX half: each call went up whole, labeled with its own slot
        for slot, cmd in ((1, sdh.DMR_DATA1), (2, sdh.DMR_DATA2)):
            ups = [f for _, f in host.uplink[mark_up:] if f[2] == cmd]
            hdr = [f for f in ups if f[3] == 0x41]
            term = [f for f in ups if f[3] == 0x42]
            voice = [f[4] for f in ups if (f[3] & 0x40) == 0]
            check(len(hdr) == 1 and len(term) == 1, f"{label} uplink TS{slot}: header {len(hdr)} terminator {len(term)}")
            check(voice == want[slot], f"{label} uplink TS{slot}: voice {[hex(x) for x in voice]}")
        lost = [f for _, f in host.uplink[mark_up:] if f[2] in (sdh.DMR_LOST1, sdh.DMR_LOST2)]
        check(not lost, f"{label}: no DMR_LOST (both calls ended with their terminators)")
        # TX half: each call on its own outbound slot, whole and in order; TC right on every CACH; AT busy for the other
        air = w.tx.air[mark_air:]
        for slot in (1, 2):
            mine = [a for a in air if a["ts"] == slot and a["kind"] != KIND_IDLE]
            kinds = [a["kind"] for a in mine]
            voice = [a["payload"][0] for a in mine if a["kind"] == KIND_VOICE]
            check(kinds[:1] == [KIND_HEADER] and kinds[-1:] == [KIND_TERM],
                  f"{label} air TS{slot}: header first, terminator last ({kinds[:2]}..{kinds[-2:]})")
            check(voice == want[slot], f"{label} air TS{slot}: voice {[hex(x) for x in voice]}")
            idx = [a["index"] for a in mine if a["kind"] == KIND_VOICE]
            check(idx == [i % 6 for i in range(18)], f"{label} air TS{slot}: voice A-F in turn")
            # AT in the CACH before outbound TSx speaks for the other channel (aligned timing, ETSI TS 102 361-1 4.5
            # figure 4.8): busy while the other slot's call is on air
            other = [a for a in air if a["ts"] == 3 - slot and a["kind"] != KIND_IDLE]
            if other:
                span = [a for a in air if a["ts"] == slot and other[0]["counter"] < a["counter"] <= other[-1]["counter"]]
                busy = sum(1 for a in span if a["at"] == 1)
                check(busy >= 0.9 * len(span), f"{label} air TS{slot}: AT busy {busy}/{len(span)}")
                gaps = [b["counter"] - a["counter"] for a, b in zip(span, span[1:])]
                check(all(g == 2 for g in gaps), f"{label} air TS{slot}: one burst per frame")
        check(all(a["tc"] == a["ts"] - 1 for a in air), f"{label}: TC on every CACH")
        check(sum(a["underrun"] for a in air) == 0, f"{label}: no driver underrun")
        first_hdr = [a for a in air if a["ts"] == 1 and a["kind"] == KIND_HEADER]
        if first_hdr:
            delay_ms = (first_hdr[0]["h"] - h_first) / 1000
            check(60 <= delay_ms <= 400, f"{label}: repeat delay {delay_ms:.0f} ms")
        st = host.status["rx"]
        check(st is not None and st["unlabeled"] == 0, f"{label}: RX half unlabeled bursts {st and st['unlabeled']}")
        check(st is not None and abs(st["residual_us"]) < 5000, f"{label}: RX half residual {st and st['residual_us']} us")
        check(st is not None and st["labeled1"] >= 20 and st["labeled2"] >= 20, f"{label}: RX half labeled counts")

        # The host goes silent: the TX half unkeys (2 s host silence), then both leases expire (10 s)
        host.silent = True
        w.run_until(w.h + 12_000_000)
        host.silent = False
        for r in (w.tx, w.rx):
            host.replies[r.name].clear()
            w.send(r, sdh.split_frame(sdh.SUB_INFO))
        w.run_until(w.h + 100_000)
        for name in ("tx", "rx"):
            info = [f for _, f in host.replies[name] if f[2] == sdh.RFOUNDRY_SPLIT and f[3] == sdh.SUB_INFO]
            check(info and sdh.parse_info(info[-1][5:])["role"] == sdh.ROLE_NORMAL,
                  f"{label} {name}: back to normal after the lease")
            check(info and not sdh.parse_info(info[-1][5:])["flags"] & sdh.FLAG_KEYED, f"{label} {name}: unkeyed")
        print(f"# {label}: tx {ppm_tx:+} ppm, rx {ppm_rx:+} ppm, spikes {spike_ms} ms: offsets "
              f"{sdh.wrap_diff(host.sync['tx'].to_modem(w.h), w.tx.modem(w.h))}/"
              f"{sdh.wrap_diff(host.sync['rx'].to_modem(w.h), w.rx.modem(w.h))} us, residual "
              f"{st and st['residual_us']} us")
    finally:
        w.close()


def relay_rules():
    """EpochRelay and ClockSync.bound_us on their own: what a bad clock sync can and can't get through"""
    P = sdh.SLOT_NS
    r = sdh.EpochRelay()
    check(not r.decide(1_000_000, P, 12_000) and r.log[-1]["action"] == "skip", "relay: bound over the limit skipped")
    check(r.decide(1_000_000, P, 4_000), "relay: the first good epoch sent")
    check(r.decide(1_000_000 + 50 * 60_000 + 2_000, P, 4_000), "relay: 50 frames on, 2 ms off: sent")
    t_sent = 1_000_000 + 50 * 60_000 + 2_000
    # One slot off (a whole-slot clock error): held, the epoch sent stays
    check(not r.decide(t_sent + 30_000 + 200 * 60_000, P, 4_000) and r.log[-1]["action"] == "hold",
          "relay: one slot off held")
    check(abs(r.log[-1]["rem_us"]) > 29_000, f"relay: the hold's remainder ({r.log[-1]['rem_us']})")
    check(r.decide(t_sent + 300 * 60_000 - 1_000, P, 4_000) and r.held == [], "relay: an agreeing epoch clears the hold")
    # Three in a row that agree with each other but not with the one sent: the one sent was wrong
    base = t_sent + 30_000
    acts = [r.decide((base + k * 600_000 + (k * 500)) % sdh.WRAP, P, 4_000) for k in range(1, 4)]
    check(acts == [False, False, True] and r.log[-1]["action"] == "replace", f"relay: replaced after 3 ({acts})")
    check(r.decide(base + 10 * 600_000 + 1_000, P, 4_000), "relay: the replacement is the reference now")
    # Across the clock wrap
    r2 = sdh.EpochRelay()
    r2.decide(sdh.WRAP - 10_000, P, 1_000)
    check(r2.decide((sdh.WRAP - 10_000 + 7 * 60_000 + 500) % sdh.WRAP, P, 1_000), "relay: agreement across the wrap")
    check(not r2.decide((sdh.WRAP - 10_000 + 7 * 60_000 + 30_500) % sdh.WRAP, P, 1_000), "relay: a slot off across the wrap")
    # bound_us: half the worst round trip the estimate rests on
    c = sdh.ClockSync()
    check(c.bound_us() == float("inf"), "bound: no samples, no bound")
    for i, rtt in enumerate((9_000, 3_000, 5_000)):
        c.add(i * 20_000, i * 20_000 + rtt, 500_000 + i * 20_000, 500_000 + i * 20_000)
    check(c.bound_us() == 1_500, f"bound: early, the best sample ({c.bound_us()})")
    for i in range(3, 40):
        h = i * 1_000_000
        c.add(h, h + (2_000 if i != 21 else 30_000), 500_000 + h, 500_000 + h)
    check(c.bound_us() == 1_000, f"bound: fit points only (a 30 ms spike filtered out) ({c.bound_us()})")


def main(argv):
    relay_rules()
    exe = argv[1] if len(argv) > 1 else os.path.join(HERE, "..", "splitSim")
    # The TX half's clock wraps through 2^32 about 20 s into the run
    scenario(exe, 1, +18.0, -22.0, WRAP - 20_000_000, 123_456_789, 8, "drift")
    scenario(exe, 2, -35.0, +30.0, 987_654_321, WRAP - 40_000_000, 25, "jitter")
    print(f"# splitPairTest: {checks} checks, {'FAILED' if failures else 'PASSED'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
