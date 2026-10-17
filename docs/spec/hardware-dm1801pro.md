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
| 5 / 6 | PTA5 / PTA6 | software I2C SCL / SDA to the authentication chip at 8-bit address 0x7A (section 8.1) | high | K bus, I device |
| 7 / 8 | PTA7 / PTA8 | RF chip clock / data | high | K |
| 15 | PTA15 | LCD backlight, high = on; also the PWM0 output, which ODR uses to dim it | low | I |
| 16 | PTA16 | audio power amplifier, high = on (the stock broadcast tuner code drives it because the tuner needs the amplifier) | low | R |
| 17 | PTA17 | power latch | high | I |
| 18 | PTA18 | data flash chip select, low = selected | high | K |
| 25 / 26 | PTA25 / PTA26 | FM broadcast tuner I2C SDA / SCL | low | K bus, I chip |
| 28 | PTA28 | RX path select, VHF side (pair with PTB8) | low | K pin, R band |
| 29 | PTA29 | FM tuner enable, low = on | high | I |
| 30 | PTA30 | RF chip chip select, low = selected | high | K |
| 31 | PTA31 | TX path select, VHF side (pair with PTB7) | low | K |
| 32 / 33 | PTB0 / PTB1 | LED green / red | low | R |
| 34 / 55 | PTB2 / PTB23 | accelerometer I2C SCL / SDA | | K bus, I chip |
| 35-38 | PTB3-PTB6 | keypad inputs, low = key closed | in | K |
| 39 | PTB7 | TX path select, UHF side | low | K pin, R band |
| 40 | PTB8 | RX path select, UHF side | low | K pin, R band |
| 41 | PTB9 | power knob switch; its level at start-up means "on" | in | R |
| 42 | PTB10 | input with interrupt, role unknown | in | ? |
| 43 / 44 | PTB11 / PTB12 | PTT / SOS (top key), low = pressed. PTB11 is also the cable's host-to-radio serial line (9.1) | in | R |
| 46 / 47 | PTB14 / PTB15 | audio path switches, as on the DM-32UV. PTB14 high = the microphone to the HR_C7000's FM input (set for each FM transmission; low there cut speech from 140 to 43 Hz rms, section 12). PTB15 high = the FD6818's audio to the amplifier, low = the HR_C7000 codec. In receive ODR drives PTB14 as PTB15's complement; it doesn't change the speaker | low | R |
| 50-54 | PTB18-PTB22 | keypad strobes, low one at a time | | K |
| 48 | PTB16 | output, role unknown; not the amplifier. The radio's start-up drives it low, between the LEDs and the audio switches, and no other code reads or writes it (K). The boot loader leaves it an input; ODR drives it low at start-up too (`boards/dm1801pro/boot_pins.h`) | low | K, R not the amplifier |
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
| ADC | 0x140D0000 | channel 2 battery (I); channel 1 compared with the VOX thresholds of the calibration page (6.4, I) | K |
| DAC | 0x140F0000 | channel B PA drive, channel C set once (0x067E): the modem's AF input bias. With the DM-32UV's 0x06E2 the modem missed the sync of about half of the DMR senders; with 0x067E it took every burst, strong and at -87 dBm | K, R |
| GPIO A / B / C | 0x14020000 / 0x14100000 / 0x14110000 | all pins above | K |
| Not used | LCD controller 0x12000000, PWM 0x140C0000 | the display is bit-banged, the backlight is a plain GPIO | K |

I2C2 0x14080000 carries the SoC's real-time clock (8.2). SPI1 0x140B0000 has an SD card driver that the firmware never
starts (8.3). SPI3 0x14150000 makes an 8-byte identity read (6.3). UART1-3 are never used: no GPS and no UART
Bluetooth.

## 5. RF chip

### 5.1 Bus

Three-wire serial, bit-banged: clock PTA7, data PTA8, chip select PTA30. A frame is CS low, an 8-bit address (bit 7 set
for a read), 16 data bits MSB first, CS high. K. The radio's wait per half clock is five passes of a short busy loop,
well under a microsecond at the 168 MHz core clock, so with the GPIO calls a frame takes some tens of microseconds (I;
an earlier reading took the loop for 5 microseconds). ODR scales the driver's delays by 5, which is slower than the
radio.

### 5.2 Frequency and band

- Unit 10 Hz, 32 bits: register 0x39 = high 16 bits, 0x38 = low 16 bits. 446.000 MHz = 0x02A88AC0. No offset. K.
- Ranges: 136-174 MHz and 400-470 MHz, plus an air band window of 108-136 MHz on the same chip (not used by ODR yet). K.
- VHF / UHF split at 200 MHz. The channel set-up keeps two band bits, one for the RX and one for the TX frequency,
  each set above 200 MHz. The RX bit picks the RX path pin and the squelch rows; the TX bit picks the TX path pin and
  the PA, modulation and register 0x40 rows (6.4). K.
- No band-word table: register 0x1A is never written; register 0x40 is 0x3480 plus a calibration byte: 0x70 at
  power-on, and on FM transmit a byte interpolated from the calibration page (6.4). K.
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
- Receiver battery save: sleep is `0x30=0000`, then 0x37 = 9D17 (DMR) or 9D00 (FM), RX path off; wake is
  `0x37=9D1F, 0x30=BFF1` straight from idle, RX path on. K.
