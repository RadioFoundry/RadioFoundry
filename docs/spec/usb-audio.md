<!-- SPDX-License-Identifier: MIT -->

# Analog and DMR voice over USB: feasibility and design

A computer should be able to hear the radio's receiver, transmit through it, and control PTT, channel and frequency
over USB: as a USB sound card (optionally with a serial port beside it, like the commercial "data radio" interfaces),
over the serial port, or over the USB network personality ([usb-network.md](usb-network.md)). This page is the
feasibility study and the design. Built so far: USRP voice over UDP on the network personality (`CONFIG_NET_USRP`,
9.1) with the stream sharing it needs (`core/usbaudio/audio_mux.c`, part of 5.1) and the DM-1701's FM transmit path
(PWM into the microphone input), and a bench probe (`CONFIG_AUDIO_PROBE`) that measures a board's audio paths. The
sound card, the serial transports and CAT are not built. Section 9 lists what is in the repository.

## Summary

| Question | Answer |
|---|---|
| DMR voice as PCM, both ways | Yes on every radio: the MCU runs the AMBE codec and already exchanges 8 kHz PCM with the audio codec. A USB endpoint uses the same blocks (5.6) |
| FM audio to and from the MCU | Depends on the radio (section 2) |
| USB composite with a sound card | Fits on the STM32F405's OTG_FS: CDC-ACM + UAC1 or CDC-NCM + UAC1, not all three, and not serial + sound card + HID (3) |
| Recommended protocols | IP: the USRP voice protocol over UDP (AllStarLink, DVSwitch Analog_Bridge, M17 and DMR bridges speak it) with PTT by its keyup flag; Hamlib's rigctld protocol, now on the radio (`CONFIG_NET_RIGCTL`, [tcp-services.md](tcp-services.md) 6). Serial: Kenwood-style CAT plus RTS/DTR PTT. Sound card: UAC1, PTT by the serial port's RTS/DTR (CM108-style HID as an alternative personality) (4) |
| Effort | About 6 to 9 working weeks for everything, in five phases; phase 0 (the probe) and phase 2 (USRP on the network) are done, phase 1 in part (7) |

## 1. Use cases and transports

- **Digital modes on FM**: fldigi, WSJT-X, JS8Call, VARA FM, Direwolf (APRS / packet) see a sound card and key PTT
  through a serial port's RTS or DTR, a CAT command set, or a CM108-style HID GPIO.
- **Linking**: AllStarLink, HamVoIP, DVSwitch and similar link a radio to a network as an RF node. They want 8 kHz
  audio, PTT from the network side, and a carrier-operated squelch (COS) signal from the radio.
- **DMR as a USB voice endpoint**: the radio's AMBE codec turns host PCM into a DMR call and DMR calls into PCM, so a
  computer can join a DMR channel on RF with no codec of its own (DVSwitch-like set-ups, dispatch consoles).
- **Remote operation over IP**: the USB network personality already gives the host an IP link to the radio.

Transports, as the task names them: (a) the USB network personality (UDP over CDC-NCM), (b) the USB serial port
(CDC-ACM, or a UART on radios without USB), (c) a USB Audio Class sound card, alone or composite with the serial port.

## 2. Audio paths per radio

What each radio offers the design. Radios not listed have not been studied.

| Radio | Host port | DMR PCM | FM receive to the MCU | FM transmit from the MCU | Details |
|---|---|---|---|---|---|
| DM-1701 (HR_C6000) | USB | I2S | the I2S stream (squelch gated) or the modem's RX RAM (ungated), 8 kHz | PWM into the mic input, or the modulator offset register (about 6 bits); AFSK from the modem's tone generator | [hardware-dm1701.md](hardware-dm1701.md) 21 |
| MD-380 (HR_C5000) | USB | I2S | the discriminator on an MCU ADC, unused so far | through the modem's codec, as on the DM-1701; the C5000's FM registers are unconfirmed | [hardware-md380.md](hardware-md380.md) 6.1 |
| MD-9600 (HR_C6000) | USB | I2S | the IF chip's discriminator on an MCU ADC; FM doesn't pass through the modem | as the DM-1701 | [hardware-md9600.md](hardware-md9600.md) 4.2, 6.1 |
| DM-32UV (HR_C7000) | UART on the programming cable (no USB connector) | the SoC's mic and speaker PCM buffers | plausible (documented FM data interrupts and PCM bits), unverified | plausible, unverified | [hardware-dm32.md](hardware-dm32.md) 9.2 |

A discriminator signal is flat (before de-emphasis and the voice filters). Voice needs de-emphasis and a 300 Hz high
pass in software; 9600 baud packet and AFSK use the flat signal directly. A timer-triggered ADC samples it at 8-48 kHz
on a second ADC, so the existing ADC scan stays as it is.

The probe (`CONFIG_AUDIO_PROBE`, [debuglink.md](debuglink.md) 3.14) measures a board's paths on the bench.

### 2.1 Budgets

