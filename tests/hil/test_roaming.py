# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""DMR repeater roaming (docs/spec/dmr-roaming.md) on a CONFIG_DMR_ROAMING build. Everything is read from the view's
`roam` section (debuglink.md 3.7) and driven by RADIO op 26 (roam) and the roaming settings 21-24 (SETTING op 24,
RAM only). The roaming zone flag is stored by the radio: each test puts back the zone flag it found, and the
restore_radio fixture puts back the channel.

The radio-only tests never transmit: roaming starts passive (set.roam_probe=passive), a search only listens. The
bench test uses the wake probe (the radio transmits BS_Dwn_Act on each site it visits) and the SDR bench's modem as
a second site (tools/bench/bench_repeater.py, which keys only when woken).

Pass criteria:
- test_roam_section: roam.built=1 and every key present; state one of off / watch / search / locked; the settings in
  range (threshold -125..-60, hysteresis 1..30, probe passive / wake, check 0 or 30..3600).
- test_roam_settings_round_trip: SETTING roam_rssi_dbm -100 (-101 if it was -100) and roam_hyst_db 9 (10) show in
  set.* within 1 s; the old values again within 1 s after they are put back.
- test_roam_zone_toggle: zone_toggle flips roam.zone_on within 1 s; a second one flips it back. When the zone is
  on, roam.state is watch with roam.sites >= 2 on a repeater channel that has a second site with the same talkgroup
  in the zone, else off with roam.sites=0.
