<!-- SPDX-License-Identifier: MIT -->
# Retevis RT10 hardware

A DMR and FM handheld for 902-928 MHz on the HR_C7000 SoC, with an FD6818 RF chip, no display and a 16-position channel
knob. A value marked `(measured)` was checked on a radio running RadioFoundry, `(unverified)` is not confirmed on a radio
or in a datasheet, and `(?)` is not determined. Unmarked values come from the radio's observed operation and the board's
markings.

## 1. Chips

| Part | Marking | Role |
| --- | --- | --- |
| SoC | HR_C7000 | C-SKY CK803S core and DMR baseband |
| RF transceiver | FD6818 | receive and transmit, on a two-wire bus (section 7) |
| Reference | 26 MHz TCXO | the FD6818's reference; the modem's DC bias sets its frequency (section 4.1). Marking unverified |
| SoC crystal | unmarked | 24 MHz (unverified: the PLL settings give 168 MHz only from 24 MHz) |
| Program flash | GD25Q16C, JEDEC C8 40 15 (measured) | 2 MB, execute in place at 0x03000000 |
| Data flash | GD25Q16C | 2 MB on SPI0 (section 4) |
| Audio | NJM2904 dual op amp | analog, no control lines |
| Clock | none found | no RTC crystal or backup cell identified |

## 2. Boot and program flash

There is no vendor loader. After reset the boot ROM offers its serial download window on UART0 (the accessory jack's
programming cable; measured), then reads the flag at flash 0 and jumps to the address it names.

| Range (flash offset) | Content |
| --- | --- |
| 0x000000 | ROM flag `74 68 64 0C` (`thd\x0c`), then the entry address, little endian |
| 0x001000-0x00BFFF | unused by RadioFoundry |
| 0x00C000 | the RadioFoundry application (linked for 0x0300C000) |
| 0x0E8000-0x0FFFFF | the codec slot |
| 0x100000-0x1FFFFF | unused |

- RadioFoundry's flag names 0x0300C000. `stub/stubtool.py --write-app` writes the flag sector after the application when
  the board profile has `rom_entry` (board.toml `[flash.stub]`), so the radio keeps starting whatever was there until
  the new image is complete.
- The original image starts at 0x1000 and runs to about 0x78000, so installing RadioFoundry overwrites it. Only a full
  2 MB backup (`stubtool --progmem FILE` with the board profile) puts it back.
- The boot ROM's window is the recovery path: with the radio off, `stubtool PORT --write-app IMAGE`, then switch it on.

Start-up state: the boot ROM leaves the SoC on the 24 MHz oscillator and the XIP controller in single-bit mode. The
platform's clock set-up (platform/hrc7000/clock.c) starts the PLL at 168 MHz CPU and 84 MHz APB.

## 3. Pins

Index = GPIO pin index (0-31 PTA, 32-63 PTB, 64-95 PTC). The IO mux stays at its reset default except SPI0 (IO_DIPLEX1
bits 8:3 cleared, bits 29:27 set).

| Pin | Index | Use | Level |
| --- | --- | --- | --- |
| PTA0-PTA4 | 0-4 | JTAG (unverified: the unpopulated 7-pad header) | |
| PTA5 / PTA6 | 5 / 6 | a second two-wire bus, no device found | |
| PTA7 / PTA8 | 7 / 8 | FD6818 clock / data | bit-banged |
| PTA18 | 18 | data flash chip select | low = selected |
| PTA20-22 | 20-22 | SPI0 clock, MOSI, MISO (unverified) | |
| PTB0 | 32 | TX supply | high = on |
| PTB1, PTB2, PTB6, PTB21 | 33, 34, 38, 53 | channel knob code bits 2, 1, 3, 0 | |
| PTB3 | 35 | PA select | high = on |
| PTB4 | 36 | RX supply | high = on |
| PTB5, PTB7 | 37, 39 | LEDs (which color: ?) | high = on |
| PTB8 | 40 | voice prompt chip enable | high = on |
| PTB9, PTB10 | 41, 42 | side keys | low = pressed (unverified) |
| PTB11 | 43 | PTT; also the cable's serial line | low = pressed |
| PTB12 | 44 | power latch | high = stay on |
| PTB13 | 45 | audio amplifier | high = on (unverified) |
| PTB17 | 49 | power switch sense | high = off (measured) |
| PTB26 | 58 | FD6818 GPIO6, its squelch output (measured) | high = open; low with PTB27 high: a held match (section 7) |
| PTB27 | 59 | FD6818 GPIO0, the sub-audio compare result (measured) | high = the tone or code is heard |
| PTB28 / PTB29 | 60 / 61 | voice prompt chip clock / data | |
| DAC B | | PA control, after IO_DIPLEX2 bit 28 is cleared | 0 = off |
| DAC C | | routed to its pin (IO_DIPLEX2 bit 29), 1762 at start-up: the modem's AF ADC bias (7.1) | |
| ADC channel 2 | | battery | divider: ? |
| UART0 | | programming cable, 115200 8N1 under RadioFoundry | |

