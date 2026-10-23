# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The microphone path on DMR transmit without a person: a test tone through the programming cable's serial line.

On some radios the programming cable's host-to-radio line (the radio's UART RX) reaches the microphone input: a byte
pattern sent at a low baud rate turns into an audio-band square wave at the codec ADC. The test keys the radio on a
DMR channel through the debug link (with the firmware's own PTT time-out, so nothing has to be sent to release it),
switches the host's port to INJECT_BAUD, sends tone bursts of 0x55 (alternating bits: a square at INJECT_BAUD / 2 Hz)
with idle gaps between them, switches back to 115200 and reads the voice layer's mic statistics
(`voice.tx_mic_level_db`, `voice.tx_mic_spread_db`, docs/spec/debuglink.md): the bursts must stand out from the
gaps the way syllables do. A mic route that never reaches the vocoder (the DM-1801 Pro's DMR transmit with PTB14 low,
2026-10-22) leaves both overs at the ADC's floor.

Opt-in: `--allow-rf --mic-inject`, the radio on a local serial port (the baud switch and the burst timing don't
survive a network serial bridge), nothing else on that port. Boards outside INJECT_BOARDS (the DM-1801 Pro, DM-32UV
and DB25-D are in it) skip with the reason; `--mic-inject-any` tries any board. The test also checks that the debug link answers again after the odd-baud bytes
and that the radio isn't left keyed. That the overs go out on air is test_dmr_tx.py's job; the SDR bench has no
vocoder, so the content check is the radio's own.
"""
import time

import pytest

from odr_debug import DebugLinkError
from odr_hil import skip

INJECT_BAUD = 6000          # 0x55 at 6000 baud: a 3 kHz square, inside the vocoder's band. The coupling grows with
                            # the edge rate: on the DM-1801 Pro 1200 baud rose 3 dB, 2400 baud 9, 6000 baud 15
BURST_S = 0.2               # tone, then as long idle: 2.5 bursts a second, a syllable rate
BURSTS = 8
KEY_UP_S = 0.8              # the over is on air and the voice stream runs before the first burst
# Boards where the cable's line reaches the mic input (the owner: all three), and what was measured
INJECT_BOARDS = {
    "DM-1801 Pro": "2026-10-22: 1200 Hz and 3600 Hz lines at the vocoder input at 2400 baud; 6000 baud 15 dB over "
                   "a quiet over",
    "DM-32UV": "2026-10-22: quiet over level 3 dB, spread 0, zc 20; injected level 21, spread 18, zc 20-28 "
               "(18 dB over the quiet over)",
    "DB25-D": "2026-10-22: 6000 baud, 3 runs: quiet over level 6 dB, spread 3; injected level 36, spread 33 "
              "(30 dB over the quiet over)",
}
SPREAD_MIN = 12             # dB between burst and gap blocks (the DM-1801 Pro 15, DM-32UV 18, DB25-D 33)
RISE_MIN = 9                # dB the burst blocks reach over the quiet over's loud blocks (DM-1801 Pro 15, DM-32UV 18, DB25-D 30)


@pytest.fixture
def inject_port(radio, pytestconfig):
    if not pytestconfig.getoption("--mic-inject"):
        skip("opt_in", "drives the radio's serial line with test tones: run with --mic-inject (and --allow-rf)")
    port = str(radio.link.port)
    if "://" in port or port.startswith("udp:"):
        skip("bench", f"the radio is on {port}: the injection needs a local serial port (run on the host the cable "
                    "is plugged into, by its device path)")
    board = radio.version().get("board", "")
    if board not in INJECT_BOARDS and not pytestconfig.getoption("--mic-inject-any"):
        skip("no_hw", f"no known path from the programming cable's line to the mic input on the {board!r} "
                    "(--mic-inject-any tries anyway)")
    return radio.link.s


def resync(radio, timeout=3.0):
    """The debug link answers again after the odd-baud bytes (the parser drops the garbage and resyncs)."""
    radio.link.s.reset_input_buffer()
    radio.link.buf.clear()
    deadline, last = time.monotonic() + timeout, None
    while time.monotonic() < deadline:
        try:
            radio.ping()
            return
        except (DebugLinkError, OSError) as e:
            last = e
            time.sleep(0.2)
    raise AssertionError(f"the debug link didn't answer within {timeout:g} s after the injection: {last}")


def over(radio, s, inject):
    """One DMR over of KEY_UP_S + 2 x BURSTS x BURST_S, bursts on the line if inject; the voice layer's mic stats."""
    hold = KEY_UP_S + 2 * BURSTS * BURST_S + 0.3
    radio.link.ptt(True, int(hold * 1000))     # the firmware lets go by itself: nothing to send mid-over
    t0 = time.monotonic()
    time.sleep(KEY_UP_S)
    if inject:
        s.baudrate = INJECT_BAUD
        burst = b"\x55" * int(INJECT_BAUD / 10 * BURST_S)
        try:
            for _ in range(BURSTS):
                s.write(burst)
                s.flush()                       # drained: the burst is on the line, then the gap
                time.sleep(BURST_S)
        finally:
            s.baudrate = 115200
    time.sleep(max(0.0, hold - (time.monotonic() - t0)) + 0.4)
    resync(radio)
    radio.link.ptt(False)
    radio.wait_for(lambda v: v.get("state") != "tx", timeout=3.0, what="the radio back from transmit")
    v = radio.view(["dmr"])
    return {k: int(v.get(f"voice.tx_mic_{k}") or 0) for k in ("level_db", "spread_db", "zc", "blocks")}


@pytest.mark.radio_tx
def test_mic_uart_inject(radio, inject_port, pytestconfig):
    f = radio.view(["dmr"])
    if "voice.tx_mic_level_db" not in f:
        skip("not_built", "this build has no voice.tx_mic_* statistics")
    radio.find_channel(int(round(pytestconfig.getoption("--dmr-freq") * 1e6)), mode="dmr")
    radio.lowest_power()
    quiet = over(radio, inject_port, inject=False)
    tone = over(radio, inject_port, inject=True)
    print(f"\nquiet over {quiet}\ninjected over {tone}")
    assert quiet["blocks"] > 25 and tone["blocks"] > 25, "the voice stream didn't run for the overs"
    bad = []
    if tone["spread_db"] < SPREAD_MIN:
        bad.append(f"the tone bursts didn't stand out from the gaps: spread {tone['spread_db']} dB < {SPREAD_MIN}")
    if tone["level_db"] < quiet["level_db"] + RISE_MIN:
        bad.append(f"the bursts reached {tone['level_db']} dB, the quiet over {quiet['level_db']} dB: less than "
                   f"{RISE_MIN} dB more")
    assert not bad, ("the injected tone didn't reach the vocoder (mic route, codec ADC mute or gain): " +
                     "; ".join(bad))
