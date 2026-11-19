<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# USB and network audio

A computer hears the radio's receiver and transmits through it over the USB network personality
([usb-network.md](usb-network.md)) or PPP, with the USRP voice protocol over UDP (`CONFIG_NET_USRP`). This page covers
the board's audio taps, the wire format, the shared audio stream, and the radio's behavior in a host audio session.
For which radios have the FM paths, see the [feature matrix](../feature-matrix.md).

Section numbers are identifiers that code cites, so the numbering has gaps.

## 2. Audio taps

A board describes its audio paths beyond the microphone and speaker in `hal_audio_tap_t` (`hal/hal_audio_tap.h`).
Receive audio rides the board's one PCM stream (`hal_audio_t`: 8 kHz, 16-bit, 20 ms blocks).

| Cap bit | Meaning |
|---|---|
| `AUDIO_TAP_FM_RX` | FM receive audio (demodulated, de-emphasized) is the stream's input while the radio receives FM |
| `AUDIO_TAP_FM_TX` | MCU audio is the FM modulation source through `fm_tx_inject()` |
| `AUDIO_TAP_DMR` | DMR voice as PCM both ways: the stream itself, present on every radio with a vocoder |
| `AUDIO_TAP_FM_RX_RAW` | FM receive audio before de-emphasis and the voice filter |
| `AUDIO_TAP_FM_TX_FLAT` | the modulator without pre-emphasis, compressor, and voice filter |

Other fields:

- `fm_tx_inject(pull, mode)`: while the radio transmits FM, the modulation source becomes the samples `pull` returns
  (`pull` non-NULL) or the microphone again (NULL). The deviation, the limiter, and the sub-audio tone stay the radio's.
  It runs only between `hal_radio_t.tx()` and `tx_off()`, in the radio task, and returns false when the board can't
  inject in that mode.
- `fm_rx_flat_rate_hz`: the rate of flat receive audio that doesn't come through the 8 kHz stream. 0 means it rides
  the stream.
- `fm_tx_nominal_peak`: the 16-bit sample peak of injected audio that gives 60 % of the peak deviation (for example,
  1.5 kHz on a 12.5 kHz channel). 0 means unknown.
- Probe hooks (`modem_reg`, `modem_ram`, `modem_burst`, `last_block`, `pin_tone`, `pin_scan`, and the PWM test tone)
  exist only with `CONFIG_AUDIO_PROBE` (9).

A discriminator signal is flat: it comes before de-emphasis and the voice filters. Voice needs de-emphasis and a
300 Hz high pass in software. 9600 baud packet and AFSK use the flat signal directly.

On an HR_C6000 board, FM receive audio is the chip's feed-through on I2S, squelch gated. FM transmit audio is PWM into
the chip's differential microphone input (9.1).

## 4. Protocols

### 4.2 USRP over UDP

USRP is the voice protocol that AllStarLink's USRP channel driver, DVSwitch Analog_Bridge and its clients, and
USRP-to-M17 or USRP-to-DMR bridges speak. It carries 8 kHz audio in 20 ms blocks, with PTT and COS in-band as the
keyup flag and no session set-up.

A datagram is a 32-byte header and an optional payload (`core/usbaudio/usrp.h`):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `"USRP"` |
| 4 | 4 | sequence number, one up per datagram, per sender |
| 8 | 4 | memory (0) |
| 12 | 4 | keyup: 1 while the sender transmits (PTT or COS), 0 otherwise |
| 16 | 4 | talkgroup (0) |
| 20 | 4 | type |
| 24 | 4 | mpxid (0) |
| 28 | 4 | reserved (0) |
| 32 | 320 or 160 | payload |

Header words are big endian. Voice samples are little endian; `USRP_SAMPLES_BIG_ENDIAN` flips them.

| Type | Value | Payload | Datagram length |
|---|---|---|---|
| VOICE | 0 | 160 signed 16-bit samples, 8 kHz (20 ms) | 352 |
| DTMF | 1 | not used by the radio | |
| TEXT | 2 | not used by the radio | |
| PING | 3 | not used by the radio | |
| TLV | 4 | not used by the radio | |
| VOICE_ADPCM | 5 | not used by the radio | |
| VOICE_ULAW | 6 | 160 G.711 mu-law bytes | 192 |

The radio sends VOICE frames. It accepts VOICE and VOICE_ULAW. A voice frame with the wrong payload length is
rejected.

**Coexistence with the IP gateway.** The gateway answers the radio's own address on ICMP, DHCP, and UDP 3334, and
forwards everything else addressed to radio IDs and talkgroups over the air ([usb-network.md](usb-network.md) 3). The
USRP port is a third local UDP port of the radio's address. A DMR voice transmission and a data transmission don't
overlap, because the data service waits for a free channel. In FM mode the gateway has no air side, and USRP is the
only user.

## 5. Design

### 5.1 The shared audio stream

