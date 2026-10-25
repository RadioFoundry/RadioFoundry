<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# TYT MD-9600 / Retevis RT90 hardware (mobile)

The TYT MD-9600 and the Retevis RT90 are one dual-band mobile radio: an STM32F405VGT6 MCU, an HR_C6000 DMR baseband
and codec chip, **two complete RF chains (one VHF, one UHF)**, each with its own synthesizer, its own RX and TX VCOs,
its own IF receiver chip and its own PA supply switch, a 49.95 MHz first IF, a 128 x 64 monochrome LCD (ST7567) on
SPI, a 4 x 3 front panel key matrix, a quadrature channel encoder, a 16-key DTMF hand microphone on an RJ-45 jack read
as two analog levels, a GPS option, and USB full speed on the MCU's own OTG_FS core (mini-USB on the rear). There is
no AT1846S and no RF transceiver chip at all; the HR_C6000 sees a 450 kHz IF from the IF receiver chip.

TYT changed the synthesizer and IF parts during production. The board revision decides how the synthesizers (and on
V5 the IF) are programmed, and **nothing in the firmware can read the revision**, so a build is made per revision (section 16).

Status: **nothing in this file has been checked on a radio.** No MD-9600 is in hand, and every value is unverified until
one is. Section 17 lists every open question.

Conventions: register values are hex; HR_C6000 registers are written `page:reg = value` as in
[modem-hrc6000.md](modem-hrc6000.md). Frequencies are given in Hz or MHz. A value marked `(datasheet)` is from the
STM32F405 data sheet and RM0090 or the Skyworks SKY72310-11 data sheet; `(unverified)` is not confirmed on a radio or
in a datasheet; `?` means not found or contradictory.

---

## 1. MCU, clocks, memory

### 1.1 Part

STM32F405VGT6, LQFP100, 1 MB flash, 128 KB SRAM + 64 KB CCM, 4 KB backup SRAM. The backup SRAM
is used (section 10.2). Read-out protection level 1 assumed, as on every TYT STM32 radio (?).

### 1.2 Clocks

| Item | Value |
| --- | --- |
| HSE | 8 MHz external clock in **bypass** mode, as the DM-1701 |
| LSE | 32.768 kHz crystal, RTC enabled |
| PLL | M 4, N 72, P 2, Q 3: SYSCLK 72 MHz, USB 48 MHz |
| AHB / APB1 / APB2 | 72 MHz / **18 MHz (/4)** / **36 MHz (/2)** |

Standby (section 10.2) drops SYSCLK to HSE / 512 with both APBs / 16 and voltage scale 2 while waiting for the power
key.

### 1.3 Internal flash layout

Same TYT loader and layout as the DM-1701 and MD-380: loader in sectors 0-2 (0x08000000-0x0800BFFF, never written),
application from 0x0800C000, VTOR 0xC000. The codec sits at a different address in every vendor version and variant
(0x08074B18 to 0x08079630 for the code; 0x0807537C in V26.45; [codec-md9600.md](codec-md9600.md)).

### 1.4 RAM

RadioFoundry maps CCM (as on the DM-1701). Backup SRAM: 4 KB at BKPSRAM_BASE, kept while DC is
applied, used for the standby flag (10.2) and for non-permanent settings.

---

## 2. Pin assignments

### 2.1 Port A

| Pin | Function | Notes |
|---|---|---|
| PA0 | ADC1 IN0: VHF RSSI |  |
| PA1 | ADC1 IN1: supply voltage | 10.3 |
| PA2 | ADC2 IN2: QT_DQT_IN, sub-audio tone input | tones are decoded in the HR_C6000 |
| PA3 | ADC1 IN3: VOX / mic level | the only mic level input (no separate "mic" signal) |
| PA4 | DAC1: TV_U, UHF receiver front-end tuning |  |
| PA5 | DAC2: APC_TV_V, **shared** TX power (APC) reference and VHF receiver front-end tuning |  |
| PA6 | ADC1 IN6: hand-mic keypad, row level ("P2") | 9.3 |
| PA7 | ADC1 IN7: hand-mic keypad, column level ("P1") | 9.3 |
| PA8 | C5_U_SW, UHF 5 V supply (receiver side) | high = on |
| PA9 | USART1 TX **and** GPS + microphone power switch | driven as GPIO high = mic and GPS powered (13.2) |
| PA10 | USART1 RX: GPS NMEA in, 9600 baud |  |
| PA11 / PA12 | USB OTG_FS D- / D+ |  |
| PA13 | R5_U_SW, UHF receiver 5 V; **also SWDIO** |  |
| PA14 | R5_V_SW, VHF receiver 5 V; **also SWCLK** |  |
| PA15 | I2S3 WS (AF6) |  |
### 2.2 Port B

