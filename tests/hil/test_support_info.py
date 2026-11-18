# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The support info on the radio (docs/spec/support-info.md, debuglink.md 3.29): VERSION's source=, SUPPORT INFO's
hardware lines, and the image hash a step per request while the radio keeps answering. Read only."""
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
    """The hash comes a step at a time with the radio answering between steps; asked again, it answers at once with
    the same digest."""
    support(radio)
    steps = []
    digest = radio.link.image_hash(progress=lambda done, size: (steps.append(done), radio.link.ping()))
    assert digest and len(digest) == 64 and int(digest, 16) >= 0, digest
    assert steps == sorted(steps)
    again = []
    assert radio.link.image_hash(progress=lambda done, size: again.append(done)) == digest
    assert again == []