- **DMR per-burst handlers.** The RF TX and RF RX interrupts only clear their flag and wake a task. In a digital
  channel that task runs the burst start on an RF TX interrupt and the burst end on an RF RX interrupt while a burst is
  on; a second path runs the burst end when the transmission stops. These two are the only callers of the sequences
  below, and there is no PA timer: the PA ramps inside the burst start. K.

  | Burst start (RF TX interrupt) | Burst end (RF RX interrupt) |
  |---|---|
  | 1. RX path off | 1. wait 1.6 ms |
  | 2. wait 1.2 / 1.0 / 0.8 ms at High / Middle / Low power | 2. PA ramp down (below) |
  | 3. 0x38 / 0x39 = TX frequency, 0x30 = 0000, 0x47 bits 11:8 cleared (0x0140 becomes 0x0040), 0x37 = 9D1F, 0x52 = 028F, 0x30 = 0200, 0x30 = C1FE, 0x50 = 3B18 | 3. wait 0.3 ms |
  | 4. wait 1.0 ms | 4. TX path off |
  | 5. TX path on | 5. wait 0.7 ms |
  | 6. PA ramp up (below) | 6. 0x30 = 0000, 0x38 / 0x39 = RX frequency, 0x32 = 0244, 0x37 = 9D1F, 0x30 = 0200, 0x30 = BFF1, 0x47 = 0140, 0x3F = 0000 |
  | | 7. RX path on, wait 0.05 ms |

  - The waits are counts of a busy loop of about 167 cycles, so microseconds at 168 MHz (K for the counts, I for the
    time). The FD6818 frames in step 3 and step 6 add a few hundred microseconds at the radio's bus speed (5.1, I).
  - PA ramp: DAC B = round(drive x t / 248) x 16 for each t of a fixed 40-entry table (1, 2, 2, 4, 8, 10 ... 228, 238,
    248), then the drive itself. Down, the entries 21 to 2 in reverse, then 0, so the drive first steps to 70/248 of
    its value. No wait between steps; each costs a software floating-point calculation. K.
  - 0x43 isn't written per burst, and the RX path is on between bursts. The RX frequency and 0x32 are written at every
    burst end, also on a simplex channel. Every DMR receive set-up writes 0x3F = 0000 and 0x47 = 0140, so in DMR the
    radio keeps 0x3F at 0, not the power-on 07FE. K.
  - RF_CONTROL (`modem-hrc7000.md`) is only ever ORed, never written whole. A simplex channel with flag bit 5 of channel
    byte 0x0D ORs in an RF TX lead of 0x1A (2.6 ms) and sets RF_MODE b11 (dual receive channels); any other channel,
    simplex or repeater, ORs in 0x15 (2.1 ms) and clears RF_MODE b11. Both OR in an RF RX lead of 0x12 (1.8 ms). K. The
    register's reset value isn't known, so neither is the lead the radio ends up with; after both kinds of channel it
    is 0x1F (3.1 ms), the DM-32UV's value (I). What the 0x0D bit selects is not known (?).
  - Timing that follows (I): the drive starts to ramp about 2 ms after the RF TX interrupt and is at the top near the
    burst start. The receiver is back about 3 ms after the RF RX interrupt: with a 1.8 ms RX lead, about 1 ms into the
    slot after ours.
- **FM key-up** (not per burst): RX path off; FD6818 0x3F = 0000, 0x59 = 0028, 0x70 = 0000, 0x58 = 0000, 0x24 = 0000;
  TX path on; the sub-audio set-up; register 0x40 (6.4); the TX frequency and state writes of step 3 above; then the
  40-step ramp, without the waits. FM key-down isn't traced. K. None of these writes depends on the band. ODR doesn't
  write the five words before the TX path, and FM transmit works without them on UHF. Earlier
  versions of this page gave the DMR per-burst order as the radio's general "Enter TX / Leave TX"; ODR's FM `tx()`
  still follows that order with 1 ms waits, and FM transmit works with it.
- RSSI register 0x67, low 9 bits: dBm = value / 2 - 160 (FD6818 programming guide; 0x2C = 0x0022 sets 2 dB of external LNA gain). Status reads 0x0C, 0x68-0x6A. CTCSS register 0x51 with a calibration byte (6.4: 0x201 for CTCSS, 0x202 for DCS); the
  tone frequency scale (register 0x07) isn't decoded. ODR sends the tone with the modem's generator and detects it with
the FD6818 (`fd6818_set_rx_tone`, result in 0x0C), as on the DM-32UV: the modem's detector has no input while the FD6818
plays FM to the speaker.

### 5.5 Paths and PA

TX path select: PTA31 + PTB7. RX path select: PTA28 + PTB8. Each is a complementary pair chosen by its band bit
(5.2): above 200 MHz the PTB pin high and the PTA pin low, below the reverse; both low = off. So PTA high = VHF side,
PTB high = UHF side (K for both pairs, from the radio's path select code). ODR receives on both bands (a VHF weather
station, UHF DMR) and transmits on UHF with this (R). Separate TX and RX band outputs, unlike the DM-32UV's PA selects
and LNA supplies. PA drive is DAC channel B (12 bit, the calibration byte times 16), ramped in 40 steps. K.

**VHF transmit.** Everything the radio's code does differently for a TX frequency at or below 200 MHz (K):

| Item | VHF | UHF |
|---|---|---|
| TX path pin high | PTA31 | PTB7 |
| Band limits the TX frequency is clamped to for the lookups | 0xCC (136.0-174.0 MHz) | 0x00 (400.0-470.0 MHz) |
| TX points | 0x33C | 0x314 |
| PA drive rows L / M / H | 0xD0 / 0xD5 / 0xDA | 0x40 / 0x45 / 0x4A |
| Modulation row | 0xA6 | 0x17 |
| Register 0x40 bytes (FM transmit) | 0xAB | 0x1C |

The TX band bit has no other reader except the factory test mode's frequency stepping. The FD6818 sequences, the
per-burst handlers (5.4), the bandwidth word and the sub-audio bytes (0x201 / 0x202) are the same on both bands. On
the HR_C7000 side the code examined writes FM_BANDWIDTH once (0x5F) and FM_DEV_COEF once at start-up (0x33500), and no
SUBVOICE_COEF write with a band test was found (?). So the per-band modulation byte is the only VHF deviation value:
on the radio examined the VHF bytes are about three times the UHF ones, which keeps f x byte about the same on both
bands, as the DM-32UV's K / f model assumes ([hardware-dm32.md](hardware-dm32.md) 11.3). ODR uses the UHF tone
deviations on VHF too (`modem-hrc7000.md` 8.3, I).

