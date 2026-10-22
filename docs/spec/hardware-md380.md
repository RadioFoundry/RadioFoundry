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
| **D** | Documented by our own tool or schematic reading; used rarely. Facts that rest only on public notes we cannot name are marked (unverified) instead |
| **unverified** | Reported in public notes, a review or a forum we cannot name or cannot check, with no independent basis. Needs a radio |
| **F** | A value from [gotchas] (our own notes) or the bench or carried over from a sibling radio, not checked on this radio |
| **?** | Not found, contradictory, or our own assumption. Needs a radio |

The sources are listed in section 18 and cited by the short names used there ([SKY], [ST],
[gotchas]). [gotchas] is [gotchas.md](gotchas.md) in this repo.

---

## 1. MCU, clocks, memory

### 1.1 Part

STM32F405VGT6, LQFP100: Cortex-M4F, 1 MB flash, 192 KB RAM (128 KB SRAM, 64 KB CCM) (C) [ST].
Read-out protection level 1 on stock radios (unverified).

### 1.2 Clocks

8 MHz crystal on HSE (unverified). ODR runs the DM-1701's clock plan: PLL M 4, N 72, P 2 (72 MHz SYSCLK), Q 3 (48 MHz
USB), AHB 72 MHz, APB1 36 MHz, APB2 72 MHz; the same frequencies the stock firmware is reported to use (72 / 36 /
72 MHz) (unverified). If HSE does not start as a crystal, `board_early_init` retries it in bypass mode (?: the DM-1701
needed bypass) before staying on HSI. LSE: a 32.768 kHz crystal is assumed (?).

### 1.3 Internal flash layout

| Sectors | Addresses | Use |
|---|---|---|
| 0-2 (3 x 16 KB) | 0x08000000-0x0800BFFF | TYT bootloader, never written (unverified) |
| 3-7 | 0x0800C000-0x0807FFFF | ODR application, 464 KB in one piece (`boards/md380/md380.ld`) |
| 8, 10, 11 | see [codec-md380.md](codec-md380.md) | The stock D002.032 codec as the merge places it: the table windows at their stock addresses (sectors 8, 10, 11), the code moved to 0x080E0000 (sector 11), the descriptor at 0x080FFF80 |
| 9 | 0x080A0000-0x080BFFFF | Holds nothing |

The application starts at 0x0800C000 with its vector table there (VECT_TAB_OFFSET 0xC000) (unverified).

