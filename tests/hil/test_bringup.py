# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Bring-up mode without a radio (bringup_plugin.py, tools/bringup.py): a fake radio that lacks a feature, run
through pytest with this directory's conftest. A required test that skips on it fails the bring-up run; the same
test only skips in a plain run; an excused skip and an optional test's skip stay skips; a run that finds no radio
fails. The tools/bringup.py parts need Python 3.11 or later ($ODR_PYTHON, else the newest python3.N on PATH)."""
import fcntl
import glob
import json
import os
import re
import shutil
import subprocess
import sys

from odr_hil import skip

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BRINGUP = os.path.join(ROOT, "tools", "bringup.py")

FAKE_TESTS = '''import pytest
from odr_hil import skip


class FakeLink:
    def ptt(self, on):
        pass


class FakeRadio:
    """A radio built with FM only: it lacks the split duplex roles."""
    link = FakeLink()

    def features(self):
        return {"FM", "DEBUGLINK"}

    def snapshot(self):
        return None

    def restore(self, snap):
        pass


@pytest.fixture
def radio():
    return FakeRadio()


@pytest.mark.feature("fake_split")
def test_gate(radio):
    if "HOTSPOT_SPLIT" not in radio.features():
        skip("no_hw", "the radio doesn't offer split duplex")


@pytest.mark.feature("fake_split", optional=True)
def test_extra(radio):
    skip("no_hw", "an optional check the radio can't do")


@pytest.mark.feature("fake_tx")
@pytest.mark.radio_tx
def test_keyed(radio):
    pass


@pytest.mark.feature("fake_split", "fake_ok")
def test_passes(radio):
    pass


def test_unlisted(radio):
    pass


@pytest.mark.feature("fake_gated")
def test_after_gate(radio):
    pass


@pytest.mark.feature("fake_gated", gate=True)
def test_zgate(radio):
    skip("no_hw", "the radio doesn't offer the gated feature")

'''

# No fixture of its own: the conftest's radio fixture, with a port that isn't there
FAKE_LINK_TEST = '''import pytest


@pytest.mark.feature("fake_link")
def test_real_radio_fixture(radio):
    pass
'''

def fake_tree(tmp_path):
    (tmp_path / "test_fake.py").write_text(FAKE_TESTS)
    (tmp_path / "test_fake_link.py").write_text(FAKE_LINK_TEST)
    return tmp_path


def env():
    e = {k: v for k, v in os.environ.items() if k != "ODR_PORT"}
    e["PYTHONPATH"] = os.pathsep.join([HERE, os.environ.get("PYTHONPATH", "")])
    return e


def pytest_run(tmp_path, *args, python=None):
    return subprocess.run([python or sys.executable, "-m", "pytest", "-p", "conftest", "-p", "no:cacheprovider",
                           "-rs", "--rootdir", str(tmp_path), "--radio-port", "/nonexistent/odr-fake-port",
                           *args], cwd=tmp_path, env=env(), capture_output=True, text=True, timeout=120)


def outcomes(path):
    with open(path) as f:
        return {t["id"].split("::")[-1]: t for t in json.load(f)["tests"]}


def test_required_skip_fails_the_bringup_run(tmp_path):
    t = fake_tree(tmp_path)
    plain = pytest_run(t, str(t / "test_fake.py"))
    assert plain.returncode == 0 and "3 passed, 4 skipped" in plain.stdout, plain.stdout + plain.stderr

    out = t / "out.json"
    r = pytest_run(t, str(t / "test_fake.py"), "--bringup", "fake_split,fake_tx", "--bringup-out", str(out))
    assert r.returncode == 1, r.stdout + r.stderr
    assert "bring-up: a required test of fake_split skipped (no_hw)" in r.stdout, r.stdout
    res = outcomes(out)
    assert res["test_gate"]["outcome"] == "failed"
    assert res["test_passes"]["outcome"] == "passed"
    assert res["test_extra"]["outcome"] == "skipped" and not res["test_extra"]["required"]
    # fake_tx needs rf and the run didn't give --allow-rf: the opt-in skip is excused, and verifies nothing
    assert res["test_keyed"]["outcome"] == "skipped" and res["test_keyed"]["excused"]
    assert res["test_keyed"]["skip_kind"] == "opt_in"
    assert "test_unlisted" not in res           # not in a selected feature: deselected
    assert "/nonexistent" not in out.read_text()


def test_a_failed_gate_fails_the_feature_unrun(tmp_path):
    t = fake_tree(tmp_path)
    out = t / "out.json"
    r = pytest_run(t, str(t / "test_fake.py"), "-v", "--bringup", "fake_gated", "--bringup-out", str(out))
    assert r.returncode == 1, r.stdout + r.stderr
    # the gate runs first though it is collected last, and the feature's other required test doesn't run
    assert r.stdout.index("test_zgate") < r.stdout.index("test_after_gate"), r.stdout
    res = outcomes(out)
    assert res["test_zgate"]["outcome"] == "failed"
    assert res["test_after_gate"]["outcome"] == "failed"
    assert "capability gate failed, not run: fake_gated's gate test_zgate failed" in res["test_after_gate"]["reason"]


def test_no_radio_fails_in_bringup_mode(tmp_path):
    t = fake_tree(tmp_path)
    plain = pytest_run(t, str(t / "test_fake_link.py"))
    assert plain.returncode == 0 and "1 skipped" in plain.stdout, plain.stdout + plain.stderr
    out = t / "out.json"
    r = pytest_run(t, str(t / "test_fake_link.py"), "--bringup", "fake_link", "--bringup-out", str(out))
    assert r.returncode == 1, r.stdout + r.stderr
    assert outcomes(out)["test_real_radio_fixture"]["outcome"] == "failed"
    assert "<port>" in out.read_text() and "/nonexistent" not in out.read_text()


def python311():
    """A Python 3.11 or later for tools/bringup.py: $ODR_PYTHON, this one, or the newest python3.N on PATH."""
    if os.environ.get("ODR_PYTHON"):
        return os.environ["ODR_PYTHON"]
    if sys.version_info >= (3, 11):
        return sys.executable
    best = None
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for p in glob.glob(os.path.join(d, "python3.*")):
            m = re.fullmatch(r"python3\.(\d+)", os.path.basename(p))
            if m and int(m.group(1)) >= 11 and os.access(p, os.X_OK) and (best is None or int(m.group(1)) > best[0]):
                best = (int(m.group(1)), p)
    if best is None:
        skip("bench", "no Python 3.11 or later on PATH for tools/bringup.py (set $ODR_PYTHON)")
    return best[1]


FAKE_TOML = '''
[feature.fake_split]
spec = "tests/hil/README.md"
source = ["tests/hil/bringup_plugin.py"]

[feature.fake_ok]
spec = "tests/hil/README.md"
source = ["tests/hil/bringup_plugin.py"]

[feature.fake_new]
spec = "tests/hil/README.md"
source = ["tests/hil/bringup_plugin.py"]

[feature.fake_seeded]
spec = "LICENSE"
source = ["LICENSE"]
'''


def collect_args(t):
    """What tools/bringup.py needs to collect the fake tree: the test file, the conftest, the rootdir."""
    return ["--pytest-python", sys.executable, "--tests", str(t / "test_fake.py"), "--pytest-arg=-p",
            "--pytest-arg=conftest", f"--pytest-arg=--rootdir={t}"]


def seed(board, rows):
    return {"format": 1, "kind": "seed", "board": board, "source": "tests/hil/README.md", "date": "2026-01-01",
            "features": rows}


def statuses(common, board, t):
    st = bringup(*common, "list", "--radio", board, "--json", *collect_args(t))
    assert st.returncode == 0, st.stdout + st.stderr
    return {x["id"]: (x["status"], x["basis"]) for x in json.loads(st.stdout)}


def bringup(*args, extra_env=None):
    return subprocess.run([python311(), BRINGUP, *args], cwd=ROOT, env=dict(env(), **(extra_env or {})),
                          capture_output=True, text=True, timeout=300)


def git_checkout():
    """Whether the tree is a git work tree: a `git archive` export, as some benches run, has no history."""
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=ROOT, capture_output=True, text=True)
    return r.returncode == 0 and os.path.realpath(r.stdout.strip()) == os.path.realpath(ROOT)


def a_board():
    return sorted(os.path.basename(os.path.dirname(p)) for p in glob.glob(os.path.join(ROOT, "boards", "*",
                                                                                          "board.mk")))[0]


def test_bringup_without_git(tmp_path):
    """A tree that isn't a git checkout (GIT_DIR pointing nowhere stands in for a `git archive` export): status and
    list say staleness wasn't checked, and run refuses before anything runs, unless --no-record."""
    t = fake_tree(tmp_path)
    (t / "features.toml").write_text(FAKE_TOML)
    results, board = t / "results", a_board()
    (results / board).mkdir(parents=True)
    (results / board / "seed.json").write_text(json.dumps(seed(board, {"fake_split": {"result": "pass",
                                                                                        "commit": "0" * 40}})))
    nogit = {"GIT_DIR": str(t / "no-git-here")}
    common = ["--features", str(t / "features.toml"), "--results", str(results)]
    st = bringup(*common, "status", "--radio", board, extra_env=nogit)
    assert st.returncode == 0 and "not a git checkout" in st.stdout, st.stdout + st.stderr
    lst = bringup(*common, "list", "--radio", board, "--json", *collect_args(t), extra_env=nogit)
    assert lst.returncode == 0 and "not a git checkout" in lst.stderr, lst.stdout + lst.stderr
    rows = {x["id"]: (x["status"], x["basis"]) for x in json.loads(lst.stdout)}
    assert rows["fake_split"] == ("pass", "seed"), rows
    r = bringup(*common, "run", "--radio", board, "--lock", str(t / "radio.lock"), *collect_args(t), "--",
                "--radio-port", "/nonexistent/odr-fake-port", extra_env=nogit)
    assert r.returncode != 0 and "not a git checkout" in r.stderr and "--no-record" in r.stderr, r.stdout + r.stderr
    assert [p.name for p in (results / board).iterdir()] == ["seed.json"]


