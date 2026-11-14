# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""DMR repeater roaming (docs/spec/dmr-roaming.md) on a CONFIG_DMR_ROAMING build. Everything is read from the view's
`roam` section (debuglink.md 3.7) and driven by RADIO op 26 (roam) and the roaming settings 21-24 (SETTING op 24,
RAM only). The roaming zone flag is stored by the radio: each test puts back the zone flag it found, and the
restore_radio fixture puts back the channel.

The radio-only tests never transmit: roaming starts passive (set.roam_probe=passive), a search only listens. The
bench test uses the wake probe (the radio transmits BS_Dwn_Act on each site it visits) and the SDR bench's modem as
a second site (a bench_repeater job, which keys only when woken). The beacon tests have the bench repeater
key an idle beacon (a bench_repeater job with --beacon) and the radio only listens (the survey, dmr-roaming.md 4.8 - 4.10).
The bench tests use the first duplex DMR channel receiving --roam-rx as the bench site. When the codeplug's sites
transmit where the bench can't serve them, bench_sites() writes the TX frequencies over CPS for the test and puts the
codeplug back after (--allow-codeplug-write): the bench site on --roam-tx, any other site the rig may not transmit on
(tx_mhz) on --roam-rx, where the bench repeater doesn't listen. When no duplex DMR channel receives --roam-rx, the
first site of the codeplug's first roaming zone (duplex DMR channels on one talkgroup, two or more receive frequencies)
is moved onto the bench pair and the bench's color code the same way. test_roam_now_passive looks for a zone with
sites itself when the current channel isn't one; every zone whose roaming flag a test turned on is put back.

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
  the other site with the wake probe on and the threshold at its lowest (-125 dBm: the bench's level is not the test),
  roam now: within 40 s the radio is on the bench site (roam.roams +1, roam.probes >= 1) and the bench_repeater job
  reported a wakeup. On a timeout it prints the radio's visits and the bench repeater's output.
- test_roam_survey_settings_round_trip: SETTING roam_beacon_s / roam_beacon_len_s / roam_survey (25-27) show in
  set.* within 1 s and are put back.
- test_roam_beacon_home (rf, bench_service; the radio does not transmit): the zone holds the bench repeater's site
  (--roam-rx / --roam-tx) and at least one other site with the same talkgroup (no repeater on the air needed). The
  radio sits on the bench site, survey synced, beacon --roam-beacon s / --roam-beacon-len s (both sides). Within 2
  intervals + 10 s roam.beacons >= 1 and roam.win_conf >= 1 (learned from its own site); within 2 more intervals
  roam.surveys +1 (a round in the window), roam.win_conf >= 2 after another interval; roam.searches, roam.roams,
  roam.probes unchanged; the radio is back on the bench site after every round; the bench_repeater job saw no wakeup
  from the radio. Each other site's surv.<i> has an age (measured) and dBm 0 once missed twice.
- test_roam_beacon_other_site (rf, bench_service; slow, about 5 min with a 20 s interval): the radio sits on a
  silent site of the same zone; the bench site beacons. Its own site never beacons, so after 2 intervals + the
  duration one discovery visit listens to another site through an interval; the bench site's surv.<i> ends with a
  dBm value and fresh=1, roam.win_conf >= 1; the radio is on its own site at the end, nothing transmitted.

Setup for the beacon tests: a zone with the bench repeater pair as a DMR repeater channel (TX != RX, the bench's
color code --dmr-cc, contact TG 9 or any talkgroup) and a second DMR repeater channel with the same contact on an
unused pair, roaming allowed for the zone (the tests turn it on and put it back). Example:

    python3 -m pytest tests/hil/test_roaming.py -v -rs -k beacon --allow-rf --allow-service-stop \
        --roam-rx RX_MHZ --roam-tx TX_MHZ --dmr-cc CC [--roam-beacon 20 --roam-beacon-len 3]

(the pair in the rig's tx_mhz too: hil_config.py)
"""
import contextlib
import json
import time

import pytest

from rfoundry_hil import skip
from rfoundry_debug import as_int
import hil_config
from hil_config import rig

ROAM_KEYS = ("roam.built", "roam.zone_on", "roam.state", "roam.why", "roam.pending", "roam.blocked", "roam.sites",
             "roam.site", "roam.home", "roam.channel", "roam.rssi_dbm", "roam.weak", "roam.searches", "roam.roams",
             "roam.stays", "roam.aborts", "roam.probes", "roam.triggers", "set.roam_rssi_dbm", "set.roam_hyst_db",
             "set.roam_probe", "set.roam_check_s", "roam.visit", "roam.win_conf", "roam.win_next_ms", "roam.confirm",
             "roam.surveys", "roam.visits", "roam.cuts", "roam.beacons", "roam.confirms", "set.roam_beacon_s",
             "set.roam_beacon_len_s", "set.roam_survey", "set.rpt_beacon")
ROAM_SETTINGS = ("roam_probe", "roam_beacon_s", "roam_beacon_len_s", "roam_survey", "roam_rssi_dbm")
SURVEYS = {"off": 0, "sync": 1, "site": 2}


def roam(radio):
    return radio.link.view(["roam"])


def wait_roam(radio, pred, timeout=3.0, what="roam"):
    return radio.wait_for(pred, timeout, sections=("roam", "core"), what=what)


@pytest.fixture
def roaming(radio):
    """The radio with roaming built in; the zone flag, the probe mode and the radio's DMR channel put back after."""
    if "DMR_ROAMING" not in radio.features():
        skip("not_built", "built without roaming (CONFIG_DMR_ROAMING=n)")
    v = roam(radio)
    # zone index -> its roaming flag as found, put back after: the starting zone's, and each zone zone_on() turned on
    radio.roam_flags = {radio.core().get("zone_index"): v.get("roam.zone_on") == "1"}
    saved = {"roam_probe": 1 if v.get("set.roam_probe") == "wake" else 0,
             "roam_beacon_s": as_int(v.get("set.roam_beacon_s", "60")),
             "roam_beacon_len_s": as_int(v.get("set.roam_beacon_len_s", "4")),
             "roam_survey": SURVEYS.get(v.get("set.roam_survey"), 1),
             "roam_rssi_dbm": as_int(v.get("set.roam_rssi_dbm", "-105"))}
    try:
        yield radio
    finally:
        L = radio.link
        L.roam("unlock")
        for k in ROAM_SETTINGS:
            L.setting(k, saved[k])
        for zone, was_on in radio.roam_flags.items():
            if radio.core().get("zone_index") != zone:
                L.zone(int(zone))
                radio.wait_for(lambda x: x.get("zone_index") == zone, 3.0, what=f"zone {zone}")
            if (roam(radio).get("roam.zone_on") == "1") != was_on:
                L.roam("zone_on" if was_on else "zone_off")
        del radio.roam_flags


def _site_zone(img):
    """(zone name, its sites) of the codeplug's first zone with duplex DMR channels on one talkgroup on two or more
    receive frequencies (sites roaming moves between, dmr-roaming.md 2; one pair's two timeslots never retune), or
    None"""
    import cps_client
    for _, name, members in cps_client.zones(img):
        by_tg = {}
        for n in members:
            ch = cps_client.decode_channel(img, n)
            dmr, _, contact = cps_client.channel_dmr(img, n)
            if ch and dmr and contact and ch["rx_hz"] != ch["tx_hz"]:
                by_tg.setdefault(contact, []).append(n)
        for sites in by_tg.values():
            if len({cps_client.decode_channel(img, n)["rx_hz"] for n in sites}) >= 2:
                return name, sites
    return None


@contextlib.contextmanager
def _bench_site_written(radio, backup_dir, pytestconfig, rx, tx):
    """No duplex DMR channel receives rx: the first site of the codeplug's first roaming zone becomes the bench site
    (RX rx, TX tx, the bench's color code), and the zone's other sites that transmit where the rig may not transmit on
    rx. Every site's power is the bench level (site_power). Written over CPS for the test and put back after."""
    if not pytestconfig.getoption("--allow-codeplug-write"):
        skip("opt_in", f"no duplex DMR channel receives {rx} MHz: run with --allow-codeplug-write to make a roaming "
                       "zone's site the bench site (written for the test, put back after)")
    import cps_client
    from test_cps import codeplug_edited
    cc = rig(pytestconfig, "dmr-cc")
    level, count = site_power(radio)

    def edit(img):
        z = _site_zone(img)
        if z is None:
            skip("state", "no zone with duplex DMR channels on one talkgroup on two receive frequencies (roaming's "
                          "sites)")
        name, sites = z
        print(f"bench site: channel {sites[0]} of zone {name!r} on {rx}/{tx} MHz, color code {cc}")
        cps_client.set_channel_rx(img, sites[0], round(rx * 1e6))
        cps_client.set_channel_tx(img, sites[0], round(tx * 1e6))
        cps_client.set_channel_cc(img, sites[0], cc)
        for n in sites[1:]:
            if not hil_config.tx_allowed(pytestconfig, cps_client.decode_channel(img, n)["tx_hz"] / 1e6):
                cps_client.set_channel_tx(img, n, round(rx * 1e6))
        for n in sites:
            cps_client.set_channel_power(img, n, level, count)
    with codeplug_edited(radio, backup_dir, edit, "roam"):
        radio.find_channel(round(rx * 1e6), "dmr", duplex=True)
        radio.wait_for(lambda x: x.get("tx_hz") == str(round(tx * 1e6)), timeout=5, what="the bench site's TX")
        yield


@contextlib.contextmanager
def bench_sites(radio, backup_dir, pytestconfig, rx, tx):
    """The roaming zone's sites on frequencies this bench serves, for the test. The bench site (the first duplex DMR
    channel receiving rx) transmits on tx, and every other site whose TX the rig may not transmit on (tx_mhz) transmits
    on rx instead, so a wake probe there stays on an allowed frequency where the bench repeater doesn't listen; every
    site's power is the bench level (site_power). Written over CPS and put back afterwards (needs
    --allow-codeplug-write); nothing is written when the codeplug has them so already. With no duplex DMR channel on rx, a roaming zone's first site is moved there (_bench_site_written)."""
    try:
        radio.find_channel(round(rx * 1e6), "dmr", duplex=True)
    except AssertionError:
        with _bench_site_written(radio, backup_dir, pytestconfig, rx, tx):
            yield
        return
    v = zone_on(radio)
    if v["roam.state"] == "off":
        skip("state", "the bench repeater's channel is not a site with a second site (same talkgroup) in its zone")
    bench_ch = v["channel_number"]
    numbers = {v[f"site.{i}"].split(":")[0] for i in range(as_int(v["roam.sites"]))}
    level, count = site_power(radio)
    want = {}           # channel number -> the TX it needs
    power = set()       # the sites whose power isn't the bench level
    size = as_int(radio.core().get("zone_size") or "0")
    for pos in range(size):
        radio.link.zone_pos(pos)
        c = wait_roam(radio, lambda x: x.get("zone_pos") == str(pos), 2.0, f"position {pos}")
        n, cur = c.get("channel_number"), as_int(c.get("tx_hz") or "0")
        if n == bench_ch and cur != round(tx * 1e6):
            want[int(n)] = round(tx * 1e6)
        elif n in numbers and n != bench_ch and not hil_config.tx_allowed(pytestconfig, cur / 1e6):
            want[int(n)] = round(rx * 1e6)
        if n in numbers and as_int(c.get("power") or "0") != level:
            power.add(int(n))
    if not want and not power:
        yield
        return
    if not pytestconfig.getoption("--allow-codeplug-write"):
        skip("opt_in", f"the zone's sites need TX frequencies the bench serves ({want}) and power level {level} "
                       f"(channels {sorted(power)}): run with --allow-codeplug-write (written for the test, put back "
                       "after)")
    import cps_client
    from test_cps import codeplug_edited

    def edit(img):
        for n, hz in want.items():
            cps_client.set_channel_tx(img, n, hz)
        for n in power:
            cps_client.set_channel_power(img, n, level, count)
    with codeplug_edited(radio, backup_dir, edit, "roam"):
        radio.find_channel(round(rx * 1e6), "dmr", duplex=True)
        radio.wait_for(lambda x: x.get("tx_hz") == str(round(tx * 1e6)), timeout=5, what="the bench site's TX")
        yield


def site_power(radio):
    """(level, count): the power level the sites are written with, the bench's (Radio.bench_tx_level), and the radio's
    number of levels. A wake probe keys at the site's own power: a power changed for now is a temporary change, and
    any temporary change blocks a search (dmr-roaming.md 4.3)."""
    return radio.bench_tx_level(), as_int(radio.core().get("power_count") or "0")


def zone_on(radio):
    v = roam(radio)
    if v.get("roam.zone_on") != "1":
        flags = getattr(radio, "roam_flags", None)
        if flags is not None:
            flags.setdefault(radio.core().get("zone_index"), False)
        radio.link.roam("zone_on")
    return wait_roam(radio, lambda v: v.get("roam.zone_on") == "1", 1.0, "roaming on for the zone")


def site_zone(radio):
    """Roaming on, on a repeater site with a second site: the current channel if it is one, else the first such
    channel of the zones in order (each zone's flag put back by the roaming fixture). Its view."""
    v = zone_on(radio)
    if v["roam.state"] != "off":
        return v
    L = radio.link
    for zone in range(int(radio.core().get("zone_count") or 1)):
        L.zone(zone)
        radio.wait_for(lambda x: x.get("zone_index") == str(zone), 3.0, what=f"zone {zone}")
        for pos in range(int(radio.core().get("zone_size") or 0)):
            L.zone_pos(pos)
            radio.wait_for(lambda x: x.get("zone_pos") == str(pos), 2.0, what=f"position {pos}")
            if radio.core().get("mode") != "dmr":
                continue
            v = zone_on(radio)
            if v["roam.state"] != "off":
                return v
    skip("state", "no zone holds a repeater site with a second site on its talkgroup (roaming's sites)")


@pytest.mark.feature("dmr_roaming")
def test_roam_section(roaming):
    v = roam(roaming)
    missing = [k for k in ROAM_KEYS if k not in v]
    assert not missing, f"missing keys: {missing}"
    assert v["roam.built"] == "1"
    assert v["roam.state"] in ("off", "watch", "search", "locked", "survey"), v["roam.state"]
    beacon = as_int(v["set.roam_beacon_s"])
    assert beacon == 0 or 10 <= beacon <= 600
    assert 1 <= as_int(v["set.roam_beacon_len_s"]) <= 10
    assert v["set.roam_survey"] in SURVEYS
    assert -125 <= as_int(v["set.roam_rssi_dbm"]) <= -60
    assert 1 <= as_int(v["set.roam_hyst_db"]) <= 30
    assert v["set.roam_probe"] in ("passive", "wake")
    check = as_int(v["set.roam_check_s"])
    assert check == 0 or 30 <= check <= 3600
    print(f"roam: zone_on={v['roam.zone_on']} state={v['roam.state']} sites={v['roam.sites']} "
          f"threshold={v['set.roam_rssi_dbm']} dBm hysteresis={v['set.roam_hyst_db']} dB")


@pytest.mark.feature("dmr_roaming")
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


@pytest.mark.feature("dmr_roaming")
def test_roam_zone_toggle(roaming):
    if roaming.leave_vfo().get("vfo") == "1":
        skip("state", "the radio is in the VFO: roaming works on channels")
    L = roaming.link
    before = roam(roaming).get("roam.zone_on")
    L.roam("zone_toggle")
    v = wait_roam(roaming, lambda x: x.get("roam.zone_on") != before, 1.0, "zone flag flipped")
    if v["roam.zone_on"] == "1":
        if v["roam.state"] == "off":
            assert v["roam.sites"] == "0"
            print(f"zone on, but this channel ({v.get('channel_number')}) is not a site with a second site")
        else:
            assert v["roam.state"] in ("watch", "search", "survey") and as_int(v["roam.sites"]) >= 2, v
    else:
        assert v["roam.state"] == "off"
    L.roam("zone_toggle")
    wait_roam(roaming, lambda x: x.get("roam.zone_on") == before, 1.0, "zone flag back")


@pytest.mark.feature("dmr_roaming")
def test_roam_now_passive(roaming):
    if roaming.leave_vfo().get("vfo") == "1":
        skip("state", "the radio is in the VFO: roaming works on channels")
    L = roaming.link
    L.setting("roam_probe", 0)
    v = site_zone(roaming)
    sites = as_int(v["roam.sites"])
    numbers = {roam(roaming)[f"site.{i}"].split(":")[0] for i in range(sites)}
    s0, p0, a0 = as_int(v["roam.searches"]), as_int(v["roam.probes"]), as_int(v["roam.aborts"])
    L.roam("now")
    wait_roam(roaming, lambda x: as_int(x.get("roam.searches", "0")) > s0, 5.0, "search started")
    v = wait_roam(roaming, lambda x: x.get("roam.state") == "watch" and as_int(x.get("roam.searches", "0")) > s0,
                  sites * 2.0 + 5.0, "search done")
    assert v.get("channel_number") in numbers, (v.get("channel_number"), numbers)
    assert as_int(v["roam.probes"]) == p0, "a passive search sent a probe"
    assert as_int(v["roam.aborts"]) == a0
    print(f"roam now: on site {v['roam.site']} (channel {v['channel_number']}), why={v['roam.why']}, "
          f"roams={v['roam.roams']} stays={v['roam.stays']}; sites: "
          + " ".join(f"{k}={v[k]}" for k in sorted(v) if k.startswith("site.")))
    L.roam("lock")
    wait_roam(roaming, lambda x: x.get("roam.state") == "locked", 1.0, "locked")
    L.roam("now")
    wait_roam(roaming, lambda x: x.get("roam.state") in ("watch", "search", "survey"), 1.0, "roam now ends the lock")


@pytest.mark.feature("dmr_roaming")
@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_roam_to_bench_repeater(roaming, sdr_bench, pytestconfig, backup_dir):
    rx, tx = pytestconfig.getoption("--roam-rx"), pytestconfig.getoption("--roam-tx")
    if not rx or not tx:
        skip("opt_in", "give the bench repeater's pair as the radio sees it: --roam-rx MHz (radio receives, bench "
                       "transmits) --roam-tx MHz (radio transmits, bench receives); a pair you are licensed for")
    with bench_sites(roaming, backup_dir, pytestconfig, rx, tx):
        _roam_to_bench_repeater(roaming, sdr_bench, pytestconfig, rx, tx)


def _roam_to_bench_repeater(roaming, sdr_bench, pytestconfig, rx, tx):
    cc = rig(pytestconfig, "dmr-cc")
    L = roaming.link
    pos_b = roaming.find_channel(round(rx * 1e6), "dmr", duplex=True)
    v = zone_on(roaming)
    if v["roam.state"] == "off":
        skip("state", "the bench repeater's channel is not a site with a second site in its zone")
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
        skip("state", "no other site to start from")
    roaming.lowest_power()      # nothing to step: bench_sites wrote the bench level into every site
    assert roaming.core().get("temporary") == "0", "a temporary change on the site: it blocks the search"
    L.setting("roam_probe", 1)
    # The lowest threshold: the test is the wake-up and the move, not the bench's level (a bench SDR's outbound can
    # reach the radio near -120 dBm, under the -105 default, and a site under it is never chosen). The fixture puts
    # the threshold back
    L.setting("roam_rssi_dbm", -125)
    wait_roam(roaming, lambda x: x.get("set.roam_rssi_dbm") == "-125", 1.0, "threshold -125 dBm")
    v = roam(roaming)
    r0, p0 = as_int(v["roam.roams"]), as_int(v["roam.probes"])
    with sdr_bench.lease():
        p = sdr_bench.repeater_start(tx, rx, cc, 60)       # input: what the radio sends on; output: what it hears
        trail, t0 = [], time.monotonic()        # the visits as the radio went: where it listened, what it heard

        def roamed(x):
            k = (x.get("channel_number"), x.get("roam.visit"), x.get("state"), x.get("dmr.repeater_up"),
                 round(as_int(x.get("rssi_dbm", "0")) / 5) * 5, x.get("roam.probes"))
            if not trail or trail[-1][1:] != k:
                trail.append((round(time.monotonic() - t0, 2),) + k)
            return x.get("channel_number") == bench_ch and x.get("roam.state") == "watch"
        err = None
        try:
            L.roam("now")
            v = roaming.wait_for(roamed, 40.0, sections=("roam", "core", "dmr"), what="roamed to the bench repeater")
        except AssertionError as e:
            err = e
        finally:
            events, out = bench_events(p)
        if err is not None:     # whether the bench heard the probes, and when the radio listened, tell radio from bench
            steps = "\n".join(" ".join(map(str, t)) for t in trail[-60:])
            raise AssertionError(f"{err}\ns channel visit state repeater_up rssi probes:\n{steps}\n"
                                 f"bench repeater: {out[-1500:] if out else '(no output)'}")
        assert as_int(v["roam.roams"]) == r0 + 1
        assert as_int(v["roam.probes"]) > p0
    print("roamed: " + " ".join(f"{k}={v[k]}" for k in sorted(v) if k.startswith("site.")) +
          f" probes={v['roam.probes']}")
    assert any("wakeup" in e for e in events), \
        f"the bench_repeater job saw no wakeup: {out[-800:] if out else ''}"


@pytest.mark.feature("dmr_roaming")
def test_roam_survey_settings_round_trip(roaming):
    L = roaming.link
    L.setting("roam_beacon_s", 30)
    L.setting("roam_beacon_len_s", 3)
    L.setting("roam_survey", 2)
    wait_roam(roaming, lambda x: x.get("set.roam_beacon_s") == "30" and x.get("set.roam_beacon_len_s") == "3" and
              x.get("set.roam_survey") == "site", 1.0, "survey settings set")
    # The roaming fixture puts the old values back


def surv(v, i):
    """surv.<i> = dBm:age_s:phase_ms:fresh as ints"""
    return [as_int(x) for x in v[f"surv.{i}"].split(":")]


def beacon_setup(roaming, pytestconfig):
    rx, tx = pytestconfig.getoption("--roam-rx"), pytestconfig.getoption("--roam-tx")
    if not rx or not tx:
        skip("opt_in", "give the bench repeater's pair as the radio sees it: --roam-rx MHz --roam-tx MHz (a pair you "
                       "are licensed for), in a zone with a second repeater channel on the same talkgroup")
    every, length = pytestconfig.getoption("--roam-beacon"), pytestconfig.getoption("--roam-beacon-len")
    L = roaming.link
    pos_b = roaming.find_channel(round(rx * 1e6), "dmr", duplex=True)
    v = zone_on(roaming)
    if v["roam.state"] == "off":
        skip("state", "the bench repeater's channel is not a site with a second site (same talkgroup) in its zone")
    L.setting("roam_probe", 0)
    L.setting("roam_survey", 1)
    L.setting("roam_beacon_s", int(every))
    L.setting("roam_beacon_len_s", int(length))
    wait_roam(roaming, lambda x: x.get("set.roam_beacon_s") == str(int(every)) and x.get("set.roam_survey") == "sync",
              1.0, "beacon settings")
    return rx, tx, every, length, pos_b, v["channel_number"]


def bench_beacon(sdr_bench, pytestconfig, rx, tx, every, length, seconds):
    cc = rig(pytestconfig, "dmr-cc")
    return sdr_bench.repeater_start(tx, rx, cc, seconds, extra=f"--beacon {every} --beacon-len {length}")


def bench_events(p):
    """End the bench repeater (repeater_start) and return (its JSON events, its output)."""
    p.terminate()
    if hasattr(p, "lines"):
        if getattr(p, "reader", None) is not None:
            p.reader.join(5)
        out = "".join(p.lines)          # the board job's lines as they came
    else:
        out, _ = p.communicate(timeout=10)
    events = []
    for line in (out or "").splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    return events, out


@pytest.mark.feature("dmr_roaming")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_roam_beacon_home(roaming, sdr_bench, pytestconfig, backup_dir):
    rx, tx = pytestconfig.getoption("--roam-rx"), pytestconfig.getoption("--roam-tx")
    if not rx or not tx:
        beacon_setup(roaming, pytestconfig)         # the skip that says what to give
    with bench_sites(roaming, backup_dir, pytestconfig, rx, tx):
        _roam_beacon_home(roaming, sdr_bench, pytestconfig)


def _roam_beacon_home(roaming, sdr_bench, pytestconfig):
    rx, tx, every, length, pos_b, bench_ch = beacon_setup(roaming, pytestconfig)
    v0 = roam(roaming)
    keys = ("roam.searches", "roam.roams", "roam.probes", "roam.surveys", "roam.beacons", "roam.cuts")
    c0 = {k: as_int(v0[k]) for k in keys}
    with sdr_bench.lease():
        p = bench_beacon(sdr_bench, pytestconfig, rx, tx, every, length, int(6 * every + 30))
        try:
            wait_roam(roaming, lambda x: as_int(x.get("roam.beacons", "0")) > c0["roam.beacons"] and
                      as_int(x.get("roam.win_conf", "0")) >= 1, 2 * every + 10, "window learned from its own site")
            wait_roam(roaming, lambda x: as_int(x.get("roam.surveys", "0")) > c0["roam.surveys"], 2 * every + 5,
                      "a survey round in the window")
            v = wait_roam(roaming, lambda x: as_int(x.get("roam.win_conf", "0")) >= 2 and
                          x.get("roam.state") == "watch", every + 10, "window confirmed, back home")
            time.sleep(2 * every)	# two more rounds: the silent sites missed twice
            v = roam(roaming)
        finally:
            events, out = bench_events(p)
    core = roaming.core()
    assert core.get("channel_number") == bench_ch, "not back on the bench site after the rounds"
    for k in ("roam.searches", "roam.roams", "roam.probes"):
        assert as_int(v[k]) == c0[k], f"{k} moved: {c0[k]} -> {v[k]}"
    assert not any("wakeup" in e for e in events), "the radio transmitted a wake-up (it only listens)"
    assert any("beacon" in e for e in events), f"the bench_repeater job sent no beacon: {out[-600:] if out else ''}"
    home = as_int(v["roam.site"])
    for i in range(as_int(v["roam.sites"])):
        if i != home:
            dbm, age, phase, fresh = surv(v, i)
            assert age >= 0, f"site {i} never measured: {v[f'surv.{i}']}"
    print("survey: " + " ".join(f"{k}={v[k]}" for k in sorted(v) if k.startswith(("surv.", "site.")) or
                                k in ("roam.win_conf", "roam.win_next_ms", "roam.surveys", "roam.visits",
                                      "roam.cuts", "roam.beacons")))


@pytest.mark.feature("dmr_roaming")
@pytest.mark.rf
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_roam_beacon_other_site(roaming, sdr_bench, pytestconfig, backup_dir):
    rx, tx = pytestconfig.getoption("--roam-rx"), pytestconfig.getoption("--roam-tx")
    if not rx or not tx:
        beacon_setup(roaming, pytestconfig)         # the skip that says what to give
    with bench_sites(roaming, backup_dir, pytestconfig, rx, tx):
        _roam_beacon_other_site(roaming, sdr_bench, pytestconfig)


def _roam_beacon_other_site(roaming, sdr_bench, pytestconfig):
    rx, tx, every, length, pos_b, bench_ch = beacon_setup(roaming, pytestconfig)
    L = roaming.link
    v = roam(roaming)
    sites = as_int(v["roam.sites"])
    numbers = [v[f"site.{i}"].split(":")[0] for i in range(sites)]
    bench_i = numbers.index(bench_ch)
    size = as_int(roaming.core().get("zone_size") or "0")
    for pos in range(size):
        if pos == pos_b:
            continue
        L.zone_pos(pos)
        c = wait_roam(roaming, lambda x: x.get("zone_pos") == str(pos), 2.0, f"position {pos}")
        if c.get("channel_number") in numbers and roam(roaming).get("roam.state") == "watch":
            break
    else:
        skip("state", "no other site to start from")
    own_ch = roaming.core().get("channel_number")
    p0 = as_int(roam(roaming)["roam.probes"])
    budget = 2 * every + length + 1 + every + length + 5 + 9 * (every + length + 2) + 2 * every
    with sdr_bench.lease():
        p = bench_beacon(sdr_bench, pytestconfig, rx, tx, every, length, int(budget + 30))
        try:
            v = wait_roam(roaming, lambda x: surv(x, bench_i)[3] == 1 and surv(x, bench_i)[0] != 0 and
                          x.get("roam.state") == "watch", budget, "the bench site measured by the survey")
        finally:
            events, out = bench_events(p)
    assert as_int(v["roam.win_conf"]) >= 1
    assert as_int(v["roam.probes"]) == p0
    assert not any("wakeup" in e for e in events), "the radio transmitted a wake-up (it only listens)"
    assert roaming.core().get("channel_number") == own_ch, "not back on its own site"
    print(f"bench site {bench_i}: surv={v[f'surv.{bench_i}']} win_conf={v['roam.win_conf']} "
          f"surveys={v['roam.surveys']} visits={v['roam.visits']}")

