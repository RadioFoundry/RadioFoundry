# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""bench_line.job_from_args: the job a bench_tx.py / bench_repeater.py command line sends a bench board. No radio:
these run anywhere, and tools/ci.sh runs them. An outbound's voice calls went as a string, so the board refused every
call ("outbound --voice: a list of 1-8 calls") and the outbound voice tests skipped."""
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "bench"))
import bench_line  # noqa: E402


def test_voice_goes_as_a_list():
    job = bench_line.job_from_args("bench_tx", "--freq 446.1 --cc 1 outbound --seconds 9.5 "
                                               "--voice '[{\"slot\":1,\"src\":3100001,\"dst\":202}]'")
    assert job == {"tool": "bench_tx", "freq": 446.1, "cc": 1, "cmd": "outbound", "seconds": 9.5,
                   "voice": [{"slot": 1, "src": 3100001, "dst": 202}]}


def test_text_and_hex_stay_strings():
    job = bench_line.job_from_args("bench_tx", ["--talker-alias", "123", "--fid", "10", "csbk", "--private"])
    assert job == {"tool": "bench_tx", "talker_alias": "123", "fid": "10", "cmd": "csbk", "private": True}


def test_bad_json_names_the_option():
    with pytest.raises(ValueError, match="--voice takes JSON"):
        bench_line.job_from_args("bench_tx", ["outbound", "--voice", "[{slot:1}]"])
