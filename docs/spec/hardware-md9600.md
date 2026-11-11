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

Status (2026-10-20): **nothing in this file has been checked on a radio.** No MD-9600 is in hand, and every value is
unverified until one is. RadioFoundry builds for it and drives FM receive on every board revision (section 19). Section
17 lists every open question.

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

| Item | Value | Note |
|---|---|---|
| HSE | 8 MHz, a crystal oscillator (not bypass mode) | V1-V3: starts in crystal mode (17 #18) |
| LSE | 32.768 kHz crystal, RTC enabled |  |
| SYSCLK, USB | 72 MHz and 48 MHz | unverified |
| AHB / APB1 / APB2 | 72 MHz / 36 MHz (/2) / 72 MHz (/1): the DM-1701's bus clocks | unverified |
| RadioFoundry's plan | HSE 8 MHz crystal, PLL M 4, N 72, P 2, Q 3 (2 MHz PLL input, VCO 144 MHz), AHB /1, APB1 /2, APB2 /1, flash 3 wait states. If the crystal doesn't start, RadioFoundry tries bypass mode, then stays on HSI 16 MHz (no USB) | ours |

RadioFoundry derives every time constant from the clock it set up (SystemCoreClock), so a wrong HSE guess shows as a wrong
USB clock and a wrong tick, not as a silent offset.

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
| PA2 | ADC123 IN2: QT_DQT_IN, sub-audio tone input, sampled by ADC3 | ADC3 (unverified) |
| PA3 | ADC1 IN3: VOX / mic level | the only mic level input (no separate "mic" signal). A level tap, not the audio: 3-4 counts with the mic quiet, unmoved by a beep in the speaker (2026-11-03). VOX reads it (`power_md9600.c`) |
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
| PB6 | AF_SW, audio amplifier mute, a plain GPIO output: high = muted | unverified; no timer on this pin |
| PB7 | U_V_MOD_SW: HR_C6000 MOD2 output to the VHF (high) or UHF (low) VCO |  |
| PB8 / PB9 | I2C1 SCL / SDA (AF4, open drain) | one device at 7-bit address 0x3D, not identified (?) |
| PB10 / PB11 | Channel encoder A (EXTI, both edges) / B; USART3 TX / RX when the optional remote head is fitted | 9.2, 9.5 |
| PB12 | SPI flash chip select |  |
| PB13 / PB14 / PB15 | SPI2 SCK / MISO / MOSI (AF5): SPI flash **and** LCD | mode 3, 18 MHz (12.1, unverified) |

### 2.3 Port C

| Pin | Function | Notes |
|---|---|---|
| PC0 / PC1 / PC2 | HR_C6000 timeslot / system / RF TX interrupts (EXTI0-2, falling edge) | see 4.4 for the RF TX line |
| PC3 | ADC2 IN13: RX_2T_5T (discriminator audio), sampled with DMA for sub-audio tone detection | unverified |
| PC4 | RF_APC_SW, PA power loop on |  |
| PC5 | ADC1 IN15: VHF noise level |  |
| PC6 | Display_Light: LCD and key backlight, TIM8 CH1 PWM (AF3) | unverified, 7.3 |
| PC7 | CTC_DCS_PWM, sub-audio tone output, TIM8 CH2 PWM (AF3) | checked |
| PC8 | BEEP_PWM, TIM8 CH3 PWM (AF3) | unverified |
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
| PE2 / PE3 / PE4 / PE5 | HR_C6000 configuration port CS / CLK / MISO / MOSI (bit-banged) | MISO and MOSI are the other way round from the DM-1701 (unverified) |
| PE6 | C6000_PWD, HR_C6000 power down |  |
| PE7 | FM_MUTE: FM audio to the volume control, high = unmuted |  |
| PE8 / PE9 | VCO_VCC_U_SW / VCO_VCC_V_SW: per band, high = RX VCO, low = TX VCO |  |
| PE10 | PTT (hand mic), input with pull-up, active low |  |
| PE11 / PE12 | V1-V4: IF_CS_V / IF_CS_U (IF chip selects). **V5: VHF / UHF IF bandwidth, high = 20 or 25 kHz** | 5.4, as V26.45 drives them |
| PE13 / PE14 | V1-V4: IF_DATA / IF_CLK. **V5: connect the VHF / UHF IF to the HR_C6000** (high = connected) | 5.4, as V26.45 drives them |
| PE15 | V1-V4: IF_RST (IF chip reset, active low) | V4A / V5: unused, V26.45 leaves it an input |

### 2.6 Reset-time levels and SWD

Reset levels: HR_C6000 CS/CLK/MOSI and power-down low, IF chip selects/data/clock high, IF reset
low, both synthesizer chip selects and the voice port CS high, synthesizer data/clock low, LCD lines low, Power_Control
low, all T5/R5/C5 supply switches low, U_V_MOD_SW low, FM_SW and FM_MUTE low, CTC_DCS_PWM high. Then
`radioPowerOn` sets both T5 low, all C5 and R5 high (both receivers are always powered together, "as the TYT firmware
does"), then Power_Control high.

RadioFoundry's transmitter-off levels (`board_rf_safe`, on any fault, crash or reset; [crash-log.md](crash-log.md)): APC_EN,
T5_V and T5_U low, VCO_U and VCO_V high (the receive VCOs), the PA reference DAC (DAC1 channel 2) at 0.

PA13 / PA14 are the receiver supplies of both bands: like the MD-380, the radio takes the SWD pins at run time, and a
debugger has to connect under reset (D for the pins; the consequence ours).

---

## 3. Interrupts, DMA, timers

| Source | Use | Note |
|---|---|---|
| EXTI0/1/2 (PC0-PC2) | HR_C6000 timeslot / system / RF TX, priority 5 |  |
| EXTI15_10 (PB10) | Channel encoder A, both edges, priority 5 |  |
| DMA1 Stream0 ch0 / Stream5 ch2 | I2S3 RX / I2S3ext TX, circular | as the DM-1701 |
| DMA2 Stream2 ch4 | USART1 RX (GPS), circular |  |
| DMA1 Stream1 / Stream3 ch4 | USART3 RX / TX (remote head only) |  |
| TIM8 CH1 / CH2 / CH3 | PWM on PC6 (backlight), PC7 (sub-audio tone), PC8 (beep): prescaler 9, period 256, about 28 kHz at 72 MHz. CH4's pin (PC9) is a GPIO | unverified |
| TIM6 | DAC2 trigger at 10 kHz for the PA power ramp | unverified |
| ADC1 | 8-channel scan with DMA (10.3) | unverified |
| ADC2 | IN13 (PC3, discriminator audio), circular DMA, for sub-audio tone detection | unverified |
| ADC3 | IN2 (PA2, QT_DQT_IN) | unverified |

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
| IF | 450 kHz (4:0x07-0x09 = 0x0B 0xB8 0x00), from the IF receiver chip into ADC_IVINP (pin 70). **4:0x01 bit 6 (receive I/Q swap) follows the first LO's side** (5.2): clear on UHF (LO below, 0xB0 in DMR), set on VHF (LO above, 0xF0), as the radio's own firmware does. The wrong value inverts the spectrum: about one voice burst in six syncs and no call is tracked (first radio, 2026-11-02, both bands) |
| FM receive audio | **not through the HR_C6000**: 4:0x36 = 0x00 always. FM audio comes from the IF chip's demodulator via U_V_AF_SW, FM_SW, FM_MUTE (6.1). The chip stays in DMR mode in FM receive (4:0x10 bit 7 clear), as the radio's own firmware leaves it: in FM mode its line out plays only its own FM audio, so beeps were silent on FM channels (first radio, 2026-11-03) |
| FM TX, CTCSS, DCS, DTMF, tones | through the HR_C6000 (TX tone generator, RX CTCSS / DCS detector); RadioFoundry sends CTCSS and DCS as PWM on PC7 instead (19.9) |
| MOD2 routing | U_V_MOD_SW (PB7) sends the MOD2 output to the active band's VCO; set with every synthesizer write |
| Reference / MOD2 offset | per band byte t from calibration (11.2); signed c = t - 128; 4:0x47 = c, 4:0x48 = 0x03 if c < 0 else 0x00, 4:0x04 = c. Rewritten at every RX entry (otherwise an RX offset is reported after TX) |
| DMR sync detection | 4:0x5F = 0xF0 (MS, BS, TDMA1, TDMA2) |
| Tone mode | **4:0xA1 = 0x00** after the power-on init (bit 7 is reserved, must be 0). With the init's 0x80 the chip never syncs on DMR, though its RSSI (4:0x43 / 0x44) follows the signal (first radio, 2026-11-02) |

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

**V1-V3 reference divider:** R 6 (VHF) and 4 (UHF) is one case. The divider also depends on a per-channel setting
and on which side of the signal the LO sits, giving R 5 / 4 / 6 or 3 / 2 / 4. What the setting means is not known
(unverified); RadioFoundry uses 6 and 4.

**V4: "PLL1590" synthesizer, one per band**. TYT's own folder for the V4 firmware (V16.45) is named "firmware for
PLL1590", which confirms the part family earlier notes call AK1590; the exact part number is still
unconfirmed (?). The V16.45 stock images (all four variants) hold the 24-bit words 0x1FF404 and 0x19F404 as literals,
that is reg 4 = 0x1FF40 and 0x19F40 in the format below; the V06.09 images hold neither (from our own reading of the stock images). 24-bit
words: 20 data bits then the 4-bit address in the low nibble. N in reg 2 (VHF 0x20000 | N, UHF 0x38000 | N), the
18-bit fraction in reg 1, reg 3 = 0x6000 | R, reg 4 = 0x1FF40 for VHF and for UHF receive, 0x19F40 only for UHF transmit (unverified), reg 5 =
reg 6 = 0. R: VHF 12,
UHF RX 8, UHF TX 4. **No x2 on VHF** here (VCO on the LO). Write order 5, 6, 3, 4, 1, 2. The V4 VHF synthesizer must
be programmed (145 MHz RX) right after power-on or it never locks.

**V5 (and V4A): TI LMX2571, one per band**, as the original V26.45 firmware programs it (read from its image, not yet
checked on a radio). 24-bit words: 8-bit address then 16 data bits. R: VHF 8, UHF 4; 24-bit fraction, the remainder
of VCO / f_pd times 2^24, truncated; reg 0x04 = 0x3000 + N, reg 0x01 = fraction >> 16, reg 0x02 = fraction low 16,
reg 0x03 = 0, reg 0x05 = 0x100 + R, reg 0x06 = 0x8581, reg 0x07 = 0x1024, reg 0x08 = 0x0430, reg 0x29 = 0x0230,
reg 0x2A = 0x0108, reg 0x00 = 0x003E. Sequence on every retune: 0x00, 0x04, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08,
0x29, 0x2A, then 0x00 = 0x003F (start calibration) and 0x00 = 0x003E. The clock is left low after each word. The reset
(0x00 = 0x2000) goes to each chip once at power-up, VHF then UHF, followed by a wait of 10 system ticks (nominally
10 ms), and never on a retune. The words are the same for RX and TX; only the frequency and R change.

The R above is for the default of a 2-bit per-channel setting (probably the CPS reference frequency choice; inferred).
Its other values give R 5 or 4 (VHF) and 3 or 2 (UHF); RadioFoundry uses only the default. The LO side follows the
band's upper edge from calibration (11.2): at or below 200 MHz high side, else low side, so VHF is high side and UHF
low side as in 5.2.

From the LMX2571 data sheet [TI]: the word is a R/W bit (0 = write), a 7-bit address and 16 data bits, clocked in on
the rising edge and latched when LE rises. In these values R4 is FRAC_ORDER 3 (third-order modulator) and PLL_N (12
bits, at most 1023), R1 holds PLL_DEN[23:16] (0) and PLL_NUM[23:16], R2 PLL_NUM[15:0], R3 PLL_DEN[15:0] (0), R5 PLL_R 1
and the pre-divider R, R6 the multiplier bypassed, R8 EXTVCO_SEL (PLL mode with an external VCO) and FSK_EN. The data
sheet asks for DEN = 0 when FSK_EN is set; the fraction is then over 2^24, as V26.45 computes it. R7 turns both of the
chip's RF output buffers off (the external VCO is the output). R42's charge pump down current is not equal to R41's up
current, though the data sheet asks for equal; that is V26.45's value. The data sheet's power-on sequence also writes
R60, R58, R53 and the rest; V26.45 writes no register outside the set above, and neither does RadioFoundry. A reset
restores the chip's defaults (its own VCO, the TX output buffer on, the external VCO's charge pump off) until R7 and
R8 come again, which is why neither resets on a retune.

No lock detect input is known for any revision (?). V26.45 reads no synthesizer pin and
never enables the LMX2571's lock detect output.

### 5.4 IF receiver per revision

**V1-V4: AKM AK2365A, one per band**. Bit-banged 16-bit words on IF_DATA (PE13) / IF_CLK (PE14) with chip
selects PE11 (VHF) / PE12 (UHF), reset PE15 (pulse low for at least 1 us). Data set while the clock is low, latched on
the rising edge. Written each time the receiver is turned on: soft reset 0x08AA, then 0x02F1 (25 kHz) or 0x02E9
(12.5 kHz), 0x041D (AGC), 0x0600 (IF gain 6 dB), 0x1601, 0x1880 (auto AGC). The chip's 450 kHz output feeds
the HR_C6000, its demodulated audio the FM path (6.1).

From the AK2365 data sheet [AKM] (the A part taken to be the same, unverified): a frame is 16 clocks, MSB first,
latched on the rising edge of SCLK while CSN is low: R/W (0 = write), a 6-bit address, one low bit, 8 data bits. That
is the word reg << 9 | data above. SCLK high and low are at least 500 ns. Register 1 = 0xF1 is mode 7 (everything on),
the wide demodulator level, the +-6 kHz filter and the 50.4 MHz second LO; 0xE9 the narrow level and the +-3 kHz
filter. Register 2 = 0x1D starts the discriminator calibration (1.3 ms). Register 4 = 0xAA is the software reset. After
power-up the chip needs a hardware reset (RSTN low at least 1 us).

**V5: no programmable IF chip.** As V26.45 drives the lines: PE11 / PE12 set the VHF / UHF IF filter, high for a 20
or 25 kHz channel and low for 12.5 kHz (only the receiving band's line is written). PE13 / PE14 connect the VHF / UHF
IF to the HR_C6000, high = connected: the receiving band's high and the other low, both high in dual watch, both low
before a transmit retune and back on last in the return to receive. PE15 is not used. The V5 IF part is not
identified (?); the firmware doesn't name it.

### 5.5 Receive and transmit switching

RX: Power_Control on, both R5 and both C5 on, T5 off, RF_APC_SW off; synthesizer to the LO with VCO_VCC high for the
band; DAC1 (UHF) or DAC2 (VHF) = the calibrated front-end tuning voltage; IF set up for the band and bandwidth;
U_V_AF_SW to the band.

TX: both R5 off, synthesizer to the carrier with VCO_VCC low, DAC2 = APC drive (for both bands: PA5 serves the APC in
TX), then the band's T5 on and the other off, then RF_APC_SW on. On V4 and V4A / V5 the other band's C5 supply goes off
while one band transmits (V4A / V5: as V26.45 does it); V1-V3 leave both on (unverified). Per DMR burst only T5 and
RF_APC_SW toggle, with no retune; return to RX drops RF_APC_SW, both T5, then reprograms for RX. There is no lock check.

V26.45 (V4A / V5) timing: the transmit retune drops RF_APC_SW, both R5 and both IF routes, sets the supplies and VCO
lines, writes the words, then waits 12 system ticks (nominally 12 ms) before T5 and RF_APC_SW. The return to receive
writes the words, turns R5 on, waits 4 ticks, then connects the IF routes. The ordering beyond that is the reference
port's.

### 5.6 Power levels and control

DAC2 (12 bit) is the APC reference: the PA drive, in DAC counts. The factory calibration (11.2) holds four PA rows per
frequency point, labeled 5 W, 10 W, 25 W and 40 W. The labels are wrong: the rows put out about 10, 14-21, 24-28 and
40-50 W. Measured on one radio with a wattmeter into a dummy load (FM carrier, procedure in
[power-calibration.md](../testing/power-calibration.md)):

| | UHF 446.100 | UHF 440.000 | VHF 151.5125 | VHF 146.535 |
|---|---|---|---|---|
| Factory rows (drive → W) | 816 → 9.6, 928 → 13.9, 1232 → 24.8, 1760 → 39.6 | 800 → 8.7, 896 → 12.3, 1216 → 23.6, 1776 → 39.2 | 992 → 10.1, 1072 → 20.7, 1136 → 28.5, 1760 → 50.2 | 1008 → 10.1, 1056 → 15.8, 1136 → 24.7, 1680 → 48.4 |
| Zero point (highest drive under 0.02 W) | 440 | 450 | 870 | 870 |
| Cliff (highest drive at about 10 W) | 410 | 420 | 860 | 850 |
| Hot (after 3 × 5 s at 25 W): zero point, cliff | 445, 415 | | 870, 860 | |
| Low curve (drive → W) | 450 → 0.07, 504 → 0.96, 677 → 4.99 | 450 → 0.01, 500 → 0.76, 680 → 4.88 | 880 → 0.22, 890 → 0.88, 950 → 5.65 | 880 → 0.36, 890 → 0.86, 950 → 4.85 |
| Top (drive → W) | 2100 → 45.4, 2250 → 47.1 | 2100 → 44.5, 2200 → 46.1 | 1500 → 46.7 | 1760 → 49.8, 1800 → 50.4 |

- **The cliff.** Below the zero point by 5-20 counts the power loop loses control and the PA puts out about 10 W. A
  drive of 0 is below it: the ramp starts and ends at the band's zero point (UHF 440, VHF 870), and the drive goes to 0
  only with the loop open. Ramping from 0 gave a carrier at key-up and a splash at key-down.
- **The low end** follows the absolute drive: the zero point moved 10 counts between the two UHF frequencies and not
  at all on VHF. The low levels are fixed drives per band. At the very bottom that spread is 2-3 dB: drive 457 gave
  0.18 W at 446.100 and 0.10 W at 440.000, so 50 mW can't hold ±2 dB across the band.
- **The upper levels** move with frequency and follow the factory rows: 25 W is interpolated in watts between the rows
  around it, the top level is the top row plus a fixed step.
- **Lowest level.** The margin above the cliff is 20 counts (hot shift 5). UHF: 100 mW (456), the lowest name that
  fits ±2 dB at both frequencies. VHF: the curve is steeper, and 250 mW (880) is the lowest level that keeps the
  margin.

| Level | UHF drive | UHF name | VHF drive | VHF name |
|---|---|---|---|---|
| 0 | 456 | 100 mW | 880 | 250 mW |
| 1 | 509 | 1 W | 892 | 1 W |
| 2 | 680 | 5 W | 947 | 5 W |
| 3 | rows 2-3 (UHF) or 1-2 (VHF) at 25 W | 25 W | as UHF | 25 W |
| 4 | top row + 337 | 45 W | top row + 45 | 50 W |

The short check with this table (section 4 of the procedure, FM carrier, watts):

| Level | 446.100 | 440.000 | 151.5125 |
|---|---|---|---|
| 0 | 0.15 (100 mW) | 0.08 (100 mW) | 0.18 (250 mW) |
| 1 W | 1.06 | | 0.93 |
| 5 W | 5.07 | | 5.29 |
| 25 W | 26.1 | | 27.0 |
| Top | 45.9 (45 W) | | 50.7 (50 W) |
| Dark step (cliff + 5) | 0.00 (437) | | 0.00 (865) |

On VHF the low levels' output rises toward the bottom of the band at a fixed drive: level 0 (880) read 0.37 W at
144.010, 0.25 W at 146.000 and 147.990, and 0.14 W at 151.5125 (−2.5 dB: outside ±2 dB). Drive 882 read 0.45 W
at 144.100 and 0.20 W at 151.5125. One fixed drive can't hold 250 mW from 144 to 152 MHz, so the VHF low levels are
calibrated at 146 MHz, the middle of 2 m, and hold ±2 dB across it. The HIL wattmeter check keys at 445 and 146 MHz
for that reason.

A radio whose rows give levels that don't rise, or a top level over 2300 counts, transmits on no level. The debug
link's `tx_drive` view key shows each level's drive, and RADIO op 36 sets one transmission's drive
([debuglink.md](debuglink.md)). RadioFoundry refuses TX without an accepted table.

### 5.7 RSSI, noise, squelch

Analog per band: VHF RSSI PA0, UHF RSSI PB0, VHF noise PC5, UHF noise PB1. A fit, not a measurement (?): V1/V2/V4
-135 dBm (VHF) / -145 dBm (UHF) + raw8 / 2; V5 -155 dBm + raw8 / 1.95 (raw8 = the 12-bit average >> 4). V26.45 has no
dBm conversion (it compares raw readings with the calibration's squelch rows), so the V5 fit doesn't come from it. Calibration also has RSSI points at -120 and -70 dBm and squelch open / close rows (11.2).

The noise line follows the modulation. Under a -73 dBm NOAA carrier (VHF, 25 kHz), it reads about 100 counts in a pause and
peaks at 900-1500 in speech for tens of ms; an empty channel holds 1700-1900 (first radio, 2026-11-03). The squelch
therefore takes the lowest reading over the last 160 ms (`radio_md9600.c`), cleared on every retune: one reading a
poll let speech close it. RSSI holds steady through the modulation and falls about 11 dB within one 25 ms poll when the
carrier ends (UHF, -127 dBm), so a fall of 8 dB or more since the last poll starts the window over and the squelch closes
on the next reading (`md9600_noise_floor`, `rfseq_md9600.c`). Without it, the noise kept the squelch open about 170 ms
after the carrier dropped.

The calibration's squelch rows suit the 12.5 kHz filter. Through the 25 kHz one the noise line reads about twice as high
under the same weak signal: a -89 dBm VHF carrier reads about 1350 instead of 710, while an empty channel moves only from
about 1750 to 1850 and a full-quieting one stays near 220. A 25 kHz FM channel therefore multiplies both thresholds by 3/2
(`md9600_rf_sql_wide`); without that, the carrier stayed shut at level 1. 7/4 opened level 1 on empty UHF channels. DMR is
always narrow and keeps them.

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

PC6, TIM8 CH1, period 256 (unverified). **On V2 and later the LEDs are off below about 16 % duty and fully on by
19.3 %**, so RadioFoundry maps 1-99 % into counts 41-49 of 256 and 100 % to always on. A bit in the hardware option block
(11.1) marks radios whose backlight is only switched on and off (unverified). On V1 the duty is proportional; V1 has
different backlight hardware,. The keys
share the backlight (?).

---

## 8. LEDs

No MCU pin drives an LED (unverified). The front panel's TX / RX indicator may be driven by the HR_C6000 or by the RF
hardware; unknown (?).

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

On a V1-V3 radio (2026-11-02): two steps per detent, resting where A and B differ (01 or 10), and B leads A turning
clockwise. RadioFoundry samples (B, A) with the rest mask for 01 and 10: 10 clicks each way count 10, a wobble counts
none. With the A = B mask, every run lost its first click.

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

The mic has 16 keys plus its own up / down / A/B (unverified). Level windows in raw 12-bit counts, both levels at or
above 3445 with no key down (unverified):

| Level | Window 0 | Window 1 | Window 2 | Window 3 | Window 4 |
|---|---|---|---|---|---|
| Row (PA6) | 2830-3444 | 2215-2705 | 1477-2090 | 862-1352 | 247-737 |
| Column (PA7) | 0-1106 | 1231-1844 | 1969-2705 | 2830-3320 | - |

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

ADC1 scans eight channels with DMA, ranks IN9 UHF noise, IN15 VHF noise, IN8 UHF RSSI, IN0 VHF RSSI, IN3 mic level,
IN1 supply, IN6 mic keypad row, IN7 mic keypad column (unverified). RadioFoundry keeps that order, 480 cycles each at PCLK2 / 8,
in a circular DMA buffer. Supply: tenths of a volt = raw / 21.3
(D, plus a user trim); maximum 15.0 V; cutoff hysteresis 8.0 / 10.0 V.

---

## 11. Calibration

### 11.1 Location

512 bytes in the SPI flash security registers, read as one 512-byte security read from register offset 0 
(the same mechanism as the MD-380, [hardware-md380.md](hardware-md380.md) 11.1). A copy can also be kept
at main-array address 0x010000, recognized by the first 8 bytes of the UHF frequency list (00 25 00 40 00 45 01 40,
400.025 / 400.145 MHz). RadioFoundry reads the security registers and never writes them. The vendor CPS backs the table up.

Registers 1 and 2 hold 256 bytes each (unverified). Register 3 holds a hardware option block: band edges at +0x14,
+0x16, +0x18, +0x1A (16-bit, BCD MHz in the top bits: 0x1450 = 145.0 MHz) and bit 4 of +0x1D, set when the backlight
is only switched on and off (unverified).

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

W25Q128 class, 16 MB, JEDEC 0x4018, on SPI2 with CS PB12, shared with the LCD (7.1). The bus runs in mode 3
at PCLK1 / 2 = 18 MHz for both the flash and the LCD (unverified). Commands in use: 0x03 read, 0x06 write enable, 0x05
status, 0x02 page program, 0x20 4 KB erase, 0xD8 64 KB erase, 0x44 / 0x42 / 0x48 security register erase / program /
read with the register number in address bits 15-12. RadioFoundry reads the JEDEC ID but accepts any part that answers with a
size. Security registers hold the calibration (11.1).

### 12.2 Map

Earlier notes put the codeplug / "EEPROM" image at 0 and the calibration copy at 0x010000. The stock TYT codeplug
and the stock user database (CSV builds) also live in this flash: the stock firmware needs its flash content restored
to run again, so a full backup before the first RadioFoundry flash is mandatory.

What the stock firmware keeps where (unverified; the meaning of most blocks is open):

| Range | Content | RadioFoundry area over it (19.5) |
|---|---|---|
| security registers 1-2 | factory calibration, 512 bytes (11) | none: never written |
| security register 3 | hardware option block, 64 bytes (11.1) | none: never written |
| 0x002000-0x0021FF | settings and state blocks | open-firmware "EEPROM" image |
| 0x02EF00-0x02F00C | the current channel / VFO records and a state record | open-firmware flash image |
| 0x040000-0x049FFF | lists of 200-byte records | open-firmware flash image |
| 0x060000-0x0B97FF | fonts and bitmaps (the update file's resource segment) | open-firmware flash image |
| 0x100000-0x2077FF | further blocks, some loaded into RAM at start | open-firmware flash image |

The update file rewrites the resource segment at 0x060000, but nothing restores the codeplug, settings or lists except
a backup. The security registers are the only factory data that a backup can't put back without the stock CPS, which
is why RadioFoundry never writes them (19.5).

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

Disconnect DC (the power key is not enough), hold **P1 and the orange (alarm) key** while reconnecting. The screen stays
blank and the backlight blinks at 1 Hz (checked on a V1-V3 radio, 2026-11-02). The loader's DFU interface string for
the internal flash, `@Internal Flash   /0x0800C000/01*016Kg,01*064Kg,07*128Kg`, tells it apart from the original
firmware in normal mode, which shows `/0x08000000/03*016Kg` under the same USB IDs and strings.

### 14.2 Protocol and encryption

USB DFU 0483:DF11 with TYT's commands, the DM-1701's protocol ([hardware-dm1701.md](hardware-dm1701.md) 15.2). The image is XORed with the MD-9600's 1024-byte key, index from the start of the application image; key SHA-256
c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae, first bytes A2 FA BB 4B
([hardware-dm1701.md](hardware-dm1701.md) 15.3). The key can be derived from an original firmware file (as for the DM-1701 and MD-380): it works on every original firmware file listed in 14.3 (measured).

### 14.3 Stock firmware files

TYT names the files by feature: P = CSV + GPS, V = CSV, S = recorder + GPS, D = recorder (unverified). TYT's V4
firmware and CPS (for the "new updated PLL", V4 and later boards) are not compatible with V1-V3 radios (unverified).
TYT's download page offers only CPS installers for the MD-9600, no firmware. Retevis's firmware download offers the RT90's V5 firmware (two zips, `RT90-Firmware-v5` and `RT90-Firmware-GPS-v5`). Dealers and community pages mirror the older TYT files.

Files examined):

| Version | Boards | Files | Container |
|---|---|---|---|
| 03.28, 03.29, 04.03 | older, board not known | `MD9600-REC-D03.28.bin`, `-REC-D03.29`, `-REC-D04.03` | as 06.09 |
| 06.03 | V1-V3 (SKY72310) | `MD9600-CSV-V06.03.bin`, `-CSV-GPS-P06.03`, `-REC-D06.03`, `-GPS-REC-S06.03` | as 06.09 |
| 06.09, 06.24 | V1-V3 (SKY72310) | `MD9600-CSV-V06.09.bin`, `-CSV-GPS-P06.09`, `-REC-D06.09`, `-GPS-REC-S06.09` | model string "MD-9600" at 0x10 |
| 16.24, 16.45 | V4 ("PLL1590") | `MD9600-CSV(V4)-V16.45.bin`, `-CSV-GPS(V4)-P16.45`, `-REC(V4)-D16.45`, `-GPS-REC(V4)-S16.45` (and the same four as 16.24) | model string blank |
| 26.45 | V5 ("2571") | `MD9600-CSV(2571V5)-V26.45.bin`, `-CSV-GPS(2571V5)-P26.45`, `-REC(2571V5)-D26.45`, `-GPS-REC(2571V5)-S26.45` | as 16.45 |
| 26.38, 26.41, 26.42 | V5 ("2571"), Retevis RT90 | `RT90-CSV(2571V5)-V26.41.bin`, `-CSV-GPS(2571V5)-P26.41`, `-REC(2571V5)-D26.38`, `-GPS-REC(2571V5)-S26.42` | as 16.45 |
| 26.57 | V4A ("2571V4") | `MD9600-REC(2571V4)-D26.57.bin` | as 16.45 |

Files named `MD9600-CSV(V4)-V26.45.bin` and `-CSV-GPS(V4)-P26.45` also circulate; they hold the V16.45 V4 ("PLL1590")
application, not LMX2571 code. The D26.57 V4A build programs the synthesizers and the IF lines exactly as the V26.45
builds do.

All of them: two segments, (0x00060000, 0x59800) for the external flash resources and (0x0800C000, 0xB1800..0xB6800)
for the application, payload at 0x100, 0x100 trailing bytes. The leading digit of the version number is the board
family (0 = V1-V3, 1 = V4, 2 = V4A / V5) (F: inferred from the known files). The V06.09 images report SP 0x20001BA0,
the V16.45 ones 0x20001BB0.

### 14.4 Flashing: stock bootloader and stub

Nothing below has run on an MD-9600 yet: every row is unverified.

| Route | When | How |
|---|---|---|
| TYT bootloader | the first install, a radio on stock firmware, or a radio whose application is not committed | keys at power-up (14.1), then DFU with the MD-9600 key (14.2): `tools/stm32_dfu.py --model md9600` |
| Flash stub | a radio already running RadioFoundry built with `CONFIG_STUB_ENTRY=y` | `stub/stubtool.py PORT --rfoundry-enter --write-app build/md9600/rfoundry_md9600.bin` ([stub-stm32f4.md](stub-stm32f4.md)) |

The stub is the DM-1701's and MD-380's image. The host sends it this radio's board record: SPI flash on SPI2 (PB13
SCK, PB14 MISO, PB15 MOSI, AF5), chip select PB12, mode 3, at most 4.5 MHz, the LCD's chip select PD14 held high while
the stub runs, an 8 MHz crystal, the bootloader ending at 0x0800C000. The stub never touches SPI1 / PD7, the HR_C6000's
voice port here.

An update with the stub writes the changed sectors of 3-8 only. It leaves the codec in sectors 9-11 alone unless the
image is merged with the codec (`rfoundry_md9600_codec.bin`), and never touches the bootloader in 0-2. Sector 3 is erased
first and its first word written last, so an interrupted update leaves the application invalid. Whether the TYT
bootloader then stays in DFU on its own, as the DM-1701's does, is unverified. If it doesn't, the key combination of
14.1 still reaches DFU. The stub also backs up and verifies the whole internal flash and the 16 MB SPI flash
(`--progmem`, `--q128`), which covers the backup 12.2 asks for once RadioFoundry runs. Before the first install the
web flasher backs up what the original firmware or the bootloader gives (15.2).

---

## 15. USB

OTG_FS on PA11 / PA12, mini-USB on the rear, straight to the MCU (no UART bridge). The bootloader's DFU device is
separate (14.2). No VBUS sensing: PA9, the OTG_FS VBUS pin, is the mic and GPS supply switch (unverified), so the core
must run with VBUS sensing off. The stock application's device is DFU class with two alternate settings, the internal
flash and the SPI flash, self powered. TYT's CPS reads and writes the radio through it; most of the commands inside
its transfers are not known (17 #21).

### 15.1 The original firmware's version over USB

Checked on a radio (V06.09, 2026-11-02). Running normally, the original firmware enumerates as USB 0483:DF11,
manufacturer "AnyRoad Technology", product "Digital Radio in USB mode": the same IDs and strings as the bootloader.
Its interface's alternate settings are "@Internal Flash   /0x08000000/03*016Kg" and
"@SPI Flash Memory /0x00000000/16*064Kg"; wTransferSize is 1024. It starts in dfuIDLE. The bootloader's first
interface string is "@Internal Flash   /0x0800C000/01*016Kg,01*064Kg,07*128Kg" (seen on a radio): the strings tell the
two apart without a request, and the installers send the requests below only to a device showing the original
firmware's string.

It reports its version, and nothing on the radio changes, with these requests on interface 0 (wIndex 0, wValue 0):

| Step | Request | Expect |
|---|---|---|
| 1 | GETSTATUS (IN 0xA1, request 3, 6 bytes) | bState 2. In 10: CLRSTATUS (OUT 0x21, request 4); in any other: ABORT (OUT 0x21, request 6) |
| 2 | DNLOAD (OUT 0x21, request 1, block 0), data `A2 02` | accepted |
| 3 | GETSTATUS, GETSTATUS | bState 4 (the command runs after this reply), then 5 |
| 4 | ABORT | dfuIDLE |
| 5 | UPLOAD (IN 0xA1, request 2, block 0, 1024 bytes) | 4 bytes `56 0M m0 mm`: version Mm.mm, decimal digits (`56 00 60 09` = 06.09, `56 01 60 45` = 16.45) |

A block 0 command is matched on its first byte and its exact length; an unknown one stalls. The bootloader answers the
same sequence too, with `54 02 10 06` (first byte 0x54, 'T'; from its code, not yet sent to a radio): only a first byte
0x56 ('V') is the original firmware's version. 0x54 means the bootloader; a stall or any other answer, unknown. The other block 0
commands, DNLOAD with block 2 and above, and a zero-length DNLOAD (its manifest step writes the SPI flash and resets the
radio) are never sent by RadioFoundry's tools. The version's leading digit is the board family (16), so the installers
read it to learn the board revision (19.3).

### 15.2 Backup before the install

The original firmware (running normally) and the bootloader both let a host read the radio without writing anything.
The web flasher backs up through either before an install (board.toml `[flash.original_backup]`,
[web-flasher.md](../dev/web-flasher.md#backup-before-the-install)). Every block 0 command below is DNLOAD block 0, then
GETSTATUS twice (bState 4, then 5), then ABORT, as in 15.1.

| Read | Original firmware (checked on a radio) | Bootloader (from its code; not yet tried) |
|---|---|---|
| Info blocks: `A2 ss`, then UPLOAD block 0 | 01 and 03: option block 0x00-0x37 (32 + 24 bytes); 04: 8 bytes of settings; 05: the calibration as loaded in RAM (512 bytes, odd points interpolated, not the raw registers); 07: CRC-32 of the chip UID, then the 96-bit UID; 08: the clock; 32: the UID record and the STM32 OTP area (561 bytes); 33: the SPI region table and the radio name; 50: a constant; any other code: the version (15.1) | the same codes; the version answers `54 02 10 06`; 01, 03, 04 and 05 may be blank |
| Internal flash | UPLOAD of block 2 or above, below program mode, answers from address 0, which on this chip is the flash from 0x08000000 (bootloader first): the block number is ignored, so one request is one snapshot of up to its wLength (at most 64 KB: the bootloader and the first 16 KB of the application). Linux usbfs refuses control transfers above 4096 bytes | the same |
| Program mode | `91 01` (only below mode 0x30); the radio stops normal operation | `91 01`, accepted once |
| SPI flash | `21` + LE32 address 0, then UPLOAD of block 2 + n, 1024 bytes each: the 16 MB from (n x 1024). About 47 KB/s, paced by the radio (346 s for 16 MB); re-reads matched | the same, for the first 1 MB only; above it the answer is a stale buffer |
| Leave | `91 05` (only in program mode): the radio restarts before it answers the first GETSTATUS, and comes back running normally | stays in program mode. Its `91 05` needs a calibration mode and is never sent. The flasher's own `91 01` only acts below 0x30 (whether it then stalls or does nothing is open), so a write in the same session starts at `91 31` (mode 0x40, erase and write). A power cycle starts over |

The codeplug areas are within the first 2 MB (0x002000-0x03EFFF and 0x110000-0x197FFF in the map of 12.2); the
11.7 MB above 0x204000 (a contact database) is most of the time a full read takes. The internal flash above
0x08010000 can't be read by either: the media read of the internal flash returns a RAM buffer.

Never sent by the backup: `41` (erase), DNLOAD with block 2 and above (write), a zero-length DNLOAD (the original
firmware's manifest writes the SPI flash at 0x02F000 and restarts), `91` with any second byte but 01 and 05 (05 only
to the original firmware), `B2`-`B5`, `C4`, `D5`, an UPLOAD of block 1, and an UPLOAD above 1024 bytes in program
mode. The flasher refuses each of them in code before it reaches the radio.

---

## 16. Board revisions

| PCB label | Synthesizer | IF | Other differences |
| --- | --- | --- | --- |
| V1 (first boards, some labeled V2) | SKY72310 x2 | AK2365A x2 | backlight not dimmable the V2 way (7.3) |
| V2, V3 | SKY72310 x2 | AK2365A x2 |  |
| V4 | "PLL1590" x2 (part ?) | AK2365A x2 | VHF synthesizer must be set at power-on |
| V4A, V5 | LMX2571 x2 | none programmable; PE11-PE14 switches | RSSI curve differs |

The original firmware follows the same split: V06.09 for V1-V3, V16.45 for V4, V26.45 for V5 (and D26.57 for V4A). The V4A
and V5 images program the radio the same way, and neither reads anything that tells V4A from V5. TYT
sometimes changed chips without changing the label; a wrong build boots but neither receives nor transmits, and does
no harm (unverified). GPS models have been reported working with the V5 build. Radios needing
TYT's V4 firmware are V4 or later.

No part of the radio reports its board revision, and TYT ships one firmware per revision (14.3). The GPS and recording
models use the same pins as the others (unverified).

The original firmware does report its version over USB (15.1), and its major version follows the revision: 06.xx
V1-V3, 16.xx V4, 26.xx V4A/V5. Any other major says nothing about the revision. A radio someone moved to another
revision's firmware would report that firmware's line (unverified whether that boots, 14.3).

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
| 1 | Which revision the test radio is | Settled for the first test radio (2026-11-02): its original firmware reported v06.09, so V1-V3; FM receive works with the V1-V3 words. PCB label not read |
| 2 | V4 synthesizer exact part ("PLL1590"), V5 IF part. V5 use of PE15: none, V26.45 leaves it an input | Photos of the board (and FCC internal photos) |
| 3 | Lock detect pin (none mapped; V26.45 reads none) | Scope / board photos; otherwise lock is inferred from RX |
| 4 | LMX2571 delays after reset and calibration. V26.45 waits 10 ticks after the reset and 12 from the transmit words to the PA; the relock time itself is not measured | Scope the PLL on retune; measure the relock time (also [gotchas.md](gotchas.md) 14.6, SFR / DMR burst switching) |
| 5 | RF TX interrupt usefulness and the 20 ms burst-off workaround | Settled (2026-11-03): keyed from the RF TX interrupt, the burst reaches full power ahead of its payload and every burst decodes; no workaround (19.8) |
| 6 | Power level labels (5/10/25/40 W) and the low-level fractions | Power meter into a dummy load, both bands, edges and center |
| 7 | RSSI / noise curves per revision | Signal generator steps |
| 8 | Mic keypad thresholds | Settled (2026-11-02): with the current thresholds all 19 hand mic keys and the mic PTT read as the right key |
| 9 | Front panel LEDs, key backlight | Look at the radio |
| 10 | Loader key combination (orange vs red + P1) | Settled (2026-11-02): P1 + orange, DC reconnected; the backlight blinks at 1 Hz (14.1) |
| 11 | Ignition sense | Schematic / board photos |
| 12 | I2C1 devices | Bus scan |
| 13 | Calibration unknown bytes, BCD frequency format | Dump the security registers and compare with the vendor CPS test screen |
| 14 | 0xA6 needed at display init | First boot |
| 15 | The I2C1 device at 0x3D | Bus scan, board photos |
| 16 | GPS UART baud rate (9600 in 13.1 is unverified) | Listen on PA10 with a GPS model |
| 17 | DMR slot and burst timing on this RF chain (synthesizer relock per TX/RX switch) | SDR captures of a DMR call |
| 18 | The crystal: 8 MHz crystal or an oscillator | Settled for V1-V3 (2026-11-03): the HSE starts in crystal mode, 72 MHz SYSCLK (`board.clock=crystal`) |
| 19 | LMX2571 registers beyond the documented set and the DEN = 0 reading: V26.45 writes nothing else and takes the fraction over 2^24, as RadioFoundry does. Still open: lock and spurs on a radio | Lock and spur check on V5 |
| 20 | Calibration record layout (11.2) and the hardware option block beyond the band edges | Dump security registers 1-3 |
| 21 | The CPS command set inside the DFU transfers (the version read is known, 15.1) | USB capture of TYT's CPS |
| 22 | Display orientation and column offset | Settled (2026-11-02): segments reversed, COM normal, column offset 4 (segments normal and COM reversed showed the picture upside down) |
| 23 | Encoder steps per detent and rest state | Settled (2026-11-02): 2 steps, resting where A != B, B leads clockwise (9.2) |
| 24 | The V1-V3 per-channel reference divider setting (5.3) | Retune across a band on V1-V3 |
| 25 | Receiver probe thresholds per revision (section 19.3) | Noise readings per revision with the probe words |
| 26 | Synthesizer settle times, the APC ramp, the PA drive rows and the squelch thresholds as RadioFoundry uses them (19.8) | Scope the synthesizer and PA lines on a retune and a key-up; power meter per level |

## 18. Sources

| Short name | Source | Kind |
|---|---|---|
| | Earlier hardware notes on the MD-9600 (pin map, register writes), outside this repository; facts only |  |
| | https://www.opengd77.org/viewtopic.php?t=83, hardware versions | Forum (F) |
| | https://www.opengd77.org/viewtopic.php?p=15, installation | Forum (F) |
| [passion-radio] | https://www.passion-radio.com/mobile/md9600-505.html | Retailer (F) |
| [TYT] | https://www.tyt888.com/download.html (2026-10-12: CPS 1.27, 1.33, 1.43 and a product photo; no firmware) | Vendor |
| | TYT stock firmware V06.09 and V16.45 ("firmware for PLL1590"), kept outside the repository; container headers and literal constants only | Vendor files |
| [FCC] | FCC ID POD-DMR3, https://fccid.io/POD-DMR3 |  |
| [SKY] | Skyworks SKY72310-11 data sheet |  |
| [ST] | STM32F405 data sheet, RM0090 |  |
| [gotchas] | [gotchas.md](gotchas.md) 13.5, 14.6 | This repo |
| [TI] | Texas Instruments LMX2571 data sheet, SNAS654A |  |
| [AKM] | AKM AK2365 data sheet, MS1453-E-02 |  |

---

## 19. RadioFoundry on the MD-9600

Bring-up state (2026-11-02): runs on a V1-V3 radio (original firmware v06.09). Checked: boot, display (orientation
7.2 and 17 #22), keys, the debug link and CPS, the data flash, calibration and option block reads, FM receive on VHF
(a NOAA station through a dummy load). Open: the receive audio level is high at the bottom of the volume knob and
sounds overdriven (the HR_C6000 settings, the FM_SW line and the IF chip words match the original firmware's; whether
the original firmware sounds the same on this radio is not checked); UHF receive, DMR, transmit. `boards/md9600/`.

### 19.1 What the image does

| Area | State |
|---|---|
| Clocks | 19.2 |
| Display | ST7567 on SPI2 (mode 3, 18 MHz) with the flash under one lock; TIM8 CH1 backlight, fully on when the option block says it can't be dimmed. The 128x64 layouts; hints name P1 and P2 ([ui.md](ui.md) 1) |
| Keys | the 4 x 3 matrix (rows open drain, driven low one at a time), the encoder on EXTI10 / EXTI11 into the shared quadrature decoder, PTT on PE10. The power key goes to the power code, not the key map |
| Power | no battery (`CAP_NO_BATTERY`). "Off" saves, sets a flag in RTC backup register 18 and resets; at start-up the flag holds the radio until the power key is pressed and released. Supply voltage from ADC1 IN1 |
| Flash | SPI NOR with the DM-1701's map (settings 0x011000, journal 0x019000, open-firmware codeplug at 0 and 0x020000); the board revision record at 0x01F000, appended. Security registers read only: calibration and option block (19.5) |
| Receive | FM on both bands, every revision (19.3). Front-end tuning from calibration rows 0x30 / 0x39 (11.2) when they aren't blank, else mid scale |
| DMR | receive on the shared HR_C6000 glue (configuration port PE2-PE5, voice port SPI1 at 4.5 MHz, interrupts PC0-PC2, I2S3 PCM). Reference offset and DMR modulation pair (11.2 rows 0x130-0x149) rewritten at each retune. The two codec profiles (tags 0x0609 and 0x1645, one id `md9600-s11`) run the stock codec from flash: code at 0x080E0000, tables in sector 9, descriptor at 0x080FFF80 ([codec-md9600.md](codec-md9600.md)) |
| Audio | FM from the IF chip (FM_MUTE high), PCM (DMR voice, beeps) from the HR_C6000 line out with FM held back, AF_SW muting otherwise (`boards/md9600/speaker_md9600.h`). A host audio session (`NET_USRP`) hears FM from PC3 and DMR from the stream, and transmits FM as PWM on PC8 into the HR_C6000's microphone input (19.9) |
| GPS | USART1 RX (PA10), receive interrupt; 9600 baud first, then 4800, 19200, 38400, 57600, 115200, 3 s each. A silent line ends the search after the first window; a module once heard feeds APRS positions. PA9 stays on (it also powers the mic) |
| Hand mic | the keypad of 9.3 through `drivers/input/adckeys`, three equal scans per key. Key roles of A-D and A/B are a proposal (unverified). No `CAP_KEYPAD`: a mic may have no keypad |
| Transmit | FM and DMR on UHF and VHF, checked off air into a dummy load (19.8, 19.9; VHF 19.9.1). On in the build (`CONFIG_MD9600_TX`, `CONFIG_MD9600_DMR_TX`); it still needs the run-time arm, a stored revision and plausible PA rows. Sub-audio: CTCSS and DCS from the PWM generator on PC7, with the DCS turn-off tail |
| Not yet | Transmit power per level (a power meter), split duplex and the two-PLL burst switching (17 #17), the CPS's own command set, update packaging |

### 19.2 Clock plan and memory map

| Item | Value |
|---|---|
| HSE | 8 MHz crystal; bypass mode tried if it doesn't start; HSI 16 MHz (no USB) as the last resort |
| PLL | M 4, N 72, P 2, Q 3: SYSCLK 72 MHz, USB 48 MHz |
| Buses | AHB 72 MHz, APB1 36 MHz (timers 72 MHz), APB2 72 MHz |
| Flash | 0x0800C000-0x0809FFFF application (sectors 3-8, 592 KB); sectors 9-11 kept free for the codec |
| RAM | 128 KB main SRAM (DMA buffers), 64 KB CCM (RTOS heap and task stacks, `RFOUNDRY_CPU_RAM`) |

### 19.3 Board revisions in one image

| Revision | Synthesizer | IF | Switch |
|---|---|---|---|
| V1-V3 | SKY72310, 16-bit words, VHF VCO at 2 x LO | AK2365A | `CONFIG_MD9600_V1V3` |
| V4 | "PLL1590", 24-bit words, the VHF synthesizer set at power-on | AK2365A | `CONFIG_MD9600_V4` |
| V4A / V5 | LMX2571, 24-bit words | PE11-PE14 filter and route lines | `CONFIG_MD9600_V5` |

All three are on by default; a build with fewer drives only those boards. The revision in use comes from, in order:

1. The stored record (8 bytes at 0x01F000: "M96R", a format byte, the revision and its complement). Only a stored
   record confirms the revision; transmit will need it.
2. The receiver probe, when built with `MD9600_REV_PROBE=1`. With the transmitter supplies and the PA loop held off and
   the RX VCOs selected, it programs the UHF synthesizer with each built revision's words for 446.0 MHz receive and
   reads the UHF noise level against the unprogrammed baseline. A revision wins when its change is at least the
   threshold and twice every other one's. It never drives PE15. The thresholds are unmeasured, so it is off by default.
3. The build default, `MD9600_REV_DEFAULT` (v5).

A stored revision this image can't drive leaves the receiver off rather than programming the wrong chip. The record
is set with Settings > Advanced > Hardware > Board revision or the debug link's HWOPT (`rfoundry_debug.py hwopt "Board
revision" V4A/V5`); it applies from the next start-up. The installers set it themselves: before the install they read
the original firmware's version (15.1, 16), and after the first start they store the revision it gives when the
radio holds none (board.toml `[flash.original_version]`, [web-flasher.md](../dev/web-flasher.md#the-original-firmware-version)).
A stored revision that differs is reported and kept. A version read by the installer counts as a person's statement:
the record it stores confirms the revision like one set by hand. The open-firmware CPS has no item for it. Until a record is
stored the radio receives with the probe's or the default's revision and never transmits, and the diagnostics page
asks for one ("Set rev: Adv>Hardware"). Its first three lines show the revision and its source ("unbuilt" for a
stored revision this image doesn't drive), that prompt or "rev confirmed", and the probe ("off" when not built).

The default is one constant, `MD9600_REV_DEFAULT` in `boards/md9600/board.mk`. It is v5, the current production
board.

### 19.4 Pin use in RadioFoundry

| Pins | Direction | Idle level | Function | Status |
|---|---|---|---|---|
| PA0, PA1, PA3, PA6, PA7, PB0, PB1, PC5 | analog in | - | ADC1 scan: RSSI, supply, mic level, mic keypad, noise | unverified |
| PA4, PA5 | analog out | 0 | DAC1 UHF tuning, DAC2 VHF tuning / APC | unverified |
| PA8, PC9, PA13, PA14 | out | low, then high in receive | C5 and R5 supplies (PA13 / PA14 are SWD: connect under reset) | unverified |
| PD5, PD6, PC4 | out | low | T5 supplies, PA loop: high only in an armed `CONFIG_MD9600_TX` build (19.8) | unverified |
| PD15 | out | low, high in receive | Power_Control | unverified |
| PE8, PE9 | out | high | RX VCOs | unverified |
| PB7 | out | low | MOD2 routing | unverified |
| PD8-PD11 | out | CS high, clock and data low | synthesizer bus, bit-banged | unverified |
| PE11-PE14 | out | CS high, clock and data low (V5: filter and route levels) | IF bus or V5 lines | unverified |
| PE15 | in, out on V1-V4 | high after the reset pulse | AK2365A reset | unverified |
| PB2, PB6, PC13, PE7 | out | low, PB6 high (muted) | FM stages, amplifier mute, audio band, FM mute | unverified |
| PC6 | TIM8 CH1 | off | backlight | unverified |
| PC7 | open drain, TIM8 CH2 when driven | released (high) | sub-audio tone PWM into the reference oscillator's modulation input (19.9). Driven low it pulls the reference: FM still receives, DMR never syncs (first radio, 2026-11-02) | checked |
| PC8 | out, TIM8 CH3 when driven | low | beep PWM net into the HR_C6000's MIC_P / MIC_N and LINEIN1; host FM transmit audio (19.9) | checked |
| PA9 | out | high | mic and GPS supply | unverified |
| PB12, PD14, PD13, PD12 | out | high, high, low, low then high | flash CS, LCD CS, A0, reset | unverified |
| PB13-PB15 | SPI2 AF5 | - | flash and LCD | unverified |
| PE0, PE1, PD0, PD1 | out, open drain | high | key rows | unverified |
| PD2-PD4 | in, pull-up | - | key columns | unverified |
| PB10, PB11 | in, pull-up, EXTI | - | encoder | unverified |
| PE10 | in, pull-up | - | PTT | unverified |
| PE2-PE6, PD7 | out | CS high, PE6 high (powered down) | HR_C6000 (held in power-down) | unverified |


### 19.5 Data flash, calibration and the board record

| Area | Range | Use |
|---|---|---|
| Open-firmware "EEPROM" image | 0x000000-0x00FFFF | codeplug part 1 (open-firmware-cps.md 7) |
| Free | 0x010000-0x010FFF | kept free (no calibration copy: the calibration stays in the security registers) |
| Settings ring | 0x011000-0x018FFF | 8 sectors ([settings-store.md](settings-store.md)) |
| Sector journal | 0x019000-0x01AFFF | 2 sectors |
| Board record | 0x01F000-0x01FFFF | the stored board revision (19.3) |
| Open-firmware flash image | 0x020000-0xDFFFFF | codeplug part 2; the CPS may write up to 0xE00000 |
| Native codeplug | 0xE00000-0xEFFFFF | reserved |
| Staging | 0xF00000-0xF3FFFF | TFTP and fast CPS writes ([tftp.md](tftp.md) 4) |

This is the DM-1701's map; a part smaller than 16 MB gets no map and the radio runs from RAM. The settings ring and the
journal sit under the foreign region guard ([settings-store.md](settings-store.md) 5), so stock data found there is
left alone until the debug link claims it.

**Board record.** Records of 8 bytes ("M96R", a format byte, the revision and its complement, a spare byte) are
appended to the sector, and the newest valid one counts. A power cut while one is written leaves the one before it.
The sector is erased only when its 512 slots are used; a cut during that erase leaves no record, which the radio
treats as a new radio (probe or build default, transmit refused).

**Security registers.** RadioFoundry reads registers 1-3 once at start-up and never erases or programs them, and never writes a
status register (the registers' one-time lock bits are there). The SPI NOR driver has no command for either, and
`tests/host/spiNorTest.c` checks that no such command reaches the chip. The open-firmware CPS can read them as area 10
and can't write them.

**Calibration.** `boards/md9600/cal_md9600.h` is the interface for the radio code, on the generic reader in
`drivers/cal/calblock/`. Both blocks are kept whole as read, unknown bytes included.

| Block | Read as | State | Values |
|---|---|---|---|
| Calibration | registers 1 and 2, 256 bytes each, one 512-byte block | `factory`, `blank` (all 0xFF or 0x00), `unreadable` (a failed read of either register drops the whole block) | every row of 11.2 by frequency: front-end tuning, the four PA levels, squelch 9 and 1 open and close, CTCSS and DCS deviation; the reference tune per band (byte - 128); the RSSI points; raw bytes |
| Hardware options | register 3, first 64 bytes | as above | band edges; the backlight on / off bit |

A row whose points are all 0xFF or all 0x00 has no value. Rows interpolate linearly between points (rounded) and
continue the last slope past the top point. Front-end tuning without a value is mid scale (2048). The four band edges
are sorted by value, so the order of the halfwords in the block doesn't matter: the two below 300 MHz are VHF, the two
above UHF, and anything else (a nibble above 9, three edges in one band, two equal) gives none. With the backlight bit
set, any brightness above 0 drives the backlight fully on. Transmit stays refused whatever the PA rows hold.

### 19.6 Debug link keys

The `board` view section ([debuglink.md](debuglink.md) 3.7) carries these keys, each as `board.<key>`. The supply
voltage is the generic `supply_mv` key instead (`hal_power_t.supply_mv`; the divider's scale is unverified, 10.3).

| Key | Value |
|---|---|
| `rev`, `rev.source`, `rev.usable`, `rev.confirmed` | the revision in use (`V1-V3`, `V4`, `V4A/V5`, `none`), where it came from (`stored`, `probe`, `default`, `none`), whether this image drives it, whether a person set it |
| `rev.stored` | the board record now (a change takes effect at the next start) |
| `probe` | `off`, or the revision the receiver probe picked (`none`: no verdict) |
| `clock`, `sysclk_hz` | `crystal`, `bypass` (an external clock on the HSE pin), `hsi_pll` or `hsi`; the core clock |
| `flash.id`, `flash.size`, `flash.map` | JEDEC memory type and capacity (`0x4018` for a 25Q128), bytes, 1 when the map of 19.5 is in use |
| `cal`, `opt` | the calibration and option block states (19.5) |
| `opt.backlight_switched`, `opt.vhf_hz`, `opt.uhf_hz` | the option bit; the band edges as `low-high` in Hz, `0-0` when there are none |
| `keys` | the matrix, one hex digit per row K0-K3, bits 0-2 = columns K4-K6, 1 = pressed |
| `ptt` | 1 while the hand-mic PTT is down |
| `lo_hz`, `retunes`, `refused` | the synthesizer's last frequency, retunes and refused retunes |
| `apc`, `cal.pa` | the APC DAC value of the last transmission; the calibrated PA drive per power level (5, 10, 25, 40 W labels) at the receive frequency as `a/b/c/d`, 0 where the calibration refuses it (19.8) |
| `pin.power`, `pin.c5`, `pin.r5`, `pin.tx` | output levels: Power_Control; C5 UHF and VHF; R5 UHF and VHF; T5 UHF, T5 VHF and the PA loop enable (`000` in this build) |
| `adc.noise_u`, `adc.noise_v`, `adc.rssi_u`, `adc.rssi_v`, `adc.mic`, `adc.supply`, `adc.mic_row`, `adc.mic_col` | the ADC1 scan, raw 12-bit (10.3) |
| `c6000`, `c6000.dmr`, `c6000.errors`, `c6000.tx_refused` | the HR_C6000 answered after set-up (with 4:0x10), in DMR, failed transfers, bursts the RF switch refused |
| `speaker` | bit 0 amplifier on, bit 1 FM audio through, bit 2 codec line out, bit 3 amplifier held warm (`speaker_hang_s`) |
| `mic_key` | the hand-mic key decoded on the last scan, 255 for none |
| `gps`, `gps.baud`, `gps.nmea` | module heard; the rate listened at (0 once given up); valid / bad sentences / bytes |
| `subtone` | the sub-audio decoder (`CONFIG_SUBTONE`): samples fed / rate; the input's rms and mean over 0.11 s (12-bit counts); the tone kind (0 none, 1 CTCSS, 2 DCS, 3 DCS inverted); match; an ADC or DMA fault |

`tests/hil/test_md9600.py` checks them on a radio: identity, every key present, the transmitter switches low, the
clock, the flash and its map, both blocks readable, no key or PTT down at rest, the ADC scan in range and a plausible
supply, and the receiver tuned when the revision is driven. It never transmits and skips on any other radio.

### 19.7 USB, programming and updates

| Path | State |
|---|---|
| USB device | CDC serial with the other RadioFoundry STM32 radios' IDs (1FC9:0094, network personality 0096, not built yet), product "MD-9600", serial number from the chip UID; VBUS sensing off (15) |
| Debug link | on the same port ([debuglink.md](debuglink.md)); the `board` view section (19.6) |
| Codeplug | the open-firmware CPS protocol as radio type 5 ([open-firmware-cps.md](open-firmware-cps.md)) and the fast codeplug protocol, writing through the journal (19.5). Area 10 reads the security registers; the CPS calibration window is empty (no calibration in the main array), so no CPS can write calibration |
| TYT's CPS | not supported: its command set is unknown (17 #21). Nothing of it is implemented |
| ROM DFU | debug link REBOOT mode 1 ([debuglink.md](debuglink.md) 3.11), only at RDP level 0, with the same safe pin levels as at power-up first (`board_sysboot_prepare`) |
| TYT bootloader | the stock route (14.1, 14.2): keys held at power-up, then DFU with the MD-9600 key |
| Flash stub | `CONFIG_STUB_ENTRY` (off by default, as on the DM-1701; in the maximal configuration): `board_stub_prepare` switches the transmitter off first, then the shared STM32F4 stub runs with this radio's board record (14.4). Not tried on this radio |

The rfoundry board entry, the web flasher's radio entry and the release images come from `boards/md9600/board.toml`.

### 19.8 RF sequencing

`boards/md9600/rfseq_md9600.c` holds the order of every RF step for every revision; `radio_md9600.c` maps its lines to
the pins of 19.4. No synthesizer has a lock detect line (5.3), so each gets a fixed settle time after its words. All
times are unverified on a radio; the V4A / V5 transmit settle and reset wait are V26.45's (5.3, 5.5).

| Step | Receive | Transmit (`CONFIG_MD9600_TX`, armed) |
|---|---|---|
| 1 | PA loop and both T5 off, APC reference 0 | the same |
| 2 | Power_Control, both R5, both C5 on | both R5 off, FM stages off |
| 3 | both VCO_VCC high (RX VCOs) | the band's C5 on; the other band's C5 off on V4 and V4A / V5, on for V1-V3; U_V_MOD_SW to the band; the band's VCO_VCC low (TX VCO); V4A / V5: both IF routes off |
| 4 | the band's synthesizer words for the LO (5.2), then 2 ms | the words for the carrier, then 2 ms (12 ms on V4A / V5) |
| 5 | front-end tuning: DAC1 (UHF) or DAC2 (VHF) from calibration | the band's T5 on, 1 ms |
| 6 | the IF: AK2365A set-up and 2 ms for its calibration (V1-V4), or the filter and route lines (V4A / V5) | RF_APC_SW on |
| 7 | U_V_AF_SW to the band, FM_SW on in FM, off in DMR; 3 ms | APC reference (DAC2) up to the calibrated level in 8 steps of 250 us |

Return to receive ramps the APC reference down in the same steps, opens the loop, drops both T5, then runs the receive
column with the last receive settings. For DMR bursts the PA alone (T5, the loop, the ramp) switches while the
synthesizer stays on the carrier. DMR receive uses the 12.5 kHz IF setting.

DMR transmit (`CONFIG_MD9600_DMR_TX`): the HR_C6000 glue's prepare hook runs steps 1-4 without the waits (TX VCO on the
carrier, receivers off) at the start of a transmission, and the task waits the synthesizer's transmit settle (step 4)
plus 1 ms. Each burst then, 0.7 ms after the RF TX interrupt (a timer, no waits in an interrupt): the band's
T5 on, 500 us,
RF_APC_SW on, the APC reference up in 4 steps of 100 us; at its end the same ramp down, RF_APC_SW off, T5 off. The
end is timed too: the key timer starts the ramp down at the last symbol's end (27.5 ms after the first, from the RF TX
interrupt), and the carrier is off 0.85 ms after it, inside the slot's guard. The modem's timeslot interrupt comes
1.85 ms after the last symbol: dropped there, the carrier stayed at full power 0.6 ms into the slot after and was off
2.0 ms after the last symbol (SDR bench, the three lowest levels). The interrupt still ends a burst the timer did not
(an SFR's, a BS outbound's). T5 alone leaks the carrier about 13 dB under full power, and that counts as on air: keyed at the
interrupt itself (about 1.3 ms ahead of the slot), every burst was 31.7-31.9 ms long at floor + 10 dB on the SDR bench
(2026-11-06, 461.4625 MHz, lowest power; the bench allows 27-31.5 ms). Delayed, full power still comes about 0.2 ms
after the slot boundary, inside the guard ahead of the payload. The split duplex TX half (a BS outbound) keys once and
has no guard: its slot starts with the CACH. Its key-up is timed from the slot clock instead, full power 6 ms ahead of
a timeslot interrupt, after that slot's sync (`docs/spec/modem-hrc6000.md` 9.1). The receiver comes back at the last burst's PA
drop when the engine says no burst follows (`stm32_hrc_dmr_tx_last_out`), else at the slot engine's TX done, on every
channel, with the receive column's synthesizer, front-end and line steps and no waits. Back only at TX done, the
receiver was set up a slot later, as an answer starting at our last burst's end came in. On a split channel a
wake-up then hears the repeater's answer (left on the carrier, every key-up to a repeater that was down ended in
`no_repeater`). On simplex the reply to a confirmed packet comes in the next frame, 30 ms after the last burst, before
the radio task ends the transmission. That end goes through the modem glue's shared path (`docs/spec/dmr-modem.md`
6.4): with the receiver back from TX done, the board's `tx_end` keeps the synthesizer and only sets the front-end tune
again (on VHF it shares the DAC with the PA drive, which the end of the transmission zeroes); `rx()` is a full set-up. An SFR receives between its bursts
(below).

SFR (`CONFIG_SFR`; the transmission receives between its bursts, `docs/spec/sfr.md` 3 item 1, gotchas 14.6): at each
burst's end, after the ramp down, the synthesizer goes back to the receive LO (receivers on, RX VCOs, the words, the
front-end DAC, FM_SW off; on V4A / V5 the IF routes) without waits; the IF chip keeps its set-up. RF_TX_INTER moves to 1.5 ms ahead of the slot
(HR_C6000 0x12 = 15, back to 0 when the transmission ends). There the TX retune runs, then 0.6 ms for the lock, then
the burst's T5 and ramp. The original firmware never retunes between bursts, so it gives no figure for this; the
0.6 ms holds for V1-V3 only, and V4A / V5 needs its own sweep. Swept on air (V1-V3, UHF 446.125 MHz, 5 W, 2026-11-03), the bench's 3-superframe call
repeated by the MMDVM SFR:

| Lead, settle | Voice repeated | Corrected bits by voice frame (1st/2nd/3rd) |
|---|---|---|
| 3.0 ms, 2 ms | 12-18 of 18 | 35-59 / 0-12 / 42-56 |
| 2.0 ms, 1 ms | 18 | 71 / 0 / 36 |
| 1.5 ms, 0.6 ms | 12-18 of 18 | 53-67 / 0-6 / 0-6 |
| 1.5 ms, 0.6 ms, the burst ended 0.6 ms before the timeslot interrupt (TIM5, 2026-11-06) | 27 bursts (4 superframes) | 37 / 48 / 48, 16 of 69 frames lost: worse, reverted |

The call goes up whole (18 voice frames and the terminator). About 15-25% of the inbound AMBE frames fail their C0
check, and the first frame of each burst carries most of the corrected bits: the receiver's recovery after the drop at
the slot start. Keeping the receivers' supplies on while transmitting changed nothing measurable. One run in three
repeated two superframes of three at any setting (not explained yet).

Status (2026-11-03): SFR on V1-V3 is not fully supported yet. The HIL repeater-quality run (4 superframes, 72 inbound
voice frames) repeats 27 bursts and passes the slot, CW ID and user-role checks, but loses 13 of 72 inbound frames
(about 18%; the limit is 3), with 64 of the 76 corrected bits in the first frame of each burst. The SKY72310's charge
pump is already at its maximum (register 6 gain 0x1F) and it has no fast-lock mode, so the receiver's recovery after
each outbound burst is set by the loop filter. Next step: scope the synthesizer's lock detect and the receive AGC after
a burst to tell the relock from what follows it.
Measured off air (UHF, 5 W, 2026-11-03): T5 left on between bursts leaks the carrier at -13 dB, hence T5 per burst. With
it, between bursts the carrier is below the measuring receiver's floor (26 dB down). On air at key-up: a 0.5 ms step to
-13 dB (T5), then full power 0.4 ms later; key-down takes 0.4 ms. On-time 30.7 ms, same-slot spacing 60.0 ms, outer
symbol deviation 1933 Hz, every burst of a 3 s call decoded (headers, 9 superframes, terminator).

The key-up follows the key-up rule ([pa-keyup.md](pa-keyup.md)): the rule's points run on the key timer, with the
band's T5 supply 500 us ahead of the loop. Neither band's loop has a knee: on a conducted path every level reaches
-3 dB 0.5-0.75 ms after the loop closes. So the board's constants have no boost, only the loop's point per band
(`PA_KEYUP_FIXED`, [pa-keyup.md](pa-keyup.md) 2). The debug link's `radio pa-keyup` changes them for tuning.

| Band | Loop closed before the first symbol | Power | -3 dB before the first symbol | Overshoot |
|---|---|---|---|---|
| VHF (146 MHz) | 0.94 ms | 422 mW / 681 mW / 3.3 W / 18.8 W | 0.27 / 0.23 / 0.20 / 0.27 ms | none (first symbol up to 1.4 dB low) |
| UHF (445 MHz) | 0.85 ms | 484 mW / 982 mW / 4.2 W / 19.1 W | 0.34 / 0.29 / 0.18 / 0.21 ms | 0.2 dB or less |

With the earlier single point (1.35 ms), each level was up 0.6-0.85 ms early, on the carrier alone.

At power-up, with the revision known: receivers on, then on V1-V4 the AK2365A reset (PE15 low for 10 us), and on V4
the VHF synthesizer set for 145 MHz receive. On V4A / V5 the LMX2571 reset goes to both chips, then 10 ms; PE15 stays
an input.

Transmit is refused unless all hold: the build has `CONFIG_MD9600_TX` (on by default); a person armed it for this
power cycle (Settings > Advanced > Hardware > Transmit, or HWOPT; every start is unarmed); the revision comes from a
stored record; the frequency is in a band; the calibration's PA rows are present at every point and rise with the
power level (11.2). A refusal or a bus failure leaves the PA loop and both T5 off and the receiver back on. Disarming
drops the PA at once.

Calibration as the RF code uses it (11.2, all unverified): front-end tuning, PA drive per level (5 W row 0x20, 10 W
0x1A0, 25 W 0x190, 40 W 0x10, VHF 9 bytes later), squelch level 1 and level 9 opening thresholds. DAC values and noise
thresholds are the byte x 16. The noise reading maps linearly onto the core's squelch scale through the two
thresholds: level 1's maps to 64, level 9's to 40. Without them it is the 12-bit reading / 16. RSSI uses the curves of
5.7. The diagnostics page shows which row groups each band accepts ("calrf V RPS U RPS"), the sequencer state, the
arm and the last refusal.

### 19.9 Other RF-side lines

| Line | Use in RadioFoundry |
|---|---|
| PA2, ADC IN2 (QT_DQT_IN, the sub-audio low-pass output) | the CTCSS / DCS decoder (`subtone_md9600.c`, `CONFIG_SUBTONE`): ADC2 converts it continuously (480 cycles, 18.3 kHz) into DMA2 stream 3, and each half-transfer interrupt feeds sums of 8 to `drivers/rf/subaudio` at 2287 Hz. Mid-supply (about 1950 counts). A 100.0 Hz tone on a narrow channel holds it at about 320 counts of amplitude, talking or not; an empty channel gives 300-600 counts rms of noise. The line is AC coupled: a DCS stream on it is high-passed with a corner near 18 Hz, so a run of equal bits sags to under half within a bit (the decoder slices with hysteresis for this). The diagnostics page's "qt" is one ADC3 conversion of it |
| PC3, ADC IN13 (RX_2T_5T) | The MCU's only copy of FM receive audio (`disc_md9600.c`). One capture, two readers with their own place on its ring: APRS receive (`afsk_md9600.c`, `CONFIG_APRS_RX`, while the core listens) and FM receive audio on the PCM stream's input (`c6000_md9600.c`, `NET_USRP`, while the stream runs in FM: taken to 8 kHz by `drivers/audio/resample`, gated by the core's squelch, silent in transmit, level `MD9600_FM_RX_GAIN_Q8`, set to the DM-1701 FM tap's level for the same signal (1.5 kHz peak deviation at 1 kHz: about 3500 rms, UHF, 2026-11-06)). While either runs, ADC3 converts it continuously (480 cycles, 18.3 kHz) into DMA2 stream 1, and each half-transfer interrupt queues sums of 2 (9146 Hz) with the DC taken away; the diagnostics page's ADC3 reading of PA2 holds its last value meanwhile. Voice-band audio with sub-audio removed: under a carrier with a 100.0 Hz tone it drops to about 16 counts rms and rises to about 400 with speech; 450 counts of noise on an empty channel |
| PC7, TIM8 CH2 | sub-audio tone PWM (`md9600_pwm_out`), on the backlight's 28 kHz timebase, into the reference oscillator's modulation input. Without a tone the pin is released (open drain high), as at start-up: driven low it pulls the reference and DMR never syncs. In FM transmit TIM8's update interrupt (priority 6) feeds `drivers/rf/subaudio`'s generator a new duty every 8 periods (about 3.6 kHz). The tone's share of the swing is set per band for about 375 Hz deviation at 12.5 kHz and doubled at 25 kHz (`MD9600_SUBTONE_PERCENT_UHF` 17, `_VHF` 24; at 30 % UHF gave 665 Hz and VHF 478 Hz). Measured off air (2026-11-04): CTCSS 100.0 Hz at 368 / 754 Hz (UHF, 12.5 / 25 kHz) and 380 / 758 Hz (VHF); DCS D023N at +-385 Hz (UHF) and +-399 Hz (VHF), with a 0.30 s 134.4 Hz tail. The path inverts: DCS goes out with the generator's polarity flipped (`MD9600_SUBTONE_TX_INVERT`) (first radio, 2026-11-03) |
| PC8, TIM8 CH3 | The beep PWM net reaches the HR_C6000's MIC_P / MIC_N and LINEIN1: a 1 kHz square wave on it while keyed goes on air as a 1 kHz tone (784 Hz rms deviation with MIC_P / MIC_N on, 442 Hz with LINEIN1, 8 Hz with neither; UHF, 2026-11-05). FM transmit audio from a host (`NET_USRP`, `fm_tx_inject` in `c6000_md9600.c`) is PWM on it (`md9600_pwm_audio` in `pwm_md9600.c`): TIM8's 28125 Hz timebase stays the backlight's, DMA2 stream 2 rewrites CCR3 every period, and 225 periods take 64 samples (8 ms) linearly interpolated with the rounding error carried. MIC_P / MIC_N on and LINEIN1 off for the over; the mic gain stays (4:0xE4 is not monotonic: gain 19 gave a quarter of gain 7). Full scale is the whole range. Off air at 1 kHz: half scale 675 Hz rms, quarter 337, eighth 173 (linear), full scale 989 Hz rms (the limiter); 400 Hz and 2.5 kHz follow the pre-emphasis; 15-16 dB SINAD, as on other HR_C6000 radios' PWM path. `fm_tx_nominal_peak` 26000 (about 1.5 kHz peak). The audio probe's `pin_pwm` runs a sine through the same path |
| PA temperature, fan | no sensor or fan line is known on any revision (?) |

#### 19.9.1 VHF transmit

Checked off air (V1-V3, 151.5125 MHz, 5 W into a dummy load, 2026-11-03), the bench board's receiver retuned to VHF:

| Check | Result |
|---|---|
| FM, 12.5 kHz, CTCSS 100.0 | carrier -31 Hz; CTCSS 100.02 Hz at 380 Hz deviation (478 Hz before the per-band level) |
| FM, 25 kHz, CTCSS 100.0 | carrier -20 Hz; CTCSS at 758 Hz deviation |
| FM, DCS D023N | decoded, +-399 Hz deviation; 134.4 Hz turn-off tail 0.32 s |
| DMR, CC 1, slot 1, 4 s | voice header, 11 superframes and the terminator: 70 bursts, no sync errors, 60.0 ms apart, 1.83-1.97 kHz deviation, carrier +46 Hz |
| Level per power step (relative, leakage from the dummy load) | 10 W +2.7 dB, 25 W +4.0 dB, 40 W +8.9 dB over 5 W (nominal +3.0 / +7.0 / +9.0); needs a power meter |

### 19.10 Build switches

The board's own switches, in `boards/md9600/board.mk` and `config.mk` (the shared ones:
[build-config.md](../build-config.md)):

| Switch | Values |
|---|---|
| `MD9600_REV_DEFAULT` | `v5` (default), `v4` or `v1v3`: the board revision driven when none is stored and the probe gives no verdict (19.3) |
| `MD9600_REV_PROBE` | `1`: the receiver probe at start-up picks the revision when none is stored. Off (`0`) until its thresholds are measured |
| `CONFIG_MD9600_V1V3` / `_V4` / `_V5` | `y` (default) or `n`: the synthesizer driver per board revision: V1-V3 (SKY72310 and the AK2365A IF receivers), V4 (the 24-bit "PLL1590" part and the AK2365A), V4A / V5 (LMX2571). All on, one image runs every revision; fewer make a smaller image for those boards. At least one must be on (19.3) |
| `CONFIG_MD9600_TX` | `y` (default) or `n`: the transmit sequence (`boards/md9600/rfseq_md9600.c`). Built, transmit is still refused until it is armed for the power cycle (Settings > Advanced > Hardware, or the debug link) on a radio whose board revision is stored, and the PA calibration rows look plausible (19.8) |
| `CONFIG_MD9600_DMR_TX` | `y` (default) or `n`: DMR transmit, the per-burst RF keying of 19.8 from the HR_C6000 glue. Needs `CONFIG_MD9600_TX`, and keys only while transmit is armed |

The 16.8 MHz TCXO has no control line (5.2), so the reference trim (`REF_TRIM`) programs each synthesizer for
f x (1 + trim) at every tune and burst, the IF unchanged (`rfseq_md9600.c`; radio-features.md 6.1).

Feature switches that differ from the platform's defaults (`config.mk`; the two lists:
[build-config.md](../build-config.md#unsupported-not-written-unconfirmed)):

| Off by default | Why |
|---|---|
| `NET_PPP` (maximal build too: `MAXIMAL_SKIP`) | the radio has the USB network adapter; PPP is for radios with only a UART |
| `LANG_ES` and what follows it | English only |
| `UI_COLOR`, `FONT_XL`, `FONT_AA`, `FONT_BIG`, `AM_RX`, `BCAST_FM`, `VIBRATE`, `NAND_STORAGE`, `FAULT_TRACE` (maximal build too: `BOARD_UNSUPPORTED`) | a 128x64 monochrome screen; no air band or broadcast receiver; no vibration motor; NOR data flash; no UART for a fault trace |