def test_bringup_run_records_and_status(tmp_path):
    if not git_checkout():
        skip("bench", "not a git checkout: the staleness check needs history")
    t = fake_tree(tmp_path)
    (t / "features.toml").write_text(FAKE_TOML)
    results, lock, board = t / "results", t / "radio.lock", a_board()
    common = ["--features", str(t / "features.toml"), "--results", str(results)]
    run = ["run", "--radio", board, "--lock", str(lock), *collect_args(t), "--", "--radio-port",
           "/nonexistent/odr-fake-port"]

    # the radio lock held by someone else: nothing runs
    with open(lock, "w") as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        r = bringup(*common, *run)
        assert r.returncode != 0 and "radio lock" in (r.stdout + r.stderr), r.stdout + r.stderr
    assert not results.exists()

    # before any run: nothing reads as unsupported, and a seed row stands on its own
    (results / board).mkdir(parents=True)
    # fake_split's row was made on the first commit, whose files differ from today's: it needs a re-run. fake_seeded's
    # paths (LICENSE) are as HEAD has them unless someone edits the license in their tree
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    root = subprocess.run(["git", "rev-list", "--max-parents=0", "HEAD"], cwd=ROOT, capture_output=True,
                          text=True).stdout.split()[0]
    (results / board / "seed.json").write_text(json.dumps(seed(board, {"fake_split": {"result": "pass",
                                                                                        "commit": root},
                                                                         "fake_seeded": {"result": "pass",
                                                                                         "commit": head}})))
    assert statuses(common, board, t) == {"fake_split": ("needs re-run", "seed"),
                                       "fake_ok": ("built, not yet recorded", None),
                                       "fake_new": ("built, not yet recorded", None),
                                       "fake_seeded": ("pass", "seed")}
    assert "rests on a seed record" in bringup(*common, "status", "--radio", board).stdout

    r = bringup(*common, *run)
    assert r.returncode == 1, r.stdout + r.stderr
    recs = [p for p in glob.glob(str(results / board / "*.json")) if not p.endswith("seed.json")]
    assert len(recs) == 1, recs
    with open(recs[0]) as f:
        rec = json.load(f)
    assert rec["kind"] == "run" and rec["board"] == board and re.fullmatch(r"[0-9a-f]{40}", rec["commit"])
    assert rec["date"].endswith("Z")
    assert {x["id"].split("::")[-1]: x["outcome"] for x in rec["tests"]} == {
        "test_gate": "failed", "test_passes": "passed", "test_extra": "skipped"}
    assert rec["features"] == ["fake_split", "fake_ok"] and set(rec["sources"]) == {"fake_split", "fake_ok"}
    assert all(re.fullmatch(r"[0-9a-f]{40}", h) for h in rec["sources"]["fake_ok"].values()), rec["sources"]
    assert rec["tools"]["pytest"] and "/nonexistent" not in json.dumps(rec)

    # the run's rows replace the seed's: fake_split failed on the radio, and the seed still vouches for fake_seeded
    assert statuses(common, board, t) == {"fake_split": ("fail", "run"), "fake_ok": ("pass", "run"),
                                       "fake_new": ("built, not yet recorded", None),
                                       "fake_seeded": ("pass", "seed")}

    # staleness is by content: a commit squash-merged away changes nothing while the files are the same
    rec["commit"] = "0" * 40
    with open(recs[0], "w") as f:
        json.dump(rec, f)
    assert statuses(common, board, t)["fake_ok"] == ("pass", "run")
    # files that differ from the record's hashes: a re-run, no downgrade, and a failure stays one
    for fid in rec["sources"]:
        rec["sources"][fid] = {p: "0" * 40 for p in rec["sources"][fid]}
    with open(recs[0], "w") as f:
        json.dump(rec, f)
    st = statuses(common, board, t)
    assert st["fake_ok"] == ("needs re-run", "run") and st["fake_split"] == ("fail", "run"), st
    shutil.rmtree(results)