`md380.ld` asserts that the image ends before 0x08080000. The default UHF image takes 320,236 bytes with link time
optimization, on by default here: [feature-matrix.md](../feature-matrix.md#dm-1701-and-md-380). Stock firmware keeps
the codec code at 0x08050C44 (sectors 6 and 7), so the merge moves it.

### 1.4 RAM

128 KB SRAM for data and every DMA buffer (minus 4 bytes at the top, as the DM-1701 keeps them for the boot request
word); 64 KB CCM for CPU-only state (`ODR_CPU_RAM`: the task stacks and the slot engine, as on the DM-1701, a76da0d;
main RAM use went from 89 % to 68 % of the default build with that move).

Sources: STM32F405 data sheet [ST]. The bootloader, clock and flash-map facts above have no independent source.

---

## 2. Pin assignments

The pin tables come from public schematic notes (from the RT3 / MD-380 service schematic) that we cannot name. Nothing independent backs them, so every pin is (unverified) unless another basis is cited. The notes warn that the
schematic "is known to be not correct" in places. Where the notes are unclear the pin is marked (?). `boards/md380/pins.h` holds the same map.

### 2.1 Port A

| Pin | Function | Notes |
|---|---|---|
| PA0 | ADC1 IN0: volume knob position | (unverified); a TX LED function here is also reported (unverified, ?) |
| PA1 | ADC1 IN1: battery voltage | (unverified) |
| PA2 | ADC1 IN2: QT_DQT_IN, sub-audio tone input | (unverified); stock decodes CTCSS / DCS in software; ODR samples it with `CONFIG_SUBTONE` (section 6.4) |
| PA3 | ADC1 IN3: VOX, mic level | (unverified) |
| PA4 | DAC1: APC / TV (PA power reference in TX, front-end tuning in RX) | (unverified); the double use (?) |
| PA5 | DAC2: MOD2_BIAS (reference oscillator bias) | (unverified); value from [gotchas] 11.1 (F) |
| PA6 | Keypad row K1 | (unverified) |
| PA7 | POW_C, power latch | (unverified) |
| PA8 | SAVE: enable of the 5C rail (U404), high = on (5.7) | (C) schematic and stock firmware; output, driven high |
| PA9 | VCOVCC_SW: RX or TX VCO supply, high = receive VCO, low = transmit VCO | (F) bench |
| PA10 | DMR_SW: receiver IF to the HR_C5000 | (unverified); low in TX, high in RX (F) [gotchas] 2.6 |
| PA11 / PA12 | USB OTG_FS D- / D+ | (unverified) |
| PA13 | W/N_SW, IF bandwidth; also SWDIO | (unverified); polarity (?) |
| PA14 | MICPWR_SW, microphone bias; also SWCLK | (unverified) |
| PA15 | I2S3 WS | (unverified) |

### 2.2 Port B

| Pin | Function | Notes |
|---|---|---|
| PB0 | ADC1 IN8: RSSI | (unverified) |
| PB1 | ADC1 IN9: BUSY, the receiver's noise squelch level | (unverified); polarity (?) |
| PB2 | FM_SW, FM receive path select | high in FM receive (measured on ODR) |
| PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI, SPI flash (AF5) | (unverified) |
| PB6 / PB7 | I2C1 SCL / SDA | (unverified); nothing on it is known; unused |
| PB8 | SPK_C, speaker mute | high = muted (measured on ODR) |
| PB9 | AFCO, audio amplifier enable | high = on (measured on ODR) |
| PB10 / PB11 | Channel switch ECN2 / ECN3 | (unverified) |
| PB12 | HR_C5000 voice port chip select | (F) [gotchas] 2.6 |
| PB13 / PB14 / PB15 | SPI2 SCK / MISO / MOSI, HR_C5000 voice port (AF5) | (F) [gotchas] 2.6 |

### 2.3 Port C

| Pin | Function | Notes |
|---|---|---|
| PC0 | HR_C5000 timeslot interrupt (EXTI0) | (unverified) |
| PC1 | HR_C5000 system interrupt (EXTI1) | (unverified) |
| PC2 | HR_C5000 RF TX interrupt (EXTI2) | (unverified) |
| PC3 | ADC1 IN13: 2T/5T (FM discriminator audio) | (unverified); unused |
| PC4 | RF_APC_SW, PA power loop on | (unverified) |
| PC5 | 5TC, transmitter supply | (unverified) |
| PC6 | LAMP, LCD backlight (TIM8 CH1 PWM, AF3) | (unverified); PWM is our choice |
| PC7 | CTC/DCS_OUT, sub-audio tone output (PWM) | (unverified); TIM3 CH2 (AF2) with `CONFIG_SUBTONE` (section 6.4), otherwise a low output |
| PC8 | BEEP (TIM8 CH3) | (unverified); unused |
| PC9 | 5RC, receiver supply | (unverified) |
| PC13 | HR_C5000 configuration clock, idles low (section 4.2) | stock bus pin; was wrongly PE3 |
| PC10 / PC11 / PC12 | I2S3 CK / ext SD / SD | (unverified); direction of the two data lines (?) |
| PC14 / PC15 | LSE crystal | (?) |

### 2.4 Port D

| Pin | Function | Notes |
|---|---|---|
| PD0 / PD1 | FSMC D2 / D3 (LCD data, keypad columns) | (unverified) |
| PD2 | Keypad row K2 | (unverified) |
| PD3 | Keypad row K3 | (unverified); carries the side keys (?) |
| PD4 / PD5 | FSMC NOE / NWE | (unverified) |
| PD6 | LCD chip select | (unverified) |
| PD7 | SPI flash chip select | (unverified) |
| PD8 | GPS module power on the G models; listed as a second flash chip select on others | (F) [gotchas] 13.7 |
| PD9 | USART3 RX (AF7): GPS NMEA in on the G models; listed as a flash chip select on others | (?) |
| PD10 | SKY72310 PS, out-of-lock, open collector: pulled up, high = locked | (unverified); pull-up (C) |
| PD11 | SKY72310 chip select | (unverified) |
| PD12 | FSMC A17, LCD RS (data / command) | (unverified) |
| PD13 | LCD reset | (unverified) |
| PD14 / PD15 | FSMC D0 / D1 | (unverified) |

### 2.5 Port E

| Pin | Function | Notes |
|---|---|---|
| PE0 | Green LED (RX) | (unverified) |
| PE1 | Red LED (TX) | (unverified) |
| PE2 | HR_C5000 configuration port chip select | (unverified) |
| PE3 | HR_C5000 SDO -> MCU (input, pull-up) | (F) bench |
| PE4 | SKY72310 clock (idles high) | (F) bench |
| PE5 | MCU -> HR_C5000 SDI, also the SKY72310 data | (F) bench |
| PE6 | DMR_SLEEP, HR_C5000 power down (high = down) | (unverified); polarity as the HR_C6000 (?) |
| PE7-PE10 | FSMC D4-D7 | (unverified) |
| PE11 | PTT, active low | (unverified) |
| PE12 | External PTT (accessory jack), active low | (unverified) |
| PE13 | FM_MUTE | high = FM audio through, low = muted (measured on ODR) |
| PE14 / PE15 | Channel switch ECN0 / ECN1 | (unverified) |

### 2.6 Reset-time levels and the SWD pins

`board_early_init` sets, before anything slow: the power latch (PA7) high; the HR_C5000 chip select and power
down high and its clock low; the SKY72310 clock high; the SKY72310 chip select high; the transmitter supply, APC loop and receiver supply low and VCO select high (the receive VCO);
DMR_SW high; speaker muted, amplifier off, FM muted; LEDs off; keypad rows low; the flash chip select high. PD8 and
PD9 are left as inputs (section 13). These are our safe choices, not stock values (?).

Transmitter safe state (`board_rf_safe`, [crash-log.md](crash-log.md) 8), at every fault, reset and stub entry and first
in `main()`: APC_EN (PC4) and 5TC (PC5) low, VCO_SEL (PA9) high (the receive VCO), the APC DAC (PA4, DAC1 channel 1) 0
when its clock is on. The SKY72310 and HR_C5000 keep their state until the next start.

PA13 and PA14 are the SWD pins. The radio needs them as the IF bandwidth switch and the microphone supply, so
`radio_md380.c` takes them over at radio init; a debugger then has to connect under reset. Build with
`MD380_DEFS=-DMD380_KEEP_SWD` to leave them alone: the microphone is then
unpowered and the bandwidth switch stays as the hardware pulls it.

Sources: the public pin table and schematic notes; [gotchas] 2.6 and 13.7.

---

## 3. Interrupts, DMA, timers

Our allocation, following the DM-1701 (`boards/dm1701`), since the same MCU pins carry the same HR_C5000 lines:

| Source | Use | Priority |
|---|---|---|
| EXTI0 / EXTI1 / EXTI2 (PC0-PC2) | HR_C5000 timeslot / system / RF TX interrupts (falling edge) | 5 |
| TIM7 | 30 ms one-shot after the RF TX interrupt: the burst's PA off (section 5.6) | 5 |
| DMA1 Stream0 ch0 / Stream5 ch2 | I2S3 receive / I2S3ext transmit (PCM, section 6.2) | 6 |
| DMA2 Stream4 ch0 | ADC1 continuous scan (circular; with `CONFIG_SUBTONE` half and full transfer interrupts feed the sub-audio detector, section 6.4) | 7 with `CONFIG_SUBTONE`, else none |
| USART3 | GPS NMEA receive (RXNE interrupt into a ring buffer) | 7 |
| TIM8 | LCD backlight PWM, CH1 on PC6 | - |
| TIM3 | sub-audio tone PWM, CH2 on PC7, only with `CONFIG_SUBTONE` and only while a tone is sent (section 6.4) | 6 |
| OTG_FS | USB (platform) | platform |

Priority 5 is FreeRTOS's `configMAX_SYSCALL_INTERRUPT_PRIORITY` (`boards/md380/FreeRTOSConfig.h`): the modem ISRs
notify the DMR task, and a masked section at that level makes every configuration-port and synthesizer transaction
atomic.

---

## 4. HR_C5000

### 4.1 Overview

The HR_C5000 is the HR_C6000's predecessor in the same family: DMR layer 1/2 baseband, two-point modulation outputs,
an IF input from the receiver, and an audio codec with I2S. ODR has a driver of its own for it,
`drivers/modem/hrc5000`, which shares only the chip-independent code (`drivers/modem/hrc_common`: the bus accessors and
the DMR slot engine) with the HR_C6000 driver. The shared engine reaches the chip through a description struct
(`hrc_chip_t`); the MD-380 build links no HR_C6000 code. The HR_C5000 user manual (HR_C5000 用户手册 V2.0, Chinese,
35 pages, `datasheets/C5000/hrc5000.pdf`) is the source for the register names and reset values in 4.6 (F). It has no
register map for 0x25, 0x26, 0x37, 0x39, 0x3D, 0x41, 0x45, 0x47, 0x48 or 0xB9-0xBB; those values are (?) until a radio
confirms them.

### 4.2 Configuration port and the synthesizer bus

Bit-banged SPI on PE2 (CS), **PC13 (clock)**, PE5 (MCU -> chip) and **PE3 (chip -> MCU)** (F) bench. Unlike the
HR_C6000 on the DM-1701, the clock idles LOW. Per bit: clock high, set MOSI, clock low (the chip samples MOSI on this
falling edge), waits, then samples MISO. The chip drives the next MISO bit only after the falling edge: sampled at
once, every read comes one bit late (0x10 read back 0x40 after 0x80 was written, and the LC 00 00 00 00 00 63 30 5A 5E
read 80 00 00 00 00 31 98 2D 2F) (F) on air 2026-10-18. Register reads in logs taken before that fix (issue 3,
issue 66, up to 2026-10-18) are shifted right by one bit. On any other pin choice every read returned 0xFF. A
transaction is 3 bytes
for a register write (page, register, value) and 2 bytes plus the data for a read (0x80 | page, register), with CS low
around the whole frame. A few cycles per half bit is enough; speed is not a constraint.

The SKY72310 has its own clock (PE4) and chip select (PD11); only the data line (PE5) is shared with the HR_C5000
(F) bench. Its clock idles high: the MCU sets the data bit while the clock is high, drops the clock, raises it again,
and the chip latches the bit on the rising edge. Because PE5 is shared, a transaction on either chip is one masked
section (`md380_rfbus_lock`), so a modem ISR's register access never lands inside a synthesizer word or the reverse.

### 4.3 Differences from the HR_C6000

The chip description in `hrc5000.c` carries these; the HR_C6000 description in `hrc6000.c` carries the other column.

| Item | HR_C6000 | HR_C5000 | Status |
|---|---|---|---|
| Configuration page | 4 | 0 | (F) bench |
| Reg 0x10 in DMR | 0x6E | 0x6B simplex, 0x4F on a repeater channel; RepeaterSlot (b0) stays 1 and the engine picks the slot from the TC (next row) | (C) manual: b2 ISRepeater = repeated operation, b0 RepeaterSlot. On air through an MMDVM duplex hotspot (issue 108): 0x4F with the engine's slot choice lands every call on the channel's slot, both slots, 2026-10-21. 0x6E / 0x6F (timeslot mode, RepeaterSlot 0 / 1) did not steer the slot: 6 key-ups each went out 2,1,1,1,1,1 and 1,1,1,1,2,1 (2026-10-20) |
| Own DMR ID 0x14-0x16 | big endian | 0x14 holds the low byte | (F) manual (LocalSreAddress L, M, H) |
| TX start / stop | access policy and vocoder control written | `tx_begin` / `tx_end` hooks absent | (?) the vocoder runs on the MCU, so 0x22 (EncodeStart) and 0x60 are not used for voice |
| Reg 0x5F (sync / slot report) | read at every burst | absent: DMO / RMO come from the channel | (?) not in the manual's table |
| Embedded LC LCSS | chip | MCU writes it in 0x50 for every voice burst: A-F = 0x08 0x19 0x2B 0x3B 0x4A 0x58 | (?) |
| CRC_MCU_Control (0x40 bit 3) | present | absent: for CSBK, MBC header and data header write 10 bytes and the chip appends the CRC | (C) on air 2026-10-20: CSBKs and data from the MD-380 decode with good CRC on a DM-32UV and an MMDVM hotspot |
| CACH TC report | 0x52 bit 2, refreshed in received slots | 0x52 bit 2 too (the manual calls bits 3-0 reserved), but **it changes only in slots opened for receive**, and 1 means TS2 is starting | (C) per-tick probe of a hotspot downlink, 2026-10-21: with receive open in both slots the bit alternates every tick (0x55555555 over 32 ticks); with one slot closed it stops changing. That froze the engine's lock (it closed the other slot once locked) and every repeater key-up was rejected or landed on either slot. The chip flag `tc_last_rx` keeps both slots open and reads the bit only after a received slot. Polarity from the calls: read as the slot that had ended, every call landed on the other slot |
| Mic gain | 0xE4 = 0x20 + gain | 0x0F = raw value (`C5000_MIC_GAIN` in `radio_md380.c`) | (?) |

DMR set-up (`hrc5000_set_mode_dmr`) writes 0x06 before the reset-release block (0x00 = 0xFF) and FM after it (?);
it runs once per mode change; per-burst switching (`hrc5000_dmr_path`) touches only the
RF side:

- TX: 0x0D = 0x8C, 0x0E = 0x44, mic gain, 0x37 = 0xC2, 0x25 = 0x0E, 0x26 = 0xFE (RX IF and audio off), 0x45 / 0x46 =
  the calibrated DMR modulation pair, 0x35 = 0x1E, 0x3F = 0x04.
- RX: 0x0E = 0x44, mic gain, 0x37 = 0xC2, 0x25 = 0x0E, 0x26 = 0xFD.

VHF radios swap I and Q with 0x01 = 0xF0 after the init. The manual gives 0x01 as the TX and RX IQ/QI bits; which value
the receiver needs is still to be confirmed on a VHF radio (?).

FM (`hrc5000_set_mode_fm_rx`, `hrc5000_set_mode_fm_tx`) uses its own tables. The TX deviation register 0x35 takes 0x30
(narrow) or 0x3C (wide) in FM and 0x1E in DMR; the narrow / wide mapping is inferred (?). CTCSS and DTMF are not
implemented for this chip yet.

### 4.4 Voice port

Hardware SPI2 (PB13-PB15, AF5) with chip select PB12, commands 0x03 (write) and 0x83 (read) followed by 0x00 and 27
bytes (one voice burst's payload) (F) bench. ODR uses SPI mode 2 at 1.125 MHz (F) on air 2026-10-18. At 4.5 MHz
transmit voice decodes on another radio, but received voice reads back with the first bits of every byte wrong: the
C0 words check only two bits off their place and C1 never does. At 1.125 MHz the three frames are in place and C1
checks too; the first data byte after the command still has a few bit errors that the Golay codes correct (?).

The voice RAM is 32 bytes: a longer read wraps. A received burst is in it as the three 72-bit AMBE+2 air frames in a
row (the HR_C6000 layout, `core/dmr/ambe/ambeFec.h`), but only while the decoder runs (4.8).

### 4.5 Reference oscillator

The calibration's reference tune byte t (section 11) sets regs 0x47 = 0x04 = 0x7F - t, 0x48 = 0x00 if t < 0x80
else 0x03, and DAC2 = t x 4 + 0x600 (F) bench. `hrc5000_set_ref()` and `hrc5000_ref_dac()`; the stored tune is
re-applied by every full mode set-up.

### 4.6 Power and start-up

PE6 low wakes the chip; ODR waits 12 ms before the first access. `hrc5000_power_on` then programs the clock
(0x0A, 0x0B, 0x0C, 0xB9-0xBB) and enters FM receive. After init the driver reads 0x10 back for the diagnostics page
(bring-up stage 4).

### 4.7 Checked against the HR_C5000 user manual

The manual's register table (section 2, "参数配置") runs from 0x00 to 0x90 with reset values. What it says about the
registers this driver writes:

| Register | Manual | Driver value | Status |
|---|---|---|---|
| 0x0A | CLKOutDiv, the output clock divider on CKOut (reset 0x02) | 0x80, then 0x00, in the clock step | Not a clock source switch; the effect is unknown (?) |
| 0x0B | PLLM (reset 0x0A) | 0x28 | (F): with XTAL 29.4912 MHz, M = 40 |
| 0x0C | PLLBP bit 7 (0 = PLL, reset 0x82 = bypass), PLL SLEEP bit 6, PLLDO bits 5-4, PLLN bits 3-0 | 0x33 | (F): N = 3, DO = 3 (divide by 8), PLL on. 29.4912 x 40 / 3 / 8 = 49.152 MHz, the manual's system clock |
| 0xB9-0xBB | not in the register table | 0x32, 0x22, 0x11 | Not confirmed. The manual gives the system clocks as fixed from 49.152 MHz: DMR 9.8304 MHz, FM 8.192 MHz, CodeC 12.288 MHz, external CodeC 24.576 MHz (?) |
| 0x18 / 0x19 | section 6 says the system clock is set here; the table defines them as group address registers | not written | Contradiction in the manual; unresolved (?) |
| 0x00 | active-low resets: bit 7 DMR protocol, bit 6 PHY, bit 5 codec, bit 4 FM, bit 3 vocoder, bit 2 UDC/DDC, bit 1 I2S, bit 0 CodeC (0 = reset) | 0x28 | DMR frames go out with DMR protocol and PHY at 0, so 0 is a running block here and the manual's polarity looks inverted (?) |
| 0x01 | bit 7 RFTransIQMode, bit 6 RFRecvIQMode (0 = IQ, 1 = QI) | 0xB0 (UHF), 0xF0 (VHF) | UHF: TX QI, RX IQ. VHF: both QI (?) |
| 0x10 | ModulatorMode bit 7 (0 DMR, 1 FM), ISRepeater bit 2 | 0x6B simplex, 0x4F repeater, 0x80 FM | (F) |
| 0x11 | LocalChanMode bit 7 (1 = valid) | 0x80 | (F) |
| 0x14-0x16 | LocalSreAddress L, M, H (low byte first) | DMR ID, low byte first | (F) |
| 0x1F | LocalEMB, colour code in bits 7-4 | 0x10 (CC 1) | (F) |
| 0x06 | Vocoder638 bit 7 (0 = CMX638 interface off); bit 0 LocalVocoderControl (1 = the CPU controls the vocoder switch) | 0x21 | Consistent; the other bits are not described (?) |
| 0x0D | Codec Ctrl, 0xC8 reset: bit 7 MCU controls the codec signals | 0x8C | Bit 7 set; the other bits unconfirmed (?) |
| 0x0E | HPoutEn, bit 7 (0 = off, reset 0x40) | 0x44 | Headphone output off; the value stays until a test shows otherwise (?) |
| 0x0F | ADLinVol (reset 0xB8) | 0xC8 | In range; the function is not confirmed (?) |
| 0x1D, 0x1E | LocalUnaddress and LocalBroadCast, receive addresses in bits 7-4 | 0xFF and 0xF1 | Reads as address 0xF; confirm on air (?) |
| 0x21 / 0x22 | access policy; EncodeStart bit 7, EncodeStop bit 6 | not written by default | See 4.3 (?) |
| 0x83, 0x87 | InterClear (write 1 to clear), SendStopMask | 0xFF, 0x00 | (F) for the clear |
| 0x30-0x33 | BCLK / LRCK counts for the external codec I2S: Mclk / BCLK and Mclk / LRCK (reset 0) | 0x00 0x17 0x02 0xFF, from the 0x11 block | (F) [ref]: 24 and 768 counts, so BCLK 512 kHz and LRCK 16 kHz from a 12.288 MHz Mclk, or 256 kHz and 8 kHz from 6.144 MHz. Which Mclk the board gives is not known (?) |
| 0x36 | not in the manual. The HR_C6000's manual has bit 7 = external codec BCLK / LRCK off | 0xF0 from the 0x11 block; `hrc5000_i2s_clock()` writes 0x70 (bit 7 clear) while a PCM stream runs | See 4.8 (?) |

Not in the manual: 0x25, 0x26, 0x36, 0x37, 0x39, 0x3D, 0x41, 0x45, 0x47, 0x48 and 0xB9-0xBB. Their values stay unconfirmed (?)
until they are checked on a radio.

### 4.8 I2S clocks and the receive interrupts

Facts from the user manual and the reference port's notes ([ref]; the register values were read from stock firmware
there). Tester logs: [issue 3](https://github.com/sfowlr/OpenDigitalRadio/issues/3).

| Fact | Basis |
|---|---|
| Before any ODR write the chip's I2S pins toggle (the reset state has the clocks running); after the set-ups they are driven but do not move (`ck 0 ws 0`) | tester logs |
| Every set-up's 0x11 block writes 0x36 = 0xF0. On the HR_C6000 bit 7 of 0x36 turns the external-codec BCLK / LRCK off; the C5000 has the same pins (17-20, BCLK / LRCK / ADCDAT / DACDAT) | (F) [ref], HR_C6000 manual |
| The stock firmware changes 0x36 at run time, in a mute / unmute routine | (F) [ref] |
| ODR never wrote 0x36 after the block, so with the same bit meaning the clocks stay off. Every other register of the DMR and FM set-ups matches the stock sequence, and the reset register tries (0x28, 0x2A, 0x3F, 0xFF) changed nothing | (F) [ref], tester logs |

The driver writes 0x36 = 0x00 while a PCM stream runs and puts 0xF0 back when it stops. Clearing bit 7 alone (0x70)
left the clocks stopped; with 0x00 they run and DMR transmit voice decodes on another radio (issue 66, 2026-10-17).
On receive the PCM doesn't reach the speaker yet: with every value from 0x00 to 0x60 the speaker plays only a faint
TDMA buzz, which sounds like the receiver's audio passed through (4.9)
(`hrc5000_i2s_clock()`, called from `audio_md380.c`); a full set-up re-applies the state, since its block rewrites 0x36.
The value is a hypothesis (?): the bit is not in the C5000 manual, and bits 6-4 are unknown. `-DHRC5000_I2S_CTL_ON=<v>`
tries another value (0x00, 0x30). With the clocks running, the diagnostics line `i2s ck N ws N` (`MD380_I2S_PROBE`)
gives the rates. The Mclk arithmetic in 4.7 says 16 kHz or 8 kHz for WS; the STM32 side assumes 8 kHz with 32-bit
channels, so a 16 kHz WS means the sample rate or the channel length needs changing (?).

Interrupts (manual section 7, with the sub-status registers):

| Item | Fact |
|---|---|
| 0x82 flags | bit 7 send request rejected, bit 6 send start (sub-status 0x84, mask 0x85), bit 5 send end (0x86, mask 0x87), bit 4 late access, bit 3 data received (0x51 gives type and CRC), bit 2 information received (sub-status 0x90: 0 = the whole message passed its check, 1 = failed), bit 1 abnormal exit, bit 0 physical-layer receive. Only the system interrupt needs the write-1 clear in 0x83 |
| 0x51 | bits 7-4 data type, bit 3 PI, bit 2 CRC (0 = good), bits 1-0 sync class (0 none, 1 voice, 2 data, 3 RC) |
| 0x52 | bits 7-4 received color code, bits 3-0 reserved; the reference port reads the slot (timecode) from bit 2 |
| Pins | TIME_SLOT_INTER every 30 ms in layer 2, used by the CPU to decide whether and what to send in the next slot; RF_RX_INTER / RF_TX_INTER the receive / transmit switch; SYS_INTER the system interrupt |
| Voice | The CPU reads and writes the voice port (SPI2 on this board) at times it derives from the captured start and end of a call; voice is not announced per burst by the system interrupt |

The tester's logs fit this: flag 0x04 (information) carries the sync-class reports, 0x08 (data) the others, and 0x52
reads 0x08 or 0x18 because the color code is only valid in a report that decoded a burst with one (a voice sync burst
has none). Whether the engine can take a received voice call from these reports alone, or must run its voice
reads from the 30 ms tick as the manual describes, is open (?).

### 4.9 DMR receive

The HR_C5000 doesn't report each received burst: there is no voice class report per burst as on the HR_C6000. With
the reads fixed (4.2), a call shows as (F) on air 2026-10-18, against an AnyTone AT-D878UV:

| Event | What the chip gives |
|---|---|
| Voice LC header, terminator | System interrupt 0x08 with 0x51 = 0x12 / 0x22 (type, class 2) and the LC in page 2 from 0x00 |
| Voice | Nothing per burst. The voice RAM changes at every tick, the other slot's ticks included |
| Late entry, information | 0x10 and 0x04: the LC of the superframe |

The manual's receive flow: with the CPU in charge of the vocoder (0x06 bit 0, which ODR sets), the CPU starts the
decoder after the voice LC header or late entry interrupt by writing 0x22 bit 5 (DecodeStart), and stops it at the
terminator or loss of sync (bit 4, DecodeStop). Without DecodeStart the voice RAM never holds received voice (0 of 324
frames checked at any bit offset); with it, the read at the tick after the call's slot holds that slot's burst.

The slot engine does this for chips with `rx_voice_read` (`hrc_chip_t`): a header, late entry or information report
starts the decoder (0x22 = 0x27, bits 2-0 keeping the 0x11 block's 0x07), the voice port is read at every tick, the
first read whose frames check (two of three C0 words with at most one error) locks the slot, and that slot's reads
go to the stack as bursts A to F. A read that hasn't changed, or has no frame that checks, is a lost burst; six in a
row end the reception (0x22 = 0x17). The chip's voice class reports are ignored, and its 25 ms report delay doesn't
count as out of step.

On air with this (2026-10-18): calls are taken with the caller and talk group, and 393 frames reached the codec with
2 bit errors in all. The decoded audio is speech (peak 3521 of 32767) and streams to the chip (`aud blk` climbing),
but the speaker plays only a faint buzz (4.8). Leads, not checked: 0x0E (HPoutEn, bit 7; ODR writes 0x44, which
leaves it off), 0x37 (DAC gain), the I2S data line directions and sample rate (unknown 17).

### 4.10 Codec and audio routing

DMR receive audio plays with the LINEOUT values below (on air, 2026-10-19: our test, not authoritative). This section
keeps the manual's facts apart from what our tests showed and what we only suspect.

**From the user manual [C5K] (C):**

| Item | Fact |
|---|---|
| Audio pins | 2 HPOUT, 10 LINEOUT, 5-8 MIC2 / MIC1 differential inputs, 11 MICBIAS |
| External-codec I2S | 17 ADCDAT (in), 18 BCLK (out), 19 LRCK (out), 20 DACDAT (out): for an external codec |
| SPI ports | SPI0 = pins 53-57 (the configuration port; the chip is the slave). SPI1 = pins 33-36, "generic vocoder interface" (the voice port; slave). SPI2 = pins 37-43, "codec and vocoder interface" (C_CS and C_SCLK are outputs: the chip drives it) |
| SPI0 command, bits 2-0 | 000 configuration registers, 001 auxiliary table, 010 information data, **011 tone data**, 100 CMX638, 101 AMBE3000; bit 7 = read |
| Tone data | the CPU writes the codec over SPI0 with command 0x03, address 0x00 and two bytes; the required write interval is missing from the manual's text |
| 0x06 | bits 7-6 vocoder interface (AMBE3000 / CMX638), 5 DMRFrom (1 = from SPI1), 4 VocoderFrom, 3 SPIFrom, 2 CodeCMode (0 internal, 1 external), 1 OpenMusic, 0 CPU vocoder control |
| MUX4 (codec source) | 0x06 bit 1 = 1: SPI >> codec (highest priority); else 0x10 bit 7 = 1 (FM): codec <> FM; else (DMR): codec <> vocoder |
| 0x0D | bit 7 CPU controls the codec, 6 suspend, 5 sleep, 4 Switch2 (K2), 3 anti-pop; reset 0xC8 |
| 0x0E | bit 7 HPoutEn, 6 HPMute, 5-4 HP volume (0 / 2 / 4 / 6 dB), **3 LineOutEn**, 2 Mic1En, 1 Mic2En, 0 Switch1; reset 0x40 |
| 0x0F | bits 7-3 ADLinVol (+12 to -34.5 dB in 1.5 dB steps), 2-1 MicVol (0 / 6 / 12 / 20 dB) |

The driver writes 0x0E = 0x44 (headphone muted, line out off, mic 1 on) and 0x06 = 0x21 in DMR.

**What the tests showed (on air, 2026-10-19; not authoritative).** The full log is in the `feature/md380_follow_up`
pull request. Each test started from a power-on, and register reads confirmed the writes took effect.

- Tone data works in FM: a 1 kHz square wave (16-bit, MSB first, one sample every 125 us) played cleanly with 0x0E = 0xBD,
  which needs bit 2 (Mic1En) and bit 0 (Switch1) set.
- In DMR, no HPOUT-side setting played the MCU's samples, by I2S or as tone data. With 0x0E bit 2 set, DMR mode feeds
  the codec's input to the speaker (acoustic feedback). FM_SW and FM_MUTE both high plays the receiver's 4FSK as a buzz.
- 0x06 bit 2 = 1 (external codec) during a call stops the voice decoding until the next set-up.
- During a received call the chip sends the MCU a slow ramp near 0x55F8 over I2S (the mic converter's level), never
  the samples the MCU sent.
- A handheld transmitting a few feet away puts a TDMA buzz into the audio amplifier: faint pulsing in a test can be
  that, not a routing result.

**The fix (our test, on air 2026-10-19; not authoritative).** The speaker amplifier is fed from the codec's LINEOUT
(pin 10, 0x0E bit 3), not HPOUT. The tests above never had 0x0D bit 4 (Switch2) and 0x0E bit 3 (LineOutEn) set together
while a call played. `hrc5000_speaker_voice()` now writes, when a DMR PCM stream opens the route: 0x0D = 0x9C, 0x0E = 0x4C
(HPMute stays set, HPoutEn stays off, Mic1En on) and 0x37 = 0xC2, with 0x36 = 0x00 already written; it writes 0x0D = 0x8C and
0x0E = 0x44 back when the route closes, which should also end the buzz after a call. `-DHRC5000_SPK_VOICE=0` leaves it out.
On an MD-380 (UHF, D002.032 codec) against an AT-D878UV on 446.075 TS1, with the build at 17d89001 plus the codec
move: received speech plays clear from the speaker and transmit is clear (one 10 s call: 498 frames, 10 bit errors, 3
concealed, 0 lost). Why both bits are needed together isn't in the manual (?). Not yet checked with it: the buzz after
a call or after our own transmit, the key beep in DMR and FM, and FM receive.

## 5. RF

### 5.1 Bands and power

One band per radio: UHF 400-480 MHz or VHF 136-174 MHz, 1 W and 5 W (unverified). ODR builds one image per band
(`make BOARD=md380 MD380_BAND=uhf|vhf`, default UHF); the image's band table refuses the other band. Which band a
radio is can't be read from the hardware by any method we know (?: the calibration's frequency list, 11.2, is a
candidate for a run-time check).

### 5.2 SKY72310 synthesizer

Skyworks SKY72310-11 (unverified as the part fitted to this radio). From the data sheet [SKY] (C):

- Serial interface: 16-bit words, MSB first: 4 address bits then 12 data bits, latched when CS goes high. Write only.
- Registers: 0 main divider (N_reg = floor(N) - 32, 9 bits, 6..505 in fractional mode); 1 dividend MSB (10 bits);
  2 dividend LSB (8 bits); 5 reference divider (R - 1, 5 bits); 6 phase detector gain (5 bits, 0.125-1 mA/rad);
  7 power down and Mux_out (bit 0 full power down, 1 synthesizer power down, 2 integer-N, 3 10-bit modulator, 8:6
  Mux_out source, 9 tristate); 8 modulation control; 9 modulation data; 14 delta-sigma (0x080, bit 5 signed mode).
- N = f_vco / f_pd with f_pd = f_osc / R <= 50 MHz; the 18-bit dividend is round(2^18 x frac(N)) in unsigned mode.
- **The driver uses the signed mode** (register 14 bit 5): N_reg = round(N) - 32 and a two's complement dividend for a
  fraction of -0.5..+0.5. Sent unsigned, every channel whose fraction was over one half came out one phase detector
  step (8.4 MHz) low with lock detect high: at 443.7, 444.0-444.5, 435 and 450 MHz the radio received nothing, at
  440, 445 and 446.075 it did (DM-32UV as the sender, 2026-10-20, issue 108). Either the unsigned-mode write doesn't
  take or the part runs signed; signed encoding is right in both cases (C) on air.
- A new ratio takes effect on the dividend MSB write: order divider, LSB, MSB.
- Lock detect high = locked (on the Mux_out / LD pin, wired to PD10 (unverified)). The pin is the PS output, open collector
  (unverified): the MCU's pull-up gives the high (5.7).

`drivers/rf/sky72310` implements this and `tests/host/sky72310Test.c` checks it against a model of the chip. The
board picks the smallest reference divider that keeps N in the fractional range (2 or 3 at 16.8 MHz), the phase
detector gain at its maximum (?: the loop filter was designed for a value we don't know) and Mux_out = the
oscillator. The lock detect output's configuration isn't in our copy of the data sheet (?).

**TX carrier offset (issue 108).** One MD-380 UHF measured about +1.9 kHz high at 448.700 MHz (an MMDVM hotspot's
DMR BER test decoded it only from 448.70175 to 448.70200 MHz, BER 0.14-0.28 %; a DM-32UV measured the same way sat
within 50 Hz of 448.700). Hotspot modems (ADF7021) decode only about +-250 Hz around the carrier, so that radio never
woke a duplex hotspot. `MD380_TX_OFFSET_HZ` (`radio_md380.c`, a build define) corrects it; -1900 put it within 250 Hz.
Whether the offset is the unit's own error, which the calibration's reference tune (11) should take out, or the way
ODR applies that tune, is open (?): measure a second radio before setting a default.

### 5.3 Frequency plan

Assumed from the MD-9600, which has the same TCXO and synthesizer and whose notes give the IF (F) [radios.md MD-9600
section]:

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
FM_SW (PB2) is high in FM receive (6.1). W/N_SW (PA13) sets the IF filter for 12.5 or 25 kHz
(unverified, polarity ?).

Transmit: 5RC off, DMR_SW low, VCOVCC_SW to the TX VCO, the synthesizer to the TX frequency and locked, then 5TC
(PC5), DAC1 = APC drive, RF_APC_SW (PC4) on (unverified for the pins, order ours). FM TX waits for lock detect (5 ms at most)
and refuses to key without it.

### 5.5 RSSI, noise, squelch

RSSI is an analog level on PB0 and the receiver's noise level ("BUSY") on PB1 (unverified). Neither curve is known (?). ODR
reports rssi_dbm = -130 + raw x 90 / 4095 and noise = raw / 16 as placeholders, and shows both raw values on the
diagnostics page so a bench session with a signal generator can fit them.

Measured on ODR (UHF, 446.000 MHz, raw ADC, a second radio nearby; 2026-10-14):

| | RSSI (IN8) | BUSY (IN9) |
|---|---|---|
| no signal | 690-760 | 1340-1620 |
| FM carrier, shown as -71 dBm | 2720-2732 | 39-81 |

BUSY falls with a signal (larger = more noise). With the USB programming cable in the accessory jack the receiver
doesn't detect a signal at all (RSSI stays near the no-signal level): read these values with the cable out (a log
line read back afterwards), or by ear.

### 5.6 PA and power control

DAC1 is the APC reference, scaled from the calibration's power rows (11.2). Two levels, 1 W and 5 W. TX is refused
without an accepted calibration table (no safe uncalibrated drive is known).

DMR: the PA follows the bursts. The RF TX interrupt keys it (APC_EN); a TIM7 one-shot drops it 30 ms later
[gotchas] 3.4, and the slot engine's PA-off at the next timeslot interrupt stays as the fallback.

The radio has one synthesizer, so it can't receive while it is set up to transmit. The switch to TX (receiver off,
the HR_C5000's transmit set, the TX VCO locked, TX_5V up) happens before the first burst, never at it: done in the RF TX
interrupt, the first burst went out unlocked and a one-burst wake-up never reached a repeater (C, issue 108). When:

| Start | Switch to TX |
|---|---|
| Own timing (simplex, the repeater wake-up) | at the modem's `tx_begin`, or at radio `tx()` if that comes later |
| Aligned to a repeater's slots | at the engine's `tx_slot_found`, two ticks before the first burst. Until then the receiver stays on: the engine needs the downlink's TC to find the slot (4, the CACH TC row) |

On a split channel the synthesizer returns to RX at the engine's TX done, so the radio hears the repeater's answer to
its wake-up; simplex stays on TX until `tx_off()` (C, issue 108).

### 5.7 The 5C rail (SAVE)

PA8 ("SAVE") is the enable of U404 (XC6204B502, CE active high), the regulator for the 5C rail; 5C feeds the
SKY72310's 3.3 V regulator U602 (XC6204B332, CE tied to its input) and part of the receive chain (C) [schematic,
page 4]. The stock firmware drives PA8 high as an output (C: its GPIOA registers read on a running radio). `board.c` drives it high at start-up.

Measured on a UHF MD-380 with no signal: the raw RSSI reading (ADC1 IN8) went from 246 with PA8 left as an input to
640-720 with it high, and the RSSI follows a received carrier. PD10 (PS, pulled up) reads 1 with SAVE low (the
synthesizer unpowered, PS floating) and 0 with SAVE high (powered, not yet programmed: out of lock) (C).

Sources: [SKY] (C); [radios.md](radios.md) MD-9600 notes for the
frequency plan (F); [gotchas] 3.3, 3.4; MD-380 UHF RF schematic for 5.7.

---

## 6. Audio

### 6.1 Paths

FM receive audio and DMR voice both reach the amplifier through the HR_C5000 in ODR's model: FM through its
feed-through (0x36) behind FM_MUTE (PE13), DMR through its codec DAC; SPK_C (PB8) mutes the speaker, AFCO (PB9) turns
the amplifier on (the routing (?)). Measured on ODR with a pin scan (the 16 levels of FM_MUTE, FM_SW, SPK_C and AFCO,
monitor held on an FM channel): noise only with FM_MUTE high, FM_SW high, SPK_C low and AFCO high. With those levels
a received FM call plays clearly. The GT3136's discriminator audio also reaches the MCU on PC3, which
ODR doesn't use.
Our tests (2026-10-19, not authoritative) found the HR_C5000's codec output reaching the speaker with FM_SW and
FM_MUTE low; DMR voice through it doesn't work yet (4.10).

The volume knob sets the level in the analog path; the MCU only sees its position on PA0. The board therefore has no
`CAP_VOLUME_POT` and `hal_power_t.volume()` returns -1. There is no `hal_radio_t.volume` either: the MCU has no way to set
the speaker level (the HR_C5000's feed-through gain isn't used for it, and the knob acts after it), so the debug link's
volume operation (RADIO op 30) answers "unavailable" (`radio-features.md` 4.2).

### 6.2 I2S PCM

I2S3 full duplex, slave to the HR_C5000's codec: WS PA15, CK PC10, SD PC12 (chip -> MCU) and ext SD PC11 (MCU ->
chip), Philips, 16-bit samples in 32-bit channels, 8 kHz (unverified for the pins; format and directions copied from the
DM-1701 (?)). `audio_md380.c` is the DM-1701's driver. Which HR_C5000 port these lines reach (the external-codec
pins or SPI2, the codec / vocoder port) is open (4.10).

