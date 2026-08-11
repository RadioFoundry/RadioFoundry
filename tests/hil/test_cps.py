# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The legacy CPS shim (docs/spec/cps-protocol.md 2) and the settings store (docs/spec/settings-store.md) on the
radio, through tools/cps_client.py the way qdmr talks to it.

Radio only, nothing written to the codeplug by default: radio info, two reads of the qdmr regions match, the current
channel decoded from the read matches the radio view, a settings save after a channel change. Opt-in:
--allow-codeplug-write backs the codeplug up, writes one channel name, reads it back, restores the backup and
verifies it; --allow-reboot checks the channel position survives a restart."""
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, as_int, find_ports
import cps_client

QDMR = cps_client.REGIONS["qdmr"]


def cps_session(radio, fn):
    """Run fn(cps) with the debug link closed and the port opened as a CPS client"""
    with radio.released() as port:
        cps = cps_client.open_cps(port, timeout=3.0)
        try:
            return fn(cps)
        finally:
            cps.close()


def storage(radio):
    return radio.view(["storage"])


def test_cps_info(radio):
    info = cps_session(radio, lambda c: c.info())
    assert info["struct"] == 3, info
    assert info["radio_type"] in (8, 10), f"not a DM-1701 type: {info}"
    assert info["flash_id"] not in (0, 0xFFFFFFFF), info
    assert info["git"], info


def test_cps_readback_matches(radio, tmp_path):
    """Two reads of the qdmr regions are identical; the current channel matches the decoded record"""
    a = cps_session(radio, lambda c: c.read_image(QDMR))
    b = cps_session(radio, lambda c: c.read_image(QDMR))
    (tmp_path / "read.bin").write_bytes(a)
    d = cps_client.compare(a, b, QDMR, skip=())
    assert not d, f"{len(d)} bytes differ between two reads, first {[(hex(x), y, z) for x, y, z in d[:4]]}"

    v = radio.core()
    if v.get("codeplug") != "legacy":
        pytest.skip(f"the radio runs the {v.get('codeplug')} codeplug: nothing stored to compare with")
    if v.get("vfo") == "1" or as_int(v.get("channel_number"), 0) == 0:
        pytest.skip("the radio is in the VFO: no channel to compare")
    ch = cps_client.decode_channel(a, as_int(v["channel_number"]))
    assert ch is not None, f"channel {v['channel_number']} not marked used in the read"
    assert ch["name"] == v["channel_name"], (ch, v["channel_name"])
    if v.get("temp") in ("0x00", "0"):
        assert ch["rx_hz"] == as_int(v["rx_hz"]) and ch["tx_hz"] == as_int(v["tx_hz"]), (ch, v)

    s = storage(radio)
    assert s.get("cps.built") == "1"
    assert as_int(s.get("cps.overflows"), 0) == 0 and as_int(s.get("storage.errors"), 0) == 0, s


def test_settings_saved(radio):
    """A channel change is saved within SETTINGS_SAVE_DELAY_MS (2 s); no failures or torn records"""
    v = radio.core()
    if v.get("vfo") == "1" or as_int(v.get("zone_size"), 0) < 2:
        pytest.skip("needs a zone with two channels or more, on a channel")
    s0 = storage(radio)
    if s0.get("settings.load") == "ram":
        pytest.skip("no flash settings area on this board")
    assert as_int(s0["settings.failures"]) == 0, s0
    pos = (as_int(v["zone_pos"]) + 1) % as_int(v["zone_size"])
    radio.link.zone_pos(pos)
    radio.wait_for(lambda c: c.get("zone_pos") == str(pos), what="channel change")
    s1 = radio.wait_for(lambda s: s.get("settings.dirty") == "0" and
                        as_int(s.get("settings.saves")) > as_int(s0["settings.saves"]),
                        timeout=5.0, sections=("storage",), what="settings saved")
    assert as_int(s1["settings.seq"]) > as_int(s0["settings.seq"]), (s0, s1)
    assert as_int(s1["settings.failures"]) == 0 and as_int(s1["storage.errors"]) == 0, s1


def reconnect(radio, timeout=20.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for port in [radio.port] + find_ports():
            try:
                radio.link = DebugLink(port, timeout=1.5)
                radio.link.ping()
                radio.port = port
                return
            except (DebugLinkError, OSError):
                time.sleep(0.5)
    raise AssertionError("the radio did not come back after the restart")


@pytest.mark.reboot
def test_settings_survive_restart(radio):
    v = radio.core()
    if v.get("vfo") == "1" or as_int(v.get("zone_size"), 0) < 2:
        pytest.skip("needs a zone with two channels or more, on a channel")
    pos = (as_int(v["zone_pos"]) + 1) % as_int(v["zone_size"])
    radio.link.zone_pos(pos)
    radio.wait_for(lambda s: s.get("settings.dirty") == "0", timeout=5.0, sections=("storage",),
                   what="settings saved")
    radio.link.reboot(0, radio.link.chip()["token"])
    radio.link.close()
    time.sleep(2.0)
    reconnect(radio)
    after = radio.wait_for(lambda c: c.get("state") is not None, timeout=5.0)
    assert after["zone_index"] == v["zone_index"] and after["zone_pos"] == str(pos), (v, after)
    s = storage(radio)
    assert s["settings.load"] in ("loaded", "upgraded"), s


@pytest.mark.codeplug_write
def test_cps_write_restore(radio, tmp_path):
    """Back up, rename one channel over CPS, read it back, restore the backup and verify it byte for byte"""
    backup = cps_session(radio, lambda c: c.read_image(QDMR))
    path = tmp_path / "codeplug-backup.bin"
    path.write_bytes(backup)
    print(f"codeplug backup: {path} (restore: tools/cps_client.py write {path} --no-reboot)")

    v = radio.core()
    number = as_int(v.get("channel_number"), 0) if v.get("vfo") != "1" else 0
    if number == 0 or cps_client.decode_channel(backup, number) is None:
        number = next((n for n in range(1, 1025) if cps_client.decode_channel(backup, n)), 0)
    if number == 0:
        pytest.skip("no stored channel to edit")
    old = cps_client.decode_channel(backup, number)["name"]
    edit = bytearray(backup)
    new = "ODR HIL" if old != "ODR HIL" else "ODR HIL 2"
    cps_client.set_channel_name(edit, number, new)

    try:
        cps_session(radio, lambda c: c.write_image(edit, QDMR, reboot=False, base=backup))
        back = cps_session(radio, lambda c: c.read_image(QDMR))
        d = cps_client.compare(edit, back, QDMR)
        assert not d, f"read back differs at {len(d)} bytes, first {[(hex(x), y, z) for x, y, z in d[:4]]}"
        assert cps_client.decode_channel(back, number)["name"] == new
        if number == as_int(v.get("channel_number"), 0) and v.get("vfo") != "1":
            radio.wait_for(lambda c: c.get("channel_name") == new, timeout=3.0, what="the radio shows the edit")
    finally:
        cps_session(radio, lambda c: c.write_image(backup, QDMR, reboot=False, base=edit))
        restored = cps_session(radio, lambda c: c.read_image(QDMR))
        d = cps_client.compare(backup, restored, QDMR)
        assert not d, f"RESTORE FAILED at {len(d)} bytes: write {path} back with tools/cps_client.py"
    s = storage(radio)
    assert as_int(s["cps.commits"]) >= 2 and as_int(s["storage.errors"]) == 0, s
