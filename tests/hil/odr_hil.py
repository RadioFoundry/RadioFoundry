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
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))

from odr_debug import (DebugLink, DebugLinkError, as_int, find_ports, parse_counters,  # noqa: E402
                       to_rgb, write_png, ACTIONS, PRESS, LONG)


# ── skips ───────────────────────────────────────────────────────────────────
# Every HIL skip names its kind (README.md, "Skip kinds"), so a bring-up run can tell a skip it expects (an --allow-*
# flag not given) from a gap (the radio lacks what the feature needs) and fail on the gap. The kind travels as the
# reason's "[kind] " prefix: that survives every path a skip reason takes (skip marks, -rs, the JUnit XML, reports
# from other processes). skip() also sets .kind on the exception, and the conftest copies the kind to the report
# (report.skip_kind) and to the test's user properties ("skip_kind", in the JUnit XML). skip_kind() reads it back.
# A bare pytest.skip in tests/hil fails tools/check_hil_skips.py (tools/ci.sh).

SKIP_KINDS = {
    "not_built": "the firmware was built without it (a switch off, or firmware older than the feature)",
    "no_hw": "the radio lacks the capability",
    "state": "a precondition the test could set up itself (a mode, a setting, the codeplug, the channel)",
    "opt_in": "an opt-in option was not given (an --allow-* flag, a frequency, a file, a second radio)",
    "bench": "the radio, the SDR bench or a host tool is missing, unreachable or busy",
    "other_board": "a board-specific test on another board",
}


def skip_reason(kind, reason):
    """The reason text of a skip of this kind: "[kind] reason". ValueError for an unknown kind or an empty reason."""
    if kind not in SKIP_KINDS:
        raise ValueError(f"unknown skip kind {kind!r} (one of {', '.join(SKIP_KINDS)})")
    if not isinstance(reason, str) or not reason.strip():
        raise ValueError("a skip needs a reason")
    return f"[{kind}] {reason}"


def skip(kind, reason):
    """Skip the running test (or fixture) as pytest.skip does, with its kind."""
    __tracebackhide__ = True        # pytest reports the caller's line, not this one
    import pytest
    e = pytest.skip.Exception(skip_reason(kind, reason))  # hil-skip: the helper
    e.kind = kind
    raise e


def skip_mark(kind, reason):
    """pytest.mark.skip with a kind, for marks added at collection."""
    import pytest
    return pytest.mark.skip(reason=skip_reason(kind, reason))  # hil-skip: the helper


def skip_if(condition, kind, reason):
    """pytest.mark.skipif with a kind."""
    import pytest
    return pytest.mark.skipif(condition, reason=skip_reason(kind, reason))  # hil-skip: the helper


def skip_kind(skipped):
    """The kind of a skip: from a skip exception, a test report or its longrepr, or the reason text. None for a skip
    without one (pytest's own, or a plugin's)."""
    kind = getattr(skipped, "kind", None)
    if kind in SKIP_KINDS:
        return kind
    text = getattr(skipped, "longrepr", skipped)
    if isinstance(text, tuple) and len(text) == 3:         # a skipped report's longrepr: (path, line, reason)
        text = text[2]
    text = str(getattr(text, "msg", text))
    if text.startswith("Skipped: "):
        text = text[len("Skipped: "):]
    if text.startswith("["):
        kind = text[1:text.find("]")]
        if kind in SKIP_KINDS:
            return kind
    return None


# ── boards and their HIL expectations ───────────────────────────────────────
# The one place the HIL tests read board data, all of it from the board registry: the debug link names from
# tools/odrflash's registry.json (any Python 3), a board's own tables ([hil], [cps]) from tools/boards.py, which
# needs Python 3.11 (tools/odr_python.sh finds one).

# board.toml [hil] keys (tools/boards.py BOARD_SCHEMA; all optional, a check whose key a board lacks skips): display
# (VERSION display=), features (VERSION features the build must have), no_features (ones it must not), flash_jedec
# (accepted data flash JEDEC ids), calibration (true: the diagnostics show the factory calibration), battery_mv
# ([low, high] a sane reading lies in), codec (accepted stock codec versions)
HIL_KEYS = ("display", "features", "no_features", "flash_jedec", "calibration", "battery_mv", "codec")
_ENTRIES = {}


