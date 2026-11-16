# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Helpers for the hardware-in-the-loop tests: the radio over its USB debug link (Radio), and the SDR bench with its
bench board (SdrBench). See README.md."""
import collections
import contextlib
import json
import os
import re
import shlex
import statistics
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))

from rfoundry_debug import (DebugLink, DebugLinkError, as_int, find_network_radios, find_ports,  # noqa: E402
                       parse_counters,
                       to_rgb, write_png, ACTIONS, PRESS, LONG, UP, APRS, APRS_OP_MESSAGE, APRS_OP_SEND_NOW,
                       APRS_OP_STATUS, DATA_OP_CALL_ALERT, DATA_OP_EMERGENCY, DATA_OP_IP_SEND,
                       DATA_OP_RADIO_CHECK, DATA_OP_SMS, DMR_DATA, KEY, OP_TONE_BURST, PTT, RADIO)


# Layer 1 of the TX guard (README.md, "Transmit frequencies"): with --allow-rf, every debug link request that keys the
# radio is compared with the rig's tx_mhz first, on the frequency it would key, and refused outside it. A test finds its
# channel by RX frequency (find_channel), so a split or repeater channel would key a TX frequency no one compared. The
# check is on DebugLink.request, so it covers every link a test opens (the second radio's too). What the radio sends
# by itself (data replies, registration, beacons, roaming probes) no request can check: layer 2 (hil_plugin.py,
# tx_inhibit outside radio_tx tests) covers that.
TX_KEYS = {ACTIONS["ptt"], ACTIONS["emergency"]}

# The start of every text message a HIL test sends the radio: test_delete_message deletes one of these without asking
HIL_SMS_PREFIX = "RadioFoundry HIL "


def keys_tx(cmd, payload):
    """Where a debug link request transmits: "channel" (the channel's TX frequency), "position" (an APRS position, by
    the way out set.aprs_via picks), "aprs" (an APRS status or message), or None (it doesn't key). KEY's op 1 (physical
    key bits) is not known here: its bits are the board's."""
    p = bytes(payload)
    if cmd == PTT:
        return "channel" if p[:1] == b"\x01" else None
    if cmd == KEY and len(p) >= 2 and p[1] in TX_KEYS:
        if p[0] == 3 or p[0] == 0 and len(p) >= 3 and p[2] != UP:     # held, or any gesture but the release
            return "channel"
        return None
    if cmd == RADIO and p[:2] == bytes([OP_TONE_BURST, 1]):
        return "channel"
    if cmd == DMR_DATA and p:
        if p[0] in (DATA_OP_SMS, DATA_OP_CALL_ALERT, DATA_OP_RADIO_CHECK, DATA_OP_IP_SEND) or \
                p[:2] == bytes([DATA_OP_EMERGENCY, 1]):
            return "channel"
        return None
    if cmd == APRS and p[:1] == bytes([APRS_OP_SEND_NOW]):
        return "position"
    if cmd == APRS and p[:1] and p[0] in (APRS_OP_STATUS, APRS_OP_MESSAGE):
        return "aprs"
    return None


def tx_freqs(link, where):
    """The frequencies (MHz) a request that keys on `where` (keys_tx) would transmit on, as the radio says now"""
    # A position goes out by AFSK, over DMR or both (set.aprs_via; a firmware that doesn't say gets both checked);
    # a status or a message by AFSK only (aprs.md). Only what is needed is read: each view is a round trip
    via = "dmr" if where == "channel" else "rf" if where == "aprs" else \
        link.view(["aprs_set"]).get("set.aprs_via", "both")
    out = []
    if via != "dmr":
        a = link.view(["aprs"])
        afsk = int(a.get("aprs.freq_hz") or 0) / 1e6 if a.get("aprs.block") == "ok" else 0     # else no AFSK goes out
        out += [afsk] if afsk else []
    if via != "rf":
        core = link.view(["core"])
        if where == "channel" or core.get("mode") == "dmr":   # positions over DMR go on the DMR channel
            out.append(int(core.get("tx_hz") or 0) / 1e6)
    return out


_request = DebugLink.request


def _guarded_request(self, cmd, payload=b"", *args, **kwargs):
    import hil_config
    if hil_config.CONFIG is not None and hil_config.CONFIG.getoption("--allow-rf"):
        where = keys_tx(cmd, payload)
        if where is not None:
            for mhz in tx_freqs(self, where):
                if not hil_config.tx_allowed(None, mhz):
                    raise RuntimeError(f"TX refused: this request keys on {mhz:.5f} MHz, not one of the rig's "
                                       f"transmit frequencies (tx_mhz)")
    return _request(self, cmd, payload, *args, **kwargs)


DebugLink.request = _guarded_request


# ── skips ───────────────────────────────────────────────────────────────────
# Every HIL skip names its kind (README.md, "Skip kinds"), so a bring-up run can tell a skip it expects (an --allow-*
# flag not given) from a gap (the radio lacks what the feature needs) and fail on the gap. The kind travels as the
# reason's "[kind] " prefix: that survives every path a skip reason takes (skip marks, -rs, the JUnit XML, reports
# from other processes). skip() also sets .kind on the exception, and hil_plugin.py copies the kind to the report
# (report.skip_kind) and to the test's user properties ("skip_kind", in the JUnit XML). skip_kind() reads it back.
# A bare pytest.skip in tests/hil fails tools/check_hil_skips.py (tools/ci.sh).

SKIP_KINDS = {
    "not_built": "the firmware was built without it (a switch off, code not written for the radio, older firmware)",
    "no_hw": "the radio's hardware lacks the capability",
    "state": "a precondition the test could set up itself (a mode, a setting, the codeplug, the channel)",
    "opt_in": "an opt-in option was not given (an --allow-* flag, a frequency, a file, a second radio)",
    "bench": "the radio, the SDR bench or the test host's tools (a program, git history) are missing, unreachable or busy",
    "other_board": "a board-specific test on another board",
}


def skip_reason(kind, reason):
    """The reason text of a skip of this kind: "[kind] reason". ValueError for an unknown kind or an empty reason."""
    if kind not in SKIP_KINDS:
        raise ValueError(f"unknown skip kind {kind!r} (one of {', '.join(SKIP_KINDS)})")
    if not isinstance(reason, str) or not reason.strip():
        raise ValueError("a skip needs a reason")
    return f"[{kind}] {reason}"


def skip(kind, reason, module=False):
    """Skip the running test (or fixture) as pytest.skip does, with its kind. module=True skips the whole module from
    its top level, as pytest.skip(allow_module_level=True)."""
    __tracebackhide__ = True        # pytest reports the caller's line, not this one
    import pytest
    e = pytest.skip.Exception(skip_reason(kind, reason), allow_module_level=module)  # hil-skip: the helper
    e.kind = kind
    raise e


def need_radio_tx(radio):
    """Skip (not_built) when the radio can't transmit at all: its view's can_tx is 0 (no band may, a transmitter not
    written yet). A test that also checks what the radio received calls this after those checks."""
    __tracebackhide__ = True
    if radio.view(["core"]).get("can_tx") == "0":
        skip("not_built", "the radio can't transmit (can_tx 0: its transmitter isn't written yet)")


def bench_sms(radio, bench, freq_mhz, cc, src=9990301):
    """Have the bench send the radio a HIL text message (HIL_SMS_PREFIX, no ACK asked: the radio doesn't transmit)
    on the DMR test channel, and wait until the inbox has it. Returns the radio's inbox count after. The inbox is RAM,
    so a test that needs a message of ours makes one rather than count on test_sms_rx and no restart since."""
    radio.go_home()
    radio.find_channel(round(freq_mhz * 1e6), "dmr")
    own = int(radio.view(["dmr"])["dmr.own_id"])
    text = f"{HIL_SMS_PREFIX}{int(time.time()) % 10000}"
    with bench.lease():
        bench.data(freq_mhz, cc, own, tms=text, tms_seq=(int(time.time()) % 31) + 1, tms_ack=False, src=src)
    v = radio.wait_for(lambda x: (radio.link.inbox(0) or {}).get("text") == text, timeout=5.0, sections=("dmr_data",),
                       what="the bench's message in the inbox")
    radio.go_home()
    return as_int(v.get("data.inbox_count"), 0)


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
# tools/rfoundry's registry.json (any Python 3), a board's own tables ([hil], [cps]) from tools/boards.py, which
# needs Python 3.11 (tools/rfoundry_python.sh finds one).

# board.toml [hil] keys (tools/boards.py BOARD_SCHEMA; all optional, a check whose key a board lacks skips): display
# (VERSION display=), features (VERSION features the build must have), no_features (ones it must not), flash_jedec
# (accepted data flash JEDEC ids), calibration (true: the diagnostics show the factory calibration), battery_mv
# ([low, high] a sane reading lies in), supply_mv (the same for a radio that measures its DC input instead), codec
# (accepted stock codec versions), limits (a test's pass limit this radio is held to instead of the default, by name,
# with the reason: tools/boards.py HIL_LIMITS)
HIL_KEYS = ("display", "features", "no_features", "flash_jedec", "calibration", "battery_mv", "supply_mv", "codec",
            "limits")
_ENTRIES = {}


def _registry():
    sys.path.insert(0, os.path.join(ROOT, "tools", "rfoundry"))
    from rfoundry import boards
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
        py = subprocess.run(["sh", os.path.join(ROOT, "tools", "rfoundry_python.sh")], capture_output=True, text=True)
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



def power_name_watts(name):
    """A power level's name in watts ("100mW" 0.1, "5W" 5), None for a name without one ("Low")"""
    m = re.fullmatch(r"\s*([0-9.]+)\s*(m?)W\s*", name or "")
    return float(m.group(1)) / (1000 if m.group(2) else 1) if m else None

class Radio:
    """The radio under test, over its debug link."""

    def __init__(self, port=None):
        self.link = DebugLink(port, timeout=1.5)
        self.port = self.link.port
        # A port given by name (--radio-port) is the only one the radio is ever reopened on: on a bench with several
        # radios, any other is someone else's
        self.pinned = port is not None
        self.uid = self._uid(self.link)
        self._version = None
        self._txlim = None      # the firmware has the test TX limit (tx_gate); None: not asked yet
        self._last_shot = 0.0
        self._last_key = 0.0     # when this host last pressed a key (ensure_awake is asked only after a pause)

    @staticmethod
    def _uid(link):
        try:
            return link.chip()["uid"]
        except (DebugLinkError, KeyError, ValueError, struct.error):
            return None

    def reconnect(self, timeout=30.0, what="the restart"):
        """Reopen the debug link after the radio restarted or re-enumerated. Pinned: only on its own port. Otherwise
        also any port found by USB ID whose chip has this radio's UID. A chip with another UID is never taken, on its
        own port name either (a re-enumeration can hand that name to another radio); one that can't say yet (an
        HR_C7000 before its UID is read) only on its own name."""
        self.link.close()       # a second open of the port is refused by a serial hub, and leaked on a tty
        end = time.monotonic() + timeout
        foreign = set()         # asked once per call: a long wait doesn't keep writing to another session's radio
        while time.monotonic() < end:
            ports = [self.port] if self.pinned else [self.port] + [p for p in find_ports() if p != self.port]
            for port in (p for p in ports if p not in foreign):
                link = None
                try:
                    link = DebugLink(port, timeout=1.5)
                    link.ping()
                    uid = self._uid(link) if self.uid is not None else None
                    if uid is not None and uid != self.uid or uid is None and port != self.port:
                        link.close()
                        if uid is not None:
                            foreign.add(port)
                        continue
                    self.link, self.port = link, port
                    return
                except (DebugLinkError, OSError):
                    # A serial hub takes one client per port: a link left open would lock out every later try.
                    if link is not None:
                        link.close()
            time.sleep(0.5)
        raise AssertionError(f"the radio did not come back after {what}")

    def serial_back(self, timeout, what="the switch back to the serial port"):
        """After a USB personality switch: True once this radio answers on a serial port again (radio.link reopened,
        as reconnect() does), False after timeout. It doesn't raise, so a test's cleanup can try the network first."""
        try:
            self.reconnect(timeout, what)
            return True
        except AssertionError:
            return False

    def close(self):
        self.link.close()

    def tx_gate(self, may_transmit):
        """Layer 2 of the TX guard (README.md, "Transmit frequencies"): the firmware's test TX limit (RAM only, so a
        restart clears it; the TX inhibit setting, which the radio saves, is left alone). A test that may transmit
        gets the run's transmit frequencies (hil_config.run_tx_mhz), any other test every transmission blocked, so
        what the radio sends by itself (data replies, registration, beacons, roaming probes) can't go out either. The
        firmware sets it before it answers. False: firmware without the limit (no txlim in the core view)."""
        import hil_config
        if self._txlim is None:
            self._txlim = "txlim" in self.view(["core"])
            if not self._txlim:
                print("rfoundry-hil: firmware without the test TX limit (RADIO op 35): only the request check guards TX")
        if not self._txlim:
            return False
        mhz = hil_config.run_tx_mhz() if may_transmit else []
        self.link.tx_limit(block=not mhz, mhz=mhz)
        return True

    def tx_gate_end(self):
        """The test TX limit off at the end of the run, as a restart leaves it"""
        if self._txlim:
            self.link.tx_limit()

    @contextlib.contextmanager
    def released(self):
        """Close the port for the duration (another tool opens it, e.g. an MMDVM client), then reopen it."""
        self.link.close()
        closed = self.link
        try:
            yield self.port
        finally:
            if self.link is closed:         # else a serial_back() inside already reopened it
                for _ in range(20):
                    try:
                        self.link = DebugLink(self.port, timeout=1.5)
                        break
                    except DebugLinkError:
                        time.sleep(0.25)
                # The port re-enumerated meanwhile and its name is another radio's now: find ours
                if self.link is not closed and self.uid is not None and self._uid(self.link) not in (None, self.uid):
                    self.serial_back(10, "the port's release")

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
        # A pause long enough for the backlight to go off (a screenshot of a large screen takes over 30 s at 115200
        # baud) would let the UI eat this key: ask whether it needs waking, after a pause only, so a sequence stays fast
        if time.monotonic() - self._last_key > 15:
            self.ensure_awake()
        self.link.key(name, gesture, value)
        self.link.wait_keys_idle()
        self._last_key = time.monotonic()
        time.sleep(settle)

    def long_press(self, name):
        self.press(name, LONG)

    def in_band(self, hz):
        """True if hz is inside one of the radio's bands (core view bands=). Firmware without the key: True."""
        bands = self.core().get("bands")
        if bands is None:
            return True
        for r in filter(None, bands.split(",")):
            lo, _, hi = r.partition("-")
            if int(lo) <= hz <= int(hi):
                return True
        return False

    def skip_out_of_band(self, hz):
        """Skip the test (no_hw) when hz is outside the radio's bands."""
        if not self.in_band(hz):
            skip("no_hw", f"{hz / 1e6:.4f} MHz is outside the radio's bands ({self.core().get('bands')})")

    def unlock_key(self, v=None):
        """The key held to unlock: "quick", "back" or "none" (a key map with neither; auto lock never locks it). Firmware
        without ui.unlock_key: "quick"."""
        v = v if v is not None else self.link.view(["link"])
        return v.get("ui.unlock_key", "quick")

    def unlock(self):
        """Unlock the keys if they are locked. With no unlock key on the radio a long Back still unlocks: the keys come
        from the debug link, which isn't held to the key map."""
        v = self.link.view(["link"])
        if v.get("ui.locked") != "1":
            return
        self.long_press("quick" if self.unlock_key(v) == "quick" else "back")
        self.wait_for(lambda v: v.get("ui.locked") == "0", sections=("link",), what="keys unlocked")

    def go_home(self):
        self.ensure_awake()
        # auto lock locks the keys while the radio sits idle; a locked radio ignores every other key
        self.unlock()
        # The zone editor's screens go five deep below Home (menu, zones, zone, channel actions, a picker), so a test cut
        # short there leaves more than a few screens; each press is waited on, so a slow one isn't pressed again
        for _ in range(8):
            s = self.screen()
            if s in ("home", "hotspot"):
                return s
            # an emergency call or alarm heard raises this screen; only OK acknowledges and leaves it
            self.press("ok" if s == "emergency" else "back")
            end = time.monotonic() + 1.0
            while self.screen() == s and time.monotonic() < end:
                time.sleep(0.05)
        raise AssertionError(f"could not get to the Home screen (screen {self.screen()})")

    def auto_lock_off(self):
        """Settings > Key lock > Auto lock back to Off (it cycles Off, 30 s, 60 s, 300 s). The session does this before
        the first test: a run cut short inside test_auto_lock leaves it on, and a radio that locks its keys after 30 s
        fails every UI test after it. Returns True when it had to change it."""
        if as_int(self.view(["storage"]).get("settings.auto_lock_s"), 0) == 0:
            return False
        self.go_home()
        for k in ("ok", "7", "9"):
            self.press(k)
        for _ in range(4):
            if self.view(["storage"]).get("settings.auto_lock_s") == "0":
                break
            self.press("ok")
        self.press("back", LONG)
        self.wait_for(lambda v: v.get("settings.auto_lock_s") == "0", sections=("storage",), what="auto lock off")
        self.go_home()
        return True


    @contextlib.contextmanager
    def debuglink_privacy(self):
        """Around a test that loads a privacy key over the debug link (dmr_privacy): the radio core writes the key in
        use back to privacy slot 1, and "off" leaves it in the slot (radio_sm.c, settings write-back), so a slot that
        was empty is emptied again afterwards (Settings > Privacy > Key type steps Motorola, Anytone, Off). Without
        this, test_privacy_key_entry later finds a key it won't overwrite."""
        before = self.view(["storage"]).get("settings.privacy_type")
        try:
            yield
        finally:
            self.link.dmr_privacy("off")
            if before == "0":
                self.privacy_slot_off()

    def privacy_slot_off(self):
        """Privacy slot 1's key type to Off from the menu, which removes its key (each OK on the row steps the type
        once). Returns True when it had to change it."""
        if self.view(["storage"]).get("settings.privacy_type") in (None, "0"):
            return False
        self.go_home()
        for k in ("ok", "7", "4"):
            self.press(k)
        for _ in range(3):
            if self.view(["storage"]).get("settings.privacy_type") == "0":
                break
            self.press("ok")
        self.press("back", LONG)
        self.wait_for(lambda v: v.get("settings.privacy_type") == "0", sections=("storage",), what="privacy slot off")
        self.go_home()
        return True

    def language_english(self):
        """Settings > Display > Language back to English. The tests match the English text the debug link reports
        (screens, toasts, banners), so a radio left in another language fails them as if the firmware were wrong. The
        session does this before the first test. Returns the language it found when it had to change it, else None
        (firmware without languages has no settings.lang)."""
        lang = self.view(["storage"]).get("settings.lang")
        if lang in (None, "en"):
            return None
        self.go_home()
        for k in ("ok", "7", "2", "up", "ok"):  # Language is the Display group's last row
            self.press(k)
        self.wait_for(lambda v: v.get("ui.screen") == "language", sections=("link",), what="the Language screen")
        for _ in range(16):  # English is the list's first row
            if as_int(self.view(["link"]).get("ui.focus"), 0) == 0:
                break
            self.press("up")
        self.press("ok")
        self.wait_for(lambda v: v.get("settings.lang") == "en", sections=("storage",), what="language English")
        self.go_home()
        return lang

    def ptt_for(self, seconds):
        self.link.ptt_for(seconds)

    # ── radio state: every test leaves the radio as it found it ──
    def snapshot(self):
        v = self.view(["core", "dmr"])
        return {k: v.get(k) for k in ("vfo", "zone_index", "zone_pos", "rx_hz", "power", "temp", "squelch",
                                      "monitor", "channel_number", "hotspot")}

    def _req_until(self, action, pred, what, timeout=6.0):
        """Send the request until pred holds. Right after a TX or an emergency the radio can take a zone or VFO
        request without acting on it, so one send is not enough. Every request sent here is idempotent."""
        end = time.monotonic() + timeout
        while True:
            action()
            try:
                return self.wait_for(pred, min(1.5, max(0.1, end - time.monotonic())), what=what)
            except AssertionError:
                if time.monotonic() >= end:
                    raise

    def restore(self, snap):
        """Put back what snapshot() saw: monitor, zone, VFO or channel position, temporary changes, power and squelch.
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

    def bench_tx_level(self):
        """The power level on-air tests key at: 0, the lowest, or with --radio-tx-watts the lowest level whose name
        (the core view's power_names, at the channel's transmit frequency) reads at least that many watts. A radio
        whose names hold no power, or firmware without power_names, stays at the lowest."""
        import hil_config
        want = hil_config.CONFIG.getoption("--radio-tx-watts") if hil_config.CONFIG is not None else None
        names = self.core().get("power_names", "")
        if not want or not names:
            return 0
        names = names.split("/")
        for i, n in enumerate(names):
            w = power_name_watts(n)
            if w is None:
                return 0
            if w >= want * 0.999:
                return i
        return len(names) - 1

    def hotspot_rf_level(self):
        """The MMDVM RF level (SET_FREQ / SET_CONFIG, 0-255) that gives bench_tx_level(): the smallest the radio
        maps to it (radio_sm_hotspot_power spreads 0-255 over the power levels, rounding)"""
        level, n = self.bench_tx_level(), int(self.core().get("power_count") or 1)
        if level == 0 or n < 2:
            return 0
        return max(0, -(-(level * 255 - 127) // (n - 1)))

    def lowest_power(self, absolute=False):
        """Step the power level to bench_tx_level(), the lowest unless --radio-tx-watts asks for more (a temporary
        change; restore() puts it back). absolute: the lowest whatever the option (a ham frequency)."""
        target = 0 if absolute else self.bench_tx_level()
        for _ in range(int(self.core().get("power_count") or 9) + 1):
            cur = int(self.core().get("power") or 0)
            if cur == target:
                return
            self.link.power_step(1 if cur < target else -1)
            time.sleep(0.08)
        raise AssertionError(f"could not reach power level {target} (now {self.core().get('power')})")

    @contextlib.contextmanager
    def vfo_session(self):
        """Enter the FM VFO with no RX or TX tone, yield its frequency, put the VFO's frequency, mode and tones back
        afterwards (restore() does the rest). The VFO's mode and tones are kept settings: a run cut short in a DMR VFO
        test leaves it DMR, and a tone left on it (a run cut short in a tone test, or set by hand) keeps the squelch
        shut on every untoned bench signal."""
        L = self.link
        if self.core().get("vfo") != "1":
            self._req_until(lambda: L.vfo(True), lambda v: v.get("vfo") == "1", "VFO on")
        hz = int(self.core()["rx_hz"])
        mode = self.core().get("mode")
        if mode != "fm":
            self._req_until(lambda: L.dmr_fields(mode="fm"), lambda v: v.get("mode") == "fm", "FM VFO")
        ch = self.view(["channel"])
        tones = {1: ch.get("ch.tx_tone", "none:0"), 2: ch.get("ch.rx_tone", "none:0")}     # 1 TX, 2 RX

        def set_tones(want):
            for which, t in want.items():
                kind, value = t.split(":")
                L.tone(which, kind, int(value))
            self.wait_for(lambda v: v.get("ch.tx_tone") == want[1] and v.get("ch.rx_tone") == want[2],
                          sections=("channel",), what=f"VFO tones TX {want[1]}, RX {want[2]}")
        none = {1: "none:0", 2: "none:0"}
        if tones != none:
            set_tones(none)
        try:
            yield hz
        finally:
            # The tones, frequency and mode are the VFO's: back in it first, after a test that left it for a channel
            if self.core().get("vfo") != "1":
                self._req_until(lambda: L.vfo(True), lambda v: v.get("vfo") == "1", "VFO on")
            if tones != none:
                set_tones(tones)
            if self.core().get("rx_hz") != str(hz):
                self._req_until(lambda: L.vfo_set(hz), lambda v: v.get("rx_hz") == str(hz), "VFO frequency back")
            if self.core().get("mode") != mode:
                self._req_until(lambda: L.dmr_fields(mode=mode), lambda v: v.get("mode") == mode, "VFO mode back")

    def leave_vfo(self):
        """Put the radio on its stored channel when it is in the VFO, and return the core view. restore() puts back the
        state the session found, so a radio left in the VFO comes back to it before each test; a test that works on
        channels leaves the VFO itself instead of skipping."""
        if self.core().get("vfo") == "1":
            L = self.link
            self._req_until(lambda: L.vfo(False), lambda v: v.get("vfo") == "0", "VFO off")
        return self.core()

    def find_channel(self, rx_hz, mode=None, duplex=None, simplex=False):
        """Select the first channel receiving on rx_hz (None: any frequency), in mode 'fm' / 'dmr' and, with duplex
        True / False, transmitting on another / the same frequency (simplex=True is duplex=False): the current zone
        first, then the others in order, so a test doesn't depend on the zone the radio was left in (restore() puts the
        zone back). Returns the channel's position in the zone it selected. AssertionError if no zone has one."""
        if simplex:
            duplex = False
        L = self.link
        self.leave_vfo()
        start = int(self.core().get("zone_index") or 0)
        count = int(self.core().get("zone_count") or 1)
        searched = []
        for zone in [start] + [z for z in range(count) if z != start]:
            if self.core().get("zone_index") != str(zone):
                self._req_until(lambda: L.zone(zone), lambda v: v.get("zone_index") == str(zone), f"zone {zone}")
            size = int(self.core().get("zone_size") or 0)
            for pos in range(size):
                v = self._req_until(lambda: L.zone_pos(pos), lambda x: x.get("zone_pos") == str(pos), f"position {pos}")
                if ((rx_hz is None or int(v["rx_hz"]) == int(rx_hz)) and (mode is None or v.get("mode") == mode)
                        and (duplex is None or (v.get("tx_hz") != v.get("rx_hz")) == duplex)):
                    return pos
            searched.append(f"{self.core().get('zone_name')!r} ({size})")
        on = "on any frequency" if rx_hz is None else f"on {rx_hz / 1e6:.5f} MHz"
        kind = "" if duplex is None else " duplex" if duplex else " simplex"
        raise AssertionError(f"no{kind} channel {on}{' ' + mode if mode else ''} in any zone: "
                             f"{', '.join(searched)}")

