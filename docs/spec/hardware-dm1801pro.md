<!-- SPDX-License-Identifier: MIT -->
# Baofeng DM-1801 Pro hardware

An HR_C7000 radio: the DM-32UV's SoC and RF chip family with a monochrome display and a different pin map. This page
records what the DM-1801 Pro does and where it differs from [hardware-dm32.md](hardware-dm32.md), which is the
reference for register names and the pin-index scheme. No board was probed. ODR first ran on a radio on 2026-10-15;
facts checked there carry the tag **R**.

| Tag | Meaning |
|---|---|
| **K** | Known: a value or behavior of the radio taken from its observed operation, no part marking or measurement |
| **I** | Inferred from patterns, constants or the DM-32UV; the reasoning is stated |
| **R** | Checked on a radio running ODR |
| **?** | Not determined |

## 1. What is the same as the DM-32UV, and what is not

| Area | DM-32UV | DM-1801 Pro | Tag |
|---|---|---|---|
| SoC | HR_C7000, C-SKY CK803S | same, same ROM boot flag | K |
| Program flash | 1 MB, XIP at 0x03000000 | same size and base, application at 0x0300C000 | K |
| Data flash | 16 MB on SPI0, CS PTA18 | same bus and CS pin; only 24-bit read, page program, 4 KB erase and status commands are needed | K |
| RF chip bus | 3-wire, clock PTA7, data PTA8, **CS PTA24** | clock PTA7, data PTA8, **CS PTA30**, slower | K |
| RF chip registers | FD6818 | same family, other power-on tables and 0x30 sequences (section 5) | K |
| Frequency registers | 0x39 high / 0x38 low, 10 Hz units | same, no IF offset | K |
| VHF / UHF split | 280 MHz | 200 MHz | K |
| Display | i8080 color TFT | **128x64 monochrome, ST7567/UC1701 style, bit-banged serial** | K bus, I controller |
| Keys | 4-column matrix, encoder, PTT on PTB11 | **5 strobes x 4 inputs** matrix plus four direct inputs; no encoder | K |
| Calibration | page tagged 2 in the data flash | same page and fields (section 6.4) | K |
| Extras | GPS | accelerometer on a software I2C, vibration motor, FM broadcast tuner; no GPS | I |

## 2. SoC and boot

| Item | Value | Tag |
|---|---|---|
| ROM flag at flash 0 | `thd\x0c`, entry address at +4 | K |
| Boot loader | flash 0x1000-0x4B00, UART0 serial loader, ID string `BF1801-V1` (DM-32UV: `BFUV32-V2`) | K |
| Application | flash 0x0C000 up, executes in place at 0x0300C000 | K |
| Start-up | PSR 0x80000100, VBR in SRAM (vector table first), cache sequence as the DM-32UV | K |

## 3. Pin map

Index = GPIO pin index (0-31 PTA, 32-63 PTB, 64-95 PTC). "Boot" is the level the radio drives at start-up.

### 3.1 Pins