| Pin | Function | Notes |
|---|---|---|
| PB0 | ADC1 IN8: UHF RSSI |  |
| PB1 | ADC1 IN9: UHF noise (squelch) level |  |
| PB2 | FM_SW, FM audio stages on (high) |  |
| PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI (AF5): HR_C6000 voice port |  |
| PB6 | AF_SW, audio amplifier mute, TIM4 CH1 PWM (AF2) | 9.x / 6.1 |
| PB7 | U_V_MOD_SW: HR_C6000 MOD2 output to the VHF (high) or UHF (low) VCO |  |
| PB8 / PB9 | I2C1 SCL / SDA (AF4, open drain) | nothing on the bus is known to be used (?) |
| PB10 / PB11 | Channel encoder A (EXTI, both edges) / B; USART3 TX / RX when the optional remote head is fitted | 9.2, 9.5 |
| PB12 | SPI flash chip select |  |
| PB13 / PB14 / PB15 | SPI2 SCK / MISO / MOSI (AF5): SPI flash **and** LCD | mode 0, 4.5 MHz (12.1) |

### 2.3 Port C

| Pin | Function | Notes |
|---|---|---|
| PC0 / PC1 / PC2 | HR_C6000 timeslot / system / RF TX interrupts (EXTI0-2, falling edge) | see 4.4 for the RF TX line |
| PC3 | ADC2 IN13: RX_2T_5T (discriminator audio) | unused |
| PC4 | RF_APC_SW, PA power loop on |  |
| PC5 | ADC1 IN15: VHF noise level |  |
| PC6 | Display_Light: LCD and key backlight, TIM3 CH1 PWM (AF2) | 7.3 |
| PC7 | CTC_DCS_PWM, sub-audio tone output | open drain, held high; tones are generated in the HR_C6000 instead |
| PC8 | BEEP_PWM | unused (beeps go through the HR_C6000) |
| PC9 | C5_V_SW, VHF 5 V supply (receiver side) |  |
| PC10 / PC11 / PC12 | I2S3 CK (AF6) / I2S3ext SD (AF5) / I2S3 SD (AF6) |  |
| PC13 | U_V_AF_SW: receive audio from the VHF IF chip (high) or UHF (low) |  |
| PC14 / PC15 | LSE crystal |  |
### 2.4 Port D

| Pin | Function | Notes |
|---|---|---|
| PD0 / PD1 | Key matrix rows K2 / K3 (open drain) |  |
| PD2 / PD3 / PD4 | Key matrix columns K4 / K5 / K6 (inputs, pull-up) | PD2 low at boot also means "remote head fitted" (9.5) |
| PD5 / PD6 | T5_V_SW / T5_U_SW: VHF / UHF transmitter 5 V |  |
| PD7 | HR_C6000 voice port chip select |  |
| PD8 / PD9 | PLL_CS_V / PLL_CS_U: VHF / UHF synthesizer chip selects |  |
| PD10 / PD11 | PLL_DATA / PLL_CLK, shared by both synthesizers |  |
| PD12 / PD13 / PD14 | LCD reset / RS (data-command) / chip select |  |
| PD15 | Power_Control: main power to the RF and audio sections (high = on) | 10.1 |

### 2.5 Port E

| Pin | Function | Notes |
|---|---|---|
| PE0 / PE1 | Key matrix rows K0 / K1 (open drain) |  |
| PE2 / PE3 / PE4 / PE5 | HR_C6000 configuration port CS / CLK / MISO / MOSI (bit-banged) | as the DM-1701 |
| PE6 | C6000_PWD, HR_C6000 power down |  |
| PE7 | FM_MUTE: FM audio to the volume control, high = unmuted |  |
| PE8 / PE9 | VCO_VCC_U_SW / VCO_VCC_V_SW: per band, high = RX VCO, low = TX VCO |  |
| PE10 | PTT (hand mic), input with pull-up, active low |  |
| PE11 / PE12 | V1-V4: IF_CS_V / IF_CS_U (IF chip selects). **V5: VHF / UHF IF bandwidth, high = 25 kHz** | 5.4 |
| PE13 / PE14 | V1-V4: IF_DATA / IF_CLK. **V5: connect the VHF / UHF IF to the HR_C6000** (one high, the other low) | 5.4 |
| PE15 | V1-V4: IF_RST (IF chip reset, active low) | V5 use unknown (?) |