FM_SQUELCH_DB = 4.0                # a bench board's FM squelch over the channel's floor (a radio on its lowest power
                                   # can arrive only 7-9 dB over it)
FM_BLOCK_S = 0.01                  # its blocks: the keyed segments' resolution


class _LineTx:
    """A bench board's tx or rx op (bench_line Request) as one of SdrBench's background jobs: .started once the board
    keyed (its "started" line) or started receiving (its rx header), .records an rx op's records, .done the done record ({"mode", "freq", "on_air_s", "cut", "dev_hz", "peak_hz", ...}) once it's over."""

    def __init__(self, line, where, req):
        self.started = threading.Event()
        self.over = threading.Event()      # the done record came (the stream may stay open after it)
        self.done = None
        self.header = None
        self.records = []
        self.pcm = {}               # an rx op's audio streams: name -> int16 LE bytes
        self.req_op = f"{req.get('op')} {req.get('mode', '')}".strip()                  # an rx op's records ({"block": ...})
        self.req = line.Request(where, req, self._on)

    def _on(self, d, data):
        # A tx op's header comes before it waits for the modem: only its "started" line says it keyed. An rx op's
        # header comes once the board receives.
        if "tx" in d or "rx" in d:
            self.header = d.get("tx", d.get("rx"))
            if "rx" in d:
                self.started.set()
        elif "rec" in d:
            self.records.append(d["rec"])
        elif "pcm" in d and data is not None:
            self.pcm.setdefault(d["pcm"].get("stream"), bytearray()).extend(data)
        elif "started" in d:
            self.started.set()
        elif "done" in d:
            self.done = d["done"]
            self.over.set()

    def wait(self, timeout=None):
        end = time.monotonic() + (timeout if timeout is not None else 1e9)
        while not self.over.wait(0.2) and not self.req.done.is_set() and time.monotonic() < end:
            pass
        if self.req.error is not None:
            raise RuntimeError(f"bench board {self.req_op} failed: {self.req.error}")
        if self.done is None:
            raise RuntimeError(f"bench board {self.req_op}: no done record ({self.req.replies[-3:]})")
        return self.done

    def poll(self):
        return 0 if self.over.is_set() or self.req.done.is_set() else None

    def terminate(self):
        self.req.cancel()      # closing the connection unkeys the board

    kill = terminate