def test_check_rejects_a_bad_seed_and_an_unknown_feature(tmp_path):
    t = fake_tree(tmp_path)
    (t / "features.toml").write_text(FAKE_TOML)
    results, board = t / "results", a_board()
    (t / "test_two_gates.py").write_text("import pytest\n\n\n@pytest.mark.feature('fake_ok', gate=True)\n"
                                        "def test_one():\n    pass\n\n\n@pytest.mark.feature('fake_ok', gate=True)\n"
                                        "def test_two():\n    pass\n")
    common = ["--features", str(t / "features.toml"), "--results", str(results), "check", *collect_args(t),
              "--tests", str(t / "test_two_gates.py")]
    (results / board).mkdir(parents=True)
    # a row's commit the repository doesn't have (a squash-merged branch) is not an error: the row stands
    good = seed(board, {"fake_ok": {"result": "pass", "source": "tests/hil/README.md", "commit": "0" * 40}})
    (results / board / "seed.json").write_text(json.dumps(good))
    r = bringup(*common)
    assert "seed.json" not in r.stdout + r.stderr, r.stdout + r.stderr
    bad = seed(board, {"fake_ok": {"result": "supported"}, "no_such_feature": {"result": "pass"}})
    bad["source"] = "docs/no-such-doc.md"
    (results / board / "seed.json").write_text(json.dumps(bad))
    out = bringup(*common)
    text = out.stdout + out.stderr
    assert out.returncode != 0
    for want in ("result is one of", "no_such_feature is not a checklist feature",
                 "source docs/no-such-doc.md does not exist",
                 "test_fake.py::test_keyed: marks feature fake_tx, which tests/hil/features.toml doesn't have",
                 "feature fake_ok has more than one capability gate"):
        assert want in text, text


