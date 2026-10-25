# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Zone sharing (docs/spec/zone-share.md): the `zs.*` keys of the air view section, and one channel sent from the UI
heard on air as a zone share fragment.

The send test drives the UI with injected keys: Zones, # on the current zone (its channels), # on the focused channel,
then Send channel to... and OK on the first contact. Which menu # opens depends on the build: zone sharing's own
(`zone_share`, item 2), or with the zone editor its Channel actions (`zchannel`, item 5); the test reads the screen
name and takes either path. A talkgroup contact gets an unconfirmed packet, a private one a
confirmed packet the bench doesn't answer; either way the first try is on air. The pass criteria are the radio's
counters (one send taken and ended) and the bench hearing a UDP datagram from the radio to port 4100 whose payload is
a fragment of a valid record header.

Opt-in RF: radio_tx + bench_service + bench_modem (--allow-rf), a DMR channel on --dmr-freq in the current zone and at
least one contact in the codeplug.

test_zone_and_channel_between_radios needs a second radio (--peer-port) and writes codeplugs (--allow-codeplug-write,
both put back): the --radio-port radio sends a zone and then a channel over a private contact, the peer takes each
offer and imports it, and its codeplug, read over CPS, must hold them (kept as a file for the Codeplug Editor)."""
import os
import time

import pytest

from rfoundry_hil import skip
import dmr_packet
from test_dmr_ip import _setup

ZS_PORT = 4100


def _zs(radio):
    v = radio.view(["air"])
    if v.get("zs.built") != "1":
        skip("not_built", "built without CONFIG_ZONE_SHARE (zs.built=0) or before the zs keys")
    return v


def _screen(radio, name):
    radio.wait_for(lambda v: v.get("ui.screen") == name, sections=("link",), what=f"screen {name}")


# Where "Send channel to..." is in each menu # on a channel can open (docs/spec/zone-share.md; the zone editor's
# Channel actions: Move up, Move down, Copy here, Edit channel, Send channel to..., Remove from zone)
SEND_CHANNEL_ITEM = {"zone_share": "2", "zchannel": "5"}


def _send_channel_menu(radio):
    """The menu # on a channel opened, and the key that picks Send channel to... in it."""
    v = radio.wait_for(lambda x: x.get("ui.screen") in SEND_CHANNEL_ITEM, sections=("link",),
                       what="screen " + " or ".join(SEND_CHANNEL_ITEM))
    return SEND_CHANNEL_ITEM[v["ui.screen"]]


@pytest.mark.feature("zone_share")
def test_zone_share_view(radio):
    """The keys are there and consistent: offers Off or Ask, a send state, counters that add up."""
    v = _zs(radio)
    assert v["zs.offers"] in ("0", "1")
    assert v["zs.send_state"] in ("0", "1", "2", "3")
    assert int(v["zs.sent"]) + int(v["zs.send_failed"]) <= int(v["zs.sends"])
    assert int(v["zs.offered"]) + int(v["zs.rx_bad"]) <= int(v["zs.rx_messages"])


@pytest.mark.feature("zone_share")
@pytest.mark.radio_tx
@pytest.mark.bench_service
@pytest.mark.bench_modem
def test_send_channel_heard(radio, sdr_bench, pytestconfig):
    before = _zs(radio)
    freq, cc, own, _ = _setup(radio, pytestconfig)
    with sdr_bench.lease():
        lis = sdr_bench.listen_start(freq, cc, seconds=25)
        radio.press("ok")
        radio.press("4")
        _screen(radio, "zones")
        radio.press("hash")
        _screen(radio, "zone")
        radio.press("hash")
        radio.press(_send_channel_menu(radio))
        _screen(radio, "zone_share_to")
        radio.press("ok")
        v = radio.wait_for(lambda x: int(x.get("zs.sends", 0)) > int(before["zs.sends"]) and
                           x.get("zs.send_state") in ("2", "3"), timeout=20, sections=("air",),
                           what="the send ended")
        heard = sdr_bench.listen_result(lis)
    radio.go_home()
    print("zs after:", {k: v[k] for k in v if k.startswith("zs.")})
    ours = [p for p in dmr_packet.packets(heard) if p.get("src") == own and p.get("dst_port") == ZS_PORT]
    print("bench heard:", [{k: p.get(k) for k in ("dpf", "group", "dst", "complete", "crc_ok")} for p in ours])
    if not ours:
        pytest.fail("the bench heard no datagram to port 4100 from the radio")
    payload = bytes.fromhex(ours[0]["payload"])
    # Fragment 1 of 1 (one channel fits one message): version 1, type 1, index 0 count 1, offset 0, then the record,
    # which starts with version 1 and its length (the rest of the payload)
    assert payload[0] == 1 and payload[1] == 1 and payload[3] == 0x01 and payload[4:6] == b"\0\0", payload.hex()
    record = payload[6:]
    assert record[0] == 1 and int.from_bytes(record[1:3], "big") == len(record), record.hex()
    assert record[7] & 0x01, "a one-channel record has the channel flag"