class _LineJob:
    """A bench board's job (bench_line Request of op run) as one of SdrBench's background jobs: like
    a Popen, with the tool's output lines in .lines (records and log lines, text with newlines; .out the records
    only) and their arrival on time.monotonic() in .stamps. Files the job writes (--ambe-out) land in file_dir."""

    def __init__(self, line, where, req, file_dir=None):
        self.lines, self.out, self.stamps = [], [], []
        self.started = threading.Event()
        self.returncode = None
        self.file_dir = file_dir
        self._files = set()
        self._L = line
        self.req = line.Request(where, req, self._on)
        self.reader = self.req.thread
        self._watch = threading.Thread(target=self._ended, daemon=True)
        self._watch.start()

    def _on(self, d, data):
        if "out" in d or "err" in d:
            text = (d.get("out") if "out" in d else d["err"]) + "\n"
            self.stamps.append(time.monotonic())
            self.lines.append(text)
            if "out" in d:
                self.out.append(text)
        elif "started" in d:
            self.lines.append(f"rfoundry-bench: started job {d['started']}\n")
            self.stamps.append(time.monotonic())
            self.started.set()
        elif "file" in d and data is not None and self.file_dir:
            name = os.path.join(self.file_dir, os.path.basename(d["file"]))
            with open(name, "ab" if name in self._files else "wb") as f:
                f.write(data)
            self._files.add(name)
        elif "exit" in d:
            if d.get("killed"):
                self.lines.append(f"rfoundry-bench: {d['killed']}\n")
            self.returncode = d["exit"]

    def _ended(self):
        self.req.wait()
        if self.returncode is None:
            # An error reply, or the stream ended with no exit: the daemon client's exit codes
            if self.req.canceled:
                self.returncode = -9
                self.started.set()
                return
            if self.req.error is None:   # the board closed the stream mid-job: it restarted
                self.req.error = self._L.BenchLineError("the bench board ended the job's stream with no exit", "lost")
            self.returncode = self._L.exit_code(self.req.error)
            self.lines.append(f"rfoundry-bench: {self.req.error}\n")
        self.started.set()

    def poll(self):
        return self.returncode if self.req.done.is_set() and not self._watch.is_alive() else None

    def wait(self, timeout=None):
        if not self.req.wait(timeout):
            raise subprocess.TimeoutExpired("bench board job", timeout)
        self._watch.join(5)
        return self.returncode

    def kill(self):
        """Close the job's connection: the board ends it (a transmission unkeys)."""
        if not self.req.done.is_set():
            self.req.cancel()
            self._watch.join(5)
            if self.returncode is None or self.returncode == 1:
                self.returncode = -9

    terminate = kill


