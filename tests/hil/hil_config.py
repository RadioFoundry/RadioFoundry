# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The test rig's own values: frequencies, color code, talkgroup, the operator's callsign and the frequencies
the rig may transmit on (README.md, "The rig's configuration"). None of them has a default in the repository: each
comes from the command line, else $RFOUNDRY_HIL_<NAME>, else the rig's TOML file (--hil-config, else
$RFOUNDRY_HIL_CONFIG, else ~/.config/rfoundry/hil.toml), whose keys are the option names with _ for -. A test that
needs a value no one gave skips (kind opt_in). With --allow-rf, every transmit frequency given must be in tx_mhz,
and the bench client refuses to key anywhere else.

The file's [board.<id>] table (the id in boards/) holds one radio's run: with --hil-board <id> (tools/bringup.py run
passes its --radio) its values come before the file's top level, and it may also give any other option that takes a
value (--bench-api, --split-tx-mhz, --sfr-freq ...), so every run of that radio gets the same ones. The command line
and the environment still come first. A flag (the --allow-* ones) stays on the command line: a file never grants a
transmission."""
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
    "vhf-fm-mhz": (float, "MHz: a VHF FM frequency the bench transmits on (test_fm: receive in the VHF band)", True),
    "aprs-test-mhz": (float, "MHz: test_aprs's AFSK frequency, an amateur simplex frequency that is not an APRS "
                             "frequency", True),
    "aprs-dmr-dst": (int, "test_aprs DMR position: the destination ID", False),
    "callsign": (str, "the licensed operator's callsign: everything the tests send that carries one (APRS, CW ID, "
                      "talker alias), RAM only on the radio", False),
    "microphone": (str, "an ALSA capture device (arecord -D, e.g. plughw:1,0) whose microphone is at the radio's "
                        "speaker: the speaker tests (test_speaker.py)", False),
    "mic-processed": (int, "1: the microphone has processing of its own that alters a tone's length and the silences "
                           "(a headset's noise suppression): the speaker tests check each tone's start, and the "
                           "radio's stream counters for the rest", False),
    "wattmeter-mhz": (str, "MHz, comma separated: where the wattmeter check keys every power level (test_fm_tx "
                           "--manual; default 445,146, the middle of where hams transmit on 70 cm and 2 m); each "
                           "must be in tx_mhz", True),
    "tx-mhz": (str, "MHz, comma separated: the frequencies this rig may transmit on (the radio and the bench); "
                    "--allow-rf needs it", False),
}
# the transmit frequencies among the other options (None when not given)
TX_OPTIONS = ("dmr-freq", "fm-mhz", "aprs-test-mhz", "sfr-freq", "roam-rx", "roam-tx", "split-out-mhz",
              "split-tx-mhz", "outbound-mhz", "am-mhz", "vhf-fm-mhz", "repeater-in-mhz")


def add_options(g):
    g.addoption("--hil-config", default=None, metavar="TOML",
                help=f"the rig's values (default $RFOUNDRY_HIL_CONFIG, else {DEFAULT_FILE} when it exists)")
    g.addoption("--hil-board", default=os.environ.get("RFOUNDRY_HIL_BOARD"), metavar="ID",
                help="the board whose [board.<id>] table in the rig's file applies (default $RFOUNDRY_HIL_BOARD; "
                     "tools/bringup.py run passes its --radio)")
    for name, (_, text, _) in RIG.items():
        g.addoption(f"--{name}", default=None, help=f"{text} (or ${env_name(name)}, or {key(name)} in the rig's file)")


def env_name(name):
    return "RFOUNDRY_HIL_" + name.upper().replace("-", "_")


def key(name):
    return name.replace("-", "_")


def _file(config):
    path = config.getoption("--hil-config") or os.environ.get("RFOUNDRY_HIL_CONFIG")
    if path == "none":          # no rig file (tools/ci.sh: the checks don't depend on the rig)
        return {}, None
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
    boards = data.pop("board", {})
    for k in data:
        # exactly key(name): a hyphenated key would pass a looser test and then never be read
        if k not in {key(n) for n in RIG} | {key(n) for n in TX_OPTIONS}:
            raise pytest.UsageError(f"{path}: unknown key {k!r} (keys: {', '.join(key(n) for n in RIG)}, "
                                    f"{', '.join(key(n) for n in TX_OPTIONS if n not in RIG)}, [board.<id>])")
    board = config.getoption("--hil-board")
    own = dict(boards.get(board, {})) if board else {}
    for k, v in list(own.items()):
        if k in {key(n) for n in RIG} | {key(n) for n in TX_OPTIONS}:
            data[k] = v         # the board's value before the file's top level
            continue
        if k == "radio_port":
            raise pytest.UsageError(f"{path} [board.{board}]: radio_port: ports move when cables do, give it on the "
                                    "command line")
        cur = getattr(config.option, k, None) if hasattr(config.option, k) else KeyError
        if cur is KeyError or isinstance(cur, bool) or isinstance(v, bool):
            why = "a flag stays on the command line" if cur is not KeyError else "no such option"
            raise pytest.UsageError(f"{path} [board.{board}]: {k!r}: {why}")
        if cur is None:         # not given on the command line or by its environment variable
            setattr(config.option, k, v)
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


WATTMETER_MHZ = (445.0, 146.0)     # the middle of where hams transmit on 70 cm and 2 m


def wattmeter_mhz(config=None):
    """The wattmeter check's frequencies (MHz): --wattmeter-mhz, else WATTMETER_MHZ"""
    config = config or CONFIG
    v = getattr(config.option, key("wattmeter-mhz"), None) if config is not None else None
    return [float(x) for x in str(v).replace(",", " ").split()] if v else list(WATTMETER_MHZ)


def run_tx_mhz(config=None):
    """The run's own transmit frequencies (MHz, sorted, each once): the TX_OPTIONS given, with --allow-rf (they are
    in tx_mhz, configure() checked), and with --manual the wattmeter check's that are in tx_mhz; none without
    --allow-rf. The firmware's test TX limit allows only these (Radio.tx_gate)"""
    config = config or CONFIG
    if config is None or not config.getoption("--allow-rf"):
        return []
    out = {round(float(f), 6) for f in (getattr(config.option, key(n), None) for n in TX_OPTIONS) if f is not None}
    if config.getoption("--manual"):
        out |= {round(f, 6) for f in wattmeter_mhz(config) if tx_allowed(config, f)}
    return sorted(out)


def rig(config, name, what=None):
    """A rig option's value, or skip (opt_in) naming the three ways to give it"""
    v = config.getoption(f"--{name}")
    if v is None:
        from rfoundry_hil import skip
        skip("opt_in", f"{what or 'needs'} --{name} (or ${env_name(name)}, or {key(name)} in the rig's file: "
                       "tests/hil/README.md, \"The rig's configuration\")")
    return v
