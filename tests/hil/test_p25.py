# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""P25 conventional on the radio, with nothing sent (docs/spec/p25.md 7). Radio only: the radio never transmits.

test_p25_channel_switch: the channel on view becomes P25 by a temporary mode change (the debug link's mode request,
debuglink.md 3.8 op 31); the p25 view section must show the engine running (`p25.on=1`, no new `begin_fails`) and,
where the provider counts it, the receive load at idle (`p25.rx_work_us / p25.rx_span_us`, at most MAX_IDLE_LOAD).
Undoing the temporary change brings the channel's own mode back with the engine stopped, and a DMR channel's call
stack running again.

test_p25_settings: the P25 settings (op 42, RAM only) round trip through the p25 view and are put back.

test_p25_stored_channel (opt-in: --allow-codeplug-write; nothing is sent): the channel on view is made a stored P25
channel over CPS (p25-codeplug.md 2): its record the safe stand-in, DMR and receive only, and a tag of kind 2 with no
system, receive only, NAC STORED_NAC both ways and normal squelch in the codeplug settings area. The radio must read it
back as that kind (`ch.kind=2`), run P25 on it (`mode=p25`, `p25.on=1`) with the channel's parameters, not the
settings' (`p25.run.rx_nac`, `p25.run.tx_nac`, `p25.run.rx_tgid=0`, `p25.run.talkaround` 1 on a simplex or direct-mode
channel, else 2), and drop it again once the backup is written back. Skipped where the settings area isn't ours to write (not
valid in the backup) and where the radio can't carry P25 (no P25 in its feature list).

test_p25_stored_channel_tx (opt-in: --allow-rf --allow-service-stop --allow-codeplug-write --p25-mhz MHz; the radio
transmits): the channel on view is made a stored P25 channel that transmits, simplex on --p25-mhz with NAC STORED_NAC,
keyed once at the lowest power and decoded off air as in test_p25_tx_off_air: the call must carry the channel's NAC,
talk target and unit ID (`p25.run.*`), not the settings'.

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