### 5.6 DMR

The modem is the HR_C7000's, so the slot engine, the interrupt glue and the PCM audio are the DM-32UV's, shared
(`platform/hrc7000/dmr_glue.c`, `audio_pcm.c`). The board supplies three hooks (`radio_dm1801pro.c`):

- **Per-burst RF switch** (the RF TX interrupt): RX path off, FD6818 0x52 = 028F and 0x30 = C1FE straight from RX,
  register 0x50 = 3B18, TX path on; after the burst (the engine's first slot tick 20 ms or more after the RF TX
  interrupt) the drive goes to 0, the TX path goes off, 0x30 = BFF1 straight from TX, and the RX path comes back on.
  The FD6818 never passes through idle (0x30 = 0000, 0200) per burst: through idle the receiver came back about 400 Hz
  off for the first inbound burst after each of ours, which lost most of a call in SFR mode (section 10). 0x37 and
  0x43 keep their values from the RX / TX sequences and the DMR mode set. Against the radio's own handlers (5.4) this
  skips idle and the frequency, 0x47, 0x32 and 0x3F writes (0x3F stays 0 from the DMR mode set), has none of the
  waits, steps the drive instead of ramping it, and ends the burst on a slot tick instead of the RF RX interrupt.
  Each frame takes 240 us with ODR's bus delays, so a switch is up to about 0.7 ms with interrupts off. The DMR mode
  set ends with 0x3F = 0000, the radio's DMR receive value.
- **PA key:** the TX path switches on and off with each burst. With it on for the whole transmission, a hotspot (keyed
  while it listens) received nothing (2026-10-16). Per burst, `test_dmr_tx_off_air` passes: ramps up 1.44 ms, down
  0.31 ms, slot spacing 60.00 ms. The DAC B drive is set 28 ms after each burst is armed (the DM-32UV's "DMR PA"
  delay), in one write. That puts it 2.1-3.2 ms after the RF TX interrupt (section 12), about where the radio's own
  ramp starts (5.4, I), so the delay stays.
- **Keyed but idle:** a DMR `tx()` leaves the FD6818 and the RX path in receive until the first burst. The hotspot keeps
  the radio keyed while it listens, and with the FD6818 in TX from `tx()` it heard nothing (checked on the bench).
- **Voice:** the radio's own codec, carved from a program flash read into the slot at 0x030E8000 and run by the shared
  host `platform/hrc7000/vocoder.c` (codec-dm1801pro.md 4). Without the slot, DMR runs without voice. On the bench, a
  voice call decoded by the SDR bench's modem had 1 corrected bit in 162 AMBE frames. Encode takes about 12 ms of each
  20 ms frame and decode about 3 ms, so the radio's idle load has to stay low.

### 5.7 FD6818 register fields

From the FD6818 programming guide (DS) unless tagged. The guide's own addresses differ: it calls the state word REG_03H,
and its offset calibration register is REG_04H. The radios write the state word at 0x30 (K), so the guide's address
map doesn't carry over one to one, and where REG_04H sits on these radios isn't known (?).

| Register | Bits | Meaning |
|---|---|---|
| 0x30 (state) | 15 | VCO calibration: a 0 to 1 edge starts one |
| | 14 | PA bias |
| | 13:10 | RX chain: LNA, mixer, filter, ADC |
| | 9 | AF DAC (the audio output) |
| | 8:4 | PLL and VCO |
| | 3 | PA driver |
| | 2 | Mic ADC |
| | 1 | TX DSP |
| | 0 | RX DSP |
| 0x73 | 13:11 | AFC range |
| | 4 | AFC off |
| 0x7E | 7 | TX DC filter bypass |
| | 6 | RX DC filter bypass |
| | 5:3 | DC filter bandwidth: 111 = 15 Hz ... 010 = 480 Hz |
| 0x0C (status) | 9 | PLL locked |
| | 8 | AFC at its limit |
| 0x3F (interrupt enable) | 7 | PLL lock lost |

What follows for this radio:

- RX word BFF1 has the RX chain, the AF DAC and the RX DSP on. TX word C1FE turns the RX chain and the AF DAC off. Every
  return through idle (0000) restarts both and starts a VCO calibration (b15 edge). The guide's "TX with sidetone" is
  C3FE (b9 set).
- 0x73 = 6681: AFC on, range about 550 Hz. The guide gives no registers for PLL loop bandwidth, charge pump or VCO
  band.
- 0x7E = B03F (DMR): both DC filters on at 15 Hz. The guide documents the bandwidth field only for the TX filter;
  that it sets the RX filter too is I. The guide's own DMR set-up uses 341E (240 Hz) with b7 set, and 0x73 = 569A (AFC
  off).
- Offset calibration (REG_04H b9:8) takes 50 ms with the RX chain on.
- The FD6818's audio output drives the HR_C7000's AF ADC P input, and DAC C biases its N input (`modem-hrc7000.md`
  2.1). That is why receive is sensitive to DAC C on this radio (DS for the inputs, I for the link).
- The DM-32UV writes only the state word and 0x43 per burst, and its 0x37 is 1F1F / 1F0F in DMR receive against 9D1F
  here (K). 0x37 is the only FD6818 word in the burst sequence that differs between the two; its bits aren't
  documented (?).

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

What the loader does with the data (K, from its code):

| Item | The loader |
|---|---|
| Block 0 | Copies the name (up to 256 bytes, to its NUL), then the size (up to 16 characters, to a space or NUL) and converts it. Above 0xF4002 it sends `18 18` and stops. The name is not checked |
| Erase | 0x0300C000 up to 0x03100000 in 4 KB sectors, before `06 43`. The whole application area, so the codec slot goes too |
| Data | Each block's bytes go to flash in order from 0x0300C000, with nothing skipped or parsed. A write error sends `18 18` |
| Checks | Block number, its complement and the CRC-16 of each block. No image checksum, no signature, no length check against the size |
| End | `04` ends the session; the loader waits for the next command. At power-on it jumps to 0x0300C000 unless PTT and SK1 are held, without looking at the image |

