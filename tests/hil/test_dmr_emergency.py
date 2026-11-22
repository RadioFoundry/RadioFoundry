# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR emergency (docs/spec/dmr-emergency.md): an emergency alarm and an emergency call heard raise the alert, and
the radio's own alarm and emergency call go out on air.

Opt-in RF like test_dmr_data.py. Bench-to-radio (rf + bench_service): the bench sends an emergency alarm CSBK
(a bench_tx csbk job --opcode 27 --fid 10) or a voice call with the emergency service option (a bench_tx voice job
--emergency) to the channel's talkgroup. Radio-to-bench (radio_tx + bench_service): the emergency key through the
debug link (DMR_DATA op 7) while the bench modem listens. Pass criteria are the dmr_ext view section's em.* and
data.emergency_* keys; what the bench heard is checked where its listen output carries it (CSBK info octets, the
LC's service options "so")."""
import time
import struct
import zlib

import pytest

from rfoundry_hil import skip
from test_dmr_data import BENCH_SRC, _need, _radio_sends, _setup, heard_tries

CSBK_EMERGENCY_ALARM = 0x27
FID_MOTOROLA = 0x10


def _ext(radio):
    out = {}
    for k, v in radio.view(["dmr_ext"]).items():
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def _emergency_setup(radio, pytestconfig, tx=False):
    freq, cc, own, feats = _setup(radio, pytestconfig, tx=tx)
    if _ext(radio).get("em.built") != 1:
        skip("not_built", "built without CONFIG_DMR_EMERGENCY")
    tg = int(radio.view(["dmr"])["dmr.target_id"])
    if radio.view(["dmr"]).get("dmr.target_private") == "1":
        skip("state", "the test channel talks to a radio, not a talkgroup")
    return freq, cc, own, tg, feats


@pytest.fixture
def em_settings(radio):
    """Puts the emergency settings back (and the emergency off) after the test."""
    x = _ext(radio)
    screen_before = radio.screen()
    yield
    if x.get("em.built") == 1:
        radio.link.emergency("cancel")
        radio.link.emergency("dismiss")
        if screen_before != "emergency" and radio.screen() == "emergency":
            radio.press("ok")
            radio.wait_for(lambda v: v.get("ui.screen") != "emergency", sections=("link",), what="received alert closed")
        radio.link.emergency_settings(x["em.mode"], x["em.dst"], x["em.dst_private"] == 1, x["em.etsi_fid"] == 1,
                                      x["em.rx_tone"] == 1, x["em.tries"], x["em.hot_mic_s"])


@pytest.mark.feature("dmr_emergency")
def test_emergency_view_and_settings(radio, pytestconfig, em_settings):
    """Radio only: the dmr_ext section is there and the settings request changes it."""
    _emergency_setup(radio, pytestconfig)
    radio.link.emergency_settings("alarm", 0, tries=2, hot_mic_s=0)
    v = radio.wait_for(lambda x: x.get("em.mode") == "alarm" and x.get("em.tries") == "2", sections=("dmr_ext",),
                       what="emergency settings applied")
    assert v["em.state"] == "idle" and v["em.hot_mic_s"] == "0", v


@pytest.mark.feature("dmr_emergency")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_emergency_alarm_rx(radio, sdr_bench, pytestconfig, em_settings):
    """An emergency alarm CSBK (opcode 0x27, Motorola FID, octet 2 0 = to a group) to the channel's talkgroup: one
    alert of kind alarm from the bench; nothing is sent back."""
    freq, cc, own, tg, feats = _emergency_setup(radio, pytestconfig)
    radio.link.emergency("dismiss")
    # A source of its own: an emergency from a source active in the last 30 s adds no alert, dismissed or not
    # (dmr-emergency.md, DMR_EMERGENCY_REALERT_MS), and an earlier test's emergency call may come from the bench's
    # default source
    src = BENCH_SRC + 5
    xb = _ext(radio)
    db = radio.data_view()
    with sdr_bench.lease():
        sdr_bench.csbk(freq, cc, tg, opcode=CSBK_EMERGENCY_ALARM, fid=FID_MOTOROLA, src=src)
    time.sleep(0.5)
    xa = _ext(radio)
    da = radio.data_view()
    print({k: xa[k] for k in xa if k.startswith("em.alert") or k.startswith("data.emergency")})
    assert xa["data.emergency_alarms_received"] - xb["data.emergency_alarms_received"] >= 1, xa
    assert xa["em.alert"] == 1 and xa["em.alert_kind"] == "alarm" and xa["em.alert_src"] == src, xa
    assert xa["em.alert_dst"] == tg and xa["em.alert_group"] == 1, xa
    assert da["data.tx_lists"] == db["data.tx_lists"], "the radio answered an emergency alarm"
    assert da["data.event"] == "emergency_alarm" and da["data.event_id"] == src, da


@pytest.mark.feature("dmr_emergency")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_emergency_call_rx(radio, sdr_bench, pytestconfig, em_settings):
    """A group voice call with the emergency service option (LC octet 1 0x80) on the channel's talkgroup: the call
    is played and raises an alert of kind call (another source than the alarm test, so it is a new alert)."""
    freq, cc, own, tg, feats = _emergency_setup(radio, pytestconfig)
    radio.link.emergency("dismiss")
    src = BENCH_SRC + 7
    xb = _ext(radio)
    with sdr_bench.lease():
        sdr_bench.voice_call(freq, cc, tg, superframes=3, src=src, emergency=True)
    time.sleep(0.5)
    xa = _ext(radio)
    assert xa["em.alerts_call"] - xb["em.alerts_call"] == 1, xa
    assert xa["em.alert"] == 1 and xa["em.alert_kind"] == "call" and xa["em.alert_src"] == src, xa
    last = radio.view(["dmr_call"])
    assert last.get("last.emergency", last.get("call.emergency")) == "1", last


@pytest.mark.feature("dmr_emergency")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_emergency_alarm_tx(radio, sdr_bench, pytestconfig, em_settings):
    """The emergency key in alarm-only mode: three alarm CSBKs about a second apart (opcode 0x27, Motorola FID,
    octets 4-6 the channel's talkgroup, 7-9 us), then em.state active until canceled."""
    freq, cc, own, tg, feats = _emergency_setup(radio, pytestconfig, tx=True)
    radio.link.emergency_settings("alarm", 0, tries=3, hot_mic_s=0)
    radio.wait_for(lambda x: x.get("em.mode") == "alarm", sections=("dmr_ext",), what="alarm mode")
    xb = _ext(radio)
    heard, _ = _radio_sends(radio, sdr_bench, freq, cc, lambda: radio.link.emergency("start"), 10,
                            lambda v: v.get("data.event") in ("emergency_sent", "emergency_failed"))
    xa = _ext(radio)
    sent = xa["data.emergency_alarms_sent"] - xb["data.emergency_alarms_sent"]
    assert sent == 3, xa
    assert xa["em.state"] == "active", xa
    alarms = heard_tries(heard, CSBK_EMERGENCY_ALARM, sent,
                         lambda x: x[1] == FID_MOTOROLA and x[3] == tg and x[4] == own, "emergency alarm")
    # the tries are about a second apart; with some missed, any two heard are still at least that far apart
    gaps = [alarms[i + 1]["t"] - alarms[i]["t"] for i in range(len(alarms) - 1)]
    assert all(g >= 0.9 for g in gaps), gaps
    radio.link.emergency("cancel")
    radio.wait_for(lambda x: x.get("em.state") == "idle", sections=("dmr_ext",), what="emergency canceled")


@pytest.mark.feature("dmr_emergency")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_emergency_call_tx(radio, sdr_bench, pytestconfig, em_settings):
    """Call-only mode with a 3 s hot mic: the radio keys by itself, a group voice call to the talkgroup whose LC
    carries the emergency service option (so & 0x80), released after the hot mic."""
    freq, cc, own, tg, feats = _emergency_setup(radio, pytestconfig, tx=True)
    if "codec" in radio.view(["dmr"]) and radio.view(["dmr"]).get("dmr.codec") != "1":
        skip("not_built", "no AMBE codec: DMR voice can't be keyed")
    radio.link.emergency_settings("call", 0, hot_mic_s=3)
    radio.wait_for(lambda x: x.get("em.mode") == "call", sections=("dmr_ext",), what="call mode")
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=10)
        radio.link.emergency("start")
        radio.wait_for(lambda v: v.get("state") == "tx", timeout=3, what="hot mic keyed")
        radio.wait_for(lambda v: v.get("state") != "tx", timeout=7, what="hot mic released")
        heard = sdr_bench.listen_result(lis)
    xa = _ext(radio)
    assert xa["em.state"] == "active" and xa["em.hot_mic_keyed"] >= 1, xa
    lcs = [r for r in heard if r.get("so") is not None and r.get("dst") == tg and r.get("src") == own]
    assert lcs and all(r["so"] & 0x80 for r in lcs), heard
    radio.link.emergency("cancel")


