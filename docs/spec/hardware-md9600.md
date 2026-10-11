<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# TYT MD-9600 / Retevis RT90 hardware (mobile)

The TYT MD-9600 and the Retevis RT90 are one dual-band mobile radio: an STM32F405VGT6 MCU, an HR_C6000 DMR baseband
and codec chip, **two complete RF chains (one VHF, one UHF)**, each with its own synthesizer, its own RX and TX VCOs,
its own IF receiver chip and its own PA supply switch, a 49.95 MHz first IF, a 128 x 64 monochrome LCD (ST7567) on
SPI, a 4 x 3 front panel key matrix, a quadrature channel encoder, a 16-key DTMF hand microphone on an RJ-45 jack read
as two analog levels, a GPS option, and USB full speed on the MCU's own OTG_FS core (mini-USB on the rear). There is
no AT1846S and no RF transceiver chip at all; the HR_C6000 sees a 450 kHz IF from the IF receiver chip.

TYT changed the synthesizer and IF parts during production. The board revision decides how the synthesizers (and on
V5 the IF) are programmed, and **nothing in the firmware can read the revision**: the aftermarket firmware tree builds one image per
revision (section 16).

Status (2026-10-12): **nothing in this file has been checked on a radio.** No MD-9600 is in hand. The facts come from
the reference port's source and notes (the aftermarket firmware tree, facts only), public hardware pages and forum
posts, and data sheets. Section 17 lists every open question.

Conventions: register values are hex; HR_C6000 registers are written `page:reg = value` as in
[modem-hrc6000.md](modem-hrc6000.md). Frequencies in the aftermarket firmware tree are in 10 Hz units; this file gives Hz or MHz.
Every fact carries a confidence tag as in [radios.md](radios.md):