So the loader never sees a file header: the host sends the bare application. The update file the stock updater opens
is not available for this radio (no file, no updater). Every DM-32UV file examined (nine Baofeng builds and two
others) has the same header and nothing else: the ID at 0, 0xFF to 0xFC, `END` at 0xFD, then the application from
flash 0x0300C000, whose first words match this radio's (K for the DM-32UV files).

| Offset | DM-1801 Pro update file (I: the DM-32UV layout with this radio's ID) |
|---|---|
| 0x000 | ASCII `BF1801-V1` (9 bytes), the ID the loader answers to `M` |
| 0x009-0x0FC | 0xFF |
| 0x0FD | ASCII `END` |
| 0x100 | the raw application, linked for 0x0300C000, at most 0xF4000 bytes. No checksum, no length field |

Such a file can be made from an ODR image the way `tools/dm32_pack.py` makes a DM-32UV one, and `tools/dm32_flash.py
--model dm1801pro` takes it and strips the header. Whether the stock updater checks anything more is not known (?).
`tools/dm32_flash.py --model dm1801pro` writes a raw image through this protocol: about ten flashes on 2026-10-15, no
refused block, erase about 12.6 s (R). The loader at 0x1000 is never written. The stock application is 0xB3780 bytes
(flash 0x0C000-0xBF77F).

### 6.3 Identity read

SPI3 (mode 1) clocks eight bytes out; the caller mixes them into a value. The DM-32UV reads the same block. I: an
identity or integrity check.

### 6.4 Calibration

**Where.** Region 0 is addressed by logical sector. The radio reads the last byte of every physical sector 1 to 0xC8 into
a table and takes the first physical sector whose tag equals the logical number. Calibration is logical sector 2; this
is the DM-32UV's rule ([hardware-dm32.md](hardware-dm32.md) 11.1). On the radio examined that page is physical 0x7C000.
Other pages tagged 0 (0x2000, 0x3000, 0xB000, 0xF000) hold the same structure with the untuned values; they aren't
the ones the radio reads. K.

**Layout** (offsets in the 4 KB page; the field offsets match the DM-32UV's, [hardware-dm32.md](hardware-dm32.md) 11.2).
Every multi-byte value is little endian. "RX points" and "TX points" are the five frequencies of the 0x300 / 0x328 and
0x314 / 0x33C lists.

| Offset | Size | Content | Tag |
|---|---|---|---|
| 0x00 | 4 | UHF band limits: two 2-byte BCD values in 100 kHz units, 400.0 and 470.0 MHz. A value of 0, or of 1000 MHz and up, is replaced by 400 / 470 | K |
| 0x0C | 2 | low-battery voltage in 10 mV units (630 = 6.30 V, 9.2) | K |
| 0x0E | 2 | battery scale: low 12 bits, counts per volt = value x 10 / 75 (integer). Outside 1-1000 the radio uses 644 (9.2) | K |
| 0x16 | 1 | TX center | K |
| 0x17 | 5 | UHF modulation points, at the TX points | K |
| 0x1C | 3 | UHF bytes for FD6818 register 0x40, at TX points 0, 2 and 4 | K |
| 0x27 | 2 | modem RF_MOD_BIAS_CTRL high half | K |
| 0x40 / 0x45 / 0x4A | 5 each | UHF PA drive, power level 0 / 1 / 2, at the TX points | K |
| 0x64 / 0x69 | 5 each | UHF squelch, wide channels: the level 9 row / the level 1 row, at the RX points | K |
| 0x6E / 0x73 | 5 each | UHF squelch, narrow channels: level 9 / level 1 | K |
| 0x80 / 0x85 | 5 each | air band squelch, level 9 / level 1, at five even steps over 108-136 MHz | K |
| 0x8E | 9 x 2 | ADC channel 1 thresholds, one per level of a setting: VOX sensitivity (I, below) | K |
| 0xA6 | 5 | VHF modulation points | K |
| 0xAB | 3 | VHF bytes for FD6818 register 0x40 | K |
| 0xCC | 4 | VHF band limits, as 0x00: 136.0 and 174.0 MHz (136 / 174 when invalid) | K |
| 0xD0 / 0xD5 / 0xDA | 5 each | VHF PA drive, power level 0 / 1 / 2 | K |
| 0xE0 / 0xE5 | 5 each | VHF squelch, wide: level 9 / level 1 | K |
| 0xEA / 0xEF | 5 each | VHF squelch, narrow: level 9 / level 1 | K |
| 0x201 / 0x202 | 1 each | FD6818 register 0x51 low byte for CTCSS / for DCS | K |
| 0x203, 0x204 | 1 each | read at start-up (a value above 31 becomes 9); no reader of the result found | ? |
| 0x205 | 1 | low nibble read at start-up (8 to 11 kept, anything else becomes 11); its use isn't traced | ? |
| 0x206-0x209 | 4 | read at start-up and masked to 5, 7, 4 and 4 bits; no reader of the results found | ? |
| 0x300 / 0x314 | 20 each | UHF RX / TX points, five 4-byte BCD values in 10 Hz units: 400.00, 417.50, 435.50, 452.50, 470.00 MHz | K |
| 0x328 / 0x33C | 20 each | VHF RX / TX points: 136.00, 145.50, 155.50, 164.50, 174.00 MHz | K |
| 0xFFF | 1 | logical sector tag, 0x02 | K |

The radio's firmware reads nothing else from the page in the code examined: 0x10-0x15, 0x1F-0x26, 0x30-0x3E,
0x4F-0x63, 0x78-0x7D, 0xA0-0xA5, 0xAE-0xCB, 0xF4-0x107 and 0x200 have no reader (?). Some of them hold five-byte rows
like the tables above, so they may be read by code not reached here, or be left from another model.

Against the untuned copies at 0x2000 and 0x3000, the radio examined differs only in the TX center, the two sets of
modulation points, the UHF PA drive and one VHF high-power byte. The squelch rows, register 0x40 bytes, sub-audio
bytes, thresholds and point lists are the untuned values (K).

**How the radio uses them** (its own firmware, K unless marked):

| Value | Use |
|---|---|
| Segment | For a frequency f (10 Hz units), clamped to the band limits: the first point at or above f, minus one, kept to 0..3. The pair of points around f is that segment and the next |
| Interpolation | Linear between the two points' bytes, truncated toward the first point's byte |
| PA drive | The TX points and the level's row give a byte; DAC B = byte x 16. The ramp's step i of 40 is byte x t(i) / 248 rounded, from a fixed table t that rises to 248, so the last step is the byte itself |
| Modulation | The TX points and the band's modulation points give a byte b; RF_MOD_BIAS_CTRL low half = b x 0x0101. On an FM channel b is first multiplied by 1.8 (wide) or 0.9 (narrow) and kept to 255 |
| RF_MOD_BIAS_CTRL high half | The halfword at 0x27, written once at start-up (0x01EF on the radio examined, in the untuned copies too) |
| Register 0x40 | On FM transmit: 0x3480 + a byte interpolated between TX points 0 and 2, or 2 and 4, from the three bytes at 0x1C (UHF) or 0xAB (VHF). 0x85 in every point on the radio examined, so 0x3505. What the register controls isn't known (?) |
| Squelch | The channel's squelch level n (1 to 9) and Wide / Narrow bit pick two rows. Per point: level 1 = the level 1 row, 9 = the level 9 row, 2 to 8 = level 1 row + (level 9 row - level 1 row) x n / 8. The RX points give the segment for the RX frequency; a result of 0 becomes 1. The threshold is stored; what it is compared with isn't traced (?) |
| Register 0x51 | The channel's tone picks the byte: a CTCSS tone 0x201, a DCS code 0x202, none 0. The register is 0x9000 + byte for CTCSS and 0x8000 or 0xA000 + byte for DCS (normal or inverted, I from ODR's bit names in 5.4) |
| 0x8E thresholds | A setting (0 to 8) picks one halfword; ADC channel 1 above it triggers, while a setting bit is on. Nine levels in equal steps (15 to 135 on the radio examined) and a menu with "VOX SW", "Gain Level" and "Delay Time" make this the VOX sensitivity (I) |

The 0x300 and 0x328 lists are therefore the receive points: the squelch uses them, and the radio's factory test mode
tunes its items below 50 at the TX points and those from 50 up at the RX points (K). The two lists hold the same
frequencies on the radio examined.

ODR reads the PA drive, the modulation and the register 0x40 bytes the radio's way: the band by the TX frequency, the
clamp to the band limits, the segment at the TX points and the truncated interpolation (`bf_cal_tx_byte`). DAC B is the
byte x 16, RF_MOD_BIAS_CTRL low half the byte x 0x0101, and FM transmit writes 0x40 = 0x3480 + byte; receive puts the
power-on 0x34F0 back. Until 2026-10-16 ODR used the DM-32UV's lookups: the PA drive between even steps from 400 and 136
MHz (VHF point 2 at 155.0 MHz instead of 155.5), within 16 DAC counts of the radio on UHF and 32 on VHF, and the
modulation by K / f ([hardware-dm32.md](hardware-dm32.md) 11.3), within 1 of the radio's byte on UHF and 3 on VHF
(K, computed from the radio examined).

What still differs: ODR doesn't apply the FM factor (x1.8 Wide, x0.9 Narrow) to the modulation byte, since its FM
deviations were set on the bench without it (section 10); it uses TX center + 384 (0x01F0 on the radio examined) for
the high half instead of 0x27; and register 0x51 uses the DM-32UV's gain byte 0x45. None of these has shown on the
bench.

The power level is one value for the three arrays: 0 = low (0x40 / 0xD0), 1 = middle (0x45 / 0xD5), 2 = high
(0x4A / 0xDA, also used for 3). The radio's menu shows the setting as High / Low / Middle and stores 2 / 0 / 1, and the
drive bytes rise with the level in every segment (K). ODR's L / M / H use the same arrays and the same lookup.

**L / M / H on air.** At 461.4625 MHz (between the 452.5 and 470 MHz points) the radio examined gets a middle drive 1.40
times the low drive and a high drive 1.46 times the middle, in DAC counts. The bench run used ODR's older lookup,
within 9 counts of the radio's own; the lookup is now the radio's. The drive steps are about even, so the 0.4 dB
between L and M against 4.4 dB between M and H on the SDR bench (section 10) doesn't come from the calibration or from
ODR's lookup. It points at the PA's response to the drive
voltage, or at the measurement. Diagnostics lines `pa 145`, `pa 435` and `pa tx` show L / M / H as DAC counts (`pa
tx` at the last TX frequency) and `pa <kHz>k dac` the drive the last transmission used, so a wattmeter reading can be
set against them.

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
| Authentication chip | I2C 7-bit 0x3D (8-bit 0x7A), SCL PTA5, SDA PTA6 | challenge and response before the codec runs (8.1); an ALPU-MP class part. ODR doesn't use it | K bus, I part |
| Real-time clock | SoC I2C2, 7-bit 0x70 | the HR_C7000's own clock, as on the DM-32UV (8.2) | K |
| SD card | SPI1, chip select 1 | a driver in SPI mode with no caller of its start-up; no card is used (8.3) | K code, I card |
| Vibration motor | PTB24, high = on | pulse about 500 ms on an incoming call; ODR pulses it once at power-on and plays the vibration alerts on it (`hal_power_t.vibrate`, `radio-features.md` 4.1) | R |

### 8.1 Authentication chip (I2C 0x7A)

Bit-banged I2C. A start-up flag picks the pins: PTA5 / PTA6 (the value used; the IO mux puts them on GPIO) or PTA7 /
PTA8 (never chosen). Frames are the 8-bit address 0x7A (write) or 0x7B (read), a register, then the data. K.

| Step | Registers |
|---|---|
| First use | write 8 zero bytes to 0x80; 0x20 = 00; 0x22 = 0x83; read 16 bytes from 0x30, write 16 bytes back to 0x30; read 8 bytes from each of 0x73, 0x74, 0x75, 0x76; write 16 bytes to 0x40 |
| Each check | 8 random bytes go through a local cipher; 16 bytes are written to a register and read back from it 5 ms later; the reply is compared with the local result |
| Tries | three; the result is a status byte |

The check runs after the codec block's set-up and from routines around the codec, among them two task loops (K for the
callers, I for their role). The code sits inside the codec's RAM block, in the range the DM-32UV notes list as stock
code that the codec never calls ([codec-dm32.md](codec-dm32.md)). K.