The transmitter is off with PTB0 and PTB3 low, PTB4 high and DAC B at 0 (`board_rf_safe`).

## 4. Data flash (2 MB, SPI0)

SPI0, mode 0, chip select PTA18 by GPIO. Read 0x03, page program 0x02, 4 KB erase 0x20, status 0x05, write enable 0x06.

| Range | Content |
| --- | --- |
| 0x001300-0x031FFF | the original firmware's settings and channels (kept, never written by RadioFoundry) |
| 0x100000-0x1FDFFF | RadioFoundry: settings, journal, open-firmware codeplug images, native codeplug (`boards/rt10/board.c`) |
| 0x1FE000-0x1FEFFF | activation block tied to the SoC's eFuse ID: never written |
| 0x1FF000-0x1FFFFF | factory calibration (section 4.1): never written |

### 4.1 Factory calibration

Byte tables of 15 entries, one per 10 MHz step. The step for a frequency is (frequency / 10 MHz) minus the base byte,
held between 0 and 14. Offsets are from 0x1FF000. 0xFF is an entry the factory didn't fill. RadioFoundry copies the
tables at start-up (`boards/rt10/calibration_rt10.c`) and transmits only with a valid base.

| Offset | Content |
| --- | --- |
| 0x1A9 | the modem's TX center (SIG_CENTER), one byte: the TCXO's tune (below) |
| 0x470 | base: the first step's frequency / 10 MHz (90 on the radio checked; 80-95 accepted) |
| 0x51D / 0x52C | PA control level / 16 (DAC B), high / low power |
| 0x559 | 15 pairs: the first byte the modulation balance (the modem's MOD_BIAS), the second not used |
| 0x5B5 / 0x5C4 | FD6818 PA gain (0x28), high / low power; the high one is capped at 7 |
| 0x5D3 / 0x5F1 | key-up delay / 10 us, high / low power: RX supply off to the TX state |
| 0x600 | key-down delay / 10 us, before the ramp down |

The TX center and the step's modulation byte go into the modem's SIG_CENTER and RF_MOD_BIAS_CTRL at start-up and at
every receive set-up, as on the other HR_C7000 radios (`boards/rt10/radio_rt10.c`). The modem's MOD1 DC level is the
TCXO's control voltage, so these bytes set the frequency of receive and transmit alike. With the modem never powered,
the radio measured 28.2 ppm low: its carrier at 927.4738 MHz for 927.5, and its receiver's peak 25 kHz low. With them
written, the carrier measured +0.47 ppm (section 5) and the receiver peaks on the bench's frequency.

## 5. Power and transmitter

| Item | Value |
| --- | --- |
| Power latch | PTB12 high from start-up; low lets the radio turn off |
| Power switch | PTB17, high = off (measured). The original firmware lets go of the latch after it reads "off" for 500 ms; RadioFoundry after five 100 ms polls (`power_rt10.c`) |
| Battery | ADC channel 2; scale ? |
| PA | DAC B drives the PA's power control; levels per frequency step come from the calibration (section 4.1) |
| Power levels | L and H: the calibration's low and high sets |

FM key-up, in the original firmware's order. Measured: the chip's TX state and frequency, the pin levels and the
signal on air (927.5 MHz at L, the modem's bias written as in section 4.1: the carrier 436 Hz high, +0.47 ppm, on
the SDR bench, whose own correction is good to about 0.5 ppm).

1. The FD6818's TX words while it still receives (0x40 0x3AD0 with or without sub-audio, the sub-audio, PA gain 0x28
   with the gain in both 3-bit fields), the speaker off, PTB3 and PTB0 low.