test_p25_rx_from_bench (opt-in: --allow-rf --allow-service-stop --p25-mhz MHz; the bench transmits, the radio only
receives; the speaker plays the call's voice frames): the VFO on --p25-mhz in P25, receiving every talk group, while
the bench's fsk4 transmitter plays a call p25tool.py gen writes: a header, RX_PAIRS LDU pairs from RX_SRC to talk
group RX_TG with the radio's receive NAC, a terminator. On the radio: one reception more (p25.rx_calls) from that
unit to that talk group (p25.src, p25.dst), at least the call's voice frames less one LDU (p25.voice), at most
MAX_RX_BAD of them bad, no link control failure, and voice frames played where the build has the decoder
(p25v.frames).

test_p25_trunk_follow (opt-in: --p25-trunk-channel NAME; nothing is sent, PTT is not pressed): the stored P25 trunked
channel of that name, its system's control channel heard at the rig (p25-trunking.md 7). The radio must run P25 on it
(`mode=p25`, `ch.kind=3`), the follower must hold a control channel (`p25t.state=2`) within CC_WAIT_S with most blocks
decoded (CRC failures at most MAX_CRC_FAIL of them), and the system it names must be the one the codeplug gives (when
that isn't "any"; printed). For FOLLOW_WAIT_S it watches for a call it follows: one followed counts a reception
(`p25.rx_calls`), and the follower is back on the control channel afterwards. No call in that time is printed, not
failed: traffic is the system's. Leaving the channel stops the follower (`p25t.on=0`). Skipped without
CONFIG_P25_TRUNK, and where the board can't carry P25 (the channel stays parked).

test_p25_trunk_tx (opt-in: --allow-rf --allow-service-stop --allow-codeplug-write --p25-cc-mhz MHz --p25-cc-in-mhz MHz
--p25-voice-mhz MHz; the bench and the radio transmit): the channel on view is made a stored P25 trunked channel over
CPS (p25-codeplug.md 3, channel-modes.md 7): its record the stand-in on the control channel pair, a tag of kind 3 with
transmit on, and a system that takes WACN SITE_WACN, system SITE_SYSID, NAC SITE_NAC with one control channel at
--p25-cc-mhz, its inbound frequency --p25-cc-in-mhz (the offset a multiple of 250 kHz) and one identifier. The backup
is written back afterwards. The bench plays a control channel that p25tool.py cc writes (p25-trunking.md 8) with, in
turn, one a unit, the answers to the radio's unit and talk group (p25.run.unit_id, p25.run.tx_dst) and a grant of a
voice channel whose inbound side is --p25-voice-mhz: an answer counts only once the radio asked. Each play is PLAY_S
long (inside a bench board's key time), and the bench's receive board records one frequency per play, from the request
on:
1. The inbound frequency. The radio must hold the control channel (p25t.state=2), register and affiliate (p25t.reg=4);
   then PTT for TRUNK_PTT_S. Off air (p25tool.py decode --inbound): a registration request with the site's WACN and
   system ID and the radio's unit, an affiliation request and a voice request for its talk group. On the radio: a
   request sent (p25t.req_tx), a grant taken (p25t.req_grant), a call sent (p25.tx_calls) on the voice channel
   (p25t.talk_hz).
2. The voice channel's inbound frequency: PTT again once the radio is back on the control channel, still registered.
   Off air: a conventional call, a header first, LDU1s with the radio's unit and talk group, a terminator last.
After each call the radio is back on the control channel with the request idle (p25t.req=0). Skipped without
CONFIG_P25_TRUNK, where the bench board has no fsk4 transmitter or no FM receiver, where the settings area isn't ours to
write or holds a system already, and where the radio has no unit ID or talk group to ask with (p25t.tx=0).

test_p25_trunk_rx (opt-in: --allow-rf --allow-service-stop --allow-codeplug-write --p25-cc-mhz MHz
--p25-voice-out-mhz MHz --bench-api-2 HOST; two bench boards transmit, the radio only receives): the channel on view
is made a stored P25 trunked channel the same way, but with transmit off on a listen-only system, so the radio never
keys (p25t.tx=0). For PLAY_S the first board plays a control channel that grants the channel's talk group
(p25.run.tx_dst) a voice channel at --p25-voice-out-mhz to another unit, RX_SRC, in every third unit, while the second
board (--bench-api-2) plays that unit's call there, over and over: the two boards key seconds after their requests
and not together, so neither the grant nor the call has one moment. The radio must hold the control channel, follow
a grant (p25t.follows) to the voice channel's frequency (p25t.voice_hz), receive the call (p25.rx_calls, at least one
LDU pair's voice frames, its unit and talk group from the link control, frames played) and be back on the control
channel while that still plays. The backup is written back afterwards. Skipped as test_p25_trunk_tx is, and without a
second transmitter.

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
        if snap["vfo"] == "1":      # the VFO's mode is its own: restore() only drops a channel's mode for now
            radio.link.dmr_fields(mode=mode0)
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


def make_p25(img, number, tx_hz=None):
    """Channel number of img made a stored P25 channel: the stand-in record, then its tag. Receive only on the
    record's frequencies, or with tx_hz a simplex channel there that transmits"""
    import cps_client
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: the stand-in, whatever the tag allows
    if tx_hz is not None:
        img[a + 0x10:a + 0x14] = img[a + 0x14:a + 0x18] = bcd_hz(tx_hz)
    tag = bytes([KIND_P25, 0xFF]) + name_check(img[a:a + 16]).to_bytes(2, "little") \
        + bytes([0x01 if tx_hz is not None else 0])     # flag bit 0: transmit allowed
    block = bytes([0]) + (0x8000 | STORED_NAC).to_bytes(2, "little") * 2    # normal squelch, RX and TX NAC
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
FSK4_HEX_MAX = 8192     # symbols the bench takes in the request itself (fsk4_bench.py)
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
@pytest.mark.needs_switch("VFO")
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
        job, grew = key_over(radio, sdr_bench, mhz, v)
    check_over(p25_decode(job), nac, tg, src, group, grew)


def key_over(radio, sdr_bench, mhz, v0):
    """One over of PTT_S recorded by the bench on mhz. Returns (the capture's job, how much the radio's p25.tx_calls
    and p25.tx_underruns grew since the view v0)"""
    with sdr_bench.lease():
        job = sdr_bench.fm_rx_start(mhz, PTT_S + 4, bw=16000, streams="disc")
        time.sleep(0.5)
        radio.ptt_for(PTT_S)
        radio.wait_for(lambda x: x.get("state") != "tx", timeout=3, what="TX ended")
        done = job.wait(PTT_S + 20)
    if done.get("gap_samples"):
        skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
    v = radio.view(["p25"])
    return job, tuple((as_int(v.get(f"p25.{k}")) or 0) - (as_int(v0.get(f"p25.{k}")) or 0)
                      for k in ("tx_calls", "tx_underruns"))


def check_over(lines, nac, tg, src, group, grew):
    """The pass criteria of an over decoded off air (p25tool's lines): one call from src with nac, to talk group tg
    (group) or unit tg"""
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
    assert grew[0] == 1, f"the radio counted {grew[0]} P25 calls"
    assert grew[1] == 0, "the air ran dry during the call"


@pytest.mark.feature("p25")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
@pytest.mark.bench_service
@pytest.mark.sdr_rx
def test_p25_stored_channel_tx(radio, sdr_bench, backup_dir, pytestconfig):
    mhz = rig(pytestconfig, "p25-mhz")
    hz = round(mhz * 1e6)
    p25_ready(radio)
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]      # with the settings area, where the tag goes
    probe, _ = read_backup(radio, backup_dir, "p25txprobe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    with codeplug_edited(radio, backup_dir, lambda img: make_p25(img, number, hz), "p25tx", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_P25) and x.get("p25.on") == "1", 5.0,
                           sections=("core", "channel", "p25"), what="the stored P25 channel running")
        assert v.get("mode") == "p25", v
        assert v.get("rx_hz") == str(hz) and v.get("tx_hz") == str(hz), \
            f"the channel is on {v.get('rx_hz')} / {v.get('tx_hz')}, not {hz}"
        assert v.get("p25.run.tx_nac") == str(STORED_NAC), v
        radio.lowest_power()
        tg, src = (as_int(v.get(f"p25.run.{k}")) or 0 for k in ("tx_dst", "unit_id"))
        if src == 0:        # no P25 unit ID set: the radio's DMR ID
            src = as_int(radio.view(["dmr"]).get("dmr.own_id")) or 0
        job, grew = key_over(radio, sdr_bench, mhz, v)
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("p25.on") != "1", 5.0,
                   sections=("core", "channel", "p25"), what="the channel back as it was, P25 stopped")
    check_over(p25_decode(job), STORED_NAC, tg, src, v.get("p25.run.unit_call") != "1", grew)


# --- Receive: a call the bench plays ---

RX_PAIRS = 4            # LDU pairs: with the header and the terminator the call goes in one request
RX_SRC, RX_TG = 1234567, 4001
MAX_RX_BAD = 0.05       # of the voice frames heard
NAC_DEFAULT = 0x293


@pytest.mark.feature("p25")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.needs_switch("VFO")
def test_p25_rx_from_bench(radio, sdr_bench, pytestconfig):
    import p25tool
    mhz = rig(pytestconfig, "p25-mhz")
    hz = round(mhz * 1e6)
    p25_ready(radio)
    v = p25_view(radio)
    old = {k: int(v[f"p25.{k}"]) for k in ("rx_nac", "tx_nac", "rx_tgid", "talkaround", "tx_dst", "unit_id")}
    old["unit_call"] = v.get("p25.unit_call") == "1"
    nac = old["rx_nac"] if old["rx_nac"] < 0xF7E else NAC_DEFAULT      # 0xF7E, 0xF7F: any NAC
    with radio.vfo_session():
        L = radio.link
        try:
            L.vfo_set(hz)
            radio.wait_for(lambda x: x.get("rx_hz") == str(hz), what="VFO tuned")
            L.p25_settings_update(rx_tgid=0)
            if radio.core().get("mode") != "p25":
                L.dmr_fields(mode="p25")
            v0 = radio.wait_for(lambda x: x.get("mode") == "p25" and x.get("p25.on") == "1"
                                and x.get("p25.rx_tgid") == "0", 3.0, sections=("core", "p25"),
                                what="P25 running on the VFO")
            with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
                path, exp = os.path.join(tmp, "call.dibits"), os.path.join(tmp, "call.json")
                p25tool.run(["gen", path, exp, "--nac", f"{nac:x}", "--tg", RX_TG, "--src", RX_SRC, "--pairs", RX_PAIRS,
                             "--lead", 96, "--tail", 48])
                with open(exp) as f:
                    n = json.load(f)["total_syms"]
                assert n <= FSK4_HEX_MAX, f"the call holds {n} symbols, past {FSK4_HEX_MAX}"
                with open(path, "rb") as f:
                    data = f.read()
                tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=n / 4800 + 2, symbols=n, hex=data.hex(), shape="c4fm",
                                        clip_hz=4064)
                tx.wait(n / 4800 + 30)
            v1 = radio.wait_for(lambda x: x.get("p25.rx") != "1", 3.0, sections=("core", "p25"),
                                what="the reception ended")
        finally:
            L.p25_settings(**old)

    def grew(key):
        return (as_int(v1.get(key)) or 0) - (as_int(v0.get(key)) or 0)

    sent = RX_PAIRS * 18
    print(f"\np25 from the bench: calls {grew('p25.rx_calls')}, units {grew('p25.units')}, voice frames "
          f"{grew('p25.voice')} of {sent} ({grew('p25.voice_bad')} bad, {grew('p25.voice_bits')} bits corrected), "
          f"played {grew('p25v.frames') if 'p25v.frames' in v1 else 'n/a'}, from {v1.get('p25.src')} to "
          f"{v1.get('p25.dst')}, RSSI {v1.get('rssi_dbm')} dBm")
    assert grew("p25.rx_calls") == 1, f"{grew('p25.rx_calls')} receptions counted"
    assert v1.get("p25.src") == str(RX_SRC) and v1.get("p25.dst") == str(RX_TG), \
        f"the call {v1.get('p25.src')} -> {v1.get('p25.dst')}, sent {RX_SRC} -> {RX_TG}"
    assert grew("p25.voice") >= sent - 9, f"{grew('p25.voice')} voice frames of {sent}"
    assert grew("p25.voice_bad") <= MAX_RX_BAD * grew("p25.voice"), f"{grew('p25.voice_bad')} voice frames bad"
    assert grew("p25.lc_fail") == 0, f"{grew('p25.lc_fail')} link control failures"
    if "p25v.frames" in v1 and v1.get("p25v.built") == "1":
        assert grew("p25v.frames") > 0, "no voice frame played"


SCAN_CALLS = 4          # calls the bench sends while the scan comes round to the P25 channel


@pytest.mark.feature("p25")
@pytest.mark.feature("scan")
@pytest.mark.rf
@pytest.mark.codeplug_write
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.parametrize("kind", ["zone", "dual"])
def test_p25_scan_from_bench(radio, sdr_bench, backup_dir, pytestconfig, kind):
    """A zone scan, or dual watch with the zone's next channel, holds on a stored P25 channel when the bench sends a
    call there (p25.md 7.1, radio-features.md 2.1): the current channel made a P25 channel on p25-mhz, the scan
    started, the bench's calls to RX_TG until it holds there"""
    import p25tool
    import cps_client
    from test_cps import codeplug_edited, pick_channel, read_backup
    mhz = rig(pytestconfig, "p25-mhz")
    p25_ready(radio)
    if as_int(radio.link.view(["features"]).get("scan.built"), 0) != 1:
        skip("not_built", "built without CONFIG_SCAN")
    regions = cps_client.REGIONS["radio"]
    probe, _ = read_backup(radio, backup_dir, "p25scan", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the tag back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    if as_int(core.get("zone_size"), 0) < 2:
        skip("state", "the current zone needs two channels or more to scan")
    with codeplug_edited(radio, backup_dir, lambda img: make_p25(img, number, round(mhz * 1e6)), "p25scan", regions):
        radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_P25) and x.get("p25.on") == "1", 5.0,
                       sections=("core", "channel", "p25"), what="the stored P25 channel running")
        if kind == "dual":
            radio.link.scan("dual", (as_int(core.get("zone_pos"), 0) + 1) % as_int(core.get("zone_size"), 1))
        else:
            radio.link.scan("zone")
        held = None
        # a second receiver (dual.both) takes the other channel and the main one stays here: no hold phase, the call
        # playing on this channel is the stop
        calls0 = as_int(radio.link.view(["p25"]).get("p25.rx_calls"), 0)
        try:
            radio.wait_for(lambda x: x.get("scan") == kind, 1.0, sections=("features",), what=f"scan={kind}")
            with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
                path, exp = os.path.join(tmp, "call.dibits"), os.path.join(tmp, "call.json")
                p25tool.run(["gen", path, exp, "--nac", f"{STORED_NAC:x}", "--tg", RX_TG, "--src", RX_SRC,
                             "--pairs", RX_PAIRS, "--lead", 96, "--tail", 48])
                with open(exp) as f:
                    n = json.load(f)["total_syms"]
                with open(path, "rb") as f:
                    data = f.read()
                for _ in range(SCAN_CALLS):
                    tx = sdr_bench.tx_start("fsk4", mhz, None, max_s=n / 4800 + 2, symbols=n, hex=data.hex(),
                                            shape="c4fm", clip_hz=4064)
                    while held is None and not tx.req.done.is_set():
                        v = radio.link.view(["core", "features", "p25"])
                        stop = v.get("scan_phase") == "hold" or (v.get("dual.both") == "1" and
                                                               as_int(v.get("p25.rx_calls"), 0) > calls0)
                        if stop and v.get("channel_number") == str(number):
                            held = v
                    tx.wait(n / 4800 + 30)
                    if held is not None:
                        break
        finally:
            radio.link.scan("cancel")
            radio.wait_for(lambda x: x.get("scan") == "off", 3.0, sections=("features",), what="scan off")
    assert held is not None, f"the {kind} scan never held on channel {number} in {SCAN_CALLS} calls"
    print(f"\n{kind} scan held on channel {number}: {held.get('p25.src')} -> {held.get('p25.dst')}")
    assert held.get("p25.dst") in (str(RX_TG), "0"), held     # 0: held before the link control was read


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
SPACING_HZ = 6250
PLAY_S = 7.0                # one keying of the bench's control channel: inside a bench board's key time
TRUNK_PTT_S = 2.0
KEY_WAIT_S = 22.0           # a bench board prepares a play before it keys (about twice the play's length): the
                            # recording starts with the request and runs this much longer
READY_S = 4.0               # from the control channel's start: found, registered, affiliated
P25T_REG_READY, P25T_REQ_IDLE = 4, 0
TRUNK_KEYS = ("p25t.state", "p25t.reg", "p25t.req", "p25t.cc_hz", "p25t.blocks", "p25t.crc_fail", "p25t.hunts",
              "p25t.wrong_sys", "p25t.reg_tx", "p25t.reg_ok", "p25t.aff_tx", "p25t.aff_ok", "p25t.req_tx",
              "p25t.req_grant", "p25t.denials", "p25t.timeouts", "p25t.send_fail", "p25t.inb_busy", "p25t.bursts",
              "p25t.burst_refused")     # what a failure prints
KEY_SYSTEM, KEY_SYS_ENTRY = 0x0100, 0x0200      # codeplug-settings.md 3: system 0, its entries 0 and 1


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


def area_keys(area):
    """The keys of a valid settings area's entries (codeplug-settings.md 2)"""
    b = bytes(area)
    hdr, n = b[5], int.from_bytes(b[6:8], "little")
    keys, at = [], 0
    while at < n:
        keys.append(int.from_bytes(b[hdr + at:hdr + at + 2], "little"))
        at += 4 + int.from_bytes(b[hdr + at + 2:hdr + at + 4], "little")
    return keys


def bcd_hz(hz):
    """A channel record's frequency field: 10 Hz steps, 8 BCD digits, the lowest pair first"""
    v, out = hz // 10, bytearray(4)
    for i in range(4):
        out[i] = (v // 10 % 10) << 4 | v % 10
        v //= 100
    return out


def make_p25_trunk(img, number, plan, cc_hz, in_hz, tx=True):
    """Channel number of img made a stored P25 trunked channel: the stand-in record on the control channel pair (the
    frequencies the views show before the follower runs), system 0 with its control channel and one identifier, then
    the channel's tag. tx: transmit on in the system and the tag (off: a listen-only system, the radio never keys)"""
    import cps_client
    base, off, cc, _ = plan
    _, a = cps_client.channel_addr(number)
    img[a + 0x18] = 1                   # DMR
    img[a + 0x33] |= 0x04               # receive only: what a build without P25 would do with the record
    img[a + 0x10:a + 0x14] = bcd_hz(cc_hz)
    img[a + 0x14:a + 0x18] = bcd_hz(in_hz)
    lo, hi = cps_client.SETTINGS_AREA
    area = bytes(img[lo:hi])
    # channel-modes.md 7.1: type 3 (P25 trunked), flags, the name, WACN, system ID, NAC
    system = bytes([3, 0x02 if tx else 0, 0, 5]) + bytes(8) + b"HIL P25T".ljust(16, b"\0") \
        + int(SITE_WACN, 16).to_bytes(3, "little") + int(SITE_SYSID, 16).to_bytes(2, "little") \
        + (0x8000 | int(SITE_NAC, 16)).to_bytes(2, "little") + bytes(3)
    area = area_with(area, KEY_SYSTEM, system)
    # 7.2: system 0's control channel (class 0), then its identifier 0 (class 2)
    area = area_with(area, KEY_SYS_ENTRY, bytes([0, 0, 0, 0x01]) + cc.to_bytes(2, "little")
                     + cc_hz.to_bytes(4, "little") + in_hz.to_bytes(4, "little"))
    area = area_with(area, KEY_SYS_ENTRY + 1, bytes([0, 2, 1, 0]) + base.to_bytes(4, "little")
                     + SPACING_HZ.to_bytes(4, "little") + off.to_bytes(4, "little", signed=True)
                     + (12500).to_bytes(4, "little") + bytes(2))
    tag = bytes([KIND_P25_TRUNK, 0]) + name_check(img[a:a + 16]).to_bytes(2, "little") + bytes([0x01 if tx else 0, 0])
    img[lo:hi] = area_with(area, KEY_CH_TAG + number - 1, tag)


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
    """The bench's fsk4 transmitter on mhz: the file's whole plays that fit in seconds, one keying"""
    with open(path, "rb") as f:
        data = f.read()
    return sdr_bench.tx_start("fsk4", mhz, None, max_s=seconds + 1, symbols=nsyms, hex=data.hex(), shape="c4fm",
                              clip_hz=4064, repeat=max(1, int(seconds * 4800 / nsyms)))


def trunk_slot(radio, backup_dir):
    """Where a test writes its trunked channel: (the stored channel on view, the CPS regions with the settings area,
    where the system and the tag go). Skip where the area isn't valid, the radio isn't on a stored channel, or the
    codeplug holds system 0 or the channel's tag already"""
    import cps_client
    from test_cps import pick_channel, read_backup
    regions = cps_client.REGIONS["radio"]
    probe, _ = read_backup(radio, backup_dir, "p25tprobe", regions)
    lo, hi = cps_client.SETTINGS_AREA
    if cps_client.area_state(probe[lo:hi]) != "valid":
        skip("state", "the codeplug settings area isn't valid: the restore couldn't take the system back")
    core = radio.core()
    number = pick_channel(radio, probe)
    if core.get("vfo") == "1" or number != as_int(core.get("channel_number"), 0):
        skip("state", "the radio isn't on a stored channel")
    used = {KEY_SYSTEM, KEY_SYS_ENTRY, KEY_SYS_ENTRY + 1, KEY_CH_TAG + number - 1} & set(area_keys(probe[lo:hi]))
    if used:
        skip("state", f"the codeplug holds a system or this channel's tag already (keys {sorted(used)})")
    return number, regions


def trunk_call(radio, sdr_bench, cc_mhz, cc, rx_mhz, need_ready):
    """One play of the control channel cc (path, symbols) with rx_mhz recorded: PTT for TRUNK_PTT_S once the radio is
    on it (registered and affiliated within READY_S when need_ready, else as soon as it is). Returns (the capture's
    job, the views before the PTT and after the call)"""
    job = sdr_bench.fm_rx_start(rx_mhz, KEY_WAIT_S + PLAY_S, bw=16000, streams="disc")
    t0 = time.monotonic()
    tx = play_cc(sdr_bench, cc_mhz, cc[0], cc[1], PLAY_S)       # returns once the board keyed
    if time.monotonic() - t0 > KEY_WAIT_S:
        tx.wait(PLAY_S + 20)
        skip("bench", f"the bench board keyed {time.monotonic() - t0:.0f} s after the request: past the recording")
    try:
        end = time.monotonic() + READY_S
        while True:
            v0 = radio.view(["core", "p25", "p25_trunk"])
            if v0.get("p25t.state") == "2" and v0.get("p25t.reg") == str(P25T_REG_READY):
                break
            if time.monotonic() > end:
                raise AssertionError(("not registered and affiliated" if need_ready else "not ready again")
                                     + f" on the bench's control channel within {READY_S} s: "
                                     + ", ".join(f"{k[5:]} {v0.get(k)}" for k in TRUNK_KEYS))
            time.sleep(0.05)
        radio.ptt_for(TRUNK_PTT_S)
        v1 = radio.wait_for(lambda x: x.get("state") != "tx" and x.get("p25t.state") == "2"
                            and x.get("p25t.req") == str(P25T_REQ_IDLE), 2.0, sections=("core", "p25", "p25_trunk"),
                            what="back on the control channel, the request idle")
    finally:
        radio.link.ptt(False)
        tx.wait(PLAY_S + 20)
        done = job.wait(KEY_WAIT_S + PLAY_S + 20)
    if done.get("gap_samples"):
        skip("bench", f"the bench board lost samples (gap_samples {done['gap_samples']})")
    print(f"\n{rx_mhz} MHz recorded: {len(job.pcm.get('disc') or b'')} bytes, "
          + ", ".join(f"{k} {done[k]}" for k in ("seconds", "carrier_s", "samples", "blocks") if k in done))
    return job, v0, v1


def inbound_blocks(job):
    """The inbound blocks p25tool decodes from a capture, by kind"""
    kinds = {}
    for d in p25_decode(job, "--inbound"):
        if d["ev"] == "unit":
            for b in d.get("blocks", []):
                if b["errs"] >= 0:
                    kinds.setdefault(b["msg"].get("kind"), []).append(b["msg"])
    return kinds


@pytest.mark.feature("p25_trunk")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
@pytest.mark.bench_service
@pytest.mark.bench_modem
@pytest.mark.sdr_rx
def test_p25_trunk_tx(radio, sdr_bench, backup_dir, pytestconfig):
    cc_mhz, in_mhz, voice_mhz = (rig(pytestconfig, k) for k in ("p25-cc-mhz", "p25-cc-in-mhz", "p25-voice-mhz"))
    if "P25_TRUNK" not in radio.features():
        skip("not_built", "built without CONFIG_P25_TRUNK")
    p25_ready(radio)
    if not sdr_bench.can_fm_rx():
        skip("bench", f"the bench board {sdr_bench.api_rx} has no FM receive")
    from test_cps import codeplug_edited
    cc_hz, in_hz, voice_in_hz = (round(m * 1e6) for m in (cc_mhz, in_mhz, voice_mhz))
    plan = site_plan(cc_hz, in_hz, voice_in_hz)
    number, regions = trunk_slot(radio, backup_dir)
    with codeplug_edited(radio, backup_dir, lambda img: make_p25_trunk(img, number, plan, cc_hz, in_hz), "p25t",
                         regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_P25_TRUNK) and x.get("p25t.on") == "1", 6.0,
                           sections=("core", "channel", "p25", "p25_trunk"), what="the trunked channel running")
        if v.get("mode") != "p25":
            skip("no_hw", "the channel is parked: the board's air interface can't carry P25")
        if v.get("p25t.tx") != "1":
            skip("state", "the follower's transmit side is off (no unit ID or no talk group)")
        assert v.get("p25t.cc_hz") == str(cc_hz), f"the follower's control channel {v.get('p25t.cc_hz')}"
        unit = as_int(v.get("p25.run.unit_id")) or as_int(v.get("p25.unit_id")) or 0
        tg = as_int(v.get("p25.run.tx_dst")) or 0
        if not unit or not tg:
            skip("state", f"no unit ID or talk group to answer (unit {unit}, talk group {tg})")
        radio.lowest_power()
        with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease():
            # each answer in every third unit: the broadcasts keep their blocks (a unit holds three)
            cc = cc_file(tmp, plan, "--reg", unit, "--reg-at", "0:0:3", "--aff", f"{unit}:{tg}", "--aff-at", "1:0:3",
                         "--grant", f"{tg}:{plan[3]:x}:{unit}", "--grant-at", "2:0:3")
            inb, a0, a1 = trunk_call(radio, sdr_bench, cc_mhz, cc, in_mhz, True)
            voice, b0, b1 = trunk_call(radio, sdr_bench, cc_mhz, cc, voice_mhz, False)
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("p25t.on") != "1", 6.0,
                   sections=("core", "channel", "p25_trunk"), what="the channel back as it was, the follower stopped")

    def grew(v0, v1, key):
        return (as_int(v1.get(key)) or 0) - (as_int(v0.get(key)) or 0)

    kinds = inbound_blocks(inb)
    print(f"\nunit {unit}, talk group {tg}; inbound: "
          + (", ".join(f"{k} x{len(m)}" for k, m in kinds.items()) or "nothing"))
    for name, (v0, v1) in (("first", (a0, a1)), ("second", (b0, b1))):
        print(f"{name} call: requests {grew(v0, v1, 'p25t.req_tx')}, grants {grew(v0, v1, 'p25t.req_grant')}, calls "
              f"{grew(v0, v1, 'p25.tx_calls')}, talked on {v1.get('p25t.talk_hz')}")
        assert grew(v0, v1, "p25t.req_tx") >= 1, f"{name} call: no voice request sent"
        assert grew(v0, v1, "p25t.req_grant") == 1, f"{name} call: grants taken {grew(v0, v1, 'p25t.req_grant')}"
        assert grew(v0, v1, "p25.tx_calls") == 1, f"{name} call: the radio counted {grew(v0, v1, 'p25.tx_calls')}"
        assert v1.get("p25t.talk_hz") == str(voice_in_hz), f"{name} call: talked on {v1.get('p25t.talk_hz')}"
    reg, aff, req = (kinds.get(k, []) for k in ("u_reg_req", "grp_aff_req", "grp_v_req"))
    assert reg and all(m["src"] == unit for m in reg), f"registration requests {reg}"
    assert reg[0]["wacn"] == int(SITE_WACN, 16) and reg[0]["sysid"] == int(SITE_SYSID, 16), reg[0]
    assert aff and all(m["src"] == unit and m["tg"] == tg for m in aff), f"affiliation requests {aff}"
    assert req and all(m["src"] == unit and m["tg"] == tg for m in req), f"voice requests {req}"
    units = [d for d in p25_decode(voice) if d["ev"] == "unit"]
    names = [u["name"] for u in units]
    print(f"voice channel: {' '.join(names)}")
    assert names and names[0] == "HDU" and names[-1] in ("TDU", "TDULC"), f"the call: {names}"
    lcs = [u.get("lc") or {} for u in units if u["name"] == "LDU1"]
    assert lcs, "no LDU1 on the voice channel"
    for lc in lcs:
        assert lc.get("src") == unit and lc.get("tgid") == tg, f"LDU1 link control {lc}"


# --- A trunked call received (p25-trunking.md 4): a control channel and a voice channel, two bench transmitters ---

P25T_CONTROL, P25T_VOICE = "2", "3"     # p25t.state: on the control channel, on a voice channel


def start_together(*starts):
    """Each start (a bench board's tx_start: it returns once its board keyed, seconds after the request) in a thread
    of its own: [(what it returned, when)], in order. An exception of one (a skip too) is raised here"""
    import threading
    out = [None] * len(starts)

    def run(i, start):
        try:
            out[i] = (start(), time.monotonic(), None)
        except BaseException as e:      # a skip is one
            out[i] = (None, time.monotonic(), e)

    threads = [threading.Thread(target=run, args=(i, s), daemon=True) for i, s in enumerate(starts)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for _, _, e in out:
        if e is not None:
            raise e
    return [(r, at) for r, at, _ in out]


@pytest.mark.feature("p25_trunk")
@pytest.mark.rf
@pytest.mark.codeplug_write
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_p25_trunk_rx(radio, sdr_bench, sdr_bench_2, backup_dir, pytestconfig):
    import p25tool
    cc_mhz, voice_mhz = rig(pytestconfig, "p25-cc-mhz"), rig(pytestconfig, "p25-voice-out-mhz")
    if "P25_TRUNK" not in radio.features():
        skip("not_built", "built without CONFIG_P25_TRUNK")
    p25_ready(radio)
    from test_cps import codeplug_edited
    cc_hz, voice_hz = round(cc_mhz * 1e6), round(voice_mhz * 1e6)
    plan = site_plan(cc_hz, cc_hz, voice_hz)        # no offset: nothing is sent inbound
    number, regions = trunk_slot(radio, backup_dir)
    with codeplug_edited(radio, backup_dir, lambda img: make_p25_trunk(img, number, plan, cc_hz, cc_hz, tx=False),
                         "p25trx", regions):
        v = radio.wait_for(lambda x: x.get("ch.kind") == str(KIND_P25_TRUNK) and x.get("p25t.on") == "1", 6.0,
                           sections=("core", "channel", "p25", "p25_trunk"), what="the trunked channel running")
        if v.get("mode") != "p25":
            skip("no_hw", "the channel is parked: the board's air interface can't carry P25")
        assert v.get("p25t.tx") == "0", "the follower's transmit side is on, on a listen-only system"
        assert v.get("p25t.cc_hz") == str(cc_hz), f"the follower's control channel {v.get('p25t.cc_hz')}"
        tg = as_int(v.get("p25.run.tx_dst")) or 0
        if not tg:
            skip("state", "the channel has no talk group to grant")
        with tempfile.TemporaryDirectory() as tmp, sdr_bench.lease(), sdr_bench_2.lease():
            # the grant in every third unit: the broadcasts keep their blocks (a unit holds three)
            cc = cc_file(tmp, plan, "--grant", f"{tg}:{plan[3]:x}:{RX_SRC}", "--grant-at", "2:0:3")
            path, exp = os.path.join(tmp, "call.dibits"), os.path.join(tmp, "call.json")
            p25tool.run(["gen", path, exp, "--nac", SITE_NAC, "--tg", tg, "--src", RX_SRC, "--pairs", RX_PAIRS,
                         "--lead", 96, "--tail", 48])
            with open(exp) as f:
                n = json.load(f)["total_syms"]
            assert n <= FSK4_HEX_MAX, f"the call holds {n} symbols, past {FSK4_HEX_MAX}"
            v0 = radio.view(["core", "p25", "p25_trunk"])
            (tx, cc_at), (vt, voice_at) = start_together(
                lambda: play_cc(sdr_bench, cc_mhz, cc[0], cc[1], PLAY_S),
                lambda: play_cc(sdr_bench_2, voice_mhz, path, n, PLAY_S))
            held, tuned, back = False, set(), None
            try:
                while time.monotonic() < cc_at + PLAY_S - 0.3:
                    x = radio.view(["core", "p25", "p25_trunk"])
                    st = x.get("p25t.state")
                    if st == P25T_VOICE:
                        tuned.add(x.get("p25t.voice_hz"))
                    elif st == P25T_CONTROL:
                        held = True
                        if tuned:
                            back = x        # on the control channel again after a call
                    time.sleep(0.05)
            finally:
                tx.wait(PLAY_S + 20)
                vt.wait(PLAY_S + 20)
            v1 = radio.wait_for(lambda x: x.get("p25.rx") != "1", 3.0, sections=("core", "p25", "p25_trunk"),
                                what="the reception ended")
    radio.wait_for(lambda x: x.get("ch.kind") == "0" and x.get("p25t.on") != "1", 6.0,
                   sections=("core", "channel", "p25_trunk"), what="the channel back as it was, the follower stopped")

    def grew(key):
        return (as_int(v1.get(key)) or 0) - (as_int(v0.get(key)) or 0)

    print(f"\ntalk group {tg}; the voice board keyed {voice_at - cc_at:+.1f} s against the control channel's; "
          f"blocks {grew('p25t.blocks')} ({grew('p25t.crc_fail')} CRC failures), grants {grew('p25t.grants')}, "
          f"follows {grew('p25t.follows')} to {sorted(tuned) or 'nowhere'}, voice lost {grew('p25t.voice_lost')}; "
          f"calls {grew('p25.rx_calls')}, voice frames {grew('p25.voice')} ({grew('p25.voice_bad')} bad), played "
          f"{grew('p25v.frames') if 'p25v.frames' in v1 else 'n/a'}, from {v1.get('p25.src')} to {v1.get('p25.dst')}")
    assert held, "the control channel never held: " + ", ".join(f"{k[5:]} {v1.get(k)}" for k in TRUNK_KEYS)
    assert grew("p25t.follows") >= 1, f"no grant followed ({grew('p25t.grants')} heard)"
    assert tuned == {str(voice_hz)}, f"followed to {sorted(tuned)}, the voice channel is {voice_hz} Hz"
    assert grew("p25.rx_calls") >= 1, "no reception counted on the voice channel"
    assert grew("p25.voice") >= 18, f"{grew('p25.voice')} voice frames: less than one LDU pair's"
    assert v1.get("p25.src") == str(RX_SRC) and v1.get("p25.dst") == str(tg), \
        f"the call {v1.get('p25.src')} -> {v1.get('p25.dst')}, sent {RX_SRC} -> {tg}"
    assert back is not None, "never back on the control channel after a call while it still played"
    assert grew("p25.tx_calls") == 0 and grew("p25t.bursts") == 0, "the radio sent something"
    if "p25v.frames" in v1 and v1.get("p25v.built") == "1":
        assert grew("p25v.frames") > 0, "no voice frame played"