The part is inferred (I): the same pins carry the ALPU-MP copy-protection chip on the DM-32UV
([hardware-dm32.md](hardware-dm32.md) 14), and the traffic is a challenge and response. No marking was read. ODR never
talks to it, and DMR voice works with the radio's own codec (5.6), so ODR's codec host doesn't need it (R).

### 8.2 Real-time clock (I2C2)

The SoC's I2C2 at 0x14080000, set up as on the DM-32UV ([hardware-dm32.md](hardware-dm32.md) 3.3): CON 0x65, INTR_MASK
0x40, SCL counts for 400 kHz, FIFO thresholds 6. Address 0x70. K.

| Register | Use |
|---|---|
| 0x00 / 0x01 / 0x02 | seconds / minutes / hours |
| 0x03 / 0x04 | days since 1970-01-01, low / high byte |
| 0x0C-0x10 | time to load: seconds, minutes, hours, then the day count low and high; 0x12 = 0 before, 1 after |
| 0x11 | control: bit 1 set at start-up, bit 2 set after a load |
| 0x12 bit 0 | read at start-up; with it clear or a day count of 0 the clock is reset (0x17 = 0, wait, 0x17 = 1) and set to 2016-09-02 16:45:20 |

ODR doesn't use the clock yet.

### 8.3 SD card (SPI1)