### 6.3 Microphone

Bias from MICPWR_SW (PA14), on whenever the radio runs (section 2.6). Mic level on PA3 for VOX (unverified).

### 6.4 Sub-audio

The stock firmware is reported to decode CTCSS / DCS in software from QT_DQT_IN (PA2) and to generate tones as PWM on
CTC/DCS_OUT (PC7) (unverified). ODR does the same behind `CONFIG_SUBTONE` (on by default, 3.3 KB of
flash). CTCSS 100.0 Hz works on air both ways (tester, 2026-10-19); DCS and the deviation aren't checked yet. Without
the switch, channels with a tone open on carrier and send no tone.
The FM TX tones (1750 Hz, roger beep) and APRS AFSK are not written (`CONFIG_FM_TX_TONES`, `CONFIG_APRS_RX` in
`config.mk`).

The DSP is chip independent (`drivers/rf/subaudio`, host-tested on synthetic signals); `boards/md380/subtone_md380.c`
and `power_md380.c` are the glue.

**Receive.** PA2 is one more rank of the ADC1 scan that `power_md380.c` already runs (IN2 after the temperature
sensor, 480-cycle sampling, ADC clock PCLK2 / 8). Seven ranks give a pass every 383 us, so the sub-audio is sampled at
PCLK2 / 8 / (7 x 492) = 2613 Hz (computed at run time). The DMA stream's half and full transfer interrupts (DMA2
Stream4, priority 7) hand each new sample to the detector; the ring grows from 8 to 16 scans, so the averaged RSSI
and battery readings cover 6 ms instead of 3 ms. The detector (`subaudio_rx.h`):