`core/usbaudio/audio_mux.c` shares the board's one PCM stream between the radio's own users (DMR voice, beeps) and a
host audio session. The radio's users get a wrapped `hal_audio_t` (`audio_mux_wrap`) and keep their start, stop, and
callback logic. The mux keeps the board's stream running while either side wants it and calls their callback from its
own.

| Direction | FM | DMR |
|---|---|---|
| Receive, to the host | the stream's input on a board with `AUDIO_TAP_FM_RX`; silence on others, whose input is the microphone | the stream's output (the decoded voice) |
| Transmit, from the host | the board pulls samples through `fm_tx_inject()` (`audio_mux_fm_pull`) | while the host transmits, its samples replace the microphone's on the way to the encoder |

Receive audio goes into a ring of `AUDIO_MUX_RX_CAP` (1024 samples, 128 ms) that the host task reads. Transmit audio
goes into a jitter buffer (5.3).

APRS (`CONFIG_APRS_RX`) has its own receive ring of `AUDIO_MUX_APRS_CAP` (1024 samples): the stream's input while it
asks for it, on a board with `AUDIO_TAP_FM_RX` ([aprs.md](aprs.md) 5.3).

Threads:

- `audio_mux_service` and the wrapped start and stop run in the radio task. They are the only places that start or
  stop the board's stream.
- The block callback runs in the board's audio task.
- `audio_mux_host_*` runs in the host link task.
- `audio_mux_fm_pull` runs in the board's PWM interrupt.

Each ring has one producer and one consumer. A stream that doesn't start (no codec clock) is retried every
`AUDIO_MUX_RETRY_MS` (1 s).

### 5.3 Rates and the jitter buffer

The stream is 8 kHz, 16-bit, mono. A USRP frame is exactly one 160-sample block in each direction.

The host and the radio run on separate clocks. The host-to-radio side has a jitter buffer (`core/usbaudio/pcm_slip.c`)
of `PCM_SLIP_CAP` (1024 samples) with a target of `AUDIO_MUX_TX_TARGET` (480 samples, 60 ms). When the fill drifts a
block away from the target, the buffer drops or repeats one sample. At 100 ppm that is one sample every 1.25 s, which
is inaudible on voice and harmless to AFSK and FSK modes. The host test runs 200 ppm either way with 15 ms of jitter
for 10 minutes with no underrun and no overflow. Audio that doesn't fit in the buffer is dropped.

The radio-to-host side has no jitter buffer.

### 5.5 FM audio: voice or flat

`audio_tap_mode_t` selects what the FM taps carry:

| | `AUDIO_TAP_VOICE` | `AUDIO_TAP_FLAT` |
|---|---|---|
| Receive | de-emphasized, voice band | the discriminator |
| Transmit | the radio's pre-emphasis, filters, and sub-audio tone | straight to the modulator; the limiter stays |

`fm_tx_inject()` takes the mode. A board sets `AUDIO_TAP_FM_RX_RAW` and `AUDIO_TAP_FM_TX_FLAT` only when it has the
flat path. The HR_C6000 PWM path is voice only: it stops at the codec's 3.5 kHz.

## 6. Behavior and safety

### 6.1 Levels

- Receive: DMR decoded audio is the codec's output level. On an HR_C6000 board, FM receive audio is the speaker's
  feed-through, so it follows the volume knob (9.1).
- Transmit: the host's audio enters the chip's FM transmit chain, so pre-emphasis, the chip's limiter, and the channel
  filter keep the deviation inside the channel whatever the host sends. `hal_audio_tap_t.fm_tx_nominal_peak` gives
  the sample level for 60 % of the peak deviation.
- Sub-audio (CTCSS or DCS) on transmit is the radio's. The host doesn't generate it.

### 6.3 Watchdogs

The radio doesn't transmit without a live host:

- **Keyup timeout**: 300 ms without a keyup frame unkeys (`USRP_KEYUP_TIMEOUT_MS`).
- **Transmission limit**: 180 s in one transmission (`USRP_TX_MAX_MS`) unkeys and locks out until the host sends
  keyup 0 or a quiet gap.
- **Stale audio**: in the radio task, no host audio for 1 s unkeys (`AUDIO_MUX_HOST_STALE_MS`). This covers the case
  where the network service stops because the USB link is in the serial personality.
- **Link down**: the network link going down unkeys.
- **Time-out timer**: the radio's TOT (`tot_s`, [radio-features.md](radio-features.md) 1 and 3.2) applies to host PTT
  too.

### 6.5 Interaction with the hotspot

Hotspot mode owns the modem. While it runs, the radio refuses host transmissions and sends no receive audio. Entering
hotspot mode ends a host transmission.

## 9. Source files and behavior

