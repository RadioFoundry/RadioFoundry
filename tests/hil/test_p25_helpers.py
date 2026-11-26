# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The P25 HIL fixture's site records and opt-in contract. No radio or bench: these run on the host."""
import json

import pytest

import cps_client
import test_cps
import test_p25 as P


def image():
    img = bytearray(b"\xff" * cps_client.IMAGE_SIZE)
    lo, hi = cps_client.SETTINGS_AREA
    img[lo:hi] = b"RFKV\x01\x10" + bytes(10) + b"\xff" * (hi - lo - 16)
    return img


def records(img):
    lo, hi = cps_client.SETTINGS_AREA
    area = img[lo:hi]
    assert cps_client.area_state(area) == "valid"
    end, at, out = area[5] + int.from_bytes(area[6:8], "little"), area[5], {}
    while at < end:
        key, n = int.from_bytes(area[at:at + 2], "little"), int.from_bytes(area[at + 2:at + 4], "little")
        out[key] = bytes(area[at + 4:at + 4 + n])
        at += 4 + n
    return out


def test_two_site_fixture_uses_independent_literal_nac_records():
    img = image()
    lo, hi = cps_client.SETTINGS_AREA
    img[lo:hi] = P.area_with(img[lo:hi], 0x4567, b"keep")
    plan = (446100000, 0, 0x1000, 0x1002)
    P.make_p25_site_nacs(img, 1, plan, 446100000, 446112500)
    r = records(img)
    assert r[P.KEY_SYS_ENTRY + 2] == bytes.fromhex("000100007A7B0000")
    assert r[P.KEY_SYS_ENTRY + 3] == bytes.fromhex("000101007C7D0080")
    assert r[P.KEY_SYS_ENTRY + 4][:6] == bytes.fromhex("000001010210")
    assert r[P.KEY_SYSTEM][1] == 0 and r[P.KEY_SYSTEM][33:35] == bytes.fromhex("9382")
    assert r[P.KEY_CH_TAG][4] == 0 and r[0x4567] == b"keep"


@pytest.mark.parametrize("nac", [P.SITE_NAC, "000"])
def test_control_playback_passes_nac_to_existing_generator(tmp_path, monkeypatch, nac):
    import p25tool
    seen = []

    def generate(args):
        seen.append(args)
        with open(args[2], "w") as f:
            json.dump({"total_syms": 200}, f)

    monkeypatch.setattr(p25tool, "run", generate)
    P.cc_file(str(tmp_path), (446100000, 0, 0x1000, 0x1002), nac=nac, units=2)
    assert seen[0][seen[0].index("--nac") + 1] == nac
    assert seen[0][seen[0].index("--units") + 1] == 2


def test_two_site_fixture_reserves_every_entry_before_writing(tmp_path, monkeypatch):
    img = image()
    lo, hi = cps_client.SETTINGS_AREA
    img[lo:hi] = P.area_with(img[lo:hi], P.KEY_SYS_ENTRY + 4, b"existing")
    monkeypatch.setattr(test_cps, "read_backup", lambda *args: (img, "unused"))
    monkeypatch.setattr(test_cps, "pick_channel", lambda *args: 1)

    class Radio:
        def core(self):
            return {"vfo": "0", "channel_number": "1"}

    with pytest.raises(pytest.skip.Exception, match="holds a system or this channel's tag"):
        P.trunk_slot(Radio(), tmp_path, entry_count=5)


def test_site_nac_hil_requires_bench_rf_and_codeplug_opt_ins_without_dut_tx():
    markers = {m.name for m in P.test_p25_site_nac.pytestmark}
    assert {"rf", "codeplug_write", "bench_service", "bench_modem"} <= markers
    assert "radio_tx" not in markers
    assert any(m.name == "feature" and m.args == ("p25_site_nac",) for m in P.test_p25_site_nac.pytestmark)
