# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""NXDN conventional on the radio (docs/spec/nxdn.md 6).

test_nxdn_mode_switch: the channel on view becomes NXDN by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the nxdn view section must show the engine running (`nxdn.on=1`, no new `begin_fails`).
Undoing the change brings the channel's own mode back with the engine stopped. Nothing is sent.

test_nxdn_settings: the NXDN settings (op 40, RAM only) round trip through the nxdn view and are put back.

test_nxdn_stored_channel (opt-in: --allow-codeplug-write; nothing is sent): the channel on view is made a stored NXDN
channel over CPS (nxdn-codeplug.md 2): its record the safe stand-in (DMR, receive only, no contact, its own unit ID
STORED_UNIT), a tag of kind 4 with no system and receive only, 4800 bit/s, selective squelch, RX RAN STORED_RAN and TX
RAN STORED_TX_RAN, and the radio-wide unit ID record RADIO_UNIT (1). A settings area that is erased is made new. The
radio must read it back as that kind (`ch.kind=4`), run NXDN on it (`mode=nxdn`, `nxdn.on=1`) with the channel's
parameters, not the settings' (`nxdn.run.*`: the rate, both RANs, the unit ID the channel's own, a group call to group
1, `rx_gid` 1), and drop it again once the backup is written back. Skipped where the settings area is neither valid
nor erased.

test_nxdn_tx_off_air[9600|4800] (opt-in: --allow-rf --allow-service-stop --nxdn-mhz MHz; the radio transmits, the bench
only receives): the VFO on --nxdn-mhz in NXDN at the lowest power, at that rate, RAN TEST_RAN, unit TEST_UNIT calling
group TEST_GROUP, keys for PTT_S through the debug link while the bench's 4FSK receiver (the rx op, mode fsk4: 2400 or
4800 baud, root raised cosine, the FSW CDF59 as its 10-symbol sync) slices it. tools/bench/nxdntool.py (the firmware's
own framer and call layer, built for the host) decodes the symbols. Pass criteria:
- one call, RAN TEST_RAN, from TEST_UNIT to group TEST_GROUP;
- at least as many frames as the over's length gives, less MISSING_FRAMES (80 ms a frame at both rates);
- voice frames handed on; at most MAX_CRC_FAIL SACCH and FACCH1 CRC failures;
- on the radio: nxdn.tx_calls one more, no nxdn.tx_underruns.
Skipped where the radio's air interface can't transmit the rate (`nxdn.fit4800` / `nxdn.fit9600` without TX).

