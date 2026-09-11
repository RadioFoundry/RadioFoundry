# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Helpers for the hardware-in-the-loop tests: the radio over its USB debug link (Radio), and the SDR bench with its
bench transmitter and RTL-SDR receiver (SdrBench). See README.md."""
import argparse
import contextlib
import json
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))

from odr_debug import (DebugLink, DebugLinkError, as_int, find_ports, parse_counters,  # noqa: E402
                       to_rgb, write_png, ACTIONS, PRESS, LONG)


class Radio:
    """The radio under test, over its debug link."""

    def __init__(self, port=None):
        self.link = DebugLink(port, timeout=1.5)
        self.port = self.link.port
        self._version = None
        self._last_shot = 0.0

    def close(self):
        self.link.close()

    @contextlib.contextmanager
    def released(self):
        """Close the port for the duration (another tool opens it, e.g. an MMDVM client), then reopen it."""
        self.link.close()
        try:
            yield self.port
        finally:
            for _ in range(20):
                try:
                    self.link = DebugLink(self.port, timeout=1.5)
                    break
                except DebugLinkError:
                    time.sleep(0.25)

    # ── reading ──
    def ping(self):
        return self.link.ping()

    def version(self, fresh=False):
        if self._version is None or fresh:
            self._version = self.link.version()
        return self._version

    def features(self):
        return set(self.version().get("features", "").split(","))

    def view(self, sections=None):
        return self.link.view(sections)

    def core(self):
        return self.link.view(["core"])

    def screen(self):
        return self.link.view(["link"]).get("ui.screen")

    def counters(self):
        """(firmware ms, flat dict of every counter): the diagnostics lines (modem.*, call.*, voice.*, audio.*), the
        dmr_stats view with full names (call.rx_calls, voice.rx_frames, ...) and the hotspot counters (hs.*)"""
        ms, lines = self.link.diag()
        out = parse_counters(lines)
        for k, v in self.link.view(["dmr_stats", "hotspot"]).items():
            n = as_int(v)
            if n is not None and k != "ms":
                out[k] = n
        return ms, out

    def reset_counters(self):
        return self.link.reset()

    def data_view(self):
        """The dmr_data and dmr_data_stats view sections, numbers as ints (state names stay strings)."""
        out = {}
        for k, v in self.link.view(["dmr_data", "dmr_data_stats"]).items():
            n = as_int(v)
            out[k] = n if n is not None else v
        return out

    def log(self):
        return self.link.log()

    # Every screenshot chunk freezes the UI's redraws for SHOT_HOLD_MS (core/debuglink/debuglink.c), so the capture
    # is one frame. A key pressed within that time after a capture changes the UI state (ui.screen) at once, but the
    # new screen is only drawn when the hold lapses: a second capture taken right away reads the old frame again (and
    # extends the hold). Captures meant to show a new screen wait the hold out first (fresh=True, the default).
    SHOT_HOLD_S = 1.0

    def screenshot(self, tries=4, fresh=True):
        """(info, bytes, stable) of the frame buffer. fresh: wait until the previous capture's redraw hold has lapsed
        and the UI has had a frame to redraw, so the capture shows the current screen."""
        if fresh:
            wait = self._last_shot + self.SHOT_HOLD_S + 0.15 - time.monotonic()
            if wait > 0:
                time.sleep(wait)
        try:
            return self.link.screenshot(tries)
        finally:
            self._last_shot = time.monotonic()

    def shot_until(self, pred, timeout=4.0, what="screen"):
        """Capture until pred(data) is true (and the frame is not blank); returns (info, data). The UI draws a new
        screen up to a frame after its state changed, and a capture can catch a redraw half done, so a single
        capture right after a key is not a reliable picture of the new screen. AssertionError on time-out."""
        end = time.monotonic() + timeout
        while True:
            info, data, stable = self.screenshot()
            if stable and any(data) and pred(data):
                return info, data
            if time.monotonic() > end:
                raise AssertionError(f"timed out after {timeout} s waiting for {what} in the screenshot "
                                     f"(stable {stable}, blank {not any(data)})")

    def save_screenshot(self, path, scale=2, data=None, info=None):
        if data is None:
            info, data, _ = self.screenshot()
        w, h, rows = to_rgb(info, data)
        write_png(path, w, h, rows, scale)
        return path

    def wait_for(self, pred, timeout=3.0, interval=0.1, sections=("core",), what="condition"):
        """Poll the view until pred(view) is true; returns that view. AssertionError with the last view if not."""
        end = time.monotonic() + timeout
        v = {}
        while True:
            v = self.link.view(list(sections))
            if pred(v):
                return v
            if time.monotonic() > end:
                raise AssertionError(f"timed out after {timeout} s waiting for {what}; last view: "
                                     f"{ {k: v[k] for k in list(v)[:40]} }")
            time.sleep(interval)

    # ── keys ──
    def ensure_awake(self):
        """The UI drops the first key after the backlight went off: wake it first when the radio has been idle."""
        idle = as_int(self.link.view(["link"]).get("input.idle_ms"), 0)
        if idle > 20000:
            self.link.wake()
            time.sleep(1.2)

    def press(self, name, gesture=PRESS, value=0, settle=0.15):
        self.link.key(name, gesture, value)
        self.link.wait_keys_idle()
        time.sleep(settle)

    def long_press(self, name):
        self.press(name, LONG)

    def go_home(self):
        self.ensure_awake()
        for _ in range(4):
            s = self.screen()
            if s in ("home", "hotspot"):
                return s
            self.press("back")
        raise AssertionError(f"could not get to the Home screen (screen {self.screen()})")

    def ptt_for(self, seconds):
        self.link.ptt_for(seconds)

    # ── radio state: every test leaves the radio as it found it ──
    def snapshot(self):
        v = self.view(["core", "dmr"])
        return {k: v.get(k) for k in ("vfo", "zone_index", "zone_pos", "rx_hz", "power", "temp", "squelch",
                                      "monitor", "channel_number", "hotspot")}

    def _req_until(self, action, pred, what, timeout=3.0):
        action()
        return self.wait_for(pred, timeout, what=what)

    def restore(self, snap):
        """Put back what snapshot() saw: monitor, zone, VFO or channel position, temporary changes, power, squelch.
        The VFO's own frequency is not known while on a channel; tests that change it restore it themselves
        (vfo_session)."""
        L = self.link
        cur = self.core()
        if cur.get("monitor") != snap["monitor"]:
            L.monitor(snap["monitor"] == "1")
        if cur.get("hotspot") == "1" and snap["hotspot"] != "1":
            L.hotspot_exit()
            self.wait_for(lambda v: v.get("hostlink") == "idle", 25, sections=("hotspot",), what="hotspot exit")
        if cur.get("zone_index") != snap["zone_index"]:
            self._req_until(lambda: L.zone(int(snap["zone_index"])),
                            lambda v: v.get("zone_index") == snap["zone_index"], "zone restored")
        if snap["vfo"] == "1":
            if self.core().get("vfo") != "1":
                self._req_until(lambda: L.vfo(True), lambda v: v.get("vfo") == "1", "VFO on")
            if self.core().get("rx_hz") != snap["rx_hz"]:
                self._req_until(lambda: L.vfo_set(int(snap["rx_hz"])),
                                lambda v: v.get("rx_hz") == snap["rx_hz"], "VFO frequency restored")
        else:
            if self.core().get("vfo") != "0":
                self._req_until(lambda: L.vfo(False), lambda v: v.get("vfo") == "0", "VFO off")
            if self.core().get("zone_pos") != snap["zone_pos"]:
                self._req_until(lambda: L.zone_pos(int(snap["zone_pos"])),
                                lambda v: v.get("zone_pos") == snap["zone_pos"], "channel restored")
        cur = self.core()
        if snap["temp"] in ("0x00", "0") and cur.get("temp") not in ("0x00", "0"):
            self._req_until(L.clear_temporary, lambda v: v.get("temp") in ("0x00", "0"), "temporary changes undone")
        self._step_to("power", L.power_step, snap["power"], int(cur.get("power_count") or 9), wraps=True)
        self._step_to("squelch", L.squelch_step, snap["squelch"], 10, wraps=False)

    def _step_to(self, key, step, target, limit, wraps):
        for _ in range(limit + 1):
            cur = self.core().get(key)
            if cur == target or target is None:
                return
            d = 1 if wraps or int(cur) < int(target) else -1
            step(d)
            time.sleep(0.08)
        raise AssertionError(f"could not restore {key} to {target} (now {self.core().get(key)})")

    def lowest_power(self):
        """Step the power level down to the lowest (a temporary change; restore() puts it back)."""
        for _ in range(int(self.core().get("power_count") or 9) + 1):
            if self.core().get("power") == "0":
                return
            self.link.power_step(-1)
            time.sleep(0.08)
        raise AssertionError(f"could not reach the lowest power (now {self.core().get('power')})")

    @contextlib.contextmanager
    def vfo_session(self):
        """Enter the VFO, yield its frequency, put the VFO frequency back afterwards (restore() does the rest)."""
        L = self.link
        if self.core().get("vfo") != "1":
            self._req_until(lambda: L.vfo(True), lambda v: v.get("vfo") == "1", "VFO on")
        hz = int(self.core()["rx_hz"])
        try:
            yield hz
        finally:
            if self.core().get("rx_hz") != str(hz):
                self._req_until(lambda: L.vfo_set(hz), lambda v: v.get("rx_hz") == str(hz), "VFO frequency back")

    def find_channel(self, rx_hz, mode=None):
        """Select the first channel of the current zone receiving on rx_hz (and in mode 'fm' / 'dmr'); returns its
        zone position. AssertionError if the zone has none."""
        L = self.link
        if self.core().get("vfo") == "1":
            self._req_until(lambda: L.vfo(False), lambda v: v.get("vfo") == "0", "VFO off")
        size = int(self.core().get("zone_size") or 0)
        for pos in range(size):
            v = self._req_until(lambda: L.zone_pos(pos), lambda x: x.get("zone_pos") == str(pos), f"position {pos}")
            if int(v["rx_hz"]) == int(rx_hz) and (mode is None or v.get("mode") == mode):
                return pos
        raise AssertionError(f"no channel on {rx_hz / 1e6:.5f} MHz{' ' + mode if mode else ''} in zone "
                             f"{self.core().get('zone_name')!r} ({size} channels)")


def counter_deltas(before, after, keys):
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


# ── the SDR bench ───────────────────────────────────────────────────────────

class SdrBench:
    """The SDR bench: a small Linux board next to the radio with an MMDVM modem board (the bench transmitter and
    DMR listener, driven by tools/bench/bench_tx.py) and an RTL-SDR receiver (raw captures by tools/bench/sdrmon.py,
    analyzed by sdrmon.py for DMR and tools/sdr_fm_check.py for FM). Both are normally held by the bench's gateway
    service (radiodesk-rfgw). Stopping that service goes through dmr_bench.py's lease: a detached process on the bench
    restarts it whatever happens here. Nothing transmits and nothing is stopped unless the run allowed it
    (--allow-rf, --allow-service-stop). Host, service, directory and python come from $ODR_SDR_BENCH_* (dmr_bench.py
    reads them)."""

    def __init__(self, allow_rf=False, allow_service_stop=False, tx_power=40, lease_max=300, gain=5.0,
                 offset_khz=-200.0):
        import dmr_bench
        self.dmr_bench = dmr_bench
        self.host = dmr_bench.BENCH_HOST
        self.dir = dmr_bench.DIR
        self.py = dmr_bench.PY
        self.allow_rf = allow_rf
        self.allow_service_stop = allow_service_stop
        self.tx_power = tx_power
        self.gain = gain
        self.offset_khz = offset_khz
        self.bench = dmr_bench.Bench(argparse.Namespace(dry_run=False, lease_max=lease_max, yes=True))
        self.bench.note = lambda line: None         # dmr_bench prints every command; pytest -s shows ours anyway
        self.leased = False
        self.bg = []

    def ssh(self, cmd, timeout=300, check=True):
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.host, cmd],
                           capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            raise RuntimeError(f"ssh {self.host} {cmd!r} failed ({r.returncode}): {r.stderr[-1500:]}")
        return r.stdout

    def ssh_bg(self, cmd):
        p = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.host, cmd],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.bg.append(p)
        return p

    def reachable(self):
        try:
            self.ssh(f"test -f ~/{self.dir}/bench_tx.py", timeout=15)
            return True
        except (RuntimeError, subprocess.TimeoutExpired):
            return False

    @contextlib.contextmanager
    def lease(self):
        """The gateway service stopped for the duration and restarted afterwards, also on failure (the bench-side
        guard restarts it after lease_max seconds whatever happens). Background bench processes started inside are
        ended before the service comes back."""
        if not self.allow_service_stop:
            raise RuntimeError("stopping the SDR bench's gateway service needs --allow-service-stop")
        self.bench.lease()
        self.leased = True
        try:
            yield
        finally:
            for p in self.bg:
                if p.poll() is None:
                    p.terminate()
            self.bg = []
            self.bench.release()
            self.leased = False

    def _need_lease(self):
        if not self.leased:
            raise RuntimeError("the bench modem and the SDR belong to the gateway service: take the lease first")

    def upload(self, local, name):
        remote = f"{self.dir}/runs/{name}"
        self.ssh(f"mkdir -p ~/{self.dir}/runs")
        subprocess.run(["scp", "-q", local, f"{self.host}:{remote}"], check=True, timeout=120)
        return f"runs/{name}"     # relative: bench_tx.py runs in ~/<dir>, and a quoted "~" would not expand

    def sync_tool(self, local):
        """Copy one tool (e.g. tools/sdr_fm_check.py) into the bench directory; returns its name there."""
        name = os.path.basename(local)
        self.ssh(f"mkdir -p ~/{self.dir}")
        subprocess.run(["scp", "-q", local, f"{self.host}:{self.dir}/{name}"], check=True, timeout=120)
        return name

    def run_dir(self, tag):
        d = f"runs/{time.strftime('%Y%m%d-%H%M%S')}_{tag}"
        self.ssh(f"mkdir -p ~/{self.dir}/{d}")
        return d

    # ── the bench modem ──
    def transmit(self, args, freq, cc, timeout=300):
        """Run bench_tx.py with args (a string) on freq (MHz) / cc; returns the JSON lines it printed. Needs the
        lease and --allow-rf."""
        if not self.allow_rf:
            raise RuntimeError("the bench transmitter keys up: needs --allow-rf")
        self._need_lease()
        out = self.ssh(f"cd ~/{self.dir} && {self.py} bench_tx.py --freq {freq} --cc {cc} --power {self.tx_power} "
                       f"{args}", timeout=timeout)
        return self.dmr_bench.jsonl(out)

    def voice_call(self, freq, cc, dst, superframes=3, calls=1, headers=8, terminators=3, gap_ms=500, ambe=None,
                   private=False, src=None, late=None, talker_alias=None, pi=False, emergency=False,
                   service_options=None, gps=None):
        """One or more voice calls from the bench (bench_tx.py voice). late=K: no header, the first K bursts left out;
        talker_alias: text in odd superframes; pi: the EMB PI bit on B-F; service_options: hex byte in the LC;
        gps: (lat, lon[, error code]) as an ETSI GPS Info LC in the odd superframes"""
        a = f"voice --dst {dst} --superframes {superframes} --calls {calls} --gap-ms {gap_ms}"
        if late is None:
            a += f" --headers {headers}"
        else:
            a += f" --late {int(late)}"
        a += f" --terminators {terminators}"
        if ambe:
            a += f" --ambe {shlex.quote(ambe)}"
        if private:
            a += " --private"
        if src:
            a += f" --src {src}"
        if talker_alias:
            a += f" --talker-alias {shlex.quote(talker_alias)}"
        if gps:
            a += " --gps " + ",".join(str(x) for x in gps)
        if pi:
            a += " --pi"
        if emergency:
            a += " --emergency"
        if service_options is not None:
            a += f" --service-options {service_options:02x}"
        return self.transmit(a, freq, cc)

    def csbk(self, freq, cc, dst, opcode=None, fid=0, octet3=0, src=None, count=4, listen_after=0):
        """A preamble CSBK train, then optionally one CSBK (bench_tx.py csbk); listen_after: then listen that many
        seconds (JSON lines of what was heard, CSBKs with their "info" octets)."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"csbk --dst {dst} --count {count}"
        if opcode is not None:
            a += f" --opcode {opcode:x} --fid {fid:x} --octet3 {octet3:x}"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc)

    def data(self, freq, cc, dst, tms=None, tms_seq=1, tms_ack=True, confirmed=False, octets=40, preambles=2,
             src=None, listen_after=0, etsi=None, etsi_response=True):
        """One data packet (bench_tx.py data): tms text in IPv4 / UDP 4007 user data, etsi text as ETSI defined short
        data (ISO 8859-1), else octets of filler."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"data --dst {dst} --preambles {preambles}"
        if etsi is not None:
            a += f" --etsi {shlex.quote(etsi)}" + ("" if etsi_response else " --etsi-no-response")
        elif tms is not None:
            a += f" --tms {shlex.quote(tms)} --tms-seq {tms_seq}" + ("" if tms_ack else " --tms-no-ack")
        else:
            a += f" --octets {octets}"
        if confirmed:
            a += " --confirmed"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc)

    def listen_start(self, freq, cc, seconds, ambe_out=None):
        """bench_tx.py listen in the background (receive only); listen_result() collects its JSON lines."""
        self._need_lease()
        extra = f"--ambe-out {ambe_out} " if ambe_out else ""
        p = self.ssh_bg(f"cd ~/{self.dir} && {self.py} bench_tx.py --freq {freq} --cc {cc} {extra}listen --json "
                        f"--seconds {seconds}")
        time.sleep(3)           # port open, tune, 1 s settle
        return p

    def listen_result(self, p, timeout=120):
        try:
            out, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            out = ""
        return self.dmr_bench.jsonl(out)

    # ── the RTL-SDR ──
    def capture_start(self, run_dir, freq, seconds, name="cap0"):
        """A raw I/Q capture (cs8, 1.536 MS/s, the channel offset_khz from the tuner center) in the background;
        returns once the capture is running. The file stays on the bench (about 3 MB/s)."""
        self._need_lease()
        f = f"~/{self.dir}/{run_dir}/{name}.cs8"
        self.ssh(f"cd ~/{self.dir} && (setsid nohup {self.py} sdrmon.py capture --service '' --seconds {seconds} "
                 f"--freq {freq} --offset-khz {self.offset_khz:.1f} --gain {self.gain} --out {f} > {f}.log 2>&1 &) ; "
                 f"for i in $(seq 1 60); do grep -q READY {f}.log && break; sleep 0.25; done; sleep 2; "
                 f"grep READY {f}.log")
        return f

    def capture_wait(self, timeout=120):
        self.ssh(f"for i in $(seq 1 {timeout * 2}); do pgrep -f 'sdrmon[.]py capture' > /dev/null || exit 0; "
                 f"sleep 0.5; done; exit 1", timeout=timeout + 30)

    def analyze_dmr(self, capture, freq, cc):
        """sdrmon.py analyze: every burst with its sync, CC, LC, timing, frequency offset, edges (a dict)."""
        txt = self.ssh(f"cd ~/{self.dir} && {self.py} sdrmon.py analyze {capture} --freq {freq} --cc {cc} "
                       f"--json {capture}.json > {capture}.txt 2>&1; cat {capture}.json", timeout=600)
        return json.loads(txt)

    def analyze_fm(self, capture, bw_khz=12.5, ctcss=None, dcs=None, ppm=None, freq=None):
        """tools/sdr_fm_check.py on the bench; returns (exit status, text)."""
        tool = self.sync_tool(os.path.join(ROOT, "tools", "sdr_fm_check.py"))
        a = f"--offset {self.offset_khz * 1000:.0f} --bw {bw_khz}"
        if ctcss is not None:
            a += f" --expect-ctcss {ctcss}"
        if dcs is not None:
            a += f" --expect-dcs {dcs}"
        if ppm is not None and freq is not None:
            a += f" --center {freq * 1e6 - self.offset_khz * 1e3:.0f} --ppm {ppm}"
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", self.host,
                            f"cd ~/{self.dir} && {self.py} {tool} {capture} {a}"],
                           capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr
