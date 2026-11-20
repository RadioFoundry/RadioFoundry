# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The radio as a DMR Tier III mobile station (ETSI TS 102 361-4; docs/spec/dmr-trunking.md) on a real trunked site
on the air, on a CONFIG_DMR_TRUNK build. The system is set up over the debug link (DL_CMD_T3: reset, system, site,
control channel, talkgroup, start), RAM only; every test stops it and resets the staged system after, and the
restore_radio fixture puts the channel and the power back. Everything is read from the view's `t3` section.

The site is the rig's, always on the air: nothing here transmits for it. The rig gives it with --t3-rx-mhz (its
control channel as the radio receives it), --t3-tx-mhz (where the radio transmits to it), --t3-lcn (the control
channel's logical channel number), --t3-system "MODEL,NET,SITE" (model 0 tiny .. 3 huge), --t3-cc and --t3-tg, the
talkgroup the radio registers with and calls; --t3-peer is a second radio ID the individual call goes to (the site
must grant calls to it). The tests that transmit key at the radio's lowest power. A granted call is sent on the
payload channel's inbound frequency: when the site's payload channels aren't on the control channel's carrier, give
theirs with --t3-payload-tx-mhz (comma separated), as the firmware's test TX limit allows only the run's transmit
frequencies. Each must be in tx_mhz. A run that transmits holds what the rig asks for the site's frequencies and the
radio (locks, leases); the tests don't take any.

Pass criteria:
- test_t3_view: t3.built=1 and every key of T3_KEYS present; t3.on=0 and t3.state=off before a start.
- test_t3_camp (receive only: the system is listen-only and the radio's TX stays blocked): within CAMP_S the radio
  camps (t3.state camped, t3.confirmed=1: an Aloha heard) on --t3-rx-mhz with the system code of --t3-system and PAR
  A+B, color code --t3-cc, both slots (t3.slot 0); t3.reg listen. Within LEARN_S more it learns a channel from the
  site's Chan_Freq broadcasts (t3.learned >= 1). Over that time CSBKs keep coming (t3.csbk grows), none is from
  another system (t3.foreign 0), at most 1 % fail their CRC, and the radio sent nothing (t3.sent 0). After the stop:
  t3.on=0, t3.state off.
- test_t3_register[syscode|talkgroup] (radio_tx): registration open, the radio registers by random access
  within REG_S: t3.reg ok, t3.ax_svc register with t3.ax_result done, t3.sent >= 1. With talkgroup the request names
  --t3-tg (TRGT_ADR_CNTS 01b); the site may answer Reg_Accepted or Reg_Subscription (t3.ax_reason 0x62 or 0x65).
  t3 dereg then de-registers within DEREG_S (t3.ax_svc deregister, t3.ax_result done).
- test_t3_group_call (radio_tx): registered, PTT held: a group call request to --t3-tg (t3.ptt_wait 1), granted
  within GRANT_S: t3.state follow, t3.call_ours 1, t3.call_group 1, t3.call_dst the talkgroup, t3.slot 1 or 2,
  t3.can_talk 1, and the radio transmits (core state tx) for TALK_S. After the release the call ends (the radio
  sends P_MAINT, the site clears it) and the radio is back on the control channel within CLEAR_S: t3.state camped,
  t3.rx_hz --t3-rx-mhz, t3.returns +1, t3.reg still ok. The site's own log says whether it heard the voice; the
  radio's view only shows it was sent.
- test_t3_private_call (radio_tx; needs --t3-peer): as the group call, to --t3-peer as an individual call
  (t3.ax_svc individual): granted within PRIVATE_S, t3.call_group 0, t3.call_dst the peer; back on the control
  channel within CLEAR_S after the release.
- test_t3_broadcast[talkgroup|allcall] (radio_tx; talkgroup needs --t3-broadcast-tg): as the group call, to
  --t3-broadcast-tg set as a broadcast talkgroup, or to all units (ALLMSID): granted within GRANT_S, then back on the
  control channel within BCAST_CLEAR_S of the release, as a broadcast has no hang time (6.6.2.1).
- test_t3_status[group|peer|poll] (radio_tx; peer and poll need --t3-peer): registered, the radio sends status
  STATUS_VALUE to --t3-tg or to --t3-peer, or polls the peer. Within SERVICE_S: t3.ax_svc status, t3.ax_result done,
  t3.ax_reason 0x60 Message_Accepted (talkgroup), 0x44 MS_Accepted (peer) or 0x47 (poll; t3_svc t3.ax_info is the
  peer's status, 0-126). The radio is back to camped.
- test_t3_sdata[group|peer] (radio_tx; peer needs --t3-peer): registered, the radio sends SDATA_TEXT as ISO
  8-bit short data (two UDT blocks) to --t3-tg or --t3-peer: within SERVICE_S t3.ax_svc sdata, t3.ax_result done,
  t3.ax_reason 0x60 or 0x44. Whether the octets arrived whole is in the site's log; the radio's view shows only that
  the site took them (it acknowledges after the UDT's CRC passed).
- test_t3_pdata[unconfirmed|confirmed] (radio_tx; needs --t3-peer and the DMR data service): registered, the
  data service sends PDATA_TEXT as an IP text message to --t3-peer. Within PDATA_S the engine asks for a data channel
  (t3.ax_svc pdata seen), the message is sent (data.sms_state ok; confirmed: the peer acknowledged its blocks) and the radio
  is back on the control channel within CLEAR_S.
- test_t3_reg_list (radio_tx; needs --t3-tg-list): registration by talkgroup with --t3-tg and --t3-tg-list
  (two to seven in all) listened to: within REG_S t3.reg ok, t3.ax_reason 0x62 or 0x65, t3_svc t3.reg_tgs the
  number sent, and t3.reg_tg_ok has no bit set past them. Which the site accepted is printed; the site's own
  configuration decides it.

- test_t3_follow_speech (radio_tx; needs --t3-call-cmd): registered, the rig's command has another radio on the
  site call --t3-tg with speech. Within GRANT_S + CLEAR_S the radio follows it (t3.state follow, t3.call_ours 0,
  t3.call_dst the talkgroup) and is back on the control channel within CALL_S after. The voice: no late entry
  (call.rx_late_entries 0), no frame lost (call.rx_lost_frames, voice.rx_lost 0), at least SPEECH_MIN frames in, all
  but PRIME played, no overflow, and the decoded speech peaks at PEAK_MIN or more (voice.rx_peak), as
  test_dmr_voice: silence would pass every counter.
- test_t3_cease (radio_tx; needs --t3-cease-cmd and a board that listens between its bursts): registered, the
  radio talks on --t3-tg with the voice test signal, PTT held (a board that heard nothing between its bursts,
  call.rx_bs_in_tx 0, skips). TALK_S into the over the rig's command has the site
  send a reverse channel cease on the call's other slot (ETSI TS 102 361-4 6.4.15.2). Within CEASE_S the radio stops
  sending (call.rc_ceased +1, call.rc_good >= 2: a cease is acted on only when heard twice) though PTT is still held,
  and it doesn't key again while PTT stays held (CEASE_HOLD_S). After the release it is back on the control channel
  within CALL_S (the site may give the channel to the radio that interrupted first).
- test_t3_cp_channel (radio_tx): the system from the codeplug instead (dmr-trunking-codeplug.md): a system record in
  slot CP_SYS with --t3-system's identity, its control channel and a tag on the channel in use. Selecting the channel
  starts the system (t3.sys CP_SYS + 1): within CAMP_S + REG_S it camps on --t3-rx-mhz and registers (t3.reg ok).
  Another channel of the zone stops it (t3.sys 0, t3.on 0) within DEREG_S. The channel's talkgroup is its contact's,
  so the channel in use must be a DMR channel whose contact the site serves.
- test_t3_cp_unavailable (radio_tx): the same system marked as needing its key, with no key: the channel is
  unavailable (t3.unavail 1, t3.on 0) and PTT is refused: nothing is sent (t3.sent 0) and the radio never transmits
  (core state never tx).
- test_t3_other_system (radio_tx): the rig's site staged with another network number (the next one the model has):
  within CAMP_S the radio, still hunting (t3.state hunt), names the site's system code as the other system heard
  (t3_roam t3.other: the site's code, or without its PAR bits when only its short LC was heard). PTT held for 2 s is
  refused with RADIO_TRUNK_WHY_OTHER_SYS (t3.why 7): the radio never transmits (core state never tx) and sends
  nothing (t3.sent unchanged).

The cp tests write codeplug records over the debug link, so they also need --allow-codeplug-write. They leave the
codeplug as they found it: they refuse a slot, channel record or tag already there, and remove what they wrote after. On a failure every test prints the last t3 view.

    python3 -m pytest tests/hil/test_t3_site.py -v -rs -s --allow-rf --t3-rx-mhz RX --t3-tx-mhz TX --t3-lcn LCN \\
        --t3-system MODEL,NET,SITE --t3-cc CC --t3-tg TG [--t3-peer ID] [--t3-payload-tx-mhz TX,...]
"""
import subprocess
import time

import pytest

from rfoundry_debug import VIEW_SECTIONS, as_int, DebugLink, CPKV_SYSTEM, CPKV_SYS_CHAN, CPKV_CH_TAG
from rfoundry_hil import counter_deltas, skip
from hil_config import rig, tx_allowed

# The debug link's names (docs/spec/debuglink.md, the t3 section); kept here so a rename touches one place
SECTION = "t3"
T3_KEYS = ("t3.built", "t3.tx_built", "t3.on", "t3.state", "t3.reg", "t3.confirmed", "t3.syscode", "t3.own_id",
           "t3.tscc_lcn", "t3.rx_hz", "t3.tx_hz", "t3.slot", "t3.cc", "t3.aloha_mask", "t3.aloha_sf",
           "t3.aloha_nrand", "t3.aloha_backoff", "t3.aloha_reg", "t3.call_dst", "t3.call_src", "t3.call_group",
           "t3.call_ours", "t3.ax_svc", "t3.ax_state", "t3.ax_result", "t3.ax_reason", "t3.ptt_wait", "t3.can_talk",
           "t3.last_rx_op", "t3.last_rx_slot", "t3.last_rx_age_ms", "t3.last_tx_op", "t3.last_tx_age_ms", "t3.ticks",
           "t3.csbk", "t3.crc_bad", "t3.grants", "t3.followed", "t3.unmapped", "t3.last_unmapped", "t3.returns",
           "t3.hunts", "t3.foreign", "t3.learned", "t3.sent", "t3.ra_tries", "t3.send_fail")
# Engine options (core/dmr/trunk/t3_ms.h T3_OPT_*)
SVC_SECTION = "t3_svc"
OPT_LISTEN_ONLY = 0x0001
OPT_PRIVATE = 0x0020
OPT_REG_TG = 0x0040
PAR_AB = 3
# Reason codes (ETSI TS 102 361-4 table 7.75): registration accepted, and accepted with a talkgroup subscription
REG_ACCEPTED = (0x62, 0x65)
# Final acknowledgements of a status or short data (table 7.75): to a talkgroup, to an MS, a poll's answer
MSG_ACCEPTED, MS_ACCEPTED, POLL_ACCEPTED = 0x60, 0x44, 0x47
STATUS_VALUE = 37
SDATA_TEXT = b"rfoundry short data test"   # 24 octets: two blocks
PDATA_TEXT = "rfoundry packet data test"   # an IP text message on a granted data channel

CAMP_S = 15.0       # hunting one control channel, then an Aloha
LEARN_S = 25.0      # Chan_Freq every 10 s on the rig's site; two chances
REG_S = 30.0        # random access with backoff and retries
DEREG_S = 15.0
GRANT_S = 10.0
PRIVATE_S = 30.0    # the site may ask the called radio first (C_AHOY)
TALK_S = 3.0
CLEAR_S = 15.0      # the site's hang time, its P_CLEAR, the retune
PDATA_S = 40.0      # the data channel request, the grant, the data, the answer (confirmed), the clear
SERVICE_S = 20.0    # random access, the site's C_AHOY to the called MS and its answer, retries
BCAST_CLEAR_S = 3.0  # a broadcast has no hang time: the terminator, the site's P_CLEAR, the retune
ALLMSID = 0xFFFFFF  # all-call (TS 102 361-4 A.4)
PTT_MAX_MS = 10000  # the debug link's longest injected hold (INPUT_INJECT_MAX_MS); a repeated press re-arms it
CALL_S = 30.0       # the other radio's call: its request, the speech, the hang time, the clear
SPEECH_MIN = 90     # voice frames: the shortest call the command may make (5 superframes)
PRIME = 3           # AMBE_VOICE_PRIME in core/dmr/ambe/ambeVoice.h
PEAK_MIN = 1000     # as test_dmr_voice: decoded speech peaks at thousands
CEASE_S = 4.0       # the command, the site's RC (every 360 ms), two heard, the over ended after its burst F
CEASE_HOLD_S = 1.5  # PTT held on after the cease: no new over
CP_SYS = 7          # the codeplug system slot the cp tests write (the last: the user's come first)
CP_CHAN = 0xF0      # the system entry they write (CPKV_SYS_CHAN + it)


def site(config, tx=True):
    """The rig's site: dict of rx/tx Hz, lcn, model, net, site, cc, tg (skips opt_in for any missing). tx False: a
    receive-only test, which doesn't need --t3-tx-mhz (tx 0 then)"""
    rx = rig(config, "t3-rx-mhz", "a Tier III site's control channel")
    tx = rig(config, "t3-tx-mhz", "a Tier III site's control channel") if tx else config.getoption("--t3-tx-mhz") or 0
    lcn = rig(config, "t3-lcn", "the control channel's logical channel number")
    sysd = rig(config, "t3-system", "the site's system identity")
    cc = rig(config, "t3-cc", "the site's color code")
    tg = rig(config, "t3-tg", "the talkgroup the radio registers with and calls")
    try:
        model, net, site_no = (int(x, 0) for x in str(sysd).split(","))
    except ValueError:
        raise pytest.UsageError(f"--t3-system {sysd!r}: MODEL,NET,SITE, numbers")
    own = config.getoption("--t3-own-id")
    return {"rx": round(rx * 1e6), "tx": round(tx * 1e6), "lcn": int(lcn), "model": model, "net": net,
            "site": site_no, "cc": int(cc), "tg": int(tg), "own_id": int(own or 0)}


def syscode(s):
    """The C_SYScode the site broadcasts (361-4 7.2.1, as core/dmr/trunk/t3_sys.c t3_syscode_join), PAR A+B"""
    site_bits = 12 - (9, 7, 4, 2)[s["model"] & 3]
    ns = ((s["net"] << site_bits) | (s["site"] & ((1 << site_bits) - 1))) & 0xFFF
    return (s["model"] & 3) << 14 | ns << 2 | PAR_AB


def t3(radio):
    return radio.link.view([SECTION])


def num(v, key):
    n = as_int(v.get(key))
    assert n is not None, f"{key} missing or not a number: {v.get(key)!r}"
    return n


def wait_t3(radio, pred, timeout, what):
    return radio.wait_for(pred, timeout, interval=0.2, sections=(SECTION, "t3_roam", "core"), what=what)


@pytest.fixture
def trunk(radio):
    """The radio with Tier III built in; the system stopped and the staged one reset after"""
    if "DMR_TRUNK" not in radio.features():
        skip("not_built", "built without Tier III trunking (CONFIG_DMR_TRUNK=n)")
    if SECTION not in VIEW_SECTIONS or not hasattr(radio.link, "t3_system"):
        skip("not_built", "tools/rfoundry_debug.py has no t3 view section or commands (older tool)")
    v = t3(radio)
    if v.get("t3.built") != "1":
        skip("not_built", "the firmware has no t3 view section (older firmware)")
    radio.link.t3("stop")
    try:
        yield radio
    finally:
        radio.link.t3("stop")
        radio.link.t3_reset()


@pytest.fixture
def trunk_tx(trunk):
    if trunk.link.view([SECTION]).get("t3.tx_built") != "1":
        skip("not_built", "built without Tier III access (CONFIG_DMR_TRUNK_TX=n)")
    trunk.lowest_power()
    return trunk


def start(radio, s, listen_only=False, reg_tg=False, talk_id=0, talk_private=False, more_tgs=(), broadcast_tg=0):
    """Stage the rig's site and start; returns once the radio is camped on it"""
    L = radio.link
    L.t3("stop")
    L.t3_reset()
    opts = OPT_PRIVATE | (OPT_LISTEN_ONLY if listen_only else 0) | (OPT_REG_TG if reg_tg else 0)
    L.t3_system(own_id=s["own_id"], model=s["model"], net=s["net"], par="a", opts=opts, cc=s["cc"], talk_id=talk_id,
                talk_private=talk_private)
    L.t3_site(s["site"])
    L.t3_ctrl(s["lcn"], s["rx"], s["tx"])
    L.t3_tg(s["tg"], listen=True, talk=True, broadcast=broadcast_tg == s["tg"])
    for tg in more_tgs:
        L.t3_tg(tg, listen=True, talk=False)
    if broadcast_tg and broadcast_tg != s["tg"]:
        L.t3_tg(broadcast_tg, listen=True, talk=False, broadcast=True)
    L.t3("start")
    try:
        return wait_t3(radio, lambda v: v.get("t3.state") == "camped" and v.get("t3.confirmed") == "1", CAMP_S,
                       "camped on the site's control channel")
    except AssertionError:
        print("t3 view:", t3(radio))
        raise


def register(radio, s, reg_tg=False, **kw):
    v = start(radio, s, reg_tg=reg_tg, **kw)
    try:
        return wait_t3(radio, lambda x: x.get("t3.reg") == "ok", REG_S, "registered")
    except AssertionError:
        print("t3 view:", t3(radio))
        raise


def wait_held(radio, pred, timeout, what):
    """wait_t3 with PTT held: the hold re-armed every few seconds, so a long wait (a grant) outlasts PTT_MAX_MS"""
    end = time.monotonic() + timeout
    while True:
        radio.link.ptt(True, PTT_MAX_MS)
        left = end - time.monotonic()
        try:
            return wait_t3(radio, pred, max(0.2, min(left, 5.0)), what)
        except AssertionError:
            if left <= 5.0:
                raise


def back_on_tscc(radio, s, returns0):
    v = wait_t3(radio, lambda x: x.get("t3.state") == "camped" and as_int(x.get("t3.returns"), 0) > returns0,
                CLEAR_S, "back on the control channel")
    assert num(v, "t3.rx_hz") == s["rx"], f"on {v.get('t3.rx_hz')} Hz, not the control channel"
    assert v.get("t3.reg") == "ok", f"registration lost after the call: {v.get('t3.reg')}"
    return v


def call(radio, s, dst, group, grant_s):
    """PTT held through a request, the grant and TALK_S of talking; released; back on the control channel. Returns
    the seconds from the release to the radio back on the control channel"""
    L = radio.link
    v0 = t3(radio)
    returns0 = num(v0, "t3.returns")
    L.ptt(True, PTT_MAX_MS)
    try:
        v = wait_t3(radio, lambda x: x.get("t3.ptt_wait") == "1" or x.get("t3.state") == "follow", 2.0,
                    "the call request")
        assert v.get("t3.ax_svc") == ("group" if group else "individual"), f"request {v.get('t3.ax_svc')}"
        v = wait_held(radio, lambda x: x.get("t3.state") == "follow" and x.get("t3.can_talk") == "1" or
                      x.get("t3.ax_result") in ("refused", "no_answer", "timeout", "not_allowed", "lost"),
                      grant_s, "the grant")
        assert v.get("t3.state") == "follow", (f"no grant: {v.get('t3.ax_result')} reason "
                                               f"{v.get('t3.ax_reason')}")
        assert v.get("t3.call_ours") == "1" and v.get("t3.call_group") == ("1" if group else "0"), \
            f"following another call: ours {v.get('t3.call_ours')} group {v.get('t3.call_group')}"
        assert num(v, "t3.call_dst") == dst, f"call to {v.get('t3.call_dst')}, asked {dst}"
        assert v.get("t3.slot") in ("1", "2"), f"payload slot {v.get('t3.slot')}"
        L.ptt(True, PTT_MAX_MS)     # re-arm the hold: the request and the grant may have used most of it
        wait_t3(radio, lambda x: str(x.get("state", "")).lower() == "tx", 3.0, "the radio transmitting voice")
        time.sleep(TALK_S)
        assert t3(radio).get("t3.state") == "follow", "the call dropped while talking"
    except AssertionError:
        print("t3 view:", t3(radio))
        raise
    finally:
        L.ptt(False)
        released = time.monotonic()
    try:
        back_on_tscc(radio, s, returns0)
    except AssertionError:
        print("t3 view:", t3(radio))
        raise
    return time.monotonic() - released


@pytest.mark.feature("dmr_trunk")
def test_t3_view(trunk):
    v = t3(trunk)
    missing = [k for k in T3_KEYS if k not in v]
    assert not missing, f"t3 keys missing: {missing}"
    assert v["t3.on"] == "0" and v["t3.state"] == "off", f"stopped: on {v['t3.on']} state {v['t3.state']}"


@pytest.mark.feature("dmr_trunk")
def test_t3_camp(trunk, pytestconfig):
    s = site(pytestconfig, tx=False)
    v = start(trunk, s, listen_only=True)
    try:
        assert num(v, "t3.syscode") == syscode(s), f"system code {v['t3.syscode']}, want {syscode(s):#06x}"
        assert num(v, "t3.rx_hz") == s["rx"], f"camped on {v['t3.rx_hz']} Hz"
        assert num(v, "t3.cc") == s["cc"] and v.get("t3.slot") == "0", f"cc {v['t3.cc']} slot {v['t3.slot']}"
        assert v.get("t3.reg") == "listen", f"listen only: reg {v['t3.reg']}"
        csbk0 = num(v, "t3.csbk")
        v = wait_t3(trunk, lambda x: as_int(x.get("t3.learned"), 0) >= 1, LEARN_S, "a channel learned from Chan_Freq")
        time.sleep(1.0)
        v = t3(trunk)
        csbk = num(v, "t3.csbk")
        assert v.get("t3.state") == "camped", f"lost the site: {v['t3.state']}"
        assert csbk > csbk0, "no CSBKs while camped"
        assert num(v, "t3.foreign") == 0, f"{v['t3.foreign']} PDUs from another system"
        assert num(v, "t3.crc_bad") * 100 <= csbk, f"{v['t3.crc_bad']} bad CRCs of {csbk} CSBKs"
        assert num(v, "t3.sent") == 0, "a listen-only radio sent"
        print(f"camped: syscode {v['t3.syscode']}, aloha mask {v['t3.aloha_mask']} nrand {v['t3.aloha_nrand']} "
              f"backoff {v['t3.aloha_backoff']} reg {v['t3.aloha_reg']}, {csbk} CSBKs, learned {v['t3.learned']}")
    except AssertionError:
        print("t3 view:", t3(trunk))
        raise
    trunk.link.t3("stop")
    v = wait_t3(trunk, lambda x: x.get("t3.on") == "0", 2.0, "stopped")
    assert v.get("t3.state") == "off"


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.parametrize("by", ["syscode", "talkgroup"])
def test_t3_register(trunk_tx, pytestconfig, by):
    s = site(pytestconfig)
    v = register(trunk_tx, s, reg_tg=(by == "talkgroup"))
    try:
        assert num(v, "t3.sent") >= 1, "registered without sending"
        assert v.get("t3.ax_svc") == "register" and v.get("t3.ax_result") == "done", \
            f"{v.get('t3.ax_svc')} {v.get('t3.ax_result')}"
        assert num(v, "t3.ax_reason") in REG_ACCEPTED, f"reason {v.get('t3.ax_reason')}"
        print(f"registered by {by}: reason {num(v, 't3.ax_reason'):#04x}, {v['t3.ra_tries']} tries")
        trunk_tx.link.t3("dereg")
        wait_t3(trunk_tx, lambda x: x.get("t3.ax_svc") == "deregister" and x.get("t3.ax_result") == "done",
                DEREG_S, "de-registered")
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_group_call(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    register(trunk_tx, s)
    took = call(trunk_tx, s, s["tg"], True, GRANT_S)
    print(f"back on the control channel {took:.1f} s after the release")


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.parametrize("kind", ["talkgroup", "allcall"])
def test_t3_broadcast(trunk_tx, pytestconfig, kind):
    s = site(pytestconfig)
    if kind == "talkgroup":
        dst = int(rig(pytestconfig, "t3-broadcast-tg", "a talkgroup the site takes broadcast calls to"))
        register(trunk_tx, s, talk_id=dst, broadcast_tg=dst)
    else:
        dst = ALLMSID
        register(trunk_tx, s, talk_id=dst)
    took = call(trunk_tx, s, dst, True, GRANT_S)
    print(f"{kind}: back on the control channel {took:.1f} s after the release")
    assert took <= BCAST_CLEAR_S, f"{took:.1f} s after the release: a hang time on a broadcast"


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_private_call(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    peer = int(rig(pytestconfig, "t3-peer", "a second radio ID the site grants individual calls to"))
    start(trunk_tx, s, talk_id=peer, talk_private=True)
    try:
        wait_t3(trunk_tx, lambda x: x.get("t3.reg") == "ok", REG_S, "registered")
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise
    call(trunk_tx, s, peer, False, PRIVATE_S)


def service(radio, svc, send, reason):
    """Run one status or short data request (send()) and wait for its end; returns the t3 and t3_svc views"""
    send()
    try:
        v = wait_t3(radio, lambda x: x.get("t3.ax_svc") == svc and x.get("t3.ax_state") == "idle" and
                    x.get("t3.ax_result") != "pending", SERVICE_S, f"the {svc} request's answer")
        assert v.get("t3.ax_result") == "done", f"{svc}: {v.get('t3.ax_result')} reason {v.get('t3.ax_reason')}"
        assert num(v, "t3.ax_reason") == reason, f"{svc}: reason {v.get('t3.ax_reason')}, expected {reason:#04x}"
        assert v.get("t3.state") == "camped", f"{svc}: state {v.get('t3.state')} after the request"
    except AssertionError:
        print("t3 view:", t3(radio), radio.link.view([SVC_SECTION]))
        raise
    return v, radio.link.view([SVC_SECTION])


def svc_target(trunk_tx, pytestconfig, to):
    s = site(pytestconfig)
    if not hasattr(trunk_tx.link, "t3_sdata") or SVC_SECTION not in VIEW_SECTIONS:
        skip("not_built", "tools/rfoundry_debug.py has no t3 status or short data (older tool)")
    if to == "group":
        dst = s["tg"]
    else:
        dst = int(rig(pytestconfig, "t3-peer", "a second radio ID the site delivers status and short data to"))
    register(trunk_tx, s)
    return dst


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.parametrize("to", ["group", "peer", "poll"])
def test_t3_status(trunk_tx, pytestconfig, to):
    dst = svc_target(trunk_tx, pytestconfig, "group" if to == "group" else "peer")
    value = 127 if to == "poll" else STATUS_VALUE
    reason = {"group": MSG_ACCEPTED, "peer": MS_ACCEPTED, "poll": POLL_ACCEPTED}[to]
    _, sv = service(trunk_tx, "status", lambda: trunk_tx.link.t3_status(dst, value, to == "group"), reason)
    if to == "poll":
        info = num(sv, "t3.ax_info")
        assert 0 <= info <= 126, f"the poll's answer {info} is not a status"


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.parametrize("to", ["group", "peer"])
def test_t3_sdata(trunk_tx, pytestconfig, to):
    dst = svc_target(trunk_tx, pytestconfig, to)
    service(trunk_tx, "sdata", lambda: trunk_tx.link.t3_sdata(dst, SDATA_TEXT, to == "group", "iso8"),
            MSG_ACCEPTED if to == "group" else MS_ACCEPTED)


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.parametrize("ack", ["unconfirmed", "confirmed"])
def test_t3_pdata(trunk_tx, pytestconfig, ack):
    s = site(pytestconfig)
    peer = rig(pytestconfig, "t3-peer", "an MS the site grants data calls to")
    radio = trunk_tx
    if radio.link.view(["dmr_data"]).get("data.built") != "1":
        skip("not_built", "built without the DMR data service")
    register(radio, s)
    returns0 = num(t3(radio), "t3.returns")
    # The standard's IP text (361-3 5.7.1): confirmed, it is delivered on the data link ACK. A confirmed TMS text would
    # also wait for the far radio's own TMS ACK, which a radio that only speaks the standard doesn't send
    radio.link.sms(peer, PDATA_TEXT, ack=ack == "confirmed", fmt="etsi_ip")
    seen = set()

    def done(v):
        seen.add(v.get("t3.ax_svc"))
        return v.get("data.sms_state") in ("ok", "failed")
    try:
        v = radio.wait_for(done, PDATA_S, interval=0.2, sections=(SECTION, "dmr_data"), what="the text message sent")
        assert "pdata" in seen, f"no packet data channel request (accesses seen: {sorted(map(str, seen))})"
        assert v.get("data.sms_state") == "ok", f"text message {v.get('data.sms_state')}"
        back_on_tscc(radio, s, returns0)
    except AssertionError:
        print("t3 view:", t3(radio))
        print("data view:", radio.link.view(["dmr_data"]))
        raise


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_reg_list(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    raw = rig(pytestconfig, "t3-tg-list", "more talkgroups the site takes in a subscription list")
    try:
        more = [int(x, 0) for x in str(raw).split(",") if x.strip()]
    except ValueError:
        raise pytest.UsageError(f"--t3-tg-list {raw!r}: talkgroups, comma separated")
    if not 1 <= len(more) <= 6:
        raise pytest.UsageError("--t3-tg-list: one to six talkgroups (seven in all with --t3-tg)")
    if SVC_SECTION not in VIEW_SECTIONS:
        skip("not_built", "tools/rfoundry_debug.py has no t3_svc section (older tool)")
    start(trunk_tx, s, reg_tg=True, more_tgs=more)
    try:
        v = wait_t3(trunk_tx, lambda x: x.get("t3.reg") == "ok" or
                    x.get("t3.ax_result") in ("refused", "no_answer", "timeout"), REG_S, "registered")
        assert v.get("t3.reg") == "ok", f"registration: {v.get('t3.ax_result')} reason {v.get('t3.ax_reason')}"
        assert num(v, "t3.ax_reason") in REG_ACCEPTED, f"reason {v.get('t3.ax_reason')}"
        sv = trunk_tx.link.view([SVC_SECTION])
        n = 1 + len(more)
        assert num(sv, "t3.reg_tgs") == n, f"list of {sv.get('t3.reg_tgs')}, {n} listened to"
        mask = num(sv, "t3.reg_tg_ok")
        assert mask & ((1 << (7 - n)) - 1) == 0, f"accepted mask {mask:#04x} has bits past the {n} sent"
        tgs = [s["tg"]] + more
        print("accepted:", [tg for i, tg in enumerate(tgs) if mask & (0x40 >> i)],
              "rejected:", [tg for i, tg in enumerate(tgs) if not mask & (0x40 >> i)])
    except AssertionError:
        print("t3 view:", t3(trunk_tx), trunk_tx.link.view([SVC_SECTION]))
        raise


@pytest.fixture
def cp_system(trunk_tx, pytestconfig):
    """A system in codeplug slot CP_SYS for the rig's site, and a tag making the channel in use one of its channels.
    Yields write(opts, needs_key): (re)writes the system with those options and selects the channel again. Everything written
    is removed after and the channel selected again"""
    radio, L = trunk_tx, trunk_tx.link
    s = site(pytestconfig)
    radio.go_home()
    if radio.core().get("vfo") == "1":
        L.vfo(False)
    size = as_int(radio.core().get("zone_size")) or 0
    if size < 2:
        skip("state", "the zone has one channel: a cp test steps off the trunked channel to stop the system")
    # The zone's first DMR channel: its talkgroup is its contact's
    for pos in range(size):
        L.zone_pos(pos)
        time.sleep(0.3)
        c = as_int(radio.core().get("channel_number"))
        ch = radio.view(["channel"])
        if c and ch.get("ch.mode") == "dmr":
            break
    else:
        skip("state", "no DMR channel in the zone in use: select a zone that has one")
    keys = (CPKV_SYSTEM + CP_SYS, CPKV_SYS_CHAN + CP_CHAN, CPKV_CH_TAG + c - 1)
    for k in keys:
        if L.cpkv_get(k) is not None:
            skip("state", f"codeplug key {k:#06x} is in use: the test won't write over the user's set-up")
    other = (pos + 1) % size

    def select():
        L.zone_pos(other)
        time.sleep(0.5)
        L.zone_pos(pos)

    def write(opts, needs_key=False):
        L.cpkv_set(keys[0], DebugLink.t3_system_record("HIL site", s["model"], s["net"], par="a", cc=s["cc"],
                                                       opts=opts, own_id=s["own_id"], sites=(s["site"],),
                                                       needs_key=needs_key))
        L.cpkv_set(keys[1], DebugLink.t3_chan_record(CP_SYS, s["lcn"], s["rx"], s["tx"], ctrl=True))
        # base_own: the tag goes on the user's channel as it is, so removing the tag leaves it as it was
        L.cpkv_set(keys[2], DebugLink.t3_tag_record(CP_SYS, ch.get("ch.name", ""), base_own=True))
        select()
        return s, other

    try:
        yield write
    finally:
        L.ptt(False)
        for k in keys:
            L.cpkv_remove(k)
        select()


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_t3_cp_channel(trunk_tx, cp_system):
    s, other = cp_system(OPT_PRIVATE)
    try:
        v = wait_t3(trunk_tx, lambda x: x.get("t3.reg") == "ok", CAMP_S + REG_S, "camped and registered")
        assert num(v, "t3.sys") == CP_SYS + 1, f"t3.sys {v.get('t3.sys')}: not the codeplug's system"
        assert v.get("t3.unavail") == "0", "a system with a control channel and no authentication is unavailable"
        assert num(v, "t3.rx_hz") == s["rx"], f"on {v.get('t3.rx_hz')} Hz, not the control channel"
        print(f"camped and registered from the codeplug: reason {num(v, 't3.ax_reason'):#04x}")
        trunk_tx.link.zone_pos(other)
        wait_t3(trunk_tx, lambda x: x.get("t3.sys") == "0" and x.get("t3.on") == "0", DEREG_S,
                "the system stopped on another channel")
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_t3_cp_unavailable(trunk_tx, cp_system):
    cp_system(OPT_PRIVATE, needs_key=True)
    L = trunk_tx.link
    try:
        v = wait_t3(trunk_tx, lambda x: x.get("t3.sys") == str(CP_SYS + 1), 3.0, "the channel's system")
        assert v.get("t3.unavail") == "1", "authenticates without a key, yet available"
        assert v.get("t3.on") == "0", "an unavailable system started"
        sent0 = num(v, "t3.sent")
        mhz = (as_int(trunk_tx.core().get("tx_hz")) or 0) / 1e6
        if not tx_allowed(None, mhz):
            # Were the refusal broken, PTT would key the channel's own frequency: only on one the rig may use
            skip("opt_in", f"PTT refusal not checked: the channel's own TX frequency {mhz:.5f} MHz isn't in tx_mhz")
        L.ptt(True, 3000)
        try:
            end = time.monotonic() + 2.0
            while time.monotonic() < end:
                assert str(trunk_tx.core().get("state", "")).lower() != "tx", "PTT keyed an unavailable channel"
                time.sleep(0.1)
        finally:
            L.ptt(False)
        assert num(t3(trunk_tx), "t3.sent") == sent0, "sent on an unavailable system"
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_other_system(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    if not tx_allowed(None, s["tx"] / 1e6):
        # Were the refusal broken, PTT could send on the site's inbound: only on a frequency the rig may use
        skip("opt_in", f"--t3-tx-mhz {s['tx'] / 1e6:.5f} MHz isn't in tx_mhz")
    nets = (512, 128, 16, 4)[s["model"] & 3]
    L = trunk_tx.link
    L.t3("stop")
    L.t3_reset()
    L.t3_system(own_id=s["own_id"], model=s["model"], net=(s["net"] + 1) % nets, par="a", opts=OPT_PRIVATE,
                cc=s["cc"], talk_id=0, talk_private=False)
    L.t3_ctrl(s["lcn"], s["rx"], s["tx"])
    L.t3_tg(s["tg"], listen=True, talk=True)
    L.t3("start")
    code = syscode(s)
    try:
        v = wait_t3(trunk_tx, lambda x: as_int(x.get("t3.other"), 0) in (code, code & ~3), CAMP_S,
                    "the site named as another system")
        assert v.get("t3.state") == "hunt", f"camped on another network's site: {v['t3.state']}"
        sent0 = num(v, "t3.sent")
        L.ptt(True, 3000)
        try:
            end = time.monotonic() + 2.0
            while time.monotonic() < end:
                assert str(trunk_tx.core().get("state", "")).lower() != "tx", "PTT keyed with no system found"
                time.sleep(0.1)
            v = trunk_tx.link.view([SECTION, "t3_roam"])
        finally:
            L.ptt(False)
        assert num(v, "t3.why") == 7, f"PTT refused for t3.why {v.get('t3.why')}, want 7 (another system heard)"
        assert num(v, "t3.sent") == sent0, "sent while hunting"
        print(f"other system heard: {v['t3.other']} (the site's {code:#06x})")
    except AssertionError:
        print("t3 view:", trunk_tx.link.view([SECTION, "t3_roam"]))
        raise


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_follow_speech(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    cmd = rig(pytestconfig, "t3-call-cmd", "a call with speech from another radio on the site")
    if trunk_tx.view(["dmr"]).get("dmr.codec") != "1":
        skip("not_built", "no vocoder merged in this firmware (dmr.codec=0): voice can't play")
    v0 = register(trunk_tx, s)
    followed0, returns0 = num(v0, "t3.followed"), num(v0, "t3.returns")
    keys = ["voice.rx_in", "voice.rx_frames", "voice.rx_concealed", "voice.rx_silence", "voice.rx_lost",
            "voice.rx_overflows", "voice.rx_bad", "voice.rx_underruns", "call.rx_lost_frames", "call.rx_voice_bursts",
            "call.rx_late_entries", "audio.ov", "audio.dr"]
    trunk_tx.reset_counters()
    _, before = trunk_tx.counters()
    try:
        subprocess.run(cmd, shell=True, check=True, timeout=30)
        v = wait_t3(trunk_tx, lambda x: x.get("t3.state") == "follow" and num(x, "t3.followed") > followed0,
                    GRANT_S + CLEAR_S, "following the other radio's call")
        assert v.get("t3.call_ours") == "0", "following a call of its own"
        assert num(v, "t3.call_dst") == s["tg"], f"following a call to {v.get('t3.call_dst')}, not {s['tg']}"
        wait_t3(trunk_tx, lambda x: x.get("t3.state") == "camped" and num(x, "t3.returns") > returns0, CALL_S,
                "back on the control channel after the call")
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise
    time.sleep(1.0)                      # playback drains
    _, after = trunk_tx.counters()
    d = counter_deltas(before, after, keys)
    peak = trunk_tx.view(["dmr_stats"]).get("voice.rx_peak")
    print(f"followed: radio deltas {d}, peak {peak}")
    assert d["call.rx_late_entries"] == 0, f"the call started late (header missed): {d}"
    assert d["call.rx_lost_frames"] == 0 and d["voice.rx_lost"] == 0, f"frames lost: {d}"
    assert d["voice.rx_in"] >= SPEECH_MIN, f"only {d['voice.rx_in']} voice frames in: {d}"
    assert d["voice.rx_frames"] >= d["voice.rx_in"] - PRIME, f"played {d['voice.rx_frames']} of {d['voice.rx_in']}: {d}"
    assert d["voice.rx_overflows"] == 0 and d["audio.ov"] == 0, f"overflows: {d}"
    assert peak is not None and int(peak) >= PEAK_MIN, f"decoded speech peaks at {peak} (silent output?): {d}"


@pytest.mark.feature("dmr_trunk_tx")
@pytest.mark.radio_tx
def test_t3_cease(trunk_tx, pytestconfig):
    s = site(pytestconfig)
    cmd = rig(pytestconfig, "t3-cease-cmd", "a reverse channel cease from the site while the radio talks")
    L = trunk_tx.link
    v0 = register(trunk_tx, s)
    returns0 = num(v0, "t3.returns")
    _, before = trunk_tx.counters()
    L.voice_test(True)
    L.ptt(True, PTT_MAX_MS)
    try:
        wait_held(trunk_tx, lambda x: x.get("t3.state") == "follow" and x.get("t3.can_talk") == "1", GRANT_S,
                  "the grant")
        L.ptt(True, PTT_MAX_MS)     # re-arm the hold after the grant
        wait_t3(trunk_tx, lambda x: str(x.get("state", "")).lower() == "tx", 3.0, "the radio transmitting voice")
        time.sleep(TALK_S)
        L.ptt(True, PTT_MAX_MS)
        subprocess.run(cmd, shell=True, check=True, timeout=30)
        L.ptt(True, PTT_MAX_MS)     # re-arm: only the cease may end the over, never the hold's time-out
        armed = asked = time.monotonic()
        try:
            wait_t3(trunk_tx, lambda x: str(x.get("state", "")).lower() != "tx", CEASE_S,
                    "the over ended by the cease")
        except AssertionError:
            _, now = trunk_tx.counters()
            if counter_deltas(before, now, ["call.rx_bs_in_tx"])["call.rx_bs_in_tx"] == 0:
                skip("no_hw", "nothing heard between the radio's bursts (call.rx_bs_in_tx 0): the board "
                     "doesn't listen while sending, so it can't hear a reverse channel")
            raise
        took = time.monotonic() - asked
        assert time.monotonic() - armed + CEASE_HOLD_S < PTT_MAX_MS / 1000, \
            "the PTT hold would time out during the check: the cease came too late to tell"
        end = time.monotonic() + CEASE_HOLD_S
        while time.monotonic() < end:
            assert str(trunk_tx.link.view(["core"]).get("state", "")).lower() != "tx", "keyed again with PTT held"
            time.sleep(0.1)
    except AssertionError:
        print("t3 view:", t3(trunk_tx))
        raise
    finally:
        L.ptt(False)
        L.voice_test(False)
    _, after = trunk_tx.counters()
    d = counter_deltas(before, after, ["call.rc_good", "call.rc_bad", "call.rc_ceased"])
    print(f"over ended {took:.2f} s after the command; {d}")
    assert d["call.rc_ceased"] == 1, f"the over didn't end on a cease: {d}"
    assert d["call.rc_good"] >= 2, f"fewer than two RCs heard: {d}"
    wait_t3(trunk_tx, lambda x: x.get("t3.state") == "camped" and num(x, "t3.returns") > returns0, CALL_S,
            "back on the control channel")
