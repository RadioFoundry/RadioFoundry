<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The OpenDigitalRadio Authors -->

# TYT MD-380 hardware (MD-390, Retevis RT3, and the "G" GPS models)

The TYT MD-380, the MD-390 (the same board in an IP67 case) and the Retevis RT3 are one board family: an STM32F405VGT6
MCU, an HR_C5000 DMR baseband and codec chip, no RF transceiver chip but a Skyworks SKY72310 fractional-N synthesizer,
separate receive and transmit VCOs, a GT3136 FM IF receiver, a discrete PA with an analog power loop, a 160 x 128
color LCD on the FSMC 8-bit bus, a SPI NOR flash, a 3-row keypad matrix sharing its columns with the LCD data bus, a
16-position channel switch, an analog volume knob with the power switch, and USB full speed on the MCU's OTG_FS core.
Each radio is single band, UHF or VHF. The "G" models (MD-380G, MD-390G, RT3 GPS) add a GPS module on a UART.

Status (2026-10-10): **nothing in this file has been checked on a radio.** ODR builds for the board
(`boards/md380`) from these facts; [md380-bringup.md](../testing/md380-bringup.md) is the order in which a radio
confirms or corrects them, and section 17 lists every open question.

Conventions: register values are hex. HR_C5000 registers are written as `reg = value` on its configuration page
(page 0; the driver keeps the HR_C6000's page numbers and maps them, section 4.3). SKY72310 registers are its 4-bit
addresses. Every fact carries a confidence tag as in [radios.md](radios.md):

| Tag | Meaning |
|---|---|
| **C** | Confirmed: manufacturer data, a datasheet, or our own bench work on the radio |
| **D** | Documented by a reverse-engineering project (schematic notes, pin tables, hardware pages) |
| **F** | Forum post, review or retailer claim, or a value carried over from a sibling radio's notes |
| **?** | Not found, contradictory, or our own assumption. Needs a radio |

The sources are listed in section 18 and cited by the short names used there ([DL4YHF], [holger], [OpenRTX],
[PoC], [SKY], [ST], [miklor], [md380tools], [gotchas], [ref]). [gotchas] is [gotchas.md](gotchas.md) in this repo;
[ref] is the prose notes of the reference port (kept outside the repository), facts only.

---

## 1. MCU, clocks, memory

### 1.1 Part

STM32F405VGT6, LQFP100: Cortex-M4F, 1 MB flash, 192 KB RAM (128 KB SRAM, 64 KB CCM) (D) [PoC], [holger], [OpenRTX].
Read-out protection level 1 on stock radios (D) [PoC].

### 1.2 Clocks

8 MHz crystal on HSE (D) [OpenRTX]. ODR runs the DM-1701's clock plan: PLL M 4, N 72, P 2 (72 MHz SYSCLK), Q 3 (48 MHz
USB), AHB 72 MHz, APB1 36 MHz, APB2 72 MHz; the same frequencies the stock firmware is reported to use (72 / 36 /
72 MHz) (D) [OpenRTX]. If HSE does not start as a crystal, `board_early_init` retries it in bypass mode (?: the DM-1701
needed bypass) before staying on HSI. LSE: a 32.768 kHz crystal is assumed (?).

### 1.3 Internal flash layout

| Sectors | Addresses | Use |
|---|---|---|
| 0-2 (3 x 16 KB) | 0x08000000-0x0800BFFF | TYT bootloader, never written (D) [PoC] |
| 3-5, 6 up to 0x08050C43 | 0x0800C000-0x08050C43 | ODR application, 275 KB (`boards/md380/md380.ld`) |
| 6 from 0x08050C44, 7, 8, 10, 11 | see [codec-md380.md](codec-md380.md) | The stock D002.032 codec's code and table windows, run in place (C) |
| 9 | 0x080A0000-0x080BFFFF | Free |

The application starts at 0x0800C000 with its vector table there (VECT_TAB_OFFSET 0xC000) (D) [PoC], [md380tools].