| Item | Files | Tests |
|---|---|---|
| USRP framing, G.711 mu-law, link state (keyup PTT, keyup watchdog, transmission limit with lockout, sequence accounting) | `core/usbaudio/usrp.h`, `usrp.c` | `tests/host/usrpTest.c` |
| Jitter buffer with sample slipping | `core/usbaudio/pcm_slip.h`, `pcm_slip.c` | `tests/host/usrpTest.c` |
| Shared stream (receive ring, transmit jitter buffer, DMR microphone replacement) | `core/usbaudio/audio_mux.h`, `audio_mux.c`; `radio_sm.c` gives the radio's users the wrapped stream | `tests/host/usrpNetTest.c`, `radioFeaturesTest.c` |
| USRP on the network address | `core/usbaudio/usrp_net.h`, `usrp_net.c`; `tools/usrp_client.py` (reference peer) | `tests/host/usrpNetTest.c`, `tests/hil/test_usrp.py` |
| Host audio PTT in the radio core | `radio_req_ptt_host()`, `radio_sm.c` (`host_audio`, its watchdog) | `tests/host/radioFeaturesTest.c` |
| FM transmit audio from the MCU | `hal/hal_audio_tap.h`; the board's audio file (PWM) and radio file (`<id>_fm_tx_inject`) under `boards/` | `tests/hil/test_usrp.py` |
| Audio probe: registers, PCM levels, chip RAM, FM flag watch | `core/debuglink/debuglink_audio_probe.c`, the board's `CONFIG_AUDIO_PROBE` parts, `tools/rfoundry_debug.py aprobe` | `tests/hil/test_audio_probe.py` |

The probe (`CONFIG_AUDIO_PROBE`, [debuglink.md](debuglink.md) 3.14) measures a board's audio paths on the bench. Run
`tools/rfoundry_debug.py aprobe` on a build with the switch on.

### 9.1 USRP on the network (CONFIG_NET_USRP)

With the switch on, in the USB network personality or PPP:

| Item | Behavior |
|---|---|
| Ports | listens on UDP 34001 at its own address; sends from 34001 to UDP 32001 (`USRP_PORT`, `USRP_PEER_PORT`). An AllStar node uses `rxchannel = usrp/<radio address>:34001:32001`; Analog_Bridge uses `txPort = 34001`, `rxPort = 32001` |
| Peer | the address of the last valid frame received. Before one, or 60 s after the last (`USRP_PEER_IDLE_MS`), the host's address on the link. Ports and peer have no setting |
| Receive (radio to peer) | a VOICE frame every 20 ms while the receiver is active (`state` rx: FM squelch and tone open, or a DMR call playing), keyup 1; one keyup-0 frame of silence when it closes; nothing in between. Not while transmitting or in hotspot mode |
| Receive audio | FM: the stream's input (on an HR_C6000 board, the chip's FM receive audio, squelch gated, after its de-emphasis and filters). DMR: the decoded voice the speaker gets, beeps mixed in |
| Transmit | a frame with keyup 1 after keyup 0 (or none) keys the radio (`radio_req_ptt_host`). Its audio and the following frames' go through the 60 ms jitter buffer. FM: the board's `fm_tx_inject` (refused on a board without `AUDIO_TAP_FM_TX`). DMR: the samples replace the microphone's on the way to the encoder; the call goes to the channel's talk target |
| Unkey | keyup 0, or any watchdog in 6.3. No roger beep. A transmission keeps the source it started with: local PTT pressed meanwhile doesn't switch to the microphone |
| Refused | in hotspot mode, and FM on a board without the transmit tap (counted in `usrp.refused`) |
| Debug link | view section `usrp` ([debuglink.md](debuglink.md) 3.7) |

On an HR_C6000 board the FM transmit path is PWM into the chip's differential microphone input. While the host
transmits, MIC_P and MIC_N are on and the microphone (LINEIN1) is off. A timer channel with DMA rewrites the duty every
carrier period. The samples are linearly interpolated over the periods, with the rounding error carried from period
to period. The carrier, the full-scale swing, and `fm_tx_nominal_peak` are the board's. The chip's own FM chain does
the rest: pre-emphasis, limiter, and sub-audio tone.

Limits of the HR_C6000 paths:

- The receive audio is the speaker's FM feed-through. It follows the volume knob (the chip's RX gain), stops at volume
  zero, and drops out while a beep plays, because the feed-through is off for PCM.
- There is no flat mode. The PWM path stops at the codec's 3.5 kHz, so 9600 baud doesn't pass.

## References

- HR_C6000 user manual (Zhejiang Hongrui): 4.5 codec, figure 4.12, 5.3 RAM tables, 6 FM, register table (0x06, 0x22,
  0x36, 0x37, 0x82, 0xD7, 0xE0-0xE4) and appendix A (FM paths).
- ITU-T G.711 (mu-law).
- AllStarLink community forum, USRP channel driver set-ups (ports 32001 and 34001 with Analog_Bridge):
  <https://community.allstarlink.org/t/linking-allstar-to-a-mmdvm-hotspot-dmr-to-allstar-node-bridge/18146>; M17
  project wiki, usrp2m17: <https://wiki.m17project.org/usrp2m17_bridge>.