| Index | Pin | Function | Boot | Tag |
|---|---|---|---|---|
| 5 / 6 | PTA5 / PTA6 | software I2C SCL / SDA, 8-bit address 0x7A, device unknown | high | K bus, ? device |
| 7 / 8 | PTA7 / PTA8 | RF chip clock / data | high | K |
| 15 | PTA15 | LCD backlight, high = on; also the PWM0 output, which ODR uses to dim it | low | I |
| 16 | PTA16 | audio power amplifier, high = on (the stock broadcast tuner code drives it because the tuner needs the amplifier) | low | R |
| 17 | PTA17 | power latch | high | I |
| 18 | PTA18 | data flash chip select, low = selected | high | K |
| 25 / 26 | PTA25 / PTA26 | FM broadcast tuner I2C SDA / SCL | low | K bus, I chip |
| 28 | PTA28 | RX path select, VHF side (pair with PTB8) | low | K pin, R band |
| 29 | PTA29 | FM tuner enable, low = on | high | I |
| 30 | PTA30 | RF chip chip select, low = selected | high | K |
| 31 | PTA31 | TX path select, VHF side (pair with PTB7) | low | K pin, I band |
| 32 / 33 | PTB0 / PTB1 | LED green / red | low | R |
| 34 / 55 | PTB2 / PTB23 | accelerometer I2C SCL / SDA | | K bus, I chip |
| 35-38 | PTB3-PTB6 | keypad inputs, low = key closed | in | K |
| 39 | PTB7 | TX path select, UHF side | low | K pin, R band |
| 40 | PTB8 | RX path select, UHF side | low | K pin, R band |
| 41 | PTB9 | power knob switch; its level at start-up means "on" | in | R |
| 42 | PTB10 | input with interrupt, role unknown | in | ? |
| 43 / 44 | PTB11 / PTB12 | PTT / SOS (top key), low = pressed | in | R |
| 46 / 47 | PTB14 / PTB15 | audio path switches (the DM-32UV's mic path and speaker source). PTB15 high = the FD6818's audio to the amplifier, low = the HR_C7000 codec. ODR drives PTB14 as PTB15's complement; it doesn't change the speaker | low | R for PTB15, I for PTB14 |
| 50-54 | PTB18-PTB22 | keypad strobes, low one at a time | | K |
| 48 | PTB16 | output, role unknown; not the amplifier | low | R |
| 56 | PTB24 | vibration motor, high = on | low | R |
| 58 / 59 | PTB26 / PTB27 | SK1 (upper side key) / SK2 (lower side key), low = pressed | in | R |
| 66 | PTC2 | display reset | high | I |
| 67 | PTC3 | display chip select, low = selected | high | K |
| 68 | PTC4 | display clock | idle high | K |
| 70 | PTC6 | display data / command, low = command | low | K |
| 72 | PTC8 | display data, MSB first | low | K |

Not found: the flashlight pin, USB. The programming cable's plug switches the internal speaker off in hardware: with
it in, the amplifier on and the speaker source selected, the speaker is silent (R).

Speaker paths (R):

| Sound | Path | PTB15 |
|---|---|---|
| FM receive | FD6818 audio output straight to the amplifier; the HR_C7000's FM playback stays off | high, only while the squelch is open |
| Beeps, DMR voice | CPU samples to the HR_C7000 prompt FIFO, its codec, the amplifier (`modem-hrc7000.md` 8.4) | low |

With the squelch closed the FD6818 outputs noise, so PTB15 stays low then: switching it high at the end of a beep let
a burst of static through. The amplifier (PTA16) turns off before PTB15 changes.

### 3.2 Display bus

Chip select PTC3 low for each byte, clock PTC4 low then high per bit, data PTC8 set while the clock is low, data/command
PTC6, reset PTC2. Bits go out MSB first. K.

## 4. Peripherals and IO mux

| Block | Base | Use | Tag |
|---|---|---|---|
| System control / IO mux | 0x11000000 | SPI0 pins to the peripheral (IO_DIPLEX1 bits 8:3 cleared), ADC B and C analog (IO_DIPLEX2 bits 20, 21 cleared) | K |
| UART0 | 0x14030000 | 115200 8N1, the programming cable | K |
| SPI0 | 0x140A0000 | 16 MB data flash | K |
| ADC | 0x140D0000 | channel 2 battery (I); channel 1 unknown | K |
| DAC | 0x140F0000 | channel B PA drive, channel C set once (0x067E): the modem's AF input bias. With the DM-32UV's 0x06E2 the modem missed the sync of about half of the DMR senders; with 0x067E it took every burst, strong and at -87 dBm | K, R |
| GPIO A / B / C | 0x14020000 / 0x14100000 / 0x14110000 | all pins above | K |
| Not used | LCD controller 0x12000000, PWM 0x140C0000 | the display is bit-banged, the backlight is a plain GPIO | K |

I2C2, SPI1 (a slave that returns 0xFF until ready) and SPI3 (an 8-byte identity read) are used for devices not identified.
UART1-3 are never used: no GPS and no UART Bluetooth.

## 5. RF chip

### 5.1 Bus

Three-wire serial, bit-banged: clock PTA7, data PTA8, chip select PTA30. A frame is CS low, an 8-bit address (bit 7 set
for a read), 16 data bits MSB first, CS high. The radio waits about 5 microseconds per half clock; ODR scales the
driver's delays by 5. K.

### 5.2 Frequency and band

- Unit 10 Hz, 32 bits: register 0x39 = high 16 bits, 0x38 = low 16 bits. 446.000 MHz = 0x02A88AC0. No offset. K.
- Ranges: 136-174 MHz and 400-470 MHz, plus an air band window of 108-136 MHz on the same chip (not used by ODR yet). K.
- VHF / UHF split at 200 MHz (selects the path pins). K.
- No band-word table: register 0x1A is never written; register 0x40 is 0x3480 plus a calibration byte (0x70 at power-on,
  the byte's source isn't located). K.
- Bandwidth register 0x43: 0x3048 for DMR and for 12.5 kHz FM, 0x3028 for 25 kHz FM. The radio's channel set-up picks
  0x3048 for a digital channel, and for an analog one by the channel's Narrow / Wide bit (Narrow 0x3048, Wide 0x3028).
  Its bandwidth table also holds 0x2028 and 0x205C, but nothing selects them. K.

### 5.3 Register tables

Power-on, in order: 0x00=8000, 0x00=0000, 0x37=9D1F, 0x13=03DF, 0x12=03DB, 0x11=033A, 0x10=0318, 0x14=0210, 0x19=1041,
0x1C=0422, 0x1D=2AAB, 0x1E=4C58, 0x1F=165A, 0x25=6DBA, 0x28=0A40, 0x29=AA00, 0x2A=6600, 0x2C=0022, 0x2F=9890, 0x3A=9A7C,
0x3E=94C6, 0x3F=07FE, 0x40=34F0, 0x46=6050, 0x48=B386, 0x49=2A32, 0x4A=5430, 0x4D=A015, 0x4F=2A28, 0x53=2028, 0x73=6681,
0x77=88EF, 0x7B=73DC, 0x7E=303E, 0x4C=E520, 0x4E=1F10, 0x36=003F; then the sixteen-entry register 0x09 table of the
DM-32UV (006F, 106B, 2067, 3062 ... F09F); then 0x21=06D8, 0x24=8C7E. K.

Analog FM mode: 0x47=6042, 0x7E=302E, 0x2B=C000 (and 0x32=0244 on the way back to receive). DMR mode: 0x47=0140,
0x48=B2F5, 0x7E=B03F, 0x2B=0700, 0x28=0A40. Tone signalling: 0x70=00E0, 0x72=3065, 0x58=00C1, 0x5A=85CF, 0x5B=AB45,
0x5C=5665. K.

On this radio the analog word 0x47=6042 leaves the FD6818's audio output off. ODR writes 0x47=0140 in FM as well,
which gives receive audio on the speaker path (R).

### 5.4 State sequences

- RX: `0x37=9D1F, 0x30=0200, 0x30=BFF1`. TX: `0x37=9D1F, 0x52=028F, 0x30=0200, 0x30=C1FE`. Always through idle
  (0x30=0000) between the two. The DM-32UV goes straight to the final word. K.
- **Enter TX:** RX path off, wait, TX frequency, the TX sequence, register 0x50 = 3B18, wait, TX path on, PA ramp (40
  steps into DAC B). **Leave TX:** wait, PA ramp down, wait, TX path off, wait, RX frequency (0x32=0244, 0x47=6042),
  RX sequence, RX path on. The radio's waits are an uncalibrated count; ODR uses 1 ms. K for the order.
- RSSI register 0x67, low 9 bits: dBm = value / 2 - 160 (FD6818 programming guide; 0x2C = 0x0022 sets 2 dB of external LNA gain). Status reads 0x0C, 0x68-0x6A. CTCSS register 0x51 with a calibration byte; the
  tone frequency scale (register 0x07) isn't decoded. ODR sends the tone with the modem's generator and detects it with
the FD6818 (`fd6818_set_rx_tone`, result in 0x0C), as on the DM-32UV: the modem's detector has no input while the FD6818
plays FM to the speaker.

### 5.5 Paths and PA

TX path select: PTA31 + PTB7. RX path select: PTA28 + PTB8. Each is a complementary pair chosen by the band bit:
above 200 MHz the PTB pin high and the PTA pin low, below the reverse; both low = off. So PTA high = VHF side,
PTB high = UHF side: ODR receives on both bands (a VHF weather station, UHF DMR) and transmits on UHF with this (R);
VHF transmit is unchecked. Separate TX and RX band outputs, unlike the DM-32UV's PA selects and LNA supplies. PA drive is DAC
channel B (12 bit, the calibration byte times 16), ramped in 40 steps. K.

### 5.6 DMR

The modem is the HR_C7000's, so the slot engine, the interrupt glue and the PCM audio are the DM-32UV's, shared
(`platform/hrc7000/dmr_glue.c`, `audio_pcm.c`). The board supplies three hooks (`radio_dm1801pro.c`):

- **Per-burst RF switch** (the RF TX interrupt): RX path off, FD6818 idle, the TX sequence, register 0x50 = 3B18; after
  the burst the drive goes to 0, the RX sequence runs with the DMR bandwidth word 0x3048 and the RX path comes back on.
  The radio's own per-burst handlers are not decoded (?), so this is the RX and TX sequences of 5.4. Each frame takes
  about five times the DM-32UV's, so a switch is about a millisecond with interrupts off; the slot timing tolerates it
  on the bench.
- **PA key:** the TX path comes on when the transmission starts and stays on to its end (switching it per burst put
  power steps into the bursts on the bench). The DAC B drive is set 28 ms after each burst is armed (the DM-32UV's
  delay; this radio's isn't known, ?), in one write: a ramp doesn't fit in a burst.
- **Keyed but idle:** a DMR `tx()` leaves the FD6818 and the RX path in receive until the first burst. The hotspot keeps
  the radio keyed while it listens, and with the FD6818 in TX from `tx()` it heard nothing (checked on the bench).
- **Voice:** the radio's own codec, carved from a program flash read into the slot at 0x030E8000 and run by the shared
  host `platform/hrc7000/vocoder.c` (codec-dm1801pro.md 4). Without the slot, DMR runs without voice. On the bench, a
  voice call decoded by the SDR bench's modem had 1 corrected bit in 162 AMBE frames. Encode takes about 12 ms of each
  20 ms frame and decode about 3 ms, so the radio's idle load has to stay low.

## 6. Flash

### 6.1 Data flash (16 MB, SPI0)

| Item | Value | Tag |
|---|---|---|
| Bus | SPI0 mode 0, 8-bit frames, chip select PTA18 by hand | K |
| Commands used | 0x06, 0x05, 0x02 (256-byte pages), 0x03, 0x20 (4 KB erase) | K |
| Identification | none by the radio; any part with these commands at 24-bit addresses works (GD25Q128, Puya PY25Q128HA) | K |

| Range | Content | Tag |
|---|---|---|
| 0x001000-0x0C8FFF | config, contacts, quick messages, emergency, encryption: logical sectors, each tagged in its last byte | K |
| 0x0C9000-0x149FFF | font bitmaps | I |
| 0x150000-0x175FFF | table of 8-byte entries | I |
| 0x201000-0x264FFF | voice prompts, RIFF/WAVE 8 kHz 16-bit mono | K |
| 0x635000-0xFFFFFF | call recordings | I |

ODR writes only at 0x700000 and above (inside the recordings area, empty on a radio that hasn't recorded). The settings
ring, journal and codeplug images sit where the DM-32UV has them ([hardware-dm32.md](hardware-dm32.md) 10.1), without
OpenDM32's calibration copy.

### 6.2 Program flash (1 MB, execute in place)

Boot loader at 0x1000, application at 0x0C000 and up. The application doesn't erase or program the program flash. The
loader speaks the DM-32UV's update protocol ([hardware-dm32.md](hardware-dm32.md) 13.2), which was traced against it:

| Step | The loader |
|---|---|
| 1-4 | `R` answers `06`; `M` answers `4D 01 <n>` where n is the length of the ID string (9), then after `06` sends the ID `BF1801-V1`; `V` answers `02 0C`; `E` answers `06`. `G` is served too (reads up to 0x20 bytes) |
| 5-9 | `31` starts a receiver that sends `C` and takes XMODEM-CRC / YMODEM frames: `01` (128 B) and `02` (1024 B), block number and complement, CRC-16/XMODEM (polynomial 0x1021, initial 0) big endian, `04` to end, `18` (CAN) twice to abort. Block 0 holds the file name, NUL and the size in ASCII; a size above 0xF4002 is refused, then the application area is erased and `06 43` answered. Each good block is answered `06` |
| Entry | The loader stays in update mode when GPIO 43 (PTB11) and 58 (PTB26) both read low, that is PTT and SK1 held at power-on; otherwise it starts the application (R) |

The file format (a 256-byte header with the ID as magic, as on the DM-32UV) is assumed, not seen. `tools/dm32_flash.py
--model dm1801pro` writes a raw image through this protocol: about ten flashes on 2026-10-15, no refused block, erase
about 12.6 s (R). The loader at 0x1000 is never written.

### 6.3 Identity read

SPI3 (mode 1) clocks eight bytes out; the caller mixes them into a value. The DM-32UV reads the same block. I: an
identity or integrity check.

### 6.4 Calibration

**Where.** Region 0 is addressed by logical sector. The radio reads the last byte of every physical sector 1 to 0xC8 into
a table and takes the first physical sector whose tag equals the logical number. Calibration is logical sector 2; this
is the DM-32UV's rule ([hardware-dm32.md](hardware-dm32.md) 11.1). On the radio examined that page is physical 0x7C000.
Other pages tagged 0 (0x2000, 0x3000, 0xB000, 0xF000) hold the same structure with the untuned values; they aren't
the ones the radio reads. K.

**Layout** (offsets in the 4 KB page; the field offsets match the DM-32UV's, [hardware-dm32.md](hardware-dm32.md) 11.2):

| Offset | Size | Content | Tag |
|---|---|---|---|
| 0x00 | 4 | `00 40 00 47` | ? |
| 0x16 | 1 | TX center | K |
| 0x17 | 5 | UHF modulation points | K |
| 0x40 / 0x45 / 0x4A | 5 each | UHF PA drive, power level 0 / 1 / 2, one byte per frequency segment | K |
| 0xA6 | 5 | VHF modulation points | K |
| 0xD0 / 0xD5 / 0xDA | 5 each | VHF PA drive, power level 0 / 1 / 2 | K |
| 0x300 / 0x314 | 20 each | UHF segment frequencies, five 4-byte BCD values in 10 Hz units, little endian: 400.00, 417.50, 435.50, 452.50, 470.00 MHz | K |
| 0x328 / 0x33C | 20 each | VHF segment frequencies: 136.00, 145.50, 155.50, 164.50, 174.00 MHz | K |
| 0xFFF | 1 | logical sector tag, 0x02 | K |

The PA drive for a frequency is linearly interpolated between the two segments around it and used times 16 as the DAC B
value (K). The radio's own firmware takes the segment edges from the 0x314 (UHF) and 0x33C (VHF) lists; ODR's shared
driver uses even steps from 400 and 136 MHz, which differ from those lists by at most 0.5 MHz at an edge (K).

The power level is one value for the three arrays: 0 = low (0x40 / 0xD0), 1 = middle (0x45 / 0xD5), 2 = high
(0x4A / 0xDA, also used for 3). The radio's menu shows the setting as High / Low / Middle and stores 2 / 0 / 1, and the
drive bytes rise with the level in every segment (K). ODR's L / M / H use the same arrays. How the 0x300 and 0x328
lists are used, and the unlabeled fields, are open. The ADC and the register 0x40 / CTCSS calibration bytes
have no located source.

### 6.5 The codec in the program flash

The radio's own firmware carries the DM-32UV's 049 codec ([codec-dm32.md](codec-dm32.md)), rebuilt with the data tables
0x48 bytes higher. It is located in the full program flash image (sha256 `072ece15...9f29e42a`). ODR runs it from the codec slot
([codec-dm1801pro.md](codec-dm1801pro.md) section 4).

| Item | Value | Tag |
|---|---|---|
| Code | flash 0xA22F0..0xB68F0 (0x14600 bytes), which the start-up code copies to RAM 0x00016CDC..0x0002B2DC (the RAM block is flash 0x9B614 to RAM 0x10000) | K |
| Signature | first words 0x152514D8, 0x74C776CD, as in the DM-32UV | K |
| Code sha256 | `8f7641b6...3f1a42` | K |
| Tables | flash 0x68770 (length 0x2F78), RAM 0x18000EF0..0x18003E68 (the data image is flash 0x68080 to RAM 0x18000800) | K |
| Tables sha256 | `10684c33...4eb0b0e1`, the same as the DM-32UV's 049 tables | K |
| Against the 049 code | 82 literal words that point into the tables are 0x48 higher; 29 words differ inside the three non-codec ranges the DM-32UV notes exclude (RAM 0x17270..0x1732C, 0x1CDBC..0x1D4C8, 0x1E8BC..0x1FE64); every other byte is equal | K |

[codec-dm1801pro.md](codec-dm1801pro.md) has the call analysis. The codec entry points are the 049 offsets from the code start (decode +0x0000, encode +0x0260, and the rest of the
table in codec-dm32.md). Hosting it needs the tables at 0x18000EF0, or the 82 pointers moved back by 0x48 as on the
DM-32UV. How it is called (state blocks, flags words) and the codec's frame path to the C7000 are not traced.

## 7. Display

128 x 64 pixels, page addressed: page command 0xB0 + n, column address 4 (so the controller has 132 columns and the
visible ones start at 4), 128 data bytes per page, 8 pages. The controller part isn't identified; ST7567 and UC1701 share
this command set. ODR sends the common ST7567 initialization (bias 1/9, regulator ratio 4, volume 0x20, power control
all on). Upright needs the segment outputs reversed (0xA1) and the common outputs normal (0xC0), with the column offset 4
(R). The picture is readable with these values; contrast and bias are not tuned further. Controller part I.

## 8. Other devices

| Device | Bus | Detail | Tag |
|---|---|---|---|
| FM broadcast tuner | I2C 7-bit 0x10 (sequential access; ODR uses 0x11, random access, the same chip's other address), SDA PTA25, SCL PTA26 | RDA5807 class: 76 and 65 MHz bases, 50 kHz steps; PTA29 enable (low = on, from the order of start and stop, not measured); its audio needs the amplifier (PTA16) | I |
| Accelerometer | I2C 7-bit 0x19, SDA PTB23, SCL PTB2 | SC7A20, a LIS3DH-compatible part: WHO_AM_I reads 0x11, not the LIS3DH's 0x33 (R). The radio writes CTRL1 = 0x47, CTRL4 = 0x88. ODR reads it for the diagnostics page only. Lying flat it reads (17, -1008, 78) mg: 1 g, so the scale is right (1 mg per count in the 12-bit mode) | R |
| Unknown | I2C 7-bit 0x3D, SCL PTA5, SDA PTA6 | registers 0x30, 0x40, 0x80, 0x20, 0x22 | ? |
| Vibration motor | PTB24, high = on | pulse about 500 ms on an incoming call; ODR pulses it once at power-on (`hal_power_t.vibrate`), nothing else calls it yet | R |

## 9. Keys, battery, power

### 9.1 Keys

The matrix strobes PTB18-PTB22 low one at a time and the inputs PTB3-PTB6 are read (low = closed): 20 positions. The
radio's panel has MENU, BACK, a four-way direction pad, VFO/MR, A/B, 0-9, `*` and `#` (20 keys), plus PTT, SK1, SK2 and a
top SOS button. Read off the radio one key at a time (R):

| Strobe | PTB3 | PTB4 | PTB5 | PTB6 |
|---|---|---|---|---|
| PTB18 | 9 | 3 | 6 | # |
| PTB19 | Down | Menu | Up | Back |
| PTB20 | Right | A/B | VFO/MR | Left |
| PTB21 | 8 | 2 | 5 | 0 |
| PTB22 | 7 | 1 | 4 | * |

Direct inputs: PTB11 PTT, PTB12 SOS, PTB26 SK1 (upper side key), PTB27 SK2 (lower side key).

### 9.2 Battery

ADC channel 2, averaged. The radio scales it with a calibration halfword (no fixed divider as on the DM-32UV) whose source
isn't located, and shows bars at 8.99, 7.99, 7.50, 6.99, 6.49, 6.31 and 5.81 V. ODR uses the DM-32UV's 2S scale until
that is measured. I.

### 9.3 Interrupts and timers

One timer gives the 10 ms main-loop tick; the GPIO banks, UART0 and the modem (PCM read and write, system, TX and RX
slots, RF TX and RX) have vectors as on the DM-32UV. The I2C, SPI, ADC and DAC blocks are polled.

### 9.4 Power, backlight, LEDs

PTA17 is the power latch, PTB9 the power key (polarity unverified; ODR takes the level at start-up as "on"). The backlight
is on PTA15. Stock only switches it as a GPIO; PTA15 is also the SoC's PWM0 output (as on the DM-32UV), so ODR dims it
with PWM0 for the brightness setting (I until seen on the radio). LEDs: PTB0 green, PTB1 red (R). The panel's top LED is red while transmitting, green while
receiving and orange flashing while scanning.

## 10. Open items

- What the 0x300 / 0x328 frequency lists are for (the PA drive uses 0x314 / 0x33C).
- VHF transmit (the TX pair's VHF side); 12.5 kHz FM on air with the 0x3048 word; the DMR per-burst handlers and PA
  delay.
- The display controller part and its contrast.
- PTB14's role, PTB16's role; whether the HR_C7000's audio (microphone, prompt FIFO) reaches the FM modulator (APRS
  AFSK sends a bare carrier, section 12).
- Where the ADC and the register 0x40 / CTCSS calibration bytes come from.
- The 0x7A I2C device, the I2C2 device, the SPI1 slave.
- The update file header.

## 11. What ODR reuses

| Part | Where |
|---|---|
| SoC, interrupt, timer, GPIO, SSI, UART, ADC, data flash | `platform/hrc7000` |
| Modem (FM and DMR) | `drivers/modem/hrc7000`; `platform/hrc7000` for the DMR glue and PCM audio |
| RF chip | `drivers/rf/fd6818`, variant `fd6818_variant_dm1801pro` |
| Display | `drivers/display/st7567` |
| Keys | `drivers/input/keymatrix` |
| Calibration | `drivers/cal/baofeng` |
| Broadcast FM tuner | `drivers/bcast/rda5807m` (chip and `hal_bcast_t`), `drivers/bus/i2c_gpio` |
| Accelerometer | `drivers/sensor/lis3dh` |
| Board wiring | `boards/dm1801pro` |

## 12. Bring-up state

First run on a radio on 2026-10-15 ([the bring-up checklist](../testing/dm1801pro-bringup.md) has the steps). Checked
there:

- start-up, the power knob, the vibration motor, the update-mode flash route
- the display (upright, readable), every key, the data flash (Puya, JEDEC 852018) and the factory calibration
- the speaker path (amplifier and source pins), the debug link on the programming cable
- FM receive audio on a weather station, the squelch and the monitor key
- the squelch closes without a signal (`fd6818_noise`), CTCSS / DCS muting, and receiver battery save (`rx_sleep`: FD6818 idle and the RX path off)
- beeps through the prompt FIFO, clean and without pops
- the accelerometer answers and reads 1 g lying flat

Checked with the SDR bench's modem: DMR voice receive and transmit with the radio's own codec, late entry (2026-10-15:
18 of 36 bursts played, as the HIL test expects), the GPS, privacy and emergency call transmit tests, and DMR data:
confirmed data with its response, SMS and ETSI SMS received and acknowledged. FM transmit with CTCSS 100.0 and DCS 023
(normal and inverted) was decoded by a DM-32UV, and the SDR bench's FM transmit tests (CTCSS, DCS, tone burst) pass.
The MMDVM hotspot on the programming cable works both ways on air (2026-10-16: the four `test_hotspot_on_air` cases).
Not in: the air band, the broadcast tuner's screens, USB (the radio has none). VOX: the radio doesn't read its
microphone level yet, so VOX never arms (`vox.mic=0`).

APRS: positions over DMR (LRRP and UDT) decode on the SDR bench. FM APRS uses the shared HR_C7000 AFSK
(`platform/hrc7000/hrc_afsk.c`). In its first on-air run (2026-10-16) no frame went out (`aprs.rf_failed`). In a
second run the same day the frame went out (`aprs.rf_sent`) but carried no tones: the SDR bench saw a 0.63 s
carrier at 7 Hz rms in the 300-3000 Hz band, so nothing decoded. The same prompt FIFO path decodes on the DM-32UV.
The board code is the same, so the difference is in this radio's audio wiring or in the FD6818's FM transmit set-up.
The prompt FIFO reaches the modulator inside the HR_C7000, so the programming cable in the speaker/mic jack doesn't
explain it. The cable does cut the internal microphone, so FM voice from the microphone can only be checked with the
cable out. Open; APRS receive is unchecked.

MMDVM SFR: the radio hears the call's header, uplinks it and starts the repeat in the other slot with the right
link control, but while it sends, the HR_C7000 delivers almost nothing from the listening slot, so the call doesn't
continue (`sfr.rx` stops after a few bursts). Open.