The 275 KB hold every feature written for this radio only with link time optimization, on by default here, and the
MMDVM hotspot left out: [feature-matrix.md](../feature-matrix.md#dm-1701-and-md-380).

### 1.4 RAM

128 KB SRAM for data and every DMA buffer (minus 4 bytes at the top, as the DM-1701 keeps them for the boot request
word); 64 KB CCM for CPU-only state (`ODR_CPU_RAM`: the task stacks and the slot engine, as on the DM-1701, a76da0d;
main RAM use went from 89 % to 68 % of the default build with that move).

Sources: STM32F405 data sheet [ST]; PoC||GTFO 10:8 [PoC]; OpenRTX MD-380 page [OpenRTX].

---

## 2. Pin assignments

The pin tables come from two public sets of schematic notes, [DL4YHF] (from the RT3 / MD-380 service schematic) and
[holger], plus [OpenRTX]. [holger] warns its schematic "is known to be not correct" in places (F). Where the two
disagree the pin is marked (?). `boards/md380/pins.h` holds the same map.

### 2.1 Port A

| Pin | Function | Notes |
|---|---|---|
| PA0 | ADC1 IN0: volume knob position | (D) [DL4YHF]; [holger] lists the TX LED here (?) |
| PA1 | ADC1 IN1: battery voltage | (D) |
| PA2 | ADC1 IN2: QT_DQT_IN, sub-audio tone input | (D); stock decodes CTCSS / DCS in software (?) |
| PA3 | ADC1 IN3: VOX, mic level | (D) |
| PA4 | DAC1: APC / TV (PA power reference in TX, front-end tuning in RX) | (D) [DL4YHF]; the double use (?) |
| PA5 | DAC2: MOD2_BIAS (reference oscillator bias) | (D); value from [gotchas] 11.1 (F) |
| PA6 | Keypad row K1 | (D) |
| PA7 | POW_C, power latch | (D) |
| PA8 | SAVE (battery saving) | (D); function (?), left as an input |
| PA9 | VCOVCC_SW: RX or TX VCO supply | (D); polarity (?) |
| PA10 | DMR_SW: receiver IF to the HR_C5000 | (D); low in TX, high in RX (F) [gotchas] 2.6 |
| PA11 / PA12 | USB OTG_FS D- / D+ | (D) |
| PA13 | W/N_SW, IF bandwidth; also SWDIO | (D); polarity (?) |
| PA14 | MICPWR_SW, microphone bias; also SWCLK | (D) |
| PA15 | I2S3 WS | (D) |

### 2.2 Port B

| Pin | Function | Notes |
|---|---|---|
| PB0 | ADC1 IN8: RSSI | (D) |
| PB1 | ADC1 IN9: BUSY, the receiver's noise squelch level | (D); polarity (?) |
| PB2 | FM_SW, FM receive path select | (D); polarity (?) |
| PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI, SPI flash (AF5) | (D) |
| PB6 / PB7 | I2C1 SCL / SDA | (D); nothing on it is known; unused |
| PB8 | SPK_C, speaker mute | (D); high = muted (?) |
| PB9 | AFCO, audio amplifier enable | (D); high = on (?) |
| PB10 / PB11 | Channel switch ECN2 / ECN3 | (D) |
| PB12 | HR_C5000 voice port chip select | (D), (F) [gotchas] 2.6 |
| PB13 / PB14 / PB15 | SPI2 SCK / MISO / MOSI, HR_C5000 voice port (AF5) | (D), (F) [gotchas] 2.6 |

### 2.3 Port C

| Pin | Function | Notes |
|---|---|---|
| PC0 | HR_C5000 timeslot interrupt (EXTI0) | (D) |
| PC1 | HR_C5000 system interrupt (EXTI1) | (D) |
| PC2 | HR_C5000 RF TX interrupt (EXTI2) | (D) |
| PC3 | ADC1 IN13: 2T/5T (FM discriminator audio) | (D); unused |
| PC4 | RF_APC_SW, PA power loop on | (D) |
| PC5 | 5TC, transmitter supply | (D) |
| PC6 | LAMP, LCD backlight (TIM8 CH1 PWM, AF3) | (D); PWM is our choice |
| PC7 | CTC/DCS_OUT, sub-audio tone output (PWM) | (D); unused |
| PC8 | BEEP (TIM8 CH3) | (D); unused |
| PC9 | 5RC, receiver supply | (D) |
| PC10 / PC11 / PC12 | I2S3 CK / ext SD / SD | (D); direction of the two data lines (?) |
| PC14 / PC15 | LSE crystal | (?) |

### 2.4 Port D

| Pin | Function | Notes |
|---|---|---|
| PD0 / PD1 | FSMC D2 / D3 (LCD data, keypad columns) | (D) |
| PD2 | Keypad row K2 | (D) |
| PD3 | Keypad row K3 | (D); carries the side keys (?) |
| PD4 / PD5 | FSMC NOE / NWE | (D) |
| PD6 | LCD chip select | (D) |
| PD7 | SPI flash chip select | (D) |
| PD8 | GPS module power on the G models; listed as a second flash chip select on others | (D), (F) [gotchas] 13.7 |
| PD9 | USART3 RX (AF7): GPS NMEA in on the G models; listed as a flash chip select on others | (?) |
| PD10 | SKY72310 lock detect, high = locked | (D) |
| PD11 | SKY72310 chip select | (D) |
| PD12 | FSMC A17, LCD RS (data / command) | (D) |
| PD13 | LCD reset | (D) |
| PD14 / PD15 | FSMC D0 / D1 | (D) |

### 2.5 Port E

| Pin | Function | Notes |
|---|---|---|
| PE0 | Green LED (RX) | (D) |
| PE1 | Red LED (TX) | (D) |
| PE2 | HR_C5000 configuration port chip select | (D) |
| PE3 | HR_C5000 configuration clock, also the SKY72310 clock | (D) |
| PE4 | HR_C5000 SDO -> MCU | (D) |
| PE5 | MCU -> HR_C5000 SDI, also the SKY72310 data | (D) |
| PE6 | DMR_SLEEP, HR_C5000 power down (high = down) | (D); polarity as the HR_C6000 (?) |
| PE7-PE10 | FSMC D4-D7 | (D) |
| PE11 | PTT, active low | (D) |
| PE12 | External PTT (accessory jack), active low | (D) |
| PE13 | FM_MUTE | (D); high = muted (?) |
| PE14 / PE15 | Channel switch ECN0 / ECN1 | (D) |

### 2.6 Reset-time levels and the SWD pins

`board_early_init` sets, before anything slow: the power latch (PA7) high; the HR_C5000 chip select, clock and power
down high; the SKY72310 chip select high; the transmitter supply, APC loop, receiver supply and VCO select low;
DMR_SW high; speaker muted, amplifier off, FM muted; LEDs off; keypad rows low; the flash chip select high. PD8 and
PD9 are left as inputs (section 13). These are our safe choices, not stock values (?).

PA13 and PA14 are the SWD pins. The radio needs them as the IF bandwidth switch and the microphone supply, so
`radio_md380.c` takes them over at radio init; a debugger then has to connect under reset. Build with
`MD380_DEFS=-DMD380_KEEP_SWD` to leave them alone: the microphone is then
unpowered and the bandwidth switch stays as the hardware pulls it.

Sources: [DL4YHF] pin table and schematic notes; [holger]; [OpenRTX]; [gotchas] 2.6 and 13.7.

---

## 3. Interrupts, DMA, timers

Our allocation, following the DM-1701 (`boards/dm1701`), since the same MCU pins carry the same HR_C5000 lines:

| Source | Use | Priority |
|---|---|---|
| EXTI0 / EXTI1 / EXTI2 (PC0-PC2) | HR_C5000 timeslot / system / RF TX interrupts (falling edge) | 5 |
| TIM7 | 30 ms one-shot after the RF TX interrupt: the burst's PA off (section 5.6) | 5 |
| DMA1 Stream0 ch0 / Stream5 ch2 | I2S3 receive / I2S3ext transmit (PCM, section 6.2) | 6 |
| DMA2 Stream4 ch0 | ADC1 continuous scan (circular, no interrupt) | - |
| USART3 | GPS NMEA receive (RXNE interrupt into a ring buffer) | 7 |
| TIM8 | LCD backlight PWM, CH1 on PC6 | - |
| OTG_FS | USB (platform) | platform |

Priority 5 is FreeRTOS's `configMAX_SYSCALL_INTERRUPT_PRIORITY` (`boards/md380/FreeRTOSConfig.h`): the modem ISRs
notify the DMR task, and a masked section at that level makes every configuration-port and synthesizer transaction
atomic.

---

## 4. HR_C5000

### 4.1 Overview

The HR_C5000 is the HR_C6000's predecessor in the same family: DMR layer 1/2 baseband, two-point modulation outputs,
an IF input from the receiver, the AMBE-less voice frame interface, and an audio codec with I2S (D) [PoC], [OpenRTX].
ODR drives it with the HR_C6000 driver (`drivers/modem/hrc6000`) in its C5000 variant, `hrc6000_attach_chip(...,
HRC6000_CHIP_C5000)`: the register map is close enough that the slot engine is shared, and the variant flag selects
the differences in 4.3. There is no public HR_C5000 data sheet; the differences come from [gotchas] 2.6, 5.2, 11.1 and
the reference port's notes [ref] (F).

### 4.2 Configuration port and the shared bus

Bit-banged SPI on PE2 (CS), PE3 (clock), PE5 (MCU -> chip) and PE4 (chip -> MCU) (D). The framing and timing are taken
to be the HR_C6000's (clock idles high, data latched on the rising edge, a few hundred ns per half bit; DM-1701
hardware-dm1701.md 4.2) (?).

