#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""
Adapted from our earlier bench tool hs_normal.py.

On-air test cases of the radio's normal (simplex) MMDVM hotspot, run from the Mac. Planned firmware capability:
needs USB CDC + MMDVM in OpenDigitalRadio (not there yet, 2026-10). They exercise the DMR modem driver and the call
stack far better than key presses can: every burst the radio hears comes back over USB, and every burst it sends
is under our control.

The radio under test is driven as MMDVMHost drives a simplex modem (mmdvm_hotspot.MmdvmLink, over USB serial or the
optional UDP transport); the SDR bench's MMDVM_HS modem board (bench_tx.py, over ssh) is the other radio:

  rf_*   the bench modem transmits, the script checks what the radio sends up, through a model of MMDVMHost's
         DMRSlot::writeModem() (a header starts a call, voice A resets the sequence, B-F must follow in order, a
         terminator ends the call, DMR_LOST ends it as lost)
  net_*  the script sends a call as MMDVMHost would (6 idle frames, 8 headers, voice, 3 terminators, 2 idle frames,
         control byte 0, written only while GET_STATUS shows space), the bench modem listens (bench_tx.py listen --json) and
         the script checks what came over the air

    hotspot_cases.py (--port /dev/tty.usbmodemXXXX | --udp host:3334) [--cases rf_voice8,net_voice,...|all]
                     [--freq 461.6875] [--cc 2] [--power 12] [--sync-tools] [--json results.json] [--list]