2. The key-up delay, then PTB4 (RX supply) low.
3. The FD6818 to the TX frequency, idle, then TX state 0xC1FE (0xC1FA for a tone).
4. DAC B powered, PTB3 and PTB0 high, DAC B ramped to the calibration's level: 180 steps of 2 us on a smoothstep curve.

Key-down: the FD6818 back to receive, the key-down delay, the ramp down, DAC B 0, PTB0 and PTB3 low, DAC B powered
down, 100 us, the receiver retuned, PTB4 high.

## 6. Keys and knob

Two side keys and PTT are direct inputs. The 16-position channel knob gives a 4-bit code on four inputs; the code is not
the position in order (mapping ?).

## 7. FD6818 bus

A two-wire bus (I2C style) on PTA7 (clock) and PTA8 (data), device address 0xE2 write / 0xE3 read, 16-bit registers,
bit-banged (measured). The register map is the FD6818's (`drivers/rf/fd6818/fd6818sh.h`): state 0x03, page select
0x7F. On this bus page 1's registers are addressed from 0x80 (0x7F = 1, then register | 0x80).

| Item | Value |
| --- | --- |
| ID (0x00) | 0x6818 (measured) |
| States (0x03) | receive 0xBFF1 (reads back 0xBFF5), TX 0xC1FE, TX with a tone 0xC1FA; every change through 0 |
| Band | 902-928 MHz is the chip's band above 600 MHz: 0x1A = 0x1F80, 0x40 = 0x3AD0 (measured: -34 dBm read from a signal the band word 0xBF24 left at the floor) |
| Bandwidth (0x43) | 0x4048 at 12.5 kHz, 0x3028 at 25 kHz |
| Receive set | the power-on tables and the analog words in `boards/rt10/radio_rt10.c`, from the original firmware's |
| AF output (0x47) | 0x7140 receive, 0x7040 mute (TX) |
| GPIO | 0x25-0x27: GPIO0 the sub-audio compare, to PTB27; GPIO6 squelch, to PTB26 (measured) |
| Noise (0x65) | no signal: 38-44 raw at 904-927.5 MHz (measured), inside the core's squelch range, so the driver adds the FD6818 offset (`fd6818_noise`) |

Sub-audio decode (measured with CTCSS 100.0 Hz and DCS 023N): the right tone or code raises PTB27, a wrong one and no
carrier leave it low. A match with CTCSS is held: PTB27 stays high after the carrier (9 s watched) and through the next
carrier, even one with another tone (131.8 Hz after 100.0 Hz, 4.6 s). Writing the sub-audio word (0x51) again clears
it, and a new match then takes about 120 ms. The driver counts PTB27 only with PTB26 (squelch) high, and writes the
sub-audio set again when it finds PTB26 low with PTB27 high. The chip doesn't tell DCS polarity apart: 023N is heard with 023 set normal or inverted.

### 7.1 DMR

The original firmware's scheme: the HR_C7000's 4FSK goes on the TCXO through MOD1 (section 4.1), and each burst
runs the FM key-up and key-down, started by the modem's RF TX interrupt. RadioFoundry runs the bursts from the shared
RF-burst task ([modem-hrc7000.md](modem-hrc7000.md) 3.1a) with the sequences in `boards/rt10/radio_rt10.c`. Values
from the original firmware's code:

| Item | Value |
| --- | --- |
| FD6818 digital set | the stock's DMR entry: 0x04 = 0x0300, 0x48 = 0x6C5C, 0x4B = 0xE442, 0x73 = 0x569A (AFC off), 0x7E = 0x349E (TX DC filter bypassed), 0x23 = 0x8F87 (AF DAC level 7), then 0x43 = 0x7E08. The FD6818 guide's DMR bypass mode lists the same 0x48, 0x4B, 0x73 and 0x7E words ("cic mode"). Leaving DMR, the analog set writes the power-on words back (0x48 = 0x6C4C) |
| 0x4B | 0xE442 puts the demodulated 4FSK on the AF output. With the power-on 0x223C the modem's PHASE_OUT range stays within about ±40 counts of its center during a call (a few millivolts at its AF ADC) and the modem never syncs; with 0xE442 it reaches the ±2432 limit (measured) |
| AF output (0x47) | receive 0x4140 (to the modem; analog 0x7140); at a burst's TX 0x6740, written before the tune |
| 0x40 | 0x3AD0: the band select in every tune writes it, so a burst transmits with it (the stock's 0x40 = 0 ahead of the tune is overwritten) |
| States (0x03) | receive 0xBFF1, TX 0xC1FE, each through 0 |
| Modem RF_MODE | 0x034C9860: AF gain 0x4C, TX symbol mapping not inverted (bit 7 clear) |
| RF TX interrupt lead | RF_CONTROL b13:8 = 0x1A (2.6 ms) |
| Modem ADC gain (GCR_ADCL) | 11 dB, the other HR_C7000 radios' value; the original's is not read. `make RT10_DMR_MIC_DB=N` |
| DAC C | the modem's AF ADC bias (its N input): each step moves the PHASE_OUT center about 9 counts. The stock keeps 1762; RadioFoundry writes `RT10_DMR_DAC_C` (1850) entering DMR and 1762 leaving it. AMBE bits corrected per frame on a bench voice call (234 frames each, measured): 1762 0.83-0.86, 1800 0.51, 1825 0.45, 1850 0.05, 1875 0.09, 1900 0.16, 1950 0.50, 2050 0.89; at 1680 no sync. `make RT10_DMR_DAC_C=N` |

Burst TX on, from the RF TX interrupt: the calibration's key-up delay (section 4.1), PTB4 (RX supply) low, the
FD6818 to the TX frequency in its TX state, PTB3 and PTB0 high, DAC B ramped to the calibration's level. TX off: the
ramp down, PTB0 and PTB3 low, 100 us, the FD6818 back to the receive frequency (0x4140, 0xBFF1), 100 us, PTB4 high.
The original starts the key-down at its RF RX interrupt plus the calibration's key-down delay; RadioFoundry holds
the TX until the burst's last symbol instead.

Measured on the SDR bench (927.5 MHz, lowest power, 2026-11-06):

| Test | Result |
| --- | --- |
| DMR transmit, 6 s key-up, decoded by the bench | 106 bursts at 60.0 ms: 3 LC headers, 17 superframes, terminator; CC 2; group voice from the radio's ID; every FEC check passed, 0 AMBE bits corrected; deviation 1904-1913 Hz, frequency error within ±60 Hz. The diagnostics line: 106 keyed, 106 ended, none late |
| DMR receive, a bench group call of recorded speech (78 voice bursts, 234 AMBE frames) | the call decoded (source, talkgroup, group, CC 2); 78 of 78 voice bursts, 3 headers, 2 terminators, no CRC errors; 234 of 234 frames played, none concealed or lost, 4 and 5 bits corrected on two calls; PCM peak 16707 |

### 7.2 APRS

The FD6818 has no AFSK path of its own, so APRS goes through the HR_C7000 (`platform/hrc7000/hrc_afsk.c`):

| Direction | Path |
| --- | --- |
| Transmit | The FD6818 keys up in its TX state, then goes to its tone state with no tone (0xC1FA: carrier on, microphone ADC off, `fd6818sh_mic_off`). The HR_C7000's FM transmitter plays the AFSK from its prompt FIFO onto MOD1 (the TCXO, as DMR does), with the codec microphone muted. Level: `amp_per_level` 55 in `boards/rt10/radio_rt10.c`, not yet set against a measured deviation |
| Receive | The HR_C7000 in FM receive copies the FD6818's AF output (0x47 = 0x7140) to the CPU (`fm_rx_audio.c`). The AF output stays on while APRS listens; the modem's FM_PLAY stays off |

The transmitter keys up and the frames go out (`aprs.block` ok). Deviation, twist, the decode of the frames and
receive are not yet measured on air.

## 8. Codec

The AMBE+2 vocoder is part of the original image: its code runs from RAM, its tables are copied to modem SRAM. No
vendor file is published, so the codec comes from a read of the radio's program flash (V1.1.2, the build name in the
image). RadioFoundry hosts it in the codec slot like the other HR_C7000 radios (`boards/rt10/memory.ld`): the firmware
copies the code to the codec window and moves the words its codec map lists ([codec-placement.md](codec-placement.md)).
No codec bytes are in the repository or in a release.