The clock (PE3) and data (PE5) also go to the SKY72310, which has its own chip select (PD11) (D) [DL4YHF]. A
transaction on either chip is therefore one masked section (`md380_rfbus_lock`), so a modem ISR's register access never
lands inside a synthesizer word or the reverse.

### 4.3 Register differences from the HR_C6000

| Item | HR_C6000 | HR_C5000 | Driver |
|---|---|---|---|
| Configuration page | 4 | 0 | `hrc6000_page()` maps page 4 to 0 on every access (F) [gotchas] 2.6 |
| Reg 0x10 in DMR | 0x6E | 0x6B simplex, 0x4F on a repeater; never 0x6E | `hrc6000_dmr_mode_value()` (F) |
| 0x20-0x22 at TX start | access policy and vocoder control written | not written; 0x21 = 0x00 and 0x22 = 0x07 once at init | `hrc6000_tx_start_access()`, `hrc6000_power_on_init()` (F) |
| Reg 0x5F (sync / slot report) | read at every burst | absent: DMO / RMO come from the channel | `hrc6000_has_sync_reg()` (F) |
| Embedded LC LCSS | chip | MCU writes it in 0x50 for every voice burst: A-F = 0x08 0x19 0x2B 0x3B 0x4A 0x58 | `hrc6000_voice_lcss()` (F) |
| Own DMR ID 0x14-0x16 | big endian | 0x14 holds the low byte | `hrc6000_set_dmr_id()` (F) [ref] |
| CRC_MCU_Control (0x40 bit 3) | present | absent: for CSBK, MBC header and data header write 10 bytes and the chip appends the CRC | `hrc6000_has_mcu_crc()`, `hrc6000_chip_crc_len()` (F) [gotchas] 5.2 |
| TX / RX switch in DMR | full DMR table not needed | small sets, below | `hrc6000_c5000_dmr_path()` (F) [ref] |