# ── Two radios: a zone and a channel sent from the --radio-port radio, offered on and imported by the --peer-port one ──
CONTACT_BASE, CONTACT_SIZE = 0xA7620, 24        # contact 1 in the CPS image (tools/cps_client.py, open-firmware-cps.md)
ZONE_BITMAP, ZONE_DATA, ZONE_SIZE, ZONE_SLOTS = 0x8010, 0x8030, 0xB0, 68


def _zones(img):
    """{name: [channel numbers]} of the zones in a CPS image (open-firmware-cps.md 7.3.4)."""
    out = {}
    for slot in range(ZONE_SLOTS):
        if img[ZONE_BITMAP + slot // 8] & (1 << (slot % 8)):
            rec = img[ZONE_DATA + slot * ZONE_SIZE:ZONE_DATA + (slot + 1) * ZONE_SIZE]
            name = bytes(rec[:16]).split(b"\xff")[0].decode("ascii", "replace")
            nums = [int.from_bytes(rec[16 + 2 * i:18 + 2 * i], "little") for i in range(80)]
            out[name] = nums[:nums.index(0)] if 0 in nums else nums
    return out


def _private_contact_1(dmr_id):
    """A CPS edit: contact 1 becomes a private contact for dmr_id, the first row of the Send to list."""
    def edit(img):
        name = b"HIL zone peer"
        img[CONTACT_BASE:CONTACT_BASE + CONTACT_SIZE] = (name + b"\xff" * (16 - len(name)) +
                                                        int(f"{dmr_id:08d}", 16).to_bytes(4, "big") +
                                                        bytes([1, 0, 0, 0xFF]))
    return edit


def _open_zone(radio):
    """Hub > Zones > the current zone's list (CONTEXT on it)."""
    radio.go_home()
    radio.press("ok")
    radio.press("4")
    _screen(radio, "zones")
    radio.press("hash")
    _screen(radio, "zone")


def _send(radio, before, what):
    """On Send to: OK on the first contact; wait for the send to end and return the view."""
    _screen(radio, "zone_share_to")
    radio.press("ok")
    v = radio.wait_for(lambda x: int(x.get("zs.sends", 0)) > int(before["zs.sends"]) and
                       x.get("zs.send_state") in ("2", "3"), timeout=60, sections=("air",), what=f"{what}: the send ended")
    radio.go_home()
    return v


def _wait_import(peer, pred, timeout, what):
    """Wait on the peer through an import. A missed answer is retried until timeout; the longest silence must stay
    under 2 s. Prints how long the import took and the longest silence."""
    from rfoundry_hil import DebugLinkError
    start = time.monotonic()
    silent_since, longest = None, 0.0
    while time.monotonic() - start < timeout:
        try:
            v = peer.view(["link"])
        except DebugLinkError:
            silent_since = silent_since or time.monotonic()
            continue
        if silent_since:
            longest = max(longest, time.monotonic() - silent_since)
            silent_since = None
        if pred(v):
            print(f"{what}: {time.monotonic() - start:.1f} s, the debug link silent {longest:.1f} s at most")
            # A lower-priority task waiting for the storage lock gets it between records (settings-store.md, Locks)
            assert longest < 2.0, f"{what}: the debug link was silent {longest:.1f} s while the import ran"
            return v
        time.sleep(0.2)
    raise AssertionError(f"{what}: not within {timeout} s (silent {longest:.1f} s at most; last {silent_since})")


def _take_offer(peer, before, what):
    """Wait for the offer on the peer, open it from the banner (CONTEXT) and pick Import."""
    v = peer.wait_for(lambda x: int(x.get("zs.offered", 0)) > int(before["zs.offered"]) and
                      x.get("zs.offer_pending") == "1", timeout=30, sections=("air",), what=f"{what}: the offer")
    peer.go_home()
    peer.press("hash")
    _screen(peer, "zone_offer")
    peer.press("1")
    return v


@pytest.mark.feature("zone_share")
@pytest.mark.second_radio
@pytest.mark.rf
@pytest.mark.radio_tx
@pytest.mark.codeplug_write
def test_zone_and_channel_between_radios(radio, pytestconfig, backup_dir):
    """The --radio-port radio sends its current zone, then one channel of it, to the --peer-port radio over a private
    contact (contact 1, written for the run and put back); the peer takes each offer from the banner and imports it.
    Both radios on the first channel on --dmr-freq, the sender at its lowest power, offers turned on in the peer's RAM
    (debug link SETTING 32). Asserted from the peer's codeplug, read over CPS before and after: a new zone named as
    the sender's, holding the sender's channels that are in the peer's bands, and the channel added to the peer's
    first zone. The peer's codeplug is put back afterwards."""
    peer_port = pytestconfig.getoption("--peer-port")
    if not peer_port:
        skip("opt_in", "needs a second radio to receive (--peer-port)")
    from rfoundry_hil import Radio
    from test_cps import QDMR, codeplug_edited, codeplug_kept, cps_session
    _zs(radio)
    freq = round(pytestconfig.getoption("--dmr-freq") * 1e6)
    peer = Radio(peer_port)
    snap_r, snap_p = radio.snapshot(), peer.snapshot()
    try:
        _zs(peer)
        peer_id = int(peer.view(["dmr"])["dmr.own_id"])
        if peer_id == 0:
            skip("state", "the peer has no DMR ID (dmr.own_id=0)")
        for r in (radio, peer):
            r.go_home()
            r.find_channel(freq, "dmr")
        radio.lowest_power()
        peer.lowest_power()
        sender_zone = radio.view(["core"])
        with codeplug_edited(radio, backup_dir, _private_contact_1(peer_id), "zs_sender") as (sent_img, _), \
                codeplug_kept(peer, backup_dir, "zs_peer") as peer_before:
            for r in (radio, peer):      # the CPS sessions left both on their channel; make sure
                r.go_home()
                r.find_channel(freq, "dmr")
            peer.link.setting("zs_offers", 1)
            zones_before = _zones(peer_before)
            sender_zones = _zones(sent_img)
            # The sender's current zone: a stored zone of its size that holds the channel it is on
            here = int(sender_zone["channel_number"])
            zone_name = next((n for n, ch in sender_zones.items()
                              if len(ch) == int(sender_zone["zone_size"]) and here in ch), None)
            if zone_name is None:
                skip("state", "the sender's channel on --dmr-freq is in no stored zone (only All channels)")
            # 1. The whole zone, from Zone options > Send zone to...
            b_r, b_p = radio.view(["air"]), peer.view(["air"])
            _open_zone(radio)
            for _ in range(90):         # Zone options is the list's last row
                v = radio.view(["link"])
                if v["ui.focus"] == str(int(v["ui.count"]) - 1):
                    break
                radio.press("down")
            else:
                raise AssertionError(f"Zone options never focused: {v}")
            radio.press("ok")
            _screen(radio, "zoneedit")
            radio.press("3")
            sent = _send(radio, b_r, "zone")
            assert sent["zs.send_state"] == "2", f"the zone send failed: {sent}"
            _take_offer(peer, b_p, "zone")
            _wait_import(peer, lambda x: "Imported" in x.get("ui.banner", "") or "Updated" in x.get("ui.banner", ""),
                         120, "the zone import")
            print("peer banner:", peer.view(["link"]).get("ui.banner"))
            # 2. One channel, from the channel's actions > Send channel to...
            b_r, b_p = radio.view(["air"]), peer.view(["air"])
            _open_zone(radio)
            radio.press("hash")
            radio.press(_send_channel_menu(radio))
            sent = _send(radio, b_r, "channel")
            assert sent["zs.send_state"] == "2", f"the channel send failed: {sent}"
            _take_offer(peer, b_p, "channel")
            _screen(peer, "zone_offer_zone")
            peer.press("ok")            # into the peer's first zone
            _wait_import(peer, lambda x: x.get("ui.screen") == "home" and "Importing" not in x.get("ui.toast", ""),
                         120, "the channel import")
            time.sleep(2)
            peer_after = cps_session(peer, lambda c: c.read_image(QDMR))
            kept = os.path.join(str(backup_dir), f"codeplug-zs_peer_imported-{time.strftime('%Y%m%d-%H%M%S')}.bin")
            with open(kept, "wb") as f:
                f.write(peer_after)     # for the Codeplug Editor check (site/tests, docs/spec/zone-share.md 5.3)
            print("peer codeplug after the imports:", kept)
            zones_after = _zones(peer_after)
            print("sender zone:", zone_name, sender_zones[zone_name])
            print("peer zones before:", {n: len(c) for n, c in zones_before.items()})
            print("peer zones after:", {n: len(c) for n, c in zones_after.items()})
            new = [n for n in zones_after if n not in zones_before]
            assert new, "the peer has no new zone after the import"
            assert any(n.startswith(zone_name[:12]) for n in new), f"new zones {new}, sent {zone_name!r}"
            assert len(new) == 1, f"new zones {new}"
            imported = zones_after[new[0]]
            # The channel went to the end of the first zone in the list, which may be the new one (a new zone takes
            # the lowest free slot). Either way the zones hold the zone import's channels plus one more entry
            added = sum(map(len, zones_after.values())) - sum(map(len, zones_before.values()))
            n_zone = added - 1
            assert 0 < n_zone <= len(sender_zones[zone_name]), \
                f"{added} zone entries added for a zone of {len(sender_zones[zone_name])} channels and one channel"
            assert len(imported) in (n_zone, n_zone + 1), f"the new zone holds {len(imported)}, the import wrote {n_zone}"
    finally:
        for r, s in ((radio, snap_r), (peer, snap_p)):
            try:
                r.go_home()
                r.restore(s)
            except Exception as e:      # the codeplug restore matters more; report and go on
                print(f"restore: {e}")
        peer.close()