INDIVIDUAL_NET = 0x0C000000        # 12.0.0.0/8: the IP gateway's individual range (default addressing)


@contextlib.contextmanager
def usb_network(radio, own_id, timeout=30.0):
    """The radio as this host's USB network adapter for the duration (docs/spec/usb-network.md; the test needs
    --allow-usb-switch): switched to the network personality, then its debug link over UDP at the radio's address once
    the host has its DHCP lease. Yields (udp, host_ip, radio_ip): the DebugLink over UDP and the addresses as strings,
    12.<own_id> for the host and that xor 1 for the radio (the default /32 link). Afterwards, also on failure, the radio
    goes back to the serial personality and radio.link is opened again."""
    import ipaddress
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | (own_id & 0xFFFFFF))
    radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
    radio.link.usb_network(True)
    radio.link.close()
    udp = None
    try:
        end = time.monotonic() + timeout
        while udp is None and time.monotonic() < end:
            try:
                udp = DebugLink(f"udp:{radio_ip}", timeout=1.0)
                udp.ping()
            except (DebugLinkError, OSError):
                udp = None
                time.sleep(0.5)
        assert udp is not None, f"no debug link over UDP at {radio_ip} (DHCP on the host's adapter?)"
        yield udp, str(host_ip), radio_ip
    finally:
        if udp is not None:
            try:
                udp.usb_network(False)
            except (DebugLinkError, OSError):
                pass
            udp.close()
        if not radio.serial_back(30):
            print("rfoundry-hil: the radio didn't come back as a serial port", file=sys.stderr)


def host_network_kind(radio):
    """The radio's IP link to this host: "usb" (USB_NETWORK, the network adapter), "ppp" (NET_PPP only: PPP on the
    serial host port, docs/spec/ppp.md) or None."""
    feats = radio.features()
    return "usb" if "USB_NETWORK" in feats else "ppp" if "NET_PPP" in feats else None


@contextlib.contextmanager
def ppp_network(radio, own_id, timeout=30.0):
    """PPP on the radio's serial host port for the duration, as usb_network yields (udp, host_ip, radio_ip): pppd on
    radio.port through tools/rfoundry_ppp.sh (root, or passwordless sudo), which adds the routes to 12.0.0.0/8 and
    225.0.0.0/8. Afterwards, also on failure, pppd hangs up (LCP Terminate) and radio.link is opened again. Skips where
    this host can't run it (no pppd, no root, the radio not on a local serial port)."""
    import ipaddress
    import shutil
    port = str(radio.port)
    if not port.startswith("/dev/"):
        skip("bench", f"PPP needs the radio's serial port on this host, not {port}")
    if not (shutil.which("pppd") or os.path.exists("/usr/sbin/pppd")):
        skip("bench", "no pppd on this host")
    sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
    if sudo and subprocess.run(sudo + ["true"], capture_output=True).returncode != 0:
        skip("bench", "pppd needs root: run as root or with passwordless sudo")
    script = os.path.join(ROOT, "tools", "rfoundry_ppp.sh")
    host_ip = ipaddress.IPv4Address(INDIVIDUAL_NET | (own_id & 0xFFFFFF))
    radio_ip = str(ipaddress.IPv4Address(int(host_ip) ^ 1))
    radio.link.close()
    up, udp = False, None
    try:
        r = subprocess.run(sudo + ["sh", script, port], capture_output=True, text=True, timeout=timeout + 30)
        assert r.returncode == 0, f"pppd didn't bring the link up: {r.stdout}{r.stderr}"
        up = True
        end = time.monotonic() + timeout
        while udp is None and time.monotonic() < end:
            try:
                udp = DebugLink(f"udp:{radio_ip}", timeout=1.0)
                udp.ping()
            except (DebugLinkError, OSError):
                udp = None
                time.sleep(0.5)
        assert udp is not None, f"no debug link over UDP at {radio_ip} over PPP"
        yield udp, str(host_ip), radio_ip
    finally:
        if udp is not None:
            udp.close()
        if up:
            subprocess.run(sudo + ["sh", script, "--stop"], capture_output=True, timeout=10)
        # pppd waits for the radio's Terminate-Ack before it lets go of the port; the debug link's first frame also
        # takes the port back from PPP (ppp.md 4)
        for _ in range(80):
            try:
                radio.link = DebugLink(radio.port, timeout=1.5)
                radio.link.ping()
                break
            except (DebugLinkError, OSError):
                time.sleep(0.25)


@contextlib.contextmanager
def host_network(radio, own_id, timeout=30.0):
    """The radio's IP link to this host for the duration (the test needs --allow-usb-switch): the USB network adapter,
    or PPP on the serial host port on a radio without one (host_network_kind). Yields as usb_network."""
    kind = host_network_kind(radio)
    if kind is None:
        skip("not_built", "no IP link to the host (neither CONFIG_USB_NETWORK nor CONFIG_NET_PPP)")
    with (usb_network if kind == "usb" else ppp_network)(radio, own_id, timeout) as link:
        yield link


def counter_deltas(before, after, keys):
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


def jsonl(text):
    """The JSON objects in a job's output lines; other lines are left out"""
    out = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


# ── the SDR bench ───────────────────────────────────────────────────────────

