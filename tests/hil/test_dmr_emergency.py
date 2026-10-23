# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR emergency (docs/spec/dmr-emergency.md): an emergency alarm and an emergency call heard raise the alert, and
the radio's own alarm and emergency call go out on air.

Opt-in RF like test_dmr_data.py. Bench-to-radio (rf + bench_service): the bench sends an emergency alarm CSBK
(bench_tx.py csbk --opcode 27 --fid 10) or a voice call with the emergency service option (bench_tx.py voice
--emergency) to the channel's talkgroup. Radio-to-bench (radio_tx + bench_service): the emergency key through the
debug link (DMR_DATA op 7) while the bench modem listens. Pass criteria are the dmr_ext view section's em.* and
data.emergency_* keys; what the bench heard is checked where its listen output carries it (CSBK info octets, the
LC's service options "so")."""
import time

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
    yield
    if x.get("em.built") == 1:
        radio.link.emergency("cancel")
        radio.link.emergency("dismiss")
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
    xb = _ext(radio)
    db = radio.data_view()
    with sdr_bench.lease():
        sdr_bench.csbk(freq, cc, tg, opcode=CSBK_EMERGENCY_ALARM, fid=FID_MOTOROLA, src=BENCH_SRC)
    time.sleep(0.5)
    xa = _ext(radio)
    da = radio.data_view()
    print({k: xa[k] for k in xa if k.startswith("em.alert") or k.startswith("data.emergency")})
    assert xa["data.emergency_alarms_received"] - xb["data.emergency_alarms_received"] >= 1, xa
    assert xa["em.alert"] == 1 and xa["em.alert_kind"] == "alarm" and xa["em.alert_src"] == BENCH_SRC, xa
    assert xa["em.alert_dst"] == tg and xa["em.alert_group"] == 1, xa
    assert da["data.tx_lists"] == db["data.tx_lists"], "the radio answered an emergency alarm"
    assert da["data.event"] == "emergency_alarm" and da["data.event_id"] == BENCH_SRC, da


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
def test_emergency_key_hold(radio, pytestconfig, em_settings):
    """The EMERGENCY key action (the DM-1701's top side key held 1.5 s; injected as the action, held for the
    hold-to-activate time): alarm-only, one try; the UI says "EMERGENCY sent", em.state leaves idle; a second hold
    cancels ("Emergency canceled", em.state idle). No bench: the alarm goes out at the lowest power."""
    from rfoundry_hil import LONG
    _emergency_setup(radio, pytestconfig, tx=True)
    radio.link.emergency_settings("alarm", 0, tries=1, hot_mic_s=0)
    radio.wait_for(lambda x: x.get("em.mode") == "alarm", sections=("dmr_ext",), what="alarm mode")
    radio.press("emergency", LONG)
    radio.wait_for(lambda x: x.get("em.state") in ("alarm", "active") and "EMERGENCY sent" in x.get("ui.toast", ""),
                   timeout=5.0, sections=("dmr_ext", "link"), what="emergency started by the key")
    radio.wait_for(lambda x: x.get("em.state") == "active", timeout=8.0, sections=("dmr_ext",), what="alarm sent")
    radio.press("emergency", LONG)
    radio.wait_for(lambda x: x.get("em.state") == "idle" and "canceled" in x.get("ui.toast", ""),
                   sections=("dmr_ext", "link"), what="canceled by the key")