| Item | Estimate | Basis |
|---|---|---|
| Sample rate on the radio side | 8 kHz, 16 bit, mono | the audio codec's word clock; the AMBE codec and the modem's FM chains run at 8 kHz |
| Latency, one way | about 60-100 ms | 20 ms DMA block + 20 ms processing slot + a 40-60 ms jitter buffer (pcm_slip target) + 1 ms USB frames; DMR adds the codec's 60 ms burst rhythm |
| CPU | under 5 % of a 72 MHz Cortex-M4 for FM audio | resampling 8 to 48 kHz and back with a 48-tap polyphase FIR is about 0.8 M multiply-adds a second each way; framing and the jitter buffer are negligible; the AMBE codec is unchanged from a normal call |
| DMA | none new | the audio stream already has its DMA; the OTG_FS core has no DMA (TinyUSB runs it in slave mode), so isochronous packets are copied by the CPU: 1000 interrupts a second per streaming endpoint, about 100 bytes each at 48 kHz |
| USB bandwidth | under 2 % of full speed | 48 kHz mono 16-bit is 96 bytes per 1 ms frame each way |
| RAM | about 3-4 KB | jitter buffer 2 KB (1024 samples), resampler state and two isochronous packet buffers about 0.5 KB, USRP datagram buffer 0.4 KB, UAC class state; the OTG FIFOs are in the peripheral (3) |
| Flash | about 12-18 KB for everything | TinyUSB audio class and descriptors about 6-8 KB, resampler 1 KB, USRP and router 2 KB, CAT parser 2-3 KB (rigctld is already built). A radio whose default image is nearly full needs room made first: LTO (about 20 KB) or features off ([feature-matrix.md](../feature-matrix.md)) |

On the STM32F4 radios only the buffers that DMA or USB touches need main RAM; the rest can go in CCM.

## 3. USB: what fits

The USB core of the STM32F405 radios (OTG_FS) has endpoint 0 plus 3 IN and 3 OUT endpoints and 1.25 KB of FIFO RAM
([hardware-dm1701.md](hardware-dm1701.md) 16; `USB_OTG_FS_MAX_IN_ENDPOINTS` = 4 including EP0 and
`USB_OTG_FS_TOTAL_FIFO_SIZE` = 1280 in ST's device header; TinyUSB's `dwc2_stm32.h`: 4 endpoints, 320 words).

| Function | IN endpoints | OUT endpoints |
|---|---|---|
| CDC-ACM (serial) | 2 (notification, bulk) | 1 (bulk) |
| CDC-NCM (network) | 2 (notification, bulk) | 1 (bulk) |
| UAC1 mono in + out, asynchronous IN, adaptive OUT | 1 (isochronous) | 1 (isochronous) |
| the same with an asynchronous OUT | 2 (isochronous, feedback) | 1 |
| HID (CM108-style PTT) | 1 (interrupt) | 0 (output reports over EP0) |