def _registry():
    sys.path.insert(0, os.path.join(ROOT, "tools", "odrflash"))
    from odrflash import boards
    return boards


def board_names():
    """boards/<id> -> the board names VERSION can report (the registry's debug link names)."""
    return {b.id: set(b.debuglink_names) for b in _registry().BOARDS.values()}


def board_id(name):
    """The boards/<id> a VERSION board name means, or None."""
    b = _registry().from_debuglink_name(name or "")
    return b.id if b else None


def board_entry(board):
    """A board's registry entry (tools/boards.py get BOARD: board.toml and what is derived), {} for no board."""
    if not board:
        return {}
    if board not in _ENTRIES:
        import subprocess
        py = subprocess.run(["sh", os.path.join(ROOT, "tools", "odr_python.sh")], capture_output=True, text=True)
        if py.returncode != 0:
            raise RuntimeError(py.stderr.strip())
        r = subprocess.run([py.stdout.strip(), os.path.join(ROOT, "tools", "boards.py"), "get", board],
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode != 0:
            raise RuntimeError(f"tools/boards.py get {board}: {r.stderr.strip()}")
        _ENTRIES[board] = json.loads(r.stdout)
    return _ENTRIES[board]


def board_hil(board):
    """A board's HIL expectations (board.toml [hil]), {} for none."""
    return dict(board_entry(board).get("hil", {}))


# ── diagnostics ─────────────────────────────────────────────────────────────

def diag_lines(radio):
    """The radio's diagnostics screen lines (debug link DIAG)."""
    return radio.link.diag()[1]


def find(lines, prefix):
    """The first line that starts with prefix, or None."""
    return next((line for line in lines if line.startswith(prefix)), None)


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
        if self.link.view(["link"]).get("ui.locked") == "1":
            # auto lock locks the keys while the radio sits idle; a locked radio ignores every other key
            self.long_press("quick")
            self.wait_for(lambda v: v.get("ui.locked") == "0", sections=("link",), what="keys unlocked")
        for _ in range(4):
            s = self.screen()
            if s in ("home", "hotspot"):
                return s
            # an emergency call or alarm heard raises this screen; only OK acknowledges and leaves it
            self.press("ok" if s == "emergency" else "back")
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
        """Enter the FM VFO, yield its frequency, put the VFO's frequency and mode back afterwards (restore() does the
        rest). The VFO's mode is a kept setting: a run cut short in a DMR VFO test leaves it DMR."""
        L = self.link
        if self.core().get("vfo") != "1":
            self._req_until(lambda: L.vfo(True), lambda v: v.get("vfo") == "1", "VFO on")
        hz = int(self.core()["rx_hz"])
        mode = self.core().get("mode")
        if mode != "fm":
            self._req_until(lambda: L.dmr_fields(mode="fm"), lambda v: v.get("mode") == "fm", "FM VFO")
        try:
            yield hz
        finally:
            if self.core().get("rx_hz") != str(hz):
                self._req_until(lambda: L.vfo_set(hz), lambda v: v.get("rx_hz") == str(hz), "VFO frequency back")
            if self.core().get("mode") != mode:
                self._req_until(lambda: L.dmr_fields(mode=mode), lambda v: v.get("mode") == mode, "VFO mode back")

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
    service. Stopping that service goes through dmr_bench.py's lease: a detached process on the bench
    restarts it whatever happens here. Nothing transmits and nothing is stopped unless the run allowed it
    (--allow-rf, --allow-service-stop). Host, service, directory and python come from $ODR_SDR_BENCH_* (dmr_bench.py
    reads them).

    A shared bench (the bench runs benchd, see dmr_bench.py) is used the same way but stops nothing: the lease
    is a daemon session, the frequencies a test captures or transmits on are leased to it (another session's radio
    test waits), bench modem jobs queue, captures come from the shared receiver. Waits are printed as they happen
    (predicted time, worst case, who holds the bench); a test that would wait longer than --bench-wait-max is
    skipped."""

    def __init__(self, allow_rf=False, allow_service_stop=False, tx_power=40, lease_max=300, gain=5.0,
                 offset_khz=-200.0, wait_max=None):
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
        if wait_max is not None:
            dmr_bench.WAIT_MAX = float(wait_max)
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
        shared = self.bench.uses_daemon()
        if not shared and not self.allow_service_stop:
            skip("opt_in", "stops the SDR bench's gateway service: run with --allow-service-stop")
        test = os.environ.get("PYTEST_CURRENT_TEST", "").split(" ")[0] or "hil"
        self.bench.label = f"{os.environ['ODR_HIL_NAME']} {test}" if os.environ.get("ODR_HIL_NAME") else test
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

    @staticmethod
    def _busy(e):
        skip("bench", f"the shared SDR bench stayed busy: {e}")

    def _claim(self, freq):
        """A shared bench: this test's lease on freq (another session's radio test waits for it)."""
        try:
            self.bench.claim(freq)
        except self.dmr_bench.BenchBusy as e:
            self._busy(e)

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

    # ── a shared bench's daemon: what it offers, and its stations (ODR radios on the bench host) ──
    def info(self):
        """benchd's view of the bench: the shared RX window, every frequency's caps and holder, the stations
        (their bands, modes, who holds them), the modem queue. {} on a bench without the daemon."""
        if not self.bench.uses_daemon():
            return {}
        return json.loads(self.ssh(f"{self.bench.ctl} info --json", timeout=30))

    def free_channels(self, caps=("rx",), in_window=True):
        """MHz of the bench's listed frequencies with all of caps that no session holds now (in_window: the
        shared receiver covers them, so a capture there needs nobody's turn)."""
        return [c["freq"] for c in self.info().get("channels", [])
                if c.get("listed") and set(caps) <= set(c["caps"]) and not c.get("claimed_by")
                and (c.get("in_window") or not in_window)]

    @contextlib.contextmanager
    def station_port(self, name, tx=(), mode=None):
        """A bench station (an ODR radio on the bench host's USB) as a local port: a pty bridged to it through the
        bench's guard, for odr_debug / Radio, the MMDVM hotspot tools, anything that opens a serial port. tx: MHz
        it may transmit on (leased to this test), mode: dmr | nfm | wfm | aprs | sfr; none: receive only. The
        guard keeps it at its lowest power and refuses PTT anywhere else."""
        import select
        import tty
        self._need_lease()
        if not self.bench.uses_daemon():
            skip("bench", "bench stations need the shared bench daemon (benchd)")
        for f in tx:
            self._claim(f)
        cmd = (f"{self.bench.ctl} bridge --session {self.bench.session} --station {shlex.quote(name)}"
               + "".join(f" --tx {f}" for f in tx) + (f" --mode {mode}" if mode else "")
               + f" --wait-max {self.dmr_bench.WAIT_MAX:g}")
        p = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.host, cmd],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        opened, text = None, ""
        for line in iter(p.stderr.readline, b""):
            text = line.decode(errors="replace").rstrip()
            if text.startswith("odr-bench:"):
                print(text, file=sys.stderr, flush=True)
            if text.startswith("odr-bench: bridge open"):
                opened = text
                break
        if opened is None:
            p.wait(10)
            if p.returncode == self.dmr_bench.EX_TEMPFAIL:
                self._busy(text)
            raise RuntimeError(f"bench station {name}: bridge failed ({p.returncode}): {text}")
        master, slave = os.openpty()
        tty.setraw(slave)
        path = os.ttyname(slave)
        stop = threading.Event()

        def up():                                   # the local tool -> the bench
            while not stop.is_set():
                if select.select([master], [], [], 0.2)[0]:
                    try:
                        data = os.read(master, 4096)
                        p.stdin.write(data)
                    except OSError:
                        return

        def down():                                 # the bench -> the local tool
            while not stop.is_set():
                data = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else os.read(p.stdout.fileno(), 4096)
                if not data:
                    return
                try:
                    os.write(master, data)
                except OSError:
                    return
        ts = [threading.Thread(target=f, daemon=True) for f in (up, down)]
        for t in ts:
            t.start()
        try:
            yield path
        finally:
            stop.set()
            try:
                p.stdin.close()
            except OSError:
                pass
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
            for t in ts:
                t.join(2)
            for fd in (master, slave):
                try:
                    os.close(fd)
                except OSError:
                    pass

    @contextlib.contextmanager
    def station_radio(self, name, tx=(), mode=None):
        """A bench station as a Radio (the whole debug link: keys, PTT, views, counters, screenshots, APRS, data,
        hotspot / SFR control ...), as the second radio of a test."""
        with self.station_port(name, tx, mode) as port:
            r = Radio(port)
            try:
                yield r
            finally:
                r.close()

    def station_run(self, name, cmd, tx=(), mode=None, expect_s=None, timeout=600):
        """cmd (a string, run in ~/<dir> on the bench) with {port} the station's guarded port: for exchanges that
        need the radio's port without a network hop (an MMDVM hotspot session). Returns (status, stdout)."""
        self._need_lease()
        for f in tx:
            self._claim(f)
        argv = (f"{self.bench.ctl} run --session {self.bench.session} --station {shlex.quote(name)}"
                + "".join(f" --tx {f}" for f in tx) + (f" --mode {mode}" if mode else "")
                + (f" --expect {expect_s:.1f}" if expect_s else "")
                + f" --wait-max {self.dmr_bench.WAIT_MAX:g} -- {cmd}")
        rc, out, err = self.bench.ssh_live(f"cd ~/{self.dir} && {argv}", timeout=timeout + self.dmr_bench.WAIT_MAX)
        try:
            self.bench.check_rc(rc, err, f"bench station {name}")
        except self.dmr_bench.BenchBusy as e:
            self._busy(e)
        return rc, out

    # ── the bench modem ──
    def modem_prefix(self, freq, expect_s=None):
        """For code that runs bench_tx.py over ssh itself (hotspot_cases): the command prefix that puts it under
        a shared bench's modem queue (the channel and the modem held for this lease), or the plain one."""
        self._need_lease()
        self._claim(freq)
        try:
            self.bench.hold_modem(expect_s)
        except self.dmr_bench.BenchBusy as e:
            self._busy(e)
        return f"cd ~/{self.dir} && " + self.bench.modem_cmd(f"{self.py} bench_tx.py --power {self.tx_power}",
                                                              "bench_tx")

    def transmit(self, args, freq, cc, timeout=300, expect_s=None):
        """Run bench_tx.py with args (a string) on freq (MHz) / cc; returns the JSON lines it printed. Needs the
        lease and --allow-rf. expect_s: about how long it keys up (a shared bench's queue predictions)."""
        if not self.allow_rf:
            raise RuntimeError("the bench transmitter keys up: needs --allow-rf")
        self._need_lease()
        self._claim(freq)
        cmd = self.bench.modem_cmd(f"{self.py} bench_tx.py --freq {freq} --cc {cc} --power {self.tx_power} {args}",
                                   "bench_tx", expect_s)
        rc, out, err = self.bench.ssh_live(f"cd ~/{self.dir} && {cmd}",
                                           timeout=timeout + (self.dmr_bench.WAIT_MAX if self.bench.daemon else 0))
        try:
            self.bench.check_rc(rc, err, "bench modem")
        except self.dmr_bench.BenchBusy as e:
            self._busy(e)
        if rc != 0:
            raise RuntimeError(f"bench_tx.py {args!r} failed ({rc}): {err[-1500:]}")
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
        bursts = (0 if late is not None else headers) + 6 * superframes + terminators
        return self.transmit(a, freq, cc, expect_s=calls * bursts * 0.06 + (calls - 1) * gap_ms / 1000)

    def csbk(self, freq, cc, dst, opcode=None, fid=0, octet3=0, src=None, count=4, listen_after=0):
        """A preamble CSBK train, then optionally one CSBK (bench_tx.py csbk); listen_after: then listen that many
        seconds (JSON lines of what was heard, CSBKs with their "info" octets)."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"csbk --dst {dst} --count {count}"
        if opcode is not None:
            a += f" --opcode {opcode:x} --fid {fid:x} --octet3 {octet3:x}"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc, expect_s=(count + 1) * 0.06 + listen_after)

    def data(self, freq, cc, dst, tms=None, tms_seq=1, tms_ack=True, confirmed=False, octets=40, preambles=2,
             src=None, listen_after=0, etsi=None, etsi_response=True, ip_text=None):
        """One data packet (bench_tx.py data): tms text in IPv4 / UDP 4007 user data, etsi text as ETSI defined short
        data (ISO 8859-1), ip_text as ETSI IP text (UTF-16BE on UDP 5016), else octets of filler."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"data --dst {dst} --preambles {preambles}"
        if etsi is not None:
            a += f" --etsi {shlex.quote(etsi)}" + ("" if etsi_response else " --etsi-no-response")
        elif tms is not None:
            a += f" --tms {shlex.quote(tms)} --tms-seq {tms_seq}" + ("" if tms_ack else " --tms-no-ack")
        elif ip_text is not None:
            a += f" --ip-text {shlex.quote(ip_text)}"
        else:
            a += f" --octets {octets}"
        if confirmed:
            a += " --confirmed"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc, expect_s=(preambles + octets / 12 + 3) * 0.06 + listen_after)

    def listen_start(self, freq, cc, seconds, ambe_out=None):
        """bench_tx.py listen in the background (receive only); listen_result() collects its JSON lines."""
        self._need_lease()
        extra = f"--ambe-out {ambe_out} " if ambe_out else ""
        cmd = f"{self.py} bench_tx.py --freq {freq} --cc {cc} {extra}listen --json --seconds {seconds}"
        if not self.bench.uses_daemon():
            p = self.ssh_bg(f"cd ~/{self.dir} && {cmd}")
            time.sleep(3)           # port open, tune, 1 s settle
            return p
        self._claim(freq)
        return self._daemon_bg(self.bench.modem_cmd(cmd, "bench_tx-listen", seconds))

    def _daemon_bg(self, cmd):
        """A modem job (cmd from modem_cmd) in the background on a shared bench: returns once it started, its output
        lines collected in p.lines."""
        p = subprocess.Popen(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", self.host,
                              f"cd ~/{self.dir} && " + cmd],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.bg.append(p)
        p.lines, p.stamps, p.started = [], [], threading.Event()     # stamps: time.monotonic() each line arrived

        def pump():
            for line in p.stdout:
                p.stamps.append(time.monotonic())
                p.lines.append(line)
                if line.startswith("odr-bench:"):
                    print(line.rstrip(), file=sys.stderr, flush=True)
                    if line.startswith("odr-bench: started"):
                        p.started.set()
            p.started.set()
        p.reader = threading.Thread(target=pump, daemon=True)
        p.reader.start()
        p.started.wait(self.dmr_bench.WAIT_MAX + 30)     # the modem may be queued behind another session's job
        if p.poll() is not None and p.returncode == self.dmr_bench.EX_TEMPFAIL:
            self._busy("".join(p.lines).strip().splitlines()[-1])
        time.sleep(3)           # port open, tune, 1 s settle
        return p

    def listen_result(self, p, timeout=120):
        if hasattr(p, "lines"):
            try:
                p.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
            p.reader.join(5)
            return self.dmr_bench.jsonl("".join(p.lines))
        try:
            out, _ = p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            out = ""
        return self.dmr_bench.jsonl(out)

    def repeater_start(self, rx, tx, cc, seconds, extra=""):
        """bench_repeater.py --json in the background: a duplex repeater with input rx and output tx (MHz), under a
        shared bench's modem queue; repeater_result() collects its JSON events. extra: more bench_repeater.py
        arguments (e.g. --beacon). Needs the lease and --allow-rf."""
        if not self.allow_rf:
            raise RuntimeError("the bench repeater keys up: needs --allow-rf")
        self._need_lease()
        cmd = (f"{self.py} bench_repeater.py --rx {rx} --tx {tx} --cc {cc} --power {self.tx_power} "
               f"--seconds {seconds} --json {extra}")
        if not self.bench.uses_daemon():
            p = self.ssh_bg(f"cd ~/{self.dir} && {cmd}")
            time.sleep(3)
            return p
        self._claim(rx)
        self._claim(tx)
        return self._daemon_bg(self.bench.modem_cmd(cmd, "bench_repeater", seconds))

    repeater_result = listen_result

    # ── the RTL-SDR ──
    def capture_start(self, run_dir, freq, seconds, name="cap0", gain=None):
        """A raw I/Q capture (cs8, 1.536 MS/s, the channel offset_khz from the tuner center) in the background;
        returns once the capture is running. The file stays on the bench (about 3 MB/s). gain: this capture's RTL-SDR
        gain instead of --sdr-gain (level measurements want the ADC far from its rails)."""
        self._need_lease()
        f = f"~/{self.dir}/{run_dir}/{name}.cs8"
        g = self.gain if gain is None else gain
        self._claim(freq)
        try:
            self.bench.capture_bg(freq, seconds, f, self.offset_khz, g)
        except self.dmr_bench.BenchBusy as e:
            self._busy(e)
        return f

    def capture_wait(self, timeout=120):
        self.bench.captures_wait(timeout)

    def _offset_khz(self, capture):
        """The channel minus the tuner center of this capture: a shared bench's receiver sits at one center for
        every channel, not at offset_khz."""
        m = self.capture_meta(capture)
        if "freq" in m and "center" in m:
            return (m["freq"] - m["center"]) / 1e3
        return self.offset_khz

    def capture_meta(self, capture):
        """The capture's FILE.json (sdrmon.py capture): gain, samples, overflows, clip_pct (I/Q samples at the ADC
        rails, over the whole file)."""
        try:
            return json.loads(self.ssh(f"cat {capture}.json", timeout=30))
        except (RuntimeError, ValueError):
            return {}

    def analyze_dmr(self, capture, freq, cc):
        """sdrmon.py analyze: every burst with its sync, CC, LC, timing, frequency offset, edges (a dict)."""
        txt = self.ssh(f"cd ~/{self.dir} && {self.py} sdrmon.py analyze {capture} --freq {freq} --cc {cc} "
                       f"--json {capture}.json > {capture}.txt 2>&1; cat {capture}.json", timeout=600)
        return json.loads(txt)

    def analyze_aprs(self, capture, bw_khz=25.0):
        """tools/aprs_afsk_check.py on the bench (Bell 202 AFSK / AX.25 decode and the tone measurements); returns
        its JSON result (frames with a valid FCS, segments)."""
        self.sync_tool(os.path.join(ROOT, "tools", "sdr_fm_check.py"))     # its filters
        tool = self.sync_tool(os.path.join(ROOT, "tools", "aprs_afsk_check.py"))
        out = self.ssh(f"cd ~/{self.dir} && {self.py} {tool} {capture} --offset {self._offset_khz(capture) * 1000:.0f} "
                       f"--bw {bw_khz} --json", timeout=600, check=False)
        lines = [ln for ln in out.splitlines() if ln.startswith("{")]
        if not lines:
            raise RuntimeError(f"aprs_afsk_check.py printed no result: {out[-1500:]}")
        return json.loads(lines[-1])

    def analyze_fm(self, capture, bw_khz=12.5, ctcss=None, dcs=None, ppm=None, freq=None, tones=False, track=False,
                   levels=False, tone_snr=None):
        """tools/sdr_fm_check.py on the bench; returns (exit status, text). tones: also report the audio tones;
        track: the carrier every 50 ms; levels: the voice band rms every 50 ms."""
        tool = self.sync_tool(os.path.join(ROOT, "tools", "sdr_fm_check.py"))
        off = self._offset_khz(capture)
        a = f"--offset {off * 1000:.0f} --bw {bw_khz}" + (" --tones" if tones else "") + \
            (" --track" if track else "") + (" --levels" if levels else "") + \
            (f" --tone-snr {tone_snr}" if tone_snr else "")
        if ctcss is not None:
            a += f" --expect-ctcss {ctcss}"
        if dcs is not None:
            a += f" --expect-dcs {dcs}"
        if ppm is not None and freq is not None:
            a += f" --center {freq * 1e6 - off * 1e3:.0f} --ppm {ppm}"
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", self.host,
                            f"cd ~/{self.dir} && {self.py} {tool} {capture} {a}"],
                           capture_output=True, text=True, timeout=600)
        return r.returncode, r.stdout + r.stderr