| | CTCSS | DCS |
|---|---|---|
| Method | Goertzel at the tone and 3 Hz either side, over overlapping 0.2 s windows, a result every 0.1 s | slicer with peak-tracked threshold, clock recovery on the edges, 23-bit register against the Golay word |
| Opens | tone holds 30 % of the AC power, beats both guard bins by 1 dB, amplitude at least 8 counts; two results in a row | word matches with at most 2 bit errors, twice 23 bits (+-1) apart |
| Closes | three bad results in a row (0.3 s); a 150 ms phase reversal at the end of a transmission is ridden out | no match for three words (0.5 s) |
| Tested | all 50 standard tones, neighbors rejected, +-1 Hz sender error, noise only for 60 s | all 104 codes both polarities, wrong code and polarity rejected, +-1 % bit rate |

`status().tone_ok` is the detector's verdict (true for a channel without an RX tone); the core debounces it and gates
the squelch (`radio-features.md`). An inverted code is the same bits inverted on the wire; if the analog path inverts
the waveform, `MD380_SUBTONE_RX_INVERT` flips it back.

**Transmit.** TIM3 CH2 on PC7 (AF2) is a 10-bit PWM at 17.6 kHz (the timer clock divided by 4 x 1024). Its update
interrupt (priority 6, running only while a tone is sent) computes a new duty every fourth period, 4.4 kHz: a sine
from a 65-point table for CTCSS, the 23-bit word at 134.4 baud with a light smoothing for DCS. The level ramps up over
20 ms; `MD380_SUBTONE_PERCENT` (default 30) sets the swing as a share of the duty range. `tx()` starts it after the PA is
on, `tx_off()` stops it, and the pin returns to its idle low output. TIM3 and DMA2 Stream4's interrupt are the only
resources added (section 3). The DCS turn-off tail (`radio-features.md` 3.3a) switches the generator to a 134.4 Hz
sine at the same swing (`subaudio_tx_tail`, with the interrupt held off for the switch); not yet checked on a radio.

