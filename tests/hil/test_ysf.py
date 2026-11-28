# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""YSF DN on the radio (docs/spec/ysf.md 5).

test_ysf_mode_switch: the channel on view becomes YSF by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the ysf view section must show the engine running (`ysf.on=1`, no new `begin_fails`).
Undoing the change brings the channel's own mode back with the engine stopped. Nothing is sent.

test_ysf_settings: the YSF DG-IDs (op 44, RAM only) round trip through the ysf view and are put back.

test_ysf_stored_channel (opt-in: --allow-codeplug-write; nothing is sent): the channel on view is made a stored YSF
channel over CPS (ysf-codeplug.md 2): its record the safe stand-in (DMR, receive only), a tag of kind 103 with no
system and receive only, transmit DG-ID STORED_TX_DGID and receive DG-ID STORED_RX_DGID. A settings area that is
erased is made new. The radio must read it back as that kind (`ch.kind=103`), run YSF on it (`mode=ysf`, `ysf.on=1`)
with the channel's DG-IDs, not the settings' (`ysf.run.*`), and drop it again once the backup is written back.

test_ysf_tx_off_air (opt-in: --allow-rf --allow-service-stop --ysf-mhz MHz; the radio transmits, the bench only
receives): the VFO on --ysf-mhz in YSF at the lowest power, transmit DG-ID TEST_DGID, keys for PTT_S through the
debug link while the bench's 4FSK receiver (the rx op, mode fsk4: 4800 baud, root raised cosine, the frame sync
D471C9634D as its 20-symbol sync) slices it. tools/bench/ysftool.py (the firmware's own framer and call layer, built
for the host) decodes the symbols. Pass criteria:
- one call from the radio's callsign (`ysf.callsign`) and radio ID (`ysf.rid`), DG-ID TEST_DGID, DN voice;
- at least as many frames as the over's length gives, less MISSING_FRAMES (100 ms a frame);
- voice frames handed on; at most MAX_FICH_FAIL FICH failures;
- on the radio: ysf.tx_calls one more, no ysf.tx_underruns.
Skipped where the radio's air interface can't transmit YSF (`ysf.fit` without TX) or the radio has no callsign
(`ysf.callsign` empty: PTT is refused, ysf.md 5.2).
"""
import os
import tempfile
import json
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

AIR_FIT_RX, AIR_FIT_TX = 0x1, 0x2
PTT_S = 3.0
FRAME_MS = 100.0
MISSING_FRAMES = 4      # the bench receiver's sync takes a frame or two, the header and terminator end it
MAX_FICH_FAIL = 1
TEST_DGID = 23
YSF_SYNC = "D471C9634D"     # the frame sync, 20 symbols (ysf.md 2)
SYNC_LEN = 20
BAUD, DEV_HZ = 4800, 2700   # ysf_air_mode: 4800 symbols/s, outer deviation


def ysf_view(radio):
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    v = radio.view(["ysf"])
    if v.get("ysf.built") != "1":
        skip("not_built", "built without CONFIG_YSF")
    if (as_int(v.get("ysf.fit")) or 0) & (AIR_FIT_RX | AIR_FIT_TX) == 0:
        skip("no_hw", "the radio's air interface doesn't carry YSF")
    return v


def settings_of(v):
    return {"tx_dgid": int(v["ysf.tx_dgid"]), "rx_dgid": int(v["ysf.rx_dgid"])}


@pytest.mark.feature("ysf")
def test_ysf_mode_switch(radio):
    v = ysf_view(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "ysf":
        skip("state", "the channel is YSF already")
    fails0 = as_int(v.get("ysf.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="ysf")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "ysf" and x.get("ysf.on") == "1", 3.0,
                               sections=("core", "ysf"), what="YSF running on the channel")
        except AssertionError:
            v = radio.view(["core", "ysf"])
            if v.get("mode") != "ysf":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("ysf.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        print(f"\nysf: DG-ID {v.get('ysf.run.tx_dgid')}/{v.get('ysf.run.rx_dgid')} fit {v.get('ysf.fit')} "
              f"callsign {v.get('ysf.callsign') or '-'} state {v.get('state')}")
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        if snap["vfo"] == "1":      # the VFO's mode is its own: restore() only drops a channel's mode for now
            radio.link.dmr_fields(mode=mode0)
        radio.restore(snap)
    radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("ysf.on") == "0", 3.0, sections=("core", "ysf"),
                   what=f"mode {mode0} back, YSF stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("ysf")
def test_ysf_settings(radio):
    old = settings_of(ysf_view(radio))
    try:
        radio.link.ysf_settings_update(tx_dgid=12, rx_dgid=34)
        v = radio.wait_for(lambda x: x.get("ysf.rx_dgid") == "34", 2.0, sections=("ysf",), what="settings taken")
        assert v.get("ysf.tx_dgid") == "12", v
        radio.link.ysf_settings_update(rx_dgid=0)
        v = radio.wait_for(lambda x: x.get("ysf.rx_dgid") == "0", 2.0, sections=("ysf",), what="one field changed")
        assert v.get("ysf.tx_dgid") == "12", "a field not given changed"
    finally:
        radio.link.ysf_settings(**old)
    radio.wait_for(lambda x: x.get("ysf.tx_dgid") == str(old["tx_dgid"]) and
                   x.get("ysf.rx_dgid") == str(old["rx_dgid"]), 2.0, sections=("ysf",), what="settings put back")


# --- A stored YSF channel (ysf-codeplug.md 2, channel-modes.md 2) ---

KIND_YSF = 103
KEY_CH_TAG = 0x0400         # codeplug-settings.md 3: channel tags, 0x0400 + channel - 1
STORED_TX_DGID = 17
STORED_RX_DGID = 42


def make_ysf(img, number):
    """Channel number of img made a stored YSF channel, receive only: the stand-in record, then its tag"""
    import cps_client
    from test_p25 import area_with, name_check
    from test_nxdn import new_area
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: the stand-in, whatever the tag allows
    tag = bytes([KIND_YSF, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0])
    lo, hi = cps_client.SETTINGS_AREA
    area = img[lo:hi]
    if cps_client.area_state(area) != "valid":
        area = new_area(hi - lo)
    img[lo:hi] = area_with(area, KEY_CH_TAG + number - 1, tag + bytes([STORED_TX_DGID, STORED_RX_DGID]))


@pytest.mark.feature("ysf")
@pytest.mark.codeplug_write
def test_ysf_stored_channel(radio, backup_dir):
    old = settings_of(ysf_view(radio))
    if "YSF" not in radio.features():
        skip("not_built", "YSF isn't in the radio's feature list")
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "ysfprobe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if core.get("am") == "1" or core.get("bcast") == "1":
        skip("state", "the channel is in a listen-only band: no YSF there")
    with codeplug_edited(radio, backup_dir, lambda img: make_ysf(img, number), "ysf", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_YSF) and x.get("ysf.on") == "1", 5.0,
                           sections=("core", "channel", "ysf"), what="the stored YSF channel running")
        assert v.get("mode") == "ysf", v
        assert v.get("ysf.run.tx_dgid") == str(STORED_TX_DGID), v
        assert v.get("ysf.run.rx_dgid") == str(STORED_RX_DGID), v
        assert settings_of(v) == old, "the channel's DG-IDs went into the settings"
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("ysf.on") != "1", 5.0,
                   sections=("core", "channel", "ysf"), what="the channel back as it was, YSF stopped")


# --- Transmit, decoded off air ---

def ysf_decode(job, done=None):
    """ysftool's JSON lines for an fsk4 rx job's records; done (the job's last record) says why there are none"""
    import ysftool
    recs = [r for r in job.records if any(isinstance(r.get(k), (str, dict)) for k in ("dibits", "syms", "symbols"))]
    if not recs:
        what = (done or {}).get("summary")
        if what and what.get("carrier") is False and not what.get("keyings"):
            raise AssertionError(f"the bench board heard no carrier while the radio keyed: {what}")
        skip("bench", f"the bench board sent no symbols (the fsk4 rx op's records): {what}")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "rx.jsonl")
        with open(path, "w") as f:
            for r in job.records:
                f.write(json.dumps({"rec": r}) + "\n")
        try:
            return ysftool.decode(path, records=True)
        except SystemExit as e:
            pytest.fail(f"ysftool: {e}")


