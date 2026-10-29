# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""pytest set-up for the hardware-in-the-loop tests (README.md): the options, the markers, and the boards' own
tests (boards/<id>/hil) added to a run of this directory. The hooks and fixtures (the radio, the SDR bench, the
restore after every test, the opt-in skips) are in hil_plugin.py."""
import glob
import os

import bringup_plugin
import hil_plugin

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def pytest_addoption(parser):
    g = parser.getgroup("rfoundry-hil", "RadioFoundry hardware-in-the-loop")
    g.addoption("--radio-port", default=os.environ.get("RFOUNDRY_PORT"),
                help="the radio's serial port (default: the only USB 1FC9:0094 device, or $RFOUNDRY_PORT)")
    g.addoption("--allow-rf", action="store_true",
                help="tests may transmit (the SDR bench's transmitter; the radio only in tests marked radio_tx)")
    g.addoption("--allow-service-stop", action="store_true",
                help="tests may stop the SDR bench's gateway service to use its transmitter and "
                     "receiver (dmr_bench.py's lease: always restarted). Not needed on a bench that runs the "
                     "shared bench daemon: nothing is stopped there")
    g.addoption("--allow-usb-switch", action="store_true",
                help="tests may switch the radio's USB personality to the network adapter (it re-enumerates; the "
                     "test switches it back over UDP), or on a radio without one run pppd on its serial port")
    g.addoption("--tx-power", type=int, default=40, help="bench transmitter RF level 0-255 (bench: low)")
    g.addoption("--dmr-freq", type=float, default=461.6875,
                help="MHz: the DMR test channel (Ch 15 of the bench's gateway configuration)")
    g.addoption("--dmr-cc", type=int, default=2)
    g.addoption("--sfr-freq", type=float, default=None,
                help="MHz: the SFR test channels' frequency (test_sfr_standalone); default --dmr-freq")
    g.addoption("--dmr-tg", type=int, default=9,
                help="talkgroup the bench transmitter calls (in the radio channel's RX group list)")
    g.addoption("--ambe", default=os.environ.get("RFOUNDRY_HIL_AMBE"),
                help="a 9-byte-frame .ambe speech recording on this Mac for test_dmr_voice ($RFOUNDRY_HIL_AMBE); never "
                     "commit recordings")
    g.addoption("--noaa-mhz", type=float, default=162.550, help="NOAA weather radio frequency for test_fm")
    g.addoption("--rssi-floor", type=int, default=-115, help="dBm: test_fm passes above this")
    g.addoption("--shots", default=None, help="directory for screenshots taken by the tests")
    g.addoption("--bench-wait-max", type=float, default=float(os.environ.get("RFOUNDRY_SDR_BENCH_WAIT_MAX", "900")),
                help="s: a test that would wait longer for a shared SDR bench (rfoundry-benchd) is skipped")
    g.addoption("--bench-station", default=os.environ.get("RFOUNDRY_BENCH_STATION"),
                help="a shared bench's station (a RadioFoundry radio on the bench host) as the second radio of the tests "
                     "that use one (bench_station fixture); rfoundry-bench info lists them")
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
    g.addoption("--codeplug-image", default=os.environ.get("RFOUNDRY_HIL_CODEPLUG_IMAGE"),
                help="the radio's codeplug as a file (env RFOUNDRY_HIL_CODEPLUG_IMAGE): the codeplug_write tests take it "
                     "as the backup instead of reading the radio, and restore and verify it afterwards")
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
    g.addoption("--split-rx-port", default=os.environ.get("RFOUNDRY_SPLIT_RX_PORT"),
                help="test_split_pair: the second radio's port, the RX half (the --radio-port radio is the TX half)")
    g.addoption("--split-ms-port", default=os.environ.get("RFOUNDRY_SPLIT_MS_PORT"),
                help="test_split_pair_handheld: a third radio, the subscriber calling into the pair (its current "
                     "DMR channel is edited for the run and put back)")
    g.addoption("--split-ms-ts", default="1,2", help="test_split_pair_handheld: the subscriber's slots, in turn")
    g.addoption("--sfr-talker-port", default=os.environ.get("RFOUNDRY_SFR_TALKER_PORT"),
                help="test_sfr_chain_rfoundry_user: a RadioFoundry radio as the SFR user that talks (a user-role channel on "
                     "--sfr-freq); the --radio-port radio is the repeater")
    g.addoption("--sfr-listener-port", default=os.environ.get("RFOUNDRY_SFR_LISTENER_PORT"),
                help="test_sfr_chain_rfoundry_user: a RadioFoundry radio as the SFR user that listens (a user-role channel on "
                     "--sfr-freq)")
    g.addoption("--sfr-listen-modes", default="0,1",
                help="test_sfr_chain_rfoundry_user: the listener's receive modes (setting sfr_rx), one call each")
    g.addoption("--peer-port", default=os.environ.get("RFOUNDRY_PEER_PORT"),
                help="a second RadioFoundry radio that receives from the --radio-port radio (test_zone_share's "
                     "receiver; its codeplug is backed up and put back)")
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
    g.addoption("--aprs-call", default=os.environ.get("RFOUNDRY_APRS_CALL"),
                help="test_aprs on air: the licensed operator's callsign ($RFOUNDRY_APRS_CALL), set as the RAM-only test "
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
    bringup_plugin.add_options(parser)


def pytest_configure(config):
    if config.getoption("--codeplug-image"):    # test_cps.codeplug_edited reads it from there
        os.environ["RFOUNDRY_HIL_CODEPLUG_IMAGE"] = os.path.expanduser(config.getoption("--codeplug-image"))
    # The hooks and fixtures are a plugin, not this conftest, so they also reach the boards' own tests
    # (boards/<id>/hil), which lie outside this directory
    config.pluginmanager.register(hil_plugin, "rfoundry-hil")
    config.pluginmanager.register(bringup_plugin.Plugin(config), "rfoundry-bringup")
    # A run of this whole directory also runs every board's own HIL tests; each skips other_board on other radios
    if any(os.path.abspath(str(a).split("::")[0]) == HERE for a in config.args):
        config.args += sorted(d for d in glob.glob(os.path.join(ROOT, "boards", "*", "hil"))
                              if glob.glob(os.path.join(d, "test_*.py")))
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
