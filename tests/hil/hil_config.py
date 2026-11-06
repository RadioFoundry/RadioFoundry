# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The test rig's own values: frequencies, color code, talkgroup, the operator's callsign and the frequencies
the rig may transmit on (README.md, "The rig's configuration"). None of them has a default in the repository: each
comes from the command line, else $RFOUNDRY_HIL_<NAME>, else the rig's TOML file (--hil-config, else
$RFOUNDRY_HIL_CONFIG, else ~/.config/rfoundry/hil.toml), whose keys are the option names with _ for -. A test that
needs a value no one gave skips (kind opt_in). With --allow-rf, every transmit frequency given must be in tx_mhz,
and the bench client refuses to key anywhere else."""
import os

import pytest

DEFAULT_FILE = "~/.config/rfoundry/hil.toml"
TX_TOL_MHZ = 1e-6

# option name -> (type, help, the radio or the bench transmits on it)
RIG = {
    "dmr-freq": (float, "MHz: the DMR test channel (simplex, the radio's channel or VFO on it)", True),
    "dmr-cc": (int, "the DMR test channel's color code", False),
    "dmr-tg": (int, "the talkgroup the bench transmitter calls (in the radio channel's RX group list)", False),
    "fm-mhz": (float, "MHz: the FM test frequency (the radio's VFO)", True),
    "noaa-mhz": (float, "MHz: a NOAA weather radio station heard at the rig (test_fm; receive only)", False),
    "aprs-test-mhz": (float, "MHz: test_aprs's AFSK frequency, an amateur simplex frequency that is not an APRS "
                             "frequency", True),
    "aprs-dmr-dst": (int, "test_aprs DMR position: the destination ID", False),
    "callsign": (str, "the licensed operator's callsign: everything the tests send that carries one (APRS, CW ID, "
                      "talker alias), RAM only on the radio", False),
    "microphone": (str, "an ALSA capture device (arecord -D, e.g. plughw:1,0) whose microphone is at the radio's "
                        "speaker: the speaker tests (test_speaker.py)", False),
    "tx-mhz": (str, "MHz, comma separated: the frequencies this rig may transmit on (the radio and the bench); "
                    "--allow-rf needs it", False),
}
# the transmit frequencies among the other options (None when not given)
TX_OPTIONS = ("dmr-freq", "fm-mhz", "aprs-test-mhz", "sfr-freq", "roam-rx", "roam-tx", "split-out-mhz",
              "split-tx-mhz", "outbound-mhz", "am-mhz")


def add_options(g):
    g.addoption("--hil-config", default=None, metavar="TOML",
                help=f"the rig's values (default $RFOUNDRY_HIL_CONFIG, else {DEFAULT_FILE} when it exists)")
    for name, (_, text, _) in RIG.items():
        g.addoption(f"--{name}", default=None, help=f"{text} (or ${env_name(name)}, or {key(name)} in the rig's file)")


def env_name(name):
    return "RFOUNDRY_HIL_" + name.upper().replace("-", "_")


def key(name):
    return name.replace("-", "_")


def _file(config):
    path = config.getoption("--hil-config") or os.environ.get("RFOUNDRY_HIL_CONFIG")
    explicit = path is not None
    path = os.path.expanduser(path or DEFAULT_FILE)
    if not os.path.exists(path):
        if explicit:
            raise pytest.UsageError(f"the rig's file {path} doesn't exist")
        return {}, None
    try:
        import tomllib
    except ImportError:         # Python before 3.11
        raise pytest.UsageError(f"reading {path} needs Python 3.11 or later (tomllib)")
    with open(path, "rb") as f:
        try:
            return tomllib.load(f), path
        except tomllib.TOMLDecodeError as e:
            raise pytest.UsageError(f"the rig's file {path}: {e}")


def _parse(name, v, typ, where):
    if name == "tx-mhz":
        items = v if isinstance(v, (list, tuple)) else str(v).replace(",", " ").split()
        try:
            return [float(x) for x in items]
        except ValueError:
            raise pytest.UsageError(f"{name} from {where}: {v!r} is not a list of MHz")
    try:
        return typ(v)
    except (TypeError, ValueError):
        raise pytest.UsageError(f"{name} from {where}: {v!r} is not a {typ.__name__}")


CONFIG = None   # the run's pytest config, once configure() ran


def configure(config):
    """Fill every rig option from the environment or the file, then check the transmit frequencies."""
    global CONFIG
    CONFIG = config
    data, path = _file(config)
    for k in data:
        # exactly key(name): a hyphenated key would pass a looser test and then never be read
        if k not in {key(n) for n in RIG} | {key(n) for n in TX_OPTIONS}:
            raise pytest.UsageError(f"{path}: unknown key {k!r} (keys: {', '.join(key(n) for n in RIG)}, "
                                    f"{', '.join(key(n) for n in TX_OPTIONS if n not in RIG)})")
    for name in list(RIG) + [n for n in TX_OPTIONS if n not in RIG]:
        dest = key(name)
        typ = RIG[name][0] if name in RIG else float
        v = getattr(config.option, dest, None)
        where = f"--{name}"
        if v is None and os.environ.get(env_name(name)):
            v, where = os.environ[env_name(name)], f"${env_name(name)}"
        if v is None and dest in data:
            v, where = data[dest], f"{path}"
        setattr(config.option, dest, None if v is None else _parse(name, v, typ, where))
    if config.option.callsign is not None:
        config.option.callsign = config.option.callsign.strip().upper()
    if config.getoption("--allow-rf"):
        allowed = config.option.tx_mhz or []
        given = {n: getattr(config.option, key(n)) for n in TX_OPTIONS if getattr(config.option, key(n)) is not None}
        if given and not allowed:
            raise pytest.UsageError("--allow-rf: give the frequencies this rig may transmit on (--tx-mhz, "
                                    f"${env_name('tx-mhz')} or tx_mhz in the rig's file)")
        bad = {n: f for n, f in given.items() if not tx_allowed(config, f)}
        if bad:
            raise pytest.UsageError("transmit frequencies not in tx_mhz: "
                                    + ", ".join(f"--{n} {f:g}" for n, f in bad.items()))


def tx_allowed(config, mhz):
    """mhz is one of the rig's transmit frequencies (tx_mhz); config None: this run's"""
    config = config or CONFIG
    return config is not None and any(abs(mhz - a) < TX_TOL_MHZ for a in (config.option.tx_mhz or []))


def run_tx_mhz(config=None):
    """The run's own transmit frequencies (MHz, sorted, each once): the TX_OPTIONS given, with --allow-rf (they are
    in tx_mhz, configure() checked); none without it. The firmware's test TX limit allows only these (Radio.tx_gate)"""
    config = config or CONFIG
    if config is None or not config.getoption("--allow-rf"):
        return []
    return sorted({round(float(f), 6) for f in (getattr(config.option, key(n), None) for n in TX_OPTIONS)
                   if f is not None})


def rig(config, name, what=None):
    """A rig option's value, or skip (opt_in) naming the three ways to give it"""
    v = config.getoption(f"--{name}")
    if v is None:
        from rfoundry_hil import skip
        skip("opt_in", f"{what or 'needs'} --{name} (or ${env_name(name)}, or {key(name)} in the rig's file: "
                       "tests/hil/README.md, \"The rig's configuration\")")
    return v