The small DMR sets (F) [ref]:

- TX: 0x0D = 0x8C, 0x0E = 0x44, mic gain, 0x37 = 0xC2, 0x25 = 0x0E, 0x26 = 0xFE (RX IF and audio off), 0x45 / 0x46 =
  the calibrated DMR modulation pair, 0x35 = 0x1E, 0x3F = 0x04.
- RX: 0x0E = 0x44, mic gain, 0x37 = 0xC2, 0x25 = 0x0E, 0x26 = 0xFD.

The rule of [gotchas] 3.3 holds: the full DMR mode set-up runs once per mode change; per-burst switching only touches
the RF side. VHF radios swap I and Q with 0x01 = 0xF0 (F) [ref]; not written yet (?: confirm on a VHF radio which
value the receiver needs).

FM uses the HR_C6000 FM tables through the page mapping, then the C5000's own reference registers (4.5). Whether the
FM TX deviation table and the FM RX feed-through (0x36) mean the same on the C5000 is unconfirmed (?).

### 4.4 Voice port

Hardware SPI2 (PB13-PB15, AF5) with chip select PB12, commands 0x03 (write) and 0x83 (read) followed by 0x00 and 27
bytes (one voice burst's payload) (F) [gotchas] 2.6. ODR uses SPI mode 2 at 4.5 MHz as on the DM-1701 (?).

### 4.5 Reference oscillator

The calibration's reference tune byte t (section 11) sets regs 0x47 = 0x04 = 0x7F - t, 0x48 = 0x00 if t < 0x80
else 0x03, and DAC2 = t x 4 + 0x600 (F) [gotchas] 11.1. `hrc6000_c5000_set_ref()` and `hrc6000_c5000_ref_dac()`.

### 4.6 Power and start-up

PE6 low wakes the chip; ODR waits 12 ms before the first access, as for the HR_C6000 (?). `hrc6000_power_on_init` then
runs the HR_C6000 sequence through the page mapping, with the C5000's 0x21 / 0x22 values. After init the driver reads
0x10 back for the diagnostics page (bring-up stage 4).

Sources: [gotchas] 2.6, 3.3, 5.2, 11.1; [ref] notes (facts only); [PoC]; [OpenRTX].

---

## 5. RF

### 5.1 Bands and power

One band per radio: UHF 400-480 MHz or VHF 136-174 MHz, 1 W and 5 W (F) [miklor]. ODR builds one image per band
(`make BOARD=md380 MD380_BAND=uhf|vhf`, default UHF); the image's band table refuses the other band. Which band a
radio is can't be read from the hardware by any method we know (?: the calibration's frequency list, 11.2, is a
candidate for a run-time check).

### 5.2 SKY72310 synthesizer

Skyworks SKY72310-11 (D) [OpenRTX], [DL4YHF]. From the data sheet [SKY] (C):

- Serial interface: 16-bit words, MSB first: 4 address bits then 12 data bits, latched when CS goes high. Write only.
- Registers: 0 main divider (N_reg = floor(N) - 32, 9 bits, 6..505 in fractional mode); 1 dividend MSB (10 bits);
  2 dividend LSB (8 bits); 5 reference divider (R - 1, 5 bits); 6 phase detector gain (5 bits, 0.125-1 mA/rad);
  7 power down and Mux_out (bit 0 full power down, 1 synthesizer power down, 2 integer-N, 3 10-bit modulator, 8:6
  Mux_out source, 9 tristate); 8 modulation control; 9 modulation data; 14 delta-sigma (0x080, bit 5 signed mode).
- N = f_vco / f_pd with f_pd = f_osc / R <= 50 MHz; the 18-bit dividend is round(2^18 x frac(N)) in unsigned mode.
- A new ratio takes effect on the dividend MSB write: order divider, LSB, MSB.
- Lock detect high = locked (on the Mux_out / LD pin, wired to PD10 (D)).