| Item | Value |
| --- | --- |
| Code start (the two signature words) | flash 0x0305E438 |
| Code | 0x12FFC bytes (77,820), sha256 `3f3b8740df14c81b44f53f984e4e570a609ec467233341729157f5c176cc3c15`. The window ends where the stock `memcpy` starts |
| Code run address | 0x16DD8: the start-up code copies flash 0x03057A60 to RAM 0x10400..0x2E324 |
| Compile | The DB25-D 020 compile, with one stock function inside the window (at +0x614) 20 bytes shorter: the codec code past it sits 20 bytes lower. The same 153 functions are reachable from the entry points |
| Entry points (offsets) | decode 0, encode 0x294, dec_init 0xE8, dec_set 0xA4, enc_init 0x400, enc_set 0x32C, fec_init 0x1CD0 |
| Init | The stock init routine calls them in the order, with the arguments and the state offsets, that `platform/hrc7000/vocoder.c` uses |
| Tables | flash 0x0303C76C, 0x2D50 bytes (11,600), sha256 `e1e435822471894c437048396f2ea96bea5803acc08446b8f1073714978e219e` (the DB25-D's tables). The start-up code copies flash 0x03038D60 to modem SRAM 0x18000800, so they run at 0x1800420C..0x18006F5C |
| Codec map | `rt10-112` (`tools/rfoundry/rfoundry/codec_maps/rt10/`): 3 self-referencing words (one reachable from the entry points), 69 table words (52 targets), no instruction pairs, no references outside the code and table windows. Measured: the merged codec decodes bench speech and encodes the radio's transmissions (7.1) |

`memory.ld` reserves modem SRAM 0x1800420C..0x18007184, 0x228 bytes more than the tables. `tools/hrc7000_codec.py
--board rt10 --stock READ` merges the codec into a built image; READ is a 1 MB or the whole 2 MB program flash read
(`stub/stubtool.py --progmem`), and the codec is found by its hashes. The slot's stock tag is 112.

## 9. Bring-up state

| Item | State |
| --- | --- |
| Boot from the ROM flag at 0x0300C000 | written |
| Host link on UART0, stub entry, watchdog, crash log | measured: the hang and stall checks keep their records and the radio restarts by itself (`test_crash.py`). The restart is a watchdog reset (2026-11-06): a reboot answers again after 3 s, and every crash and watchdog test passes through it (`test_crash.py`, `test_watchdog.py`, 12 of 12) |
| Battery scale, LED colors, key levels, knob map | open |
| FD6818 two-wire bus, FM receive, sub-audio decode | measured |
| Factory calibration read | measured (the radio's own sector) |
| FM transmit | measured on air (section 5) |
| Battery save | measured: asleep is the FD6818 idle and PTB4 (RX supply) low; waking retunes the receiver |
| Reference | measured: the modem's bias from the calibration puts receive and transmit on frequency (section 4.1) |
| Reference trim | written: the trim moves the synthesizer frequency at the next tune; the modem's bias stays the factory's tune. Untested on air |
| DMR transmit and receive, codec | measured on air (7.1): voice both ways with the codec map `rt10-112` (section 8) |
| DMR speaker audio, DMR microphone | open: decoded voice played (the counters) but not listened to; in a DMR key-up the vocoder's microphone level is a steady floor (peak 2946, spread 3 dB), speech not checked |
| IP data over the air | measured on air: UDP to and from the radio, compressed and full headers, off-net, confirmed, bursts from the radio |
| IP link to the computer | written: PPP on the host link (UART0, 115200 baud). Not working yet: twice the radio went silent soon after pppd's LCP opened, before IPCP, and answered nothing (PPP, debug link, boot ROM) until a power cycle, with no LED activity. The likely end: a crash or hang restart, then a start that hung. The restart was a soft reset then, which turns the watchdog off ([watchdog.md](watchdog.md) 3), so nothing reset the radio again; it is now a watchdog reset. The first fault is open |
| APRS | written (7.2): the radio keys up and sends its frames; deviation, twist and decoding on air not yet measured |
| Hotspot split, USRP | not written: no continuous base-station outbound; USRP's 8 kHz audio both ways needs more than 115200 baud |
| Screen-only features | left out: no display (`boards/rt10/config.mk`) |
| Microphone through the programming cable's line | measured: barely couples. FM key-ups on the SDR bench with 0x55 bursts at 6000 and 2400 baud on the line: the audio rose from about 5 Hz rms deviation to 6-8 Hz rms during the bursts, no tone. `tests/hil/test_mic_uart_inject.py` leaves the RT10 out |