@pytest.mark.feature("dmr_emergency")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_emergency_key_hold(radio, pytestconfig, em_settings, backup_dir):
    """A physical-key injection held through the 3 s countdown starts and cancels an alarm-only emergency.
    Normalize Key/Alert/Hold in the codeplug, then restore their original values (including absent keys).
    No bench: one alarm goes out at the lowest power; requires RF and codeplug-write opt-in."""
    import cps_client
    from test_cps import cps_session

    _emergency_setup(radio, pytestconfig, tx=True)
    if radio.core().get("codeplug") != "open-firmware":
        skip("state", "needs the radio's own codeplug for the emergency settings write")
    region = cps_client.SETTINGS_AREA
    lo, hi = region
    original = cps_session(radio, lambda c: c.read_image([region]))
    state = cps_client.area_state(original[lo:hi])
    if state not in ("valid", "empty"):
        skip("state", f"settings area is {state}: preserving it")
    from pathlib import Path
    backup_dir = Path(backup_dir)
    backup_dir.mkdir(parents=True, exist_ok=True)
    (backup_dir / "emergency-settings.bin").write_bytes(original[lo:hi])
    restored = bytearray(original)
    if state == "empty":
        # A valid empty area restores absent keys; CPS deliberately preserves an area when given erased bytes.
        restored[lo:lo + 16] = b"RFKV" + bytes([1, 16]) + bytes(10)
    edited = bytearray(restored)
    hdr = edited[lo + 5]
    end = lo + hdr + int.from_bytes(edited[lo + 6:lo + 8], "little")
    hold_s = 3
    emergency_controls = {0x000A: 1, 0x000B: 0, 0x000C: hold_s}  # key, alert, hold
    entries = bytearray()
    at = lo + hdr
    while at < end:
        key, size = struct.unpack_from("<HH", edited, at)
        if key not in emergency_controls:
            entries.extend(edited[at:at + 4 + size])
        at += 4 + size
    for key, value in emergency_controls.items():
        entries.extend(struct.pack("<HHB", key, 1, value))
    if hdr + len(entries) > hi - lo:
        skip("state", "settings area has no room for the emergency controls")
    edited[lo + 6:lo + 12] = struct.pack("<HI", len(entries), zlib.crc32(entries))
    edited[lo + hdr:lo + hdr + len(entries)] = entries

    def write_checked(image):
        cps_session(radio, lambda c: c.write_image(image, [region], reboot=False))
        got = cps_session(radio, lambda c: c.read_image([region]))
        assert got[lo:hi] == image[lo:hi], "emergency settings write did not read back"

    try:
        write_checked(edited)
        radio.link.emergency("cancel")
        radio.link.emergency_settings("alarm", 0, tries=1, hot_mic_s=0)
        radio.wait_for(lambda x: x.get("em.mode") == "alarm" and x.get("em.state") == "idle",
                       sections=("dmr_ext",), what="alarm mode, idle")
        radio.link.key_hold("emergency", 600)
        time.sleep(1)
        assert _ext(radio)["em.state"] == "idle", "early release started emergency"
        # The key's short action can step the power level when released early.
        radio.lowest_power(absolute=True)
        before_alarms = _ext(radio)["data.emergency_alarms_sent"]
        hold_ms = hold_s * 1000 + 500
        radio.link.key_hold("emergency", hold_ms)
        radio.wait_for(lambda x: x.get("ui.screen") == "emergency-hold", timeout=2.0, sections=("link",),
                       what="the countdown")
        radio.wait_for(lambda x: x.get("em.state") in ("alarm", "active"), timeout=hold_s + 5,
                       sections=("dmr_ext",), what="emergency started at the end of the hold")
        radio.wait_for(lambda x: x.get("em.state") == "active", timeout=8.0, sections=("dmr_ext",),
                       what="alarm phase complete")
        assert _ext(radio)["data.emergency_alarms_sent"] == before_alarms + 1
        assert radio.data_view()["data.event"] == "emergency_sent"
        radio.wait_for(lambda x: x.get("ui.screen") != "emergency-hold", timeout=5.0, sections=("link",),
                       what="the outcome closed")
        radio.link.key_hold("emergency", hold_ms)
        radio.wait_for(lambda x: x.get("em.state") == "idle", timeout=hold_s + 5,
                       sections=("dmr_ext", "link"), what="canceled by the key")
    finally:
        try:
            radio.link.wait_keys_idle()
            radio.link.emergency("cancel")
        finally:
            write_checked(restored)
