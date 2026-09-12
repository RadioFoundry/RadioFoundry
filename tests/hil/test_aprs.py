# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""APRS (docs/spec/aprs.md 13): the debug link side on the radio alone, and on air with the SDR bench as the receiver.

Radio only (no flags): the aprs / aprs_set view sections, a setting round trip, the RAM test callsign, host NMEA as
the position source. Nothing transmits.

On air, AFSK (radio_tx + bench_service: --allow-rf --allow-service-stop, and --aprs-call / $ODR_APRS_CALL, a licensed
operator's callsign; the transmissions are amateur ones). The radio sends a position, a status and a message on the
2 m band at its lowest power while the bench's RTL-SDR records I/Q (the gateway service stopped through the lease and
started again afterwards). tools/aprs_afsk_check.py FM demodulates the capture on the bench, decodes Bell 202 / AX.25
and measures the tones.

Where and how it transmits, so that nothing reaches the APRS network:
- Frequency: --aprs-test-mhz, default 145.090 MHz, through the custom APRS region. US band plans put 2 m packet at
  145.01-145.09 MHz; 144.390 is the national APRS frequency, where digipeaters and iGates would repeat the test
  frames and gate them to APRS-IS. The tests refuse every regional APRS frequency (144.390, 144.800, 145.175,
  144.575, 144.660, 144.640). Pick another simplex packet channel with the option if 145.090 is busy locally.
- Path: none (no WIDEn-N), so a digipeater that hears a frame does not repeat it.
- Power: the radio's lowest level (50 mW on the DM-1701). The tests check `aprs.tx_power` (the level an APRS
  transmission keys at) is 0 before keying, and skip on firmware without that key: it used to key APRS at the
  settings' default power (1 W on the DM-1701) whatever the radio was set to.
- Callsign: --aprs-call as the RAM-only test call (never saved) with SSID --aprs-ssid (default 15). Destination
  APZODR.
- Every APRS setting a test changes (transmit, region, custom frequency, SSID, path, send via, position source, fixed
  position, digital destination, UDT) is put back afterwards, after the queued frames and message retries are on
  air (still on the test frequency). If they don't drain, the region stays on the test frequency and the test fails
  saying so, so a leftover frame never goes out on 144.390.
