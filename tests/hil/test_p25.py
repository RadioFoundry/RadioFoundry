# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""P25 conventional on the radio, with nothing sent (docs/spec/p25.md 7). Radio only: the radio never transmits.

test_p25_channel_switch: the channel on view becomes P25 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the p25 view section must show the engine running (`p25.on=1`, no new `begin_fails`) and,
where the provider counts it, the receive load at idle (`p25.rx_work_us / p25.rx_span_us`, at most MAX_IDLE_LOAD).
Undoing the temporary change brings the channel's own mode back with the engine stopped, and a DMR channel's call
stack running again.

test_p25_settings: the P25 settings (op 39, RAM only) round trip through the p25 view and are put back.

test_p25_stored_channel (opt-in: --allow-codeplug-write; nothing is sent): the channel on view is made a stored P25
channel over CPS (p25-codeplug.md 2): its record the safe stand-in, DMR and receive only, and a tag of kind 2 with no
system, receive only, NAC STORED_NAC both ways and normal squelch in the codeplug settings area. The radio must read it
back as that kind (`ch.kind=2`), run P25 on it (`mode=p25`, `p25.on=1`) with the channel's parameters, not the
settings' (`p25.run.rx_nac`, `p25.run.tx_nac`, `p25.run.rx_tgid=0`, `p25.run.talkaround` 1 on a simplex or direct-mode
channel, else 2), and drop it again once the backup is written back. Skipped where the settings area isn't ours to write (not
valid in the backup) and where the radio can't carry P25 (no P25 in its feature list).

test_p25_tx_off_air (opt-in: --allow-rf --allow-service-stop --p25-mhz MHz; the radio transmits, the bench only
receives): the VFO on --p25-mhz in P25 at the lowest power keys for PTT_S through the debug link while the bench's
receive board records its FM discriminator (the rx op's disc stream). tools/bench/p25tool.py (the firmware's own
framer and call layer, built for the host) decodes it. Pass criteria:
- one call: a header first with the radio's NAC (p25.tx_nac) and talk group (p25.tx_dst), a terminator last;
- LDU1 and LDU2 in turn, every LDU1's link control with the radio's ID (p25.unit_id, else dmr.own_id) and the talk group, every LDU2's
  encryption sync clear;
- the LDUs back to back: 180 ms apart (+-0.5 ms), as many as the over's length gives (within two);
- every unit's NID decoded with at most MAX_NID_ERRS bit errors; no voice frame bad, the corrected bits at most
  MAX_CORRECTED of the voice bits; no link control or encryption sync failure;
- the outer deviation the soft modem measures at its syncs (median) within DEV_PCT of 1800 Hz;
- on the radio: p25.tx_calls one more, no p25.tx_underruns.
The carrier offset the soft modem measures is printed, not judged: it includes the bench board's own error.