class SdrBench:
    """The SDR bench: a bench board (api="HOST[:PORT]", --bench-api), over the bench line protocol
    (tools/bench/bench_line.py, TCP 7373). transmit() and the calls built on it (voice_call, data, csbk), listen_start(),
    repeater_start() and outbound_start() send bench_tx / bench_repeater jobs the board runs with its own DMR stack,
    under a session held for the lease. bursts_start(), fm_rx_start() and aprs_rx_start() read the receive board
    (api_rx, --bench-api-rx; the same board by default): what it decoded and measured, never I/Q. A job the board can't
    do (a duplex repeater on a half-duplex board) skips, and a test that would wait longer than --bench-wait-max skips.
    Nothing transmits unless the run allowed it (--allow-rf)."""

    def __init__(self, allow_rf=False, lease_max=300, wait_max=None, api=None, api_rx=None, api_level=None):
        self.api = api
        self.api_rx = api_rx or api
        self.api_level = api_level      # the board's transmit level 0-255 (None: its own; an SDR ignores it)
        self.power_dbm = None           # an SDR's level, dBm at its antenna (None: its own); the steps are exact
        self.dev_scale = None           # the DMR deviation, a share of the standard's (0.5-1.5; None: 1)
        self.lease_max = lease_max
        self.wait_max = 900.0 if wait_max is None else float(wait_max)
        self.label = None               # the lease's name on the board
        self._api_session = None
        self._api_tx_reach = None       # (tx_lo, tx_hi) after a tune in this session (_api_reach)
        self._run_dirs = []
        self.allow_rf = allow_rf
        self.leased = False
        self.bg = []

    # ── a bench board, over the line protocol ──
    @staticmethod
    def _line():
        import bench_line
        return bench_line

    def _api_info(self, rx=False):
        """The board's info (cached): window (MHz), channels with their caps, device."""
        key = "_api_info_rx" if rx else "_api_info_tx"
        if getattr(self, key, None) is None:
            L = self._line()
            setattr(self, key, L.request(L.address(self.api_rx if rx else self.api), {"op": "info"}))
        return getattr(self, key)

    def has(self, feature):
        """Whether the TX board says it can do feature (its info's features, e.g. "dev_scale")"""
        return feature in (self._api_info().get("features") or {})

    def _api_fail(self, e, what):
        """A BenchLineError from the board: a wait that ran out or a request the board can't serve is a skip."""
        if e.code in ("timeout", "busy", "deadlock"):
            self._busy(f"{what}: {e}")
        if e.code in ("unsupported", "refused", "unavailable"):
            skip("bench", f"the bench board can't do {what}: {e}")
        if e.code == "lost":
            skip("bench", f"the bench board stopped answering during {what}: {e}")
        raise RuntimeError(f"bench board {what} failed: {e}")

    @staticmethod
    def _tx_check(freq):
        """Refuse to key on a frequency outside the rig's tx_mhz (hil_config.py), in a pytest run"""
        import hil_config
        if hil_config.CONFIG is not None and not hil_config.tx_allowed(None, float(freq)):
            raise RuntimeError(f"{freq} MHz is not one of the rig's transmit frequencies (tx_mhz)")

    def _api_claim(self, freq):
        L = self._line()
        try:
            L.request(L.address(self.api), {"op": "claim", "session": self._api_session, "freqs": [freq],
                                            "wait_max": self.wait_max},
                      timeout=self.wait_max + 30)
        except L.BenchLineError as e:
            self._api_fail(e, f"a claim on {freq} MHz")

    def _api_reach(self, freq):
        """The TX board's window moved to freq for the session when its transmitter can't reach freq from where it is
        tuned (info's tx_lo / tx_hi: a board has one synthesizer; both 0: anywhere). The board puts its own window
        back when the session ends."""
        reach = self._api_tx_reach
        if reach is None:
            w = self._api_info().get("window") or {}
            reach = (w.get("tx_lo") or 0, w.get("tx_hi") or 0)
        if reach == (0, 0) or reach[0] <= freq <= reach[1]:
            return
        L = self._line()
        try:
            r = L.request(L.address(self.api), {"op": "tune", "session": self._api_session, "center": freq})
        except L.BenchLineError as e:
            self._api_fail(e, f"a tune to {freq} MHz")
        w = (r.get("tuned") or {}).get("window") or {}
        self._api_tx_reach = (w.get("tx_lo") or freq, w.get("tx_hi") or freq)

    def _api_job(self, tool, args, expect_s=None, wait_start=True, rx=False):
        """A bench_tx / bench_repeater command line as a job on the TX board (rx: the receive board, for a job that
        only listens), started in the background: returns once the board started it (it may have queued behind
        another session's job). The job's records and log lines are in .lines (text, the tool's output) with their
        arrival in .stamps; wait(), kill(), returncode. The frequency's claim stays on the TX board, the session's."""
        L = self._line()
        if self.api_level is not None and tool in ("bench_tx", "bench_repeater"):
            args = f"--power {self.api_level} {args}"
        if self.power_dbm is not None and tool in ("bench_tx", "bench_repeater"):
            args = f"--power-dbm {self.power_dbm:g} {args}"
        if self.dev_scale is not None and tool in ("bench_tx", "bench_repeater"):
            args = f"--dev-scale {self.dev_scale:g} {args}"
        job = L.job_from_args(tool, args)
        for k in ("freq", "tx"):            # bench_tx's frequency, bench_repeater's output
            if job.get(k) is not None:
                self._tx_check(job[k])
                self._api_claim(job[k])
        ambe_dir = None
        if job.get("ambe_out"):             # the board sends the file's bytes back: written where the test asked
            ambe_dir = os.path.dirname(os.path.abspath(os.path.expanduser(job["ambe_out"])))
            os.makedirs(ambe_dir, exist_ok=True)    # a write failing in the reader thread would lose the job's output
            job["ambe_out"] = os.path.basename(job["ambe_out"])
        board = self.api_rx if rx else self.api
        req = {"op": "run", "job": job, "label": self.label or tool,
               "wait_max": self.wait_max}
        if board == self.api:           # the lease's session is the TX board's; another board has none of ours
            req["session"] = self._api_session
        if expect_s:
            req["expect_s"] = round(float(expect_s), 1)
        p = _LineJob(L, L.address(board), req, ambe_dir)
        self.bg.append(p)
        # A TX job says started once it has the modem (it may queue behind another session's job); a listen job
        # starts at once and says nothing until it hears something, so only a refusal is waited for
        p.started.wait(self.wait_max + 30 if wait_start else 1.0)
        if p.req.error is not None:
            self._api_fail(p.req.error, f"{tool} {job.get('cmd', '')}".strip())
        if wait_start and not p.started.is_set():
            # Still queued (or the board never said started): a test going on now would judge a radio with nothing sent
            p.kill()
            raise RuntimeError(f"the bench board never started {tool} {job.get('cmd', '')}: {''.join(p.lines)[-400:]}")
        return p

    # ── a bench board's transmitter (the line protocol's tx op, the gateway's bench-api.md 14.6) ──
    def tx_start(self, mode, freq, program, max_s=10.0, **fields):
        """The TX board's signal mode (fm, am: its info's signal_modes) on freq (MHz), started in the background:
        program is a list of segments ({"kind": "tone", "hz": 1000, "s": 2}, "fsk", "dtmf", "sweep", "noise",
        "silence"), fields the mode's others (FM: ctcss, dcs, dev_hz, sub_dev_hz, lead_s, tail_s, repeat; AM: depth
        ...; None left out). Returns once the board keyed: a _LineTx whose wait() gives the board's done record. Needs
        the lease (tests mark it bench_service and rf). A board without the mode skips."""
        name = mode.upper()
        if not self.allow_rf:
            raise RuntimeError(f"the bench board's {name} transmitter keys up: needs --allow-rf")
        self._tx_check(freq)
        self._need_lease()
        modes = (self._api_info().get("signal_modes") or {}).get("tx", [])
        if mode not in modes:
            skip("bench", f"the bench board {self.api} doesn't transmit {name} (its signal modes: {modes or 'none'})")
        L = self._line()
        self._api_reach(freq)
        self._api_claim(freq)
        req = {"op": "tx", "mode": mode, "freq": freq, "session": self._api_session, "max_s": max_s,
               "program": program, "wait_max": self.wait_max,
               "label": self.label or mode}
        if self.api_level is not None:
            req["power"] = self.api_level
        req.update({k: v for k, v in fields.items() if v is not None})
        t = _LineTx(L, L.address(self.api), req)
        self.bg.append(t)
        end = time.monotonic() + self.wait_max + 30      # it may queue behind another session's job
        while not t.started.wait(0.2) and not t.req.done.is_set() and time.monotonic() < end:
            pass
        e = t.req.error
        if e is not None:
            if "only sends frequency" in str(e):    # a board whose transmitter can't vary its amplitude (AM)
                skip("bench", f"the bench board can't send {name}: {e}")
            self._api_fail(e, f"{name} on {freq} MHz")
        if not t.started.is_set():
            t.terminate()
            raise RuntimeError(f"the bench board never keyed {name} on {freq} MHz: {t.req.replies[-3:]}")
        return t

    def fm_tx_start(self, freq, program, max_s=10.0, **fields):
        """tx_start of FM"""
        return self.tx_start("fm", freq, program, max_s, **fields)

    def fm_transmit(self, freq, program, max_s=10.0, **fields):
        """fm_tx_start, then its done record once the last sample went out"""
        return self.fm_tx_start(freq, program, max_s, **fields).wait(max_s + 15)

    # ── a bench board's confirmed data responder (the line protocol's mssim op, the gateway's bench-api.md 14.2) ──
    def mssim_arm(self, freq, cc, profile, seconds=120):
        """The TX board answers individual confirmed data packets on freq (MHz) as a radio would, per profile (answer,
        slots: the answer's first burst this many 30 ms slots after the end burst's start, end: terminator or block,
        to, from: bench-api.md). It holds the board's transmitter until mssim_disarm() or `seconds`. Needs the lease and
        --allow-rf; a board without the responder skips."""
        if not self.allow_rf:
            raise RuntimeError("the bench board's confirmed data responder keys up: needs --allow-rf")
        self._tx_check(freq)
        self._need_lease()
        L = self._line()
        self._api_claim(freq)
        req = {"op": "mssim", "action": "arm", "session": self._api_session, "freq": freq, "cc": cc,
               "seconds": int(seconds)}
        if self.api_level is not None:
            req["power"] = self.api_level
        req.update(profile)
        try:
            r = L.request(L.address(self.api), req, timeout=60)
        except L.BenchLineError as e:
            self._api_fail(e, f"the confirmed data responder on {freq} MHz")
        self._mssim_since = 0
        return r.get("mssim", r)

    def mssim_log(self):
        """The responder's log entries since the last call: one per confirmed packet it heard (src, dst, ns, heard,
        terminator, answer, sent, failed, turnaround_us, gap_us ...)"""
        L = self._line()
        try:
            r = L.request(L.address(self.api), {"op": "mssim", "action": "status", "session": self._api_session,
                                                "since": getattr(self, "_mssim_since", 0)})
        except L.BenchLineError as e:
            self._api_fail(e, "the confirmed data responder's log")
        m = r.get("mssim", {})
        self._mssim_since = m.get("next", self._mssim_since)
        return m.get("log", [])

    def mssim_disarm(self):
        """Gives the board's transmitter back (errors ignored: the session's end frees it too)"""
        L = self._line()
        try:
            L.request(L.address(self.api), {"op": "mssim", "action": "disarm", "session": self._api_session})
        except L.BenchLineError:
            pass

    def can_fm_rx(self):
        """True when the receive board demodulates FM itself (the rx op, mode fm): what the FM tests measure the radio
        with"""
        return "fm" in (self._api_info(rx=True).get("signal_modes") or {}).get("rx", [])

    def fm_rx_start(self, freq, seconds, **fields):
        """The receive board's FM measurement on freq (MHz) for `seconds` from now, in the background: a _LineTx whose
        wait() gives the done record, its summary the board's figures (carrier_s, cfo_hz, deviation, tone, sub: the
        CTCSS or DCS it decoded). fields: the mode's others (bw, block_s, streams, dtmf, aprs, nominal_dev_hz,
        deemph_us; None left out). Receives only: no claim on the frequency."""
        if not self.can_fm_rx():
            skip("bench", f"the bench board {self.api_rx} has no FM receive (signal modes)")
        self._need_lease()
        L = self._line()
        req = {"op": "rx", "mode": "fm", "freq": freq, "seconds": float(seconds),
               "label": self.label or "fm rx"}
        if self.api_rx == self.api:     # the lease's session is the TX board's; another board has none of ours
            req["session"] = self._api_session
        req.update({k: v for k, v in fields.items() if v is not None})
        t = _LineTx(L, L.address(self.api_rx), req)
        self.bg.append(t)
        end = time.monotonic() + 15
        while not t.started.wait(0.2) and not t.req.done.is_set() and time.monotonic() < end:
            pass
        if t.req.error is not None:
            self._api_fail(t.req.error, f"FM receive on {freq} MHz")
        return t

    def fm_over_start(self, freq, seconds, bw_khz=12.5, audio=False, dtmf=False):
        """The receive board measures FM on freq (MHz) for `seconds` from now, in the background, so the radio can key
        next: 10 ms blocks for the keyed segments, with audio the timed tones and the audio stream (voice figures), the DTMF
        digits with dtmf. Finish with fm_over_result(); the board's analyzers, no I/Q."""
        job = self.fm_rx_start(freq, seconds, bw=25000 if bw_khz > 12.5 else 16000, block_s=FM_BLOCK_S,
                               squelch_over_db=FM_SQUELCH_DB, streams="audio" if audio else None,
                               dtmf=True if dtmf else None, tones=True if audio else None)
        job.seconds = float(seconds)
        job.want_audio = audio
        return job

    def fm_over_result(self, job, timeout=None):
        """fm_measure.measure()'s report of an fm_over_start() job, waiting for its end. Samples the board lost break a
        tone or cut a key-up, so that is a bench fault, a skip."""
        import fm_measure
        done = job.wait(timeout if timeout is not None else job.seconds + 20)
        gap = done.get("gap_samples") or 0
        if gap:
            where = [g["gap"] for g in job.req.replies if "gap" in g][:4]
            skip("bench", f"the bench board lost samples during the FM measurement (gap_samples {gap}, sc0 "
                          f"{(job.header or {}).get('sc0')}, {where}); it can't judge the radio")
        pcm = job.pcm.get("audio")
        if job.want_audio and not pcm:
            skip("bench", f"the bench board {self.api_rx} sent no FM audio stream")
        if job.want_audio and "tones" not in (done.get("summary") or {}):
            skip("bench", f"the bench board {self.api_rx} has no tone analyzer: its gateway needs an update")
        return fm_measure.measure(done, job.records, job.header, pcm)

    def reachable(self):
        # Both boards answer, the TX board lists a frequency it transmits on, the receive board's SDR runs
        if not self.api:
            return False
        try:
            tx = self._api_info()
            rx = self._api_info(rx=True)
        except self._line().BenchLineError:
            return False
        return (any({"sdr_tx", "mmdvm_tx"} & set(c.get("caps", [])) for c in tx.get("channels", []))
                and rx.get("sdr") == "shared")

    @contextlib.contextmanager
    def lease(self):
        """A session on the TX board for the duration: its claims keep other sessions' jobs off the test's frequencies, its
        jobs queue in order. Closing the session's connection ends it, and the board unkeys. Background jobs started
        inside end with it."""
        test = os.environ.get("PYTEST_CURRENT_TEST", "").split(" ")[0] or "hil"
        L = self._line()
        self.label = f"{os.environ['RFOUNDRY_HIL_NAME']} {test}" if os.environ.get("RFOUNDRY_HIL_NAME") else test
        try:
            s = L.Request(L.address(self.api), {"op": "session", "label": self.label, "max_s": self.lease_max})
        except L.BenchLineError as e:
            self._api_fail(e, "a session")
        s.first.wait(30)
        if s.error is not None or not s.replies or "session" not in s.replies[0]:
            s.cancel()
            raise RuntimeError(f"the bench board gave no session: {s.error or s.replies}")
        self._api_session = s.replies[0]["session"]
        self._api_tx_reach = None
        self.leased = True
        try:
            yield
        finally:
            for p in self.bg:
                p.kill()
            self.bg = []
            s.cancel()
            self._api_session = None
            self._api_tx_reach = None
            self.leased = False

    def _need_lease(self):
        if not self.leased:
            raise RuntimeError("the bench board needs a session: take the lease first")

    @staticmethod
    def _busy(e):
        skip("bench", f"the bench board stayed busy: {e}")

    def upload(self, local, name):
        return os.path.abspath(local)       # a job's input is read here and sent to the board

    def run_dir(self, tag):
        """A local directory: what a job writes lands here"""
        import tempfile
        d = tempfile.mkdtemp(prefix=f"rfoundry-hil-{tag}-")
        self._run_dirs.append(d)
        return d

    def run_path(self, run_dir, name=""):
        """Where a file of run_dir() is, for a job's output path or a message"""
        return os.path.join(run_dir, name)

    def remove_run_dirs(self):
        """Delete the run directories this session made. RFOUNDRY_HIL_KEEP_RUNS=1 keeps them for a look."""
        if os.environ.get("RFOUNDRY_HIL_KEEP_RUNS") or not self._run_dirs:
            return
        import shutil
        for d in self._run_dirs:
            shutil.rmtree(d, ignore_errors=True)
        self._run_dirs = []

    # ── the bench modem ──
    def transmit(self, args, freq, cc, timeout=300, expect_s=None, background=False):
        """A bench_tx job with args (a string) on freq (MHz) / cc, which the board builds and sends (a --listen-after
        listens on its receiver); returns its JSON records. Needs the lease and --allow-rf. expect_s: about how long
        it keys up. No --power: the board's own level, unless --bench-api-level gives one. background: return the
        job once it started (wait() / kill() it; the lease's end kills it too)."""
        if not self.allow_rf:
            raise RuntimeError("the bench transmitter keys up: needs --allow-rf")
        self._tx_check(freq)
        self._need_lease()
        p = self._api_job("bench_tx", f"--freq {freq} --cc {cc} {args}", expect_s)
        if background:
            return p
        try:
            p.wait(timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            raise RuntimeError(f"bench_tx {args!r} on the bench board didn't end in {timeout} s")
        if p.req.error is not None:
            self._api_fail(p.req.error, f"bench_tx {args!r}")
        if p.returncode != 0:
            raise RuntimeError(f"bench_tx {args!r} on the bench board failed ({p.returncode}): "
                               f"{''.join(p.lines)[-1500:]}")
        return jsonl("".join(p.out))

    def voice_call(self, freq, cc, dst, superframes=3, calls=1, headers=8, terminators=3, gap_ms=500, ambe=None,
                   private=False, src=None, late=None, talker_alias=None, pi=False, emergency=False,
                   service_options=None, gps=None, offset_hz=0, offset_sweep=None, superframes_per_step=1,
                   background=False):
        """One or more voice calls from the bench (a bench_tx voice job). late=K: no header, the first K bursts left out;
        talker_alias: text in odd superframes; pi: the EMB PI bit on B-F; service_options: hex byte in the LC;
        gps: (lat, lon[, error code]) as an ETSI GPS Info LC in the odd superframes. offset_hz: the carrier that far
        from freq (+-2500 Hz; the lease stays on freq). offset_sweep=(from, to, step) Hz: one call stepping through
        the offsets, superframes_per_step superframes each (superframes ignored), with a "step" record a step.
        background: the started job, as transmit()'s"""
        if offset_sweep:
            lo, hi, st = offset_sweep
            superframes = (int((hi - lo) / st + 1e-9) + 1) * superframes_per_step
            calls = 1
        a = f"voice --dst {dst} --calls {calls} --gap-ms {gap_ms}"
        if offset_sweep:
            a += f" --offset-sweep {lo:g}:{hi:g}:{st:g} --superframes-per-step {superframes_per_step}"
        else:
            a += f" --superframes {superframes}"
        if offset_hz:
            a += f" --offset-hz {offset_hz:g}"
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
        return self.transmit(a, freq, cc, expect_s=calls * bursts * 0.06 + (calls - 1) * gap_ms / 1000,
                             background=background)

    def csbk(self, freq, cc, dst, opcode=None, fid=0, octet3=0, src=None, count=4, listen_after=0):
        """A preamble CSBK train, then optionally one CSBK (a bench_tx csbk job); listen_after: then listen that many
        seconds (JSON lines of what was heard, CSBKs with their "info" octets)."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"csbk --dst {dst} --count {count}"
        if opcode is not None:
            a += f" --opcode {opcode:x} --fid {fid:x} --octet3 {octet3:x}"
        if src:
            a += f" --src {src}"
        return self.transmit(a, freq, cc, expect_s=(count + 1) * 0.06 + listen_after)

    def data(self, freq, cc, dst, tms=None, tms_seq=1, tms_ack=True, confirmed=False, octets=40, preambles=2,
             src=None, listen_after=0, etsi=None, etsi_response=True, ip_text=None, udp_port=None, udp_src_port=None,
             udp_payload=None, repeat=1, repeat_gap_ms=0):
        """One data packet (a bench_tx data job): tms text in IPv4 / UDP 4007 user data, etsi text as ETSI defined short
        data (ISO 8859-1), ip_text as ETSI IP text (UTF-16BE on UDP 5016), udp_port a UDP datagram to that port (SAP 4,
        the full IPv4 header) with udp_payload (bytes) or octets of filler as its payload, else octets of filler.
        repeat: that many packets back to back, each with its own preambles and header, in one key-up (repeat_gap_ms
        0) or that far apart; with udp_port the payload's first 2 octets are each packet's index."""
        a = f"--listen-after {listen_after} " if listen_after else ""
        a += f"data --dst {dst} --preambles {preambles}"
        user = octets
        if etsi is not None:
            a += f" --etsi {shlex.quote(etsi)}" + ("" if etsi_response else " --etsi-no-response")
        elif tms is not None:
            a += f" --tms {shlex.quote(tms)} --tms-seq {tms_seq}" + ("" if tms_ack else " --tms-no-ack")
        elif ip_text is not None:
            a += f" --ip-text {shlex.quote(ip_text)}"
        elif udp_port is not None:
            a += f" --udp-port {int(udp_port)}" + (f" --udp-src-port {int(udp_src_port)}" if udp_src_port else "")
            a += f" --udp-payload {bytes(udp_payload).hex()}" if udp_payload is not None else f" --udp-octets {octets}"
            user = 28 + (len(udp_payload) if udp_payload is not None else octets)
        else:
            a += f" --octets {octets}"
        if repeat > 1:
            a += f" --repeat {int(repeat)} --repeat-gap-ms {int(repeat_gap_ms)}"
        if confirmed:
            a += " --confirmed"
        if src:
            a += f" --src {src}"
        # Key-up time: preambles, the header and the rate 1/2 blocks (the CRC-32 included) of each packet, and the
        # gaps between them; confirmed blocks carry 10 octets
        per = 10 if confirmed else 12
        bursts = preambles + 1 + -(-(user + 4) // per)
        n = max(1, int(repeat))
        return self.transmit(a, freq, cc,
                             expect_s=n * bursts * 0.06 + (n - 1) * repeat_gap_ms / 1000 + listen_after)

    def listen_start(self, freq, cc, seconds, ambe_out=None):
        """A bench_tx listen job in the background (receive only): what the receive board's channel hears, at once (no
        modem to wait for). listen_result() collects its JSON lines."""
        self._need_lease()
        extra = f"--ambe-out {ambe_out} " if ambe_out else ""
        return self._api_job("bench_tx", f"--freq {freq} --cc {cc} {extra}listen --json --seconds {seconds}",
                             seconds, wait_start=False, rx=True)

    def listen_result(self, p, timeout=120):
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
        p.reader.join(5)
        return jsonl("".join(p.lines))

    def repeater_start(self, rx, tx, cc, seconds, extra=""):
        """A bench_repeater job in the background: a duplex repeater with input rx and output tx (MHz);
        repeater_result() collects its JSON events. extra: more of the job's arguments (e.g. --beacon). Needs the lease
        and --allow-rf. A half-duplex bench board refuses the job: a skip."""
        if not self.allow_rf:
            raise RuntimeError("the bench repeater keys up: needs --allow-rf")
        self._tx_check(tx)
        self._need_lease()
        self._api_claim(rx)
        return self._api_job("bench_repeater", f"--rx {rx} --tx {tx} --cc {cc} --seconds {seconds} --json {extra}", seconds)

    repeater_result = listen_result

    def outbound_start(self, freq, cc, seconds):
        """A repeater's idle outbound on freq (MHz) for `seconds` in the background: BS-sync idle bursts on both slots
        with the CACH, no repeater behind it (a bench_tx outbound job). A radio on a split channel
        receiving freq locks to it. Its records (start, one per burst with slot, sc, t; summary) are collected as the
        job's lines; repeater_result() waits for them. Needs the lease and --allow-rf."""
        if not self.allow_rf:
            raise RuntimeError("the bench outbound keys up: needs --allow-rf")
        self._need_lease()
        return self._api_job("bench_tx", f"--freq {freq} --cc {cc} outbound "
                                         f"--seconds {seconds}", seconds)


    # ── DMR bursts decoded on the receive board: no raw I/Q, only one record per burst ──
    def can_watch(self, freq, bw_hz=12500.0):
        """True when dmr_watch_start() can decode bursts on freq (with bw_hz around it): inside the receive board's
        window. Otherwise dmr_watch_start() skips."""
        w = self._api_info(rx=True).get("window", {})
        return bool(w.get("enabled", True)) and w.get("lo", 0) + bw_hz / 2e6 <= freq <= w.get("hi", 0) - bw_hz / 2e6

    def bursts_start(self, freq, seconds, cc=None, bursts=None, until_end=False, envelope=False, floor_db=None):
        """The receive board's decoded bursts on freq for `seconds` (the line protocol's bursts op), collected by a thread;
        returns once it is watching, and bursts_result() waits. envelope: with the carrier's keyed segments too, keyed
        from floor_db (carrier_floor_db()) when given, else from the channel's measured floor. No
        session: receiving never waits."""
        self._need_lease()
        L = self._line()
        req = {"op": "bursts", "freq": freq, "seconds": seconds, "until_end": bool(until_end)}
        if cc is not None:
            req["cc"] = cc
        if bursts:
            req["max_bursts"] = bursts
        if envelope:
            req["envelope"] = True
            if floor_db is not None:
                req["floor_db"] = floor_db
        try:
            r = L.Request(L.address(self.api_rx), req)
        except L.BenchLineError as e:
            self._api_fail(e, f"bursts on {freq} MHz")
        r.seconds = seconds
        r.first.wait(10)                # ready: the board is watching before the transmitter keys
        if r.error is not None and r.error.code != "no_signal":
            self._api_fail(r.error, f"bursts on {freq} MHz")
        return r

    def bursts_result(self, p, timeout=120):
        """(burst records, summary) of a bursts_start(): waits for the watch to end. No bursts is an empty list."""
        # The board ends the stream at its seconds; 5 s past that it is ended here
        if not p.wait(min(timeout, p.seconds + 5)):
            p.cancel()
        if p.error is not None and p.error.code != "no_signal":
            self._api_fail(p.error, "bursts")
        rows = [d["burst"] for d in p.replies if "burst" in d]
        summary = next((d["done"] for d in p.replies if "done" in d), {"bursts": len(rows)})
        # Times from the sample count: a board steps its stream's UTC when it drifts from NTP or GNSS, which moves
        # `t` by hundreds of ms inside a transmission with no burst missing
        fs = self._api_info(rx=True).get("window", {}).get("fs")
        sc0 = rows[0].get("sc") if rows and fs else None
        if sc0 is not None:
            summary = dict(summary, edges=self.edges_from_segments(
                [d["segment"] for d in p.replies if "segment" in d], sc0, fs))
        t0 = rows[0]["t"] if rows else 0.0
        out = []
        for r in rows:
            r = dict(r)
            t_rel = (r["sc"] - sc0) / fs * 1e3 if sc0 is not None and "sc" in r else (r.get("t", t0) - t0) * 1e3
            r["t_rel_ms"] = round(t_rel, 3)
            slot_ab = "AB"[int(round(t_rel / 30.0)) % 2]
            # A board's own label comes from its `t`: a UTC step inside the call would split the call in two
            r["slot_ab"] = slot_ab if sc0 is not None else r.get("slot_ab", slot_ab)
            r.setdefault("voice", str(r.get("kind", "")).startswith("VOICE") or (r.get("sync") or "").endswith("VOICE"))
            for k in ("cfo_hz", "level_dbfs", "dev_hz"):
                if r.get(k) is None:
                    r[k] = 0.0
            r.setdefault("lc", None)
            r["fec"] = r.get("fec") or {"fec_ok": None}
            r["sync"] = r.get("sync") or "EMB"
            out.append(r)
        return out, summary

    # The envelope's keying threshold: the bench board's default over its floor, and how far under the carrier it may
    # go at most (what the capture analyzer allows a close receiver)
    ENVELOPE_ON_DB = 10.0
    CARRIER_BELOW_DB = 30.0

    @staticmethod
    def edges_from_segments(segments, sc0, fs):
        """A board's envelope segments as the edges check_tx reads, in ms from the sample count sc0 (the first burst's), so on the
        bursts' time base. A segment cut by the watch or by lost samples, or with a ramp the board couldn't measure,
        has no on_ms: check_tx doesn't judge it."""
        edges = []
        for s in segments:
            e = {"up_ms": round((s["up_sc"] - sc0) / fs * 1e3, 2), "ramp_up_ms": s.get("ramp_up_ms"),
                 "level_dbfs": s.get("level_dbfs"), "steps": s.get("steps", [])}
            whole = not (s.get("open_start") or s.get("open_end") or s.get("gap"))
            if whole and None not in (s.get("on_ms"), s.get("ramp_up_ms"), s.get("ramp_down_ms")):
                e.update(down_ms=round(e["up_ms"] + s["on_ms"], 2), ramp_down_ms=s["ramp_down_ms"], on_ms=s["on_ms"])
            edges.append(e)
        return edges

    @staticmethod
    def analysis_from_bursts(records, summary):
        """The records as one dict: bursts (t, t_ms, slot, family, voice, kind, cc, lc, foff_hz, cach_ts, sync_errors,
        and on a board's BS bursts cach, the decoded CACH: dmr_cach's dict; `slot` is the A/B position on the 30 ms
        grid), summary (counts of each kind) and meta. The edges and the power / overlap summary come from a watch
        with the envelope."""
        import dmr_cach
        slots = dmr_cach.CachDecoder()
        bursts = []
        for r in records:
            fam = r["sync"].split("_")[0].lower()
            bursts.append({"t_ms": r["t_rel_ms"], "slot": r.get("slot_ab"), "family": fam, "voice": r["voice"],
                           "kind": r["kind"], "cc": None if r["cc"] == -1 else r["cc"],    # -1: a voice sync burst has none
                           "lc": r["lc"], "foff_hz": round(r["cfo_hz"]),
                           "cach_ts": r["slot"] if fam == "bs" else None, "level_dbfs": r["level_dbfs"],
                           "sc": r.get("sc"),     # the board's sample counter (32 bits): its time without clock steps
                           "dev_hz": r["dev_hz"], "fec_ok": r["fec"]["fec_ok"], "t": r.get("t"),
                           "sync_errors": r.get("sync_errors", 0)})
            if r.get("cach"):
                bursts[-1]["cach"] = slots.push(int(r["cach"], 16))
        env = summary.get("envelope")
        out = {"bursts": bursts, "edges": [], "summary": {"counts": dict(collections.Counter(b["kind"] for b in bursts))},
               "meta": {"clip_pct": (env or summary).get("clip_pct", 0)}, "source": "bursts", "bursts_summary": summary}
        if env:
            spacing = []
            for slot in {b["slot"] for b in bursts}:
                t = [b["t_ms"] for b in bursts if b["slot"] == slot]
                spacing += [y - x for x, y in zip(t, t[1:]) if 55 <= y - x <= 65]      # same-slot spacing
            out["edges"] = summary.get("edges", [])
            out["summary"].update(overlaps=env["overlaps"], power_steps=env["power_steps"],
                                  unexplained_carrier=env["unexplained_carrier"])
            if spacing:
                out["summary"]["same_slot_spacing_ms"] = {
                    "n": len(spacing), "mean": round(statistics.fmean(spacing), 3),
                    "sd": round(statistics.pstdev(spacing), 3)}
        return out

    @staticmethod
    def carrier_floor_db(sdr):
        """The floor to key a transmitter's envelope from, given a watch of it: the channel's, raised to
        CARRIER_BELOW_DB + ENVELOPE_ON_DB under the carrier when the receiver is close. Keyed from a floor far under the
        carrier, an envelope follows the transmitter's skirt and the receiver's own quantization, not its power mask."""
        env = sdr["bursts_summary"]["envelope"]
        levels = [e["level_dbfs"] for e in sdr["edges"] if e.get("level_dbfs") is not None]
        if not levels:
            return env["floor_db"]
        return max(env["floor_db"], max(levels) - SdrBench.CARRIER_BELOW_DB - SdrBench.ENVELOPE_ON_DB)

    def dmr_watch_start(self, rdir, freq, seconds, cc=None, name="cap0", waveform=False, floor_db=None):
        """Watch freq for DMR bursts for `seconds`, started before the transmitter keys up, decoded on the bench: only
        JSON comes back. Skips where the bench can't. Finish with dmr_watch_result(). waveform: the keying edges too
        (need_dmr_waveform() first), the board's envelope, keyed from floor_db when given. rdir and name are unused."""
        if waveform:
            self.need_dmr_waveform(freq)
        elif not self.can_watch(freq):
            skip("bench", f"the bench can't decode bursts on {freq} MHz")
        return {"p": self.bursts_start(freq, seconds, cc=cc, envelope=waveform, floor_db=floor_db), "freq": freq, "cc": cc}

    def dmr_watch_wait(self, w, timeout=120):
        """Nothing to wait for here: a burst watch ends in dmr_watch_result()."""

    def dmr_watch_result(self, w, timeout=120):
        """A dmr_watch_start()'s bursts: analysis_from_bursts()."""
        rows, summary = self.bursts_result(w["p"], timeout)
        if summary.get("complete") is False:    # a burst the receiver lost would read as one the radio didn't send
            skip("bench", f"the bench board lost samples while watching bursts ({summary.get('lost')}); "
                          "it can't judge the radio")
        return self.analysis_from_bursts(rows, summary)

    def need_dmr_waveform(self, freq):
        """Skip now, before the radio keys, when the waveform can't be measured on freq: it needs a bench board with
        the envelope and freq inside its receive window."""
        if not self.api:
            skip("bench", "the keying edges come from a bench board's envelope (--bench-api)")
        if "bursts_envelope" not in self._api_info(rx=True).get("features", []):
            skip("bench", f"the bench board {self.api_rx} has no burst envelope (bursts_envelope)")
        if not self.can_watch(freq):
            skip("bench", f"{freq} MHz is outside the bench board's receive window")

    def need_bursts_cach(self, freq):
        """Skip now, before the radio keys, when bursts on freq won't carry their CACH: a bench board's feature."""
        if not self.api:
            skip("bench", "the CACH comes from a bench board's bursts (--bench-api)")
        if "bursts_cach" not in self._api_info(rx=True).get("features", []):
            skip("bench", f"the bench board {self.api_rx} doesn't report the CACH (bursts_cach)")
        if not self.can_watch(freq):
            skip("bench", f"{freq} MHz is outside the bench board's receive window")

    APRS_RX_BW = 25000.0        # Hz: the board's FM channel filter for an AFSK job

    def aprs_rx_start(self, freq, seconds, bw=APRS_RX_BW):
        """The receive board's own Bell 202 / AX.25 decode (an FM rx job with "aprs": true, the gateway's
        bench-api.md 14.6) on freq (MHz) for `seconds` from now, in the background; returns once the board receives,
        so the radio can key next. No raw I/Q comes back. Finish with aprs_rx_result()."""
        job = self.fm_rx_start(freq, seconds, bw=bw, aprs=True)
        job.seconds = float(seconds)
        return job

    def aprs_rx_result(self, job, timeout=None):
        """The frames and segments of an aprs_rx_start() as aprs_afsk_check.py's JSON: waits for the job's end (its seconds plus 20 s at most)."""
        done = job.wait(timeout if timeout is not None else job.seconds + 20)
        res = self.aprs_from_records(job.records, done)
        if res is None:     # a board that doesn't know the field ignores it: no frames would read as none sent
            skip("bench", f"the bench board {self.api_rx} doesn't decode APRS (its FM rx summary has no aprs)")
        return res

    @staticmethod
    def aprs_from_records(records, done):
        """An FM rx job's records and done record (with "aprs": true) as aprs_afsk_check.py's JSON: None when the
        summary has no `aprs` (the board doesn't decode it). Each frame keeps the board's names and units: deviation
        in Hz (peak_dev_hz the 99.9th percentile of the audio, max_dev_hz, rms_dev_hz, mark_dev_hz, space_dev_hz
        each tone's level), mark_hz / space_hz the measured tone frequencies, twist_db mark over space (dB),
        txdelay_flags / txdelay_ms the flags back to back before it. t_start_s / t_end_s are the board's t0 / t1 (s
        from the stream's start, the first and last bit). The board measures neither the carrier to the first frame
        bit (keyup_to_data_ms) nor the phase at the tone changes (phase_*): those keys are absent. segments: the
        runs of blocks with carrier (start, end, mean carrier offset, median power), floor_dbfs the summary's
        noise_dbfs; fcs_bad: the frames between flags whose FCS failed."""
        done = done or {}
        summary = done.get("summary") or {}
        if not isinstance(summary.get("aprs"), dict):
            return None

        def octet(v):
            if isinstance(v, str):
                return int(v, 0) if v.lower().startswith("0x") else int(v, 16)
            return v

        frames = []
        for rec in records:
            a = rec.get("aprs") if isinstance(rec, dict) else None
            if not a:
                continue
            f = {k: v for k, v in a.items() if k not in ("t0", "t1")}
            digis = a.get("digis") or []
            f["digis"] = [d for d in digis.split(",") if d] if isinstance(digis, str) else list(digis)
            f["control"], f["pid"] = octet(a.get("control")), octet(a.get("pid"))
            f["fcs_ok"] = bool(a.get("fcs_ok", True))     # the board sends only frames with a good FCS
            f["t_start_s"], f["t_end_s"] = a.get("t0"), a.get("t1")
            if f.get("txdelay_ms") is None and a.get("txdelay_flags") is not None:
                f["txdelay_ms"] = round(a["txdelay_flags"] * 8 / 1200 * 1000, 1)
            frames.append(f)

        blocks = [r["block"] for r in records if isinstance(r, dict) and isinstance(r.get("block"), dict)]
        step = min((b2["t"] - b1["t"] for b1, b2 in zip(blocks, blocks[1:]) if b2["t"] > b1["t"]), default=0.1)
        segments, run = [], []
        for b in blocks + [None]:
            if b is not None and b.get("carrier"):
                run.append(b)
                continue
            if run:
                cfo = [x["cfo_hz"] for x in run if x.get("cfo_hz") is not None]
                pw = sorted(x["power_dbfs"] for x in run if x.get("power_dbfs") is not None)
                segments.append({"t_start_s": round(run[0]["t"], 3), "t_end_s": round(run[-1]["t"] + step, 3),
                                 "carrier_hz": round(sum(cfo) / len(cfo)) if cfo else 0,
                                 "level_dbfs": round(pw[len(pw) // 2], 1) if pw else None})
                run = []
        return {"source": "board", "seconds": done.get("seconds"), "frames": frames, "segments": segments,
                "floor_dbfs": summary.get("noise_dbfs"), "fcs_bad": summary["aprs"].get("fcs_bad", 0),
                "decoded": summary["aprs"].get("frames", len(frames))}
