<!-- SPDX-License-Identifier: MIT -->
# Radioddity DB25-D PRO hardware

Status: research, 2026-10-12. The facts a fresh firmware needs to bring up the DB25-D PRO mobile: SoC, memory, pins,
the two RF chips, display, storage, calibration, the PC link and the update path. The survey entry, with sources for
the model and FCC facts, is in [radios.md](radios.md). The HR_C7000 itself (UARTs, timers, ADC, DAC, PIC, GPIO) is in
[hardware-dm32.md](hardware-dm32.md) and its modem in [modem-hrc7000.md](modem-hrc7000.md); this page only says
where the DB25-D differs.

**Nothing here has been checked on a radio.** The owner has a DB25-D PRO; its firmware version (batch code) hasn't
been read yet.

## Confidence tags

| Tag | Meaning |
|---|---|
| **BIN** | Observed in the stock application images by disassembly (facts only: addresses, register values, pin numbers, strings; no code carried over). Six images, 2022-10 to 2025-01 |
| **FCC** | FCC internal photos of the DB25-D PRO (2AN62-DB25DPRO) |
| **COM** | Radioddity manuals, release notes and update guide |
| **DM32** | The same fact as on the DM-32UV ([hardware-dm32.md](hardware-dm32.md)) |
| **INF** | Inferred by us; not seen directly |

---

## 1. Models and batches

| Item | Finding | Tag |
|---|---|---|
| SoC | One HR_C7000 (CK803S core), marked "HR_C7000 D56323MP1" | FCC |
| Boards (PRO) | Control "CDR300UV-MCU-BT-AI-TDA-1.0", display "C7DR300UV-TFT-51M-FD-2.0", RX "C7DR300UV-RX-BT-AD-3.0", TX "C7DR300UV-TX-RD35-BT-AD-3.5" | FCC |
| One image for every board | The firmware identifies both RF chips at boot (section 4.2) and writes two letters after the "D" of its version string | BIN |
| Batch codes | D64, D66, D6T: FD6818 main transceiver (2021-2023). **DBB: two BK4819, the PRO** | BIN, COM |
| Minimum stock firmware on a PRO | The 2024-05-09 build is the first that knows the BK4819. Older stock images must never be loaded on a PRO | BIN |

## 2. Memory and image

| Item | Finding | Tag |
|---|---|---|
| Program flash | XIP at 0x03000000. Larger than 1 MB: the stock firmware keeps data up to at least 0x031FF000. Part and size: ? | BIN |
| Loader | 0x03000000-0x0300BFFF (48 KB), the vendor IAP loader. It's never in an update package | BIN, INF |
| Application | Linked at **0x0300C000**; the loader jumps to its first byte. Raw image: no header, no checksum, no encryption | BIN |
| Start-up state | PSR 0x80000100, SP 0x50000, VBR 0x10000, cache set as on the DM-32 | BIN, DM32 |
| SRAM | 0x10000-0x57FFF as on the DM-32. The stock image uses 0x10000-0x525E0 | BIN, DM32 |
| Modem SRAM | The stock image copies its modem and vocoder tables to 0x18000800-0x18004F90 | BIN |

## 3. Pin map

GPIO index as on the DM-32: 0-31 PTA, 32-63 PTB, 64-95 PTC (DM32). Levels are "high = on" unless stated.

### 3.1 Buses

