# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Helpers for the hardware-in-the-loop tests: the radio over its USB debug link (Radio), and the SDR bench with its
bench transmitter and RTL-SDR receiver (SdrBench). See README.md."""
import argparse
import contextlib
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

    def log(self):
        return self.link.log()

    def screenshot(self, tries=4):
        return self.link.screenshot(tries)

    def save_screenshot(self, path, scale=2):
        info, data, _ = self.link.screenshot()
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

def bench_host(dmr_bench):
    """The SSH host tools/bench/dmr_bench.py talks to (from a dry run of its ssh call, so both always agree);
    $ODR_SDR_BENCH_HOST overrides it for our own calls."""
    if os.environ.get("ODR_SDR_BENCH_HOST"):
        return os.environ["ODR_SDR_BENCH_HOST"]
    b = dmr_bench.Bench(argparse.Namespace(dry_run=True, lease_max=0, yes=True))
    b.log = []
    b.note = lambda line: b.log.append(line)
    b.ssh("true")
    return shlex.split(b.log[-1][2:])[1]       # "$ ssh HOST true"


class SdrBench:
    """The SDR bench: a small Linux board next to the radio with an MMDVM hotspot board (the bench transmitter,
    driven by tools/bench/hat_tx.py) and an RTL-SDR receiver (tools/bench/sdrmon.py). Both are normally held by the
    bench's gateway service (radiodesk-rfgw). Stopping that service goes through dmr_bench.py's lease: a detached
    process on the bench restarts it whatever happens here. Nothing transmits and nothing is stopped unless the run
    allowed it (--allow-rf, --allow-service-stop)."""

    def __init__(self, allow_rf=False, allow_service_stop=False, tx_power=40, lease_max=300):
        import dmr_bench
        self.dmr_bench = dmr_bench
        self.host = bench_host(dmr_bench)
        self.dir = dmr_bench.DIR
        self.py = dmr_bench.PY
        self.allow_rf = allow_rf
        self.allow_service_stop = allow_service_stop
        self.tx_power = tx_power
        self.bench = dmr_bench.Bench(argparse.Namespace(dry_run=False, lease_max=lease_max, yes=True))
        self.leased = False

    def ssh(self, cmd, timeout=300, check=True):
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.host, cmd],
                           capture_output=True, text=True, timeout=timeout)
        if check and r.returncode != 0:
            raise RuntimeError(f"ssh {self.host} {cmd!r} failed ({r.returncode}): {r.stderr[-1500:]}")
        return r.stdout

    def reachable(self):
        try:
            self.ssh(f"test -f ~/{self.dir}/hat_tx.py", timeout=15)
            return True
        except (RuntimeError, subprocess.TimeoutExpired):
            return False

    @contextlib.contextmanager
    def lease(self):
        """The gateway service stopped for the duration and restarted afterwards, also on failure (the bench-side
        guard restarts it after lease_max seconds whatever happens)."""
        if not self.allow_service_stop:
            raise RuntimeError("stopping the SDR bench's gateway service needs --allow-service-stop")
        self.bench.lease()
        self.leased = True
        try:
            yield
        finally:
            self.bench.release()
            self.leased = False

    def upload(self, local, name):
        remote = f"{self.dir}/runs/{name}"
        self.ssh(f"mkdir -p ~/{self.dir}/runs")
        subprocess.run(["scp", "-q", local, f"{self.host}:{remote}"], check=True, timeout=120)
        return f"~/{remote}"

    def transmit(self, args, freq, cc, timeout=300):
        """Run hat_tx.py with args (a string) on freq (MHz) / cc; returns the JSON lines it printed. Needs the lease
        and --allow-rf."""
        if not self.allow_rf:
            raise RuntimeError("the bench transmitter keys up: needs --allow-rf")
        if not self.leased:
            raise RuntimeError("the bench transmitter belongs to the gateway service: take the lease first")
        out = self.ssh(f"cd ~/{self.dir} && {self.py} hat_tx.py --freq {freq} --cc {cc} --power {self.tx_power} "
                       f"{args}", timeout=timeout)
        return self.dmr_bench.jsonl(out)

    def voice_call(self, freq, cc, dst, superframes=3, calls=1, headers=8, terminators=3, gap_ms=500, ambe=None,
                   private=False, src=None):
        a = f"voice --dst {dst} --superframes {superframes} --calls {calls} --headers {headers} " \
            f"--terminators {terminators} --gap-ms {gap_ms}"
        if ambe:
            a += f" --ambe {shlex.quote(ambe)}"
        if private:
            a += " --private"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc)