test_nxdn_rx_from_bench[9600|4800] (opt-in: --allow-rf --allow-service-stop --nxdn-mhz MHz; the bench transmits, the
radio only receives; not yet run on a radio whose air interface receives NXDN): the VFO on --nxdn-mhz in NXDN at that rate, any
RAN and group, while the bench's fsk4 transmitter plays a call nxdntool.py gen writes (RAN TEST_RAN, TEST_UNIT to group
TEST_GROUP, RX_VOICE_FRAMES voice frames). On the radio: one reception more (nxdn.rx_calls) from that unit to that
group (nxdn.src, nxdn.dst) with that RAN. Skipped where the radio's air interface can't receive the rate (the fit
without RX: a provider whose receiver needs a 24-symbol sync has none for NXDN's 10-symbol FSW).
"""
import json
import os
import tempfile
import time

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

AIR_FIT_RX, AIR_FIT_TX = 0x1, 0x2
RX_VOICE_FRAMES = 40
FSK4_HEX_MAX = 8192     # symbols the bench takes in the request itself (fsk4_bench.py)
PTT_S = 3.0
FRAME_MS = 80.0
MISSING_FRAMES = 4      # the bench receiver's sync takes a frame or two to find the carrier, the release frame ends it
MAX_CRC_FAIL = 1
TEST_RAN = 5
TEST_UNIT = 1234
TEST_GROUP = 77
NXDN_SYNC = "CDF59"     # the FSW, ten symbols (TS 1-A 4.3)
SYNC_LEN = 10
RATES = {"9600": (4800, 2400), "4800": (2400, 1050)}    # bit/s -> (symbols/s, outer deviation Hz, TS 1-A table 3.3-1)


def nxdn_view(radio):
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")
    v = radio.view(["nxdn"])
    if v.get("nxdn.built") != "1":
        skip("not_built", "built without CONFIG_NXDN")
    if ((as_int(v.get("nxdn.fit4800")) or 0) | (as_int(v.get("nxdn.fit9600")) or 0)) & 0x3 == 0:
        skip("no_hw", "the radio's air interface carries no NXDN rate")
    return v


def settings_of(v):
    return {"rate9600": v.get("nxdn.rate") == "9600", "rx_ran": int(v["nxdn.rx_ran"]), "tx_ran": int(v["nxdn.tx_ran"]),
            "rx_gid": int(v["nxdn.rx_gid"]), "unit_call": v.get("nxdn.unit_call") == "1",
            "tx_dst": int(v["nxdn.tx_dst"]), "unit_id": int(v["nxdn.unit_id"]),
            "emergency": v.get("nxdn.tx_emergency") == "1"}


@pytest.mark.feature("nxdn")
def test_nxdn_mode_switch(radio):
    v = nxdn_view(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "nxdn":
        skip("state", "the channel is NXDN already")
    fails0 = as_int(v.get("nxdn.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="nxdn")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "nxdn" and x.get("nxdn.on") == "1", 3.0,
                               sections=("core", "nxdn"), what="NXDN running on the channel")
        except AssertionError:
            v = radio.view(["core", "nxdn"])
            if v.get("mode") != "nxdn":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("nxdn.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        print(f"\nnxdn: rate {v.get('nxdn.rate')} RAN {v.get('nxdn.rx_ran')}/{v.get('nxdn.tx_ran')} "
              f"fit {v.get('nxdn.fit')} state {v.get('state')}")
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        radio.restore(snap)
    radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("nxdn.on") == "0", 3.0, sections=("core", "nxdn"),
                   what=f"mode {mode0} back, NXDN stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("nxdn")
def test_nxdn_settings(radio):
    old = settings_of(nxdn_view(radio))
    try:
        radio.link.nxdn_settings_update(rx_ran=9, rx_gid=4321, unit_call=True, tx_dst=555)
        v = radio.wait_for(lambda x: x.get("nxdn.rx_gid") == "4321", 2.0, sections=("nxdn",), what="settings taken")
        assert v.get("nxdn.rx_ran") == "9" and v.get("nxdn.unit_call") == "1" and v.get("nxdn.tx_dst") == "555"
        assert v.get("nxdn.tx_ran") == str(old["tx_ran"]), "a field not given changed"
    finally:
        radio.link.nxdn_settings(**old)
    radio.wait_for(lambda x: x.get("nxdn.rx_gid") == str(old["rx_gid"]), 2.0, sections=("nxdn",),
                   what="settings put back")


# --- A stored NXDN channel (nxdn-codeplug.md, channel-modes.md 2) ---

KIND_NXDN = 4
KEY_CH_TAG = 0x0400         # codeplug-settings.md 3: channel tags, 0x0400 + channel - 1
KEY_UNIT = 0x0A00 + KIND_NXDN   # the radio-wide record (channel-modes.md 5.5)
STORED_RAN = 9
STORED_TX_RAN = 5
STORED_UNIT = 2345          # the channel's own
RADIO_UNIT = 4321           # the radio-wide record's: the channel's own comes first
CH_LFLAG1, LFLAG1_ID, CH_DMR_ID, CH_CONTACT = 0x26, 0x80, 0x27, 0x2E    # the channel record (codeplug.md)


def new_area(size):
    """An empty valid settings area (codeplug-settings.md 2.1): the header, no entries, the rest erased"""
    import zlib
    head = b"RFKV" + bytes([1, 16]) + (0).to_bytes(2, "little") + zlib.crc32(b"").to_bytes(4, "little") + bytes(4)
    return head + b"\xff" * (size - len(head))


def make_nxdn(img, number):
    """Channel number of img made a receive-only stored NXDN channel: the stand-in record, then its tag"""
    import cps_client
    from test_p25 import area_with, name_check
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only
    img[a + CH_CONTACT:a + CH_CONTACT + 2] = bytes(2)                  # no contact: group 1
    img[a + CH_LFLAG1] |= LFLAG1_ID
    img[a + CH_DMR_ID:a + CH_DMR_ID + 3] = STORED_UNIT.to_bytes(3, "big")
    tag = bytes([KIND_NXDN, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0])   # no TX flag
    block = bytes([0x05, STORED_RAN, 0x80 | STORED_TX_RAN])            # 4800 bit/s, selective, RX and TX RAN
    lo, hi = cps_client.SETTINGS_AREA
    area = img[lo:hi]
    if cps_client.area_state(area) != "valid":
        area = new_area(hi - lo)
    area = area_with(area, KEY_CH_TAG + number - 1, tag + block)
    img[lo:hi] = area_with(area, KEY_UNIT, RADIO_UNIT.to_bytes(2, "little"))


@pytest.mark.feature("nxdn")
@pytest.mark.codeplug_write
def test_nxdn_stored_channel(radio, backup_dir):
    nxdn_view(radio)
    if "NXDN" not in radio.features():
        skip("not_built", "NXDN isn't in the radio's feature list")
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "nxdnprobe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if core.get("am") == "1" or core.get("bcast") == "1":
        skip("state", "the channel is in a listen-only band: no NXDN there")
    with codeplug_edited(radio, backup_dir, lambda img: make_nxdn(img, number), "nxdn", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_NXDN) and x.get("nxdn.on") == "1", 5.0,
                           sections=("core", "channel", "nxdn"), what="the stored NXDN channel running")
        assert v.get("mode") == "nxdn", v
        assert v.get("nxdn.run.rate") == "4800", v
        assert v.get("nxdn.run.rx_ran") == str(STORED_RAN) and v.get("nxdn.run.tx_ran") == str(STORED_TX_RAN), v
        assert v.get("nxdn.run.unit_id") == str(STORED_UNIT), v
        assert v.get("nxdn.run.unit_call") == "0" and v.get("nxdn.run.tx_dst") == "1", v
        assert v.get("nxdn.run.rx_gid") == "1", v
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("nxdn.on") != "1", 5.0,
                   sections=("core", "channel", "nxdn"), what="the channel back as it was, NXDN stopped")


def nxdn_decode(job, rate, done=None):
    """nxdntool's JSON lines for an fsk4 rx job's records; done (the job's last record) says why there are none"""
    import nxdntool
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
            syms = nxdntool.records_to_syms(path, tmp)
            return nxdntool.run(["decode", syms, "--unpacked"] + (["--9600"] if rate == "9600" else []))
        except SystemExit as e:
            pytest.fail(f"nxdntool: {e}")


@pytest.mark.feature("nxdn")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
@pytest.mark.parametrize("rate", list(RATES))
def test_nxdn_tx_off_air(radio, sdr_bench, pytestconfig, rate):
    import nxdntool
    mhz = rig(pytestconfig, "nxdn-mhz")
    hz = round(mhz * 1e6)
    v = nxdn_view(radio)
    if (as_int(v.get(f"nxdn.fit{rate}")) or 0) & AIR_FIT_TX == 0:
        skip("no_hw", f"the radio's air interface can't transmit NXDN at {rate} bit/s")
    if not sdr_bench.can_fsk4_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no 4FSK receive")
    baud, dev = RATES[rate]
    old = settings_of(v)
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
            L.nxdn_settings(rate9600=rate == "9600", rx_ran=0, tx_ran=TEST_RAN, rx_gid=0, unit_call=False,
                            tx_dst=TEST_GROUP, unit_id=TEST_UNIT)
            if radio.core().get("mode") != "nxdn":
                L.dmr_fields(mode="nxdn")
            v = radio.wait_for(lambda x: x.get("mode") == "nxdn" and x.get("nxdn.on") == "1" and
                               x.get("nxdn.rate") == rate, 3.0, sections=("core", "nxdn"),
                               what=f"NXDN at {rate} bit/s on the VFO")
            assert v.get("tx_hz") == str(hz), f"the VFO transmits on {v.get('tx_hz')}, not {hz}"
            radio.lowest_power()
            calls0, under0 = (as_int(v.get(f"nxdn.{k}")) or 0 for k in ("tx_calls", "tx_underruns"))
            with sdr_bench.lease():
                job = sdr_bench.fsk4_rx_start(mhz, PTT_S + 4, baud=baud, shape="rrc", sync=NXDN_SYNC,
                                              sync_len=SYNC_LEN, nominal_dev_hz=dev)
                time.sleep(0.5)
                radio.ptt_for(PTT_S)
                radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
                done = job.wait(PTT_S + 20)
            v = radio.view(["nxdn"])
            print(f"\nnxdn tx: calls {calls0}->{v.get('nxdn.tx_calls')}, underruns {under0}->"
                  f"{v.get('nxdn.tx_underruns')}; bench {done.get('summary')}")
        finally:
            L.nxdn_settings(**old)
    lines = nxdn_decode(job, rate, done)
    m = nxdntool.summary(lines)
    syncs = [r for r in job.records if "sync" in r or "offset_hz" in r]
    print(f"\nnxdn off air at {rate} bit/s: {nxdntool.summary_text(m)}; bench syncs {len(syncs)}")
    if syncs:
        print(f"first sync record (not judged): {syncs[0]}")
    assert m["calls"] == 1, f"{m['calls']} calls"
    assert m["ran"] == TEST_RAN, f"RAN {m['ran']}, the radio's {TEST_RAN}"
    assert m["src"] == TEST_UNIT and m["dst"] == TEST_GROUP and m["type"] == "group", \
        f"call {m['type']} {m['src']} -> {m['dst']}, the radio's group {TEST_UNIT} -> {TEST_GROUP}"
    want = PTT_S * 1000 / FRAME_MS
    assert m["frames"] >= want - MISSING_FRAMES, f"{m['frames']} frames in a {PTT_S} s over (about {want:.0f})"
    assert m["voice"] > 0, "no voice frames handed on"
    assert m["crc_fail"] <= MAX_CRC_FAIL, f"{m['crc_fail']} SACCH / FACCH1 CRC failures"
    assert (as_int(v.get("nxdn.tx_calls")) or 0) == calls0 + 1, "the radio counted no NXDN call"
    assert (as_int(v.get("nxdn.tx_underruns")) or 0) == under0, "the air ran dry during the call"


@pytest.mark.feature("nxdn")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("rate", list(RATES))
def test_nxdn_rx_from_bench(radio, sdr_bench, pytestconfig, rate):
    import nxdntool
    mhz = rig(pytestconfig, "nxdn-mhz")
    hz = round(mhz * 1e6)
    v = nxdn_view(radio)
    if (as_int(v.get(f"nxdn.fit{rate}")) or 0) & AIR_FIT_RX == 0:
        skip("no_hw", f"the radio's air interface can't receive NXDN at {rate} bit/s")
    baud, dev = RATES[rate]
    old = settings_of(v)
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
            L.nxdn_settings(rate9600=rate == "9600", rx_ran=0, tx_ran=TEST_RAN, rx_gid=0, unit_call=False,
                            tx_dst=TEST_GROUP, unit_id=TEST_UNIT + 1)
            if radio.core().get("mode") != "nxdn":
                L.dmr_fields(mode="nxdn")
            v0 = radio.wait_for(lambda x: x.get("mode") == "nxdn" and x.get("nxdn.on") == "1" and
                                x.get("nxdn.rate") == rate, 3.0, sections=("core", "nxdn"),
                                what=f"NXDN at {rate} bit/s on the VFO")
            with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
                path, exp = os.path.join(tmp, "call.dibits"), os.path.join(tmp, "call.json")
                try:
                    nxdntool.run(["gen", path, "--expect", exp, "--ran", TEST_RAN, "--group", TEST_GROUP, "--src",
                                  TEST_UNIT, "--voice-frames", RX_VOICE_FRAMES, "--lead", baud // 4]
                                 + (["--9600"] if rate == "9600" else []))
                except SystemExit as e:
                    pytest.fail(f"nxdntool: {e}")
                with open(path, "rb") as f:
                    data = f.read()
                n = len(data) * 4
                assert n <= FSK4_HEX_MAX, f"the call holds {n} symbols, past {FSK4_HEX_MAX}"
                tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=n / baud + 2, symbols=n, hex=data.hex(), shape="rrc",
                                        baud=baud, dev_hz=dev)
                tx.wait(n / baud + 30)
            time.sleep(0.6)
            v1 = radio.view(["core", "nxdn"])
        finally:
            L.nxdn_settings(**old)
    keys = ("rx_calls", "frames", "voice", "played", "voice_held", "sacch_fail", "facch_fail", "lich_fail", "fec_bits",
            "ran_rejects", "rx_chunks")
    print(f"\nnxdn from the bench at {rate} bit/s: "
          + ", ".join(f"{k} {v0.get('nxdn.' + k)}->{v1.get('nxdn.' + k)}" for k in keys
                      if v0.get("nxdn." + k) != v1.get("nxdn." + k)))
    assert (as_int(v1.get("nxdn.rx_calls")) or 0) == (as_int(v0.get("nxdn.rx_calls")) or 0) + 1, "no call received"
    assert v1.get("nxdn.src") == str(TEST_UNIT) and v1.get("nxdn.dst") == str(TEST_GROUP), \
        f"the call {v1.get('nxdn.src')} -> {v1.get('nxdn.dst')}, sent {TEST_UNIT} -> {TEST_GROUP}"
    assert v1.get("nxdn.ran") == str(TEST_RAN), f"RAN {v1.get('nxdn.ran')}, sent {TEST_RAN}"