@pytest.mark.feature("ysf")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.needs_switch("VFO")
def test_ysf_tx_off_air(radio, sdr_bench, pytestconfig):
    mhz = rig(pytestconfig, "ysf-mhz")
    hz = round(mhz * 1e6)
    v = ysf_view(radio)
    if (as_int(v.get("ysf.fit")) or 0) & AIR_FIT_TX == 0:
        skip("no_hw", "the radio's air interface can't transmit YSF")
    if not v.get("ysf.callsign"):
        skip("state", "the radio has no YSF callsign: PTT is refused (ysf.md 5.2)")
    if not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")
    old = settings_of(v)
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
            L.ysf_settings(tx_dgid=TEST_DGID, rx_dgid=0)
            if radio.core().get("mode") != "ysf":
                L.dmr_fields(mode="ysf")
            v0 = radio.wait_for(lambda x: x.get("mode") == "ysf" and x.get("ysf.on") == "1" and
                                x.get("ysf.run.tx_dgid") == str(TEST_DGID), 3.0, sections=("core", "ysf"),
                                what="YSF on the VFO")
            assert v0.get("tx_hz") == str(hz), f"the VFO transmits on {v0.get('tx_hz')}, not {hz}"
            radio.lowest_power()
            with sdr_bench.lease():
                job = sdr_bench.fsk4_rx_start(mhz, PTT_S + 4, baud=BAUD, shape="rrc", sync=YSF_SYNC,
                                              sync_len=SYNC_LEN, nominal_dev_hz=DEV_HZ)
                time.sleep(0.5)
                radio.ptt_for(PTT_S)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                done = job.wait(PTT_S + 20)
            v1 = radio.view(["ysf"])
        finally:
            L.ysf_settings(**old)
    grew = {k: (as_int(v1.get(f"ysf.{k}")) or 0) - (as_int(v0.get(f"ysf.{k}")) or 0) for k in ("tx_calls", "tx_underruns")}
    lines = ysf_decode(job, done)
    calls = [x for x in lines if x["ev"] in ("call_start", "call_update")]
    stats = next(x for x in lines if x["ev"] == "stats")
    print(f"\nysf tx: radio {grew}; bench {done.get('summary')}; decoded {stats}")
    assert sum(1 for x in lines if x["ev"] == "call_start") == 1, f"not one call: {calls}"
    last = calls[-1]
    assert last["src"] == v0.get("ysf.callsign") and last["src_id"] == v0.get("ysf.rid"), last
    assert last["dgid"] == TEST_DGID and last["dt"] == 2, last      # DN voice (V/D mode 2)
    want = int(PTT_S * 1000 / FRAME_MS) - MISSING_FRAMES
    assert stats["call"]["frames"] >= want, f"{stats['call']['frames']} frames, want {want} or more"
    assert stats["voice_out"] > 0, "no voice frames handed on"
    assert stats["call"]["fich_fail"] <= MAX_FICH_FAIL, stats
    assert grew["tx_calls"] == 1 and grew["tx_underruns"] == 0, grew
