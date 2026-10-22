# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The open-firmware CPS shim (docs/spec/cps-protocol.md 2) and the settings store (docs/spec/settings-store.md) on the
radio, through tools/cps_client.py the way qdmr talks to it.

Radio only, nothing written to the codeplug by default: radio info, two reads of the qdmr regions match, the current
channel decoded from the read matches the radio view, a settings save after a channel change. Opt-in:
--allow-codeplug-write backs the codeplug up, writes one channel name, reads it back, restores the backup and
verifies it; --allow-reboot checks the channel position survives a restart. test_cps_write_dry_run checks the backup and
restore steps without writing; run it (and look at its output) before the first --allow-codeplug-write run on a
radio. Backups go to --codeplug-backup-dir (default the test's tmp directory)."""
import contextlib
import os
import time

import pytest

from odr_hil import DebugLink, DebugLinkError, as_int, find_ports, skip
import cps_client

QDMR = cps_client.REGIONS["qdmr"]


def cps_session(radio, fn):
    """Run fn(cps) with the debug link closed and the port opened as a CPS client"""
    # The image's place in the flash (the HR_C7000 radios: 0x700000), what the client adds to its addresses
    offset = as_int(storage(radio).get("cps.openfw_eeprom"), 0)
    with radio.released() as port:
        cps = cps_client.open_cps(port, timeout=3.0, offset=offset)
        try:
            return fn(cps)
        finally:
            cps.close()


def storage(radio):
    return radio.view(["storage"])


@pytest.mark.feature("cps_openfw")
def test_cps_info(radio):
    info = cps_session(radio, lambda c: c.info())
    assert info["struct"] == 3, info
    # The type qdmr knows the radio by (cps-protocol.md 2.2): the DM-1701's own; other radios one qdmr knows (a board
    # without a hal_cps_t reports the shim's default, 6)
    board_name = radio.version().get("board")
    want = (8, 10) if board_name == "DM-1701" else tuple(cps_client.RADIO_TYPES)
    assert info["radio_type"] in want, f"radio type {info['radio_type']} on the {board_name}: {info}"
    assert info["flash_id"] not in (0, 0xFFFFFFFF), info
    assert info["git"], info


@pytest.mark.feature("cps_openfw")
def test_cps_readback_matches(radio, tmp_path):
    """Two reads of the qdmr regions are identical; the current channel matches the decoded record"""
    a = cps_session(radio, lambda c: c.read_image(QDMR))
    b = cps_session(radio, lambda c: c.read_image(QDMR))
    (tmp_path / "read.bin").write_bytes(a)
    d = cps_client.compare(a, b, QDMR, skip=())
    assert not d, f"{len(d)} bytes differ between two reads, first {[(hex(x), y, z) for x, y, z in d[:4]]}"

    v = radio.core()
    if v.get("codeplug") != "open-firmware":
        skip("state", f"the radio runs the {v.get('codeplug')} codeplug: nothing stored to compare with")
    if v.get("vfo") == "1" or as_int(v.get("channel_number"), 0) == 0:
        skip("state", "the radio is in the VFO: no channel to compare")
    ch = cps_client.decode_channel(a, as_int(v["channel_number"]))
    assert ch is not None, f"channel {v['channel_number']} not marked used in the read"
    assert ch["name"] == v["channel_name"], (ch, v["channel_name"])
    if v.get("temp") in ("0x00", "0"):
        assert ch["rx_hz"] == as_int(v["rx_hz"]) and ch["tx_hz"] == as_int(v["tx_hz"]), (ch, v)

    s = storage(radio)
    assert s.get("cps.built") == "1"
    assert as_int(s.get("cps.overflows"), 0) == 0 and as_int(s.get("storage.errors"), 0) == 0, s


@pytest.mark.feature("settings_store")
def test_settings_saved(radio):
    """A channel change is saved within SETTINGS_SAVE_DELAY_MS (2 s); no failures or torn records"""
    v = radio.core()
    if v.get("vfo") == "1" or as_int(v.get("zone_size"), 0) < 2:
        skip("state", "needs a zone with two channels or more, on a channel")
    s0 = storage(radio)
    if s0.get("settings.load") == "ram":
        skip("no_hw", "no flash settings area on this board")
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


@pytest.mark.feature("settings_store")
@pytest.mark.reboot
def test_settings_survive_restart(radio):
    v = radio.core()
    if v.get("vfo") == "1" or as_int(v.get("zone_size"), 0) < 2:
        skip("state", "needs a zone with two channels or more, on a channel")
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


def read_backup(radio, backup_dir, tag):
    """Two reads of the qdmr regions, identical byte for byte (else the codeplug isn't stable enough to restore
    exactly), saved to backup_dir and read back from the file. Returns (image, path)."""
    a = cps_session(radio, lambda c: c.read_image(QDMR))
    b = cps_session(radio, lambda c: c.read_image(QDMR))
    d = cps_client.compare(a, b, QDMR, skip=())
    assert not d, f"two codeplug reads differ at {len(d)} bytes: not writing (first {d[:4]})"
    os.makedirs(backup_dir, exist_ok=True)
    path = os.path.join(str(backup_dir), f"codeplug-{tag}-{time.strftime('%Y%m%d-%H%M%S')}.bin")
    with open(path, "wb") as f:
        f.write(a)
    assert cps_client.load_image(path)[:len(a)] == a, f"the backup file {path} doesn't read back"
    print(f"codeplug backup: {path} (restore: tools/cps_client.py write {path} --no-reboot)")
    return a, path


@contextlib.contextmanager
def codeplug_edited(radio, backup_dir, edit, tag):
    """Back the codeplug up (read_backup), write edit(copy of the backup) over CPS (only the sectors that differ),
    check the radio reads back exactly the edit, yield (backup, edited); afterwards write the backup again over
    whatever the radio holds then and verify it byte for byte."""
    backup, path = read_backup(radio, backup_dir, tag)
    edited = bytearray(backup)
    edit(edited)
    changed = cps_client.compare(backup, edited, QDMR, skip=())
    assert changed, "the edit changes nothing"
    try:
        cps_session(radio, lambda c: c.write_image(edited, QDMR, reboot=False, base=backup))
        back = cps_session(radio, lambda c: c.read_image(QDMR))
        d = cps_client.compare(edited, back, QDMR)
        assert not d, f"read back differs at {len(d)} bytes, first {[(hex(x), y, z) for x, y, z in d[:4]]}"
        yield backup, edited
    finally:
        try:
            now = cps_session(radio, lambda c: c.read_image(QDMR))
        except Exception:                       # write every sector of the edit's plan if the read fails
            now = edited
        for attempt in range(3):                # a radio busy just after a test can miss the session's first command
            try:
                cps_session(radio, lambda c: c.write_image(backup, QDMR, reboot=False, base=now))
                break
            except cps_client.CpsError:
                if attempt == 2:
                    raise
                time.sleep(3)
        restored = cps_session(radio, lambda c: c.read_image(QDMR))
        d = cps_client.compare(backup, restored, QDMR)
        assert not d, f"RESTORE FAILED at {len(d)} bytes: write {path} back with tools/cps_client.py"
        print(f"codeplug restored and verified ({len(changed)} bytes had been changed)")


def pick_channel(radio, image):
    """The current channel's number when it is stored, else the first stored channel (0: none)."""
    v = radio.core()
    number = as_int(v.get("channel_number"), 0) if v.get("vfo") != "1" else 0
    if number == 0 or cps_client.decode_channel(image, number) is None:
        number = next((n for n in range(1, 1025) if cps_client.decode_channel(image, n)), 0)
    return number


@pytest.mark.feature("cps_openfw")
def test_cps_write_dry_run(radio, backup_dir):
    """Radio only, nothing written: everything test_cps_write_restore relies on, checked before any write. Two reads
    identical, the backup file reads back, the channel rename changes only that name's 16 bytes, writing the edit
    and writing the backup back plan the same sectors, and the restore plan applied to the edited image gives the
    backup back in every qdmr region byte."""
    backup, _ = read_backup(radio, backup_dir, "dryrun")
    number = pick_channel(radio, backup)
    if number == 0:
        skip("state", "no stored channel to edit")
    old = cps_client.decode_channel(backup, number)["name"]
    edited = bytearray(backup)
    cps_client.set_channel_name(edited, number, "ODR HIL" if old != "ODR HIL" else "ODR HIL 2")
    _, rec = cps_client.channel_addr(number)
    changed = cps_client.compare(backup, edited, QDMR, skip=())
    assert changed and all(rec <= a < rec + 16 for a, _, _ in changed), f"the edit touches {changed}"
    write_plan = cps_client.plan_write(edited, QDMR, base=backup)
    restore_plan = cps_client.plan_write(backup, QDMR, base=edited)
    assert write_plan and write_plan == restore_plan, (write_plan, restore_plan)
    radio_after_write = cps_client.apply_plan(bytearray(backup), edited, write_plan)
    assert not cps_client.compare(edited, radio_after_write, QDMR, skip=()), "the write plan misses edited bytes"
    radio_after_restore = cps_client.apply_plan(radio_after_write, backup, restore_plan)
    assert not cps_client.compare(backup, radio_after_restore, QDMR, skip=()), "the restore plan doesn't restore"
    print(f"dry run: channel {number} {old!r}: {len(changed)} bytes, sectors {[hex(j[0]) for j in write_plan]}")


@pytest.mark.feature("cps_openfw")
@pytest.mark.codeplug_write
def test_cps_write_restore(radio, backup_dir):
    """Back up, rename one channel over CPS, read it back, restore the backup and verify it byte for byte"""
    probe, _ = read_backup(radio, backup_dir, "probe")
    number = pick_channel(radio, probe)
    if number == 0:
        skip("state", "no stored channel to edit")
    v = radio.core()
    old = cps_client.decode_channel(probe, number)["name"]
    new = "ODR HIL" if old != "ODR HIL" else "ODR HIL 2"
    with codeplug_edited(radio, backup_dir, lambda img: cps_client.set_channel_name(img, number, new), "rename") \
            as (_, edited):
        assert cps_client.decode_channel(edited, number)["name"] == new
        if number == as_int(v.get("channel_number"), 0) and v.get("vfo") != "1":
            radio.wait_for(lambda c: c.get("channel_name") == new, timeout=3.0, what="the radio shows the edit")
    if number == as_int(v.get("channel_number"), 0) and v.get("vfo") != "1":
        radio.wait_for(lambda c: c.get("channel_name") == old, timeout=3.0, what="the radio shows the old name")
    s = storage(radio)
    assert as_int(s["cps.commits"]) >= 2 and as_int(s["storage.errors"]) == 0, s


@pytest.mark.feature("settings_store")
def test_storage_region(radio):
    """The settings / journal region: its state is reported, the read window holds, a read outside it is refused.
    A foreign region (another firmware's data) is reported, not claimed: `odr_debug.py storage claim` does that"""
    info = radio.link.storage_info()
    s = storage(radio)
    assert s.get("storage.region") == info["region"], (s, info)
    if info["region"] == "foreign":
        assert s.get("settings.load") == "foreign" and s.get("storage.journal") == "foreign", s
        skip("state", "the settings / journal region holds another firmware's data: dump and claim it with "
                      "tools/odr_debug.py storage claim (writes are refused until then)")
    assert info["region"] == "ours" and s.get("storage.journal") == "ok", (s, info)
    addr, _ = info["ranges"]["settings"]
    assert len(radio.link.storage_read(addr, 64)) == 64
    with pytest.raises(DebugLinkError) as e:
        radio.link.storage_read(addr - 16, 16)        # just before the ring: outside the window
    assert e.value.status == 7