| Tag | Meaning |
|---|---|
| **C** | Confirmed: manufacturer data, a datasheet, or our own bench work on the radio |
| **D** | Documented by a reverse-engineering project (schematic notes, pin tables, hardware pages, the reference port's pin map and register writes) |
| **F** | Forum post, review or retailer claim, or a value carried over from a sibling radio's notes |
| **?** | Not found, contradictory, or our own assumption. Needs a radio |

Short source names are listed in section 18 ([ref], [OpenRTX], [ogd77-t83], [ogd77-p15], [miklor], [miklor-fw],
[TYT], [FCC], [SKY], [ST], [gotchas]). The reference port's pin map comes from a CubeMX project, so (D) for pins means
"the reference port drives the pin this way and the radio works on air for its users", not a schematic.

---

## 1. MCU, clocks, memory

### 1.1 Part

STM32F405VGT6, LQFP100, 1 MB flash, 128 KB SRAM + 64 KB CCM, 4 KB backup SRAM (D) [OpenRTX], [ref]. The backup SRAM
is used (section 10.2). Read-out protection level 1 assumed, as on every TYT STM32 radio (?).

### 1.2 Clocks

| Item | Value | Tag |
|---|---|---|
| HSE | 8 MHz external clock in **bypass** mode, as the DM-1701 | (D) [ref] |
| LSE | 32.768 kHz crystal, RTC enabled | (D) [ref] |
| PLL | M 4, N 72, P 2, Q 3: SYSCLK 72 MHz, USB 48 MHz | (D) [ref] |
| AHB / APB1 / APB2 | 72 MHz / **18 MHz (/4)** / **36 MHz (/2)** | (D) [ref]; differs from the DM-1701 (/2, /1). ODR may use the DM-1701 plan (?: nothing found that needs the slower buses) |

Standby (section 10.2) drops SYSCLK to HSE / 512 with both APBs / 16 and voltage scale 2 while waiting for the power
key (D) [ref].

### 1.3 Internal flash layout

Same TYT loader and layout as the DM-1701 and MD-380: loader in sectors 0-2 (0x08000000-0x0800BFFF, never written),
application from 0x0800C000, VTOR 0xC000 (D) [ref]. The stock codec sits at a different address in every stock version and variant
(0x08074B18 to 0x08079630 for the code; 0x0807537C in V26.45; [codec-md9600.md](codec-md9600.md)); the aftermarket firmware tree linked
its application below 0x0807537C and the V26.45 codec there.

### 1.4 RAM

The reference port does not map CCM; ODR will (as on the DM-1701). Backup SRAM: 4 KB at BKPSRAM_BASE, kept while DC is
applied, used for the standby flag (10.2) and for non-permanent settings (D) [ref].

---

## 2. Pin assignments

### 2.1 Port A

| Pin | Function | Notes |
|---|---|---|
| PA0 | ADC1 IN0: VHF RSSI | (D) |
| PA1 | ADC1 IN1: supply voltage | (D), 10.3 |
| PA2 | ADC2 IN2: QT_DQT_IN, sub-audio tone input | (D); the reference port doesn't decode tones here (the HR_C6000 does) |
| PA3 | ADC1 IN3: VOX / mic level | (D); the only mic level input (no separate "mic" signal) |
| PA4 | DAC1: TV_U, UHF receiver front-end tuning | (D) |
| PA5 | DAC2: APC_TV_V, **shared** TX power (APC) reference and VHF receiver front-end tuning | (D) |
| PA6 | ADC1 IN6: hand-mic keypad, row level ("P2") | (D), 9.3 |
| PA7 | ADC1 IN7: hand-mic keypad, column level ("P1") | (D), 9.3 |
| PA8 | C5_U_SW, UHF 5 V supply (receiver side) | (D); high = on |
| PA9 | USART1 TX **and** GPS + microphone power switch | (D); driven as GPIO high = mic and GPS powered (13.2) |
| PA10 | USART1 RX: GPS NMEA in, 9600 baud | (D) |
| PA11 / PA12 | USB OTG_FS D- / D+ | (D) |
| PA13 | R5_U_SW, UHF receiver 5 V; **also SWDIO** | (D) |
| PA14 | R5_V_SW, VHF receiver 5 V; **also SWCLK** | (D) |
| PA15 | I2S3 WS (AF6) | (D) |

### 2.2 Port B

| Pin | Function | Notes |
|---|---|---|
| PB0 | ADC1 IN8: UHF RSSI | (D) |
| PB1 | ADC1 IN9: UHF noise (squelch) level | (D) |
| PB2 | FM_SW, FM audio stages on (high) | (D) |
| PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI (AF5): HR_C6000 voice port | (D) |
| PB6 | AF_SW, audio amplifier mute, TIM4 CH1 PWM (AF2) | (D); 9.x / 6.1 |
| PB7 | U_V_MOD_SW: HR_C6000 MOD2 output to the VHF (high) or UHF (low) VCO | (D) |
| PB8 / PB9 | I2C1 SCL / SDA (AF4, open drain) | (D); nothing on the bus is used by the reference port (?) |
| PB10 / PB11 | Channel encoder A (EXTI, both edges) / B; USART3 TX / RX when the optional remote head is fitted | (D), 9.2, 9.5 |
| PB12 | SPI flash chip select | (D) |
| PB13 / PB14 / PB15 | SPI2 SCK / MISO / MOSI (AF5): SPI flash **and** LCD | (D); mode 0, 4.5 MHz (12.1) |

### 2.3 Port C

| Pin | Function | Notes |
|---|---|---|
| PC0 / PC1 / PC2 | HR_C6000 timeslot / system / RF TX interrupts (EXTI0-2, falling edge) | (D); see 4.4 for the RF TX line |
| PC3 | ADC2 IN13: RX_2T_5T (discriminator audio) | (D); unused |
| PC4 | RF_APC_SW, PA power loop on | (D) |
| PC5 | ADC1 IN15: VHF noise level | (D) |
| PC6 | Display_Light: LCD and key backlight, TIM3 CH1 PWM (AF2) | (D), 7.3 |
| PC7 | CTC_DCS_PWM, sub-audio tone output | (D); open drain, held high; the reference port generates tones in the HR_C6000 instead |
| PC8 | BEEP_PWM | (D); unused by the reference port (beeps go through the HR_C6000) |
| PC9 | C5_V_SW, VHF 5 V supply (receiver side) | (D) |
| PC10 / PC11 / PC12 | I2S3 CK (AF6) / I2S3ext SD (AF5) / I2S3 SD (AF6) | (D) |
| PC13 | U_V_AF_SW: receive audio from the VHF IF chip (high) or UHF (low) | (D) |
| PC14 / PC15 | LSE crystal | (D) |

### 2.4 Port D

| Pin | Function | Notes |
|---|---|---|
| PD0 / PD1 | Key matrix rows K2 / K3 (open drain) | (D) |
| PD2 / PD3 / PD4 | Key matrix columns K4 / K5 / K6 (inputs, pull-up) | (D); PD2 low at boot also means "remote head fitted" (9.5) |
| PD5 / PD6 | T5_V_SW / T5_U_SW: VHF / UHF transmitter 5 V | (D) |
| PD7 | HR_C6000 voice port chip select | (D) |
| PD8 / PD9 | PLL_CS_V / PLL_CS_U: VHF / UHF synthesizer chip selects | (D) |
| PD10 / PD11 | PLL_DATA / PLL_CLK, shared by both synthesizers | (D) |
| PD12 / PD13 / PD14 | LCD reset / RS (data-command) / chip select | (D) |
| PD15 | Power_Control: main power to the RF and audio sections (high = on) | (D), 10.1 |

### 2.5 Port E

| Pin | Function | Notes |
|---|---|---|
| PE0 / PE1 | Key matrix rows K0 / K1 (open drain) | (D) |
| PE2 / PE3 / PE4 / PE5 | HR_C6000 configuration port CS / CLK / MISO / MOSI (bit-banged) | (D), as the DM-1701 |
| PE6 | C6000_PWD, HR_C6000 power down | (D) |
| PE7 | FM_MUTE: FM audio to the volume control, high = unmuted | (D) |
| PE8 / PE9 | VCO_VCC_U_SW / VCO_VCC_V_SW: per band, high = RX VCO, low = TX VCO | (D) |
| PE10 | PTT (hand mic), input with pull-up, active low | (D) |
| PE11 / PE12 | V1-V4: IF_CS_V / IF_CS_U (IF chip selects). **V5: VHF / UHF IF bandwidth, high = 25 kHz** | (D), 5.4 |
| PE13 / PE14 | V1-V4: IF_DATA / IF_CLK. **V5: connect the VHF / UHF IF to the HR_C6000** (one high, the other low) | (D), 5.4 |
| PE15 | V1-V4: IF_RST (IF chip reset, active low) | (D); V5 use unknown (?) |

### 2.6 Reset-time levels and SWD

The reference port's reset levels: HR_C6000 CS/CLK/MOSI and power-down low, IF chip selects/data/clock high, IF reset
low, both synthesizer chip selects and the voice port CS high, synthesizer data/clock low, LCD lines low, Power_Control
low, all T5/R5/C5 supply switches low, U_V_MOD_SW low, FM_SW and FM_MUTE low, CTC_DCS_PWM high (D) [ref]. Then
`radioPowerOn` sets both T5 low, all C5 and R5 high (both receivers are always powered together, "as the TYT firmware
does"), then Power_Control high (D) [ref].

PA13 / PA14 are the receiver supplies of both bands: like the MD-380, the radio takes the SWD pins at run time, and a
debugger has to connect under reset (D for the pins; the consequence ours).

---

## 3. Interrupts, DMA, timers

| Source | Use | Tag |
|---|---|---|
| EXTI0/1/2 (PC0-PC2) | HR_C6000 timeslot / system / RF TX, priority 5 | (D) |
| EXTI15_10 (PB10) | Channel encoder A, both edges, priority 5 | (D) |
| DMA1 Stream0 ch0 / Stream5 ch2 | I2S3 RX / I2S3ext TX, circular | (D), as the DM-1701 |
| DMA2 Stream2 ch4 | USART1 RX (GPS), circular | (D) |
| DMA1 Stream1 / Stream3 ch4 | USART3 RX / TX (remote head only) | (D) |
| TIM3 CH1 | Backlight PWM, period 3000 counts (prescaler 0) | (D) |
| TIM4 CH1 | Amplifier mute PWM on AF_SW, prescaler 16, period 100 | (D) |
| TIM6 | HAL time base | (D) |
| ADC1 | 9-channel scan (10.3), 480 cycles each, interrupt per conversion | (D) |
| ADC2 | IN2 (QT_DQT_IN), single, 3 cycles | (D); not used for decoding (?) |

---

## 4. HR_C6000

### 4.1 Hookup

Configuration port bit-banged on PE2-PE5, voice port on SPI1 (PB3-PB5, CS PD7, CPOL high, CPHA first edge,
PCLK2 / 64 = about 560 kHz in the reference port), I2S3 slave full duplex Philips 16-bit (extended) 8 kHz, power down PE6, interrupts
PC0-PC2: the DM-1701's wiring except the voice port, which is on SPI1 here instead of the DM-1701's SPI (D) [ref]. The
register behavior is the DM-1701's ([modem-hrc6000.md](modem-hrc6000.md)).

### 4.2 Differences from the DM-1701 set-up

| Item | MD-9600 | Tag |
|---|---|---|
| Clock / PLL | 4:0x0A=0x80, 0x0B=0x28, 0x0C=0x33, 0x0A=0x00, 0xB9=0x05, 0xBA=0x04, 0xBB=0x02: the same values ODR already uses | (D) [ref] |
| IF | 450 kHz (4:0x07-0x09 = 0x0B 0xB8 0x00), from the IF receiver chip, two-point modulation (4:0x01=0xB0) | (D) |
| FM receive audio | **not through the HR_C6000**: 4:0x36 = 0x00 always. FM audio comes from the IF chip's demodulator via U_V_AF_SW, FM_SW, FM_MUTE (6.1) | (D) [ref] |
| FM TX, CTCSS, DCS, DTMF, tones | through the HR_C6000 (TX tone generator, RX CTCSS / DCS detector); the stock firmware is reported to use PWM on PC7 for TX tones instead | (D) [ref] |
| MOD2 routing | U_V_MOD_SW (PB7) sends the MOD2 output to the active band's VCO; set with every synthesizer write | (D) |
| Reference / MOD2 offset | per band byte t from calibration (11.2); signed c = t - 128; 4:0x47 = c, 4:0x48 = 0x03 if c < 0 else 0x00, 4:0x04 = c. Rewritten at every RX entry (the reference port reports an RX offset after TX otherwise) | (D) [ref] |
| DMR sync detection | 4:0x5F = 0xF0 (MS, BS, TDMA1, TDMA2) | (D) |

### 4.3 Voice port and I2S

As the DM-1701 (4.3 / 4.4 of [hardware-dm1701.md](hardware-dm1701.md)) apart from the SPI instance. DMR receive AGC
in the reference port maps a peak level to the HR_C6000 DAC gain; not a hardware fact.

### 4.4 Burst timing

The reference port says the MD-9600 "doesn't have the hardware" for the receive-side interrupt the other STM32 radios
use to end a TX burst, and ends the burst from the timeslot interrupt once the PA has been on 20 ms (D) [ref]. Whether
the RF TX interrupt (PC2) works as on the DM-1701 is not stated (?). [gotchas] 14.6: every TX/RX switch means a new
synthesizer word and a different VCO (5.2), so the per-burst relock time is the main DMR timing risk.

---

## 5. RF

### 5.1 Bands, power

Dual band, 136-174 MHz and 400-480 MHz (D) [OpenRTX]; the reference port allows 127-178 and 380-564 MHz outside its
data-sheet mode (not a recommendation). Rated 50 W VHF / 45 W UHF (40 W on some retailer pages); miklor measured 44 W
at 146 MHz and 40 W at 446 MHz (F) [miklor]. Band selection is by frequency: below 300 MHz is VHF (D) [ref].

### 5.2 Frequency plan (all revisions)

| Item | Value | Tag |
|---|---|---|
| Reference | 16.8 MHz TCXO, shared | (D) [ref], [OpenRTX] |
| First IF | 49.95 MHz | (D) [ref] |
| VHF RX | LO = f + 49.95 MHz (high side) | (D) |
| UHF RX | LO = f - 49.95 MHz (low side) | (D) |
| TX | VCO on the carrier (UHF), x2 on V1/V2 VHF (5.3) | (D) |
| RX vs TX VCO | separate VCOs per band, picked by VCO_VCC_x_SW (high = RX) | (D) |

One synthesizer per band drives both of that band's VCOs, so RX and TX (49.95 MHz apart) always mean reprogramming.

### 5.3 Synthesizer per revision

All write-only, chip select low during the word, data MSB first, data set while the clock is low and latched on the
rising edge (D) [ref]. Fractional rule in V1/V2/V4: if the fraction is above one half, N is rounded up and the fraction
used as a negative offset (signed modulator) (D).

**V1, V2 (and V3): Skyworks SKY72310, one per band** (D) [ref], [OpenRTX]. 16-bit words, 4-bit address in the top
bits (as [hardware-md380.md](hardware-md380.md) 5.2, data sheet [SKY]):

| Band | R (reg 5 = R - 1) | Comparison frequency | VCO |
|---|---|---|---|
| VHF | 6 | 2.8 MHz | 2 x LO, the VHF output divides by 2 |
| UHF | 4 | 4.2 MHz | LO |

N = VCO / f_pd; reg 0 = N - 32; 18-bit fraction (x 2^18), reg 1 = fraction >> 8, reg 2 = low 8 bits. Fixed: reg 6 =
0x01F, reg 7 = 0x3D0, reg 8 = 0, reg 9 = 0. Write order: 8, 9, 5, 0, 2, 1, 6, 7 ("the order the TYT firmware uses")
(D) [ref].

**V4: "PLL1590" synthesizer, one per band** (D) [ref]. TYT's own folder for the V4 firmware (V16.45) is named "firmware for
PLL1590" (C) [TYT-fw], which confirms the part family the reference port calls AK1590; the exact part number is still
unconfirmed (?). The V16.45 stock images (all four variants) hold the 24-bit words 0x1FF404 and 0x19F404 as literals,
that is reg 4 = 0x1FF40 (VHF) and 0x19F40 (UHF) in the format below; the V06.09 images hold neither (C, our own
reading of the decrypted images). 24-bit
words: 20 data bits then the 4-bit address in the low nibble. N in reg 2 (VHF 0x20000 | N, UHF 0x38000 | N), the
18-bit fraction in reg 1, reg 3 = 0x6000 | R, reg 4 = 0x1FF40 (VHF) or 0x19F40 (UHF), reg 5 = reg 6 = 0. R: VHF 12,
UHF RX 8, UHF TX 4. **No x2 on VHF** here (VCO on the LO). Write order 5, 6, 3, 4, 1, 2. The V4 VHF synthesizer must
be programmed (145 MHz RX) right after power-on or it never locks (D) [ref].

**V5 (and V4A): TI LMX2571, one per band** (F: "PLL 2571" in the stock file name and a retailer page [passion-radio];
the register set the reference port writes is the LMX2571's). 24-bit words: 8-bit address then 16 data bits. R: VHF
8, UHF 4; 24-bit fraction; reg 0x04 = 0x3000 + (N & 0xFF), reg 0x01 = fraction >> 16, reg 0x02 = fraction low 16,
reg 0x03 = 0, reg 0x05 = 0x100 + R, reg 0x06 = 0x8581, reg 0x07 = 0x1024, reg 0x08 = 0x0430, reg 0x29 = 0x0230,
reg 0x2A = 0x0108, reg 0x00 = 0x003E. Sequence: reset (0x00 = 0x2000), 0x00, 0x04, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07,
0x08, 0x29, 0x2A, then 0x00 = 0x003F (start calibration) and 0x00 = 0x003E. The clock is left low after each word.
The reference port notes that delays after reset and calibration "may be needed" (?) (D) [ref].

No lock detect input is wired in the reference port's pin map for any revision (?).

### 5.4 IF receiver per revision

**V1-V4: AKM AK2365A, one per band** (D) [ref]. Bit-banged 16-bit words on IF_DATA (PE13) / IF_CLK (PE14) with chip
selects PE11 (VHF) / PE12 (UHF), reset PE15 (pulse low for at least 1 us). Data set while the clock is low, latched on
the rising edge. Written each time the receiver is turned on: soft reset 0x08AA, then 0x02F1 (25 kHz) or 0x02E9
(12.5 kHz), 0x041D (AGC), 0x0600 (IF gain 6 dB), 0x1601, 0x1880 (auto AGC) (D) [ref]. The chip's 450 kHz output feeds
the HR_C6000, its demodulated audio the FM path (6.1).

**V5: no programmable IF chip.** PE11 / PE12 set the VHF / UHF IF filter (high = 25 kHz), PE13 / PE14 connect the VHF
or UHF IF to the HR_C6000 (D) [ref]. The V5 IF part is not identified (?).

### 5.5 Receive and transmit switching

RX: Power_Control on, both R5 and both C5 on, T5 off, RF_APC_SW off; synthesizer to the LO with VCO_VCC high for the
band; DAC1 (UHF) or DAC2 (VHF) = the calibrated front-end tuning voltage; IF set up for the band and bandwidth;
U_V_AF_SW to the band (D) [ref].

TX: both R5 off, synthesizer to the carrier with VCO_VCC low, DAC2 = APC drive (for both bands: PA5 serves the APC in
TX), then the band's T5 on and the other off, then RF_APC_SW on. Per DMR burst only T5 and RF_APC_SW toggle; return to
RX drops RF_APC_SW, both T5, then reprograms for RX (D) [ref]. The ordering beyond that is the reference port's; no
lock check (?).

### 5.6 Power levels and control

DAC2 (12 bit) is the APC reference. Four calibrated points per frequency (11.2) that the reference port treats as 5 W,
10 W, 25 W and 40 W (F: labels, not measured); lower levels as a fixed fraction of the 5 W value (VHF 0.82-0.90, UHF
0.41-0.64 for 100 mW..1 W) (D) [ref]. The forum reports power poorly calibrated on some radios, and some older units
only working well on DMR at full power (F) [ref notes]. ODR: refuse TX without an accepted table, as on the MD-380.

### 5.7 RSSI, noise, squelch

Analog per band: VHF RSSI PA0, UHF RSSI PB0, VHF noise PC5, UHF noise PB1. The reference port's dBm mapping: V1/V2/V4
-135 dBm (VHF) / -145 dBm (UHF) + raw8 / 2; V5 -155 dBm + raw8 / 1.95 (raw8 = the 12-bit average >> 4) (D) [ref, a
fit, not a measurement (?)]. Calibration also has RSSI points at -120 and -70 dBm and squelch open / close rows (11.2).

---

## 6. Audio

### 6.1 Paths

FM: IF chip discriminator -> U_V_AF_SW (band) -> FM_SW (stages on) -> FM_MUTE (high = to the volume control) -> analog
volume knob -> amplifier. DMR and beeps: HR_C6000 line out -> volume control. The amplifier mute is AF_SW (PB6) driven
as PWM from TIM4: 0 % = unmuted, 100 % = muted; the reference port uses a partial duty (30 %) as a click-suppression
option (D) [ref]. The volume knob is analog only (no ADC input): no software volume (D) [ref notes].

### 6.2 Microphone

Hand mic on the RJ-45 jack: mic audio to the HR_C6000 line input, PTT on PE10, keypad on PA6 / PA7 (9.3). Mic and GPS
share one supply switched by PA9 high; the reference port keeps it on whenever the radio runs (D) [ref]. VOX / mic
level on PA3 only.

---

## 7. Display

### 7.1 Panel and bus

128 x 64 monochrome, ST7567 (D) [OpenRTX], [ref], on SPI2 shared with the SPI flash: SCK PB13, MOSI PB15, CS PD14, RS
PD13 (low = command, high = data), reset PD12 (D). The flash and the LCD must not be accessed at the same time.

### 7.2 Init, orientation, contrast

Reset low 1 ms, then high, wait 5 ms (D). Commands: 0xE2 (reset), 0x2F (booster, regulator, follower on), 0x81 0x12
(electronic volume = contrast 18), 0xA2 (bias 1/9), 0xA1 (SEG reverse), 0xC0 (COM normal), 0xA4 (normal, not all
pixels on) or 0xA7 (inverse), 0xAF (display on) (D) [ref]. Writes per page: 0xB0 | page, 0x10, 0x04 (column 4 offset),
then 128 data bytes. Contrast 0x81 + value; the reference port allows 5-30. Sleep: 0xAE then 0xA5 (D). The reference
port never sends 0xA6; it relies on the reset state for non-inverse video (?: ODR should send 0xA6).

### 7.3 Backlight

PC6, TIM3 CH1, period 3000. On V1 the duty is proportional (percent x 20). **On V2 and later the LEDs are off below
about 16 % duty (480) and fully on by 19.3 % (580)**, so the reference port maps 1-99 % into 480-580 (D) [ref]; V1 has
different backlight hardware (D) [ref], [ogd77-t83: "a V2 whose display can't be dimmed is probably V1"]. The keys
share the backlight (?).

---

## 8. LEDs

The reference port drives no LED GPIOs on the MD-9600 (D) [ref]. Whether the front panel's TX / RX indicator is
hardware-driven or absent is unknown (?).

---

## 9. Keys, encoder, mic keypad, PTT

### 9.1 Front panel matrix

Rows K0 PE0, K1 PE1, K2 PD0, K3 PD1 (open drain, idle high, driven low one at a time); columns K6 PD4, K5 PD3, K4 PD2
(pull-up inputs, low = pressed) (D) [ref]:

| Row | K6 (PD4) | K5 (PD3) | K4 (PD2) |
|---|---|---|---|
| K0 (PE0) | Power | Alarm (orange) | P1 |
| K1 (PE1) | P2 | P3 | P4 |
| K2 (PD0) | Back (red) | Down | Enter (green) |
| K3 (PD1) | Up | - | - |

### 9.2 Channel encoder

Quadrature on PB10 (A, interrupt on both edges) and PB11 (B); one step per A edge, direction = (A != B) (D) [ref].
No pull-ups configured (?: external).

### 9.3 Hand-mic keypad

20 positions as two analog levels: row = 4 - (PA6 raw / 800), column = PA7 raw / 1000 (12-bit raw); row < 5 and
column < 4 means a key is down (D) [ref]:

| Row | Col 0 | Col 1 | Col 2 | Col 3 |
|---|---|---|---|---|
| 0 | 1 | 2 | 3 | A |
| 1 | 4 | 5 | 6 | B |
| 2 | 7 | 8 | 9 | C |
| 3 | * | 0 | # | D |
| 4 | (unused) | A/B | Up | Down |

The mic has 16 keys plus its own up / down / A/B (F) [miklor]. Thresholds and the mic's resistor ladder are unverified (?).

### 9.4 PTT

Hand-mic PTT on PE10, pull-up, active low (D). No separate accessory PTT input in the reference port's map (?).

### 9.5 Remote head (not a TYT part)

The reference port supports a community-built remote front panel: if PD2 (column K4) reads low at boot, PB10 / PB11
become USART3 at 250000 baud and keys, encoder, mic keys, PTT and GPS characters arrive as 3-byte messages, while
display rows go out over the link (D) [ref]. Not stock hardware, and ODR doesn't support it. ODR must not take a P1, P4
or Enter key held at boot for the panel (whether those keys pull PD2 low is unverified).

---

## 10. Power

### 10.1 Supply and main switch

13.8 V mobile supply. The MCU is powered whenever DC is applied; there is no latch the MCU releases to turn itself
off. Power_Control (PD15) switches the RF and audio sections (D) [ref]. No ignition-sense input is known (?).

### 10.2 "Off" is standby

Power key = matrix K0 x K6. The reference port turns the radio "off" by saving settings, writing a standby flag (0x2BED)
to backup SRAM word 0, and resetting; at boot it finds the flag, drops to HSE / 512, drives row K0 low and waits for
K6 to go low (power key), waits for release, writes the running flag (0xCAFE) and resets again (D) [ref]. Settings
that need not survive a supply cut also live in backup SRAM. Removing DC is the only true off. The flags are the
reference port's own values, not stock (?: whether the stock firmware's standby state survives into ODR is
irrelevant; ODR picks its own).

### 10.3 Supply voltage ADC and scan

ADC1 scan order (rank 1-9): IN6 mic row, IN7 mic column, IN1 supply, IN3 VOX, IN0 VHF RSSI, IN8 UHF RSSI, IN15 VHF
noise, IN9 UHF noise, temperature sensor; 480 cycles each, PCLK2 / 8 (D) [ref]. Supply: tenths of a volt = raw / 21.3
(D, plus a user trim); maximum 15.0 V; cutoff hysteresis 8.0 / 10.0 V (D) [ref].

---

## 11. Calibration

### 11.1 Location

512 bytes in the SPI flash security registers, read as one 512-byte security read from register offset 0 (D) [ref]
(the same mechanism as the MD-380, [hardware-md380.md](hardware-md380.md) 11.1). The reference port also keeps a copy
at main-array address 0x010000, recognized by the first 8 bytes of the UHF frequency list (00 25 00 40 00 45 01 40,
400.025 / 400.145 MHz). ODR reads the security registers and never writes them. The stock CPS backs the table up
(Ctrl+T test screen) (F) [ref notes].

### 11.2 Layout (dual band: UHF 9 points, VHF 5 points)

| Offset | Content | Tag |
|---|---|---|
| 0x00 / 0x01 | VOX level 1 / 10 | (D) |
| 0x02 / 0x03 | RX low / high voltage (use unknown) | (?) |
| 0x04 / 0x05 | RSSI at -120 / -70 dBm | (D) |
| 0x09 | UHF reference (MOD2 offset) tune, offset by 128 | (D), 4.2 |
| 0x0C | VHF reference tune, offset by 128 | (D) |
| 0x10 / 0x19 | High power (40 W) UHF x9 / VHF x5 | (D) |
| 0x20 / 0x29 | Low power (5 W) UHF / VHF | (D) |
| 0x30 / 0x39 | RX front-end tuning UHF / VHF | (D) |
| 0x40, 0x50, 0x60, 0x70 | UHF squelch level 9 open / close, level 1 open / close (x9) | (D) |
| 0x90 / 0x9B, 0xA0 / 0xAB, 0xB0 / 0xBB | CTCSS deviation 67 / 151.4 / 254.1 Hz, UHF x9 / VHF x5 | (D) |
| 0xD0 / 0xDB | DCS deviation UHF / VHF | (D) |
| 0xE0-0xF3 | VHF squelch 9 open, 9 close, 1 open, 1 close (x5 each) | (D) |
| 0x100-0x127 | VHF calibration frequencies, 10 x 4 bytes BCD (5 RX / TX pairs) | (D) |
| 0x130 / 0x139, 0x140 / 0x149 | DMR I / Q gain UHF / VHF | (D); unused by the reference port |
| 0x170 / 0x179, 0x180 / 0x189 | FM I / Q gain UHF / VHF | (D); unused |
| 0x190 / 0x199 | Mid power (25 W) UHF / VHF | (D) |
| 0x1A0 / 0x1A9 | Mid-low power (10 W) UHF / VHF | (D) |
| 0x1B0-0x1F7 | UHF calibration frequencies, 18 x 4 bytes BCD (9 pairs) | (D) |

Everything else is unknown. DAC values are byte x 16 (D).

### 11.3 Interpolation

UHF points every 10 MHz from 400 MHz (index 0-8); VHF every 9.5 MHz from 136 MHz (0-4); linear between neighbors,
extrapolated with the last slope above the top point, clamped to 0-4095 (D) [ref].

---

## 12. SPI flash

### 12.1 Device and bus

W25Q128 class, 16 MB, JEDEC 0x4018 (D) [ref], [OpenRTX], on SPI2 (mode 0, PCLK1 / 4 = 4.5 MHz in the reference
port) with CS PB12, shared with the LCD (7.1). Security registers hold the calibration (11.1).

### 12.2 Map

The reference port puts its codeplug / "EEPROM" image at 0 and the calibration copy at 0x010000. The stock TYT codeplug
and the stock user database (CSV builds) also live in this flash: the stock firmware needs its flash content restored
to run again, so a full backup before the first ODR flash is mandatory (D) [ref notes]. ODR's map: TODO for the
clean side (reuse the DM-1701 map, keeping 0x010000-0x010FFF free if the calibration copy is wanted).

---

## 13. GPS

### 13.1 Module

Optional, JS-M710 (D) [OpenRTX]; the "MD-9600-5P GPS" FCC class II change (C) [FCC]. NMEA on USART1 RX (PA10),
9600 baud, 8N1, DMA (D) [ref]. The reference port reports bursts of invalid ('V') frames from time to time (F) [ref].

### 13.2 Power and detection

PA9 high powers the GPS and the microphone together, so it stays on (D) [ref]. No presence pin: detect by listening
for valid NMEA, as on the MD-380 (ours).

---

## 14. Bootloader and images

### 14.1 Entering the loader

Disconnect DC (the power key is not enough), hold **P1 and the orange (alarm) key** ([ref] notes) or **the red key and
P1** ([miklor-fw]) while reconnecting; the backlight / display flashes, the screen stays blank (F: the two sources
disagree on the second key (?)).

### 14.2 Protocol and encryption

USB DFU 0483:DF11 with TYT's commands, the DM-1701's protocol ([hardware-dm1701.md](hardware-dm1701.md) 15.2) (D)
[ref]. The image is XORed with the MD-9600's 1024-byte key, index from the start of the application image; key SHA-256
c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae, first bytes A2 FA BB 4B
([hardware-dm1701.md](hardware-dm1701.md) 15.3) (D). Deriving it from a stock file (as done for the DM-1701 and MD-380)
works: on every V06.09 and V16.45 stock file the plain column vote alone gives the key with that hash (C, 2026-10-12;
[codec-md9600.md](codec-md9600.md)).

### 14.3 Stock firmware files

TYT names the files by feature: P = CSV + GPS, V = CSV, S = recorder + GPS, D = recorder (F) [miklor-fw]. TYT's V4
firmware and CPS (for the "new updated PLL", V4 and later boards) are not compatible with V1-V3 radios (F)
[miklor-fw]. TYT's download page (checked 2026-10-12) offers only CPS installers for the MD-9600, no firmware (C) [TYT].

Files examined (C, [TYT-fw]; container header only here, the codec in [codec-md9600.md](codec-md9600.md)):

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

OTG_FS on PA11 / PA12, mini-USB on the rear, straight to the MCU (no UART bridge) (D). The bootloader's DFU device is
separate (14.2).

---

## 16. Board revisions

| PCB label | Synthesizer | IF | Old-tree build | Other differences | Tag |
|---|---|---|---|---|---|
| V1 (first boards, some labeled V2) | SKY72310 x2 | AK2365A x2 | VERSION=1 | backlight not dimmable the V2 way (7.3) | (D), (F) [ogd77-t83] |
| V2, V3 | SKY72310 x2 | AK2365A x2 | VERSION=2 | | (D) |
| V4 | "PLL1590" x2 (C for the name, part ?) | AK2365A x2 | VERSION=4 | VHF synthesizer must be set at power-on | (D) |
| V4A, V5 | LMX2571 x2 (F) | none programmable; PE11-PE14 switches | VERSION=5 | RSSI curve differs | (D) |

Stock firmware follows the same split: V06.09 for V1-V3, V16.45 for V4, V26.45 for V5 (C for the file names). TYT
sometimes changed chips without changing the label; a wrong build boots but neither receives nor transmits, and does
no harm (F) [ogd77-t83], [ogd77-p15]. GPS models have been reported working with the V5 build (F). Radios needing
TYT's V4 firmware are V4 or later (F).

**Detection:** no ID pin or register is known. A run-time probe is possible because the families differ in the bus
they need, but every candidate needs a radio: for example, program the V1/V2 SKY72310 words and watch RSSI / noise on
a known signal, then the V5 words. The safer first design is one image with the revision as a setting chosen at first
boot (or in the bring-up menu), defaulting to V5, with TX blocked until RX works (?: our proposal).

**Target first:** V5 / V4A. It is the current production board (the stock file the codec comes from is "2571V5"), GPS
models report V5, and it has the simplest IF path. V1/V2 come next (SKY72310: the driver exists from the MD-380). V4
last (unidentified synthesizer, needs its power-on quirk).

---

## 17. Unknowns that need a radio

| # | Unknown | How to settle it |
|---|---|---|
| 1 | Which revision the test radio is | Open the top cover, read the PCB label; then try the V5 image's RX |
| 2 | V4 synthesizer exact part ("PLL1590"), V5 IF part, V5 use of PE15 | Photos of the board (and FCC internal photos [FCC]) |
| 3 | Lock detect pin (none mapped) | Scope / board photos; otherwise lock is inferred from RX |
| 4 | LMX2571 delays after reset and calibration | Scope the PLL on retune; measure the relock time (also [gotchas] 14.6, SFR / DMR burst switching) |
| 5 | RF TX interrupt usefulness and the 20 ms burst-off workaround | Scope T5 against the burst on an SDR |
| 6 | Power level labels (5/10/25/40 W) and the low-level fractions | Power meter into a dummy load, both bands, edges and center |
| 7 | RSSI / noise curves per revision | Signal generator steps |
| 8 | Mic keypad thresholds | Press every mic key, log PA6 / PA7 raw |
| 9 | Front panel LEDs, key backlight | Look at the radio |
| 10 | Loader key combination (orange vs red + P1) | Try both |
| 11 | Ignition sense | Schematic / board photos |
| 12 | I2C1 devices | Bus scan |
| 13 | Calibration unknown bytes, BCD frequency format | Dump the security registers and compare with the stock CPS test screen |
| 14 | 0xA6 needed at display init | First boot |

## 18. Sources

| Short name | Source | Kind |
|---|---|---|
| [ref] | The reference port's MD-9600 tree (CubeMX pin map, register writes, README / STATUS notes), outside this repository; facts only | (D) |
| [OpenRTX] | OpenRTX MD-9600 hardware page, https://openrtx.org/#/hardware/md9600 (prose) | (D) |
| [ogd77-t83] | https://www.opengd77.org/viewtopic.php?t=83, hardware versions | Forum (F) |
| [ogd77-p15] | https://www.opengd77.org/viewtopic.php?p=15, installation | Forum (F) |
| [miklor] | https://www.miklor.com/COM/Review_MD9600.php | Review (F) |
| [miklor-fw] | https://www.miklor.com/MD9600/9600-FWupdate.php | Guide (F) |
| [passion-radio] | https://www.passion-radio.com/mobile/md9600-505.html | Retailer (F) |
| [TYT] | https://www.tyt888.com/download.html (2026-10-12: CPS 1.27, 1.33, 1.43 and a product photo; no firmware) | Vendor (C) |
| [TYT-fw] | TYT stock firmware V06.09 and V16.45 ("firmware for PLL1590"), kept outside the repository; container headers and literal constants only | Vendor files (C) |
| [FCC] | FCC ID POD-DMR3, https://fccid.io/POD-DMR3 | (C) |
| [SKY] | Skyworks SKY72310-11 data sheet | (C) |
| [ST] | STM32F405 data sheet, RM0090 | (C) |
| [gotchas] | [gotchas.md](gotchas.md) 13.5, 14.6 | This repo |
