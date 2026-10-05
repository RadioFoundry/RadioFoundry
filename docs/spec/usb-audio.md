<!-- SPDX-License-Identifier: MIT -->

# Analog and DMR voice over USB: feasibility and design

A computer should be able to hear the radio's receiver, transmit through it, and control PTT, channel and frequency
over USB: as a USB sound card (optionally with a serial port beside it, like the commercial "data radio" interfaces),
over the serial port, or over the USB network personality ([usb-network.md](usb-network.md)). This page is the
feasibility study and the design. It is mostly not built yet. What is in the repository so far: the USRP framing,
the PTT watchdog and the jitter buffer as tested host code (`core/usbaudio`), a shared audio tap interface sketch
(`hal/hal_audio_tap.h`), and a bench probe (`CONFIG_AUDIO_PROBE`) that answers the open hardware question (section
2.3). Section 10 lists them.

## Summary

| Question | Answer on the DM-1701 |
|---|---|
| DMR voice as PCM, both ways | **Yes, today.** The I2S link to the HR_C6000 already carries 8 kHz 16-bit PCM both ways, and the MCU runs the AMBE codec. Decoded calls can go to the host and host audio can be encoded and sent, at no new CPU cost (2.2) |
| FM receive audio to the MCU | **Yes, two ways** (bench, 2026-10-12, 2.3.1). The I2S stream carries it with the stock settings, squelch gated. The HR_C6000's RX RAM carries it ungated, 8 kHz big endian 16-bit, one 256-sample half per 32 ms, with de-emphasis switchable (0x34 bit 5): the voice and flat modes of 5.5, flat from about 300 Hz up |
| MCU audio into the FM modulator | **Not found yet.** None of A, B or the RAM settings tried puts MCU audio on air, though the chip runs its TX RAM handshake at 8 kHz while keyed. The bit that selects the RAM as the source is the open question; AFSK for APRS can use the AT1846S tone generator meanwhile (2.3.1) |
| An MCU ADC on the RX audio line, or a free DAC on the mic line | **No.** ADC1 sees only the volume pot, the battery and a mic level tap; DAC1 drives the PA power, the DAC2 pin is a GPIO (2.1) |
| USB composite with a sound card | **Fits**: CDC-ACM + UAC1 (3 IN, 2 OUT endpoints of the F405's 3 + 3), or CDC-NCM + UAC1; not all three, and not serial + sound card + HID together (3) |
| Recommended protocols | IP: the USRP voice protocol over UDP (AllStarLink, DVSwitch Analog_Bridge, M17 and DMR bridges speak it) with PTT by its keyup flag; Hamlib's rigctld protocol, now on the radio (`CONFIG_NET_RIGCTL`, [tcp-services.md](tcp-services.md) 6). Serial: Kenwood-style CAT plus RTS/DTR PTT. Sound card: UAC1, PTT by the serial port's RTS/DTR (CM108-style HID as an alternative personality) (4) |
| Effort | About 6 to 9 working weeks for everything, in five phases; phase 0 (the probe) is done and needs one bench session (7) |

The DM-32UV has no usable USB (the SoC's controller has no connector: the USB-C socket is on the battery and only
charges it; [hardware-dm32.md](hardware-dm32.md) 3.11); it could carry DMR voice audio over the programming
cable's UART, and its HR_C7000 documents FM sample paths that look usable, unverified (8).

## 1. Use cases and transports

- **Digital modes on FM**: fldigi, WSJT-X, JS8Call, VARA FM, Direwolf (APRS / packet) see a sound card and key PTT
  through a serial port's RTS or DTR, a CAT command set, or a CM108-style HID GPIO.
- **Linking**: AllStarLink, HamVoIP, DVSwitch and similar link a radio to a network as an RF node. They want 8 kHz
  audio, PTT from the network side, and a carrier-operated squelch (COS) signal from the radio.
- **DMR as a USB voice endpoint**: the radio's AMBE codec turns host PCM into a DMR call and DMR calls into PCM, so a
  computer can join a DMR channel on RF with no codec of its own (DVSwitch-like set-ups, dispatch consoles).
- **Remote operation over IP**: the USB network personality already gives the host an IP link to the radio.

Transports, as the task names them: (a) the USB network personality (UDP over CDC-NCM), (b) the USB serial port
(CDC-ACM, also the DM-32's UART), (c) a USB Audio Class sound card, alone or composite with the serial port.

## 2. Hardware: the DM-1701

### 2.1 What the board has

From [hardware-dm1701.md](hardware-dm1701.md) sections 2, 4.4, 8 and 11 and the HR_C6000 user manual (the vendor
datasheet; register table and appendix A in its Chinese section):

| Path | Route | MCU access |
|---|---|---|
| FM RX | antenna, AT1846S, its audio output, into the HR_C6000, FM receive processing there (de-emphasis, sub-audio detection, the audio filters, page 4 0x34), the HR_C6000 codec DAC, line out, the RX audio mux (PD9 high), the speaker amp | none documented: the audio never leaves the HR_C6000 digitally |
| FM TX | microphone (bias PA13), HR_C6000 LINEIN1, its codec ADC, FM transmit processing (compressor, pre-emphasis, limiter, channel filter), two-point modulation outputs | none documented |
| DMR RX | AT1846S baseband, HR_C6000 4FSK demodulator, AMBE frames over V_SPI, MCU codec, PCM over I2S, HR_C6000 DAC, amp | **PCM, 8 kHz, MCU to codec** |
| DMR TX | microphone, HR_C6000 codec ADC, PCM over I2S, MCU codec, AMBE frames over V_SPI | **PCM, 8 kHz, codec to MCU** |
| Beeps, prompts | MCU PCM over I2S to the codec DAC | PCM out |
| Mic level (VOX) | an analog tap on PA3 (ADC1 IN3) | a level, not audio (sampled at about 70 Hz) |

The I2S link (SPI3 as I2S slave on PA15/PC10/PC11/PC12, the HR_C6000 driving the clocks): Philips, 16-bit data in
32-bit frames, both channels, 8 kHz, circular DMA on DMA1 streams 0 and 5 in 20 ms halves
(`boards/dm1701/audio_dm1701.c`, `hal/hal_audio.h`). This is the only audio data path between the MCU and the radio.

No other path exists on this board: ADC1's four inputs are the volume pot (PA0), battery (PA1), mic level (PA3) and
the temperature sensor; DAC channel 1 (PA4) sets the PA power (APC); the DAC channel 2 pin (PA5) is the VHF receiver
5 V switch. The RX audio mux (PD9) chooses between the HR_C6000 line out and the AT1846S audio output toward the
speaker amp only. A hardware modification (a wire from the AT1846S audio output to a free ADC pin) would work for
receive but is not something users should need, and there is no equivalent for transmit.

### 2.2 DMR: works today

The DMR voice path already moves PCM between the MCU and the codec in both directions (`core/dmr/ambe/ambeVoice.c`):
decoded frames are played as 20 ms blocks, and microphone blocks are encoded while transmitting. For a USB voice
endpoint the audio router (5.1) takes the decoded block for the host as well as (or instead of) the speaker, and
feeds host PCM to the encoder instead of the microphone. CPU, codec stack and RAM are what a normal DMR call uses
(the AMBE codec runs for one direction at a time; a DMR radio is half duplex). DMR on this board was verified on air
in both directions ([hardware-dm1701.md](hardware-dm1701.md) 20).

### 2.3 FM: the open question and how to settle it

What the HR_C6000 manual says, in short:

- The built-in codec's input mux takes MIC_P / MIC_N (differential) or LINEIN1 / LINEIN2 (single ended), then a gain
  stage, the ADC and a digital filter; its output path is a gain stage (page 4 0x37), the DAC and LINEOUT1 / LINEOUT2
  (manual 4.5.1, figure 4.12).
- Register 0xE0 (manual register table, Chinese section): bit 7 MCU controls the codec, **bit 6 LINEIN1 enable, bit 5
  LINEIN2 enable**, bit 4 LINEOUT1, bit 3 LINEOUT2, bit 2 MIC_N, bit 1 MIC_P, bit 0 I2S slave. Our 0xC9 (DMR, FM TX) is
  MCU control + LINEIN1 + LINEOUT2 + I2S; 0x89 (FM RX) the same without LINEIN1. So the microphone is wired to
  LINEIN1, and [modem-hrc6000.md](modem-hrc6000.md) 2 calls bit 6 "mic input enable", which is right in effect.
  What LINEIN2 is wired to on the DM-1701 is unknown (no schematic). FM receive audio plays with 0xE0 = 0x89, both
  line inputs off, and the HR_C6000 detects the sub-audio tones, so the receive audio reaches its FM chain through
  the baseband ADC (the path DMR uses), not the codec inputs: [hardware-dm1701.md](hardware-dm1701.md) 8's "line in"
  for FM RX is probably that. LINEIN2 may still carry something (the AT1846S audio output also feeds the RX audio
  mux), which is why it is a candidate.
- 0xE3 bit 0: "codec internal DAC and ADC pass-through enable, normally 0". The direction is not stated.
- The FM transmit chain takes the codec ADC's samples; the FM receive chain ends at the codec DAC (manual 6, figure
  6.1). The I2S "vocoder interface" carries the codec ADC's samples to an external vocoder and the vocoder's PCM to
  the DAC (manual pin table, C_SDI / C_SDO; 0x22 bits 2-1 choose I2S and its master mode). Nothing says the FM chains
  can read from or write to the I2S interface.
- The chip has "FM data" RAM: two 512-byte buffers in the TX RAM ("can be written externally for FM send; voice data
  can also come internally from the codec") and two in the RX RAM (manual 5.3 tables), and FM-mode interrupts in 0x82:
  bit 7 "FM: sending data request", bit 1 "FM: receiving data". Register 0xDB bit 1 selects "send FM voice" over MSK
  signaling. This looks like the MCU can exchange FM audio samples with the chip through its RAM pages, but the
  format, rate and control are not documented. Our FM RX set-up writes 0x81 = 0x19, which masks both FM data flags.
- The FM chain can use an external codec instead of the built-in one (0x06 bit 2, appendix A2), over a second I2S
  port on pins 17-20 with the chip as master. The MCU sits on the other one, the vocoder port (pins 34-37), so this
  only helps if pins 17-20 reach the MCU, which nothing we have shows. The block diagram's "I2S" next to the FM
  transmit chain is this port.
- A "prompt tone" path (0x06 bit 1, appendix A1) lets the MCU write DAC samples over U_SPI without I2S: speaker only,
  but with 0xE3 bit 0 it is another way into the ADC and so the FM transmit chain if the I2S clock stops in FM.
  [modem-hrc6000.md](modem-hrc6000.md) 8.1 has the details of all of these.

### 2.3.1 Bench results (DM-1701, 2026-10-12)

Probe firmware, `tests/hil/test_audio_probe.py`, NOAA WX3 (162.475 MHz) for receive, 467.375 MHz into the SDR bench
for transmit.

| Question | Result |
|---|---|
| I2S clock in FM | runs: 50 blocks a second |
| FM RX on I2S | **yes, with the stock settings** (0xE0 = 0x89): RMS about 2000 on NOAA, both channels the same, exactly 0 with the squelch closed (the feed-through, 0x36, gates the digital stream too). LINEIN1, LINEIN2 and 0xE3 bit 0 only lower it |
| FM RX in the RX RAM | **yes, ungated.** 0x82 bit 1 comes 31.0 times a second once 0x81 unmasks it (0x19 masks it, apart from about 1 a second): one 512-byte half per 32 ms, so 256 samples at 8 kHz. Samples are 16-bit **big endian**. The chip fills a half progressively: read both halves twice and take the one that didn't change (98-100 % of the audio captured at about 47 reads a second over the debug link). NOAA speech: 50 ms envelope CV 0.82; a quiet channel: steady noise, CV 0.06, louder than the signal (noise quieting) |
| What the RX RAM tap sits after | 0x34's processing: with de-emphasis (bit 5) the noise falls 12 dB from 500 Hz to 2.5 kHz, without it it is flat (-1 dB). Bit 7 adds the 300-3400 Hz band pass, bit 2 the wider RX filter. Below 300 Hz stays about 20 dB down in every setting: a fixed sub-audio high pass |
| FM TX from I2S (A, B) | **no.** 0xE3 bit 0 changes nothing. LINEIN2 on (0xE0 bit 5) puts a steady 2.49 kHz tone on air at about 1.7 kHz RMS deviation whether the probe sends nothing, 1 kHz or 600 Hz: something on that input, not MCU audio. Don't enable LINEIN2 |
| FM TX from the TX RAM (C) | **not found.** Keyed, 0x82 bit 7 ("sending data request") comes 31.0 times a second once unmasked: the chip runs the TX handshake at 8 kHz. But a 1 kHz tone in both halves (either byte order) never reaches the air with any of: the flags unmasked, 0xDB bit 1, 0x60 = 0x40 or 0xC0, 0x06 bits 0, 2, 3 or 4, 0xE0 bit 7 off, the codec ADC off (0xE2 bit 4). The chip doesn't clear or rewrite the TX RAM |

What is left for FM transmit: the RAM source bit (a register sweep while keyed, watching the deviation; candidates
the undocumented bits of 0x25 / 0x26, 0xE1, 0xE5 and page 1), the prompt tone path into the DAC with 0xE3 bit 0,
and candidate D. Receive is settled.

Candidates, in order of preference:

| | Receive (FM audio to the MCU) | Transmit (MCU audio to the modulator) | Cost if it works |
|---|---|---|---|
| A | 0xE0 bit 5: LINEIN2 into the codec ADC, if the AT1846S audio (or the speaker line) is wired to it; I2S then carries it as it carries the microphone in DMR | (none) | nothing: same I2S stream |
| B | 0xE3 bit 0 with FM feed-through on: the DAC's signal (FM receive audio) looped into the ADC and out on I2S | 0xE3 bit 0 with feed-through off: I2S PCM to the DAC, looped into the ADC, which feeds the FM transmit chain | nothing: same I2S stream |
| C | FM data RAM (page 2, 0x030-0x42F) read on the "receiving data" flag | FM data RAM written on the "sending data request" flag | 16 KB/s each way over the bit-banged config port (about 1-2 Mbit/s, so roughly 8-16 % of the bus per direction, in 64-byte transactions); FM only, when the modem ISRs don't run. Format, rate and the TX source bit to be found on the bench |
| D | a hardware modification | none | not for users |

If B works it gives both directions with one bit, and keeps the HR_C6000's sub-audio tone generation, limiter,
pre-emphasis and channel filter on transmit (the host audio is processed like microphone audio). If only A works,
the receive side is solved and transmit needs C. If nothing works, FM over USB is not possible on this board without
C, and the product is DMR-only over USB plus AFSK transmit through the AT1846S tone generator (the APRS work package
can send 1200 baud AFSK that way with I2C writes at the bit rate: tone 1 at 1200 / 2200 Hz, as the radio already does
for the roger beep and the 1750 Hz burst, [radio-features.md](radio-features.md) 3.3).

**The probe.** A firmware built with `CONFIG_AUDIO_PROBE=y` adds debug link command 0x10
([debuglink.md](debuglink.md) 3.14): read and write HR_C6000 registers, run the PCM stream with a test tone and
report both I2S channels' levels. `tests/hil/test_audio_probe.py` runs the experiments (section 10 has the steps):

1. Does the I2S clock run on an FM channel? (If not, no FM PCM at all without C. Beeps on FM depend on it too:
   `beep.no_stream` in [radio-features.md](radio-features.md) 4 asks the same question.)
2. Receive: NOAA weather radio in the VFO with the squelch open, then a quiet frequency; for the register settings
   as set, A, LINEIN1 and B, the RMS of both I2S channels. A setting whose RMS rises clearly with the signal carries
   receive audio. It also tells which I2S channel the chip fills (`audio_dm1701.c` assumes the left one, unverified).
3. Transmit (opt-in RF, SDR bench): key FM with the microphone muted (0xE0 bit 6 off) while the probe sends a 1 kHz
   tone into I2S; for "no tone", "tone", B and A, the voice band deviation the bench measures.
4. Candidate C, receive: on NOAA and on a quiet frequency, the rate of the 0x82 FM flags with 0x81 as set and with
   bits 7 and 1 unmasked, whether the RX RAM buffers change, and their level read as 16-bit (both byte orders) and
   8-bit samples.
5. Candidate C, transmit (opt-in RF): both TX RAM buffers hold a 1 kHz tone, a whole number of cycles in 256 samples,
   so a chip that replays them sends a steady tone without refills. Keyed with the microphone muted, for the likely
   source settings (FM flags unmasked, 0xDB bit 1, 0x60 = 0x40 or 0xC0, 0x06 bit 0, big endian samples): the FM flag
   rate and the voice band deviation against a silent-RAM baseline.

### 2.4 Budgets

| Item | Estimate | Basis |
|---|---|---|
| Sample rate on the radio side | 8 kHz, 16 bit, mono | the HR_C6000 I2S word clock; the codec and the FM chains run at 8 kHz |
| Latency, one way | about 60-100 ms | 20 ms DMA block + 20 ms processing slot + a 40-60 ms jitter buffer (pcm_slip target) + 1 ms USB frames; DMR adds the codec's 60 ms burst rhythm |
| CPU | under 5 % of the 72 MHz core for FM audio | resampling 8 to 48 kHz and back with a 48-tap polyphase FIR is about 0.8 M multiply-adds a second each way; framing and the jitter buffer are negligible; the AMBE codec is unchanged from a normal call |
| DMA | none new | I2S already on DMA1 S0 / S5; the OTG_FS core has no DMA (TinyUSB runs it in slave mode), so isochronous packets are copied by the CPU: 1000 interrupts a second per streaming endpoint, about 100 bytes each at 48 kHz |
| USB bandwidth | under 2 % of full speed | 48 kHz mono 16-bit is 96 bytes per 1 ms frame each way |
| RAM | about 3-4 KB | jitter buffer 2 KB (1024 samples), resampler state and two isochronous packet buffers about 0.5 KB, USRP datagram buffer 0.4 KB, UAC class state; the OTG FIFOs are in the peripheral (3) |
| Flash | about 12-18 KB for everything | TinyUSB audio class and descriptors about 6-8 KB, resampler 1 KB, USRP and router 2 KB, CAT parser 2-3 KB (rigctld is already built: `docs/feature-matrix.md`). The DM-1701 default image now leaves about 4.5 KB of its 336 KB free (`docs/feature-matrix.md`, 2026-10-11), so this needs room made first (LTO, about 20 KB, or features off) |

Main RAM was the tight resource when this was written (88 % used; since then the task stacks moved to the CCM); the buffers above can live in main RAM only where DMA
or USB touches them, the rest in CCM.

## 3. USB: what fits

The STM32F405's OTG_FS core has endpoint 0 plus 3 IN and 3 OUT endpoints and 1.25 KB of FIFO RAM
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

**Clocks**: the radio's audio clock is the HR_C6000's 8 kHz word clock (its own crystal), not the USB frame clock.
The IN stream (radio to host) is asynchronous: the device sends 47 / 48 / 49 samples per frame (or 7 / 8 / 9 at
8 kHz) as its clock dictates, which every host handles. The OUT stream is adaptive: the host sends at its rate, and
the firmware's jitter buffer (`core/usbaudio/pcm_slip.c`) absorbs the difference by dropping or repeating one sample
when the fill drifts a block away from its target. At 100 ppm that's one sample every 1.25 s, inaudible on voice and
harmless to AFSK and the FSK modes (host test: 200 ppm either way for 10 minutes with 15 ms of jitter, no underrun,
no overflow). The same buffer serves USRP and serial audio, whose senders run on the host's clock too.

**IDs**: the radio uses NXP's VID with the legacy PIDs today (`boards/dm1701/usb_dm1701.c` says a release should move
to an ID of its own). A composite personality changes the interface set, so it needs its own PID (Windows caches the
driver binding per VID/PID). pid.codes (VID 0x1209) gives open-source projects PIDs for free; the AIOC adapter, an
MIT-licensed STM32 sound card + serial + CM108-style HID cable, does exactly that (1209:7388).

## 4. Protocols

The rule: implement what existing software already speaks; add our own only where nothing fits.

### 4.1 Control (PTT, frequency, channel, squelch state)

| Protocol | Who speaks it | Transport | Pros | Cons |
|---|---|---|---|---|
| **RTS / DTR PTT** on a serial port | fldigi, WSJT-X, JS8Call, VARA, Direwolf, Hamlib, almost everything | CDC-ACM SET_CONTROL_LINE_STATE | trivial, universal | the MMDVM hotspot and the CPS share the port and toggle DTR: PTT must be enabled only in audio sessions (6.3) |
| **Kenwood-style CAT** (TS-480 / TS-2000 subset: `FA`, `FB`, `IF`, `MD`, `TX`, `RX`, `ID`, `PS`, `BY` for busy) | Hamlib, fldigi, WSJT-X, JS8Call, VARA's own CAT, Direwolf through Hamlib | the serial port | the most widely supported CAT set; ASCII, `;`-terminated, small parser | ASCII letters collide with the legacy CPS's request bytes (`R`, `W`, `X`, `C`, `D`): needs a recognizer (two uppercase letters, then digits or `;`) or a session mode (6.4) |
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
AllStar node with a packet capture before release (owner question 6). The GPL implementations were not used as a
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

Serial audio matters most where there is no USB sound card or network personality: the DM-32 (8), and hosts where
installing the network adapter is unwanted. With the composite sound card the serial port carries only CAT and
RTS/DTR.

### 4.4 Choices

| Transport | Audio | Control and PTT | COS / squelch | Works with, unchanged |
|---|---|---|---|---|
| (c) sound card: CDC-ACM + UAC1 | UAC1 mono, 48 / 16 / 8 kHz | RTS/DTR, Kenwood CAT on the CDC port; rigctld through the host bridge | CAT `BY`, CDC SERIAL_STATE DCD (shows as the port's DCD / RLSD line) | fldigi, WSJT-X, JS8Call, VARA FM, Direwolf, Hamlib, any SDR-style app |
| (c') sound card fob: UAC1 + HID | UAC1 | CM108-style GPIO output report | CM108-style input report bit | Direwolf `PTT CM108`, Hamlib CM108 PTT, AllStarLink usbradio / simpleusb once our ID is known to them |
| (a) network: CDC-NCM (+ UAC1) | USRP over UDP (or UAC1 beside it) | USRP keyup; rigctld (host bridge, later on the radio) | USRP keyup | AllStarLink, HamVoIP, DVSwitch Analog_Bridge, usrp2m17-style bridges; Hamlib clients through the bridge |
| (b) serial only (DM-32 UART, or CDC-ACM without audio) | USRP datagrams in SLIP frames | USRP keyup, CAT | USRP keyup, CAT `BY` | the above through the host bridge |

## 5. Design

### 5.1 The audio router (core/audio, new)

Today the DMR voice code (`ambeVoice.c`) owns `hal_audio_t.start()` while in DMR, and beeps start the stream on their
own in FM (`core/radio/beep.c`). With USB audio and APRS (the `core/aprs` work package) there are four consumers of
the one 8 kHz stream, so one module should own it and hand each block to its users:

| Consumer | Takes the stream's input (mic or RX tap) | Writes the stream's output (speaker or TX inject) |
|---|---|---|
| DMR voice | mic blocks to the encoder (TX) | decoded blocks (RX) |
| beeps | | mixed into the speaker block |
| APRS modem | RX tap blocks to the AFSK demodulator | AFSK tone blocks while it transmits (or the AT1846S tones, 2.3) |
| USB / network audio | RX audio (FM tap, or decoded DMR) to the host; the mic is not sent | host audio to the TX inject (FM) or the encoder (DMR) while host PTT is keyed |

The router keeps the stream running whenever any consumer wants it, calls `board.audio_tap->fm_rx_capture()` /
`fm_tx_inject()` (`hal/hal_audio_tap.h`) as the active consumers need, and resolves conflicts by priority: local PTT
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
names the radio ("DM-1701 audio"), so it is recognizable in sound settings. If a host mishandles the radio
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
| Rate | 8 kHz | the source's rate: 24 or 48 kHz from an MCU ADC on the discriminator; 8 kHz where the audio comes from the HR_C6000 |

Where each radio can do it:

- MD-380, MD-9600: flat RX is the native path (the discriminator on PC3, 8.1). At 48 kHz it carries 9600 baud. Flat
  TX goes through the HR_C6000 / C5000 with 0x34's pre-emphasis, compressor and band-pass bits off
  ([modem-hrc6000.md](modem-hrc6000.md) 8, 8.1). Whether the chip's transmit path has a DC-coupled or low-corner
  response (9600 baud needs content down to a few tens of Hz) is a bench measurement.
- DM-1701: receive comes from the HR_C6000's RX RAM (2.3.1) at 8 kHz, so flat is limited to about 300 Hz-3.5 kHz:
  enough for 1200 AFSK and 2400-4800 baud schemes, not 9600. Measured: the tap follows 0x34, so voice is the radio's
  0x34 and flat is de-emphasis (bit 5) and the band pass (bit 7) off; a fixed sub-audio high pass stays. The RAM path
  is ungated, so the squelch gating of 5.4 is done in software from the radio's COS. Flat TX waits for a transmit path.

The setting lives with the other USB audio settings, and the host can change it on the control channel (a CAT
extension and a debug link / rigctld command), so a modem program can switch to flat for a session. `hal_audio_tap_t`
carries it: `fm_rx_capture()` and `fm_tx_inject()` take the mode, and `caps` says which modes and rates a board has
(`AUDIO_TAP_FM_RX_RAW`, `AUDIO_TAP_FM_TX_FLAT`).

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
- TX: the host's audio goes through the same FM transmit chain as the microphone (if candidate B works): pre-emphasis,
  the limiter (page 4 0x3F) and the channel filter keep the deviation inside the channel whatever the host sends. The
  feature unit volume and a firmware gain scale it; `hal_audio_tap_t.fm_tx_nominal_peak` (the sample level that gives
  60 % of peak deviation) is measured once on the bench per band and bandwidth with the probe's tone and the SDR
  bench, like the existing deviation calibration ([hardware-dm1701.md](hardware-dm1701.md) 12.5).
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
uppercase letters followed by digits, a sign or `;`; legacy CPS requests are a letter followed by binary bytes (an
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

| Phase | Content | Switch | Flash / RAM (DM-1701) | Effort |
|---|---|---|---|---|
| 0 | the probe (done, in this branch) and **one bench session**: answers 2.3 | `CONFIG_AUDIO_PROBE` (diagnostic) | 6 KB / 2 KB, diagnostic builds only | 1 (bench) |
| 1 | audio router (5.1) with the DMR voice and beep consumers moved onto it; the DM-1701 tap for whichever candidate won (A or B: a few register writes; C: a RAM-page driver, add 5-8 days); APRS can build on it | (part of DMR / FM) | 1-2 KB / 0.5 KB | 4-6 (+5-8 for C) |
| 2 | network audio: USRP on the IP gateway's address, COS and keyup PTT with the watchdogs, the jitter buffer; a USRP reference client (rigctld is already on the radio, [tcp-services.md](tcp-services.md) 6) for tests; HIL test against the SDR bench | `CONFIG_USB_AUDIO_NET` (needs `CONFIG_USB_NETWORK`) | 3 KB / 2.5 KB | 6-8 |
| 3 | sound card: the composite personalities with UAC1 (descriptors, alternate settings, resampler, adaptive OUT into the jitter buffer), CDC SERIAL_STATE, RTS/DTR PTT with its arming rules, the Kenwood CAT subset and the host link's second side protocol; HIL test with a host audio loopback | `CONFIG_USB_AUDIO` (needs `CONFIG_USB`) | 9-11 KB / 3 KB | 10-14 |
| 4 | DMR as a voice endpoint over all three transports (5.6), metadata to the host | follows `CONFIG_DMR` | 1 KB | 3-4 |
| 5 | options: audio fob personality (UAC1 + CM108-style HID), SLIP-framed USRP on the serial port and the DM-32 UART (rigctld on the radio: done, `CONFIG_NET_RIGCTL`) | `CONFIG_USB_AUDIO_HID`, `CONFIG_USRP_SERIAL` | 2 / 1.5 KB | 3 + 3 |

Total: about 30-45 working days for phases 1-4 (6-9 weeks), plus phase 5 options. Phases 2 and 3 are independent
after phase 1; phase 4 needs only phase 1 and one of 2 / 3. If 2.3 finds no FM path, phases 2-4 still deliver DMR
over USB, and FM is limited to receive (candidate A) or nothing.

## 8. The DM-32UV

- **USB**: the HR_C7000's USB controller has no connector on this radio (the USB-C socket is on the battery and only
  charges it); the host port is the programming cable's UART0 at 115200 baud ([hardware-dm32.md](hardware-dm32.md)
  3.1, 3.11). USB audio and the network personality are out.
- **Serial audio**: 8 kHz mu-law is 64 kbit/s plus framing, about 70 % of a 115200 baud line one way: workable half
  duplex, not full duplex. The UART (16550 style on an 84 MHz APB clock) can run faster if the cable's USB-serial chip
  does (most do 460800 or more); a higher host port rate is a setting both ends must agree on.
- **DMR PCM**: yes, the same way as the DM-1701: the SoC's mic and speaker PCM buffers feed the CPU's AMBE codec
  ([modem-hrc7000.md](modem-hrc7000.md) 1, 6.2).
- **FM**: on this board FM also runs through the modem SoC: the HR_C7000's FM block demodulates and plays receive
  audio through its codec DAC when AUDIO_CONTROL bit 6 is set, and the microphone goes into its codec ADC, with pin
  0x2E choosing the FM or the DMR path ([hardware-dm32.md](hardware-dm32.md) 9). The HR_C7000 documents more than
  the HR_C6000 here: "FM RX data" and "FM TX data" interrupt sources, and AUDIO_CONTROL bits for mic PCM to the CPU
  (bit 4), CPU-fed speaker PCM (bit 5) and the voice source (bit 0: codec or TX RAM)
  ([modem-hrc7000.md](modem-hrc7000.md) 2.4, 8.1). So a PCM path in FM is plausible on the DM-32, unverified; the same
  kind of probe would answer it.
- So the DM-32 can be a DMR voice endpoint over its UART (phase 5, USRP in SLIP frames), FM likely but unverified,
  with the UART's bandwidth as the limit.

### 8.1 The other STM32 radios

| Radio | FM receive to the MCU | FM transmit from the MCU |
|---|---|---|
| MD-380 (HR_C5000) | yes: the GT3136 discriminator on PC3 (ADC1 IN13, also on ADC2 / ADC3), unused so far ([hardware-md380.md](hardware-md380.md) 6.1) | as the DM-1701 (mic into the HR_C5000 codec), but the C5000's FM registers are unconfirmed: needs its own probe run |
| MD-9600 (HR_C6000) | yes: the IF chip's discriminator on PC3 (ADC2 IN13); FM never passes through the HR_C6000 here ([hardware-md9600.md](hardware-md9600.md) 4.2, 6.1) | as the DM-1701: the probe's result carries over |

The discriminator signal is flat (before de-emphasis and the voice filters): voice needs de-emphasis and a 300 Hz high
pass in software; 9600 baud packet and AFSK get the flat path they prefer. Sampled by a timer-triggered ADC at 8-48
kHz on a second ADC, so the existing scan stays as it is.

## 9. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| No FM path on the HR_C6000 (none of A, B, C works) | FM over USB impossible on the DM-1701 family | phase 0 decides it before any other work; DMR over USB still delivers; AFSK TX through the AT1846S |
| Candidate C only | a register-page streaming driver on a bit-banged bus shared with the modem ISRs | measure bus load with the probe; FM only (the modem ISRs don't run in FM) |
| USRP byte order or field use differs from the public descriptions | no audio with real peers | packet capture against Analog_Bridge / AllStar on the bench before release; the sample order is one build switch |
| Host PTT stuck on | transmitting without control | three watchdogs (6.3) plus the TOT; host tested |
| DTR / RTS toggled by MMDVMHost or a CPS keys the radio | unwanted transmissions | line PTT armed only in audio sessions and with the setting on (6.3) |
| RAM (88 % used) | composite USB plus buffers may not fit with every feature on | buffers in CCM where no DMA touches them; jitter buffer sized by setting |
| Windows binding a stale driver after a personality change | the sound card doesn't appear | a PID per personality (5.2) |
| CM108 emulation not matched by software that checks C-Media IDs | the fob personality ignored by AllStar | the serial + audio personality covers Direwolf and Hamlib; ask AllStarLink to add our ID (they did for AIOC) |

## 10. In the repository now

| Item | Files | Tested |
|---|---|---|
| USRP framing, G.711 mu-law, the link state (keyup PTT, keyup watchdog, TX limit with lockout, sequence accounting) | `core/usbaudio/usrp.h`, `usrp.c` | `tests/host/usrpTest.c` |
| jitter buffer with sample slipping | `core/usbaudio/pcm_slip.h`, `pcm_slip.c` | `tests/host/usrpTest.c` (±200 ppm and 15 ms jitter for 10 minutes) |
| audio tap interface for USB audio and APRS (sketch; the probe part implemented) | `hal/hal_audio_tap.h`, `board_t.audio_tap` | builds |
| the probe: registers, PCM levels, HR_C6000 RAM, FM flag watch | `core/debuglink/debuglink_audio_probe.c`, `boards/dm1701/audio_dm1701.c` and `radio_dm1701.c` (`CONFIG_AUDIO_PROBE` parts), `hrc6000_ram()`, `tools/odr_debug.py aprobe` | `tests/hil/test_audio_probe.py` |

`core/usbaudio` is not built into the firmware yet (no switch selects it); phases 2 and 3 will add it.

**Bench steps (phase 0)**, on a DM-1701 with the AMBE codec merged as usual:

```
make BOARD=dm1701 CONFIG_AUDIO_PROBE=y BUILD=build/dm1701-probe   # flash build/dm1701-probe/odr_dm1701.bin
python3 -m pytest tests/hil/test_audio_probe.py -v -s -rs          # stream in FM, the receive variants (NOAA), RX RAM
python3 -m pytest tests/hil/test_audio_probe.py -v -s -rs --allow-rf --allow-service-stop -k tx   # I2S and RAM TX variants
```

By hand (an FM channel or the VFO):

```
tools/odr_debug.py aprobe start            # stream with silence out; then:
tools/odr_debug.py aprobe status           # blocks counting up = the I2S clock runs in FM
tools/odr_debug.py aprobe reg 4 0xE0       # 0x89 expected in FM receive
tools/odr_debug.py aprobe wreg 4 0xE0 0xA9 # LINEIN2 on (candidate A); status / samples with and without a signal
tools/odr_debug.py aprobe wreg 4 0xE0 0x89
tools/odr_debug.py aprobe wreg 4 0xE3 0x53 # candidate B (0xE3 is 0x52 at init: bit 0 set); watch status
tools/odr_debug.py aprobe wreg 4 0xE3 0x52
tools/odr_debug.py aprobe samples -o /tmp/blk.raw   # one block of both channels, tone estimate printed
tools/odr_debug.py aprobe stop
tools/odr_debug.py aprobe wreg 4 0x81 0x9B # unmask the FM data flags (0x19 | 0x82)
tools/odr_debug.py aprobe watch 1000 0x82 0x030   # FM flag rates, RX RAM word changes
tools/odr_debug.py aprobe ram 2 0x030 64   # the start of the RX FM buffer
```

Expected readings: in FM receive with the defaults, both channels near silence (the microphone is off). A candidate
works for receive if, with NOAA audible and the squelch open, one channel's RMS rises well above (4x or more) its value
on a quiet frequency. For transmit, the SDR bench's voice band RMS deviation with the tone and the candidate setting
is well above the "mic muted, no tone" row.

## 11. Owner questions

1. If only DMR works over USB (no FM path), is the feature still wanted for the DM-1701 family, or should FM-capable
   radios (another board family) come first?
2. Candidate C (FM audio through the HR_C6000 RAM pages) costs a week or more of reverse engineering: worth it if A
   and B fail?
3. USB IDs: move to a pid.codes VID 0x1209 block for all personalities now (the existing serial / network PIDs too),
   or keep NXP's VID for serial and network and use pid.codes only for the new ones?
4. Default serial PTT line and polarity: RTS asserted (most software's default) or DTR=1 / RTS=0 (AIOC's choice,
   safe with CHIRP-style programmers)? Either way it only arms in audio sessions.
5. Which personality should a stock radio boot into: serial (today), or serial + audio once phase 3 is done?
6. USRP: confirm the wire format with a capture from the owner's AllStar node or Analog_Bridge before the firmware
   relies on it; which ports and peer address as defaults?
7. ~~rigctld on the radio, or a host bridge?~~ Settled: on the radio (`CONFIG_NET_RIGCTL`, [tcp-services.md](tcp-services.md) 6).
8. DMR voice endpoint: send DMR call metadata (IDs, talker alias) to the host as USRP TEXT frames (Analog_Bridge's
   convention), on the debug link only, or both?

## References

- HR_C6000 user manual (Zhejiang Hongrui; vendor datasheet): 4.5 codec, figure 4.12, 5.3 RAM tables, 6 FM, register
  table (0x06, 0x22, 0x36, 0x37, 0x82, 0xDB, 0xE0-0xE4) and appendix A (FM paths).
- AT1846S datasheet 0.3 and programming guide 1.4 (via [hardware-dm1701.md](hardware-dm1701.md) 5).
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