- Each transmission is short: TX delay 300 ms plus about 0.3 s of frame. The message is sent 3 times (0, 30, 90 s:
  the DM-1701 can't hear the ack), all on the test frequency.

On air, DMR (radio_tx + bench_service): on the DMR test channel (--dmr-freq / --dmr-cc, e.g. 467.375 MHz CC 2) at
the lowest power the radio sends its position as an LRRP report (UDP 4001 in rate 1/2 data) and as an ETSI UDT
location to --aprs-dmr-dst; the bench modem's listener (bench_tx.py listen) hears the data header and blocks, and
the test reassembles and decodes them here.

Pass criteria:
- test_aprs_on_air_position: one frame with a valid FCS from <call>-<ssid> to APZODR, no digipeaters, control 0x03,
  PID 0xF0, a position within 1e-4 degree of 33.7490, -84.3880; aprs.rf_sent +1; afterwards aprs.rf_active=0, the
  radio idle on its channel (ch.rx_hz, rx_hz, mode unchanged). Deviation peak (99.9th percentile of the
  demodulated audio over the transmission) 2.0-4.5 kHz; |twist| (mark / space tone amplitudes) under 6 dB. Printed,
  not asserted: the mark and space deviation, TX delay (flags before the frame, and carrier to first frame bit),
  phase continuity at the tone changes (residual RMS / max in degrees after the fitted switching delay; a
  continuous phase generator gives about 10 degrees, a random phase restart about 100).
- test_aprs_on_air_status: the frame's information field is ">" + the status text.
- test_aprs_on_air_message: ":ADDRESSEE:text{N" with the addressee padded to 9 characters and N a message number
  1-99999; then the retries (aprs.rf_sent +3 in all, aprs.msg_tries 3).
- test_aprs_dmr_position[lrrp|udt]: aprs.dmr_sent +1; a data header from our DMR ID to the destination (CRC ok),
  LRRP: DPF 2 (unconfirmed) carrying IPv4 / UDP to port 4001 with a triggered location report (document 0x0D);
  UDT: DPF 0, format 5 (NMEA location), appended block CRC ok; the position within 2e-5 degree.
"""
import contextlib
import re
import time

import pytest

POS = (33.7490, -84.3880)
APRS_HZ = {144390000, 144800000, 145175000, 144575000, 144660000, 144640000}
PATHS = ["", "WIDE1-1", "WIDE1-1,WIDE2-1", "WIDE2-1", "WIDE2-2"]
VIAS = {"rf": 0, "dmr": 1, "both": 2}
REGION_CUSTOM, POS_FIXED, PATH_NONE = 6, 3, 0
RF_TRIES, MSG_TRIES = 3, 3
CAPTURE_S = 8


def _need_aprs(radio):
    if "APRS" not in radio.features():
        pytest.skip("built without CONFIG_APRS")


def _aprs(radio):
    v = radio.view(["aprs"])
    v.update(radio.view(["aprs_set"]))
    return v


def _n(v, k):
    return int(v.get(k, 0))


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


def test_aprs_test_call_and_nmea(radio):
    """A RAM test callsign clears the "no callsign" block; host NMEA becomes the position (source "host")."""
    _need_aprs(radio)
    before = _aprs(radio)
    try:
        radio.link.aprs_test_call("N0TST")
        radio.link.aprs_set("pos_src", 2)       # Computer (host NMEA)
        radio.link.aprs_nmea("$GPRMC,123519,A,3344.940,N,08423.280,W,000.0,000.0,101026,,")
        # pos_valid alone can already be 1 from a fixed position: wait for the source
        v = radio.wait_for(lambda x: x.get("aprs.pos_valid") == "1" and x.get("aprs.pos_src") == "host",
                           sections=("aprs",), what="host position")
        assert v["aprs.pos_src"] == "host" and v["aprs.call"].startswith("N0TST"), v
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
        assert v["aprs.rf_active"] == "0" and v["aprs.msg_pending"] == "0", \
            f"APRS busy before the test (rf_active / msg_pending): {v}"
        self.keep = {"tx": _n(v, "set.aprs_tx"), "region": _n(v, "set.aprs_region"), "ssid": _n(v, "set.aprs_ssid"),
                     "pos_src": _n(v, "set.aprs_pos_src"), "dmr_dst": _n(v, "set.aprs_dmr_dst"),
                     "dmr_udt": _n(v, "set.aprs_dmr_udt"), "via": VIAS[v["set.aprs_via"]],
                     "path": PATHS.index(v["set.aprs_path"])}
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
        assert hz not in APRS_HZ and 144000000 <= hz <= 148000000, f"{hz} Hz: not a 2 m non-APRS frequency"
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
        for f in ("ssid", "path", "via", "dmr_dst", "dmr_udt", "pos_src"):
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
                "path": PATHS.index(v["set.aprs_path"])}
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
    call = pytestconfig.getoption("--aprs-call")
    if not call:
        pytest.skip("give --aprs-call (or $ODR_APRS_CALL): a licensed operator's callsign (amateur transmission)")
    if "FM" not in radio.features():
        pytest.skip("built without FM")
    hz = round(pytestconfig.getoption("--aprs-test-mhz") * 1e6)
    if hz in APRS_HZ or not 144000000 <= hz <= 148000000:
        pytest.fail(f"--aprs-test-mhz {hz / 1e6:.3f}: must be a 2 m frequency that is not an APRS frequency")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    with contextlib.ExitStack() as stack:
        if start == "fm_vfo":
            if "VFO_FM" not in radio.features():
                pytest.skip("built without the FM VFO")
            stack.enter_context(radio.vfo_session())
            fm_hz = round(pytestconfig.getoption("--fm-mhz") * 1e6)
            radio.link.vfo_set(fm_hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(fm_hz) and x.get("mode") == "fm", what="FM VFO")
        yield from _aprs_air(radio, pytestconfig, call, hz)


def _aprs_air(radio, pytestconfig, call, hz):
    radio.lowest_power()
    v = radio.view(["aprs"])
    if "aprs.tx_power" not in v:
        pytest.skip("this firmware keys APRS at the settings' default power (1 W on the DM-1701), not the radio's "
                    "lowest: needs the aprs.tx_power fix")
    assert v["aprs.tx_power"] == "0" == radio.core()["power"], f"not at the lowest power: {v}"
    ssid = pytestconfig.getoption("--aprs-ssid")
    with aprs_session(radio) as s:
        s.rf_setup(call, ssid, hz)
        s.call, s.ssid, s.hz = call.upper(), ssid, hz
        yield s


def send_and_capture(radio, sdr_bench, s, tag, send):
    """Capture while send() queues one frame; wait for it on air and the radio back on its channel; analyze."""
    core0, ch0 = radio.core(), radio.view(["channel"])
    v0 = _aprs(radio)
    rdir = sdr_bench.run_dir(tag)
    with sdr_bench.lease():
        cap = sdr_bench.capture_start(rdir, s.hz / 1e6, seconds=CAPTURE_S)
        send()
        s.expected_rf += 1
        v = radio.wait_for(lambda x: _n(x, "aprs.rf_sent") > _n(v0, "aprs.rf_sent") or
                           _n(x, "aprs.rf_busy") > _n(v0, "aprs.rf_busy") or
                           _n(x, "aprs.rf_failed") > _n(v0, "aprs.rf_failed"),
                           timeout=15, sections=("aprs",), what="APRS transmission")
        v = radio.wait_for(lambda x: x.get("aprs.rf_active") == "0", timeout=8, sections=("aprs",),
                           what="APRS transmission over")
        sdr_bench.capture_wait()
    print("aprs view:", {k: v[k] for k in ("aprs.rf_sent", "aprs.rf_busy", "aprs.rf_failed", "aprs.last_info")
                         if k in v})
    assert _n(v, "aprs.rf_sent") == _n(v0, "aprs.rf_sent") + 1, \
        f"not sent (rf_busy: the channel was busy for 10 s; rf_failed: aborted): {v}"
    core, ch = radio.wait_for(lambda x: x.get("state") == "idle", timeout=3, what="idle"), radio.view(["channel"])
    for k in ("rx_hz", "mode", "zone_pos", "vfo"):
        assert core.get(k) == core0.get(k), f"{k}: {core0.get(k)} -> {core.get(k)}"
    assert ch.get("ch.rx_hz") == ch0.get("ch.rx_hz"), (ch0, ch)
    res = sdr_bench.analyze_aprs(f"{rdir}/cap0.cs8")
    for seg in res.get("segments", []):
        print(f"keyed {seg['t_start_s']:.2f}-{seg['t_end_s']:.2f} s, carrier {seg['carrier_hz']:+d} Hz, "
              f"{seg['level_dbfs']} dBFS (floor {res.get('floor_dbfs')})")
    ours = [f for f in res["frames"] if f["src"] == f"{s.call}-{s.ssid}"]
    for f in res["frames"]:
        print_frame(f)
    assert len(ours) == 1, f"expected one frame from {s.call}-{s.ssid} with a valid FCS, got {res['frames']} " \
                           f"(capture ~/{sdr_bench.dir}/{rdir}/cap0.cs8, segments {res.get('segments')})"
    f = ours[0]
    assert f["fcs_ok"] and f["dst"] == "APZODR" and f["digis"] == [], f
    assert f["control"] == 0x03 and f["pid"] == 0xF0, f
    return f


def print_frame(f):
    print(f"decoded: {f['tnc2']}")
    print(f"  TX delay {f['txdelay_flags']} flags ({f['txdelay_ms']} ms), carrier up to first frame bit "
          f"{f.get('keyup_to_data_ms')} ms")
    if "mark_dev_hz" in f:
        print(f"  deviation peak {f['peak_dev_hz']:.0f} Hz (99.9 %), max {f['max_dev_hz']:.0f} Hz; mark "
              f"{f['mark_dev_hz']:.0f} Hz, space {f['space_dev_hz']:.0f} Hz, twist {f['twist_db']:+.2f} dB")
    if "phase_rms_deg" in f:
        print(f"  phase at {f['phase_transitions']} tone changes: residual rms {f['phase_rms_deg']:.1f} deg, max "
              f"{f['phase_max_deg']:.1f} deg, {100 * f['phase_over45']:.0f} % over 45 deg, tone switch "
              f"{f['phase_switch_us']:+.0f} us from the bit clock")


def check_tones(f):
    assert "mark_dev_hz" in f, f"tone levels not measured: {f}"
    assert 2000 <= f["peak_dev_hz"] <= 4500, f"peak deviation {f['peak_dev_hz']:.0f} Hz outside 2.0-4.5 kHz"
    assert abs(f["twist_db"]) < 6, f"twist {f['twist_db']:+.1f} dB (mark {f['mark_dev_hz']:.0f} Hz, space " \
                                   f"{f['space_dev_hz']:.0f} Hz)"


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.parametrize("aprs_air", ["channel", "fm_vfo"], indirect=True)
def test_aprs_on_air_position(radio, sdr_bench, aprs_air):
    """One position at 50 mW on the test frequency, decoded off air; deviation, twist, phase, TX delay printed."""
    f = send_and_capture(radio, sdr_bench, aprs_air, "hil_aprs_pos", radio.link.aprs_send_now)
    pos = parse_position(f["info"])
    assert pos is not None, f"not a position: {f['info']!r}"
    assert abs(pos[0] - POS[0]) < 1e-4 and abs(pos[1] - POS[1]) < 1e-4, f"position {pos}, sent {POS}"
    check_tones(f)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_aprs_on_air_status(radio, sdr_bench, aprs_air):
    text = "ODR HIL status test"
    f = send_and_capture(radio, sdr_bench, aprs_air, "hil_aprs_status", lambda: radio.link.aprs_status(text))
    assert f["info"] == ">" + text, f
    check_tones(f)


@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_aprs_on_air_message(radio, sdr_bench, aprs_air):
    """A message with a message number; then its two retries (no ack can come back to the DM-1701)."""
    to, text = f"{aprs_air.call}-1", "ODR HIL message test"
    aprs_air.message = True
    f = send_and_capture(radio, sdr_bench, aprs_air, "hil_aprs_msg", lambda: radio.link.aprs_message(to, text))
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


# ── digital APRS: positions over DMR ────────────────────────────────────────

def crc16_ccitt(data, mask):
    crc = 0
    for b in data[:10]:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return (~crc ^ ((mask << 8) | mask)) & 0xFFFF


def crc32_dmr(data):
    """ETSI TS 102 361-1 B.3.8: the octets of each 16 bit word swapped, polynomial 0x04C11DB7, initial 0."""
    crc = 0
    n = len(data) + (len(data) & 1)
    for i in range(n):
        j = i ^ 1
        crc ^= (data[j] if j < len(data) else 0) << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def data_packets(lines):
    """Data headers heard (dt 6) with the rate 1/2 blocks (dt 7) that follow: dicts with the header fields."""
    out, cur = [], None
    for r in lines:
        dt, info = r.get("dt"), r.get("info")
        if not info:
            continue
        b = bytes.fromhex(info)
        if dt == 6:
            dpf = b[0] & 0x0F
            n = (b[8] & 0x7F) if dpf == 2 else (b[8] & 0x03) + 1 if dpf == 0 else 0
            cur = {"dpf": dpf, "sap": b[1] >> 4, "dst": int.from_bytes(b[2:5], "big"),
                   "src": int.from_bytes(b[5:8], "big"), "pad": (b[0] & 0x10) | (b[1] & 0x0F), "fmt": b[1] & 0x0F,
                   "crc_ok": crc16_ccitt(b, 0xCC) == int.from_bytes(b[10:12], "big"), "need": n, "blocks": []}
            out.append(cur)
        elif dt == 7 and cur is not None and len(cur["blocks"]) < cur["need"]:
            cur["blocks"].append(b)
    return out


def lrrp_position(user):
    """(lat, lon) of an LRRP triggered location report in IPv4 / UDP to port 4001; asserts the layers."""
    assert user[0] >> 4 == 4 and user[9] == 17, f"not IPv4 / UDP: {user.hex()}"
    ihl = (user[0] & 0x0F) * 4
    udp = user[ihl:]
    assert int.from_bytes(udp[2:4], "big") == 4001, f"UDP port {int.from_bytes(udp[2:4], 'big')}"
    doc = udp[8:]
    assert doc[0] == 0x0D, f"LRRP document {doc[0]:#x} (0x0D: triggered location report): {doc.hex()}"
    i = 2 if doc[1] < 0x80 else 3
    while i < len(doc):
        t = doc[i]
        if t == 0x22:
            i += 2 + doc[i + 1]
        elif t == 0x34:
            i += 6
        elif t == 0x51:
            la, lo = int.from_bytes(doc[i + 1:i + 5], "big"), int.from_bytes(doc[i + 5:i + 9], "big", signed=True)
            lat = (la & 0x7FFFFFFF) * 90.0 / 2 ** 31
            return (-lat if la & 0x80000000 else lat), lo * 360.0 / 2 ** 32
        else:
            break
    raise AssertionError(f"no position token in the LRRP report: {doc.hex()}")


def udt_position(block):
    bits = int.from_bytes(block[:10], "big")

    def f(pos, width):
        return (bits >> (80 - pos - width)) & ((1 << width) - 1)
    north, east, valid = f(1, 1), f(2, 1), f(3, 1)
    lat = f(11, 7) + (f(18, 6) + f(24, 14) / 10000.0) / 60.0
    lon = f(38, 8) + (f(46, 6) + f(52, 14) / 10000.0) / 60.0
    return valid, (lat if north else -lat), (lon if east else -lon)


@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.parametrize("form", ["lrrp", "udt"])
def test_aprs_dmr_position(radio, sdr_bench, pytestconfig, form):
    """The position as an LRRP report or a UDT location over DMR at the lowest power, decoded from the bench modem."""
    _need_aprs(radio)
    feats = radio.features()
    if "DMR" not in feats or "DMR_DATA_CORE" not in feats:
        pytest.skip("built without DMR data")
    if form == "lrrp" and "DMR_LRRP" not in feats:
        pytest.skip("built without CONFIG_DMR_LRRP")
    freq, cc = pytestconfig.getoption("--dmr-freq"), pytestconfig.getoption("--dmr-cc")
    dst = pytestconfig.getoption("--aprs-dmr-dst")
    radio.go_home()
    if radio.screen() == "hotspot":
        pytest.skip("the radio is in hotspot mode")
    radio.find_channel(round(freq * 1e6), "dmr")
    radio.lowest_power()
    own = int(radio.view(["dmr"])["dmr.own_id"])
    if own == 0:
        pytest.skip("the radio has no DMR ID")
    time.sleep(1.0)
    with aprs_session(radio) as s:
        s.set("via", VIAS["dmr"])
        s.set("dmr_dst", dst)
        s.set("dmr_udt", 1 if form == "udt" else 0)
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
