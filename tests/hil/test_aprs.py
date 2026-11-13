# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""APRS (docs/spec/aprs.md 13): the debug link side on the radio alone, and on air with the SDR bench as the receiver.

Radio only (no flags): the aprs / aprs_set view sections, a setting round trip, the RAM test callsign, host NMEA as
the position source. Nothing transmits.

On air, AFSK (radio_tx + bench_service: --allow-rf --allow-service-stop, and the rig's callsign, a licensed
operator's: the transmissions are amateur ones; the rig's values are in hil_config.py). The radio sends a position, a
status and a message on the test frequency at its lowest power while a bench board (--bench-api) listens: the board
decodes Bell 202 / AX.25 itself and measures the tones (an FM rx job with "aprs": true); no I/Q comes back. A bench
without the board's FM receive skips.

Where and how it transmits, so that nothing reaches the APRS network:
- Frequency: --aprs-test-mhz, through the custom APRS region, and one of the rig's transmit frequencies (tx_mhz); no
  default. Give an amateur simplex frequency that is not an APRS one: on an APRS frequency, digipeaters and iGates
  would repeat the test frames and gate them to APRS-IS. The regional APRS frequencies (144.390, 144.800, 145.175,
  144.575, 144.660, 144.640) are refused even when tx_mhz lists them. A band with no APRS network nearby also keeps
  the frames away from the bench's other receivers.
- Path: none (no WIDEn-N), so a digipeater that hears a frame does not repeat it.
- Power: the radio's lowest level. The tests check `aprs.tx_power` (the level an APRS
  transmission keys at) is 0 before keying, and skip on firmware without that key: it used to key APRS at the
  settings' default power (1 W on the DM-1701) whatever the radio was set to.
  A fixed APRS power level (`set.aprs_power` not 0) is set to follow the channel for the run and put back after.
- Callsign: --callsign as the RAM-only test call (never saved) with SSID --aprs-ssid (default 15). Destination
  APZRDF.
- Every APRS setting a test changes (transmit, region, custom frequency, SSID, path, send via, position source, fixed
  position, digital destination, UDT) is put back afterwards, after the queued frames and message retries are on
  air (still on the test frequency). If they don't drain, the region stays on the test frequency and the test fails
  saying so, so a leftover frame never goes out on the radio's own APRS frequency.