| Bus | Pins | Protocol | Tag |
|---|---|---|---|
| Display | PTC2 (66) reset, PTC3 (67) CS, PTC4 (68) SCL, PTC7 (71) SDA, PTC6 (70) D/C (low = command) | 4-wire serial, 8 bits MSB first, data set while SCL is low and taken on the rising edge. ADC channel 2 picks how the stock firmware drives the pins: reading ≥ 50, bit-banged GPIO; below 50, the SPI2 controller (0x140E0000) on the same pins | BIN |
| SPI NAND | SPI0 (0x140A0000), CS PTA18 (18) by GPIO | Mode 0, 8-bit frames, BAUDR 4 (21 MHz) | BIN |
| RF chips | SCLK PTB16 (48), SDIO PTB24 (56) bidirectional, SEN main PTB25 (57), SEN LO PTC20 (84) | 3-wire, as the DM-32's FD6818: 8-bit address (bit 7 = read), 16 data bits, MSB first. One lock around every access | BIN |
| RF chip lines | PTB27 (59) input with interrupt; PTB26 (58) input | ? (interrupt / sub-audio detect, status) | BIN, INF |
| FM broadcast (RDA5802N) | Bit-banged I2C, SCL PTA7 (7), SDA PTA8 (8), 7-bit address 0x11 | | BIN, FCC |
| RTC | SoC I2C2, address 0x70 | | BIN, DM32 |
| PC link | **UART1** (0x14040000), RXD PTA9 (9), TXD PTA10 (10), 115200 8N1. The stock debug output goes to the same port | | BIN, COM |
| GPS | UART2 (0x14050000), 9600 baud; power PTB17 (49) | | BIN |

The DM-32's programming cable is on UART0. The DB25-D application never uses UART0 (BIN).

### 3.2 RF switching and the PA

| Pin | Use | Tag |
|---|---|---|
| PTC19 (83) | RX front end, on in both RX paths | BIN |
| PTB4 (36) / PTA25 (25) | RX path UHF / VHF | BIN; band by analogy with the DM-32 (INF) |
| PTB3 (35) / PTA26 (26) | TX path (PA select) UHF / VHF | BIN; band INF |
| PTA23 (23) | High in RX, low in TX | BIN |
| PTB2 (34), PTA27 (27) | Low in RX, raised 200 us into TX (TX supply) | BIN |
| DAC B | PA power control. Ramped over 180 steps of 4 us at the start and end of each burst | BIN |
| DAC C | 0x06E2 at init, as on the DM-32 | BIN, DM32 |

To TX: RX paths off, 50 us, PA power ramp up, PA select of the band, PTA23 low, 200 us, TX supply on. To RX: TX supply
off, PTA23 high, RX path of the band (BIN).

### 3.3 Keys, encoder, power, LEDs, audio