- test_roam_now_passive: (the current zone holds 2+ sites: repeater channels with the current channel's talkgroup)
  roaming on, roam now: roam.searches +1 within 5 s (the quiet time is 3 s), roam.state back to watch within
  sites x 2 s + 5 s; the radio is then on one of the sites, roam.probes unchanged (nothing sent), roam.aborts
  unchanged. Then lock: roam.state=locked; roam now ends the lock (watch or search).
- test_roam_to_bench_repeater (rf, radio_tx, bench_service): the zone holds a site whose frequencies are the bench
  repeater's (--roam-rx / --roam-tx, color code --dmr-cc) and another site with no repeater on the air. Starting on
  the other site with the wake probe on, roam now: within 40 s the radio is on the bench site (roam.roams +1,
  roam.probes >= 1) and bench_repeater.py reported a wakeup.
"""
import json
import time

import pytest

from odr_debug import as_int

ROAM_KEYS = ("roam.built", "roam.zone_on", "roam.state", "roam.why", "roam.pending", "roam.blocked", "roam.sites",
             "roam.site", "roam.home", "roam.channel", "roam.rssi_dbm", "roam.weak", "roam.searches", "roam.roams",
             "roam.stays", "roam.aborts", "roam.probes", "roam.triggers", "set.roam_rssi_dbm", "set.roam_hyst_db",
             "set.roam_probe", "set.roam_check_s")


def roam(radio):
    return radio.link.view(["roam"])


def wait_roam(radio, pred, timeout=3.0, what="roam"):
    return radio.wait_for(pred, timeout, sections=("roam", "core"), what=what)


@pytest.fixture
def roaming(radio):
    """The radio with roaming built in; the zone flag, the probe mode and the radio's DMR channel put back after."""
    if "DMR_ROAMING" not in radio.features():
        pytest.skip("built without roaming (CONFIG_DMR_ROAMING=n)")
    v = roam(radio)
    zone = radio.core().get("zone_index")
    was_on = v.get("roam.zone_on") == "1"
    probe = 1 if v.get("set.roam_probe") == "wake" else 0
    try:
        yield radio
    finally:
        L = radio.link
        L.roam("unlock")
        L.setting("roam_probe", probe)
        if radio.core().get("zone_index") == zone and (roam(radio).get("roam.zone_on") == "1") != was_on:
            L.roam("zone_on" if was_on else "zone_off")


def zone_on(radio):
    if roam(radio).get("roam.zone_on") != "1":
        radio.link.roam("zone_on")
    return wait_roam(radio, lambda v: v.get("roam.zone_on") == "1", 1.0, "roaming on for the zone")


def test_roam_section(roaming):
    v = roam(roaming)
    missing = [k for k in ROAM_KEYS if k not in v]
    assert not missing, f"missing keys: {missing}"
    assert v["roam.built"] == "1"
    assert v["roam.state"] in ("off", "watch", "search", "locked"), v["roam.state"]
    assert -125 <= as_int(v["set.roam_rssi_dbm"]) <= -60
    assert 1 <= as_int(v["set.roam_hyst_db"]) <= 30
    assert v["set.roam_probe"] in ("passive", "wake")
    check = as_int(v["set.roam_check_s"])
    assert check == 0 or 30 <= check <= 3600
    print(f"roam: zone_on={v['roam.zone_on']} state={v['roam.state']} sites={v['roam.sites']} "
          f"threshold={v['set.roam_rssi_dbm']} dBm hysteresis={v['set.roam_hyst_db']} dB")


def test_roam_settings_round_trip(roaming):
    L = roaming.link
    v = roam(roaming)
    old_t, old_h = as_int(v["set.roam_rssi_dbm"]), as_int(v["set.roam_hyst_db"])
    new_t = -101 if old_t == -100 else -100
    new_h = 10 if old_h == 9 else 9
    try:
        L.setting("roam_rssi_dbm", new_t)
        L.setting("roam_hyst_db", new_h)
        wait_roam(roaming, lambda x: as_int(x.get("set.roam_rssi_dbm", "0")) == new_t and
                  as_int(x.get("set.roam_hyst_db", "0")) == new_h, 1.0, "roaming settings set")
    finally:
        L.setting("roam_rssi_dbm", old_t)
        L.setting("roam_hyst_db", old_h)
    wait_roam(roaming, lambda x: as_int(x.get("set.roam_rssi_dbm", "0")) == old_t and
              as_int(x.get("set.roam_hyst_db", "0")) == old_h, 1.0, "roaming settings back")


def test_roam_zone_toggle(roaming):
    if roaming.core().get("vfo") == "1":
        pytest.skip("the radio is in the VFO: roaming works on channels")
    L = roaming.link
    before = roam(roaming).get("roam.zone_on")
    L.roam("zone_toggle")
    v = wait_roam(roaming, lambda x: x.get("roam.zone_on") != before, 1.0, "zone flag flipped")
    if v["roam.zone_on"] == "1":
        if v["roam.state"] == "off":
            assert v["roam.sites"] == "0"
            print(f"zone on, but this channel ({v.get('channel_number')}) is not a site with a second site")
        else:
            assert v["roam.state"] in ("watch", "search") and as_int(v["roam.sites"]) >= 2, v
    else:
        assert v["roam.state"] == "off"
    L.roam("zone_toggle")
    wait_roam(roaming, lambda x: x.get("roam.zone_on") == before, 1.0, "zone flag back")


def test_roam_now_passive(roaming):
    if roaming.core().get("vfo") == "1":
        pytest.skip("the radio is in the VFO: roaming works on channels")
    L = roaming.link
    L.setting("roam_probe", 0)
    v = zone_on(roaming)
    if v["roam.state"] == "off":
        pytest.skip("the current channel is not a repeater site with a second site (same talkgroup) in its zone")
    sites = as_int(v["roam.sites"])
    numbers = {roam(roaming)[f"site.{i}"].split(":")[0] for i in range(sites)}
    s0, p0, a0 = as_int(v["roam.searches"]), as_int(v["roam.probes"]), as_int(v["roam.aborts"])
    L.roam("now")
    wait_roam(roaming, lambda x: as_int(x.get("roam.searches", "0")) > s0, 5.0, "search started")
    v = wait_roam(roaming, lambda x: x.get("roam.state") == "watch", sites * 2.0 + 5.0, "search done")
    assert v.get("channel_number") in numbers, (v.get("channel_number"), numbers)
    assert as_int(v["roam.probes"]) == p0, "a passive search sent a probe"
    assert as_int(v["roam.aborts"]) == a0
    print(f"roam now: on site {v['roam.site']} (channel {v['channel_number']}), why={v['roam.why']}, "
          f"roams={v['roam.roams']} stays={v['roam.stays']}; sites: "
          + " ".join(f"{k}={v[k]}" for k in sorted(v) if k.startswith("site.")))
    L.roam("lock")
    wait_roam(roaming, lambda x: x.get("roam.state") == "locked", 1.0, "locked")
    L.roam("now")
    wait_roam(roaming, lambda x: x.get("roam.state") in ("watch", "search"), 1.0, "roam now ends the lock")


@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
def test_roam_to_bench_repeater(roaming, sdr_bench, pytestconfig):
    rx, tx = pytestconfig.getoption("--roam-rx"), pytestconfig.getoption("--roam-tx")
    if not rx or not tx:
        pytest.skip("give the bench repeater's pair as the radio sees it: --roam-rx MHz (radio receives, bench "
                    "transmits) --roam-tx MHz (radio transmits, bench receives); a pair you are licensed for")
    cc = pytestconfig.getoption("--dmr-cc")
    L = roaming.link
    pos_b = roaming.find_channel(round(rx * 1e6), "dmr")
    v = zone_on(roaming)
    if v["roam.state"] == "off":
        pytest.skip("the bench repeater's channel is not a site with a second site in its zone")
    bench_ch = v["channel_number"]
    others = [roam(roaming)[f"site.{i}"].split(":")[0] for i in range(as_int(v["roam.sites"]))]
    others = [n for n in others if n != bench_ch]
    # Start on another site (silent: no repeater on its frequency)
    size = as_int(roaming.core().get("zone_size") or "0")
    for pos in range(size):
        if pos == pos_b:
            continue
        L.zone_pos(pos)
        c = wait_roam(roaming, lambda x: x.get("zone_pos") == str(pos), 2.0, f"position {pos}")
        if c.get("channel_number") in others and roam(roaming).get("roam.state") == "watch":
            break
    else:
        pytest.skip("no other site to start from")
    roaming.lowest_power()
    L.setting("roam_probe", 1)
    v = roam(roaming)
    r0, p0 = as_int(v["roam.roams"]), as_int(v["roam.probes"])
    with sdr_bench.lease():
        p = sdr_bench.ssh_bg(f"cd ~/{sdr_bench.dir} && {sdr_bench.py} bench_repeater.py --rx {tx} --tx {rx} "
                             f"--cc {cc} --power {sdr_bench.tx_power} --seconds 60 --json")
        time.sleep(3)
        L.roam("now")
        v = wait_roam(roaming, lambda x: x.get("channel_number") == bench_ch and x.get("roam.state") == "watch",
                      40.0, "roamed to the bench repeater")
        assert as_int(v["roam.roams"]) == r0 + 1
        assert as_int(v["roam.probes"]) > p0
        p.terminate()
        out, _ = p.communicate(timeout=10)
    events = []
    for line in (out or "").splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    assert any("wakeup" in e for e in events), \
        f"bench_repeater.py saw no wakeup: {out[-800:] if out else ''}"