Needs RadioDesk (RADIODESK env, default ~/RadioDesk; run with its server venv's python) for
burst building and decoding. rfgw must not hold the bench modem: run it through `dmr_bench.py run H1` (which stops and
restores radiodesk-rfgw@<bench host> around it) or stop the service yourself. --sync-tools copies bench_tx.py to the
SDR bench (~/odr-bench/) first.
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mmdvm_hotspot as M      # noqa: E402

M.radiodesk_path()
from radiodesk.dmr import packet, pdu          # noqa: E402
from radiodesk_rfgw import layer2 as L2, sim   # noqa: E402

BENCH_HOST = os.environ.get("ODR_SDR_BENCH_HOST", "sdr-bench")
BENCH_DIR = os.environ.get("ODR_SDR_BENCH_DIR", "~/odr-bench")
MODEM_CMD = f"cd {BENCH_DIR} && /opt/radiodesk-rfgw/.venv/bin/python bench_tx.py"

DMR_DATA2, DMR_LOST2 = M.DMR_DATA2, M.DMR_LOST2
SYNC_DATA, SYNC_AUDIO = M.SYNC_DATA, M.SYNC_AUDIO
DT_HEADER, DT_TERM, DT_CSBK, DT_DHDR, DT_IDLE = 1, 2, 3, 6, 9

MODEM_SRC, MODEM_SRC2, MODEM_DST = 9990301, 9990302, 215
NET_SRC, NET_SRC2, NET_DST = 3101999, 3101998, 9
frame = M.frame


# ── MMDVMHost's RF side, reduced (DMRSlot::writeModem) ────────────────────────

class HostModel:
    def __init__(self):
        self.state = "listening"
        self.last_n = 0
        self.calls = []             # {src, dst, voice, dropped, end: "terminator" | "lost" | None, t0}
        self.data = []              # (t, data type, info)
        self.events = []

    def call(self):
        return self.calls[-1] if self.calls else None

    def feed(self, t, cmd, d):
        if cmd == DMR_LOST2:
            self.events.append((t, "LOST"))
            if self.state == "audio":
                self.call()["end"] = "lost"
                self.call()["t_end"] = t
            self.state = "listening"
            return
        if cmd != DMR_DATA2 or len(d) < 37:
            self.events.append((t, f"cmd {cmd:02X}"))
            return
        control, burst = d[3], d[4:37]
        if control & SYNC_DATA:
            dt = control & 0x0F
            self.events.append((t, f"data {dt}"))
            if dt == DT_HEADER:
                if self.state == "audio":
                    return
                lc = pdu.decode_voice_lc(burst, DT_HEADER)
                self.calls.append({"src": lc.src_id if lc else None, "dst": lc.dst_id if lc else None,
                                   "fid": lc.fid if lc else None, "voice": 0, "dropped": 0, "end": None,
                                   "t0": t, "headers": 1})
                self.state, self.last_n = "audio", 0
            elif dt == DT_TERM:
                if self.state == "audio":
                    self.call()["end"] = "terminator"
                    self.call()["t_end"] = t
                self.state = "listening"
            else:
                self.data.append((t, dt, burst))
        elif control & SYNC_AUDIO:
            self.events.append((t, "voice 0"))
            if self.state == "audio":
                self.last_n = 0
                self.call()["voice"] += 1
            elif self.calls is not None:
                self.events.append((t, "voice A outside a call"))
        else:
            n = control & 0x0F
            self.events.append((t, f"voice {n}"))
            if self.state != "audio" or n > 5 or n != self.last_n + 1:
                if self.state == "audio":
                    self.call()["dropped"] += 1
                return
            self.last_n = n
            self.call()["voice"] += 1


# ── bursts as MMDVMHost writes them ──────────────────────────────────────────

def ms_data(dt, info, cc):
    return L2.data_burst(info, dt, cc, L2.data_sync(L2.MS_VOICE))


def net_voice_frames(src, dst, cc, superframes, control_bytes):
    """6 idle (jitter), 8 headers, the voice, 3 terminators, 2 idle; control 0 as MMDVMHost (or set, as rfgw)."""
    idle = ms_data(DT_IDLE, bytes.fromhex("ff83df1732094ed1cfcd8a41"), cc)
    call = sim.voice_call(src, dst, cc, superframes, family="ms", slot=1, voice=[sim.AMBE_SILENCE] * (18 * superframes))
    out = [(SYNC_DATA | DT_IDLE, idle)] * 6
    for tb in call:
        if tb.dt == pdu.DT_VOICE_LC_HEADER:
            out += [(SYNC_DATA | DT_HEADER, tb.burst)] * 8
        elif tb.dt == pdu.DT_TERMINATOR_WITH_LC:
            out += [(SYNC_DATA | DT_TERM, tb.burst)] * 3
        else:
            out.append((SYNC_AUDIO if tb.dt == pdu.DT_VOICE_SYNC else tb.n, tb.burst))
    out += [(SYNC_DATA | DT_IDLE, idle)] * 2
    return [((c if control_bytes else 0x00), b) for c, b in out]


def net_data_frames(src, dst, cc, confirmed, octets, control_bytes):
    user = bytes((i * 7 + 1) & 0xFF for i in range(octets))
    if confirmed:
        infos = packet.confirmed_packet(src, dst, 4, user, ns=2).bursts(preambles=2)
    else:
        infos = packet.unconfirmed_packet(src, dst, 4, user, preambles=2)
    return [((SYNC_DATA | dt) if control_bytes else 0x00, ms_data(dt, info, cc)) for dt, info in infos]


def send_paced(hs, frames, upfront):
    """MMDVMHost's network side: the first *upfront* frames are queued at once (idle + headers), the rest at the
    network's 60 ms; frames go to the modem only while it reports space (CModem::clock)."""
    t0 = time.monotonic()
    for i, (control, burst) in enumerate(frames):
        due = t0 + max(0, i - upfront) * 0.060
        while time.monotonic() < due or not hs.take_space():
            time.sleep(0.002)
        hs.send(frame(DMR_DATA2, bytes([control]) + burst))


# ── the bench modem over ssh ────────────────────────────────────────

def modem(args_line, freq, cc, timeout=120):
    cmd = f"{MODEM_CMD} --freq {freq} --cc {cc} {args_line}"
    r = subprocess.run(["ssh", BENCH_HOST, cmd], capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        print(r.stderr[-2000:])
    return r.returncode == 0


class ModemListener:
    def __init__(self, freq, cc, seconds):
        cmd = f"{MODEM_CMD} --freq {freq} --cc {cc} listen --json --seconds {seconds}"
        self.p = subprocess.Popen(["ssh", BENCH_HOST, cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.ready = threading.Event()
        threading.Thread(target=self._out, daemon=True).start()
        threading.Thread(target=self._err, daemon=True).start()
        if not self.ready.wait(20):
            raise RuntimeError("the bench modem listener didn't start")

    def _out(self):
        for line in self.p.stdout:
            try:
                self.lines.put(json.loads(line))
            except ValueError:
                pass

    def _err(self):
        for line in self.p.stderr:
            if "MHz cc" in line:
                self.ready.set()

    def records(self):
        self.p.wait(timeout=120)
        out = []
        while not self.lines.empty():
            out.append(self.lines.get())
        return out


def air_calls(recs):
    """What the bench modem heard, as calls: header LCs, voice bursts, terminators, and voice after the call's terminator."""
    calls, cur = [], None
    for r in recs:
        if r.get("dt") == DT_HEADER:
            if cur is None or cur.get("terminated") or cur["src"] != r.get("src"):
                cur = {"src": r.get("src"), "dst": r.get("dst"), "headers": 0, "voice": 0, "terminated": False,
                       "voice_after_end": 0}
                calls.append(cur)
            cur["headers"] += 1
        elif r.get("dt") == DT_TERM:
            if cur is not None:
                cur["terminated"] = True
        elif "voice" in r and cur is not None:
            if cur["terminated"]:
                cur["voice_after_end"] += 1
            else:
                cur["voice"] += 1
    return calls


# ── the cases ────────────────────────────────────────────────────────────────

class Result:
    def __init__(self, name):
        self.name, self.checks, self.info = name, [], {}

    def check(self, what, ok):
        self.checks.append((what, bool(ok)))

    @property
    def ok(self):
        return all(ok for _, ok in self.checks) and bool(self.checks)


def rf_case(hs, args, modem_line, settle=1.5, timeout=120):
    mark = hs.mark()
    ok = modem(modem_line, args.freq, args.cc, timeout)
    time.sleep(settle)
    frames, naks = hs.since(mark)
    m = HostModel()
    for t, cmd, d in frames:
        m.feed(t, cmd, d)
    return ok, m, naks


def case_rf_voice8(hs, args, r):
    ok, m, _ = rf_case(hs, args, f"voice --src {MODEM_SRC} --dst {MODEM_DST} --headers 8 --superframes 3 --terminators 3")
    r.info["calls"] = m.calls
    r.check("bench modem sent", ok)
    r.check("one call", len(m.calls) == 1)
    if m.calls:
        c = m.calls[0]
        r.check("header LC is the bench modem's", (c["src"], c["dst"]) == (MODEM_SRC, MODEM_DST))
        r.check("no DMR_LOST during the 8 headers", c["end"] != "lost")
        r.check("17-18 of 18 voice bursts in sequence", c["voice"] >= 17)
        r.check("ended by the terminator", c["end"] == "terminator")


def case_rf_late(hs, args, r):
    # No header, the first 8 voice bursts left out: the hotspot makes a header from the embedded LC (from the
    # superframe after the one it joined in), then that superframe from A as silence and the voice from there
    ok, m, _ = rf_case(hs, args, f"voice --src {MODEM_SRC} --dst {MODEM_DST} --late 8 --superframes 5")
    r.info["calls"] = m.calls
    # What came up and when (ms from the first frame), to tell a late LC from missed bursts when it fails
    t0 = m.events[0][0] if m.events else 0
    r.info["events"] = [f"{(t - t0) * 1000:.0f} {e}" for t, e in m.events]
    r.check("bench modem sent", ok)
    r.check("one call", len(m.calls) == 1)
    if m.calls:
        c = m.calls[0]
        r.check("made header has the bench modem's LC (not the last call's)", (c["src"], c["dst"]) == (MODEM_SRC, MODEM_DST))
        r.check("voice from at most a superframe after the LC is known (>= 10 of 22)", c["voice"] >= 10)
        r.check("no voice dropped by the host's sequence check", c["dropped"] == 0)
        r.check("ended by the terminator", c["end"] == "terminator")


def case_rf_long(hs, args, r):
    ok, m, _ = rf_case(hs, args, f"voice --src {MODEM_SRC} --dst {MODEM_DST} --superframes 50", settle=2, timeout=180)
    r.info["calls"] = [dict(c) for c in m.calls]
    r.check("bench modem sent", ok)
    r.check("one call (no LOST and restart)", len(m.calls) == 1)
    if m.calls:
        c = m.calls[0]
        r.check(">= 297 of 300 voice bursts", c["voice"] >= 297)
        r.check("ended by the terminator", c["end"] == "terminator")


def case_rf_lost(hs, args, r):
    ok, m, _ = rf_case(hs, args, f"voice --src {MODEM_SRC} --dst {MODEM_DST} --superframes 2 --terminators 0", settle=2)
    r.info["calls"] = m.calls
    r.check("bench modem sent", ok)
    r.check("one call", len(m.calls) == 1)
    voice_t = [t for t, e in m.events if e.startswith("voice")]
    r.check("voice went up", bool(voice_t))
    if m.calls and m.calls[0]["end"] == "lost" and voice_t:
        lost_after = m.calls[0]["t_end"] - max(voice_t)
        r.info["lost_after_s"] = round(lost_after, 3)
        r.check("DMR_LOST 0.4-0.8 s after the last burst", 0.4 <= lost_after <= 0.8)
    else:
        r.check("ended by DMR_LOST", False)


def case_rf_b2b(hs, args, r):
    ok, m, _ = rf_case(hs, args, f"voice --src {MODEM_SRC} --src2 {MODEM_SRC2} --dst {MODEM_DST} --superframes 2 --calls 3 "
                       f"--gap-ms 60", timeout=150)
    r.info["calls"] = m.calls
    r.check("bench modem sent", ok)
    r.check("three calls", len(m.calls) == 3)
    r.check("sources alternate", [c["src"] for c in m.calls] == [MODEM_SRC, MODEM_SRC2, MODEM_SRC])
    r.check("each ended by its terminator", all(c["end"] == "terminator" for c in m.calls))
    r.check("each >= 11 of 12 voice bursts", all(c["voice"] >= 11 for c in m.calls))


def case_rf_csbk(hs, args, r):
    ok, m, _ = rf_case(hs, args, f"csbk --src {MODEM_SRC} --dst 3101276 --count 4")
    csbks = [x for x in m.data if x[1] == DT_CSBK]
    r.info["csbk"] = len(csbks)
    r.check("bench modem sent", ok)
    r.check(">= 3 of 4 CSBKs up", len(csbks) >= 3)
    r.check("no voice call", not m.calls)


def data_case(hs, args, r, confirmed):
    octets = 40
    ok, m, _ = rf_case(hs, args, f"data --src {MODEM_SRC} --dst 3101276 --octets {octets} --preambles 2"
                       + (" --confirmed" if confirmed else ""))
    types = [dt for _, dt, _ in m.data]
    blocks = (octets + 4 + 9) // 10 if confirmed else (octets + 4 + 11) // 12
    r.info["types"] = types
    r.check("bench modem sent", ok)
    r.check("data header up", types.count(DT_DHDR) == 1)
    r.check(f"{blocks} rate 1/2 blocks up", types.count(7) == blocks)
    if DT_DHDR in types:
        info = next(b for _, dt, b in m.data if dt == DT_DHDR)
        h = pdu.decode_data_header(info)
        r.info["header"] = str(h)
        r.check("header announces the blocks", getattr(h, "blocks", blocks) == blocks)
    r.check("no voice call", not m.calls)


def case_rf_data_unconf(hs, args, r):
    data_case(hs, args, r, False)


def case_rf_data_conf(hs, args, r):
    data_case(hs, args, r, True)


def net_case(hs, args, frames, upfront, listen_s):
    lis = ModemListener(args.freq, args.cc, listen_s)
    mark = hs.mark()
    send_paced(hs, frames, upfront)
    recs = lis.records()
    _, naks = hs.since(mark)
    return recs, naks


def case_net_voice(hs, args, r, control_bytes=False):
    frames = net_voice_frames(NET_SRC, NET_DST, args.cc, 3, control_bytes)
    recs, naks = net_case(hs, args, frames, 14, 8)
    calls = air_calls(recs)
    r.info.update(calls=calls, naks=naks)
    r.check("one call on air", len(calls) == 1)
    if calls:
        c = calls[0]
        r.check("header LC is the network call's", (c["src"], c["dst"]) == (NET_SRC, NET_DST))
        r.check(">= 17 of 18 voice bursts", c["voice"] >= 17)
        r.check("terminator on air", c["terminated"])
        r.check("no voice after the terminator (terminators / idle sent as voice)", c["voice_after_end"] == 0)
    r.check("no NAKs (idle frames are taken)", not naks)


def case_net_voice_ctl(hs, args, r):
    case_net_voice(hs, args, r, control_bytes=True)


def case_net_data(hs, args, r, confirmed=False):
    frames = net_data_frames(NET_SRC, 9990301, args.cc, confirmed, 40, False)
    recs, naks = net_case(hs, args, frames, len(frames), 6)
    data = [x for x in recs if "dt" in x]
    types = [x["dt"] for x in data]
    blocks = (40 + 4 + 9) // 10 if confirmed else (40 + 4 + 11) // 12
    # An MMDVM receiver keeps putting out frames of the last data type every 60 ms until it has lost the sync for a
    # while (eb78cd3: 12 more "blocks" after both packets), so only the blocks the header announced count; the rest are
    # listed with their RSSI and timing (compare with an sdrmon capture to see what was on air)
    after = types[types.index(DT_DHDR) + 1:] if DT_DHDR in types else []
    run = 0
    while run < len(after) and after[run] == 7:
        run += 1
    i0 = (types.index(DT_DHDR) + 1) if DT_DHDR in types else 0
    r.info.update(types=types, naks=naks, extra=[(round(x["t"] - data[0]["t"], 3), x.get("rssi"))
                                                  for x in data[i0 + blocks:]])
    r.check("data header on air", types.count(DT_DHDR) == 1)
    r.check(f"the {blocks} announced blocks follow it", run >= blocks)
    r.check("no voice on air", not any("voice" in x for x in recs))
    r.check("no NAKs", not naks)


def case_net_data_conf(hs, args, r):
    case_net_data(hs, args, r, confirmed=True)


def case_net_b2b(hs, args, r):
    # Two calls back to back: the second's idle frames and headers straight after the first's terminators
    frames = net_voice_frames(NET_SRC, NET_DST, args.cc, 2, False) + net_voice_frames(NET_SRC2, NET_DST, args.cc, 2, False)
    recs, naks = net_case(hs, args, frames, 14, 12)
    calls = air_calls(recs)
    r.info.update(calls=calls, naks=naks)
    r.check("two calls on air", len(calls) == 2)
    r.check("in order, each under its own header", [c["src"] for c in calls] == [NET_SRC, NET_SRC2])
    r.check("each with its terminator", all(c["terminated"] for c in calls))
    r.check("each >= 11 of 12 voice bursts", all(c["voice"] >= 11 for c in calls))


CASES = {
    "rf_voice8": case_rf_voice8, "rf_late": case_rf_late, "rf_long": case_rf_long, "rf_lost": case_rf_lost,
    "rf_b2b": case_rf_b2b, "rf_csbk": case_rf_csbk, "rf_data_unconf": case_rf_data_unconf,
    "rf_data_conf": case_rf_data_conf,
    "net_voice": case_net_voice, "net_voice_ctl": case_net_voice_ctl, "net_data": case_net_data,
    "net_data_conf": case_net_data_conf, "net_b2b": case_net_b2b,
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="the radio's USB CDC MMDVM serial port")
    ap.add_argument("--udp", default=None, help="host:port of the radio's MMDVM UDP transport (optional)")
    ap.add_argument("--local", default=":3335")
    ap.add_argument("--cases", default="all")
    ap.add_argument("--freq", type=float, default=461.6875)
    ap.add_argument("--cc", type=int, default=2)
    ap.add_argument("--power", type=int, default=12)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--sync-tools", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.list:
        print("\n".join(CASES))
        return 0
    if not (args.port or args.udp):
        ap.error("give --port or --udp")
    hz = int(round(args.freq * 1e6))
    names = list(CASES) if args.cases == "all" else args.cases.split(",")
    if args.sync_tools:
        subprocess.run(["ssh", BENCH_HOST, f"mkdir -p {BENCH_DIR}"], check=True)
        subprocess.run(["scp", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench_tx.py"),
                        f"{BENCH_HOST}:{BENCH_DIR}/bench_tx.py"], check=True)

    hs = M.MmdvmLink(M.Transport(args.port, args.udp, args.local), hz, args.cc, args.power, simplex=True)
    results = []
    for _ in range(args.repeat):
        for name in names:
            r = Result(name)
            t = time.monotonic()
            try:
                CASES[name](hs, args, r)
            except Exception as e:      # a case that breaks is a failure, the rest still run
                r.check(f"ran ({e!r})", False)
            r.info["seconds"] = round(time.monotonic() - t, 1)
            results.append(r)
            print(f"{'PASS' if r.ok else 'FAIL'} {name}")
            for what, ok in r.checks:
                print(f"     {'ok ' if ok else 'BAD'} {what}")
            print("     " + json.dumps(r.info, default=str)[:600])
            time.sleep(1.5)     # the channel clear between cases
    hs.stop = True
    passed = sum(r.ok for r in results)
    print(f"# {passed}/{len(results)} cases passed")
    if args.json:
        with open(args.json, "w") as f:
            json.dump([{"case": r.name, "ok": r.ok, "checks": r.checks, "info": r.info} for r in results], f,
                      indent=1, default=str)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
