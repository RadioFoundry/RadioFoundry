# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""pytest set-up for the hardware-in-the-loop tests (README.md): finds the radio by its USB ID, skips cleanly when
it isn't there, restores the radio's state after every test, and keeps transmitting and stopping the SDR bench's
gateway service behind opt-in flags."""
import os

import pytest

from odr_hil import DebugLinkError, Radio, SdrBench, find_ports


def pytest_addoption(parser):
    g = parser.getgroup("odr-hil", "OpenDigitalRadio hardware-in-the-loop")
    g.addoption("--radio-port", default=os.environ.get("ODR_PORT"),
                help="the radio's serial port (default: the only USB 1FC9:0094 device, or $ODR_PORT)")
    g.addoption("--allow-rf", action="store_true",
                help="tests may transmit (the SDR bench's transmitter; the radio only in tests marked radio_tx)")
    g.addoption("--allow-service-stop", action="store_true",
                help="tests may stop the SDR bench's gateway service to use its transmitter and "
                     "receiver (dmr_bench.py's lease: always restarted). Not needed on a bench that runs the "
                     "shared bench daemon: nothing is stopped there")
    g.addoption("--allow-usb-switch", action="store_true",
                help="tests may switch the radio's USB personality to the network adapter (it re-enumerates; the "
                     "test switches it back over UDP)")
    g.addoption("--tx-power", type=int, default=40, help="bench transmitter RF level 0-255 (bench: low)")
    g.addoption("--dmr-freq", type=float, default=461.6875,
                help="MHz: the DMR test channel (Ch 15 of the bench's gateway configuration)")
    g.addoption("--dmr-cc", type=int, default=2)
    g.addoption("--dmr-tg", type=int, default=9,
                help="talkgroup the bench transmitter calls (in the radio channel's RX group list)")
    g.addoption("--ambe", default=os.environ.get("ODR_HIL_AMBE"),
                help="a 9-byte-frame .ambe speech recording on this Mac for test_dmr_voice ($ODR_HIL_AMBE); never "
                     "commit recordings")
    g.addoption("--noaa-mhz", type=float, default=162.550, help="NOAA weather radio frequency for test_fm")
    g.addoption("--rssi-floor", type=int, default=-115, help="dBm: test_fm passes above this")
    g.addoption("--shots", default=None, help="directory for screenshots taken by the tests")
    g.addoption("--bench-wait-max", type=float, default=float(os.environ.get("ODR_SDR_BENCH_WAIT_MAX", "900")),
                help="s: a test that would wait longer for a shared SDR bench (benchd) is skipped")
    g.addoption("--bench-station", default=os.environ.get("ODR_BENCH_STATION"),
                help="a shared bench's station (an ODR radio on the bench host) as the second radio of the tests "
                     "that use one (bench_station fixture); odr-bench info lists them")
    g.addoption("--sdr-gain", type=float, default=5.0, help="RTL-SDR gain for raw captures (dmr_bench.py C0 picks it)")
    g.addoption("--sdr-ppm", type=float, default=None,
                help="the SDR's own frequency error in ppm for FM carrier offsets (default: the SDR bench's calibration, "
                     "recorded with each capture)")
    g.addoption("--max-foff", type=float, default=500.0, help="Hz: largest carrier offset the TX tests accept")
    g.addoption("--fm-mhz", type=float, default=461.6875,
                help="MHz: the FM TX test frequency (the radio's VFO; a channel you may transmit on)")
    g.addoption("--allow-codeplug-write", action="store_true",
                help="tests may write the radio's codeplug over CPS (marker codeplug_write: backed up first, "
                     "restored and verified afterwards)")
    g.addoption("--codeplug-backup-dir", default=None,
                help="keep the codeplug backups the codeplug_write tests take here (default: the test's tmp "
                     "directory, which pytest prunes after a few runs); outside the repository")
    g.addoption("--manual", action="store_true",
                help="a person is at the radio: run the tests that ask for something done by hand (marker manual)")
    g.addoption("--mic-inject", action="store_true",
                help="test_mic_uart_inject may send test tones on the radio's serial line at a low baud rate (its "
                     "programming cable's line reaches the mic input on some radios); with --allow-rf")
    g.addoption("--mic-inject-any", action="store_true",
                help="test_mic_uart_inject on a board without a measured cable-to-mic path")
    g.addoption("--allow-reboot", action="store_true",
                help="tests may restart the radio (marker reboot: the settings persistence check)")
    g.addoption("--roam-rx", type=float, default=None,
                help="MHz: test_roaming's bench repeater site as the radio sees it, the radio's receive frequency "
                     "(bench_repeater.py --tx); no default: a pair you are licensed for")
    g.addoption("--roam-tx", type=float, default=None,
                help="MHz: the radio's transmit frequency of that site (bench_repeater.py --rx)")
    g.addoption("--split-rx-port", default=os.environ.get("ODR_SPLIT_RX_PORT"),
                help="test_split_pair: the second radio's port, the RX half (the --radio-port radio is the TX half)")
    g.addoption("--split-ms-port", default=os.environ.get("ODR_SPLIT_MS_PORT"),
                help="test_split_pair_handheld: a third radio, the subscriber calling into the pair (its current "
                     "DMR channel is edited for the run and put back)")
    g.addoption("--split-ms-ts", default="1,2", help="test_split_pair_handheld: the subscriber's slots, in turn")
    g.addoption("--split-out-mhz", type=float, default=None,
                help="MHz: test_split_pair, the repeater output the TX half sends on (its input is --dmr-freq, where "
                     "the bench calls); no default: one you may transmit on, inside the SDR's shared window")
    g.addoption("--split-tx-mhz", type=float, default=None,
                help="MHz: test_dmr_tx_split_repeater, the radio's TX frequency on its DMR test channel made split "
                     "(the bench repeater's input; its output is --dmr-freq); no default: one you may transmit on")
    g.addoption("--split-calls", type=int, default=1,
                help="test_dmr_tx_split_repeater: key up this many times in one run (the split is written and "
                     "restored once; every call needs a fresh repeater start)")
    g.addoption("--roam-beacon", type=float, default=20.0,
                help="s: test_roaming's beacon tests, the bench repeater's beacon interval (set on the radio too)")
    g.addoption("--roam-beacon-len", type=int, default=3,
                help="s: their beacon duration (bench_repeater.py --beacon-len, the radio's roam_beacon_len_s)")
    g.addoption("--aprs-call", default=os.environ.get("ODR_APRS_CALL"),
                help="test_aprs on air: the licensed operator's callsign ($ODR_APRS_CALL), set as the RAM-only test "
                     "call (never saved)")
    g.addoption("--aprs-ssid", type=int, default=15, help="test_aprs on air: the SSID sent (restored afterwards)")
    g.addoption("--aprs-test-mhz", type=float, default=446.050,
                help="MHz: test_aprs AFSK frequency, a 2 m or 70 cm amateur simplex frequency that is not an APRS "
                     "frequency (test_aprs.py docstring); never 144.390")
    g.addoption("--aprs-dmr-dst", type=int, default=9990301,
                help="test_aprs DMR position: the destination ID (the bench modem listens to any)")
    g.addoption("--mic-gain-steps", default="3,5,7",
                help="test_mic_gain: the Mic gain steps (0-10, 2 dB a step, 5 = the board's level) keyed at each "
                     "bandwidth, one over per step")
    g.addoption("--mic-gain-window", type=float, default=30.0,
                help="test_mic_gain: seconds the SDR bench records per over (unplug, key, talk, release in it)")


def pytest_configure(config):
    config.addinivalue_line("markers", "rf: transmits (needs --allow-rf)")
    config.addinivalue_line("markers", "bench_service: uses the SDR bench's transmitter / receiver through the "
                            "lease (a bench without a shared bench daemon stops its gateway service for it: "
                            "needs --allow-service-stop, else the test skips when it takes the lease)")
    config.addinivalue_line("markers", "sdr_rx: needs the SDR bench's receiver (the RTL-SDR) and no bench modem; a "
                            "bench without one skips it. With bench_modem on the same test: needs both")
    config.addinivalue_line("markers", "bench_modem: needs the SDR bench's modem or a bench station (the MMDVM "
                            "board that transmits to the radio, listens to it or acts as a repeater) and no SDR; "
                            "skips without one. With sdr_rx on the same test: needs both")
    config.addinivalue_line("markers", "sdr_or_modem: needs the SDR or the bench modem, either will do (the test "
                            "accepts what one of them heard); skips only when the bench has neither")
    config.addinivalue_line("markers", "radio_tx: the radio under test transmits (needs --allow-rf)")
    config.addinivalue_line("markers", "codeplug_write: writes the radio's codeplug over CPS, backed up and restored "
                            "(needs --allow-codeplug-write)")
    config.addinivalue_line("markers", "reboot: restarts the radio (needs --allow-reboot)")
    config.addinivalue_line("markers", "usb_switch: switches the USB personality (needs --allow-usb-switch)")
    config.addinivalue_line("markers", "manual: asks a person at the radio to do something (needs --manual and -s)")
    config.addinivalue_line("markers", "audio_smoke: the DMR audio smoke tests (test_dmr_audio_smoke.py), part of every "
                            "bench pass")


def pytest_collection_modifyitems(config, items):
    for item in items:
        if ("rf" in item.keywords or "radio_tx" in item.keywords) and not config.getoption("--allow-rf"):
            if "audio_smoke" in item.keywords:
                item.add_marker(pytest.mark.skip(reason="DMR AUDIO NOT CHECKED: transmits; run the bench pass with "
                                                        "--allow-rf --ambe FILE (README.md, Default bench pass)"))
            else:
                item.add_marker(pytest.mark.skip(reason="transmits: run with --allow-rf"))
        elif "usb_switch" in item.keywords and not config.getoption("--allow-usb-switch"):
            item.add_marker(pytest.mark.skip(reason="re-enumerates the radio as a network adapter: run with "
                                                    "--allow-usb-switch"))
        elif "codeplug_write" in item.keywords and not config.getoption("--allow-codeplug-write"):
            item.add_marker(pytest.mark.skip(reason="writes the codeplug: run with --allow-codeplug-write"))
        elif "reboot" in item.keywords and not config.getoption("--allow-reboot"):
            item.add_marker(pytest.mark.skip(reason="restarts the radio: run with --allow-reboot"))
        elif "manual" in item.keywords and not config.getoption("--manual"):
            item.add_marker(pytest.mark.skip(reason="needs a person at the radio: run with --manual -s"))


@pytest.fixture(scope="session")
def radio(pytestconfig):
    port = pytestconfig.getoption("--radio-port")
    if not port and not find_ports():
        pytest.skip("no radio on USB (1FC9:0094): plug it in or give --radio-port (DMR audio not checked either)")
    try:
        r = Radio(port)
    except DebugLinkError as e:
        pytest.skip(f"radio not reachable: {e}")
    try:
        r.ping()
    except DebugLinkError as e:
        r.close()
        pytest.skip(f"the radio doesn't answer the debug link (firmware without CONFIG_DEBUGLINK?): {e}")
    yield r
    r.close()


@pytest.fixture(autouse=True)
def restore_radio(request):
    """Every test that uses the radio leaves it as it found it (channel / VFO, zone, power, squelch, monitor)."""
    # test_tcp_services.py's module fixture "net" has the radio on the network (its serial link closed) for the whole
    # module and puts it back on serial itself: nothing to snapshot over serial then
    if "radio" not in request.fixturenames or "net" in request.fixturenames:
        yield
        return
    r = request.getfixturevalue("radio")
    snap = r.snapshot()
    try:
        yield
    finally:
        try:
            r.link.ptt(False)       # whatever happened, no PTT held
        except DebugLinkError:
            pass
        r.restore(snap)


@pytest.fixture(scope="session")
def sdr_bench(pytestconfig):
    b = SdrBench(allow_rf=pytestconfig.getoption("--allow-rf"),
                 allow_service_stop=pytestconfig.getoption("--allow-service-stop"),
                 tx_power=pytestconfig.getoption("--tx-power"), gain=pytestconfig.getoption("--sdr-gain"),
                 wait_max=pytestconfig.getoption("--bench-wait-max"))
    if not b.reachable():
        pytest.skip(f"SDR bench {b.host} not reachable over ssh, or its tools missing (tools/bench/dmr_bench.py "
                    "sync): no bench test runs, the DMR audio smoke neither")
    yield b
    b.remove_run_dirs()


@pytest.fixture(autouse=True)
def bench_hardware(request):
    """Skips a test marked sdr_rx / bench_modem / sdr_or_modem when the SDR bench lacks that piece (SdrBench.lacks).
    Unmarked tests need neither and don't touch the bench here."""
    kw = request.node.keywords
    sdr, modem, either = "sdr_rx" in kw, "bench_modem" in kw, "sdr_or_modem" in kw
    if not (sdr or modem or either):
        yield
        return
    if "radio" in request.fixturenames:
        request.getfixturevalue("radio")        # no radio: skip for that first, without asking the bench
    bench = request.getfixturevalue("sdr_bench")
    why = bench.lacks(sdr=sdr, modem=modem, either=either)
    if why:
        pytest.skip(why)
    yield


@pytest.fixture(scope="session")
def bench_station(pytestconfig, sdr_bench):
    """The name of the bench station to use as the second radio (--bench-station); skips without one."""
    name = pytestconfig.getoption("--bench-station")
    if not name:
        pytest.skip("no bench station: give --bench-station NAME (odr-bench info on the bench lists them)")
    st = next((s for s in sdr_bench.info().get("stations", []) if s["name"] == name), None)
    if st is None or not st.get("present"):
        pytest.skip(f"bench station {name} is not on the bench (or the bench has no benchd)")
    return name


@pytest.fixture
def shots(pytestconfig, tmp_path):
    d = pytestconfig.getoption("--shots")
    if d:
        os.makedirs(d, exist_ok=True)
        return d
    return str(tmp_path)


@pytest.fixture
def backup_dir(pytestconfig, tmp_path):
    """Where codeplug_write tests keep their codeplug backups (--codeplug-backup-dir, else the test's tmp dir)."""
    return pytestconfig.getoption("--codeplug-backup-dir") or str(tmp_path)
