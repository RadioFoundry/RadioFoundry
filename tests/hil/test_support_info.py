# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The support info on the radio (docs/spec/support-info.md, debuglink.md 3.29): VERSION's source=, SUPPORT INFO's
hardware lines, and the image hash a step per request while the radio keeps answering. Read only."""
import hashlib
import time

import pytest

from rfoundry_debug import DebugLinkError
import rfoundry_hil

SOURCES = {"official", "service", "pr", "local", "fork", "unknown"}


def support(radio):
    try:
        return radio.link.support()
    except DebugLinkError as e:
        if e.status in (1, 4):
            rfoundry_hil.skip("not_built", "firmware without the SUPPORT command (CONFIG_SUPPORT_INFO off, or older)")
        raise


@pytest.mark.feature("support_info")
def test_version_says_where_the_build_came_from(radio):
    v = radio.version(fresh=True)
    if "source" not in v:
        rfoundry_hil.skip("not_built", "firmware older than VERSION source=")
    assert v["source"] in SOURCES, v["source"]


@pytest.mark.feature("support_info")
def test_support_info_lines(radio):
    kv = support(radio)
    assert kv.get("source") == radio.version(fresh=True).get("source")
    assert kv.get("hw.mcu", "").startswith(("STM32F4", "HR_C7000")), kv
    size = int(kv.get("image.size", "0"))
    assert 64 * 1024 < size < 4 * 1024 * 1024, kv


@pytest.mark.feature("support_info")
def test_image_hash_steps_and_is_kept(radio):
    """The hash comes a step at a time with the radio answering between steps, each step short (5 ms of hashing on
    the radio, support-info.md 3); asked again, it answers at once with the same digest."""
    support(radio)
    steps, gaps = [], []
    last = [time.monotonic()]

    def step(done, size):
        gaps.append(time.monotonic() - last[0])
        steps.append(done)
        radio.link.ping()
        last[0] = time.monotonic()

    digest = radio.link.image_hash(progress=step)
    assert digest and len(digest) == 64 and int(digest, 16) >= 0, digest
    assert steps == sorted(steps)
    # a request's round trip with its step: the link's own latency plus the 5 ms, far under this on any link
    assert not gaps or max(gaps) < 0.25, "slowest HASH step %.0f ms" % (max(gaps) * 1000)
    again = []
    assert radio.link.image_hash(progress=lambda done, size: again.append(done)) == digest
    assert again == []


@pytest.mark.feature("support_info")
def test_image_hash_matches_the_flashed_image(radio, pytestconfig):
    """The digest is the flashed .bin's SHA-256 (support-info.md 3), the comparison a support report makes against
    releases.json: the same bytes, the same span. Needs --firmware-image."""
    path = pytestconfig.getoption("--firmware-image")
    if not path:
        rfoundry_hil.skip("bench", "give --firmware-image FILE (the .bin the radio was flashed with)")
    with open(path, "rb") as f:
        image = f.read()
    kv = support(radio)
    assert int(kv.get("image.size", "0")) == len(image), (kv.get("image.size"), len(image), path)
    assert radio.link.image_hash() == hashlib.sha256(image).hexdigest(), path