| Pin | Use | Tag |
|---|---|---|
| PTB18, PTB20, PTB21, PTB22, PTB23 (50, 52-55) | Key matrix drive lines, driven low one at a time | BIN, INF |
| PTB9, PTB10, PTB11 (41-43) | Key matrix sense lines, low = closed | BIN, INF |
| PTA9 (9) | Also read as a GPIO by the key code: the mic connector's line that carries PTT and the radio's RXD | BIN, COM |
| PTC11 (75) | Input read by the key task (mic sense ?) | BIN |
| PTB5 / PTB8 (37 / 40) | Rotary encoder A / B, GPIO B interrupt | BIN |
| PTB12 (44) | Power key sense | BIN, INF |
| PTB13 (45) | Power latch: set at start-up, cleared to switch off | BIN |
| PTA17 (17) | Output, set at init (the DM-32's power latch pin; role here ?) | BIN |
| PTB0, PTB1 (32, 33) | Status LEDs | BIN, INF |
| PTB14, PTB15 (46, 47) | Audio path selects (the DM-32's mic route and speaker source pins) | BIN, DM32, INF |
| PTA15, PTA24 (15, 24) | Switched with the audio path: amplifier (TDA8542AT) enable / mute | BIN, FCC, INF |
| PTC18 (82) | Audio switch | BIN |
| PTA19 (19) | FM broadcast audio enable | BIN, INF |
| ADC 2 | Board ID: display bus mode | BIN |
| ADC 6 | RSSI (squelch and channel busy) | BIN |
| ADC 0, ADC 7 | Read periodically: supply voltage and ? | BIN, INF |

Which matrix position is which key isn't traced. The front keys are power, P1, P2, P3, VOL/SQL, TONE/T.SEL, B/E,
Menu/OK and the encoder push (COM). The 16-key hand microphone has its own MCU (FCC); how its keys reach the radio isn't
known (?).

## 4. RF chain

### 4.1 Architecture (PRO)

Superheterodyne with a **51.550 MHz IF** (COM, manual section 18.2). One BK4819 is the receiver and transmitter
("main"), the other is the local oscillator ("LO") (BIN):

| State | Main BK4819 | LO BK4819 |
|---|---|---|
| RX on f | Tuned to 51.550 MHz, RX state | Synthesizer at **f + 51.550 MHz** (high side): frequency in 0x38 / 0x39, state 0x30 = TX enable value so its output stage drives the mixer, level in 0x36 |
| TX on f | Tuned to f, TX state | Off (0x30 = 0, 0x37 = 0x1D00) |

Analog FM is modulated and demodulated by the main BK4819, not by the HR_C7000: the stock firmware never puts the C7000
in its FM modem mode (BIN). How the main chip's audio and modulation lines reach the C7000 for DMR needs the board (?).

### 4.2 Chip identification

Register 0 of each chip is read at boot (BIN):

| Register 0 | Main transceiver (letter) | LO (letter) |
|---|---|---|
| 0x6818 (FD6818) | '6' | '6' |
| 0x7813 (?) | '4' | '4' |
| 0x1846 | 'A' | - |
| 0x1100 (AT1141 PLL) | - | 'T' |
| 0x4819 (BK4819) | 'B' | 'B' |
| other | soft reset and read again | same |

### 4.3 BK4819 registers (address: value, in order)

The register meanings are the BK4819's public ones; the values are what the stock firmware writes (BIN).

| Register | Meaning |
|---|---|
| 0x00 | bit 15 soft reset |
| 0x30 | Enables. 0x0000 idle; 0xBFF1 RX; 0xC1FE TX (VCO calibration, PLL / VCO, PA gain, mic ADC, TX DSP); 0x0200, 0x0302 during init |
| 0x36 | PA bias and gain (the LO's output level) |
| 0x37 | Power-up of the LDOs, bandgap and crystal (0x1D0F normal, 0x1D00 off) |
| 0x38 / 0x39 | Frequency, 10 Hz units, low / high 16 bits |
| 0x43 | Receive filter bandwidth: 0x3028 and 0x4048 among the values written (wide / narrow, INF) |
| 0x47 | AF output select: 0x6042 muted, 0x6140 FM audio on |
| 0x65 / 0x67 | Noise / RSSI indicators (as the FD6818, [hardware-dm32.md](hardware-dm32.md) 8.4) |

**Main chip init:** 00:8000, 00:0000, 33:0003, 34:0000, 35:0000, 37:1D0F, 47:6042, 13:03BE, 12:037B, 11:027B,
10:007A, 14:0019, 49:2A38, 7B:8420, 7D:E950, 48:B3BF, 1F:5454, 3E:A037, 3F:07EE, 2A:4F18, 28:6B00, 53:C074, 40:352A,
29:AB24, 4B:7126, 4E:6F15, 4F:3332, 27:A430, 48:B3B3, 37:1D9F, 30:0302, 47:6042, 30:0200, 33:0000, 30:0200, 47:6042,
30:0200, 37:1D0F, 7D:E34A.

**LO chip init:** the same up to 37:1D9F, except 4F:2F2E; then 30:0302, 47:6042, 30:0000, 33:0000, 30:0000, 47:6042,
30:0000, 37:1D0F, 51:0000, 40:0000, 73 |= 0x0010 (read, bit 4 set, written back: AFC off), 47:6943,
7E:3006, 43:4048, 47:6042, then **36:00BF**: an output level of 15 on a 0-15 scale, written as level x 63 / 15 with
bit 7 set (BIN).

**LO tune to f_LO:** 30:0000, 38 / 39 = f_LO / 10 Hz, 37:1D0F, 30:0000, 30:C1FE (BIN).

**LO off:** 30:0000, 37:1D00 (BIN).

## 5. Display

| Item | Finding | Tag |
|---|---|---|
| Panel | 1.77" color TFT, ST7735S-class controller, 160 x 128 landscape (MADCTL 0x60) | COM, BIN, INF for the size |
| Reset | high 10 ms, low 10 ms, high 130 ms | BIN |
| Init | ST7735 frame-rate and power commands as ODR's `st7735` type 1 except VCOM (C5 = 0x12) and the first positive gamma pair (E0: 04 22 ...) | BIN |
| Backlight | Pin not traced; the IAP loader leaves it on | COM, ? |

## 6. Storage and calibration

| Item | Finding | Tag |
|---|---|---|
| NAND | Kioxia TC58CVG1S3HRAIJ, 2 Gbit SPI NAND: 2 KB pages, 128 KB blocks. Standard SPI NAND opcodes (0x9F ID, 0x13 page read, 0x06, 0x84, 0x0F / 0xC0 status, 0xD8 erase). The stock firmware builds a bad-block table at boot | FCC, BIN |
| Calibration | A 2 KB page at **NAND 0x280000** (block 20) starting with "TUNE", and a copy in program flash at 0x031FE000 (4 KB sector). The stock firmware repairs one copy from the other at boot | BIN |
| Calibration layout | 6.1 – 6.4 | BIN |
| Stock NAND areas | 0x40000, 0x110000, 0x3C0000, 0x1100000, 0x2980000 (codeplug and databases, INF) | BIN |

**Rule:** ODR reads both calibration copies and never writes them. It never writes program flash at 0x031FE000 or NAND
block 20.

### 6.1 Page header and the two copies

| Offset | Size | Field | Tag |
|---|---|---|---|
| 0x000 | 4 | Magic "TUNE" | BIN |
| 0x004 | 4 | Save counter, u32 little-endian. The stock tuning mode adds 1 on every save | BIN |

At boot the stock firmware reads 8 bytes of each copy. If both carry "TUNE", the copy with the higher counter wins and
is copied over the other. If only one does, it's copied over the other. If neither does, the radio runs untuned ("No
tune"). There's no checksum. ODR applies the same choice when it reads, without writing anything back (BIN).

### 6.2 Bands and frequency points

The page has two 1 KB halves with the same layout: **0x000–0x3FF for UHF and 0x400–0x7FF for VHF**. The stock
firmware picks the half from the band of the frequency in use. Offsets below are UHF; add 0x400 for VHF. The few
fields that exist only once are marked (BIN).

Most tables hold one value per **frequency point**, 8 points per band (BIN):

| Point | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| UHF (MHz) | 403.175 | 414.175 | 425.175 | 436.625 | 447.775 | 458.775 | 469.825 | 478.825 |
| VHF (MHz) | 136.125 | 142.125 | 146.125 | 152.125 | 156.125 | 162.125 | 166.125 | 173.125 |

A per-point table is one leading byte, then 8 entries in point order, so entry p (1–8) of size n starts at
table + 1 + n·(p − 1). The leading byte isn't used by any per-point lookup (BIN).

**Interpolation (BIN):** for f at or below point 1, use point 1. Otherwise take the first point p ≥ 2 at or above f
and interpolate linearly between points p − 1 and p, rounding half up. Above point 8 (478.825–480 MHz, 173.125–174
MHz), the stock code reads past the table into the next one. ODR uses point 8 there instead.

The squelch table works by **segment**: 8 segments per band. VHF: below 142, 142–146, 146–152, 152–156, 156–162,
162–166, 166–173, from 173 MHz. UHF: below 410, then 10 MHz steps (410–420 … 460–470), from 470 MHz. A segment's
upper edge belongs to the next segment (BIN).

### 6.3 Fields

| Offset (UHF) | Size | Content | Used for | Tag |
|---|---|---|---|---|
| 0x02C | 1 + 8 × u16 | RX front-end tuning, per point | DAC A, written when the receiver is set up for a frequency. The tuning voltage of the receive band-pass filter (INF) | BIN |
| 0x43E (VHF half only) | 7 | Global bytes, both bands. Byte 0x43F is the **TX center** (128 = mid-scale, as on the DM-32); the other six aren't traced | Modem SIG_CENTER and RF_MOD_BIAS_CTRL, 6.4 | BIN |
| 0x045 | 1 + 8 × u16 | **PA drive, power level 1** (H, INF), per point. DAC B value: the APC ramp's end value | TX power | BIN |
| 0x056 | 1 + 8 × u16 | **PA drive, every other power level** (L, INF), per point | TX power | BIN |
| 0x067 | 1 + 8 × (u8, u8) + 1 | Modulation, per point. The first byte of each pair goes to the modem (6.4). The second is read but then cleared before use | TX modulation level | BIN |
| 0x09B | 1 + 8 × u8 | A per-point value capped at 15, written into the RF chip driver's state (use not traced). The leading byte is read on its own and kept with the channel's RF parameters | ? | BIN |
| 0x0AE | 1 + 8 × (u16, u16) | TX timing in µs, per point: the delay between tuning the transmitter and selecting the PA, and the delay at TX end before the ramp down. A value above 2000 counts as 0 | TX switching | BIN |
| 0x0CF | 1 + 8 × 9 × u16 | **Squelch**, per segment: one value for each squelch level 1–9 (level 0 doesn't use the table) | RF chip squelch, 6.4 | BIN |
| 0x160, 0x1B1 | 1 + 8 × 5 × u16 each | Five values for each of 8 ranges: up to point 2, then between points (2–3 … 7–8), then above point 8. No interpolation. Two variants chosen by channel mode (use not traced) | ? | BIN |
| 0x202 / 0x206 / 0x207 / 0x208 / 0x20B | 4 / 1 / 1 / 3 / ? | Small items the tuning mode reads and writes. 0x20B is in the UHF half only (use not traced) | ? | BIN |

The tuning mode moves each table as a unit (the sizes above), plus 198 bytes from the start of each half. Everything
not listed is unused as far as the image shows.

### 6.4 What the values drive

| Output | Value | Tag |
|---|---|---|
| PA drive (DAC B, 12 bit) | The interpolated 0x045 or 0x056 value for the TX frequency, chosen by power level. The APC ramp runs up to it at TX start and down from it at TX end, as on the DM-32 | BIN |
| RX front end (DAC A) | The interpolated 0x02C value for the RX frequency | BIN |
| DAC C | 1780 when receive is set up, not from the page (the TCXO trim, INF) | BIN |
| SIG_CENTER (modem 0x108) | (c << 24) or (c << 16) or ((m & 0xFF) << 8) or (m & 0xFF), with c = the TX center byte and m below | BIN |
| RF_MOD_BIAS_CTRL (modem 0x114) | ((m & 0x3FF) << 16) or a. m = round((1.228 + c × 0.422 / 128) × 1023 / 3.3): 381 at c = 0, 512 at c = 128, close to the DM-32's c + 384. a = the interpolated first byte of 0x067 | BIN, DM32 |
| BK4819 squelch (reg 0x78) | For level n (1–9), v = low byte of entry n of the segment's 0x0CF table: v is the open threshold in dB below 0 dBm at the main chip. Register = (o << 8) or (o − 4), o = 2 × (160 − v), i.e. open at −v dBm and close 2 dB lower. Level 0 uses v = 136 | BIN |
| Untuned radio | No page: the stock firmware keeps running, values unknown | BIN |

ODR's use: TX stays refused until a reader exists. Then TX power comes from 0x045 / 0x056, and refuses when the page
is missing or a PA value is 0 or 0xFFFF. The RSSI that ODR shows stays the chip's own reading: the page has no RSSI
offset, only squelch thresholds (INF).

## 7. PC link, update and recovery

| Item | Finding | Tag |
|---|---|---|
| Connectors | RJ-45 mic: 1 power off, 2 PTT / radio RXD, 3 mic, 4 shield, 5 radio TXD, 6 ground, 7 +8 V, 8 speaker. 3.3 V logic. The side K1 jack carries the same serial pair | COM |
| Cable | USB-serial (Prolific PL2303 originally; FTDI accepted from 909E.D4.EARSAB.018) | COM |
| Stock PC protocol | UART1, 115200. Identifies as "+DRS-300UV" / "+136-480MHZ+". 2 KB packets with an additive checksum | BIN |
| IAP loader entry | Radio off, cable in, hold P1 and press power: red LED, blank display with backlight. The vendor "IAP" tool sends the raw .bin; the radio turns off when done | COM |
| IAP protocol | Not documented yet. Capture of a vendor update, or analysis of the vendor tool | ? |
| Loader rewrite | The stock application accepts writes to program flash at 0x03000000 / 0x03001000 (the vendor ".flash" loader update). A bad transfer there can erase the loader | BIN |
| JTAG | The application never changes the IO mux of PTA0-PTA4, the SoC's JTAG pins. Pads: ? | BIN, DM32, INF |
| ROM serial boot | The HR_C7000 boot ROM offers a download on UART0 at reset. Where UART0 goes on this board: ? | DM32 |

## 8. Codec

The stock image links the HR_C7000 vocoder library (V1.9.6); AMBE+2 runs in software on the CK803S and the frames pass
through the CPU, as on the DM-32 (BIN). Its code has the layout of the DM-32 046 build and moves with every stock build
(file offset 0xB5C58 in the 2025-01-21 image), so the [own-stock](codec-dm32.md) carving model carries over with one
recipe per stock file (BIN). ODR needs a recipe for at least one stock image a PRO can run (2024-05-09 or later).

## 9. How ODR uses it (`make BOARD=db25d`)

Bring-up state, nothing run on a radio yet:

| Part | State |
|---|---|
| Platform | `platform/hrc7000`, image at 0x0300C000 behind the IAP loader; the host port on UART1 |
| RF | `drivers/rf/bk4819`: main and LO on one 3-wire bus. FM receive by the main chip at the IF with the LO at f + 51.55 MHz |
| TX | **Refused** until the calibration reader exists: the PA power control has no known safe value without it. The page layout is known (6.1 – 6.4); the reader needs the NAND driver |
| Display | `st7735` on a bit-banged 4-wire serial bus |
| Keys | Matrix and encoder decoded; the key positions are provisional until checked on the radio |
| Storage | None yet: the core runs without stored settings. Needs an SPI NAND driver and a 4 KB sector layer over it |
| DMR | Not yet: needs the codec recipe and the DM-32's modem glue shared between the two boards |
| Older batches (FD6818 main) | Not supported yet |

## 10. Open questions

| # | Question | Needs the radio? |
|---|---|---|
| 1 | Firmware version in Device Info (expect 909E.DBB...) | Yes, menu only |
| 2 | Program flash part and size; JTAG and UART0 pads | Yes, photos of the control board |
| 3 | Panel size and which bus mode the PRO uses (ADC 2) | Yes |
| 4 | Calibration page: which of 0x045 / 0x056 is the high level, the 0x09B / 0x160 / 0x1B1 / 0x2xx items, real values (6.3) | A page read from a radio |
| 5 | Can a stock CPS read back the calibration page (a backup path)? | Yes, serial capture of a CPS read |
| 6 | IAP loader protocol and any model check | Capture of a vendor update, or the vendor tool |
| 7 | Hand-mic key protocol on RJ-45 pin 2 | Yes, logic analyzer |
| 8 | Key matrix positions; the backlight pin | Yes |
| 9 | Band assignment of the RF switch pins (UHF = PTB3 / PTB4 is by analogy) | No, then a meter |
| 10 | Supply voltage ADC channel and scale | No, then a meter |