### 2.6 Reset-time levels and SWD

Reset levels: HR_C6000 CS/CLK/MOSI and power-down low, IF chip selects/data/clock high, IF reset
low, both synthesizer chip selects and the voice port CS high, synthesizer data/clock low, LCD lines low, Power_Control
low, all T5/R5/C5 supply switches low, U_V_MOD_SW low, FM_SW and FM_MUTE low, CTC_DCS_PWM high. Then
`radioPowerOn` sets both T5 low, all C5 and R5 high (both receivers are always powered together, "as the TYT firmware
does"), then Power_Control high.

PA13 / PA14 are the receiver supplies of both bands: like the MD-380, the radio takes the SWD pins at run time, and a
debugger has to connect under reset (D for the pins; the consequence ours).

---

## 3. Interrupts, DMA, timers

| Source | Use |
| --- | --- |
| EXTI0/1/2 (PC0-PC2) | HR_C6000 timeslot / system / RF TX, priority 5 |
| EXTI15_10 (PB10) | Channel encoder A, both edges, priority 5 |
| DMA1 Stream0 ch0 / Stream5 ch2 | I2S3 RX / I2S3ext TX, circular |
| DMA2 Stream2 ch4 | USART1 RX (GPS), circular |
| DMA1 Stream1 / Stream3 ch4 | USART3 RX / TX (remote head only) |
| TIM3 CH1 | Backlight PWM, period 3000 counts (prescaler 0) |
| TIM4 CH1 | Amplifier mute PWM on AF_SW, prescaler 16, period 100 |
| TIM6 | HAL time base |
| ADC1 | 9-channel scan (10.3), 480 cycles each, interrupt per conversion |
| ADC2 | IN2 (QT_DQT_IN), single, 3 cycles |

---

## 4. HR_C6000

### 4.1 Hookup

Configuration port bit-banged on PE2-PE5, voice port on SPI1 (PB3-PB5, CS PD7, CPOL high, CPHA first edge,
PCLK2 / 64 = about 560 kHz), I2S3 slave full duplex Philips 16-bit (extended) 8 kHz, power down PE6, interrupts
PC0-PC2: the DM-1701's wiring except the voice port, which is on SPI1 here instead of the DM-1701's SPI. The
register behavior is the DM-1701's ([modem-hrc6000.md](modem-hrc6000.md)).

### 4.2 Differences from the DM-1701 set-up

| Item | MD-9600 |
| --- | --- |
| Clock / PLL | 4:0x0A=0x80, 0x0B=0x28, 0x0C=0x33, 0x0A=0x00, 0xB9=0x05, 0xBA=0x04, 0xBB=0x02: the same values RadioFoundry already uses |
| IF | 450 kHz (4:0x07-0x09 = 0x0B 0xB8 0x00), from the IF receiver chip, two-point modulation (4:0x01=0xB0) |
| FM receive audio | **not through the HR_C6000**: 4:0x36 = 0x00 always. FM audio comes from the IF chip's demodulator via U_V_AF_SW, FM_SW, FM_MUTE (6.1) |
| FM TX, CTCSS, DCS, DTMF, tones | through the HR_C6000 (TX tone generator, RX CTCSS / DCS detector); PWM on PC7 is reported for TX tones instead (unverified) |
| MOD2 routing | U_V_MOD_SW (PB7) sends the MOD2 output to the active band's VCO; set with every synthesizer write |
| Reference / MOD2 offset | per band byte t from calibration (11.2); signed c = t - 128; 4:0x47 = c, 4:0x48 = 0x03 if c < 0 else 0x00, 4:0x04 = c. Rewritten at every RX entry (otherwise an RX offset is reported after TX) |
| DMR sync detection | 4:0x5F = 0xF0 (MS, BS, TDMA1, TDMA2) |

### 4.3 Voice port and I2S

As the DM-1701 (4.3 / 4.4 of [hardware-dm1701.md](hardware-dm1701.md)) apart from the SPI instance. DMR receive AGC
maps a peak level to the HR_C6000 DAC gain; not a hardware fact.

### 4.4 Burst timing

The MD-9600 is reported to lack the hardware for the receive-side interrupt the other STM32 radios
use to end a TX burst (unverified); the burst can end from the timeslot interrupt once the PA has been on 20 ms. Whether
the RF TX interrupt (PC2) works as on the DM-1701 is not stated (?). [gotchas.md](gotchas.md) 14.6: every TX/RX switch means a new
synthesizer word and a different VCO (5.2), so the per-burst relock time is the main DMR timing risk.

---

## 5. RF

### 5.1 Bands, power

Dual band, 136-174 MHz and 400-480 MHz (unverified); 127-178 and 380-564 MHz are reported outside the data-sheet mode (unverified; not a recommendation). Rated 50 W VHF / 45 W UHF (40 W on some retailer pages); one review measured 44 W
at 146 MHz and 40 W at 446 MHz (unverified). Band selection is by frequency: below 300 MHz is VHF.

### 5.2 Frequency plan (all revisions)

| Item | Value |
| --- | --- |
| Reference | 16.8 MHz TCXO, shared |
| First IF | 49.95 MHz |
| VHF RX | LO = f + 49.95 MHz (high side) |
| UHF RX | LO = f - 49.95 MHz (low side) |
| TX | VCO on the carrier (UHF), x2 on V1/V2 VHF (5.3) |
| RX vs TX VCO | separate VCOs per band, picked by VCO_VCC_x_SW (high = RX) |

One synthesizer per band drives both of that band's VCOs, so RX and TX (49.95 MHz apart) always mean reprogramming.

### 5.3 Synthesizer per revision

All write-only, chip select low during the word, data MSB first, data set while the clock is low and latched on the
rising edge. Fractional rule in V1/V2/V4: if the fraction is above one half, N is rounded up and the fraction
used as a negative offset (signed modulator).

**V1, V2 (and V3): Skyworks SKY72310, one per band**. 16-bit words, 4-bit address in the top
bits (as [hardware-md380.md](hardware-md380.md) 5.2, data sheet):

| Band | R (reg 5 = R - 1) | Comparison frequency | VCO |
|---|---|---|---|
| VHF | 6 | 2.8 MHz | 2 x LO, the VHF output divides by 2 |
| UHF | 4 | 4.2 MHz | LO |

N = VCO / f_pd; reg 0 = N - 32; 18-bit fraction (x 2^18), reg 1 = fraction >> 8, reg 2 = low 8 bits. Fixed: reg 6 =
0x01F, reg 7 = 0x3D0, reg 8 = 0, reg 9 = 0. Write order: 8, 9, 5, 0, 2, 1, 6, 7 ("the order the TYT firmware uses")
.

**V4: "PLL1590" synthesizer, one per band**. TYT's own folder for the V4 firmware (V16.45) is named "firmware for
PLL1590", which names the part family (AK1590); the exact part number is unconfirmed (?). Its programming words are reg 4 = 0x1FF40 (VHF) and 0x19F40 (UHF) in the format below (unverified). 24-bit
words: 20 data bits then the 4-bit address in the low nibble. N in reg 2 (VHF 0x20000 | N, UHF 0x38000 | N), the
18-bit fraction in reg 1, reg 3 = 0x6000 | R, reg 4 = 0x1FF40 (VHF) or 0x19F40 (UHF), reg 5 = reg 6 = 0. R: VHF 12,
UHF RX 8, UHF TX 4. **No x2 on VHF** here (VCO on the LO). Write order 5, 6, 3, 4, 1, 2. The V4 VHF synthesizer must
be programmed (145 MHz RX) right after power-on or it never locks.

**V5 (and V4A): TI LMX2571, one per band** ("PLL 2571" on a retailer page, unverified). 24-bit words: 8-bit address then 16 data bits. R: VHF
8, UHF 4; 24-bit fraction; reg 0x04 = 0x3000 + (N & 0xFF), reg 0x01 = fraction >> 16, reg 0x02 = fraction low 16,
reg 0x03 = 0, reg 0x05 = 0x100 + R, reg 0x06 = 0x8581, reg 0x07 = 0x1024, reg 0x08 = 0x0430, reg 0x29 = 0x0230,
reg 0x2A = 0x0108, reg 0x00 = 0x003E. Sequence: reset (0x00 = 0x2000), 0x00, 0x04, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07,
0x08, 0x29, 0x2A, then 0x00 = 0x003F (start calibration) and 0x00 = 0x003E. The clock is left low after each word.
Delays after reset and calibration may be needed (?).

No lock detect input is known to be wired on any revision (?).

### 5.4 IF receiver per revision

**V1-V4: AKM AK2365A, one per band**. Bit-banged 16-bit words on IF_DATA (PE13) / IF_CLK (PE14) with chip
selects PE11 (VHF) / PE12 (UHF), reset PE15 (pulse low for at least 1 us). Data set while the clock is low, latched on
the rising edge. Written each time the receiver is turned on: soft reset 0x08AA, then 0x02F1 (25 kHz) or 0x02E9
(12.5 kHz), 0x041D (AGC), 0x0600 (IF gain 6 dB), 0x1601, 0x1880 (auto AGC). The chip's 450 kHz output feeds
the HR_C6000, its demodulated audio the FM path (6.1).

**V5: no programmable IF chip.** PE11 / PE12 set the VHF / UHF IF filter (high = 25 kHz), PE13 / PE14 connect the VHF
or UHF IF to the HR_C6000. The V5 IF part is not identified (?).

### 5.5 Receive and transmit switching

RX: Power_Control on, both R5 and both C5 on, T5 off, RF_APC_SW off; synthesizer to the LO with VCO_VCC high for the
band; DAC1 (UHF) or DAC2 (VHF) = the calibrated front-end tuning voltage; IF set up for the band and bandwidth;
U_V_AF_SW to the band.

TX: both R5 off, synthesizer to the carrier with VCO_VCC low, DAC2 = APC drive (for both bands: PA5 serves the APC in
TX), then the band's T5 on and the other off, then RF_APC_SW on. Per DMR burst only T5 and RF_APC_SW toggle; return to
RX drops RF_APC_SW, both T5, then reprograms for RX. The ordering beyond that is not determined; no
lock check (?).

### 5.6 Power levels and control

DAC2 (12 bit) is the APC reference. Four calibrated points per frequency (11.2) labeled 5 W,
10 W, 25 W and 40 W (labels, not measured); lower levels as a fixed fraction of the 5 W value (VHF 0.82-0.90, UHF
0.41-0.64 for 100 mW..1 W). Power is reported poorly calibrated on some radios, and some older units
only working well on DMR at full power (unverified). RadioFoundry: refuse TX without an accepted table, as on the MD-380.

### 5.7 RSSI, noise, squelch

Analog per band: VHF RSSI PA0, UHF RSSI PB0, VHF noise PC5, UHF noise PB1. A fit, not a measurement (?): V1/V2/V4
-135 dBm (VHF) / -145 dBm (UHF) + raw8 / 2; V5 -155 dBm + raw8 / 1.95 (raw8 = the 12-bit average >> 4). Calibration also has RSSI points at -120 and -70 dBm and squelch open / close rows (11.2).

---

## 6. Audio

### 6.1 Paths

FM: IF chip discriminator -> U_V_AF_SW (band) -> FM_SW (stages on) -> FM_MUTE (high = to the volume control) -> analog
volume knob -> amplifier. DMR and beeps: HR_C6000 line out -> volume control. The amplifier mute is AF_SW (PB6) driven
as PWM from TIM4: 0 % = unmuted, 100 % = muted; a partial duty (30 %) can be used for click suppression. The volume knob is analog only (no ADC input): no software volume.

### 6.2 Microphone

Hand mic on the RJ-45 jack: mic audio to the HR_C6000 line input, PTT on PE10, keypad on PA6 / PA7 (9.3). Mic and GPS
share one supply switched by PA9 high; it can stay on whenever the radio runs. VOX / mic
level on PA3 only.

---

## 7. Display

### 7.1 Panel and bus

128 x 64 monochrome, ST7567, on SPI2 shared with the SPI flash: SCK PB13, MOSI PB15, CS PD14, RS
PD13 (low = command, high = data), reset PD12. The flash and the LCD must not be accessed at the same time.

### 7.2 Init, orientation, contrast

Reset low 1 ms, then high, wait 5 ms. Commands: 0xE2 (reset), 0x2F (booster, regulator, follower on), 0x81 0x12
(electronic volume = contrast 18), 0xA2 (bias 1/9), 0xA1 (SEG reverse), 0xC0 (COM normal), 0xA4 (normal, not all
pixels on) or 0xA7 (inverse), 0xAF (display on). Writes per page: 0xB0 | page, 0x10, 0x04 (column 4 offset),
then 128 data bytes. Contrast 0x81 + value; the usable range is about 5-30. Sleep: 0xAE then 0xA5. 0xA6 (non-inverse video) is the reset state (?: RadioFoundry should send it).

### 7.3 Backlight

PC6, TIM3 CH1, period 3000. On V1 the duty is proportional (percent x 20). **On V2 and later the LEDs are off below
about 16 % duty (480) and fully on by 19.3 % (580)**, so 1-99 % maps into 480-580; V1 has
different backlight hardware (a V2 whose display can't be dimmed is probably V1, unverified). The keys
share the backlight (?).

---

## 8. LEDs

No LED GPIOs are known on the MD-9600. Whether the front panel's TX / RX indicator is
hardware-driven or absent is unknown (?).

---

## 9. Keys, encoder, mic keypad, PTT

### 9.1 Front panel matrix

Rows K0 PE0, K1 PE1, K2 PD0, K3 PD1 (open drain, idle high, driven low one at a time); columns K6 PD4, K5 PD3, K4 PD2
(pull-up inputs, low = pressed):

| Row | K6 (PD4) | K5 (PD3) | K4 (PD2) |
|---|---|---|---|
| K0 (PE0) | Power | Alarm (orange) | P1 |
| K1 (PE1) | P2 | P3 | P4 |
| K2 (PD0) | Back (red) | Down | Enter (green) |
| K3 (PD1) | Up | - | - |

### 9.2 Channel encoder

Quadrature on PB10 (A, interrupt on both edges) and PB11 (B); one step per A edge, direction = (A != B).
No pull-ups configured (?: external).

### 9.3 Hand-mic keypad

20 positions as two analog levels: row = 4 - (PA6 raw / 800), column = PA7 raw / 1000 (12-bit raw); row < 5 and
column < 4 means a key is down:

| Row | Col 0 | Col 1 | Col 2 | Col 3 |
|---|---|---|---|---|
| 0 | 1 | 2 | 3 | A |
| 1 | 4 | 5 | 6 | B |
| 2 | 7 | 8 | 9 | C |
| 3 | * | 0 | # | D |
| 4 | (unused) | A/B | Up | Down |

The mic has 16 keys plus its own up / down / A/B (unverified). Thresholds and the mic's resistor ladder are unverified (?).

### 9.4 PTT

Hand-mic PTT on PE10, pull-up, active low. No separate accessory PTT input is known (?).

### 9.5 Remote head (not a TYT part)

A community-built remote front panel exists: if PD2 (column K4) reads low at boot, PB10 / PB11
become USART3 at 250000 baud and keys, encoder, mic keys, PTT and GPS characters arrive as 3-byte messages, while
display rows go out over the link (unverified). RadioFoundry doesn't support it and must not take a P1, P4
or Enter key held at boot for the panel (whether those keys pull PD2 low is unverified).

---

## 10. Power

### 10.1 Supply and main switch

13.8 V mobile supply. The MCU is powered whenever DC is applied; there is no latch the MCU releases to turn itself
off. Power_Control (PD15) switches the RF and audio sections. No ignition-sense input is known (?).

### 10.2 "Off" is standby

Power key = matrix K0 x K6. The radio can be turned "off" by saving settings, writing a standby flag to backup SRAM word 0, and resetting; at boot
the flag is found, the clock drops to HSE / 512, row K0 is driven low and K6 is awaited (power key), then the radio
runs again after a release and a second reset. Settings that need not survive a supply cut also live in backup SRAM.
Removing DC is the only true off. The flag values are RadioFoundry's choice.

### 10.3 Supply voltage ADC and scan

ADC1 scan order (rank 1-9): IN6 mic row, IN7 mic column, IN1 supply, IN3 VOX, IN0 VHF RSSI, IN8 UHF RSSI, IN15 VHF
noise, IN9 UHF noise, temperature sensor; 480 cycles each, PCLK2 / 8. Supply: tenths of a volt = raw / 21.3
; maximum 15.0 V; cutoff hysteresis 8.0 / 10.0 V.

---

## 11. Calibration

### 11.1 Location

512 bytes in the SPI flash security registers, read as one 512-byte security read from register offset 0 
(the same mechanism as the MD-380, [hardware-md380.md](hardware-md380.md) 11.1). A copy can also be kept
at main-array address 0x010000, recognized by the first 8 bytes of the UHF frequency list (00 25 00 40 00 45 01 40,
400.025 / 400.145 MHz). RadioFoundry reads the security registers and never writes them. The vendor CPS backs the table up.

### 11.2 Layout (dual band: UHF 9 points, VHF 5 points)

| Offset | Content |
| --- | --- |
| 0x00 / 0x01 | VOX level 1 / 10 |
| 0x02 / 0x03 | RX low / high voltage (use unknown) |
| 0x04 / 0x05 | RSSI at -120 / -70 dBm |
| 0x09 | UHF reference (MOD2 offset) tune, offset by 128 |
| 0x0C | VHF reference tune, offset by 128 |
| 0x10 / 0x19 | High power (40 W) UHF x9 / VHF x5 |
| 0x20 / 0x29 | Low power (5 W) UHF / VHF |
| 0x30 / 0x39 | RX front-end tuning UHF / VHF |
| 0x40, 0x50, 0x60, 0x70 | UHF squelch level 9 open / close, level 1 open / close (x9) |
| 0x90 / 0x9B, 0xA0 / 0xAB, 0xB0 / 0xBB | CTCSS deviation 67 / 151.4 / 254.1 Hz, UHF x9 / VHF x5 |
| 0xD0 / 0xDB | DCS deviation UHF / VHF |
| 0xE0-0xF3 | VHF squelch 9 open, 9 close, 1 open, 1 close (x5 each) |
| 0x100-0x127 | VHF calibration frequencies, 10 x 4 bytes BCD (5 RX / TX pairs) |
| 0x130 / 0x139, 0x140 / 0x149 | DMR I / Q gain UHF / VHF |
| 0x170 / 0x179, 0x180 / 0x189 | FM I / Q gain UHF / VHF |
| 0x190 / 0x199 | Mid power (25 W) UHF / VHF |
| 0x1A0 / 0x1A9 | Mid-low power (10 W) UHF / VHF |
| 0x1B0-0x1F7 | UHF calibration frequencies, 18 x 4 bytes BCD (9 pairs) |

Everything else is unknown. DAC values are byte x 16.

### 11.3 Interpolation

UHF points every 10 MHz from 400 MHz (index 0-8); VHF every 9.5 MHz from 136 MHz (0-4); linear between neighbors,
extrapolated with the last slope above the top point, clamped to 0-4095.

---

## 12. SPI flash

### 12.1 Device and bus

W25Q128 class, 16 MB, JEDEC 0x4018, on SPI2 (mode 0, PCLK1 / 4 = 4.5 MHz in the reference
port) with CS PB12, shared with the LCD (7.1). Security registers hold the calibration (11.1).

### 12.2 Map

The codeplug / "EEPROM" image can sit at 0 and the calibration copy at 0x010000. The vendor codeplug
and user database (CSV builds) also live in this flash and the vendor firmware needs them restored
to run again, so a full backup before the first RadioFoundry flash is mandatory. RadioFoundry's map: TODO for the
clean side (reuse the DM-1701 map, keeping 0x010000-0x010FFF free if the calibration copy is wanted).

---

## 13. GPS

### 13.1 Module

Optional, JS-M710 (unverified); the "MD-9600-5P GPS" FCC class II change. NMEA on USART1 RX (PA10),
9600 baud, 8N1, DMA. Bursts of invalid ('V') frames are reported from time to time (unverified).

### 13.2 Power and detection

PA9 high powers the GPS and the microphone together, so it stays on. No presence pin: detect by listening
for valid NMEA, as on the MD-380 (ours).

---

## 14. Bootloader and images

### 14.1 Entering the loader

Disconnect DC (the power key is not enough), hold **P1 and the orange (alarm) key** ( notes) while reconnecting; the backlight / display flashes, the
screen stays blank. Another report gives **the red key and P1** instead (unverified); the second key is open (?).

### 14.2 Protocol and encryption

USB DFU 0483:DF11 with TYT's commands, the DM-1701's protocol ([hardware-dm1701.md](hardware-dm1701.md) 15.2). The image is XORed with the MD-9600's 1024-byte key, index from the start of the application image; key SHA-256
c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae, first bytes A2 FA BB 4B
([hardware-dm1701.md](hardware-dm1701.md) 15.3). The key can be derived from a vendor file (as for the DM-1701 and MD-380): it works on every V06.09 and V16.45 file (measured).

### 14.3 Stock firmware files

TYT names the files by feature: P = CSV + GPS, V = CSV, S = recorder + GPS, D = recorder (unverified). TYT's V4
firmware and CPS (for the "new updated PLL", V4 and later boards) are not compatible with V1-V3 radios (unverified).
TYT's download page (checked 2026-10-12) offers only CPS installers for the MD-9600, no firmware.

Files examined):

| Version | Boards | Files | Container |
|---|---|---|---|
| 06.09 | V1-V3 (SKY72310) | `MD9600-CSV-V06.09.bin`, `-CSV-GPS-P06.09`, `-REC-D06.09`, `-GPS-REC-S06.09` | model string "MD-9600" at 0x10 |
| 16.45 | V4 ("PLL1590") | `MD9600-CSV(V4)-V16.45.bin`, `-CSV-GPS(V4)-P16.45`, `-REC(V4)-D16.45`, `-GPS-REC(V4)-S16.45` | model string blank |
| 26.45 | V5 ("2571") | `MD9600-CSV(2571V5)-V26.45.bin` | not on hand |

All eight: two segments, (0x00060000, 0x59800) for the external flash resources and (0x0800C000, 0xB1800..0xB6800)
for the application, payload at 0x100, 0x100 trailing bytes. The leading digit of the version number is the board
family (0 = V1-V3, 1 = V4, 2 = V5) (F: inferred from the three known files). The V06.09 images report SP 0x20001BA0,
the V16.45 ones 0x20001BB0.

---

## 15. USB

OTG_FS on PA11 / PA12, mini-USB on the rear, straight to the MCU (no UART bridge). The bootloader's DFU device is
separate (14.2).

---

## 16. Board revisions

| PCB label | Synthesizer | IF | Other differences |
| --- | --- | --- | --- |
| V1 (first boards, some labeled V2) | SKY72310 x2 | AK2365A x2 | backlight not dimmable the V2 way (7.3) |
| V2, V3 | SKY72310 x2 | AK2365A x2 |  |
| V4 | "PLL1590" x2 (part ?) | AK2365A x2 | VHF synthesizer must be set at power-on |
| V4A, V5 | LMX2571 x2 | none programmable; PE11-PE14 switches | RSSI curve differs |

Vendor firmware follows the same split: V06.09 for V1-V3, V16.45 for V4, V26.45 for V5. TYT
sometimes changed chips without changing the label; a wrong build boots but neither receives nor transmits, and does
no harm (unverified). GPS models have been reported working with the V5 build. Radios needing
TYT's V4 firmware are V4 or later.

**Detection:** no ID pin or register is known. A run-time probe is possible because the families differ in the bus
they need, but every candidate needs a radio: for example, program the V1/V2 SKY72310 words and watch RSSI / noise on
a known signal, then the V5 words. The safer first design is one image with the revision as a setting chosen at first
boot (or in the bring-up menu), defaulting to V5, with TX blocked until RX works (?: our proposal).

**Target first:** V5 / V4A. It is the current production board (the codec comes from the "2571V5" file), GPS
models report V5, and it has the simplest IF path. V1/V2 come next (SKY72310: the driver exists from the MD-380). V4
last (unidentified synthesizer, needs its power-on quirk).

---

## 17. Unknowns that need a radio

| # | Unknown | How to settle it |
|---|---|---|
| 1 | Which revision the test radio is | Open the top cover, read the PCB label; then try the V5 image's RX |
| 2 | V4 synthesizer exact part ("PLL1590"), V5 IF part, V5 use of PE15 | Photos of the board (and FCC internal photos) |
| 3 | Lock detect pin (none mapped) | Scope / board photos; otherwise lock is inferred from RX |
| 4 | LMX2571 delays after reset and calibration | Scope the PLL on retune; measure the relock time (also [gotchas.md](gotchas.md) 14.6, SFR / DMR burst switching) |
| 5 | RF TX interrupt usefulness and the 20 ms burst-off workaround | Scope T5 against the burst on an SDR |
| 6 | Power level labels (5/10/25/40 W) and the low-level fractions | Power meter into a dummy load, both bands, edges and center |
| 7 | RSSI / noise curves per revision | Signal generator steps |
| 8 | Mic keypad thresholds | Press every mic key, log PA6 / PA7 raw |
| 9 | Front panel LEDs, key backlight | Look at the radio |
| 10 | Loader key combination (orange vs red + P1) | Try both |
| 11 | Ignition sense | Schematic / board photos |
| 12 | I2C1 devices | Bus scan |
| 13 | Calibration unknown bytes, BCD frequency format | Dump the security registers and compare with the vendor CPS test screen |
| 14 | 0xA6 needed at display init | First boot |

## 18. Sources

- TYT downloads page (https://www.tyt888.com/download.html): CPS releases and a product photo, no firmware.
- FCC ID POD-DMR3, https://fccid.io/POD-DMR3
- Skyworks SKY72310-11 data sheet; STM32F405 data sheet and RM0090.
- Retailer page https://www.passion-radio.com/mobile/md9600-505.html (unverified claims).
- [gotchas.md](gotchas.md) 13.5, 14.6.