`drivers/rf/sky72310` implements this and `tests/host/sky72310Test.c` checks it against a model of the chip. The
board picks the smallest reference divider that keeps N in the fractional range (2 or 3 at 16.8 MHz), the phase
detector gain at its maximum (?: the loop filter was designed for some value we don't know), Mux_out = the oscillator
(the lock detect output's configuration is not in our copy of the data sheet (?)).

### 5.3 Frequency plan

Assumed from the MD-9600, which has the same TCXO and synthesizer and whose notes give the IF (F) [radios.md MD-9600
section], [ref]:

| Item | Value | Tag |
|---|---|---|
| Reference | 16.8 MHz TCXO | (F), from the MD-9600 |
| First IF | 49.95 MHz | (F), from the MD-9600 |
| UHF receive | VCO = f - 49.95 MHz (LO below) | (?) |
| VHF receive | VCO = 2 x (f + 49.95 MHz), divided by 2 (LO above) | (?) |
| UHF transmit | VCO = f | (?) |
| VHF transmit | VCO = 2 x f | (?) |

`radio_md380.c` keeps the plan in `MD380_TCXO_HZ`, `MD380_IF_HZ`, `MD380_RX_LO_ABOVE` and `MD380_VCO_MULT`. A wrong
plan shows at once on the bench: no lock (diagnostics "pll UNLK") or a carrier off by a fixed amount (bring-up stage 4).

### 5.4 Receive chain and switching

Receive: VCOVCC_SW selects the RX VCO, the synthesizer is tuned, 5RC (PC9) powers the receiver, DAC1 sets the
front-end tuning voltage from the calibration (11.2), the GT3136 IF goes to the HR_C5000 with DMR_SW (PA10) high, and
FM_SW (PB2) selects the FM audio path in FM (D, polarities ?). W/N_SW (PA13) sets the IF filter for 12.5 or 25 kHz
(D, polarity ?).

Transmit: 5RC off, DMR_SW low, VCOVCC_SW to the TX VCO, the synthesizer to the TX frequency and locked, then 5TC
(PC5), DAC1 = APC drive, RF_APC_SW (PC4) on (D for the pins, order ours). FM TX waits for lock detect (5 ms at most)
and refuses to key without it.

### 5.5 RSSI, noise, squelch

RSSI is an analog level on PB0 and the receiver's noise level ("BUSY") on PB1 (D). Neither curve is known (?). ODR
reports rssi_dbm = -130 + raw x 90 / 4095 and noise = raw / 16 as placeholders, and shows both raw values on the
diagnostics page so a bench session with a signal generator can fit them.

### 5.6 PA and power control

DAC1 is the APC reference, scaled from the calibration's power rows (11.2). Two levels, 1 W and 5 W. TX is refused
without an accepted calibration table (no safe uncalibrated drive is known).

DMR: the PA follows the bursts. The RF TX interrupt keys it (at the first burst of a transmission the receiver is
switched off and the synthesizer moved to the TX frequency first, without waiting for lock; the interrupt precedes the
burst by some milliseconds (?)). A TIM7 one-shot drops it 30 ms later, as the stock firmware is reported to do
(F) [gotchas] 3.4; the slot engine's PA-off at the next timeslot interrupt stays as the fallback. The synthesizer
returns to RX at `tx_off()`.

Sources: [SKY] (C); [DL4YHF], [OpenRTX] (D); [miklor] (F); [radios.md](radios.md) MD-9600 notes and [ref] for the
frequency plan (F); [gotchas] 3.3, 3.4.

---

## 6. Audio

### 6.1 Paths

FM receive audio and DMR voice both reach the amplifier through the HR_C5000 in ODR's model: FM through its
feed-through (0x36) behind FM_MUTE (PE13), DMR through its codec DAC; SPK_C (PB8) mutes the speaker, AFCO (PB9) turns
the amplifier on (D for the pins; the routing (?)). The GT3136's discriminator audio also reaches the MCU on PC3, which
ODR doesn't use.

The volume knob sets the level in the analog path; the MCU only sees its position on PA0. The board therefore has no
`CAP_VOLUME_POT` and `hal_power_t.volume()` returns -1.

### 6.2 I2S PCM

I2S3 full duplex, slave to the HR_C5000's codec: WS PA15, CK PC10, SD PC12 (chip -> MCU) and ext SD PC11 (MCU ->
chip), Philips, 16-bit samples in 32-bit channels, 8 kHz (D for the pins; format and directions copied from the
DM-1701 (?)). `audio_md380.c` is the DM-1701's driver.

### 6.3 Microphone

Bias from MICPWR_SW (PA14), on whenever the radio runs (section 2.6). Mic level on PA3 for VOX (D).

### 6.4 Sub-audio

The stock firmware is reported to decode CTCSS / DCS in software from QT_DQT_IN (PA2) and to generate tones as PWM on
CTC/DCS_OUT (PC7) (D, details ?). Neither is written: `CONFIG_SUBTONE` is off for this board, so channels with a tone
open on carrier and send no tone. The FM TX tones (1750 Hz, roger beep) and APRS AFSK are off for the same reason
(`config.mk`).

---

## 7. Display

160 x 128 color TFT on the FSMC 8-bit bus (NE1), command at 0x60000000 and data at 0x60040000 (A17 = RS on PD12),
chip select PD6, reset PD13: the DM-1701's wiring (D) [OpenRTX], [DL4YHF]. The controller is reported as an HX8302A
(D) [OpenRTX], [DL4YHF] or "maybe an ILI9481" (F) [holger]; RT3 and MD-380 panels are said to be "slightly
incompatible" (F). ODR drives it with `drivers/display/st7735` (the HX8302A takes the ST7735 command set (?)) with
every row visible (no inset) and two build-time knobs for the bench: `MD380_PANEL_TYPE` (the st7735 driver's panel
type, default 0) and `MD380_PANEL_MADCTL_XOR` (orientation bits flipped, default 0). Backlight: LAMP (PC6), TIM8 CH1
PWM at 1 kHz (D for the pin; PWM ours).

---

## 8. LEDs

Green (RX) on PE0, red (TX) on PE1, active high (D, polarity ?).

---

## 9. Keypad, side keys, channel switch, PTT

### 9.1 Matrix

Rows K1 PA6, K2 PD2, K3 PD3, driven high one at a time; columns are the LCD data lines D0-D7, read under the LCD bus
lock as on the DM-1701 (D for the pins). The key positions are assumed to be the MD-UV380's, the nearest documented
relative (?):

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| K1 | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| K2 | Green (menu) | Up | Down | 7 | 8 | 9 | # | Red (back) |
| K3 | - | - | - | - | - | - | SK2 | SK1 |

The diagnostics page shows the raw row bits ("keys rr rr rr"), so one press of each key on a radio fills in this table.

### 9.2 Channel switch

A 16-position switch with four code lines ECN0-ECN3 on PE14, PE15, PB10, PB11 (D), not a quadrature encoder.
`input_md380.c` reads the code at every 10 ms scan and reports position changes as encoder steps (the shorter way
round across the stop). Pulled up with closed contacts reading low, plain binary, by default; `MD380_CH_ACTIVE_HIGH`
and `MD380_CH_GRAY` switch both assumptions (?). The diagnostics page shows the raw code ("ch x").

### 9.3 PTT

PTT on PE11 and the accessory jack's PTT on PE12, both active low with pull-ups (D).

---

## 10. Power and analog inputs

### 10.1 Power switch and latch

The volume knob carries the power switch. On the MD-380 it is believed to switch the supply directly, with POW_C
(PA7) as a latch the firmware holds (D for the pin; the switch wiring (?)). `switch_on()` therefore always says on, and
power-off from the menu drops the latch.

### 10.2 Battery

7.4 V 2S Li-ion pack (F) [miklor]. ADC1 IN1 through a divider: ODR assumes 40.3 counts per 0.1 V
(`MD380_BATT_COUNTS_X10`) (?). Low 6.2 V and critical 6.0 V are ODR's choices, as on the DM-1701.

### 10.3 ADC scan

ADC1 scans IN0 (volume), IN1 (battery), IN3 (mic), IN8 (RSSI), IN9 (noise) and IN16 (temperature sensor) continuously at
480 cycles each into a DMA ring of 8 scans; readings are averages over the ring (`power_md380.c`).

---

## 11. Calibration

### 11.1 Location

512 bytes in the SPI flash's security registers 1 and 2 (flash addresses 0x1000 and 0x2000, 256 bytes each) (F)
[gotchas] 11.1, [ref]. The reference port once read a third 256-byte block into a 512-byte buffer (F) [gotchas] 11.1;
ODR reads exactly two. Read only: ODR never writes them.

### 11.2 Layout

Assumed to follow the MD-UV380 family's first rows (the same factory tool), single band, 9 points per row (?):

| Offset | Content |
|---|---|
| 0x009 | Reference oscillator tune t (section 4.5) |
| 0x010-0x018 | TX power, high (5 W), per point |
| 0x020-0x028 | TX power, low (1 W), per point |
| 0x030-0x038 | Receiver front-end tuning, per point |
| 0x130-0x138 / 0x140-0x148 | DMR modulation pair (0x45 / 0x46) |
| 0x170-0x178 / 0x180-0x188 | FM modulation pair |
| 0x1B0-0x1D3 | The 9 calibration frequencies, 4-byte BCD, least significant byte first, 10 Hz units |

Byte values are taken x 16 to the 12-bit DACs (?). `calibration_md380.c` accepts the table when the power rows are
neither erased nor zero and low < high at every point, and no tuning or modulation row is erased. If the frequency list
is not BCD, ascending and in band, it uses an even grid over the band instead and says so ("factory/grid").

### 11.3 Interpolation

Linear between the two neighboring points, clamped at the ends.

---

## 12. SPI flash

### 12.1 Device and bus

Winbond W25Q128FV, 16 MB, on SPI1 (PB3-PB5, AF5) with chip select PD7, mode 0, 4.5 MHz (D) [OpenRTX], [DL4YHF]. Some
early VHF radios have a 1 MB part (D) [md380tools]. `flash_md380.c` accepts any W25Q (JEDEC type 0x40) and probes the
size.

### 12.2 Map

On a 16 MB part ODR uses the DM-1701's map (settings 0x011000, journal 0x019000, legacy codeplug 0x000000 and
0x020000-0xE00000, native area 0xE00000, TFTP staging 0xF00000), so the legacy CPS works the same. The stock TYT codeplug at 0 is overwritten
by the first legacy write: read it out with the stock CPS first if it matters ([md380-migration.md](md380-migration.md) does that, converts it with qdmr and writes it back). There is no calibration window in the
main array (11.1). On a smaller part the core gets no areas: settings live in RAM and nothing is written.