test_p25_trunk_follow (opt-in: --p25-trunk-channel NAME; nothing is sent, PTT is not pressed): the stored P25 trunked
channel of that name, its system's control channel heard at the rig (p25-trunking.md 7). The radio must run P25 on it
(`mode=p25`, `ch.kind=3`), the follower must hold a control channel (`p25t.state=2`) within CC_WAIT_S with most blocks
decoded (CRC failures at most MAX_CRC_FAIL of them), and the system it names must be the one the codeplug gives (when
that isn't "any"; printed). For FOLLOW_WAIT_S it watches for a call it follows: one followed counts a reception
(`p25.rx_calls`), and the follower is back on the control channel afterwards. No call in that time is printed, not
failed: traffic is the system's. Leaving the channel stops the follower (`p25t.on=0`). Skipped without
CONFIG_P25_TRUNK, and where the board can't carry P25 (the channel stays parked).

test_p25_trunk_tx (opt-in: --allow-rf --allow-service-stop --p25-trunk-tx-channel NAME --p25-cc-mhz MHz
--p25-cc-in-mhz MHz --p25-voice-mhz MHz; the bench and the radio transmit; written, not yet run on a radio): the stored
P25 trunked channel NAME, transmit on, its system's control channel at --p25-cc-mhz with its inbound frequency at
--p25-cc-in-mhz (the offset a multiple of 250 kHz) and a system that takes WACN SITE_WACN, system SITE_SYSID, NAC
SITE_NAC. The bench plays a control channel that p25tool.py cc writes (p25-trunking.md 8) and captures what the radio
sends with its FM discriminator. Three steps:
1. Broadcasts only, the inbound frequency captured: the radio must send a registration request (p25tool.py decode
   --inbound) with the site's WACN and system ID; its unit ID is taken from it.
2. The site accepts the registration in every unit: the radio must send an affiliation request for its talk group
   (taken from it) and reach p25t.reg=4 (registered and affiliated).
3. The site also accepts the affiliation and grants that unit and group a voice channel whose inbound side is
   --p25-voice-mhz; PTT for PTT_S with --p25-voice-mhz captured. The radio must ask (p25t.req_tx), go to the grant
   (p25t.req=3, p25t.talk_hz the voice channel), and transmit a conventional call there: a header, LDUs with its unit
   and talk group, a terminator. After release it is back on the control channel (p25t.state=2) with the request
   idle (p25t.req=0).
Skipped without CONFIG_P25_TRUNK, where the bench board has no fsk4 transmitter or no FM receiver, and where the
radio's channel isn't trunked with transmit on (p25t.tx=0).

Skipped without CONFIG_P25, on a channel in a listen-only band, and in hotspot mode."""
import json
import os
import statistics
import tempfile
import time
import zlib

import pytest

from rfoundry_hil import as_int, skip
from hil_config import rig

# Receive work over the signal it covered, P25 running with nothing on the channel. The soft demodulator runs on
# every sample whether a call is there or not, so idle is close to the busy figure
MAX_IDLE_LOAD = float(os.environ.get("RFOUNDRY_HIL_P25_MAX_LOAD", "0.5"))


def p25_ready(radio):
    if "P25" not in radio.features():
        skip("not_built", "built without CONFIG_P25")
    radio.go_home()
    if radio.core().get("hotspot") == "1":
        skip("state", "the radio is in hotspot mode")


def p25_view(radio):
    v = radio.view(["p25"])
    if v.get("p25.built") != "1":
        skip("not_built", "no p25 view section")
    return v


def load(radio):
    """(work_us, span_us) of the provider's receive since P25 started, or None when it doesn't count them."""
    v = radio.view(["p25"])
    w, s = as_int(v.get("p25.rx_work_us")), as_int(v.get("p25.rx_span_us"))
    return None if w is None or s is None else (w, s)


@pytest.mark.feature("p25")
def test_p25_channel_switch(radio):
    p25_ready(radio)
    snap = radio.snapshot()
    mode0 = radio.core().get("mode")
    if mode0 == "p25":
        skip("state", "the channel is P25 already")
    fails0 = as_int(p25_view(radio).get("p25.begin_fails")) or 0
    try:
        radio.link.dmr_fields(mode="p25")
        try:
            v = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1", 3.0,
                               sections=("core", "p25"), what="P25 running on the channel")
        except AssertionError:
            v = radio.view(["core", "p25"])
            if v.get("mode") != "p25":
                skip("state", f"the radio kept mode {v.get('mode')} (a listen-only band, or the VFO without "
                              "both modes)")
            raise
        assert (as_int(v.get("p25.begin_fails")) or 0) == fails0, "the engine did not start (begin_fails)"
        print(f"\np25: nac {v.get('p25.rx_nac')}/{v.get('p25.tx_nac')} tg {v.get('p25.rx_tgid')} "
              f"state {v.get('state')}")
        a = load(radio)
        if a is not None:
            time.sleep(3.0)
            b = load(radio)
            dw, ds = b[0] - a[0], b[1] - a[1]
            assert ds > 0, "the provider counted no receive in 3 s"
            share = dw / ds
            print(f"p25 receive load at idle: {dw} us of work over {ds} us = {share:.1%}")
            assert share <= MAX_IDLE_LOAD, f"receive takes {share:.1%} of the CPU at idle"
        assert radio.core().get("state") in ("idle", "rx")
    finally:
        radio.restore(snap)
    v = radio.wait_for(lambda x: x.get("mode") == mode0 and x.get("p25.on") == "0", 3.0,
                       sections=("core", "p25"), what=f"mode {mode0} back, P25 stopped")
    if mode0 == "dmr" and "DMR" in radio.features():
        radio.wait_for(lambda x: x.get("dmr.active") == "1", 3.0, sections=("dmr",), what="the DMR stack back")


@pytest.mark.feature("p25")
def test_p25_settings(radio):
    p25_ready(radio)
    v = p25_view(radio)
    old = {k: int(v[f"p25.{k}"]) for k in ("rx_nac", "tx_nac", "rx_tgid", "talkaround", "tx_dst", "unit_id")}
    old["unit_call"] = v.get("p25.unit_call") == "1"
    try:
        radio.link.p25_settings_update(rx_nac=0xF7E, rx_tgid=4321, talkaround="on")
        v = radio.wait_for(lambda x: x.get("p25.rx_tgid") == "4321", 2.0, sections=("p25",), what="settings taken")
        assert v.get("p25.rx_nac") == str(0xF7E) and v.get("p25.talkaround") == "1"
        assert v.get("p25.tx_nac") == str(old["tx_nac"]), "a field not given changed"
    finally:
        radio.link.p25_settings(**old)
    radio.wait_for(lambda x: x.get("p25.rx_tgid") == str(old["rx_tgid"]), 2.0, sections=("p25",),
                   what="settings put back")

# --- A stored P25 channel (p25-codeplug.md 2, channel-modes.md 2) ---

STORED_NAC = 0x2A5
KIND_P25 = 2
KEY_CH_TAG = 0x0400     # codeplug-settings.md 3: channel tags, 0x0400 + channel - 1


def name_check(name):
    """The tag's check of a record's 16-octet name (channel-modes.md 2.2): FNV-1a 32, NUL-padded, halves XORed"""
    h, end = 2166136261, False
    for c in bytes(name[:16]).ljust(16, b"\0"):
        c = 0 if end or c == 0xFF else c
        end = end or c == 0
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFF


def area_with(area, key, value):
    """A valid settings area (codeplug-settings.md 2) with key set to value, the other entries and header as found"""
    b = bytes(area)
    hdr, n = b[5], int.from_bytes(b[6:8], "little")
    keep, at = [], 0
    while at < n:
        ln = int.from_bytes(b[hdr + at + 2:hdr + at + 4], "little")
        if int.from_bytes(b[hdr + at:hdr + at + 2], "little") != key:
            keep.append(b[hdr + at:hdr + at + 4 + ln])
        at += 4 + ln
    keep.append(key.to_bytes(2, "little") + len(value).to_bytes(2, "little") + value)
    data = b"".join(keep)
    head = bytearray(b[:hdr])
    head[6:8] = len(data).to_bytes(2, "little")
    head[8:12] = zlib.crc32(data).to_bytes(4, "little")
    out = bytes(head) + data
    assert len(out) <= len(b), "the settings area is full"
    return out + b"\xff" * (len(b) - len(out))


def make_p25(img, number):
    """Channel number of img made a receive-only stored P25 channel: the stand-in record, then its tag"""
    import cps_client
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only
    tag = bytes([KIND_P25, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0])   # no TX flag
    block = (0x8000 | STORED_NAC).to_bytes(2, "little") * 2 + bytes([0])    # RX and TX NAC, normal squelch
    lo, hi = cps_client.SETTINGS_AREA
    img[lo:hi] = area_with(img[lo:hi], KEY_CH_TAG + number - 1, tag + block)


@pytest.mark.feature("p25")
@pytest.mark.codeplug_write
def test_p25_stored_channel(radio, backup_dir):
    p25_ready(radio)
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "p25probe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if core.get("am") == "1" or core.get("bcast") == "1":
        skip("state", "the channel is in a listen-only band: no P25 there")
    ch = cps_client.decode_channel(probe, number)
    _, a = cps_client.channel_addr(number)
    talkaround = ch["rx_hz"] == ch["tx_hz"] or bool(probe[a + 0x26] & 0x04)    # automatic, or direct mode set
    with codeplug_edited(radio, backup_dir, lambda img: make_p25(img, number), "p25", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_P25) and x.get("p25.on") == "1", 5.0,
                           sections=("core", "channel", "p25"), what="the stored P25 channel running")
        assert v.get("mode") == "p25", v
        assert v.get("p25.run.rx_nac") == str(STORED_NAC) and v.get("p25.run.tx_nac") == str(STORED_NAC), v
        assert v.get("p25.run.rx_tgid") == "0", v
        assert v.get("p25.run.talkaround") == ("1" if talkaround else "2"), v
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("p25.on") != "1", 5.0,
                   sections=("core", "channel", "p25"), what="the channel back as it was, P25 stopped")


PTT_S = 3.0
MAX_NID_ERRS = 3        # of the NID's 64 bits (the BCH code corrects 11)
MAX_CORRECTED = 0.005   # of the voice bits heard
DEV_PCT = (80, 115)     # the outer deviation, % of 1800 Hz: the bench board's discriminator scale is not calibrated
LDU_MS = 180.0


def p25_decode(job, *extra):
    """p25tool's JSON lines for an rx job's disc stream (extra: more decode options, as --inbound)"""
    import p25tool
    pcm = job.pcm.get("disc")
    if not pcm:
        skip("bench", "the bench board sent no discriminator stream (the rx op's disc)")
    rate, full = None, None
    for st in (job.header or {}).get("streams", []):
        if st.get("name") == "disc":
            rate, full = st.get("rate", (job.header or {}).get("rate")), st.get("full")
    if not rate or not full:
        skip("bench", f"the bench board gave no rate or full scale for its disc stream: {job.header}")
    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, "disc.raw")
        with open(raw, "wb") as f:
            f.write(pcm)
        try:
            return p25tool.run(["disc", raw, "--rate", int(round(float(rate))), "--full", full] + list(extra))
        except SystemExit as e:
            pytest.fail(f"p25tool: {e}")


@pytest.mark.feature("p25")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_p25_tx_off_air(radio, sdr_bench, pytestconfig):
    mhz = rig(pytestconfig, "p25-mhz")
    hz = round(mhz * 1e6)
    p25_ready(radio)
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    with radio.vfo_session():
        L = radio.link
        L.vfo_set(hz)
        radio.wait_for(lambda v: v.get("rx_hz") == str(hz), what="VFO tuned")
        if radio.core().get("mode") != "p25":
            L.dmr_fields(mode="p25")
        v = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1", 3.0,
                           sections=("core", "p25"), what="P25 running on the VFO")
        assert v.get("tx_hz") == str(hz), f"the VFO transmits on {v.get('tx_hz')}, not {hz}"
        radio.lowest_power()
        nac, tg, src = (int(v[f"p25.{k}"]) for k in ("tx_nac", "tx_dst", "unit_id"))
        if src == 0:        # no P25 unit ID set: the radio's DMR ID
            src = as_int(radio.view(["dmr"]).get("dmr.own_id")) or 0
        group = v.get("p25.unit_call") != "1"
        calls0, under0 = (as_int(v.get(f"p25.{k}")) or 0 for k in ("tx_calls", "tx_underruns"))
        with sdr_bench.lease():
            job = sdr_bench.fm_rx_start(mhz, PTT_S + 4, bw=16000, streams="disc")
            time.sleep(0.5)
            radio.ptt_for(PTT_S)
            radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
            done = job.wait(PTT_S + 20)
        if done.get("gap_samples"):
            skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
        v = radio.view(["p25"])
    lines = p25_decode(job)

    units = [d for d in lines if d["ev"] == "unit"]
    stats = next(d for d in lines if d["ev"] == "stats")
    syncs = [d for d in lines if d["ev"] == "sync" and "outer_hz" in d]
    names = [u["name"] for u in units]
    print(f"\np25 off air: {len(units)} units {' '.join(names)}")
    print(f"framer {stats['framer']}, call {stats['call']}")
    assert units, "nothing decoded"
    c = stats["call"]
    assert c["calls"] == 1, f"{c['calls']} calls"
    assert names[0] == "HDU", f"the call starts with {names[0]}, not a header"
    assert names[-1] in ("TDU", "TDULC"), f"the call ends with {names[-1]}, not a terminator"
    hdr = units[0].get("hdr")
    assert hdr is not None and hdr["algid"] == 0x80, f"header: {hdr}"
    if group:
        assert hdr["tgid"] == tg, f"header talk group {hdr['tgid']}, the radio's {tg}"
    for u in units:
        assert u["nac"] == nac, f"{u['name']} NAC {u['nac']:03X}, the radio's {nac:03X}"
        assert u["nid_errs"] <= MAX_NID_ERRS, f"{u['name']} NID with {u['nid_errs']} bit errors"
    ldus = [u for u in units if u["name"] in ("LDU1", "LDU2")]
    assert [u["name"] for u in ldus] == ["LDU1", "LDU2"] * (len(ldus) // 2) + ["LDU1"] * (len(ldus) % 2), \
        f"LDUs not in turn: {names}"
    for u in ldus:
        if u["name"] == "LDU1":
            lc = u.get("lc") or {}
            assert lc.get("src") == src, f"LDU1 source {lc.get('src')}, the radio's {src}"
            if group:
                assert lc.get("lcf") == 0 and lc.get("tgid") == tg, f"LDU1 link control {lc}"
            else:
                assert lc.get("lcf") == 3 and lc.get("dst") == tg, f"LDU1 link control {lc}"
        else:
            assert (u.get("es") or {}).get("algid") == 0x80, f"LDU2 encryption sync {u.get('es')}"
    gaps = [(b["t_us"] - a["t_us"]) / 1000 for a, b in zip(ldus, ldus[1:])]
    assert all(abs(g - LDU_MS) <= 0.5 for g in gaps), f"LDU spacing {min(gaps):.2f}-{max(gaps):.2f} ms"
    want = PTT_S * 1000 / LDU_MS
    assert abs(len(ldus) - want) <= 2, f"{len(ldus)} LDUs in a {PTT_S} s over (about {want:.0f})"
    assert c["voice_bad"] == 0, f"{c['voice_bad']} voice frames bad"
    share = c["voice_bits"] / max(1, c["voice"] * 144)
    print(f"voice frames {c['voice']}, {c['voice_bits']} bits corrected ({share:.3%})")
    assert share <= MAX_CORRECTED, f"{c['voice_bits']} bits corrected in {c['voice']} voice frames"
    assert c["lc_fail"] == 0 and c["es_fail"] == 0, f"LC fails {c['lc_fail']}, ES fails {c['es_fail']}"
    assert syncs, "the soft modem measured no level"
    outer = statistics.median(d["outer_hz"] for d in syncs)
    offset = statistics.median(d["offset_hz"] for d in syncs)
    print(f"outer deviation {outer:.0f} Hz ({outer / 18:.1f} % of 1800), carrier offset {offset:+.0f} Hz (not judged)")
    assert DEV_PCT[0] <= outer / 18 <= DEV_PCT[1], f"outer deviation {outer:.0f} Hz"
    assert (as_int(v.get("p25.tx_calls")) or 0) == calls0 + 1, "the radio counted no P25 call"
    assert (as_int(v.get("p25.tx_underruns")) or 0) == under0, "the air ran dry during the call"


# --- A trunked channel (p25-trunking.md 7): receive only ---

KIND_P25_TRUNK = 3
CC_WAIT_S = 30.0            # a hunt over every programmed control channel, with its backoff
FOLLOW_WAIT_S = float(os.environ.get("RFOUNDRY_HIL_P25T_FOLLOW_S", "120"))
MAX_CRC_FAIL = 0.2          # of the blocks: a control channel heard well enough to follow


def select_named(radio, name):
    """Select the first channel named name, any zone; AssertionError if none"""
    L = radio.link
    radio.leave_vfo()
    count = int(radio.core().get("zone_count") or 1)
    for zone in range(count):
        radio._req_until(lambda: L.zone(zone), lambda v: v.get("zone_index") == str(zone), f"zone {zone}")
        size = int(radio.core().get("zone_size") or 0)
        for pos in range(size):
            radio._req_until(lambda: L.zone_pos(pos), lambda x: x.get("zone_pos") == str(pos), f"position {pos}")
            if radio.view(["channel"]).get("ch.name") == name:
                return
    raise AssertionError(f"no channel named {name!r} in any zone")


def trunk_view(radio):
    return radio.view(["p25", "p25_trunk"])


@pytest.mark.feature("p25_trunk")
def test_p25_trunk_follow(radio, pytestconfig):
    name = rig(pytestconfig, "p25-trunk-channel")
    if "P25_TRUNK" not in radio.features():
        skip("not_built", "built without CONFIG_P25_TRUNK")
    p25_ready(radio)
    snap = radio.snapshot()
    try:
        select_named(radio, name)
        ch = radio.view(["core", "channel"])
        if ch.get("ch.kind") != str(KIND_P25_TRUNK):
            skip("state", f"channel {name!r} is kind {ch.get('ch.kind')}, not a P25 trunked one")
        if ch.get("mode") != "p25":
            skip("no_hw", "the channel is parked: the board's air interface can't carry P25")
        v = radio.wait_for(lambda x: x.get("p25t.on") == "1" and x.get("p25t.state") == "2", CC_WAIT_S,
                           sections=("p25_trunk",), what="the follower on the system's control channel")
        print(f"\np25 trunk: control channel {int(v['p25t.cc_hz']) / 1e6:.5f} MHz (cc {v['p25t.cc']}), "
              f"NAC {int(v['p25t.nac']):03X}, WACN {int(v['p25t.wacn']):05X} system {int(v['p25t.sysid']):03X} "
              f"RFSS {v['p25t.rfss']} site {v['p25t.site']}")
        time.sleep(3.0)
        v = trunk_view(radio)
        blocks, crc = as_int(v.get("p25t.blocks")) or 0, as_int(v.get("p25t.crc_fail")) or 0
        print(f"blocks {blocks}, CRC failures {crc}, identifiers {int(v['p25t.idens']):04X}, "
              f"grants {v.get('p25t.grants')}, retunes {v.get('p25t.retunes')}, skips {v.get('p25t.skips')}")
        assert blocks > 0, "nothing decoded on the control channel"
        assert crc <= MAX_CRC_FAIL * (blocks + crc), f"{crc} CRC failures to {blocks} blocks"
        calls0 = as_int(v.get("p25.rx_calls")) or 0
        follows0 = as_int(v.get("p25t.follows")) or 0
        end = time.time() + FOLLOW_WAIT_S
        followed = None
        while time.time() < end:
            v = trunk_view(radio)
            if (as_int(v.get("p25t.follows")) or 0) > follows0:
                followed = v
                break
            time.sleep(0.5)
        if followed is None:
            print(f"no call followed in {FOLLOW_WAIT_S:.0f} s (grants {v.get('p25t.grants')}, refused "
                  f"{v.get('p25t.refused')} TG {v.get('p25t.refused_tg')})")
        else:
            print(f"followed TG {followed.get('p25t.tg')} from {followed.get('p25t.src')} on "
                  f"{int(followed['p25t.voice_hz']) / 1e6:.5f} MHz")
            v = radio.wait_for(lambda x: x.get("p25t.state") == "2", 130.0, sections=("p25", "p25_trunk"),
                               what="the follower back on the control channel")
            assert (as_int(v.get("p25.rx_calls")) or 0) > calls0 or (as_int(v.get("p25t.wrong_call")) or 0) > 0 \
                or (as_int(v.get("p25t.voice_lost")) or 0) > 0, "a call followed but nothing received on it"
    finally:
        radio.restore(snap)
    if radio.view(["channel"]).get("ch.kind") != str(KIND_P25_TRUNK):
        radio.wait_for(lambda x: x.get("p25t.on") == "0", 3.0, sections=("p25_trunk",), what="the follower stopped")


# --- A trunked channel with transmit on, against a control channel the bench plays (p25-trunking.md 5, 8) ---

SITE_NAC, SITE_WACN, SITE_SYSID = "293", "BEE00", "2F3"
FSK4_HEX_MAX = 8192         # symbols the bench takes in the request itself (fsk4_bench.py)
SPACING_HZ = 6250
STEP_S = 12.0               # each step's capture: the radio registers within a few seconds of holding the channel
P25T_REG_READY, P25T_REQ_IDLE, P25T_REQ_TALK = 4, 0, 3


def site_plan(cc_hz, in_hz, voice_in_hz):
    """The identifier and channel numbers for the bench's control channel: (base Hz, offset Hz, cc ch, voice ch), or
    skip when the frequencies don't fit one identifier"""
    off = in_hz - cc_hz
    voice_hz = voice_in_hz - off
    base = min(cc_hz, voice_hz) // SPACING_HZ * SPACING_HZ
    n_cc, n_voice = (cc_hz - base) / SPACING_HZ, (voice_hz - base) / SPACING_HZ
    if off % 250000 or abs(off) // 250000 > 8191 or not n_cc.is_integer() or not n_voice.is_integer() \
            or max(n_cc, n_voice) > 0xFFF or n_cc == n_voice:
        skip("state", f"control channel {cc_hz} Hz (inbound {in_hz}), voice inbound {voice_in_hz}: not one "
                      f"identifier's channels ({SPACING_HZ} Hz steps, the offset a multiple of 250 kHz)")
    return base, off, 0x1000 | int(n_cc), 0x1000 | int(n_voice)


def cc_file(tmp, plan, *answers):
    """p25tool.py cc: the bench's control channel, answers (cc options) in every unit; at most FSK4_HEX_MAX symbols.
    Returns (path, symbols)"""
    import p25tool
    base, off, cc, _ = plan
    path, exp = os.path.join(tmp, "cc.dibits"), os.path.join(tmp, "cc.json")
    for units in (27, 24, 20, 16):
        p25tool.run(["cc", path, exp, "--units", units, "--nac", SITE_NAC, "--wacn", SITE_WACN, "--sysid", SITE_SYSID,
                     "--ch", f"{cc:x}", "--base", base, "--spacing", SPACING_HZ, "--tx-offset", off] + list(answers))
        with open(exp) as f:
            n = json.load(f)["total_syms"]
        if n <= FSK4_HEX_MAX:
            return path, n
    raise AssertionError(f"the control channel file holds {n} symbols, past {FSK4_HEX_MAX}")


def play_cc(sdr_bench, mhz, path, nsyms, seconds):
    """The bench's fsk4 transmitter on mhz, the file repeated for about seconds (fsk4_bench.py tx's request). The
    request's fields as fsk4_bench.py sends them: not yet run through SdrBench"""
    with open(path, "rb") as f:
        data = f.read()
    return sdr_bench.tx_start("fsk4", mhz, None, max_s=seconds + 10, symbols=nsyms, hex=data.hex(), shape="c4fm",
                              repeat=max(1, int(seconds * 4800 / nsyms) + 1))


def capture(sdr_bench, mhz, seconds):
    job = sdr_bench.fm_rx_start(mhz, seconds, bw=16000, streams="disc")
    done = job.wait(seconds + 20)
    if done.get("gap_samples"):
        skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
    return job


def inbound_blocks(job, kind):
    lines = p25_decode(job, "--inbound")
    return [b["msg"] for d in lines if d["ev"] == "unit" for b in d.get("blocks", [])
            if b["errs"] >= 0 and b["msg"].get("kind") == kind]


@pytest.mark.feature("p25_trunk")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.sdr_rx
def test_p25_trunk_tx(radio, sdr_bench, pytestconfig):
    name = rig(pytestconfig, "p25-trunk-tx-channel")
    cc_mhz, in_mhz, voice_mhz = (rig(pytestconfig, k) for k in ("p25-cc-mhz", "p25-cc-in-mhz", "p25-voice-mhz"))
    if "P25_TRUNK" not in radio.features():
        skip("not_built", "built without CONFIG_P25_TRUNK")
    p25_ready(radio)
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    plan = site_plan(round(cc_mhz * 1e6), round(in_mhz * 1e6), round(voice_mhz * 1e6))
    voice_rx_hz = round(voice_mhz * 1e6) - plan[1]
    snap = radio.snapshot()
    try:
        select_named(radio, name)
        ch = radio.view(["core", "channel"])
        if ch.get("ch.kind") != str(KIND_P25_TRUNK):
            skip("state", f"channel {name!r} is kind {ch.get('ch.kind')}, not a P25 trunked one")
        if ch.get("mode") != "p25":
            skip("no_hw", "the channel is parked: the board's air interface can't carry P25")
        if trunk_view(radio).get("p25t.tx") != "1":
            skip("state", f"channel {name!r}: transmit is off (listen only, no unit ID or no talk group)")
        radio.lowest_power()
        with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
            # 1. Broadcasts only: the registration request
            path, n = cc_file(tmp, plan)
            tx = play_cc(sdr_bench, cc_mhz, path, n, STEP_S + 4)
            radio.wait_for(lambda x: x.get("p25t.state") == "2", CC_WAIT_S, sections=("p25_trunk",),
                           what="the follower on the bench's control channel")
            reg = inbound_blocks(capture(sdr_bench, in_mhz, STEP_S), "u_reg_req")
            tx.wait(STEP_S + 30)
            print(f"\nstep 1: {len(reg)} registration requests {reg[:1]}")
            assert reg, "no registration request on the inbound frequency"
            unit = reg[0]["src"]
            assert reg[0]["wacn"] == int(SITE_WACN, 16) and reg[0]["sysid"] == int(SITE_SYSID, 16), reg[0]
            # 2. Registration accepted: the affiliation request
            path, n = cc_file(tmp, plan, "--reg", unit, "--reg-at", 0)
            tx = play_cc(sdr_bench, cc_mhz, path, n, STEP_S + 4)
            aff = inbound_blocks(capture(sdr_bench, in_mhz, STEP_S), "grp_aff_req")
            tx.wait(STEP_S + 30)
            print(f"step 2: {len(aff)} affiliation requests {aff[:1]}")
            assert aff and all(a["src"] == unit for a in aff), f"affiliation requests {aff}"
            tg = aff[0]["tg"]
            # 3. Affiliation accepted and the grant: PTT, the voice channel captured
            path, n = cc_file(tmp, plan, "--reg", unit, "--reg-at", 0, "--aff", f"{unit}:{tg}", "--aff-at", 0,
                              "--grant", f"{tg}:{plan[3]:x}:{unit}", "--grant-at", 0)
            tx = play_cc(sdr_bench, cc_mhz, path, n, PTT_S + STEP_S)
            v = radio.wait_for(lambda x: x.get("p25t.reg") == str(P25T_REG_READY), STEP_S,
                               sections=("p25_trunk",), what="registered and affiliated")
            req0 = as_int(v.get("p25t.req_tx")) or 0
            calls0 = as_int(radio.view(["p25"]).get("p25.tx_calls")) or 0
            job = sdr_bench.fm_rx_start(voice_mhz, PTT_S + 4, bw=16000, streams="disc")
            time.sleep(0.5)
            radio.ptt_for(PTT_S)
            done = job.wait(PTT_S + 20)
            v = radio.wait_for(lambda x: x.get("p25t.state") == "2" and x.get("p25t.req") == str(P25T_REQ_IDLE),
                               10.0, sections=("p25", "p25_trunk"), what="back on the control channel")
            tx.wait(PTT_S + STEP_S + 30)
    finally:
        radio.restore(snap)
    if done.get("gap_samples"):
        skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
    print(f"step 3: req_tx {v.get('p25t.req_tx')}, grants {v.get('p25t.req_grant')}, talk {v.get('p25t.talk_hz')}")
    assert (as_int(v.get("p25t.req_tx")) or 0) > req0, "no voice request sent"
    assert v.get("p25t.talk_hz") == str(voice_rx_hz), f"talked on {v.get('p25t.talk_hz')}, not {voice_rx_hz}"
    assert (as_int(v.get("p25.tx_calls")) or 0) == calls0 + 1, "the radio counted no P25 call"
    lines = p25_decode(job)
    units = [d for d in lines if d["ev"] == "unit"]
    names = [u["name"] for u in units]
    print(f"voice channel: {' '.join(names)}")
    assert names and names[0] == "HDU" and names[-1] in ("TDU", "TDULC"), f"the call: {names}"
    for u in units:
        if u["name"] == "LDU1":
            lc = u.get("lc") or {}
            assert lc.get("src") == unit and lc.get("tgid") == tg, f"LDU1 link control {lc}"