SPI1 at 0x140B0000, chip select 1 of the controller, clock divider 16 (a routine for divider 2 has no caller). The
driver speaks SD in SPI mode (K):

| Item | Commands |
|---|---|
| Start-up | 80 clocks, CMD0 (CRC 0x95), CMD8 0x1AA (CRC 0x87, echo checked), CMD55 + ACMD41 with HCS until ready, CMD58 (CCS: block or byte addressing) |
| Identity | CMD9 (CSD, capacity for version 1 and 2 cards), CMD10 (CID) |
| Data | 512-byte blocks: CMD17 / CMD18 + CMD12 read, CMD24 / CMD25 write (tokens 0xFE, 0xFC, 0xFD; data response 0x05) |

Nothing calls the start-up, the identity reads or the write path (K). The only reachable use is a block read from one
programming-cable command (`D`), which without the start-up reads with byte addressing. The firmware also holds "NO SD
CARD" screen strings. So the code is shared with a model that has a card, and this radio has none (I; no slot was
looked for). ODR doesn't use SPI1.

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

PTB11 (PTT) is also the programming cable's host-to-radio serial line, as on the DM-32UV (hardware-dm32.md 6.2):
every 0 bit the host sends pulls it low. Before the fix, two low key scans passed the debounce and made 13 short DMR
keyups during a debug link soak and one during an MMDVM network call (2026-10-16). The key scan now counts a low PTT
pin as pressed only after 50 ms without a good byte from the host (`drivers/input/sharedptt`); diagnostics line 0
counts the ignored lows (`ptt ign`). On the radio (R): 30 s of debug link polling gave `ptt ign 20`, and the two
hotspot network cases another 14, with no PTT press.

### 9.2 Battery

