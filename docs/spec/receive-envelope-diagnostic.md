<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Receive-envelope diagnostic

`CONFIG_RX_ENVELOPE_DIAG` is an opt-in experiment, off by default. It provides experimental AM audio and does not set
`CAP_AM_RX`. A supporting board substitutes its analog RSSI signal for the FM PCM input and may expose an
experimental receive range. Channels in this range must refuse transmit independently of saved settings, PTT,
or network clients, even with an in-band transmit offset. Normal supported channels retain their existing transmit
checks and audio source. See the board's hardware page for wiring and range.

The capture retains the existing DC removal, sample-rate conversion and PCM transport. RSSI may be logarithmic,
filtered, or changed by AGC: nonzero samples or a tuning-state update do not prove demodulation, sensitivity or PLL
lock. Establish reception with recognizable speech from a known station, and compare an adjacent frequency.

Use FM mode, wide bandwidth, squelch zero and monitor on. Monitor opens the local audio path but does not force
the receive state used by the network audio gate; squelch zero is needed for continuous host recordings.
The diagnostic PCM is available through the receive side of
[USRP](usb-audio.md#91-usrp-on-the-network-config_net_usrp) and the live speaker monitor below.
The current experiment does not change IF gain or AGC registers.

The board view reports compiled transmit support in `board.tx_built`, `board.envelope_diag=1`, and `board.envelope_samples`, the capture's
sample count. The count must advance during a PCM session. Hardware observations must distinguish empty audio,
transport loss, and an unrecognized/noisy waveform from confirmed AM speech.

The host RF tests verify the experimental range, unchanged transmit frequency plan, and refusal of normal and
direct PA requests on experimental channels even with the board revision confirmed, plus normal-band transmit
in the mocked sequencer. Both transmit-enabled and globally receive-only builds are tested. The HIL check is passive and never keys a transmitter.

## Live speaker monitor

Boards advertising `AUDIO_TAP_RX_ENVELOPE` can play the stream input locally in this diagnostic. When receive audio
is open, the audio mux keeps sampling even with no host connection and selects the PCM speaker route. It applies
a 200 Hz second-order high-pass and 2400 Hz fourth-order low-pass, with limited gain; the host's input recording stays unmodified. Beeps and
voice prompts take priority, then local reception resumes. Closing receive audio or leaving FM releases this
monitor. This is still an experimental envelope receiver, not a general AM capability advertisement.

The filter is linear and uses fractional state to preserve low-level samples. It has no amplitude threshold or
noise gate. In-band noise remains; filtering cannot separate noise from speech at the same frequencies.
Monitor overrides squelch. Leave Monitor off for menu squelch control: zero keeps audio open, and a nonzero level
allows scanning to move past quiet channels. The current squelch still uses FM noise measurements, so even its
lowest nonzero level can miss weak AM. Open-squelch single-channel listening remains the weak-signal test.

The UI labels airband channels using an envelope-capable diagnostic as AM. This display classification is separate
from native AM receiver capability: it does not call a missing `rx_am` driver or change codeplug storage, tuning,
the receive filter, or transmit restrictions. Other analog frequencies retain the FM label.