def test_the_lock_follows_the_port(tmp_path):
    t = fake_tree(tmp_path)
    (t / "features.toml").write_text(FAKE_TOML)
    common = ["--features", str(t / "features.toml"), "--results", str(t / "results")]

    def lock(*pytest_args, **env_over):
        e = {k: v for k, v in env().items() if k not in ("ODR_RADIO_LOCK", "ODR_PORT")}
        e.update(HOME=str(t), **env_over)
        r = subprocess.run([python311(), BRINGUP, *common, "run", "--radio", a_board(), "--dry-run", "--feature",
                            "fake_ok", *collect_args(t), "--", *pytest_args], cwd=ROOT, env=e, capture_output=True,
                           text=True, timeout=300)
        assert r.returncode == 0, r.stdout + r.stderr
        return next(x for x in r.stdout.splitlines() if x.startswith("lock: "))[len("lock: "):]

    assert lock() == str(t / ".odr-radio-usb.lock")
    assert lock("--radio-port", "/dev/cu.one") == str(t / ".odr-radio-dev_cu.one.lock")
    assert lock("--radio-port=/dev/cu.two") == str(t / ".odr-radio-dev_cu.two.lock")
    assert lock(ODR_PORT="/dev/ttyS3") == str(t / ".odr-radio-dev_ttyS3.lock")
    assert lock("--radio-port", "/dev/cu.one", ODR_RADIO_LOCK=str(t / "bench-{port}.lock")) == str(
        t / "bench-dev_cu.one.lock")