ADC channel 2, averaged. The radio scales it with the calibration halfword at 0x0E (6.4): volts x 100 = counts x 100 /
(value x 10 / 75), and shows bars at 8.99, 7.99, 7.50, 6.99, 6.49, 6.31 and 5.81 V; the halfword at 0x0C (6.30 V) sets a
second low-battery flag (K). On one radio, switched on, the channel read 856 counts while a DMM read 8.306 V across the
pack (2026-10-16), so ODR uses 103 counts per volt (the DM-32UV's divider gives 82; R). That radio's calibration page
gives 104 counts per volt (value 780), which agrees.

### 9.3 Interrupts and timers

One timer gives the 10 ms main-loop tick; the GPIO banks, UART0 and the modem (PCM read and write, system, TX and RX
slots, RF TX and RX) have vectors as on the DM-32UV. The I2C, SPI, ADC and DAC blocks are polled.

### 9.4 Power, backlight, LEDs

PTA17 is the power latch, PTB9 the power key (polarity unverified; ODR takes the level at start-up as "on"). The backlight
is on PTA15. Stock only switches it as a GPIO; PTA15 is also the SoC's PWM0 output (as on the DM-32UV), so ODR dims it
with PWM0 for the brightness setting (I until seen on the radio). LEDs: PTB0 green, PTB1 red (R). The panel's top LED is red while transmitting, green while
receiving and orange flashing while scanning.

## 10. Open items

- VHF transmit on air. The code follows the radio's (5.5, "VHF transmit"), but no VHF transmission has been checked.
  To check: the carrier on frequency at L / M / H (the `pa 145` line gives the drive), CTCSS, DCS and the 1750 Hz tone
  deviation against the UHF values (the tone deviations are the UHF ones, I), and a DMR burst decoded on VHF.
- The power levels on air: on the SDR bench L and M are only 0.4 dB apart and H is 4.4 dB above M (2026-10-16, 461.4625
  MHz, the drive from the 0x40 / 0x45 / 0x4A arrays). The drive itself steps evenly (6.4, "L / M / H on air"), so the
  cause is the PA or the measurement. The SDR's levels are relative and the radio is in its near field, so this wants
  a wattmeter, read against the `pa tx` diagnostics line.
- The display controller part and its contrast.
- FM transmit deviation at 12.5 kHz. The TX limiter is 5 there, scaled from the 25 kHz results (section 12); not
  measured. Speech deviation at both bandwidths with the new limiter needs a cable-out session. The radio itself
  writes FM_DEV_COEF only once, 0x33500 at start-up (limiter 3, TX coefficient 0, K), and FM_BANDWIDTH = 0x5F for both
  bandwidths; how it reaches its deviation with those values is unknown (?). Its FM factor on the modulation byte
  (6.4: x1.8 on a Wide channel, x0.9 on a Narrow one, kept to 255) may be the answer; ODR leaves it out.
- In the calibration page (6.4): what register 0x40 controls, what the squelch thresholds are compared with, the
  fields read at start-up without a found reader (0x203-0x209), and the unread rows.
- The authentication chip's marking (8.1), and what PTB16 drives (3.1).
- Whether the stock updater accepts a `BF1801-V1` file made as in 6.2. Neither the updater nor a stock update file for
  this radio is at hand.
- The FM mic gain levels. The FM values are still the DM-32UV's 11 dB (section 12); measure them with
  `tests/hil/test_mic_gain.py` at 12.5 and 25 kHz ([the bring-up checklist](../testing/dm1801pro-bringup.md) 5.1) and
  set `MIC_GAIN_FM_12K5_DB` and `MIC_GAIN_FM_25K_DB`.
- Whether the mic PCM carries the microphone in FM receive (PTB14 high), which VOX needs (section 12).
- MMDVM SFR: the inbound losses are fixed by switching the FD6818 straight between its RX and TX words (5.6). What
  remains: 28 of 29 bursts heard with 2-4 bad header CRCs, and the SDR bench can't confirm the repeats during a call,
  since its station keeps its carrier up through our slot. How the trip through idle detunes the receiver isn't
  known (?). The experiment build (PR 38) recorded each modem interrupt, RF switch and deframe (RX_TYPE_INFO,
  RECV_INFO_COUNT, PHASE_OUT) and switched these variants at run time (BENCH 2026-10-16, 461.4625 MHz, a 29-burst bench
  call):

  | Variant | Bursts heard | Sent up | Bad header CRC |
  |---|---|---|---|
  | Through idle, ODR before the fix | 20-21 | 8-9 | 3 |
  | No RF switching, no PA ("dry") | 30 | 26 | 0 |
  | RX / TX paths switch per burst, FD6818 held in receive | 28 | 26 | 0 |
  | **Straight between BFF1 and C1FE, no idle (the fix)** | **28** | **24** | **2-4** |
  | Straight, plus 0x7E = B07F, TX word C3FE or FDFE, or a b15 edge at each switch | 28 | 24 | 2-4 |
  | Straight, b15 clear in both words (3FF1 / 41FE) | 2 | 2 | 0 |
  | FD6818 switches per burst, paths held | 20 | 8 | 3 |
  | PA drive 0; FD6818 PA bias and driver off (TX 81F6) | 21 | 8 | 3 |
  | Through idle with TX word C3FE, FDFE or FFFF; 0x7E = B07F or B01F; AFC off; DAC_CONTROL 0x80000009 | 20-21 | 8 | 3 |
  | Through idle with: DMR filter 0x43 = 4048; 0x50 / 0x52 restored at RX; dynamic sync off; RF TX interrupt 2.0 ms ahead; the DM-32UV's two-write switch; no 0x30 = 0200 | 20-21 | 8 | 3 |
  | The radio's tables without 0x37; no 0x50 write; a full RX retune at each return; the radio's own burst sequence (5.4) | 11-13 | 3-7 | 1 |
  | 0x37 = 1F1F at RX (the DM-32UV's word); HR_C7000 AF gain 0x1C | 0-2 | 0-1 | 0-1 |

  Through idle, the first inbound burst in each listen window after one of ours demodulated with an offset:
  PHASE_OUT's center read +185 to +315 (minimum -600 to -850 against -1213 normally), about 400 Hz, and failed sync or
  EMB. Later bursts settled to about 0 within 60-90 ms. With the straight switch the centers read 1 to 35 from the
  first burst on. Keeping the RX chain, the AF DAC or the RX DSP on through TX (FD6818 programming guide bits, 5.7)
  doesn't help while the switch passes through idle, so what settles is in the PLL / VCO section that idle powers down
  (I). DMR transmit with the straight switch passes `test_dmr_tx_off_air` (ramps up 0.88 ms, down 0.38 ms, AMBE 1 bit
  corrected in 162 frames).

  Ruled out earlier: an FD6818 bit clock of 1 us instead of 5 us, and the RX path left on during TX bursts.

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
| VOX mic level | `platform/hrc7000/audio_pcm.c` (the meter on the mic PCM), `drivers/audio/miclevel` (the level math) |
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
(normal and inverted) was decoded by a DM-32UV, and the SDR bench's FM transmit tests (CTCSS, DCS, tone burst, roger
beep) pass. The bandwidth test passes too: the CTCSS 100.0 tone deviates 298 Hz at 12.5 kHz (word 0x3048) and 597 Hz at
25 kHz (0x3028), a ratio of 2.00 (2026-10-16).
The MMDVM hotspot on the programming cable works both ways on air (2026-10-16: the four `test_hotspot_on_air` cases).
Not in: the air band, the broadcast tuner's screens, USB (the radio has none).

VOX (not checked on the radio): the radio has no analog microphone level tap, so ODR measures the level on the codec
ADC's mic PCM, the same 10 ms halves DMR voice uses (`radio-features.md` 3.4). The first `mic_level()` call sets
AUDIO_CONTROL b4 and unmasks the PCM interrupt without starting the PCM stream; 1 s without a call turns them off
again unless the stream is running. The PCM interrupt runs in FM: FM beeps are paced by it. In FM receive with the
squelch closed PTB14 is high (the microphone to the HR_C7000's FM input, as for an FM transmission), so the codec ADC
should hear the microphone the way it does on transmit. Whether its samples carry the microphone in receive is the
first thing to check. On the radio: `vox.mic=1`, then `vox.level` (features section) and the diagnostics line
`mic lvl L pk P n N on|off` (level, the last half's peak from its mean, halves measured, running): N should grow by
about 100 a second while `vox.level` is read, and L and P should rise when someone talks at the radio
(`test_radio_features::test_mic_level`, radio only; `test_vox_quiet` with `--allow-rf`).

Mic gain: GCR_ADCL (bits 5:0 = 0 to 42 dB, `hardware-dm32.md` 9.1) gets three board levels, as the stock firmware
writes separate FM and DMR values (`modem-hrc7000.md` 4 step 32, 5.1 step 2): `MIC_GAIN_DMR_DB` at power-on, at
every DMR entry and at every DMR transmit start, and `MIC_GAIN_FM_12K5_DB` or `MIC_GAIN_FM_25K_DB` at every FM
transmit start, before FM_PTT. All three are the DM-32UV's 11 dB (0x8B) for now. The Mic gain setting adds (step -
5) x 2 dB to each through `hal_radio_t.mic_gain` (`radio-features.md` 3.6), so the bench can step it without a
reflash (debug link SETTING 29). Speech on FM gave 1.4 kHz peak deviation (above), which is low; the channel
bandwidth of that measurement isn't recorded, so the target isn't settled. If the path is linear up to the deviation
limiter, each dB of gain adds 1 dB of deviation. See section 10.

APRS: positions over DMR (LRRP and UDT) decode on the SDR bench. FM APRS uses the shared HR_C7000 AFSK
(`platform/hrc7000/hrc_afsk.c`), which plays the tones through the prompt FIFO. On the DM-32UV the FIFO's playback
reaches the modulator by itself (ring play). On this radio it doesn't: the frame went out as a bare carrier (7 Hz rms
in the 300-3000 Hz band on the SDR bench, 2026-10-16), and every direct codec register, AUDIO_CONTROL, WORK_MODE,
FM_PTT and RF_MODE matched the DM-32UV's during the tones. An experiment build tried each candidate route on air
(BENCH 2026-10-16):

| Route | Result |
|---|---|
| Ring play alone, mic muted (the DM-32UV's) | carrier only |
| Codec mixer MIX_2 mix_rec, mic muted | carrier only |
| Codec mixer MIX_2 mix_rec, MIX_3 zeroes the ADC's own input, mic not muted | decodes, peak 1.60 kHz |
| TX RAM as the FM source (AUDIO_CONTROL b0) | decodes, peak 1.59 kHz |
| FD6818 tone 1 retuned per symbol | decodes, peak 5.15 kHz |
| FD6818 0x47 = 0x6042, line out 1, PTB16, FM_PTT order, TONE_SEND_CTRL b6, AUDIO_CONTROL b3 | carrier only |

ODR takes the mixer route (`hrc7000_codec_dac_to_tx`, `modem-hrc7000.md` 8.4): it keeps the FIFO feed both radios
share. The codec's mic mute sits after the mix, so it would mute the tones too; MIX_3 keeps the microphone out
instead. With it, positions decode off air with a valid FCS at every level tried.

At first the deviation saturated near 1.65 kHz peak at 25 kHz, whatever the level. The cause was the TX limiter,
FM_DEV_COEF b23:16 (`modem-hrc7000.md` 8.4): the KP value 4 caps all audio through the codec ADC, the microphone
included. This radio sets 0x0F at 25 kHz (the chip's reset value; 4.7 kHz most) and 5 at 12.5 kHz. With 0x0F the
tones are linear in the level, and the 2200 Hz tone sits 5.1 dB above the 1200 Hz one, the slope of the TX
pre-emphasis (FM_BANDWIDTH b4) between them. With `amp_per_level` 68 at 25 kHz (BENCH 2026-10-16):

| APRS level | Peak | Mark | Space |
|---|---|---|---|
| 20 | 1.89 kHz | 0.95 kHz | 1.70 kHz |
| 30 | 2.83 kHz | 1.41 kHz | 2.55 kHz |
| 40 | 3.78 kHz | 1.89 kHz | 3.39 kHz |
| 50 | 4.62 kHz | 2.36 kHz | 4.23 kHz |

`amp_per_level` is 46: at level 50 the position decoded with 3.25 kHz peak (mark 1.61, space 2.91 kHz) and
`test_aprs` passes. APRS receive is unchecked.

MMDVM SFR: the radio hears the call's header, uplinks it and repeats it in the other slot with the right link
control, on the right slot and a clean 60 ms grid. While it sends, it loses inbound bursts (2026-10-16,
`test_hotspot_sfr.py::test_sfr_repeats_bench_call`). The bench's call is clean on the SDR (8 voice LC headers 60 ms
apart, 18 voice bursts, 3 terminators, no errors), but only about 20 of its 29 bursts reach the SFR runner. Headers at
60, 180 and 360 ms are missing and those at 118 and 299 ms fail their CRC; voice bursts at about 539, 722, 778, 838
and 1210 ms come with a wrong EMB (color code 1, 0 or 9 for 2, a wrong voice index). The pattern is the same in every
run, also right after a fresh start. The DM-32UV on the same test: 25 received, 22 repeated, 3 bad CRCs (the
terminators). Our bursts: RF on to off 32-33 ms, the RF switch 2.06 ms with interrupts off, the PA keyed 2.1-3.2 ms
after RF on. Fixed by switching the FD6818 straight between RX and TX per burst (5.6): 28 heard, 24 sent up (section
10).