| Personality | IN | OUT | Fits |
|---|---|---|---|
| CDC-ACM (today) | 2 | 1 | yes |
| CDC-NCM (today) | 2 | 1 | yes |
| CDC-ACM + UAC1 | 3 | 2 | **yes** |
| CDC-NCM + UAC1 | 3 | 2 | **yes** |
| UAC1 + HID | 2 | 1 | yes |
| CDC-ACM + UAC1 + HID | 4 | 2 | no |
| CDC-ACM + CDC-NCM (+ anything) | 4+ | 2+ | no (why the network mode is a switch today, [usb-network.md](usb-network.md) 1) |
| CDC-ACM + UAC1 with feedback endpoint | 4 | 2 | no: so the OUT stream must be adaptive (the device follows the host's rate) |

FIFO budget for CDC-ACM + UAC1 at 48 kHz: RX FIFO about 64 words (control, bulk 64 B and isochronous 96 B packets with
the per-packet status words), TX FIFOs EP0 16, notification 16, bulk IN 32 (double), isochronous IN 2 x 25 words:
about 180 of 320 words. Comfortable at 48 kHz, ample at 8 or 16 kHz.

**TinyUSB** (third_party/tinyusb, release 0.21.0) has an audio device class that handles both UAC1 and UAC2
descriptors (`src/class/audio/audio_device.c`; `TUD_AUDIO10_*` descriptor macros in `usbd.h`), composite devices,
and a `cdc_uac2` example of exactly the serial + sound card composite.

**Driverless**: UAC1 binds the in-box class driver on Windows (usbaudio.sys, since long before Windows 10), macOS and
Linux (snd-usb-audio). UAC2 binds in-box on macOS, Linux and Windows 10 version 1703 and later (usbaudio2.sys).
CDC-ACM binds in-box everywhere current (usbser.sys on Windows 10+). HID needs no driver. **Choose UAC1**: the widest
reach, everything a mono 8-48 kHz interface needs, and no clock-source entities to get wrong.

**Rates**: offer 48 kHz (the default alternate setting: what WSJT-X, fldigi and the OS mixers prefer and what CM108
interfaces offer), plus 16 kHz and 8 kHz (no resampling at 8 kHz; Direwolf and AllStar's 8 kHz paths). The firmware
resamples 8 kHz to the chosen rate and back.

**Clocks**: the radio's audio clock is the modem's 8 kHz word clock (on its own crystal), not the USB frame
clock.
The IN stream (radio to host) is asynchronous: the device sends 47 / 48 / 49 samples per frame (or 7 / 8 / 9 at
8 kHz) as its clock dictates, which every host handles. The OUT stream is adaptive: the host sends at its rate, and
the firmware's jitter buffer (`core/usbaudio/pcm_slip.c`) absorbs the difference by dropping or repeating one sample
when the fill drifts a block away from its target. At 100 ppm that's one sample every 1.25 s, inaudible on voice and
harmless to AFSK and the FSK modes (host test: 200 ppm either way for 10 minutes with 15 ms of jitter, no underrun,
no overflow). The same buffer serves USRP and serial audio, whose senders run on the host's clock too.

**IDs**: the radios use NXP's VID with the aftermarket firmware's PIDs today (`board_usb_ids`; `boards/dm1701/usb_dm1701.c` says a
release should move to an ID of its own). A composite personality changes the interface set, so it needs its own PID (Windows caches the
driver binding per VID/PID). pid.codes (VID 0x1209) gives open-source projects PIDs for free; the AIOC adapter, an
MIT-licensed STM32 sound card + serial + CM108-style HID cable, does that (1209:7388).

## 4. Protocols

The rule: implement what existing software already speaks; add our own only where nothing fits.

### 4.1 Control (PTT, frequency, channel, squelch state)

| Protocol | Who speaks it | Transport | Pros | Cons |
|---|---|---|---|---|
| **RTS / DTR PTT** on a serial port | fldigi, WSJT-X, JS8Call, VARA, Direwolf, Hamlib, almost everything | CDC-ACM SET_CONTROL_LINE_STATE | trivial, universal | the MMDVM hotspot and the CPS share the port and toggle DTR: PTT must be enabled only in audio sessions (6.3) |
| **Kenwood-style CAT** (TS-480 / TS-2000 subset: `FA`, `FB`, `IF`, `MD`, `TX`, `RX`, `ID`, `PS`, `BY` for busy) | Hamlib, fldigi, WSJT-X, JS8Call, VARA's own CAT, Direwolf through Hamlib | the serial port | the most widely supported CAT set; ASCII, `;`-terminated, small parser | ASCII letters collide with the open-firmware CPS's request bytes (`R`, `W`, `X`, `C`, `D`): needs a recognizer (two uppercase letters, then digits or `;`) or a session mode (6.4) |
| **Hamlib rigctld protocol** ("NET rigctl", model 2) | Hamlib clients (WSJT-X, fldigi, JS8Call, GridTracker, Direwolf `PTT RIG`), default TCP port 4532 | TCP | one implementation serves every Hamlib client over the network personality; simple line protocol (`F`, `f`, `T`, `t`, `M`, `m`, `\dump_state`, `\chk_vfo`, `\get_dcd`) | needs TCP, built since (`core/net`, [tcp-services.md](tcp-services.md)). `\dump_state` is long and version sensitive |
| flrig XML-RPC (TCP 12345) | fldigi, WSJT-X, JS8Call (as "FLRig") | HTTP over TCP | well supported | XML and HTTP on the radio: too heavy |
| CM108-style HID GPIO | Direwolf (`PTT CM108`), Hamlib (CM108 PTT type), AllStarLink usbradio / simpleusb (COS on an input, PTT on GPIO 3) | USB HID | Linux stacks key PTT with no serial port; COS can be an input report | its own personality (doesn't fit beside CDC + UAC, 3); software matching C-Media IDs needs our ID added (AIOC got it into AllStarLink 3) |
| our debug link | our tools and HIL tests | serial, UDP 3334 | already has PTT with a firmware time-out, channel, VFO frequency, power, squelch, tones (debug link ops) | nobody else speaks it |

**Recommendation**:

- On the serial port: **RTS/DTR PTT** (configurable which line and polarity; default PTT = RTS asserted, as most
  software defaults to RTS; AIOC uses DTR=1 and RTS=0 after finding that CHIRP-style programmers toggle DTR) and a
  **Kenwood TS-480-style CAT subset** recognized beside the other protocols.
- Over IP: **rigctld on the radio**. Built: port 4532 on the USB network link with a minimal TCP (`CONFIG_NET_RIGCTL`,
  [tcp-services.md](tcp-services.md) 6), so the host bridge first planned here (`tools/odr_rig.py`) isn't needed.
- In the sound-card-only personality: **CM108-compatible HID** report layout (PTT on GPIO 3 as an output report, COS
  as an input report bit), under our own VID/PID.

### 4.2 Audio over IP

| Protocol | Who speaks it | Format | Fit |
|---|---|---|---|
| **USRP** (named after the GNU Radio hardware it first linked) | AllStarLink's USRP channel driver, DVSwitch Analog_Bridge and its clients, GNU Radio / OP25 repeater scripts, M17 and DMR bridges (usrp2m17) | UDP; a 32-byte header ("USRP", sequence, keyup, talkgroup, type) and 160 samples of 8 kHz 16-bit PCM (20 ms) or 160 bytes of mu-law; PTT is the keyup flag in every frame | **best**: exactly our sample rate and block size, PTT and COS in-band, no session set-up, and it plugs the radio into AllStar / HamVoIP / DVSwitch as an RF node with no adapter |
| RTP (RFC 3550) with PCMU / PCMA (RFC 3551), L16 or Opus (RFC 7587) | SIP phones, Asterisk, IP radio gateways | UDP, 12-byte header | standard, but PTT and COS need a side channel (SIP INFO, RFC 4733 events, or a vendor extension); session set-up (SIP / SDP) is the real cost |
| EUROCAE ED-137 (part 1, radio) | air traffic control radio-over-IP: radios, gateways and voice switches | RTP with PTT and squelch bits in an RTP header extension, SIP session set-up, keep-alives | the one standard that is radio over IP with PTT and squelch in the audio stream; but SIP, keep-alives and its conformance rules are a lot of code for a ham radio, and ham software doesn't speak it |
| TCI (Expert Electronics' Transceiver Control Interface) | the SunSDR transceivers' software as server; JTDX, MSHV, some loggers and digital-mode programs as clients | WebSocket over TCP: text commands (CAT, PTT) and binary audio frames on one connection | control and two-way audio in one open protocol that digital-mode software already speaks; WebSocket and HTTP upgrade on the radio, and fewer clients than Hamlib plus a sound card |
| Mumble | Mumble servers and clients; hobby radio-over-IP gateways | TLS control, UDP Opus voice | push-to-talk voice chat with an ecosystem, but Opus and TLS on the MCU are too heavy |
| VBAN | VB-Audio's mixers, some SDR software | UDP port 6980, 28-byte header | simple, but no PTT; Windows-centric ecosystem |
| IAX2 (RFC 5456) | Asterisk, AllStarLink node-to-node | UDP 4569, full / mini frames, call signaling | much more protocol than we need; AllStar nodes link to the radio better through USRP on the node |
| Kenwood / Icom / Yaesu remote | their own apps | proprietary | references only |
| Zello | Zello clients | proprietary over TCP / TLS, Opus | out of scope |

**Recommendation: USRP over UDP**, voice type 0 (16-bit PCM) by default and type 6 (mu-law, half the bytes) as an
option. The radio sends a frame every 20 ms while its receiver is active (keyup = COS, 5.4) and stops (after a keyup-0
frame) when it closes; the host's frames with keyup 1 key the transmitter. Ports are configurable; AllStar and
Analog_Bridge set-ups commonly pair 32001 / 34001 (one side's TX port is the other's RX port). The format as
implemented in `core/usbaudio/usrp.c` comes from the public descriptions (header words big endian, samples little
endian, as every known peer runs on little-endian hosts); it must be confirmed against a live Analog_Bridge or
AllStar node with a packet capture before release (owner question 4). The GPL implementations were not used as a
source.

**Coexistence with the IP gateway**: the gateway answers the radio's own address on ICMP, DHCP and UDP 3334, and
forwards everything else addressed to radio IDs and talkgroups over the air ([usb-network.md](usb-network.md) 3). The
USRP port becomes a third local UDP port of the radio's address; the gateway's air side and the voice side share the
modem only in the sense that a DMR voice transmission and a data transmission can't overlap (the data service already
waits for a free channel). In FM mode the gateway has no air side at all, and USRP is the only user.

### 4.3 Audio over the serial port

8 kHz 16-bit PCM is 128 kbit/s each way, 256 kbit/s full duplex: about 2 % of full-speed USB, and CDC-ACM bulk moves
several Mbit/s on all three host OSes, so it fits with a wide margin (8-bit mu-law halves it). The issue is framing
on a byte stream that the host link already shares among MMDVM (first byte 0xE0), the debug link (0xA5 0x5A) and the
CPS (ASCII letters) ([cps-protocol.md](cps-protocol.md) 1). Options:

- the debug link's framing: requests are capped at 64 bytes, too small for a 320-byte frame without a new frame kind;
- KISS (FEND 0xC0): the TNC framing Direwolf and APRS software speak, but its data frames mean AX.25 packets, and a
  KISS client would misread audio;
- **SLIP (RFC 1055) framing of whole USRP datagrams**, first byte 0xC0 (END), which no other protocol on the port
  starts with. The radio runs the same USRP code for serial and IP; a host bridge (`socat`-like, a few lines of
  Python) turns the serial port into a UDP USRP endpoint, so Analog_Bridge or AllStar can use the radio over the
  serial port too. **Recommended.**

Serial audio matters most on radios whose host port is a UART (section 2), and on hosts where installing the
network adapter is unwanted. 8 kHz mu-law is 64 kbit/s plus framing, about 70 % of a 115200 baud UART in one
direction: half duplex works, full duplex doesn't. Most USB-serial cable chips run 460800 baud or more; a faster host
port is a setting both ends must agree on. With the composite sound card the serial port carries only CAT and
RTS/DTR.

### 4.4 Choices

| Transport | Audio | Control and PTT | COS / squelch | Works with, unchanged |
|---|---|---|---|---|
| (c) sound card: CDC-ACM + UAC1 | UAC1 mono, 48 / 16 / 8 kHz | RTS/DTR, Kenwood CAT on the CDC port; rigctld through the host bridge | CAT `BY`, CDC SERIAL_STATE DCD (shows as the port's DCD / RLSD line) | fldigi, WSJT-X, JS8Call, VARA FM, Direwolf, Hamlib, any SDR-style app |
| (c') sound card fob: UAC1 + HID | UAC1 | CM108-style GPIO output report | CM108-style input report bit | Direwolf `PTT CM108`, Hamlib CM108 PTT, AllStarLink usbradio / simpleusb once our ID is known to them |
| (a) network: CDC-NCM (+ UAC1) | USRP over UDP (or UAC1 beside it) | USRP keyup; rigctld (host bridge, later on the radio) | USRP keyup | AllStarLink, HamVoIP, DVSwitch Analog_Bridge, usrp2m17-style bridges; Hamlib clients through the bridge |
| (b) serial only (a UART host port, or CDC-ACM without audio) | USRP datagrams in SLIP frames | USRP keyup, CAT | USRP keyup, CAT `BY` | the above through the host bridge |

## 5. Design

### 5.1 The audio router (core/audio, new)

Today the DMR voice code (`ambeVoice.c`) owns `hal_audio_t.start()` while in DMR, and beeps start the stream on their
own in FM (`core/radio/beep.c`). With USB audio and APRS (the `core/aprs` work package) there are four consumers of
the one 8 kHz stream, so one module should own it and hand each block to its users:

| Consumer | Takes the stream's input (mic or RX tap) | Writes the stream's output (speaker or TX inject) |
|---|---|---|
| DMR voice | mic blocks to the encoder (TX) | decoded blocks (RX) |
| beeps | | mixed into the speaker block |
| APRS modem | RX tap blocks to the AFSK demodulator | AFSK tone blocks while it transmits (or the RF chip's tone generator) |
| USB / network audio | RX audio (FM tap, or decoded DMR) to the host; the mic is not sent | host audio to the TX inject (FM) or the encoder (DMR) while host PTT is keyed |

The router keeps the stream running whenever any consumer wants it, calls `board.audio_tap->fm_tx_inject()`
(`hal/hal_audio_tap.h`) as the active consumers need, and resolves conflicts by priority: local PTT
and emergency over host PTT, DMR voice over APRS, APRS transmit only on an idle channel. `hal_audio_tap.h` is the
interface the USB audio and APRS packages share; the APRS agent should use it rather than adding its own audio hooks.

### 5.2 USB personalities

The serial / network switch ([usb-network.md](usb-network.md) 1) grows into a choice of personalities, each with its
own PID, chosen by a setting and switched by re-enumeration:

| Personality | Interfaces | PID |
|---|---|---|
| serial | CDC-ACM | today's |
| serial + audio | CDC-ACM, UAC1 (control, speaker streaming, microphone streaming), with an IAD per function | new |
| network | CDC-NCM | today's |
| network + audio | CDC-NCM, UAC1 | new |
| audio fob | UAC1, HID (CM108-style reports) | new |

UAC1 topology: input terminal "radio receiver" (type 0x0710, radio receiver) to an output terminal USB streaming (the
host's microphone); USB streaming input terminal to an output terminal "radio transmitter" (0x0711) (the host's
speaker); a feature unit with mute and volume on the transmit side lets the host set the TX audio level. Product string
names the radio (the board's product name followed by "audio"), so it is recognizable in sound settings. If a host mishandles the radio
terminal types, fall back to the generic microphone (0x0201) and speaker (0x0301) types.

### 5.3 Rates and the radio side

The stream stays 8 kHz. Toward UAC at 48 or 16 kHz a polyphase FIR interpolates and decimates (3.4 kHz passband,
fixed point, about 1 KB of code). The exception is flat FM audio from an MCU ADC (5.5): it goes to the host at the
ADC's rate, without the voice band filter. The jitter buffer sits on the host-to-radio side (`pcm_slip`, target 60 ms); the
radio-to-host side needs none (the asynchronous IN endpoint carries the radio's own clock). USRP frames are exactly
one 160-sample block each way.

### 5.4 Squelch and COS

The radio core knows when the receiver is "active": FM squelch open with the right tone, or a DMR call playing
(`radio_status`, the `state` key of the debug link view). That one bit is the COS:

- USRP: keyup in the radio's frames; frames flow only while it is set, plus a keyup-0 frame at the end.
- CDC: SERIAL_STATE notification with DCD (needs `CFG_TUD_CDC_NOTIFY`; Windows exposes it as RLSD, Linux as TIOCM_CD),
  and the Kenwood `BY` answer.
- HID: an input report bit, as CM108-based interfaces report a COS input.
- rigctld: `\get_dcd`.

RX audio sent to the host is gated by the same bit (silence otherwise), so software that has no squelch of its own
(VOX-keyed linking) doesn't hear noise. A setting "ungated" passes the receiver's audio all the time for decoders that
prefer to see noise (some packet modems); that needs the FM feed-through on in the chip regardless of the squelch, which
the probe will show is or isn't independent of the speaker.

### 5.5 FM audio: voice or flat

A setting picks what FM audio the host gets and gives, so the same radio serves voice linking and data modems that
bring their own waveform (9600 baud G3RUH, VARA FM, M17 or other 4FSK, anything a soft modem generates):

| | Voice (default) | Flat |
|---|---|---|
| RX | de-emphasized, voice band (300-3000 Hz), gated by the squelch and tone (5.4) | the discriminator before de-emphasis and the voice filters, DC coupled where the hardware allows; gated by carrier squelch only, or ungated |
| TX | pre-emphasis, compressor if the channel has it, voice filter, limiter, the channel's sub-audio tone | no pre-emphasis, no compressor, no voice band filter; the limiter stays as deviation protection (it clips, so the host keeps its level below it); sub-audio tone per a separate setting (off by default in flat) |
| Rate | 8 kHz | the source's rate: 24 or 48 kHz from an MCU ADC on the discriminator; 8 kHz where the audio comes from the modem |

What flat means depends on the receive source (section 2):

- Discriminator on an MCU ADC: flat RX is the native path, and at 48 kHz it carries 9600 baud. Flat TX goes through
  the modem with its pre-emphasis, compressor and band-pass bits off ([modem-hrc6000.md](modem-hrc6000.md) 8, 8.1).
  Whether the modem's transmit path reaches down to a few tens of Hz, as 9600 baud needs, is a bench measurement.
- FM receive audio from the modem at 8 kHz: flat is limited to about 300 Hz-3.5 kHz, enough for 1200 baud AFSK and
  2400-4800 baud schemes, not 9600. Voice and flat are two settings of the modem's receive filters. If the tap is
  ungated, the squelch gating of 5.4 is done in software from the radio's COS.

The setting lives with the other USB audio settings, and the host can change it on the control channel (a CAT
extension and a debug link / rigctld command), so a modem program can switch to flat for a session. `hal_audio_tap_t`
carries it: `fm_tx_inject()` takes the mode, and `caps` says which modes and rates a board has
(`AUDIO_TAP_FM_RX_RAW`, `AUDIO_TAP_FM_TX_FLAT`). The DM-1701 offers voice only so far (9.1).

### 5.6 DMR as a USB voice endpoint

Receive: every DMR call that the radio would play (talkgroup and contact filters as for the speaker) goes to the host
as PCM, with COS for the call's duration; the call's metadata (source, destination, talker alias) goes to the host on
the control channel (CAT has no place for it: the debug link view, or USRP TEXT frames, type 2, which Analog_Bridge
uses for metadata). Transmit: host PTT starts a call to the channel's talk target (or one the host sets) and the host
PCM is encoded instead of the microphone. The timing is that of a normal call: the encoder already runs from 20 ms
blocks queued ahead of the timeslot interrupt ([modem-hrc6000.md](modem-hrc6000.md) 6.3). No new codec work, no new
CPU.

## 6. Behavior and safety

### 6.1 Levels

- RX: the FM tap is at the codec's level before the volume knob; the host sets its own capture level. DMR decoded
  audio is the codec's output level.
- TX: where the host's audio enters the modem's FM transmit chain (section 2), pre-emphasis, the modem's limiter and
  the channel filter keep the deviation inside the channel whatever the host sends. Where it bypasses the chain, the
  firmware does pre-emphasis and limiting. The
  feature unit volume and a firmware gain scale it; `hal_audio_tap_t.fm_tx_nominal_peak` (the sample level that gives
  60 % of peak deviation) is measured once on the bench per band and bandwidth with the probe's tone and the SDR
  bench, like the FM deviation calibration.
- Sub-audio (CTCSS / DCS) on transmit stays the radio's: the host doesn't have to generate it.

### 6.2 PTT sources and priority

Local PTT (the side key, the accessory jack), VOX, the radio's own features, and host PTT (RTS/DTR, CAT `TX`, HID
GPIO, USRP keyup, rigctld `T 1`). Local PTT wins: pressing it while the host transmits switches the source to the
microphone for as long as it is held. Emergency always wins. Host PTT is refused (and reported: CAT `?;`, USRP frames
ignored) when the radio refuses any transmission: TX inhibit, receive-only channel, busy channel lockout, battery
critical, out of band.

### 6.3 Watchdogs (no transmission without a live host)

- **USRP**: the host must keep sending keyup frames; 300 ms without one unkeys (`USRP_KEYUP_TIMEOUT_MS`), and a hard
  per-transmission limit (default 180 s) locks out until the host sends keyup 0 or stops (`core/usbaudio/usrp.c`,
  host tested).
- **RTS/DTR**: honored only while the host has the audio interface streaming (the OUT alternate setting active) or the
  port open with DTR in a CAT session; unplug, suspend, port close or the OUT stream stopping for 1 s unkeys.
- **CAT `TX;`**: unkeys when the host has sent nothing on the port for 5 s (CAT clients poll every second or so).
- **Always**: the radio's TOT (`tot_s`, [radio-features.md](radio-features.md) 1 and 3.2) applies to host PTT too.
- MMDVMHost and the CPS toggle DTR and RTS as they open the port: line PTT is armed only when the "serial PTT" setting
  is on and no MMDVM or CPS session is active (`hostlink_mode()` idle), so programming the radio can't key it.

### 6.4 Sharing the serial port

The host link dispatches by the first byte of a request ([cps-protocol.md](cps-protocol.md) 1). CAT commands are two
uppercase letters followed by digits, a sign or `;`; open-firmware CPS requests are a letter followed by binary bytes (an
area or sector number 1-10, or a sub-command 2-7). A CAT recognizer accepts `[A-Z][A-Z]` followed by `[0-9;+-]`
within 3 ms, and a CAT session ends after 2 s without a CAT command, as the CPS session ends today. SLIP-framed USRP
starts with 0xC0. Both need the host link to accept more than one side protocol (today it takes one, the debug link:
`hostlink_set_side_protocol`), a small change in `core/hostlink`.

### 6.5 Interaction with the hotspot and the UI

- Hotspot mode owns the modem (MMDVM over the serial port or UDP); USB audio sessions are refused while it runs, and
  entering hotspot mode ends them.
- The SFR and the IP gateway follow the same rule as the hotspot for DMR transmit (the data service's channel access).
- The UI shows a status chip ("USB", "NET") while a host audio session is active and "TX USB" while the host keys;
  the speaker keeps playing received audio unless the user mutes it; the volume knob doesn't change the level sent to
  the host.
- Settings (UI and qdmr): USB personality (serial, serial + audio, network, network + audio, audio fob), serial PTT
  line and polarity, RX gating (squelch / ungated), USRP ports and peer, TX audio gain.

## 7. Phased plan and effort

Effort in working days for one developer who knows the code base, including host tests, HIL tests and docs.

| Phase | Content | Switch | Flash / RAM (STM32F405 build) | Effort |
|---|---|---|---|---|
| 0 | the probe and a bench session per board family; done for the DM-1701 ([hardware-dm1701.md](hardware-dm1701.md) 21) | `CONFIG_AUDIO_PROBE` (diagnostic) | 6 KB / 2 KB, diagnostic builds only | 1 (bench) |
| 1 | audio router (5.1) with the DMR voice and beep consumers moved onto it; each board's audio tap; APRS can build on it | (part of DMR / FM) | 1-2 KB / 0.5 KB | 4-6 |
| 2 | network audio: USRP on the IP gateway's address, COS and keyup PTT with the watchdogs, the jitter buffer; a USRP reference client (rigctld is already on the radio, [tcp-services.md](tcp-services.md) 6) for tests; HIL test against the SDR bench. **Done** (9.1); the stream sharing it needed is the part of phase 1 that is built | `CONFIG_NET_USRP` (needs `CONFIG_DMR_IP_GATEWAY`) | 4.5 KB / 5.6 KB on the DM-1701 | 6-8 |
| 3 | sound card: the composite personalities with UAC1 (descriptors, alternate settings, resampler, adaptive OUT into the jitter buffer), CDC SERIAL_STATE, RTS/DTR PTT with its arming rules, the Kenwood CAT subset and the host link's second side protocol; HIL test with a host audio loopback | `CONFIG_USB_AUDIO` (needs `CONFIG_USB`) | 9-11 KB / 3 KB | 10-14 |
| 4 | DMR as a voice endpoint over all three transports (5.6), metadata to the host | follows `CONFIG_DMR` | 1 KB | 3-4 |
| 5 | options: audio fob personality (UAC1 + CM108-style HID), SLIP-framed USRP on the serial port or a UART host port (rigctld on the radio: done, `CONFIG_NET_RIGCTL`) | `CONFIG_USB_AUDIO_HID`, `CONFIG_USRP_SERIAL` | 2 / 1.5 KB | 3 + 3 |

Total: about 30-45 working days for phases 1-4 (6-9 weeks), plus phase 5 options. Phases 2 and 3 are independent
after phase 1; phase 4 needs only phase 1 and one of 2 / 3. On a radio with no FM path, phases 2-4 still deliver DMR
over USB.

## 8. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| A radio has no FM path between the MCU and the modem | no FM over USB on that radio | the probe settles it per board family first; DMR over USB still works |
| USRP byte order or field use differs from the public descriptions | no audio with real peers | packet capture against Analog_Bridge / AllStar on the bench before release; the sample order is one build switch |
| Host PTT stuck on | transmitting without control | three watchdogs (6.3) plus the TOT; host tested |
| DTR / RTS toggled by MMDVMHost or a CPS keys the radio | unwanted transmissions | line PTT armed only in audio sessions and with the setting on (6.3) |
| RAM | composite USB plus buffers may not fit with every feature on | buffers in CCM where no DMA touches them; jitter buffer sized by setting |
| Windows binding a stale driver after a personality change | the sound card doesn't appear | a PID per personality (5.2) |
| CM108 emulation not matched by software that checks C-Media IDs | the fob personality ignored by AllStar | the serial + audio personality covers Direwolf and Hamlib; ask AllStarLink to add our ID (they did for AIOC) |

## 9. In the repository now

| Item | Files | Tested |
|---|---|---|
| USRP framing, G.711 mu-law, the link state (keyup PTT, keyup watchdog, TX limit with lockout, sequence accounting) | `core/usbaudio/usrp.h`, `usrp.c` | `tests/host/usrpTest.c` |
| jitter buffer with sample slipping | `core/usbaudio/pcm_slip.h`, `pcm_slip.c` | `tests/host/usrpTest.c` (±200 ppm and 15 ms jitter for 10 minutes) |
| the shared stream (receive ring, transmit jitter buffer, DMR microphone replacement) | `core/usbaudio/audio_mux.h`, `audio_mux.c`; `radio_sm.c` hands the radio's users the wrapped stream | `tests/host/usrpNetTest.c`, `radioFeaturesTest.c` |
| USRP on the network address | `core/usbaudio/usrp_net.h`, `usrp_net.c`; `tools/usrp_client.py` (reference peer) | `tests/host/usrpNetTest.c`, `tests/hil/test_usrp.py` |
| host audio PTT in the radio core | `radio_req_ptt_host()`, `radio_sm.c` (`host_audio`, its watchdog) | `tests/host/radioFeaturesTest.c` |
| audio tap: FM transmit audio from the MCU | `hal/hal_audio_tap.h`; DM-1701 `audio_dm1701.c` (PWM on PC8), `radio_dm1701.c` (`dm1701_fm_tx_inject`) | HIL `test_usrp.py` |
| the probe: registers, PCM levels, HR_C6000 RAM, FM flag watch | `core/debuglink/debuglink_audio_probe.c`, `boards/dm1701/audio_dm1701.c` and `radio_dm1701.c` (`CONFIG_AUDIO_PROBE` parts), `hrc6000_ram()`, `tools/odr_debug.py aprobe` | `tests/hil/test_audio_probe.py` |

Running the probe on a DM-1701: [hardware-dm1701.md](hardware-dm1701.md) 21.4.

### 9.1 USRP on the network (CONFIG_NET_USRP)

What the radio does with the switch on, in the USB network personality (or PPP):

| Item | Behavior |
|---|---|
| Ports | listens on UDP 34001 at its own address; sends from 34001 to UDP 32001 (`USRP_PORT`, `USRP_PEER_PORT`). An AllStar node's `rxchannel = usrp/<radio address>:34001:32001`, or Analog_Bridge with `txPort = 34001`, `rxPort = 32001` |
| Peer | the address of the last valid frame received; before one (or 60 s after the last), the host's address on the link. No setting yet |
| Receive (radio to peer) | a VOICE frame (16-bit PCM) every 20 ms while the radio's receiver is active (`state` rx: FM squelch and tone open, or a DMR call playing), keyup 1; one keyup 0 frame of silence when it closes; nothing in between. Not while transmitting or in hotspot mode |
| Receive audio | FM: the I2S stream's input (the HR_C6000's FM receive audio, squelch gated, after its de-emphasis and filters). DMR: the decoded voice the speaker gets (beeps mixed in) |
| Transmit | a frame with keyup 1 after keyup 0 (or none) keys the radio (`radio_req_ptt_host`); its audio and the following frames' go through a 60 ms jitter buffer. FM: the board's `fm_tx_inject` (refused on a board without `AUDIO_TAP_FM_TX`). DMR: the samples replace the microphone's on the way to the encoder; the call goes to the channel's talk target |
| Unkey | keyup 0; no keyup frame for 300 ms; 180 s in one transmission (then locked out until keyup 0 or a quiet gap); the radio's TOT; the link going down; and, in the radio task, no host audio for 1 s (`AUDIO_MUX_HOST_STALE_MS`: the network service doesn't run while the USB link is in the serial personality). No roger beep. A transmission keeps the source it started with: local PTT pressed meanwhile doesn't switch to the microphone |
| Refused | in hotspot mode, and FM on a board without the transmit tap (counted in `usrp.refused`) |
| Debug link | view section `usrp` ([debuglink.md](debuglink.md) 3.7) |

The DM-1701's FM transmit path is PWM on PC8 into the HR_C6000's differential microphone input (hardware-dm1701.md
21.3.1): MIC_P / MIC_N on and the microphone (LINEIN1) off while the host transmits, TIM8 CH3 at 72 kHz with DMA,
nine carrier periods a sample, linearly interpolated with the rounding error carried from period to period. Full
scale is ±50 timer steps, about 2.6 kHz rms deviation for a full-scale sine before the chip's limiter;
`fm_tx_nominal_peak` (13800) is about 60 % of the peak deviation. The chip's own FM chain does the rest
(pre-emphasis, limiter, sub-audio tone). On the SDR bench a 1 kHz tone at 13800 from the host went out at 1.1-1.2 kHz
rms deviation (1.68 kHz peak) with 13-14 dB SINAD in the voice band (2026-10-13). The probe's plain PWM measured
14-16 dB, so the error feedback doesn't improve it, and the PWM's resolution is probably not the limit.

Known limits:

- The receive audio is the speaker's FM feed-through, so it follows the volume knob (the HR_C6000's RX gain), stops
  at volume zero, and drops out while a beep plays (the feed-through is off for PCM). A tap independent of the
  speaker (the RX RAM, hardware-dm1701.md 21.3.1) would fix all three.
- No flat mode: the PWM path stops at the codec's 3.5 kHz, so 9600 baud needs the 0x04 path or a modification.
- Ports and peer are fixed; settings (UI, qdmr) come with phase 3's settings.
- The wire format is from the public descriptions; a capture against a live AllStar node or Analog_Bridge is still
  owed (owner question 4).

### 9.2 MMDVM FM frames

MMDVMHost can carry FM audio to and from the modem (AllStar linking through its FM network). That frame layout is
not in any public prose description we found (2026-10-13): the protocol write-ups predate FM, and the layout lives in
the GPL modem firmware and MMDVMHost, which the clean-room rules keep us from reading. It can still be specified the
way the rest of [mmdvm-protocol.md](mmdvm-protocol.md) was: by observing what an unmodified MMDVMHost sends to and
expects from a stock MMDVM modem with FM configured (a serial capture on the bench), written up as black-box
behavior. Until then the hotspot only ACKs FM_PARAMS (mmdvm-protocol.md 2.14) and USRP (9.1) is the FM network path.

## 10. Owner questions

1. USB IDs: move to a pid.codes VID 0x1209 block for all personalities now (the existing serial / network PIDs too),
   or keep NXP's VID for serial and network and use pid.codes only for the new ones?
2. Default serial PTT line and polarity: RTS asserted (most software's default) or DTR=1 / RTS=0 (AIOC's choice,
   safe with CHIRP-style programmers)? Either way it only arms in audio sessions.
3. Which personality should a stock radio boot into: serial (today), or serial + audio once phase 3 is done?
4. USRP: confirm the wire format with a capture from the owner's AllStar node or Analog_Bridge before the firmware
   relies on it; which ports and peer address as defaults?
5. ~~rigctld on the radio, or a host bridge?~~ Settled: on the radio (`CONFIG_NET_RIGCTL`, [tcp-services.md](tcp-services.md) 6).
6. DMR voice endpoint: send DMR call metadata (IDs, talker alias) to the host as USRP TEXT frames (Analog_Bridge's
   convention), on the debug link only, or both?

## References

- HR_C6000 user manual (Zhejiang Hongrui; vendor datasheet): 4.5 codec, figure 4.12, 5.3 RAM tables, 6 FM, register
  table (0x06, 0x22, 0x36, 0x37, 0x82, 0xD7, 0xE0-0xE4) and appendix A (FM paths).
- STM32F405 device header (`stm32f405xx.h`, ST, in `third_party/cmsis-device-f4`): OTG_FS endpoint count and FIFO
  size. TinyUSB 0.21.0 (MIT): `src/class/audio`, `src/portable/synopsys/dwc2/dwc2_stm32.h`, `examples/device/cdc_uac2`.
- USB Device Class Definition for Audio Devices 1.0 and 2.0; USB CDC 1.2 (ACM, NCM); USB HID 1.11 (usb.org).
- ITU-T G.711 (mu-law). RFC 1055 (SLIP), RFC 3550 / 3551 (RTP and its audio profile), RFC 4733 (RTP events),
  RFC 5456 (IAX2), RFC 7587 (Opus over RTP).
- Hamlib `rigctld(1)` manual (the NET rigctl protocol); flrig documentation (XML-RPC).
- AIOC, an MIT-licensed USB sound card + serial + CM108-style HID cable on an STM32: <https://github.com/skuep/AIOC>
  (interfaces, PTT lines, rates, ASL3 and Direwolf support as its README states).
- AllStarLink community forum, USRP channel driver set-ups (ports 32001 / 34001 with Analog_Bridge):
  <https://community.allstarlink.org/t/linking-allstar-to-a-mmdvm-hotspot-dmr-to-allstar-node-bridge/18146>;
  M17 project wiki, usrp2m17: <https://wiki.m17project.org/usrp2m17_bridge>; AllStarLink wiki, usbradio channel
  driver: <https://wiki.allstarlink.org/wiki/Chan_usbradio>.
