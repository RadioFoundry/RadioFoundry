# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""MD-380 panel detection (docs/spec/hardware-md380.md 7), radio only, never transmits. At start-up the display driver
reads the controller's ID (0x04, and 0xDA-0xDC) after a hardware reset and the factory byte (SPI flash security
register 3, 0x1D), and picks the st7735 driver's type from them (boards/md380/panel_md380.c). The link section shows
what it found: display.panel_raw, panel_id, panel_id_reg, panel_controller, panel_source and panel_type. Checks: the
ID reads back the same through both commands (a working read path), the controller is one we know, and the type is
the one the rules give. A build with MD380_PANEL_TYPE (panel_source=build) skips the last check. A board's own test:
skipped other_board on any other radio (tests/hil/hil_plugin.py)."""
from rfoundry_hil import skip


def panel(radio):
    v = radio.view(["link"])
    if "display.panel_controller" not in v:
        skip("not_built", "firmware without the panel detection keys")
    return v


def expected_type(raw, panel_id):
    """The rule of panel_md380.c, written out again."""
    factory = None if raw in (None, "none") else int(raw, 16)
    programmed = factory is not None and factory != 0xFF
    if panel_id.startswith("0091"):
        return 2 if programmed and factory & 1 == 0 else 3
    if panel_id.startswith("7c"):
        return 1 if programmed and factory & 3 == 1 else 0
    return factory & 3 if programmed else 0


def test_panel_id_reads_back(radio):
    """RDDID and ID1-ID3 return the same three bytes; all 00 or all ff means the read path is broken."""
    v = panel(radio)
    pid, reg = v.get("display.panel_id"), v.get("display.panel_id_reg")
    print(f"panel id {pid}, id1-3 {reg}, controller {v['display.panel_controller']}, factory {v.get('display.panel_raw')}")
    assert pid not in (None, "000000", "ffffff"), f"panel ID not read: {pid}"
    assert pid == reg, f"RDDID {pid} and ID1-ID3 {reg} differ"


def test_panel_controller_known(radio):
    v = panel(radio)
    assert v["display.panel_controller"] != "unknown", \
        f"controller ID {v.get('display.panel_id')} not recognized: add it to boards/md380/panel_md380.c"
    assert v["display.panel_source"] in ("id", "build"), v["display.panel_source"]


def test_panel_type_follows_rules(radio):
    v = panel(radio)
    if v["display.panel_source"] == "build":
        skip("state", "MD380_PANEL_TYPE build: the type is the build's")
    want = expected_type(v.get("display.panel_raw"), v.get("display.panel_id", ""))
    assert int(v["display.panel_type"]) == want, f"type {v['display.panel_type']}, rules give {want}"