- Each transmission is short: TX delay 300 ms plus about 0.3 s of frame. The message is sent 3 times (0, 30, 90 s:
  a radio that can't hear the ack), all on the test frequency.

On air, receive (rf + bench_service: --allow-rf, --callsign): a bench board (--bench-api) sends one AX.25 position
frame from <call>-7 (-11 when that is the radio's own call) in Bell 202 AFSK on its FM transmitter, on --fm-mhz (one of the rig's transmit frequencies: an amateur
non-APRS one), path none. The radio, in the FM VFO there with APRS receive on that frequency (custom region), decodes
it; it doesn't transmit (beacons and the digipeater off, put back after).

On air, iGate (rf + bench_service + usb_switch: also --allow-usb-switch): the same frame, heard by the radio in the USB
network personality with the iGate on (iGate transmit off). [aprsis] the test runs a minimal APRS-IS server on the host
(at the host's own address on the USB network, so no NAT), points the radio's APRS-IS client at it over HTTP and reads
the line where it arrives; [kiss_tcp] with no server online the line goes to the KISS TCP client (port 8001,
SetHardware 'G').

On air, DMR (radio_tx + bench_service): on the DMR test channel (--dmr-freq / --dmr-cc) at
the lowest power the radio sends its position as an LRRP report (UDP 4001 in rate 1/2 data) and as an ETSI UDT
location to --aprs-dmr-dst; the bench modem's listener (a bench_tx listen job) hears the data header and blocks, and
the test reassembles and decodes them here.

Pass criteria:
- test_aprs_on_air_position: one frame with a valid FCS from <call>-<ssid> to APZRDF, no digipeaters, control 0x03,
  PID 0xF0, a position within 1e-4 degree of 33.7490, -84.3880; aprs.rf_sent +1; afterwards aprs.rf_active=0, the
  radio idle on its channel (ch.rx_hz, rx_hz, mode unchanged). Deviation peak (99.9th percentile of the
  demodulated audio over the transmission) 2.0-4.5 kHz; |twist| (mark / space tone amplitudes) under 6 dB. Printed,
  not asserted: the mark and space deviation, TX delay (flags before the frame), the tones' frequencies.
- test_aprs_on_air_status: the frame's information field is ">" + the status text.
- test_aprs_on_air_message: ":ADDRESSEE:text{N" with the addressee padded to 9 characters and N a message number
  1-99999; then the retries (aprs.rf_sent +3 in all, aprs.msg_tries 3).
- test_aprs_dmr_position[lrrp|udt]: aprs.dmr_sent +1; a data header from our DMR ID to the destination (CRC ok),
  LRRP: DPF 2 (unconfirmed) carrying IPv4 / UDP to port 4001 with a triggered location report (document 0x0D);
  UDT: DPF 0, format 5 (NMEA location), appended block CRC ok; the position within 2e-5 degree.
- test_aprs_rx_on_air: aprs.rx_frames +1 with aprs.rx_bad unchanged, and aprs.stations at least 1.
- test_aprs_igate_on_air: aprs.igated +1, and the line "<src>>APZRDF,qAR,<radio's call>:<info>" arrives once:
  [aprsis] at the server, after a login as the radio's call with its passcode; [kiss_tcp] on KISS TCP.
"""
import contextlib
import re
import time

import pytest

from dmr_packets import crc16_ccitt, crc32_dmr, data_packets, lrrp_position, udt_position
from rfoundry_hil import DebugLinkError, skip
from hil_config import rig, tx_allowed

POS = (33.7490, -84.3880)
# The APRS network's own frequencies (regional, public): digipeaters and iGates there would repeat and gate the test
# frames. tx_mhz lists every frequency the rig may key on (DMR and FM tests too), so it can't stand in for this
APRS_HZ = {144390000, 144800000, 145175000, 144575000, 144660000, 144640000}


def allowed_test_hz(hz):
    """One of the rig's transmit frequencies (tx_mhz), and not an APRS network frequency"""
    return hz not in APRS_HZ and tx_allowed(None, hz / 1e6)


PATHS = ["", "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE2-1", "WIDE2-2"]
VIAS = {"rf": 0, "dmr": 1, "both": 2}
REGION_CUSTOM, POS_FIXED, PATH_NONE = 6, 3, 0
RF_TRIES, MSG_TRIES = 3, 3
CAPTURE_S = 14       # a message waits for the engine's next service pass: it went out 5.7 s after the request once


def _need_aprs(radio):
    if "APRS" not in radio.features():
        skip("not_built", "built without CONFIG_APRS")


def _aprs(radio):
    v = radio.view(["aprs"])
    v.update(radio.view(["aprs_set"]))
    return v


def _n(v, k):
    return int(v.get(k, 0))


@pytest.mark.feature("aprs")
def test_aprs_view_and_settings(radio):
    """The view sections answer, a setting round-trips through the debug link (and is put back)."""
    _need_aprs(radio)
    v = _aprs(radio)
    assert v.get("aprs.built") == "1", v
    for k in ("aprs.block", "aprs.freq_hz", "aprs.pos_src", "aprs.rf_sent", "set.aprs_tx", "set.aprs_region"):
        assert k in v, k
    old = int(v["set.aprs_ssid"])
    try:
        radio.link.aprs_set("ssid", 9 if old != 9 else 8)
        assert _aprs(radio)["set.aprs_ssid"] == str(9 if old != 9 else 8)
    finally:
        radio.link.aprs_set("ssid", old)


@pytest.mark.feature("aprs")
def test_aprs_test_call_and_nmea(radio, pytestconfig):
    """A RAM test callsign clears the "no callsign" block; host NMEA becomes the position (source "host")."""
    _need_aprs(radio)
    call = rig(pytestconfig, "callsign")
    before = _aprs(radio)
    try:
        radio.link.aprs_test_call(call)
        radio.link.aprs_set("pos_src", 2)       # Computer (host NMEA)
        radio.link.aprs_nmea("$GPRMC,123519,A,3344.940,N,08423.280,W,000.0,000.0,101026,,")
        # pos_valid alone can already be 1 from a fixed position: wait for the source
        v = radio.wait_for(lambda x: x.get("aprs.pos_valid") == "1" and x.get("aprs.pos_src") == "host",
                           sections=("aprs",), what="host position")
        assert v["aprs.pos_src"] == "host" and v["aprs.call"].startswith(call), v
        assert abs(float(v["aprs.lat"]) - 33.749) < 1e-3 and abs(float(v["aprs.lon"]) + 84.388) < 1e-3, v
        assert v["aprs.block"] in ("ok", "not_amateur", "no_afsk"), v
    finally:
        radio.link.aprs_test_call("")
        radio.link.aprs_set("pos_src", int(before.get("set.aprs_pos_src", 0)))


# ── the APRS settings a test changes, put back afterwards ───────────────────

class AprsSession:
    """Snapshot of every APRS setting the on-air tests touch; restore() drains what was queued first."""

    def __init__(self, radio):
        self.radio = radio
        L = radio.link
        v = _aprs(radio)
        # a message from an earlier test keeps msg_pending for its tries and ack wait: tries at 0, 30 and 90 s, then
        # 120 s for the ack, 210 s in all when that test stopped (a skip) right after its first try
        end = time.monotonic() + 225
        while (v["aprs.rf_active"] != "0" or v["aprs.msg_pending"] != "0") and time.monotonic() < end:
            time.sleep(2.0)
            v = _aprs(radio)
        assert v["aprs.rf_active"] == "0" and v["aprs.msg_pending"] == "0", \
            f"APRS busy before the test (rf_active / msg_pending): {v}"
        self.keep = {"tx": _n(v, "set.aprs_tx"), "region": _n(v, "set.aprs_region"), "ssid": _n(v, "set.aprs_ssid"),
                     "pos_src": _n(v, "set.aprs_pos_src"), "dmr_dst": _n(v, "set.aprs_dmr_dst"),
                     "dmr_udt": _n(v, "set.aprs_dmr_udt"), "via": VIAS[v["set.aprs_via"]],
                     "path": PATHS.index(v["set.aprs_path"]), "rx": _n(v, "set.aprs_rx"),
                     "digi": _n(v, "set.aprs_digi"), "power": _n(v, "set.aprs_power"),
                     "ambiguity": _n(v, "set.pos_ambiguity")}
        self.v0 = v
        self.expected_rf = 0
        self.message = False
        self.changed = set()
        # automatic beacons off while the region is moved about (restored)
        if self.keep["tx"]:
            self.set("tx", 0)
        # the fixed position and the custom frequency aren't in aprs_set: read them through the aprs view
        self.set("pos_src", POS_FIXED)
        x = radio.wait_for(lambda y: y.get("aprs.pos_src") == "fixed" or y.get("aprs.pos_valid") == "0",
                           sections=("aprs",), what="fixed position source")
        self.fixed = (round(float(x["aprs.lat"]) * 1e7), round(float(x["aprs.lon"]) * 1e7)) \
            if x.get("aprs.pos_valid") == "1" else (0, 0)
        self.changed |= {"lat", "lon"}
        self.custom_hz = None
        L.aprs_test_call("")

    def set(self, field, value):
        self.radio.link.aprs_set(field, value)
        self.changed.add(field)

    def rf_setup(self, call, ssid, hz):
        """AFSK on hz (custom region), no path, via RF, our test call and SSID, the fixed test position."""
        assert allowed_test_hz(hz), f"{hz} Hz: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)"
        self.set("region", REGION_CUSTOM)
        time.sleep(0.3)             # the view's frequency follows on the engine's next tick (25 ms)
        self.custom_hz = int(self.radio.view(["aprs"])["aprs.freq_hz"])
        self.set("freq_hz", hz)
        self.set("ssid", ssid)
        self.set("path", PATH_NONE)
        self.set("via", VIAS["rf"])
        self.position()
        self.radio.link.aprs_test_call(call)
        v = self.radio.wait_for(lambda y: y.get("aprs.call") == f"{call.upper()}-{ssid}" and
                                int(y.get("aprs.freq_hz", 0)) == hz, sections=("aprs",), what="test call, frequency")
        assert v["aprs.block"] == "ok" and int(v["aprs.freq_hz"]) == hz, v
        return v

    def rx_setup(self, hz):
        """Receive on hz (custom region), the digipeater off: the radio never repeats what the bench sends"""
        assert allowed_test_hz(hz), f"{hz} Hz: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)"
        self.set("digi", 0)
        self.set("region", REGION_CUSTOM)
        time.sleep(0.3)             # the view's frequency follows on the engine's next tick (25 ms)
        self.custom_hz = int(self.radio.view(["aprs"])["aprs.freq_hz"])
        self.set("freq_hz", hz)
        self.set("rx", 1)
        return self.radio.wait_for(lambda y: int(y.get("aprs.freq_hz", 0)) == hz and y.get("set.aprs_rx") == "1",
                                   sections=("aprs", "aprs_set"), what="APRS receive on the test frequency")

    def position(self):
        self.set("lat", POS[0])
        self.set("lon", POS[1])
        self.set("pos_src", POS_FIXED)

    def _drained(self, v):
        sent = _n(v, "aprs.rf_sent") - _n(self.v0, "aprs.rf_sent")
        msg_done = not self.message or v.get("aprs.msg_pending") == "0" or _n(v, "aprs.msg_tries") >= MSG_TRIES
        return v.get("aprs.rf_active") == "0" and msg_done and sent >= self.expected_rf

    def restore(self, timeout=150):
        L = self.radio.link
        end = time.monotonic() + timeout
        v = _aprs(self.radio)
        while not self._drained(v) and time.monotonic() < end:
            time.sleep(1.0)
            v = _aprs(self.radio)
        if not self._drained(v):
            sent = _n(v, "aprs.rf_sent") - _n(self.v0, "aprs.rf_sent")
            tries = (_n(v, "aprs.rf_failed") + _n(v, "aprs.rf_busy") -
                     _n(self.v0, "aprs.rf_failed") - _n(self.v0, "aprs.rf_busy"))
            dropped = v.get("aprs.rf_active") == "0" and tries >= RF_TRIES * (self.expected_rf - sent) and \
                (not self.message or _n(v, "aprs.msg_tries") >= MSG_TRIES)
            if not dropped:
                L.aprs_test_call("")
                raise AssertionError("APRS frames still queued after the test: the APRS settings stay on the test "
                                     f"frequency so they can't go out on the APRS frequency; view {v}")
        L.aprs_test_call("")
        k = self.keep
        if {"lat", "lon"} & self.changed:
            L.aprs_set("lat", self.fixed[0])
            L.aprs_set("lon", self.fixed[1])
        for f in ("ssid", "path", "via", "dmr_dst", "dmr_udt", "pos_src", "rx", "digi", "power", "ambiguity"):
            if f in self.changed:
                L.aprs_set(f, k[f])
        if self.custom_hz is not None:
            L.aprs_set("freq_hz", self.custom_hz)          # region still custom here
        if "region" in self.changed:
            L.aprs_set("region", k["region"])
        if "tx" in self.changed:
            L.aprs_set("tx", k["tx"])
        v = _aprs(self.radio)
        back = {"tx": _n(v, "set.aprs_tx"), "region": _n(v, "set.aprs_region"), "ssid": _n(v, "set.aprs_ssid"),
                "pos_src": _n(v, "set.aprs_pos_src"), "dmr_dst": _n(v, "set.aprs_dmr_dst"),
                "dmr_udt": _n(v, "set.aprs_dmr_udt"), "via": VIAS[v["set.aprs_via"]],
                "path": PATHS.index(v["set.aprs_path"]), "rx": _n(v, "set.aprs_rx"), "digi": _n(v, "set.aprs_digi"),
                "power": _n(v, "set.aprs_power"), "ambiguity": _n(v, "set.pos_ambiguity")}
        assert back == k, f"APRS settings not restored: {back} != {k}"


@contextlib.contextmanager
def aprs_session(radio):
    s = AprsSession(radio)
    try:
        yield s
    finally:
        s.restore()


# ── decoding helpers ─────────────────────────────────────────────────────────

def b91(s):
    v = 0
    for c in s:
        v = v * 91 + ord(c) - 33
    return v


def parse_position(info):
    """(lat, lon) of an APRS position without timestamp ('!' or '='), plain or compressed; None otherwise."""
    if not info or info[0] not in "!=":
        return None
    b = info[1:]
    if b[:1].isdigit():
        m = re.match(r"(\d\d)(\d\d\.\d\d)([NS]).(\d\d\d)(\d\d\.\d\d)([EW])", b)
        if not m:
            return None
        lat = int(m.group(1)) + float(m.group(2)) / 60
        lon = int(m.group(4)) + float(m.group(5)) / 60
        return (lat if m.group(3) == "N" else -lat, lon if m.group(6) == "E" else -lon)
    if len(b) < 10:
        return None
    return 90 - b91(b[1:5]) / 380926.0, -180 + b91(b[5:9]) / 190463.0


@pytest.fixture
def aprs_air(radio, pytestconfig, request):
    """The radio ready to send APRS AFSK on the test frequency at its lowest power; the settings restored after.
    Parameter (indirect): where the radio sits before it leaves for the APRS frequency, "channel" (the current one;
    the DMR test channel on the bench) or "fm_vfo" (the FM VFO on --fm-mhz, receive only there)."""
    start = getattr(request, "param", "channel")
    _need_aprs(radio)
    call = rig(pytestconfig, "callsign", "an amateur transmission: needs a licensed operator's callsign,")
    if "APRS_AFSK" not in radio.features():
        skip("not_built", "built without CONFIG_APRS_AFSK (APRS on FM)")
    hz = round(rig(pytestconfig, "aprs-test-mhz") * 1e6)
    if not allowed_test_hz(hz):
        pytest.fail(f"--aprs-test-mhz {hz / 1e6:.4f}: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    with contextlib.ExitStack() as stack:
        if start == "fm_vfo":
            if "VFO_FM" not in radio.features():
                skip("not_built", "built without the FM VFO")
            stack.enter_context(radio.vfo_session())
            fm_hz = round(rig(pytestconfig, "fm-mhz") * 1e6)
            radio.link.vfo_set(fm_hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(fm_hz) and x.get("mode") == "fm", what="FM VFO")
        yield from _aprs_air(radio, pytestconfig, call, hz)


def _aprs_air(radio, pytestconfig, call, hz):
    radio.lowest_power(absolute=True)     # an APRS test frequency is a ham one: the lowest, whatever --radio-tx-watts
    v = radio.view(["aprs"])
    if "aprs.tx_power" not in v:
        skip("not_built", "this firmware keys APRS at the settings' default power (1 W on the DM-1701), not the "
                          "radio's lowest: needs the aprs.tx_power fix")
    ssid = pytestconfig.getoption("--aprs-ssid")
    with aprs_session(radio) as s:
        if v["aprs.tx_power"] != "0" and "set.aprs_power" in s.v0:
            # A fixed APRS power level: follow the channel's (now the lowest) for the run; restore() puts it back
            s.set("power", 0)
            v = radio.wait_for(lambda x: x.get("aprs.tx_power") == "0", sections=("aprs",),
                               what="APRS power following the channel")
        if v["aprs.tx_power"] != "0":
            skip("state", f"APRS would key at power level {v['aprs.tx_power']}, not the lowest, and this firmware "
                          "can't set its APRS power over the link")
        assert radio.core()["power"] == "0", f"not at the lowest power: {radio.core()}"
        s.rf_setup(call, ssid, hz)
        s.call, s.ssid, s.hz = call.upper(), ssid, hz
        yield s


def send_and_capture(radio, sdr_bench, s, send):
    """The receive board listens while send() queues one frame; wait for it on air and the radio back on its channel;
    the board's own AFSK decode."""
    core0, ch0 = radio.core(), radio.view(["channel"])
    v0 = _aprs(radio)
    with sdr_bench.lease():
        job = sdr_bench.aprs_rx_start(s.hz / 1e6, seconds=CAPTURE_S)
        send()
        s.expected_rf += 1
        v = radio.wait_for(lambda x: _n(x, "aprs.rf_sent") > _n(v0, "aprs.rf_sent") or
                           _n(x, "aprs.rf_busy") > _n(v0, "aprs.rf_busy") or
                           _n(x, "aprs.rf_failed") > _n(v0, "aprs.rf_failed"),
                           timeout=15, sections=("aprs",), what="APRS transmission")
        v = radio.wait_for(lambda x: x.get("aprs.rf_active") == "0", timeout=8, sections=("aprs",),
                           what="APRS transmission over")
        res = sdr_bench.aprs_rx_result(job)
    print("aprs view:", {k: v[k] for k in ("aprs.rf_sent", "aprs.rf_busy", "aprs.rf_failed", "aprs.last_info")
                         if k in v})
    assert _n(v, "aprs.rf_sent") == _n(v0, "aprs.rf_sent") + 1, \
        f"not sent (rf_busy: the channel was busy for 10 s; rf_failed: aborted): {v}"
    core, ch = radio.wait_for(lambda x: x.get("state") == "idle", timeout=3, what="idle"), radio.view(["channel"])
    for k in ("rx_hz", "mode", "zone_pos", "vfo"):
        assert core.get(k) == core0.get(k), f"{k}: {core0.get(k)} -> {core.get(k)}"
    assert ch.get("ch.rx_hz") == ch0.get("ch.rx_hz"), (ch0, ch)
    if "fcs_bad" in res:
        print(f"bench board: {res['decoded']} frames decoded, {res['fcs_bad']} with a bad FCS")
    for seg in res.get("segments", []):
        print(f"keyed {seg['t_start_s']:.2f}-{seg['t_end_s']:.2f} s, carrier {seg['carrier_hz']:+d} Hz, "
              f"{seg['level_dbfs']} dBFS (floor {res.get('floor_dbfs')})")
    ours = [f for f in res["frames"] if f["src"] == f"{s.call}-{s.ssid}"]
    for f in res["frames"]:
        print_frame(f)
    assert len(ours) == 1, f"expected one frame from {s.call}-{s.ssid} with a valid FCS, got {res['frames']} " \
                           f"(segments {res.get('segments')})"
    f = ours[0]
    assert f["fcs_ok"] and f["dst"] == "APZRDF" and f["digis"] == [], f
    assert f["control"] == 0x03 and f["pid"] == 0xF0, f
    return f


def print_frame(f):
    """The frame and its measurements"""
    print(f"decoded: {f['tnc2']}")
    print(f"  TX delay {f['txdelay_flags']} flags ({f['txdelay_ms']} ms)")
    if "mark_dev_hz" in f:
        print(f"  deviation peak {f['peak_dev_hz']:.0f} Hz (99.9 %), max {f['max_dev_hz']:.0f} Hz; mark "
              f"{f['mark_dev_hz']:.0f} Hz, space {f['space_dev_hz']:.0f} Hz, twist {f['twist_db']:+.2f} dB")
    if f.get("mark_hz") is not None and f.get("space_hz") is not None:
        print(f"  tones {f['mark_hz']:.1f} Hz mark, {f['space_hz']:.1f} Hz space")


def check_tones(f):
    assert "mark_dev_hz" in f, f"tone levels not measured: {f}"
    assert 2000 <= f["peak_dev_hz"] <= 4500, f"peak deviation {f['peak_dev_hz']:.0f} Hz outside 2.0-4.5 kHz"
    assert abs(f["twist_db"]) < 6, f"twist {f['twist_db']:+.1f} dB (mark {f['mark_dev_hz']:.0f} Hz, space " \
                                   f"{f['space_dev_hz']:.0f} Hz)"


@pytest.mark.feature("aprs")
@pytest.mark.feature("aprs_afsk")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.parametrize("aprs_air", ["channel", "fm_vfo"], indirect=True)
def test_aprs_on_air_position(radio, sdr_bench, aprs_air):
    """One position at 50 mW on the test frequency, decoded off air; deviation, twist, phase, TX delay printed."""
    f = send_and_capture(radio, sdr_bench, aprs_air, radio.link.aprs_send_now)
    pos = parse_position(f["info"])
    assert pos is not None, f"not a position: {f['info']!r}"
    assert abs(pos[0] - POS[0]) < 1e-4 and abs(pos[1] - POS[1]) < 1e-4, f"position {pos}, sent {POS}"
    check_tones(f)


@pytest.mark.feature("aprs")
@pytest.mark.feature("aprs_afsk")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_aprs_on_air_status(radio, sdr_bench, aprs_air):
    text = "RadioFoundry HIL status test"
    f = send_and_capture(radio, sdr_bench, aprs_air, lambda: radio.link.aprs_status(text))
    assert f["info"] == ">" + text, f
    check_tones(f)


@pytest.mark.feature("aprs")
@pytest.mark.feature("aprs_afsk")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_aprs_on_air_message(radio, sdr_bench, aprs_air):
    """A message with a message number; then its two retries (no ack can come back to the DM-1701)."""
    to, text = f"{aprs_air.call}-1", "RadioFoundry HIL message test"
    aprs_air.message = True
    f = send_and_capture(radio, sdr_bench, aprs_air, lambda: radio.link.aprs_message(to, text))
    m = re.fullmatch(r":(.{9}):(.*)\{(\d{1,5})", f["info"])
    assert m, f"not a message with a number: {f['info']!r}"
    assert m.group(1).rstrip() == to and m.group(2) == text and 1 <= int(m.group(3)) <= 99999, m.groups()
    check_tones(f)
    v0 = _aprs(radio)
    aprs_air.expected_rf += 2
    v = radio.wait_for(lambda x: _n(x, "aprs.msg_tries") >= MSG_TRIES and x.get("aprs.rf_active") == "0" and
                       _n(x, "aprs.rf_sent") >= _n(v0, "aprs.rf_sent") + 2, timeout=120, interval=1.0,
                       sections=("aprs",), what="the message's retries")
    print(f"message {m.group(3)}: tries {v['aprs.msg_tries']}, acked {v['aprs.msg_acked']}")


@pytest.mark.feature("aprs")
@pytest.mark.feature("aprs_afsk")
@pytest.mark.radio_tx
@pytest.mark.parametrize("over", ["serial", pytest.param("network", marks=pytest.mark.usb_switch),
                                  pytest.param("kiss_tcp", marks=pytest.mark.usb_switch)])
def test_aprs_tx_link_answers(radio, aprs_air, over, pytestconfig):
    """The debug link keeps answering while the radio sends a frame, over the serial port and over the USB network:
    the host link task never waits on the transmission (a stall here took the USB network down with it)."""
    with contextlib.ExitStack() as stack:
        if over != "serial":
            if not pytestconfig.getoption("--allow-usb-switch"):
                skip("opt_in", "needs --allow-usb-switch")
            from test_net_services import network_mode
            serial = radio.link
            udp, ip, _ = stack.enter_context(network_mode(radio))
            radio.link = udp
            stack.callback(lambda: setattr(radio, "link", serial) if radio.link is udp else None)
        send = radio.link.aprs_send_now
        if over == "kiss_tcp":
            # The same frame from a KISS client on TCP 8001 (CONFIG_NET_KISS), sourced from our own callsign
            if "NET_KISS" not in radio.features():
                skip("not_built", "built without CONFIG_NET_KISS")
            import socket
            from test_tcp_services import _kiss, _ui_frame
            if radio.view(["aprs_set"]).get("set.aprs_kiss", "0") != "1":
                radio.link.aprs_set("kiss", 1)
                stack.callback(lambda: radio.link.aprs_set("kiss", 0))
            s = stack.enter_context(socket.create_connection((ip, 8001), timeout=5))
            time.sleep(0.3)
            frame = _kiss(_ui_frame(aprs_air.call, aprs_air.ssid, b">RFOUNDRY HIL KISS-TCP"))
            send = lambda: s.sendall(frame)  # noqa: E731
        _link_answers(radio, aprs_air, send)


def _link_answers(radio, aprs_air, send):
    v0 = _aprs(radio)
    send()
    aprs_air.expected_rf += 1
    worst, t_last, keyed = 0.0, time.monotonic(), False
    end = time.monotonic() + 10
    while time.monotonic() < end:
        try:
            radio.link.ping()
        except DebugLinkError:
            pass
        now = time.monotonic()
        worst, t_last = max(worst, now - t_last), now
        v = radio.view(["aprs"])
        keyed = keyed or v.get("aprs.rf_active") != "0"
        if _n(v, "aprs.rf_sent") > _n(v0, "aprs.rf_sent") and v.get("aprs.rf_active") == "0":
            break
    print(f"longest gap between debug link answers: {worst * 1000:.0f} ms")
    assert _n(radio.view(["aprs"]), "aprs.rf_sent") > _n(v0, "aprs.rf_sent"), "the frame never went out"
    assert worst < 0.5, f"the debug link stopped answering for {worst * 1000:.0f} ms during the transmission"


# ── digital APRS: positions over DMR ────────────────────────────────────────

@pytest.mark.feature("dmr_lrrp")
@pytest.mark.feature("aprs", optional=True)
@pytest.mark.feature("aprs_dmr")
@pytest.mark.needs_switch("APRS_DMR")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("form", ["lrrp", "udt"])
def test_aprs_dmr_position(radio, sdr_bench, pytestconfig, form):
    """The position as an LRRP report or a UDT location over DMR at the lowest power, decoded from the bench modem."""
    _need_aprs(radio)
    feats = radio.features()
    if "APRS_DMR" not in feats:
        skip("not_built", "built without CONFIG_APRS_DMR (APRS over DMR)")
    if form == "lrrp" and "DMR_LRRP" not in feats:
        skip("not_built", "built without CONFIG_DMR_LRRP")
    freq, cc = rig(pytestconfig, "dmr-freq"), rig(pytestconfig, "dmr-cc")
    dst = rig(pytestconfig, "aprs-dmr-dst")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    own = int(radio.view(["dmr"])["dmr.own_id"])
    if own == 0:
        skip("state", "the radio has no DMR ID")
    time.sleep(1.0)
    with aprs_session(radio) as s:
        s.set("via", VIAS["dmr"])
        s.set("dmr_dst", dst)
        s.set("dmr_udt", 1 if form == "udt" else 0)
        s.set("ambiguity", 0)       # a DMR report gets the ambiguity's blur (position_blur): the exact position here
        s.position()
        v0 = _aprs(radio)
        with sdr_bench.lease():
            lis = sdr_bench.listen_start(freq, cc, seconds=10)
            radio.link.aprs_send_now()
            v = radio.wait_for(lambda x: _n(x, "aprs.dmr_sent") > _n(v0, "aprs.dmr_sent") or
                               _n(x, "aprs.dmr_failed") > _n(v0, "aprs.dmr_failed"), timeout=10,
                               sections=("aprs",), what="DMR position sent")
            heard = sdr_bench.listen_result(lis)
    assert _n(v, "aprs.dmr_sent") == _n(v0, "aprs.dmr_sent") + 1, f"DMR position not sent: {v}"
    pk = [p for p in data_packets(heard) if p["src"] == own and p["dst"] == dst]
    print(f"data headers heard: {[(p['src'], p['dst'], p['dpf'], len(p['blocks'])) for p in data_packets(heard)]}")
    assert pk, f"no data header from {own} to {dst} heard by the bench modem: {heard}"
    p = pk[0]
    assert p["crc_ok"], p
    assert len(p["blocks"]) == p["need"], f"{len(p['blocks'])} of {p['need']} blocks heard: {p}"
    if form == "lrrp":
        assert p["dpf"] == 2, p
        user = b"".join(p["blocks"])
        body, crc = user[:-4], int.from_bytes(user[-4:], "little")
        assert crc32_dmr(body) == crc, "packet CRC-32"
        lat, lon = lrrp_position(body[:len(body) - p["pad"]] if p["pad"] else body)
    else:
        assert p["dpf"] == 0 and p["fmt"] == 5, p
        blk = p["blocks"][0]
        assert crc16_ccitt(blk, 0x00) == int.from_bytes(blk[10:12], "big"), "UDT block CRC"
        valid, lat, lon = udt_position(blk)
        assert valid, blk.hex()
    print(f"{form} position from {own} to {dst}: {lat:.6f}, {lon:.6f}")
    assert abs(lat - POS[0]) < 2e-5 and abs(lon - POS[1]) < 2e-5, f"{lat}, {lon} != {POS}"


BENCH_RX_SSIDS = (7, 11)    # the bench's frame: --callsign with the first that isn't the radio's own call
BENCH_RX_INFO = "!3330.00N/08645.00W-RadioFoundry bench frame"


def bench_frame(call, own, info=BENCH_RX_INFO):
    """The bench's frame: (its source, the bench board's FSK program). The source is call-7, or -11 when that is the
    radio's own call (the radio skips its own packets)"""
    import aprs_afsk_check as afsk
    src = next(c for c in (f"{call.upper()}-{n}" for n in BENCH_RX_SSIDS) if c != own)
    bits = afsk.hdlc_bits([afsk.ax25_frame(src, "APZRDF", [], info)], 40)     # 40 flags: 267 ms TX delay
    packed = bits + "0" * (-len(bits) % 8)
    hexs = "".join(f"{int(packed[i:i + 8], 2):02x}" for i in range(0, len(packed), 8))   # MSB first, as sent
    return src, [{"kind": "fsk", "hex": hexs, "bits": len(bits), "nrzi": True, "level": 1.0}]


@pytest.mark.feature("aprs_rx")
@pytest.mark.rf
@pytest.mark.bench_service
def test_aprs_rx_on_air(radio, sdr_bench, pytestconfig):
    """One AX.25 UI frame (a position, from --callsign-7 or -11, not the radio's own call) in Bell 202 AFSK from the bench board's FM transmitter on
    --fm-mhz: the radio, in the FM VFO there with APRS receive on that frequency, decodes it (aprs.rx_frames) with no
    FCS error and lists the station. The radio doesn't transmit: beacons and the digipeater are off for the test."""
    _need_aprs(radio)
    if "APRS_RX" not in radio.features() or "VFO_FM" not in radio.features():
        skip("not_built", "built without APRS receive or the FM VFO")
    call = rig(pytestconfig, "callsign", "an amateur transmission: needs a licensed operator's callsign,")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    if not allowed_test_hz(hz):
        pytest.fail(f"--fm-mhz {mhz:.4f}: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    src, program = bench_frame(call, _aprs(radio).get("aprs.call"))
    L = radio.link
    with radio.vfo_session(), aprs_session(radio) as s:
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO")
        s.rx_setup(hz)
        radio.wait_for(lambda v: v.get("aprs.rx_listening") == "1", sections=("aprs",), what="APRS receiver on")
        v0 = _aprs(radio)
        with sdr_bench.lease():
            done = sdr_bench.fm_transmit(mhz, program, dev_hz=3000, lead_s=0.1, tail_s=0.1)
        print(f"bench: {done}")
        try:
            v = radio.wait_for(lambda x: _n(x, "aprs.rx_frames") > _n(v0, "aprs.rx_frames"), timeout=5,
                               sections=("aprs",), what="the bench's frame decoded")
        except AssertionError:
            v = _aprs(radio)
            raise AssertionError("no frame decoded: " + " ".join(
                f"{k}={_n(v0, k)}->{_n(v, k)}" for k in ("aprs.rx_bits", "aprs.rx_hdlc_frames", "aprs.rx_fcs_errors",
                                                         "aprs.rx_aborts", "aprs.rx_bad", "aprs.rx_peak")))
        print(" ".join(f"{k}={_n(v0, k)}->{_n(v, k)}" for k in ("aprs.rx_frames", "aprs.rx_fcs_errors",
                                                               "aprs.rx_bad", "aprs.stations", "aprs.rx_peak")))
        assert _n(v, "aprs.rx_frames") == _n(v0, "aprs.rx_frames") + 1, f"one frame expected: {v}"
        assert _n(v, "aprs.rx_bad") == _n(v0, "aprs.rx_bad"), f"the frame was refused (rx_bad): {v}"
        assert _n(v, "aprs.stations") >= 1, f"the station isn't listed: {v}"


@pytest.mark.feature("aprs_rx")
@pytest.mark.rf
@pytest.mark.bench_service
def test_aprs_rx_third_party_message(radio, sdr_bench, pytestconfig):
    """A message to the radio wrapped as an iGate puts APRS-IS traffic on RF ("}SRC>DST,TCPIP,IGATE*:info", sent by
    --callsign-7 or -11 as the iGate, from --callsign-12 inside), from the bench board's FM transmitter on --fm-mhz:
    the radio keeps it as from the inner sender (aprs.msg_from, msg_text). No message number, so no ack: the radio doesn't transmit."""
    _need_aprs(radio)
    if "APRS_RX" not in radio.features() or "VFO_FM" not in radio.features():
        skip("not_built", "built without APRS receive or the FM VFO")
    call = rig(pytestconfig, "callsign", "an amateur transmission: needs a licensed operator's callsign,")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    if not allowed_test_hz(hz):
        pytest.fail(f"--fm-mhz {mhz:.4f}: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    own = _aprs(radio).get("aprs.call")
    if not own:
        skip("state", "the radio has no APRS callsign")
    inner = f"{call.upper()}-12"
    if inner == own:
        skip("state", f"the radio's own call is {own}, the test's inner sender")
    gate = next(c for c in (f"{call.upper()}-{n}" for n in BENCH_RX_SSIDS) if c != own)
    text = f"RadioFoundry HIL third party {int(time.time()) % 100000}"     # new each run: not the inbox's last one
    info = f"}}{inner}>APZRDF,TCPIP,{gate}*::{own:<9}:{text}"
    src, program = bench_frame(call, own, info)
    L = radio.link
    with radio.vfo_session(), aprs_session(radio) as s:
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz) and v.get("mode") == "fm", what="FM VFO")
        s.rx_setup(hz)
        radio.wait_for(lambda v: v.get("aprs.rx_listening") == "1", sections=("aprs",), what="APRS receiver on")
        v0 = _aprs(radio)
        with sdr_bench.lease():
            done = sdr_bench.fm_transmit(mhz, program, dev_hz=3000, lead_s=0.1, tail_s=0.1)
        print(f"bench: {src}: {info}: {done}")
        v = radio.wait_for(lambda x: _n(x, "aprs.rx_frames") > _n(v0, "aprs.rx_frames"), timeout=5,
                           sections=("aprs",), what="the bench's frame decoded")
        v = radio.wait_for(lambda x: x.get("aprs.msg_text") == text, timeout=2, sections=("aprs",),
                           what="the wrapped message kept")
        assert v.get("aprs.msg_from") == inner, f"the message isn't from the inner sender {inner}: {v}"
        assert _n(v, "aprs.rf_sent") == _n(v0, "aprs.rf_sent"), f"the radio transmitted: {v}"


def kiss_frames(data):
    """The frames in a KISS byte stream, unescaped (type byte first)"""
    out, cur, esc = [], bytearray(), False
    for b in data:
        if b == 0xC0:
            if cur:
                out.append(bytes(cur))
            cur, esc = bytearray(), False
        elif esc:
            cur.append({0xDC: 0xC0, 0xDD: 0xDB}.get(b, b))
            esc = False
        elif b == 0xDB:
            esc = True
        else:
            cur.append(b)
    return out


class AprsIsServer:
    """A minimal APRS-IS server on the host (tcp-services.md 7: "a local relay on the host"): a banner, "# logresp
    CALL verified" to any login, a comment every 15 s, and every line it receives kept in .lines. One client at a
    time, on a free port; the radio reaches it at the host's own address on the USB network."""

    def __init__(self):
        import socket
        import threading
        self.lines, self.port = [], 0
        self._stop = threading.Event()
        self._srv = socket.socket()
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("0.0.0.0", 0))
        self._srv.listen(2)
        self._srv.settimeout(0.5)
        self.port = self._srv.getsockname()[1]
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        import socket
        while not self._stop.is_set():
            try:
                c, _ = self._srv.accept()
            except socket.timeout:
                continue
            c.settimeout(0.5)
            buf, last = b"", time.monotonic()
            try:
                c.sendall(b"# RadioFoundry HIL APRS-IS\r\n")
                while not self._stop.is_set():
                    try:
                        d = c.recv(512)
                        if not d:
                            break
                        buf += d
                    except socket.timeout:
                        pass
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip().decode("latin-1")
                        self.lines.append(line)
                        if line.startswith("user "):
                            c.sendall(f"# logresp {line.split()[1]} verified, server HIL\r\n".encode())
                    if time.monotonic() - last > 15:
                        c.sendall(b"# keepalive\r\n")
                        last = time.monotonic()
            except OSError:
                pass
            finally:
                c.close()

    def close(self):
        self._stop.set()
        self._thread.join(5)
        self._srv.close()


@pytest.mark.feature("aprs_igate")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.usb_switch
@pytest.mark.parametrize("to", ["aprsis", "kiss_tcp"])
def test_aprs_igate_on_air(radio, sdr_bench, pytestconfig, to):
    """RF to APRS-IS (aprs.md 9.1): the bench's frame, as in test_aprs_rx_on_air, heard by the radio in the USB network
    personality with the iGate on and the test callsign; the line "<src>>APZRDF,qAR,<radio's call>:<info>" goes out
    once, and aprs.igated counts it.
    aprsis: through the radio's own APRS-IS client (CONFIG_NET_APRSIS) to a server the test runs on the host: the
    client logs in as the radio's call with its passcode, and the line arrives at the server.
    kiss_tcp: with no APRS-IS server online, the line goes to the KISS TCP client (port 8001) as SetHardware 'G'.
    The radio doesn't transmit: iGate transmit, beacons and the digipeater are off."""
    import socket
    from test_net_services import network_mode
    from test_tcp_services import _get
    _need_aprs(radio)
    feats = radio.features()
    for f in ("APRS_RX", "APRS_IGATE", "VFO_FM", "NET_APRSIS" if to == "aprsis" else "NET_KISS"):
        if f not in feats:
            skip("not_built", f"built without CONFIG_{f}")
    if not pytestconfig.getoption("--allow-usb-switch"):
        skip("opt_in", "needs --allow-usb-switch (the IP services are in the USB network personality)")
    call = rig(pytestconfig, "callsign", "an amateur transmission: needs a licensed operator's callsign,")
    mhz = rig(pytestconfig, "fm-mhz")
    hz = round(mhz * 1e6)
    if not allowed_test_hz(hz):
        pytest.fail(f"--fm-mhz {mhz:.4f}: an APRS frequency, or not one of the rig's transmit frequencies (tx_mhz)")
    radio.go_home()
    if radio.screen() == "hotspot":
        skip("state", "the radio is in hotspot mode")
    with contextlib.ExitStack() as stack:
        serial = radio.link
        udp, ip, _ = stack.enter_context(network_mode(radio))
        radio.link = udp
        stack.callback(lambda: setattr(radio, "link", serial) if radio.link is udp else None)
        L = radio.link
        if L.view(["net"]).get("aprsis.state", "") == "online":
            _get(ip, "/api/aprsis?server=off")      # a server left set: the line would go there, not to the test
        stack.enter_context(radio.vfo_session())
        s = stack.enter_context(aprs_session(radio))
        aset = _aprs(radio)
        for field, want in (("igate_tx", 0), ("kiss", 1), ("igate", 1)):
            was = _n(aset, f"set.aprs_{field}")
            if was != want:
                L.aprs_set(field, want)
                stack.callback(L.aprs_set, field, was)
        L.aprs_test_call(call)
        stack.callback(L.aprs_test_call, "")
        v = radio.wait_for(lambda x: x.get("aprs.call", "").startswith(call.upper()) and
                           x.get("set.aprs_igate") == "1" and x.get("set.aprs_kiss") == "1",
                           sections=("aprs", "aprs_set"), what="test call, iGate and KISS on")
        own = v["aprs.call"]
        info = f"{BENCH_RX_INFO} ({to})"    # one text per variant: the iGate passes a packet once in 30 s
        src, program = bench_frame(call, own, info)
        if to == "aprsis":
            srv = AprsIsServer()
            stack.callback(srv.close)
            host = ".".join(ip.split(".")[:3] + [str(int(ip.split(".")[3]) ^ 1)])     # the host's address
            c0 = _n(L.view(["net"]), "aprsis.connects")
            st, _, body = _get(ip, f"/api/aprsis?server={host}:{srv.port}")
            assert st == 200, body
            stack.callback(lambda: _get(ip, "/api/aprsis?server=off"))
            radio.wait_for(lambda x: x.get("aprsis.state") == "online", timeout=60, sections=("net",),
                           what=f"the APRS-IS client online at {host}:{srv.port}")
            login = [ln for ln in srv.lines if ln.startswith("user ")]
            print(f"login: {login}")
            assert login and login[-1].startswith(f"user {own} pass "), f"login {login}, expected as {own}"
            # One connection and one login: a client that drops its first try reconnects only after its back-off
            connects = _n(L.view(["net"]), "aprsis.connects") - c0
            assert len(login) == 1 and connects == 1, f"{len(login)} logins, {connects} connects for one setup"
            n0 = len(srv.lines)
        else:
            k = stack.enter_context(socket.create_connection((ip, 8001), timeout=5))
            k.settimeout(0.5)
        L.vfo_set(hz)
        radio.wait_for(lambda x: x.get("rx_hz") == str(hz) and x.get("mode") == "fm", what="FM VFO")
        s.rx_setup(hz)
        radio.wait_for(lambda x: x.get("aprs.rx_listening") == "1", sections=("aprs",), what="APRS receiver on")
        time.sleep(0.3)
        v0 = _aprs(radio)
        net0 = L.view(["net"])
        with sdr_bench.lease():
            done = sdr_bench.fm_transmit(mhz, program, dev_hz=3000, lead_s=0.1, tail_s=0.1)
        print(f"bench: {done}")
        try:
            v = radio.wait_for(lambda x: _n(x, "aprs.igated") > _n(v0, "aprs.igated"), timeout=5,
                               sections=("aprs",), what="the bench's frame gated")
        except AssertionError:
            v = _aprs(radio)
            raise AssertionError("not gated: " + " ".join(
                f"{key}={_n(v0, key)}->{_n(v, key)}" for key in ("aprs.rx_frames", "aprs.rx_fcs_errors", "aprs.rx_bad",
                                                                 "aprs.igated", "aprs.host_out", "aprs.rx_peak")))
        want = f"{src}>APZRDF,qAR,{own}:{info}"
        end = time.monotonic() + 3
        if to == "aprsis":
            while time.monotonic() < end and want not in srv.lines[n0:]:
                time.sleep(0.1)
            time.sleep(0.5)             # a second copy would follow at once
            lines = [ln for ln in srv.lines[n0:] if not ln.startswith("#")]
            net = L.view(["net"])
            print(f"aprsis.lines_out {net0.get('aprsis.lines_out')}->{net.get('aprsis.lines_out')}")
        else:
            data = b""
            while time.monotonic() < end and want.encode() not in data:
                try:
                    data += k.recv(4096)
                except socket.timeout:
                    pass
            lines = [f[2:].decode("latin-1") for f in kiss_frames(data) if f[:2] == b"\x06G"]
        print(f"aprs.igated {_n(v0, 'aprs.igated')}->{_n(v, 'aprs.igated')}, lines to APRS-IS ({to}): {lines}")
        assert _n(v, "aprs.igated") == _n(v0, "aprs.igated") + 1, f"one packet gated expected: {v}"
        assert lines == [want], f"expected the line {want!r} once ({to}), got {lines}"
