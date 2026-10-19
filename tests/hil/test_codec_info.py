# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""VERSION's codec= key (docs/spec/debuglink.md 3.1), which tools/odrflash reads to decide whether a firmware update
can leave the codec sectors alone. Read only."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "odrflash"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))
from odrflash import boards  # noqa: E402
import hrc7000_codec  # noqa: E402


def test_version_reports_codec(radio):
    v = radio.version(fresh=True)
    assert "codec" in v, "firmware without the codec= key (older than the release tooling)"
    board = boards.from_debuglink_name(v.get("board", ""))
    allowed = {"none", "absent", "present"} | ({board.codec.id} if board else set())
    if board is not None and board.family != "anyroad-dfu":
        # HR_C7000: "<board>-<tag>" for each stock build of the radio (platform/hrc7000/vocoder.c profile())
        allowed |= {"%s-%03d" % (board.id, p["tag"]) for p in hrc7000_codec.PROFILES.values() if p["board"] == board.id}
        allowed.add(board.id + "-unknown")
    assert v["codec"] in allowed, v["codec"]


def test_codec_key_agrees_with_dmr_view(radio):
    """codec= and the dmr section's dmr.codec come from the same board check."""
    v = radio.version(fresh=True)
    dmr = radio.view(["dmr"])
    if dmr.get("dmr.built") == "0":
        assert v["codec"] == "none"
        return
    present = dmr.get("dmr.codec") == "1"
    assert (v["codec"] not in ("absent", "none")) == present, (v["codec"], dmr.get("dmr.codec"))


def test_odrflash_probe_reads_it(radio):
    """odrflash's own minimal debug-link client parses the same reply (frame, CRC, key=value)."""
    from odrflash import debuglink
    reply = radio.link.request(debuglink.CMD_VERSION)[1].decode("ascii", "replace")
    kv = debuglink.parse_kv(reply)
    assert kv.get("codec") == radio.version(fresh=True).get("codec")