**Unverified, check on a radio** (`docs/testing/md380-bringup.md` 5 and 6):

- That PC7 reaches the modulator and where (summing into the HR_C5000's modulation input or the VCO line), and the
  deviation 30 % gives. CTCSS wants 0.5 to 0.75 kHz of deviation.
- The PWM's own filter: the carrier is 17.6 kHz and the tone at most 254.1 Hz, so any RC corner under about 1 kHz works.
- The signal on PA2: its level (`MD380_SUBTONE_MIN_AMP`, default 8 counts of 4096) and its polarity for DCS.
- That the ADC rate really is 2613 Hz (the 480-cycle sampling is the existing scan's) and that PA2 is not
  loaded by the longer sampling.
- That the stock firmware's tone path is the same: the sources describing it are second hand.
- A DCS transmission ends without the 134.4 Hz turn-off burst (not written); the receiving squelch tail may be
  longer than with a stock radio.

---

## 7. Display

160 x 128 color TFT on the FSMC 8-bit bus (NE1), command at 0x60000000 and data at 0x60040000 (A17 = RS on PD12),
chip select PD6, reset PD13: the DM-1701's wiring (unverified). The controller is reported as an HX8302A
(unverified) or possibly an ILI9481 (unverified, ?); RT3 and MD-380 panels are said to be "slightly
incompatible" (unverified). ODR drives it with `drivers/display/st7735` (the HX8302A takes the ST7735 command set (?)) with
safe-area insets of 1 px at the top and 2 px on the left (the case edge crowds them; right and bottom are close but usable; seen on the radio, 2026-10-16) and two build-time knobs for the bench: `MD380_PANEL_TYPE` (the st7735 driver's panel
type, default 0) and `MD380_PANEL_MADCTL_XOR` (orientation bits flipped, default 0). Backlight: LAMP (PC6), TIM8 CH1
PWM at 1 kHz (unverified for the pin; PWM ours).

---

## 8. LEDs

Green (RX) on PE0, red (TX) on PE1, active high (unverified, polarity ?).

---

## 9. Keypad, side keys, channel switch, PTT

### 9.1 Matrix

Rows K1 PA6, K2 PD2, K3 PD3, driven high one at a time; columns are the LCD data lines D0-D7, read under the LCD bus
lock as on the DM-1701 (unverified for the pins). The key positions are assumed to be the MD-UV380's, the nearest documented
relative (?):

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| K1 | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| K2 | Green (menu) | Up | Down | 7 | 8 | 9 | # | Red (back) |
| K3 | - | - | - | - | - | - | SK2 | SK1 |

The diagnostics page shows the raw row bits ("keys rr rr rr"), so one press of each key on a radio fills in this table.

### 9.2 Channel switch

A 16-position switch with four code lines ECN0-ECN3 on PE14, PE15, PB10, PB11 (unverified), not a quadrature encoder.
`input_md380.c` reads the code at every 10 ms scan and reports position changes as encoder steps (the shorter way
round across the stop). A code counts after three equal scans (30 ms): the contacts bounce. Pulled up, closed contacts
reading low (`MD380_CH_ACTIVE_HIGH` flips it). The code is a Gray code in its own line order. Read on ODR's diagnostics
page ("ch x", after the polarity), one detent at a time clockwise (bench, 2026-10-16):

| Position | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Code | 0 | 8 | A | 2 | 3 | B | 9 | 1 | 5 | D | F | 7 | 6 | E | C | 4 |

Each step changes one line, so a bouncing contact reads as the next or previous position, never a jump.

### 9.3 PTT

PTT on PE11 and the accessory jack's PTT on PE12, both active low with pull-ups (unverified).

---

## 10. Power and analog inputs

### 10.1 Power switch and latch

The volume knob carries the power switch. It doesn't switch the supply: POW_C (PA7) is a latch the firmware holds (unverified),
and the switch breaks the battery sense line (and the volume pot's supply) (C, our bench). Measured on a UHF MD-380 with
the radio on USB: the raw battery reading (ADC1 IN1) is 3341-3351 (8.3 V) with the knob on and 3-4 with it off, at once;
the volume reading falls from about 130-155 to 2-4. The radio stays on with the knob off, and with `switch_on()` always
on it only showed the battery running down. `switch_on()` now reads the knob from the battery line (`radio-features.md`
5.5, `core/radio/power_switch.c`, as on the DM-1701), and power-off drops the latch.

### 10.2 Battery

7.4 V 2S Li-ion pack (unverified). ADC1 IN1 through a divider: ODR assumes 40.3 counts per 0.1 V
(`MD380_BATT_COUNTS_X10`) (?). Low 6.2 V and critical 6.0 V are ODR's choices, as on the DM-1701.

### 10.3 ADC scan

ADC1 scans IN0 (volume), IN1 (battery), IN3 (mic), IN8 (RSSI), IN9 (noise) and IN16 (temperature sensor) continuously at
480 cycles each into a DMA ring of 8 scans; readings are averages over the ring (`power_md380.c`).

---

## 11. Calibration

### 11.1 Location

512 bytes in the SPI flash's security registers 1 and 2 (flash addresses 0x1000 and 0x2000, 256 bytes each) (F)
[gotchas] 11.1. A third 256-byte block must not be read into a 512-byte buffer (F) [gotchas] 11.1;
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

Winbond W25Q128FV, 16 MB, on SPI1 (PB3-PB5, AF5) with chip select PD7, mode 0, 4.5 MHz (unverified). Some
early VHF radios have a 1 MB part (unverified). `flash_md380.c` accepts any W25Q (JEDEC type 0x40) and probes the
size.

### 12.2 Map

On a 16 MB part ODR uses the DM-1701's map (settings 0x011000, journal 0x019000, open-firmware codeplug 0x000000 and
0x020000-0xE00000, native area 0xE00000, TFTP staging 0xF00000), so the open-firmware CPS works the same. The stock TYT codeplug at 0 is overwritten
by the first open-firmware write: read it out with the stock CPS first if it matters ([md380-migration.md](md380-migration.md) does that, converts it with qdmr and writes it back). There is no calibration window in the
main array (11.1). On a smaller part the core gets no areas: settings live in RAM and nothing is written.

---

## 13. GPS

### 13.1 Module

The G models carry a GPS module whose NMEA output goes to the MCU's USART3 (unverified for the module; pins (?)). The module's
part number is not settled: JS-H210 and JS-M710 are both reported (?). 9600 baud is assumed (?).

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

TYT ships the application as an encrypted "OutSecurityBin" container, the same family as the DM-1701's (unverified).
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

The payload is XOR-encrypted with a 1024-byte key held in the bootloader (unverified). The key's SHA-256 is
5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd and its first bytes 2E DF 40 B5 (D,
`tools/stm32_dfu.py` md380 model). It can be derived from the D2.32 stock file and checked by that hash (C,
2026-10-10): the plain column vote is wrong in a third of the columns, and a byte-pair refinement learned from the file
fixes them ([codec-md380.md](codec-md380.md), "Key recovery"; `odrflash/stockfw.derive_key`). A key file still works
(`--key`).

### 14.3 Bootloader and DFU

Entered by holding PTT and the upper side key while switching on (unverified). USB DFU 0483:DF11 with TYT's
non-standard commands; the same erase-by-sector and block-write protocol as the DM-1701's loader (unverified). Flash an ODR
image with `tools/stm32_dfu.py --model md380 --key KEY odr_md380.bin`. The image is plain, starts at 0x0800C000, and the
tool encrypts it for the loader. odrflash knows the board (`boards.py` MD380: not releasable, an update writes
sectors 3-7 and keeps the codec).

### 14.4 The codec

The AMBE+2 codec is inside the stock application. Located in D002.032 (C, 2026-10-10): the same library as the
DM-1701's, at other addresses. In the stock file: code window 0x08050C44+0x10884, table window 0x0808FC64+0xAFF8 and
four small table windows in sectors 10 and 11. The merge moves the code to 0x080E0000, keeps the table windows at
their stock addresses and writes a descriptor at 0x080FFF80; the codec runs in place from there. Entry points,
calling convention, state sizes, the placement and the extraction recipe are in [codec-md380.md](codec-md380.md).
`stockfw.PROFILES` has the profile, the codec map (`odrflash/codec_maps/md380/md380-d2032.json`) the rewrites,
`md380.ld` keeps the application out of sectors 8-11, `vocoder_md380.c` checks and hosts the codec, and
`make BOARD=md380` merges it when the stock file is in `codecs/`. The debug link reports the codec as
`md380-d2032-s11`; a build from before the move reports `md380-d2032`. Without the codec merged the radio is FM only.

---

## 15. USB

OTG_FS device on PA11 / PA12 (unverified). ODR enumerates with the DM-1701's IDs and the product name "MD-380"
(`usb_md380.c`); the bootloader's DFU device is separate (14.3).

The open-firmware CPS shim reports radio type 7 and serves the MCU ROM from 0x08000000, 1 MB (`boards/md380/board.c`,
`cps-protocol.md` 2.2).

---

## 16. Variants

| Radio | Difference | Tag |
|---|---|---|
| MD-380 | the base board | (unverified) |
| MD-390 | the same electronics in an IP67 case | (unverified) |
| Retevis RT3 | a rebadged MD-380; its panel is said to be slightly incompatible | (unverified) |
| MD-380G / MD-390G / RT3 GPS | GPS module on USART3, power on PD8 (section 13) | (unverified, pins ?) |
| UHF / VHF | separate boards: different RF parts and VCOs, one ODR image each | (unverified) |
| 1 MB flash | some early VHF radios; ODR stores nothing on them | (unverified) |

---

## 17. Unknowns that need a radio

Each with how to settle it; the stage numbers are [md380-bringup.md](../testing/md380-bringup.md)'s.

| # | Unknown | How to settle it on a radio |
|---|---|---|
| 1 | HSE crystal or external clock | Stage 1: the radio boots on HSE (USB enumerates); if only bypass works, record it |
| 2 | Display controller, orientation, colors | Stage 2: try `MD380_PANEL_TYPE` / `MD380_PANEL_MADCTL_XOR` until the boot screen is right side up and in color |
| 3 | Key matrix positions, side keys on K3 | Stage 3: press each key, read "keys" on the diagnostics page, correct section 9.1 and `input_md380.c` |
| 4 | Channel switch code polarity and order | Settled 2026-10-16: a Gray code, table in 9.2 |
| 5 | Volume knob on PA0 vs the TX LED | Stage 3: "vol" on the diagnostics page follows the knob, and the red LED lights on PE1 |
| 6 | Battery divider | Stage 3: compare "bat raw" against a meter; set `MD380_BATT_COUNTS_X10` |
| 7 | Power switch wiring (direct or sensed) | Stage 3: switch off with the knob: does the radio die at once? |
| 8 | Flash size and JEDEC ID | Stage 3: "flash" line |
| 9 | Calibration layout and scaling | Stage 4: "cal" must say factory; read both security registers over the open-firmware CPS protocol (area 10, which `md380_cps_security_read` serves) and compare the rows with what the stock CPS shows |
| 10 | Frequency plan (TCXO, IF, LO side, VCO multiplier per band) | Stage 4: "pll lock" on every channel; an SDR on a 25 kHz-spaced pair of channels shows the receive LO leak and TX carrier |
| 11 | SKY72310 bus timing and lock detect polarity | Stage 4: "pll lock" with a correct carrier; a scope on PD10 while retuning |
| 12 | W/N_SW and DMR_SW polarities (FM_SW, FM_MUTE, SPK_C and AFCO measured, 6.1) | Stage 4/5: receive a known FM signal; invert one pin's level at a time in `radio_md380.c` (one rebuild each, batched) until IF, audio and bandwidth are right |
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
| [SKY] | Skyworks SKY72310-11 data sheet 203639L, https://www.skyworksinc.com/Products/PLLs-Synthesizers-VCOs/SKY72310-11 | Data sheet (C) |
| [ST] | STMicroelectronics STM32F405xx data sheet and RM0090 | Data sheet (C) |
| [C5K] | Hangzhou Hongrui HR_C5000 user manual (Chinese): pins, SPI ports, configuration registers, audio routing | Data sheet (C) |
| [gotchas] | [gotchas.md](gotchas.md) 2.6, 3.3, 3.4, 5.2, 11.1, 13.7 | This repo |

No GPL source, no disassembly and no binary of another project were read for this file. The stock D2.32 file was only
hashed and its container header read (14.1).

## 19. Sizes

Flash: the table in [feature-matrix.md](../feature-matrix.md#dm-1701-and-md-380) and the per-feature figures in
[feature-matrix.md](../feature-matrix.md). Measured 2026-10-19 with LTO: default UHF image 320,236 B of 475,136 B
(464 KB), 107,120 B of 131,068 B SRAM, 59,640 B of 64 KB CCM; maximal 327,064 B flash.
