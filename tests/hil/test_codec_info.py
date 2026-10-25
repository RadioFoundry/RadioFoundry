# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""VERSION's codec= key (docs/spec/debuglink.md 3.1), which tools/rfoundry reads to decide whether a firmware update
can leave the codec sectors alone. Read only."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "rfoundry"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from rfoundry import boards  # noqa: E402
import hrc7000_codec  # noqa: E402


@pytest.mark.feature("codec_check")
def test_version_reports_codec(radio):
    v = radio.version(fresh=True)
    assert "codec" in v, "firmware without the codec= key (older than the release tooling)"
    board = boards.from_debuglink_name(v.get("board", ""))
    allowed = {"none", "absent", "present"} | ({board.codec.id} if board else set())
    if board is not None and board.family != "usb-dfu":
        # HR_C7000: "<board>-<tag>" for each stock build of the radio (platform/hrc7000/vocoder.c profile())
        allowed |= {"%s-%03d" % (board.id, p["tag"]) for p in hrc7000_codec.PROFILES.values() if p["board"] == board.id}
        allowed.add(board.id + "-unknown")
    assert v["codec"] in allowed, v["codec"]


@pytest.mark.feature("codec_check")
def test_codec_key_agrees_with_dmr_view(radio):
    """codec= and the dmr section's dmr.codec come from the same board check."""
    v = radio.version(fresh=True)
    dmr = radio.view(["dmr"])
    if dmr.get("dmr.built") == "0":
        assert v["codec"] == "none"
        return
    present = dmr.get("dmr.codec") == "1"
    assert (v["codec"] not in ("absent", "none")) == present, (v["codec"], dmr.get("dmr.codec"))


@pytest.mark.feature("codec_check")
def test_rfoundry_probe_reads_it(radio):
    """rfoundry's own minimal debug-link client parses the same reply (frame, CRC, key=value)."""
    from rfoundry import debuglink
    reply = radio.link.request(debuglink.CMD_VERSION)[1].decode("ascii", "replace")
    kv = debuglink.parse_kv(reply)
    assert kv.get("codec") == radio.version(fresh=True).get("codec")
