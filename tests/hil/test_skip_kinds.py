# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The skip helper (odr_hil.skip and friends) and tools/check_hil_skips.py. No radio: these run anywhere, and
tools/ci.sh runs them."""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

import pytest

from odr_hil import ROOT, SKIP_KINDS, skip, skip_if, skip_kind, skip_mark, skip_reason

sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_hil_skips  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def test_kinds():
    assert set(SKIP_KINDS) == {"not_built", "no_hw", "state", "opt_in", "bench", "other_board"}
    assert check_hil_skips.skip_kinds() == set(SKIP_KINDS)


def test_reason_format():
    assert skip_reason("not_built", "built without CONFIG_X") == "[not_built] built without CONFIG_X"
    with pytest.raises(ValueError, match="unknown skip kind"):
        skip_reason("missing", "x")
    with pytest.raises(ValueError, match="needs a reason"):
        skip_reason("state", "  ")
    with pytest.raises(ValueError):
        skip("nope", "x")       # a bad kind is a test bug: it fails, it doesn't skip  # hil-skip: the test


def test_skip_raises_pytests_skip():
    with pytest.raises(pytest.skip.Exception) as e:  # hil-skip: the test
        skip("opt_in", "run with --allow-rf")
    assert e.value.msg == "[opt_in] run with --allow-rf"
    assert e.value.kind == "opt_in"
    assert skip_kind(e.value) == "opt_in"


def test_skip_kind_reads_every_form():
    assert skip_kind("[bench] SDR bench busy") == "bench"
    assert skip_kind("Skipped: [no_hw] no PWM output") == "no_hw"
    assert skip_kind(("tests/hil/test_x.py", 12, "Skipped: [state] hotspot mode")) == "state"
    assert skip_kind("plain pytest skip") is None
    assert skip_kind("[unknown] x") is None
    assert skip_kind("[no closing bracket") is None


def test_marks_carry_the_kind():
    assert skip_mark("opt_in", "transmits").kwargs["reason"] == "[opt_in] transmits"
    m = skip_if(True, "opt_in", "set ODR_X=1")
    assert m.args == (True,) and m.kwargs["reason"] == "[opt_in] set ODR_X=1"


def test_report_and_junit_carry_the_kind(tmp_path):
    """Through pytest itself with this directory's conftest: the kind on the report and in the JUnit XML, and the
    skip reported at the caller's line."""
    (tmp_path / "test_fake.py").write_text(
        "import pytest\n"
        "from odr_hil import skip, skip_if\n\n"
        "def test_called():\n"
        "    skip('no_hw', 'the radio lacks it')\n\n"
        "@skip_if(True, 'opt_in', 'set ODR_X=1')\n"
        "def test_marked():\n"
        "    pass\n\n"
        "def test_plain():\n"
        "    pytest.skip('a skip without a kind')\n")
    xml = tmp_path / "out.xml"
    env = dict(os.environ, PYTHONPATH=os.pathsep.join([HERE, os.environ.get("PYTHONPATH", "")]))
    r = subprocess.run([sys.executable, "-m", "pytest", "-p", "conftest", "-p", "no:cacheprovider", "-rs",
                        f"--junitxml={xml}", "--rootdir", str(tmp_path), str(tmp_path / "test_fake.py")],
                       cwd=tmp_path, env=env, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "test_fake.py:5: [no_hw] the radio lacks it" in r.stdout, r.stdout
    kinds = {}
    for tc in ET.parse(xml).getroot().iter("testcase"):
        props = {p.get("name"): p.get("value") for p in tc.iter("property")}
        kinds[tc.get("name")] = props.get("skip_kind")
    assert kinds == {"test_called": "no_hw", "test_marked": "opt_in", "test_plain": None}, kinds


def check(src):
    return check_hil_skips.check_source(src, os.path.join(ROOT, "tests", "hil", "test_x.py"), set(SKIP_KINDS))


def test_check_flags_bare_skips():
    assert len(check("import pytest\npytest.skip('x')\n")) == 1
    assert len(check("import pytest\n@pytest.mark.skipif(True, reason='x')\ndef test_a(): pass\n")) == 1
    assert len(check("import pytest\nm = pytest.mark.skip(reason='x')\n")) == 1
    assert len(check("import pytest\npytest.importorskip('numpy')\n")) == 1
    assert len(check("import pytest\nraise pytest.skip.Exception('x')\n")) == 1
    assert len(check("from pytest import skip\n")) == 1
    # the pragma counts only in odr_hil.py
    assert len(check("import pytest\npytest.skip('x')  # hil-skip: the helper\n")) == 1


def test_check_wants_a_known_literal_kind():
    assert check("from odr_hil import skip\nskip('state', 'the radio is in hotspot mode')\n") == []
    assert check("import odr_hil\nodr_hil.skip('bench', 'busy')\n") == []
    assert check("skip_if(True, 'opt_in', 'x')\nskip_mark('opt_in', 'x')\n") == []
    assert "unknown kind" in check("skip('later', 'x')\n")[0]
    assert "string literal" in check("k = 'state'\nskip(k, 'x')\n")[0]
    assert "no reason" in check("skip('state')\n")[0]
    assert "unknown kind" in check("skip_if(True, 'nope', 'x')\n")[0]


def test_tree_is_clean():
    assert check_hil_skips.main([]) == 0