---

## 13. GPS

### 13.1 Module

The G models carry a GPS module whose NMEA output goes to the MCU's USART3 (D for the module; pins (?)). The module is
named JS-H210 on [OpenRTX]'s MD-380 page and JS-M710 in its hardware table (?). 9600 baud is assumed (?).

### 13.2 Detection

The same board ships with and without the module, and PD8 (the module's power switch on G models) is listed as a
second flash chip select on others (F) [gotchas] 13.7. So nothing drives PD8 until the module has been heard:
`gps_md380.c` enables USART3 RX on PD9 at start-up and listens for 5 s. One checksum-valid sentence (any talker, up to
82 characters plus CR LF; `drivers/gps/nmea_line`) makes the module present: PD8 is then held at the level it already
had, and every sentence goes to the APRS position source (`aprs_nmea`, CONFIG_APRS). Nothing heard: the UART is
switched off and `board.gps_present()` stays false, so `board_has_gps()` hides the GPS features. UART errors are
counted and cleared in the interrupt, so one framing error never stops reception ([gotchas] 13.7).

---

## 14. Bootloader, firmware images, codec

### 14.1 Stock firmware file

TYT ships the application as an encrypted "OutSecurityBin" container, the same family as the DM-1701's (D) [PoC].
The file examined here (C, our own reading of the container header only):

| Item | Value |
|---|---|
| File | `MD-380-D2.32(AD).bin` (TYT "Firmware 2.32") |
| SHA-256 | e1a1a11a84d9afd2d2ceed61be8395e32a8034785abd0be0236478f7d8609f43 |
| Size | 0xF2E00 bytes |
| Model string | "JST51" |
| Segment count word (0x7C) | 0xFFFFFFFF |
| Segment table | one entry: 0x0800C000, 0xF2C00 |
| Payload | 0x100 + 0xF2C00, then 0x100 trailing bytes |

`odrflash/stockfw.parse_container` reads this single-segment form (count 0xFFFFFFFF = one segment in the first
table entry).

### 14.2 Encryption and key

The payload is XOR-encrypted with a 1024-byte key held in the bootloader (D) [PoC]. The key's SHA-256 is
5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd and its first bytes 2E DF 40 B5 (D,
`tools/stm32_dfu.py` md380 model). It can be derived from the D2.32 stock file and checked by that hash (C,
2026-10-10): the plain column vote is wrong in a third of the columns, and a byte-pair refinement learned from the file
fixes them ([codec-md380.md](codec-md380.md), "Key recovery"; `odrflash/stockfw.derive_key`). A key file still works
(`--key`).

### 14.3 Bootloader and DFU

Entered by holding PTT and the upper side key while switching on (D) [md380tools]. USB DFU 0483:DF11 with TYT's
non-standard commands; the same erase-by-sector and block-write protocol as the DM-1701's loader (D) [PoC]. Flash an ODR
image with `tools/stm32_dfu.py --model md380 --key KEY odr_md380.bin`. The image is plain, starts at 0x0800C000, and the
tool encrypts it for the loader. odrflash knows the board (`boards.py` MD380: not releasable, codec sectors 7-11 kept).

### 14.4 The codec

The AMBE+2 codec is inside the stock application (D) [PoC]. Located in D002.032 (C, 2026-10-10): the same library as the
DM-1701's, at other addresses; code window 0x08050C44+0x10884, table window 0x0808FC64+0xAFF8 and four small table
windows in sectors 10 and 11, run in place. Entry points, calling convention, state sizes and the extraction recipe
are in [codec-md380.md](codec-md380.md). `stockfw.PROFILES` has the profile (`md380-d2032`), `boards.py` the windows,
`md380.ld` ends the application at the code window, `vocoder_md380.c` hosts it, and `make BOARD=md380` merges it when
the stock file is in `codecs/`. Without the codec merged the radio is FM only.

---

## 15. USB

OTG_FS device on PA11 / PA12 (D). ODR enumerates with the DM-1701's IDs and the product name "MD-380"
(`usb_md380.c`); the bootloader's DFU device is separate (14.3).

The legacy CPS shim reports radio type 7 and serves the MCU ROM from 0x08000000, 1 MB (`boards/md380/board.c`,
`cps-protocol.md` 2.2).

---

## 16. Variants

| Radio | Difference | Tag |
|---|---|---|
| MD-380 | the base board | (D) |
| MD-390 | the same electronics in an IP67 case | (F) [miklor] |
| Retevis RT3 | a rebadged MD-380; its panel is said to be slightly incompatible | (F) [holger] |
| MD-380G / MD-390G / RT3 GPS | GPS module on USART3, power on PD8 (section 13) | (D, pins ?) |
| UHF / VHF | separate boards: different RF parts and VCOs, one ODR image each | (D) |
| 1 MB flash | some early VHF radios; ODR stores nothing on them | (D) [md380tools] |

---

## 17. Unknowns that need a radio

Each with how to settle it; the stage numbers are [md380-bringup.md](../testing/md380-bringup.md)'s.

| # | Unknown | How to settle it on a radio |
|---|---|---|
| 1 | HSE crystal or external clock | Stage 1: the radio boots on HSE (USB enumerates); if only bypass works, record it |
| 2 | Display controller, orientation, colors | Stage 2: try `MD380_PANEL_TYPE` / `MD380_PANEL_MADCTL_XOR` until the boot screen is right side up and in color |
| 3 | Key matrix positions, side keys on K3 | Stage 3: press each key, read "keys" on the diagnostics page, correct section 9.1 and `input_md380.c` |
| 4 | Channel switch code polarity and order | Stage 3: turn through all 16 positions, read "ch"; set `MD380_CH_ACTIVE_HIGH` / `MD380_CH_GRAY` |
| 5 | Volume knob on PA0 vs the TX LED | Stage 3: "vol" on the diagnostics page follows the knob, and the red LED lights on PE1 |
| 6 | Battery divider | Stage 3: compare "bat raw" against a meter; set `MD380_BATT_COUNTS_X10` |
| 7 | Power switch wiring (direct or sensed) | Stage 3: switch off with the knob: does the radio die at once? |
| 8 | Flash size and JEDEC ID | Stage 3: "flash" line |
| 9 | Calibration layout and scaling | Stage 4: "cal" must say factory; read both security registers over the legacy CPS protocol (area 10, which `md380_cps_security_read` serves) and compare the rows with what the stock CPS shows |
| 10 | Frequency plan (TCXO, IF, LO side, VCO multiplier per band) | Stage 4: "pll lock" on every channel; an SDR on a 25 kHz-spaced pair of channels shows the receive LO leak and TX carrier |
| 11 | SKY72310 bus timing and lock detect polarity | Stage 4: "pll lock" with a correct carrier; a scope on PD10 while retuning |
| 12 | VCOVCC_SW, FM_SW, W/N_SW, DMR_SW, FM_MUTE, SPK_C, AFCO polarities | Stage 4/5: receive a known FM signal; invert one pin's level at a time in `radio_md380.c` (one rebuild each, batched) until IF, audio and bandwidth are right |
| 13 | RSSI and noise curves | Stage 5: signal generator steps, record "rssi" and "busy", fit `status()` |
| 14 | HR_C5000 configuration port timing, page mapping, reg 0x10 read-back | Stage 4: "C5000 10=" shows a sane value after init |
| 15 | FM TX deviation and modulation balance on the C5000 | Stage 6: TX into a dummy load and an SDR: deviation and frequency against the stock firmware |
| 16 | APC scaling (table x 16) and the two power levels | Stage 6: power meter on 1 W and 5 W at band edges and center |
| 17 | I2S data line directions and format | Stage 7: DMR receive with the codec merged (after 14.4); a silent or noisy speaker means a direction or channel problem |
| 18 | RF TX interrupt lead time and the 30 ms PA hold | Stage 8: scope the PA supply (PC5) against the burst on an SDR |
| 19 | VHF I / Q swap (0x01 = 0xF0) | Stage 8 on a VHF radio: DMR receive and transmit against a known radio |
| 20 | GPS UART pin, baud rate, power pin polarity | Stage 9: on a G model "gps yes" with counts rising; on a non-G model "gps no" and no flash errors |
| 21 | Codec windows in D002.032 | Located and verified in an emulator (14.4); stage 7 checks voice on the radio |
| 22 | Whether the bootloader key can be derived from a stock file | Done (14.2): derived from D2.32 and checked by its hash |

## 18. Sources

| Short name | Source | Kind |
|---|---|---|
| [DL4YHF] | Wolfgang Buescher, "MD380 / RT3 hardware" notes, https://qsl.net/d/dl4yhf/RT3/md380_hw.html | Schematic notes, pin table (D) |
| [holger] | Holger Schurig, MD-380 schematic notes, https://github.com/holgerschurig/md380 (prose and tables only) | Schematic notes (D, with its own warning) |
| [OpenRTX] | OpenRTX hardware pages, https://openrtx.org/#/hardware/md380 and https://openrtx.org/#/hardware/hardware (prose only, no source) | Hardware description (D) |
| [PoC] | Travis Goodspeed, PoC\|\|GTFO 10:8, https://mcfp.felk.cvut.cz/publicDatasets/pocorgtfo/contents/articles/10-08.pdf | Article (D) |
| [SKY] | Skyworks SKY72310-11 data sheet 203639L, https://www.skyworksinc.com/Products/PLLs-Synthesizers-VCOs/SKY72310-11 | Data sheet (C) |
| [ST] | STMicroelectronics STM32F405xx data sheet and RM0090 | Data sheet (C) |
| [miklor] | https://miklor.com/MD380/380-Specs.php | Specifications (F) |
| [md380tools] | md380tools README prose, https://github.com/travisgoodspeed/md380tools | Project notes (D) |
| [gotchas] | [gotchas.md](gotchas.md) 2.6, 3.3, 3.4, 5.2, 11.1, 13.7 | This repo |
| [ref] | the reference port's prose notes, outside the repository (not the hunk code) | Facts from the reference port's notes (F) |

No GPL source, no disassembly and no binary of another project were read for this file. The stock D2.32 file was only
hashed and its container header read (14.1).

## 19. Sizes

Flash: the table in [feature-matrix.md](../feature-matrix.md#dm-1701-and-md-380) and the per-feature figures in
[feature-matrix.md](../feature-matrix.md) (the default UHF image, with LTO, leaves about 2 KB of 281,668 bytes).

Obsolete (2026-10-10, before the 275 KB limit of `md380.ld`, LTO and the CCM move): default UHF 252,136 B flash,
116,248 B SRAM, 8,516 B CCM; maximal 299,808 B flash, 127,784 B SRAM, 15,520 B CCM.
