<!-- SPDX-License-Identifier: MIT -->

# Baofeng DM-1701 hardware (and its STM32 siblings)

The Baofeng DM-1701, the Retevis RT84 and the TYT MD-UV380 / MD-UV390 (5 W and "Plus 10 W") are one board family:
an STM32F405VGT6 (LQFP100) MCU, an HR_C6000 DMR baseband/codec chip, one AT1846S RF transceiver covering both VHF
and UHF, a 160 x 128 color LCD on an 8-bit 8080-style bus, a 16 MB SPI NOR flash, a 3-row keypad matrix that shares
its column lines with the LCD data bus, a rotary channel encoder, a volume potentiometer with a built-in power switch,
and USB full speed device on the MCU's OTG_FS core. This file records what the hardware needs. The DM-1701 is the
primary target; differences for the siblings are collected in section 17.

Conventions:

| Item | Convention |
|---|---|
| Register values | Hex |
| AT1846S registers | 16 bit, written as `reg = value` |
| HR_C6000 registers | Written as `page:reg = value` |
| "Unverified" | The fact is inferred. No datasheet or measurement confirms it |

Register-level HR_C6000 behavior is in `modem-hrc6000.md` and is not repeated here.

---

## 1. MCU, clocks, memory

### 1.1 Part

| Item | Value |
|---|---|
| MCU | STM32F405VGTx, LQFP100, Cortex-M4F, 1 MB flash, 128 KB SRAM (0x20000000), 64 KB CCM RAM (0x10000000), 4 KB backup SRAM (unused) |
| FPU | single precision, hard float ABI (fpv4-sp-d16) |
| NVIC priority bits | 4 (16 levels); priority grouping 4 (all preemption) |
| RTOS tick | 1 kHz (SysTick); a separate 1 kHz HAL time base runs on TIM14 at priority 3 |

### 1.2 Clock tree (normal run mode)

| Item | Value |
|---|---|
| HSE | 8 MHz external clock in **bypass** mode (no crystal on OSC_IN/OSC_OUT; a clock signal is fed in). 8 MHz is implied by USB working at 48 MHz with the PLL values below |
| LSE | 32.768 kHz crystal on PC14/PC15, clocks the RTC |
| PLL | source HSE, M = 4 (VCO in 2 MHz), N = 72 (VCO 144 MHz), P = 2, Q = 3 |
| SYSCLK / HCLK | 72 MHz |
| APB1 (PCLK1) | 36 MHz (/2); APB1 timer clock 72 MHz |
| APB2 (PCLK2) | 72 MHz (/1); APB2 timer clock 72 MHz |
| 48 MHz domain (USB) | 144 / 3 = 48 MHz |
| Flash wait states | 2 at boot; 3 is used when the clock is re-applied at run time |
| Voltage scale | 1 |
| PLLI2S | N = 192, R = 4 -> 96 MHz (unused in practice: I2S3 runs as slave, see 4.4) |
| RTC | LSE, async prescaler 127, sync prescaler 255 -> 1 Hz |

Boot gotcha: the bootloader leaves the system running from HSE/PLL. The HSE configuration cannot be changed
while it is the system clock, so the application first switches SYSCLK to HSI, then reprograms HSE/PLL, then switches
back to the PLL.

### 1.3 Reduced-power ("eco") clock

| Item | Value |
|---|---|
| PLL P | 6 (SYSCLK 24 MHz) |
| APB1 | /1 (24 MHz) |
| Flash wait states | 0 |
| Backlight timer period | changed with the clock (TIM1, section 9.3) |
| Rule | Not entered while USB is in the configured state. The 48 MHz domain stays at 48 MHz, but the bus clocks drop too far for the USB stack. On a USB reset the full speed clock is restored first |

### 1.4 Internal flash layout

STM32F405, 1 MB: sectors 0-3 are 16 KB, sector 4 is 64 KB, sectors 5-11 are 128 KB. The V02.06 CSV codec runs
in place from sectors 10 and 11, where the merge puts it ([codec-placement.md](codec-placement.md), "STM32 radios";
[codec-dm1701.md](codec-dm1701.md)): tables at their original address 0x080D19F0..0x080E3A72, code moved from its original
address 0x08074714 to 0x080E3A74..0x080F3C90, and the descriptor at 0x080FFF80. The codec from a V02.01, V02.02 or
V02.03 CSV file sits 0x40 lower in the same sectors: tables 0x080D19B0..0x080E3A32, code 0x080E3A34..0x080F3C50.
The codec from a noCSV file sits higher, still in sectors 10 and 11: V02.06 noCSV tables 0x080D4794..0x080E6826, code
0x080E6828..0x080F6A28; V02.03 noCSV 0x40 lower.

| Sector | Address | Size | Holds |
|---|---|---|---|
| 0-2 | 0x08000000..0x0800BFFF | 48 KB | bootloader: never erased or written |
| 3 | 0x0800C000..0x0800FFFF | 16 KB | RadioFoundry (vector table first) |
| 4 | 0x08010000..0x0801FFFF | 64 KB | RadioFoundry |
| 5-9 | 0x08020000..0x080BFFFF | 5 x 128 KB | RadioFoundry |
| 10 | 0x080C0000..0x080DFFFF | 128 KB | codec tables (from 0x080D19F0; V02.01 to V02.03: 0x080D19B0; noCSV: 0x080D4794, 0x080D4754) |
| 11 | 0x080E0000..0x080FFFFF | 128 KB | codec tables (to 0x080E3A71), codec code (0x080E3A74..0x080F3C8F), descriptor (0x080FFF80..0x080FFFFF); V02.01 to V02.03: tables to 0x080E3A31, code 0x080E3A34..0x080F3C4F; V02.06 noCSV: tables to 0x080E6825, code 0x080E6828..0x080F6A27 (V02.03 noCSV 0x40 lower) |

A flash sector can only be erased whole, so `boards/dm1701/memory.ld` gives the application sectors 3-9
(0x0800C000..0x080BFFFF, 720 KB in one piece) and asserts that everything it loads into flash ends before 0x080C0000.
The image is one `.bin` from 0x0800C000, and an update erases and writes only the sectors it covers. The bytes around
the codec in sectors 10 and 11 are 0xFF after a merge and are never executed. The DM-1701's settings and codeplug live
in the external SPI flash, so nothing else writes internal flash.

The manufacturer's firmware, and RadioFoundry builds from before the codec moved, keep the codec code in sectors 7 and 8, which the
application now covers. A release or manifest with two ranges (the application and a sector 9 data part) is from
before the move and is refused. Moving a radio to this layout writes the codec once, from the manufacturer's firmware file or from a
copy the tools read from the radio earlier ([release.md](../dev/release.md), "Codec handling").

rfoundry's keep and merge modes follow from this map ([release.md](../dev/release.md), "Codec handling").

The codec's entry points, as offsets from the code start: encode +0x130, encode FEC +0x4E8, decode +0x5D8. Call with
the Thumb bit set when calling through a pointer.

Codec calling convention (the interface to the third-party blob; unverified beyond "it works"):

| Call | r0 | r1 | r2 | r3 | stack words [0], [1], [2], [3] |
|---|---|---|---|---|---|
| decode (called twice per 20 ms frame) | PCM out (80 samples per call) | 80 | 49 input bits, one bit per 16-bit word | 0 | 0, half index (0 then 1), pointer to decoder state (0x7EC bytes) |
| encode (called twice per 20 ms frame) | 72-word bit buffer out | 0 | PCM in (80 samples per call) | 80 | flags (0x1840 first half, 0x0800 second half), half index (0/1), 0x2000, pointer to encoder state (0x2000 bytes) |
| encode FEC | bit buffer | same bit buffer | 0 | pointer to FEC state (0x100 bytes) | none |

The 72 encoded bits come back one bit per 16-bit word (LSB) and are packed MSB first into 9 bytes. On decode, the
9-byte (72-bit) AMBE+2 frame must first be de-interleaved and FEC-decoded to 49 bits by the caller (Golay(24,12) on C0,
Golay(23,12) on C1 after removing the pseudo-random whitening seeded from C0's 12 data bits with x' = 173x + 13849 mod
65536, the standard AMBE+2 3600x2450 FEC; see the codec spec). The state buffers must be initialized by the codec's own init calls (`codec-dm1701.md` C). Gotcha: a call site that builds the stack
arguments must not be optimized in a way that moves the stack pointer (with `-Os` around the call, DMR TX crashed);
calling through a small assembly shim with an explicit frame avoids this.

### 1.5 RAM

| Region | Address | Size | Notes |
|---|---|---|---|
| SRAM1+SRAM2 | 0x20000000 | 128 KB | general data, DMA buffers. Initial stack pointer must be **below** the top of RAM: the bootloader rejects SP = 0x20020000, so use 0x2001FFFC (4-byte aligned) |
| CCM | 0x10000000 | 64 KB | CPU (D-bus) only: **not reachable by any DMA controller**. Not zeroed and not initialized by the startup code: anything placed there starts with garbage and initializers are ignored unless the startup code copies them |
| Backup SRAM | 0x40024000 | 4 KB | battery backed only if VBAT is supplied (unverified on this board); unused |

Main RAM and CCM are both nearly full in a full build (frame buffers, flash sector buffer, calibration copy, USB buffers, data-service buffers); plan buffers explicitly.


---

## 2. Pin assignments

All 100 pins that matter, by port. "In" pins have the pull noted. Active levels are what the board needs.

### 2.1 Port A

| Pin | Dir | Function | Notes |
|---|---|---|---|
| PA0 | analog | ADC1_IN0: volume potentiometer wiper | log-taper pot, see 11.2 |
| PA1 | analog | ADC1_IN1: battery voltage divider | see 11.1; also senses the power switch (11.3) |
| PA2 | out | UHF receiver 5 V switch | high = UHF RX chain powered |
| PA3 | analog | ADC1_IN3: microphone level (VOX detector) | |
| PA4 | analog | DAC_OUT1: PA power control reference (APC) | 12-bit, buffered; see 7 |
| PA5 | out | VHF receiver 5 V switch | high = VHF RX chain powered |
| PA6 | out | keypad row 0 drive | push-pull high when scanning this row, floating otherwise |
| PA7 | out | power latch | high keeps the radio powered after the switch; low lets it turn off |
| PA8 | AF4 open drain | I2C3_SCL (AT1846S) | 400 kHz |
| PA9 | out | GPS module power enable | high = on; the pin has a USART1_TX function but is used as a GPIO (no TX to the GPS) |
| PA10 | AF7 | USART1_RX from GPS | 9600 8N1, RX only |
| PA11 | AF10 | USB_OTG_FS_DM | |
| PA12 | AF10 | USB_OTG_FS_DP | |
| PA13 | out | microphone bias/power switch | high = mic powered. **This is SWDIO**: once driven as GPIO, SWD only works with connect-under-reset |
| PA14 | SWCLK | debug clock | left as SWCLK |
| PA15 | AF6 | I2S3_WS (from HR_C6000) | |

### 2.2 Port B

| Pin | Dir | Function | Notes |
|---|---|---|---|
| PB3 | AF5 | SPI1_SCK (SPI flash) | |
| PB4 | AF5 | SPI1_MISO (SPI flash DO) | |
| PB5 | AF5 | SPI1_MOSI (SPI flash DI) | |
| PB8 | out | speaker mute | high = muted (reset state high) |
| PB9 | out | audio power amplifier enable | high = on (reset state high) |
| PB11 | in, pull-up, EXTI11 both edges | rotary encoder B | |
| PB12 | out | HR_C6000 voice SPI chip select (V_CS) | idle high |
| PB13 | AF5 | SPI2_SCK (HR_C6000 voice port) | |
| PB14 | AF5 | SPI2_MISO | |
| PB15 | AF5 | SPI2_MOSI | |

### 2.3 Port C

| Pin | Dir | Function | Notes |
|---|---|---|---|
| PC0 | in, EXTI0 falling | HR_C6000 TIME_SLOT_INTER | |
| PC1 | in, EXTI1 falling | HR_C6000 SYS_INTER | |
| PC2 | in, EXTI2 falling | HR_C6000 RF_TX_INTER | the chip's RF_RX_EN / RF_TX_EN outputs are **not** routed to the MCU |
| PC4 | out | PA enable 2: power control (APC) loop enable | high = on |
| PC5 | out | PA enable 1: TX supply | high = on |
| PC6 | out | PA band select (DM-1701, RT84, MD-UV380 5 W) | low = VHF PA, high = UHF PA. On the MD-UV390 Plus 10 W this pin is the LCD backlight instead |
| PC7 | out, open drain | CTCSS/DCS PWM output | idles high; not used (the AT1846S generates sub-audio tones) |
| PC8 | out | beep PWM output | idles low; not used (beeps go out as PCM via the HR_C6000) |
| PC9 | AF4 open drain | I2C3_SDA (AT1846S) | |
| PC10 | AF6 | I2S3_CK (from HR_C6000) | |
| PC11 | AF5 | I2S3ext_SD: MCU -> HR_C6000 PCM | |
| PC12 | AF6 | I2S3_SD: HR_C6000 -> MCU PCM | |
| PC14/PC15 | | LSE 32.768 kHz crystal | |

### 2.4 Port D

| Pin | Dir | Function | Notes |
|---|---|---|---|
| PD0 | AF12 / in pull-down | LCD D2 (FSMC_D2) / keypad column 2 | shared, section 9 and 10 |
| PD1 | AF12 / in pull-down | LCD D3 (FSMC_D3) / keypad column 3 | |
| PD2 | out | keypad row 1 drive | |
| PD3 | out | keypad row 2 drive (also the side-key row) | |
| PD4 | AF12 | LCD RD (FSMC_NOE) | |
| PD5 | AF12 | LCD WR (FSMC_NWE) | |
| PD6 | out | LCD chip select | GPIO, active low (not an FSMC NE pin) |
| PD7 | out | SPI flash chip select | idle high. (This pin is FSMC_NE1 but is not used as such) |
| PD8 | out | LCD backlight (DM-1701, RT84, MD-UV380 5 W) | high = on, PWM by DMA (9.3). On the Plus 10 W this pin is the PA band select instead |
| PD9 | out | RX audio source multiplexer | default high (HR_C6000 line out to the speaker amp); driven low to route the AT1846S audio output directly (used while sending AFSK audibly). Polarity meaning unverified |
| PD12 | AF12 | LCD RS / D-C (FSMC_A17) | 0 = command, 1 = data |
| PD13 | out | LCD reset | active low |
| PD14 | AF12 / in pull-down | LCD D0 (FSMC_D0) / keypad column 0 | |
| PD15 | AF12 / in pull-down | LCD D1 (FSMC_D1) / keypad column 1 | |

### 2.5 Port E

| Pin | Dir | Function | Notes |
|---|---|---|---|
| PE0 | out | green LED | high = on |
| PE1 | out | red LED | high = on |
| PE2 | out | HR_C6000 config SPI chip select (U_CS) | idle high, bit-banged port |
| PE3 | out | HR_C6000 config SPI clock | bit-banged, idles high |
| PE4 | out | HR_C6000 config SPI MOSI | |
| PE5 | in | HR_C6000 config SPI MISO | |
| PE6 | out | HR_C6000 PWD (power down) | high = powered down, low = running. Reset state high |
| PE7 | AF12 / in pull-down | LCD D4 (FSMC_D4) / keypad column 4 | |
| PE8 | AF12 / in pull-down | LCD D5 / keypad column 5 | |
| PE9 | AF12 / in pull-down | LCD D6 / keypad column 6 | |
| PE10 | AF12 / in pull-down | LCD D7 / keypad column 7 | |
| PE11 | in, pull-up | PTT (side) | active low |
| PE12 | in, pull-up | external PTT (accessory jack) | active low |
| PE14 | in, pull-up, EXTI14 both edges | rotary encoder A | sampled with B on every edge of either (10.3) |

### 2.6 Port H

| Pin | Function |
|---|---|
| PH0 / PH1 | HSE external clock input (bypass) |

### 2.7 Reset-time output levels

Set before configuring as outputs: C6000 CS and PWD high; C6000 CLK/MOSI low; LEDs off; both RX 5 V switches, all
keypad rows, power latch, mic power low; PA enables, PA band select and beep low; voice CS, speaker mute and amp enable
high; backlight low; RX audio mux and flash CS high; CTCSS PWM high. The application drives the power latch high as its
first action, otherwise the radio turns off when the user releases nothing more than the power switch transient.

Transmitter safe state (`board_rf_safe`, [crash-log.md](crash-log.md) 8), at every fault, reset and stub entry and first
in `main()`: PA_EN1 (PC5) and PA_EN2 (PC4) low, PA band select (PC6) low, the APC DAC (PA4, DAC1 channel 1) 0 when its
clock is on. The AT1846S and HR_C6000 keep their state until the next start.


---

## 3. Interrupts, DMA, timers

### 3.1 Interrupt priorities

RTOS kernel at the lowest priority; RTOS "max syscall" priority 4: ISRs that call RTOS APIs must be at 4..15. No
interrupt uses 4. The level is even so a part with 3 priority bits can hold it (14.1, [gotchas.md](gotchas.md) 13.11).

| IRQ | Priority | Source |
|---|---|---|
| TIM8_TRG_COM_TIM14 | 3 | 1 kHz HAL/application time base (TIM14) |
| EXTI0 | 5 | HR_C6000 timeslot interrupt (PC0) |
| EXTI1 | 5 | HR_C6000 system interrupt (PC1) |
| EXTI2 | 5 | HR_C6000 RF TX interrupt (PC2) |
| TIM6_DAC | 5 | AFSK bit clock (TIM6), DAC underrun |
| EXTI15_10 | 6 | rotary encoder A (PE14, EXTI14) and B (PB11, EXTI11) |
| DMA1_Stream0 | 6 | I2S3 RX |
| DMA1_Stream5 | 6 | I2S3ext TX |
| USART1 | 6 | GPS |
| OTG_FS | 6 | USB |
| DMA2_Stream1 | 7 | TIM1 CC1 backlight pattern |
| DMA2_Stream2 | 7 | USART1 RX |
| DMA2_Stream4 | 7 | ADC1 |
| PendSV | 7 | RTOS |

The three HR_C6000 interrupts only matter in DMR mode; in FM they can be ignored.

### 3.2 DMA

| Controller / stream / channel | Peripheral | Direction | Width | Mode | Priority |
|---|---|---|---|---|---|
| DMA1 S0 ch0 | SPI3_RX (I2S3 RX) | P->M | 16 bit | circular | low |
| DMA1 S5 ch2 | I2S3_EXT_TX | M->P | 16 bit | circular | low |
| DMA2 S0 ch0 | memory-to-memory (frame buffer -> LCD data address) | M->M | 8 bit, FIFO on | normal | low |
| DMA2 S1 ch6 | TIM1_CH1 (backlight pattern -> GPIO BSRR) | M->P | 32 bit | circular | high |
| DMA2 S2 ch4 | USART1_RX | P->M | 8 bit | circular | low |
| DMA2 S4 ch0 | ADC1 | P->M | 16 bit | circular | low |

Memory-to-memory transfers on the F4 are only possible on DMA2. The frame buffer source must be in main SRAM (not CCM).

### 3.3 Timers

| Timer | Clock | Setting | Use |
|---|---|---|---|
| TIM1 | 72 MHz | prescaler 0, period 7100 (4000 in eco), CC1 timing mode, DMA request on CC1 | backlight software PWM: about 10.1 kHz DMA rate, 100 steps -> about 101 Hz PWM |
| TIM2 | 72 MHz | prescaler 1024, period 1024 | configured, no use found (unverified) |
| TIM3 | 72 MHz | prescaler 1023, period 1000, TRGO on update | ADC1 trigger, about 70.2 Hz scan rate |
| TIM4 | 72 MHz | prescaler 7200, period 1000 | configured, no use found (unverified) |
| TIM6 | 72 MHz | prescaler 1, period 30000 -> 1200 Hz; prescaler 7 -> 300 Hz | AFSK (APRS) bit clock |
| TIM14 | 72 MHz | 1 MHz count, period 1000 | 1 kHz time base |


---

## 4. HR_C6000 connection

### 4.1 Overview

| Function | MCU side | Notes |
|---|---|---|
| Configuration port (U_SPI) | bit-banged on PE2 (CS), PE3 (CLK), PE4 (MOSI), PE5 (MISO) | 8-bit transfers, MSB first |
| Voice port (V_SPI) | SPI2 hardware on PB13/14/15, CS on PB12 (GPIO) | used only for the 27-byte vocoder frame buffer (page 3) |
| Audio | I2S3 full duplex, MCU is slave | 8 kHz PCM both ways |
| Interrupts | PC0 timeslot, PC1 system, PC2 RF TX, falling edge | |
| Power | PE6 = PWD, high = sleep | |
| Clock | chip XTALI from a board oscillator | the PLL values used (0x0B = 0x28, 0x0C = 0x33) are the datasheet's row for a 29.4912 MHz XTALI giving 49.152 MHz. Oscillator frequency on the board unverified |

### 4.2 Configuration port timing

Mode 3 equivalent: clock idles high; for each bit MOSI is set, CLK driven low, MISO sampled, CLK driven high (the chip
latches on the rising edge). No added delays were needed at 72 MHz with GPIO writes (about 1-2 MHz effective). CS low
for the whole transaction.

Framing (same as the GD-77 family, see `modem-hrc6000.md` 1.1):

| Transaction | Bytes sent |
|---|---|
| write one register | page, register, value |
| read one register | page OR 0x80, register, dummy; data comes back in the third byte |
| write extended (11-bit) address | page OR 0x40, address bits 7:0, address bits 10:8, value |
| block write | page, start register, n data bytes (up to about 130 used) |
| block read | page OR 0x80, start register, n dummy bytes (up to 0x60 used) |

The port is shared by task code and ISRs: transactions must be atomic with respect to the HR_C6000 ISRs (mask or
lock around each transaction).

### 4.3 Voice port

SPI2 master, 8 bit, MSB first, CPOL = 1, CPHA = first edge (mode 2), prescaler 8 -> 4.5 MHz, software CS on PB12.
Framing: write = page 0x03, register 0x00, then 27 AMBE bytes; read = 0x83, 0x00, then 27 bytes clocked in (the data
follows the two header bytes). Up to 32 data bytes per transfer.

### 4.4 I2S audio

| Item | Value |
|---|---|
| Peripheral | SPI3 in I2S mode, slave, Philips standard, 16-bit data in a 32-bit frame (16B extended), CPOL high, MCLK not output |
| Clock and WS | driven by the HR_C6000 (its codec runs at 12.288 MHz) |
| Rate | 8 kHz |
| RX (PC12, I2S3_SD) | PCM from the HR_C6000 codec ADC (microphone) |
| TX (PC11, I2S3ext_SD) | PCM to the HR_C6000 codec DAC (decoded voice, beeps, voice prompts) |
| Buffers | double buffered blocks of 160 samples (20 ms), half and full transfer interrupts |
| Start | start the DMA right after a WS rising edge (wait for WS low, then high, then low, then high) so the channels line up |

The HR_C6000 codec register 0xE0 value 0xC9 says "I2S slave" in its documentation while the STM32 is also slave;
in practice the chip drives CK and WS on this board (unverified which register bit makes it master).

### 4.5 Power and start-up

1. PWD low (PE6), wait at least 10 ms.
2. Program the PLL while running from XTALI, switch to the PLL. The datasheet asks for more than 500 us between
   writing 0x0B/0x0C and switching 0x0A back to the PLL; a driver should wait.
3. Continue with the init sequence in `modem-hrc6000.md` section 3.

Power down: PWD high. The microphone power (PA13) is usually switched with the receiver; some external speaker-mics
generate false PTT events when their bias is removed, so keep PA13 high during receive power saving.


---

## 5. AT1846S RF transceiver

### 5.1 Bus and part

| Item | Value |
|---|---|
| Count | one AT1846S covers both bands on the DM-1701 / RT84 / MD-UV380 (the MD-2017 has two, see 17) |
| Bus | I2C3 hardware (PA8 SCL, PC9 SDA), 400 kHz, fast mode duty 2 |
| 7-bit address | 0x2E (the datasheet address with SEN high; 0x71 if SEN were low). Shifted 8-bit form 0x5C |
| Write | address+W, register, data high byte, data low byte |
| Read | address+W, register, then repeated transaction address+R, 2 bytes (high first). A stop between the two works |
| Retries | the bus occasionally NAKs; retry up to 3 times with 1 ms between |
| Reference | 26 MHz assumed (reg 0x04 bit0 = 0 and reg 0x30 bit14 = 1 select 25.6/26 MHz and 26/13 MHz, i.e. 26 MHz). Unverified on the board |
| Paging | reg 0x7F selects the register page (0 or 1); page 1 holds the AGC table |

Caching the last value written per register and page avoids redundant I2C traffic (writes that do not change a value
can be skipped; read-modify-write can use the cache). I2C3 is used from task code and, for burst-by-burst RF
switching, from ISRs: an ISR must not start a transfer while the task owns the bus (check the peripheral state first and
skip the action rather than block).

### 5.2 Init sequence (data)

"wait N ms" rows are delays, not register writes.

| Step | Register = value | Purpose |
|---|---|---|
| 1 | 0x30 = 0x0001 | soft reset |
| 2 | wait 50 ms | |
| 3 | 0x30 = 0x0004 | power on (pdn_reg) |
| 4 | 0x04 = 0x0FD0 | clock mode 25.6/26 MHz |
| 5 | 0x0A = 0x7C20 | default value |
| 6 | 0x13 = 0xA100 | undocumented |
| 7 | 0x1F = 0x1001 | GPIO6 = squelch out, GPIO1 = CSS out |
| 8 | 0x31 = 0x0031 | undocumented (vendor value) |
| 9 | 0x33 = 0x44A5 | AGC number |
| 10 | 0x34 = 0x2B89 | RX digital gain |
| 11 | 0x41 = 0x4122 | digital voice gain (bits 6:0); upper bits as found |
| 12 | 0x42 = 0x1052 | VOX shut threshold (per the RDA1846 manual) |
| 13 | 0x43 = 0x0100 | FM deviation |
| 14 | 0x44 = 0x07FF | RX / TX gain controls |
| 15 | 0x3A = 0x00C3 | squelch / voice channel config |
| 16 | 0x59 = 0x0B90 | deviation settings |
| 17 | 0x47 = 0x7F2F | undocumented (same as other AT1846S radios) |
| 18 | 0x4F = 0x2C62 | undocumented |
| 19 | 0x53 = 0x0094 | undocumented (vendor value) |
| 20 | 0x54 = 0x2A3C | undocumented (vendor value) |
| 21 | 0x55 = 0x0081 | undocumented (vendor value) |
| 22 | 0x56 = 0x0B02 | squelch detection time |
| 23 | 0x57 = 0x1C00 | bypass RSSI low-pass filter |
| 24 | 0x58 = 0x9CDD | filter settings |
| 25 | 0x5A = 0x06DB | unknown |
| 26 | 0x63 = 0x16AD | pre-emphasis bypass threshold |
| 27 | 0x0F = 0x8A24 | unknown |
| 28 | 0x05 = 0x8763 | unknown (also written before every frequency change) |
| 29 | 0x30 = 0x40A4 | prepare calibration |
| 30 | 0x30 = 0x40A6 | calibration enable |
| 31 | wait 100 ms | |
| 32 | 0x30 = 0x4006 | calibration done |
| 33 | wait 10 ms | |
| 34 | 12.5 kHz table (5.4) | default bandwidth |
| 35 | wait 200 ms | |
| 36 (after the HR_C6000 init) | 0x15 = 0x1100 | IF tuning bits 12:9 |

DTMF tone registers (0x67-0x76) are not needed: DTMF is generated by the HR_C6000.

### 5.3 Register 0x30 values used

Bit meanings (datasheet): 14 xtal_mode (1 = 26/13 MHz), 13 filter_band_sel (1 = 25 kHz analog filter), 12 band_mode
(1 = 25 kHz DSP), 7 mute, 6 tx_on, 5 rx_on, 3 sq_on, 2 pdn_reg, 1 chip_cal_en, 0 soft_reset.

| State | 12.5 kHz | 25 kHz |
|---|---|---|
| idle (RX and TX off), before a frequency write | 0x6006 | 0x7006 |
| receiver off at power-down | 0x4006 | 0x7006 |
| RX on | 0x6026 | 0x7026 |
| FM TX | 0x6046 | 0x7046 |
| DMR TX | 0x60C6 | 0x60C6 (same value for both) |

The 12.5 kHz setting keeps filter_band_sel = 1 (the 25 kHz analog filter) with the 12.5 kHz DSP mode, which is what
works on these radios. Bit 1 (chip_cal_en in the datasheet) stays set in normal operation, as found.

### 5.4 Bandwidth tables (data)

Send the whole table, then update reg 0x30: keep the other bits, set bits 13:12 to 11 (25 kHz) or 10 (12.5 kHz), clear
tx_on/rx_on, then set rx_on.

| Register | 12.5 kHz | 25 kHz | Purpose |
|---|---|---|---|
| 0x3A | 0x44CB | 0x40CB | bandwidth related squelch config |
| 0x15 | 0x1100 | 0x1F00 | IF tuning bits 12:9 |
| 0x32 | 0x4495 | 0x7564 | AGC target power |
| 0x3A | 0x00C3 | 0x00C3 | modu_det_sel (rewritten) |
| 0x59 | 0x0B90 | 0x0B90 | deviation; bits 5:0 are the AT1846S CTCSS/DCS deviation. Not doubled for 25 kHz: the 25 kHz mode already doubles it (0x0BA0 measured 4.0× the 12.5 kHz tone deviation on air, 0x0B90 gives 2.0×) |
| 0x3F | 0x29D1 | 0x29D1 | rssi3_th |
| 0x3C | 0x1B34 | 0x1B34 | pk_det_th |
| 0x48 | 0x19B1 | 0x1E38 | noise1_th |
| 0x60 | 0x0F17 | 0x0F17 | noise2_th |
| 0x62 | 0x1425 | 0x3767 | modu_det_th |
| 0x65 | 0x2494 | 0x248A | th_sif for squelch RSSI detect |
| 0x66 | 0xEB2E | 0xFF2E | rssi_comp and AFC range |
| 0x7F | 0x0001 | 0x0001 | page 1 |
| p1:0x06 | 0x0014 | 0x0024 | AGC table |
| p1:0x07 | 0x020C | 0x0214 | AGC table |
| p1:0x08 | 0x0214 | 0x0224 | AGC table |
| p1:0x09 | 0x030C | 0x0314 | AGC table |
| p1:0x0A | 0x0314 | 0x0324 | AGC table |
| p1:0x0B | 0x0324 | 0x0344 | AGC table |
| p1:0x0C | 0x0344 | 0x0384 | AGC table |
| p1:0x0D | 0x1344 | 0x1384 | AGC table |
| p1:0x0E | 0x1B44 | 0x1B84 | AGC table |
| p1:0x0F | 0x3F44 | 0x3F84 | AGC table |
| p1:0x12 | 0xE0EB | 0xE0EB | AGC table |
| 0x7F | 0x0000 | 0x0000 | back to page 0 |

### 5.5 Mode tables (data)

FM (analog) mode:

| Register = value | Purpose |
|---|---|
| 0x33 = 0x44A5 | AGC number |
| 0x41 = 0x4431 | digital voice gain |
| 0x42 = 0x10F0 | VOX shut threshold |
| 0x43 = 0x00A9 | FM deviation |
| 0x58 = 0xBC85 | high/low pass filters on, **de-emphasis off** (de-emphasis is done in the HR_C6000) |
| 0x44 = 0x06CC | internal volume about 80 % |
| 0x3A = 0x00C3 | modu_det_sel |
| 0x40 = 0x0030 | undocumented; bit 0 = pass low-frequency audio. Cleared for FM |

DMR mode (the HR_C6000 needs the raw 4FSK baseband):

| Register = value | Purpose |
|---|---|
| 0x40 = 0x0031 | undocumented; bit 0 set passes low-frequency audio (needed for 4FSK) |
| 0x15 = 0x1100 | IF tuning |
| 0x32 = 0x4495 | AGC target |
| 0x3A = 0x00C3 | no mic input to the AT1846S modulator |
| 0x3C = 0x1B34 | pk_det_th |
| 0x3F = 0x29D1 | rssi3_th |
| 0x41 = 0x4122 | digital voice gain |
| 0x42 = 0x1052 | VOX shut threshold |
| 0x43 = 0x0100 | deviation |
| 0x48 = 0x19B1 | noise1_th |
| 0x58 = 0x9CDD | all audio filters off |
| 0x44 = 0x07FF | AF gain 100 %. Lower values (e.g. 0x00CF-ish) give better BER on strong signals but fail to decode some hotspots; the DM-1701 needs more AF gain than the MD-UV380 |

### 5.6 Frequency setting

The synthesizer word is in units of 1/16 kHz (62.5 Hz): word = f(Hz) / 62.5 = f(MHz) x 16000. Registers: 0x29 holds
word bits 29:16 (in bits 13:0), 0x2A holds bits 15:0. Example from the datasheet: 409.75 MHz -> 0x640960 -> 0x29 =
0x0064, 0x2A = 0x0960.

Write sequence (RX or TX frequency):

| Step | Action |
|---|---|
| 1 | 0x30 = idle value (5.3) |
| 2 | 0x05 = 0x8763 ("normal" frequency mode) |
| 3 | 0x29 = high word |
| 4 | 0x2A = low word |
| 5 | 0x49 = 0x0C15 (squelch open/shut thresholds; see 5.9) |
| 6 | 0x30 = RX on value |
| 7 | set the PA DAC: the TX drive value when this was the TX frequency, 0 when RX |

TX frequency offset (empirical): with the normal TX sequence (RX frequency written, then TX frequency written with RX
on, then TX on) the carrier is about 3 kHz low; RadioFoundry adds +3.00 kHz (12.5 kHz) or +6.15 kHz (25 kHz FM) to
the TX word. With a different sequence (frequency written with the receiver off, then straight to TX) there is no
offset and adding it puts the carrier +2.93 kHz high (measured on air). The cause is unknown; a new driver should
measure the actual offset of its own sequence.

Band selection for the receive front end: VHF band -> PA5 high, PA2 low; UHF -> PA2 high, PA5 low (5.8, section 6).

### 5.7 Sub-audio (CTCSS / DCS)

| Register | Field | Use |
|---|---|---|
| 0x4A | ctcss1_freq = Hz x 100 | TX CTCSS tone; set to 13440 (134.4 Hz, the DCS bit rate) for DCS |
| 0x4D | ctcss2_freq = Hz x 100 | RX CTCSS tone to detect |
| 0x4B / 0x4C | cdcss code bits 23:16 / 15:0 | the 23-bit Golay word of the DCS code, both RX and TX |
| 0x4E bits 10:9 | ctcss/cdcss_sel for TX | 11 = CTCSS, 10 = DCS, 00 = none. Bits 8:6 = cdcss_sel: bit 8 = inverted, bit 7 = 1, bit 6 = 0 (23-bit) |
| 0x3A bits 4:0 | RX detect select | bit 3 = CTCSS2, bit 1 = normal DCS, bit 2 = inverted DCS (bit 0 CTCSS1, bit 4 phase shift unused) |
| 0x5B | ctcss match / unmatch thresholds (high / low byte) | both bytes = (25000 - tone_reg) / 1000, or 1 if tone_reg > 24000 (tone_reg = Hz x 100) |
| 0x1C | flags | bit 9 ctcss1_cmp, bit 8 ctcss2_cmp, bit 7 cdcss positive match, bit 6 cdcss negative match, bit 0 sq_cmp |

Recipes (as values):

| Action | Writes |
|---|---|
| TX CTCSS on | 0x4A = tone; 0x4D = 0; 0x4B = 0; 0x4C = 0; 0x4E high byte: keep bits under mask 0xF9, set 0x06 |
| TX sub-audio off | 0x4A = 0; 0x4D = 0; 0x4E high byte: keep mask 0xF9, set 0x00 |
| TX DCS | 0x4A = 13440; 0x4D = 0; 0x4B = Golay bits 23:16; 0x4C = bits 15:0; 0x4E high byte: keep mask 0x38, set 0x04 (normal) or 0x05 (inverted) |
| RX CTCSS | 0x4A = 0; 0x4B = 0; 0x4C = 0; 0x4D = tone; 0x5B = threshold in both bytes; 0x3A low byte: keep mask 0xE0, set 0x08 |
| RX DCS | 0x4A = 13440; 0x4D = 0; 0x4B/0x4C = Golay word; 0x4E high byte as TX DCS; 0x3A low byte: keep mask 0xE0, set 0x02 (normal) or 0x04 (inverted) |
| RX off | 0x4A = 0; 0x4D = 0; 0x4E high byte keep mask 0xF9, set 0x00 |
| check | read 0x1C: require bit 0 (sq_cmp); for DCS also bit 7 (normal) or bit 6 (inverted); for CTCSS also bit 8 (high byte bit 0) |

DCS word: the 9-bit code (octal digits as 3-bit groups, e.g. 023 -> 0x013), with bit 11 set (the fixed "100" marker:
word bits 11:9 = 100), and the 11 Golay(23,12) parity bits of the 12-bit value (marker + code) in bits 22:12. Generator
polynomial x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75). Compute it; no table is needed.

The chip sends the word MSB first (bit 22 first) and compares received bits in the same order, while DCS sends code
bit 0 first. So 0x4B/0x4C take the word **bit-reversed**: 023 -> 0x763813 -> reversed 0x640E37 -> 0x4B = 0x0064,
0x4C = 0x0E37. The unreversed word (0x0076 / 0x3813, as the AT1846S programming guide shows) went out as D023 read backwards on
the SDR bench, and the radio accepted no other radio's DCS; with the reversed word it sends and receives D023N.

DCS turn-off tail (`radio-features.md` 3.3a): 0x4E bits 10:9 from 10 (DCS) to 11 (CTCSS) for the tail. 0x4A already
holds 13440, so the chip sends 134.4 Hz as a CTCSS tone; back to 10 (and the polarity in bits 8:6, kept) on PTT again.
The chip's own tail elimination (0x30 bit 11, 0x4E bits 15:14 phase shift) is a CTCSS feature and stays off. Not yet
measured on the SDR bench.

### 5.8 Tones, voice channel and TX audio source

| Register | Use |
|---|---|
| 0x3A bits 14:12 | voice_sel: 000 nothing (the mode tables' 0x00C3), 001 tone1, 010 tone2, 011 tone1 + tone2, 100 microphone (AT1846S programming guide, TX voice channel select). Bits 6:4 are squelch / sub-audio detect enables (gotchas 3.10) |
| 0x7A bit 14 | single tone enable (bit 15 DTMF enable) |
| 0x35 | tone1 frequency = Hz x 10 |
| 0x36 | tone2 frequency = Hz x 10 |
| 0x79 = 0xC000 | single tone select |
| 0x57 bit 0 | audio feedback (TX audio looped to the speaker) |
| 0x59 bits 15:6 | tone deviation; 0x1C used for tones (about 3.1 kHz peak; 0x40 measured 7.2 kHz on air), restore the saved value after |
| 0x41 bits 6:0 | voice TX gain; save before tones, restore after |

On these radios FM microphone audio is processed and modulated by the HR_C6000 (two-point modulation), not by the
AT1846S. The AT1846S tone generator is still used for the 1750 Hz burst and for AFSK (with the HR_C6000 microphone input
muted, HR_C6000 0xE0 bit 6 cleared): voice_sel = tone1 makes the AT1846S modulate it, as it does its CTCSS. With
voice_sel left at 000 the carrier went out unmodulated (gotchas 3.10); the fixed path is verified on air (HIL
2026-10-10: the burst, the roger beep and APRS AFSK, `docs/testing/on-air-coverage.md`).

Morse keying for the hotspot's CW ID (`tx_tone_key`, `boards/dm1701/radio_dm1701.c`): with the tone set up by
`tx_tone`, key up writes the 0x59 tone deviation bits 15:6 to 0 and key down back to the tone value (0x1C) (bits 5:0 kept from the
saved value), so the carrier stays up and the tone switches without a retune. On air: `test_hotspot_cwid` checks the
Morse tone (`docs/testing/on-air-coverage.md`).

RadioFoundry's `tx_tone(hz)` (`boards/dm1701/radio_dm1701.c`, the roger beep and the 1750 Hz burst of `radio-features.md`
3.3) uses tone1 with 0x79 = 0xC000 and tone deviation 0x1C, and mutes the HR_C6000 microphone path (page 4 0xE0
bit 6). `tx_tone(0)` and every return to receive restore the saved registers.

DTMF (`tx_dtmf`, `radio-features.md` 3.7) is the same path with tone2 as well: 0x35 = the low tone, 0x36 = the high
tone, voice_sel = 011 (tone1 + tone2), single tone select and enable as above (0x7A bit 15, the chip's own DTMF mode,
times each digit itself and isn't used). The deviation is `DTMF_DEV` in 0x59 bits 15:6, 0x1C like the single tone
until measured for the pair. A new digit only rewrites 0x35 and 0x36.

### 5.9 RSSI, noise, squelch

| Register | Field |
|---|---|
| 0x1B high byte | rssi_db (1 dB units) |
| 0x1B low byte | noise_db (1 dB units) |
| 0x1A low byte | mic_db (0.5 dB units), not used (mic level comes from the MCU ADC on these radios) |
| 0x49 = 0x0C15 | th_h_sq = 0x18, th_l_sq = 0x15 (hardware squelch thresholds, 1 dB units) |

Conversion to dBm (measured): UHF dBm = rssi - 151; VHF dBm = -164 + rssi x 32 / 27.
The same formulas apply to noise_db. A software squelch on noise_db works well: carrier present when noise_db is below a
threshold (RadioFoundry: 70 minus 3 per squelch step, 3 dB hysteresis), sampled every 25 ms.

Roaming reads this RSSI as the site signal ([dmr-roaming.md](dmr-roaming.md) section 3). A survey visit relies on the
AT1846S RSSI and the HR_C6000's sync acquisition needing well under 100 ms on a continuous BS outbound (a burst every
30 ms); four 25 ms readings follow.


### 5.10 APRS AFSK transmit

RadioFoundry's `hal_afsk_t` (`boards/dm1701/afsk_dm1701.c`, `aprs.md` 5) uses the AT1846S tone generator as the voice source,
as for the roger beep. TIM6 (APB1 timer clock 72 MHz, prescaler 2, `ARR = clk / 2 / 1200 - 1`, interrupt priority
5, no RTOS calls) interrupts at every symbol boundary. It writes the next tone to register 0x35 when the tone
changes: one I2C write of about 0.1 ms in an 833 us symbol. The radio core keeps the I2C bus free meanwhile. At the
end it force-writes 0x35, because the interrupt bypassed the driver's cache. The level (0-100, 50 default) scales the
tone deviation register 0x59 around the driver's value.

The tone generator is the voice source only with reg 0x3A bits 14:12 (voice_sel) = 001 and 0x7A bit 14 (single tone)
set. An earlier build wrote bits 6:4 and keyed a silent carrier (`gotchas.md` 3.10, fixed in 694249b).
`tests/hil/test_aprs.py` measured the result on air (`aprs.md` 13).

### 5.11 Receiver sleep

RadioFoundry's `rx_sleep(true)` (receiver battery save, `radio-features.md` 5.4) puts the AT1846S in idle and switches the
receive supply off. `rx_sleep(false)` retunes.

---

## 6. RF path control (PA, LNA, band)

| Signal | Pin | RX | TX VHF | TX UHF | Off |
|---|---|---|---|---|---|
| UHF RX 5 V | PA2 | high if UHF | low | low | low |
| VHF RX 5 V | PA5 | high if VHF | low | low | low |
| TX supply (PA enable 1) | PC5 | low | high | high | low |
| APC loop enable (PA enable 2) | PC4 | low | high | high | low |
| PA band select | PC6 (PD8 on Plus 10 W) | don't care | low | high | low |
| APC reference | DAC1 / PA4 | 0 | drive value | drive value | 0 |
| HR_C6000 PWD | PE6 | low | low | low | high |

Ordering:

- Enter TX: both RX supplies off; configure the HR_C6000 for FM TX or DMR; AT1846S reg 0x30 to the TX value; then PA
  enable 1 high, PA band select, then PA enable 2 high. The DAC drive is set when the TX frequency is written.
- Enter RX: PA enable 1 and 2 low first; RX supply for the band; AT1846S reg 0x30 to the RX value; HR_C6000 to FM RX or
  DMR.
- Burst-by-burst DMR TX (the PA follows the bursts), keyed 1 ms after the RF TX interrupt (it comes about 1.28 ms
  before the slot boundary; keyed at once, the PA cut the end of the inbound burst an SFR receives in the other slot).
  Two sequences:
  - Direct, when the synthesizer already holds the TX frequency (SFR, simplex) and the band is UHF: reg 0x30 straight
    from RX to DMR TX, HR_C6000 4:0x04 (MOD1 offset) = calibrated value + 42, both RX supplies off, then the PA. Back to
    RX: the PA enables low, 4:0x04 = calibrated value, the RX supply, reg 0x30 RX on. RX straight to TX puts the
    carrier about 3.07 kHz low, and a frequency write can't correct it (the AT1846S applies one only coming out of
    idle); 42 steps of about 75 Hz put it back (measured at 461.1375 MHz: +16..+60 Hz). The idle step that the other
    sequence takes every slot makes the synthesizer settle again, and an SFR lost the first and last frames of its
    inbound bursts to it: about 190 bit errors and 5-10 lost frames per 72 repeated frames, against about 60 and none
    direct (SDR bench, 2026-10-19).
  - Through idle, otherwise (VHF, where 3.07 kHz is more steps than the register holds; RX and TX frequencies that
    differ): reg 0x30 receiver off, the TX frequency, reg 0x30 DMR TX, both RX supplies off, then the PA. Back to RX,
    the PA enables low, reg 0x30 receiver off, the RX frequency if it differs, the RX supply for the band, reg 0x30 RX
    on. Going straight back to RX after a burst entered from idle leaves the receiver about 3 kHz off.

  The receiver must work between bursts (an SFR's inbound slot, `sfr.md` 3). Dropping the PA earlier than the next
  timeslot interrupt (28.5-30 ms after the RF TX interrupt) did not reduce the inbound errors. The "transmitting" state of the radio must therefore be taken from the HR_C6000 slot state, not from the PA
  pins: the last burst of a call (the terminator) is still in flight after the PA logic has gone back to RX once.
- Power-down: PA enables low, both RX supplies low, mic power optionally low, HR_C6000 PWD high, AT1846S reg 0x30 =
  receiver off value.

Powering both receivers together and powering only the band in use both work; RadioFoundry powers only the band in use.


---

## 7. TX power control (DAC)

DAC channel 1 on PA4, 12-bit right aligned, output buffer on, no trigger: 0..4095. The DAC value sets the APC loop
reference: higher = more power. The calibration table gives 8-bit points scaled x16 to 12 bits (section 12).

Power levels from four calibrated curves (very low, low, mid, high), each interpolated against frequency (12.3):

| Level | Nominal (5 W radios) | DAC value |
|---|---|---|
| 0 | 50 mW | very low - 160, or 0 if very low <= 160 |
| 1 | 250 mW | very low |
| 2 | 500 mW | very low + (low - very low) x f0 |
| 3 | 750 mW | very low + (low - very low) x f1 |
| 4 | 1 W | low |
| 5 | 2 W | mid |
| 6 | 3 W | mid + (high - mid) x f2 |
| 7 | 4 W | mid + (high - mid) x f3 |
| 8 | 5 W | high |
| 9 | user | user-set DAC value |

Interpolation fractions (empirical):

| Radio | Band | f0 | f1 | f2 | f3 |
|---|---|---|---|---|---|
| DM-1701 / RT84 | VHF | 0.45 | 0.75 | 0.25 | 0.53 |
| DM-1701 / RT84 | 220 MHz | 0.45 | 0.75 | 0.25 | 0.53 (estimate, untested) |
| DM-1701 / RT84 | UHF | 0.46 | 0.73 | 0.14 | 0.36 |
| MD-UV380 5 W | VHF / 220 / UHF | 0.35 / 0.38 / 0.40 | 0.70 | 0.34 / 0.30 / 0.25 | 0.61 / 0.59 / 0.55 |
| MD-UV390 Plus 10 W | VHF / 220 / UHF | 0.58 / 0.58 / 0.55 | 0.83 / 0.83 / 0.75 | 0.21 / 0.21 / 0.17 | 0.45 / 0.45 / 0.43 |

On the 10 W radio levels 5-8 are 2 W, 3 W, 5 W, 10 W: level 5 = low + (mid - low) x f2, level 6 = low + (mid - low) x
f3, level 7 = mid, level 8 = high. Clamp to 4095.

Without a loaded calibration RadioFoundry drives a fixed DAC value of 300 (`DM1701_DAC_UNCALIBRATED` in
`boards/dm1701/radio_dm1701.c`), a very low drive.


---

## 8. Audio paths

| Path | Route |
|---|---|
| FM RX | antenna -> AT1846S demodulator -> AT1846S audio out -> HR_C6000 line in -> HR_C6000 FM feed-through (page 4 reg 0x36 = 0x02) with de-emphasis (0x34) -> HR_C6000 DAC line out -> RX audio mux (PD9 high) -> speaker amp |
| DMR RX | AT1846S baseband (filters off, reg 0x40 bit 0 set) -> HR_C6000 4FSK demodulator -> 27-byte AMBE frames over V_SPI -> MCU codec -> PCM over I2S -> HR_C6000 DAC -> amp |
| FM TX | mic (bias on PA13) -> HR_C6000 mic input (codec ADC, mic gain 0xE4) -> HR_C6000 FM processing -> two-point modulation outputs (MOD1 to the reference oscillator, MOD2 to the VCO) |
| DMR TX | mic -> HR_C6000 codec ADC -> PCM over I2S -> MCU codec -> AMBE frames over V_SPI -> HR_C6000 framing and two-point 4FSK modulation |
| Beeps / prompts | MCU-generated PCM (8 kHz sine synthesis, prompts decoded by the codec) -> I2S -> HR_C6000 DAC -> amp |
| Mic level / VOX | separate analog tap on PA3 (ADC) |
| Speaker | amp enable PB9 high and mute PB8 low to hear audio; amp off and mute high otherwise |

The RX audio mux on PD9 is driven low when the AT1846S output (e.g. its tone generator during audible AFSK) should go
straight to the amp (unverified).

APRS receive takes FM receive audio from the I2S stream (squelch gated, 8 kHz): no ADC pin sees the discriminator, but
the HR_C6000 puts its FM receive audio on I2S (21). The audio tap says so (`AUDIO_TAP_FM_RX`), and the core reads it
through the shared stream (`aprs.md` 5.3). The stream is gated by the FM feed-through (4:0x36), so it stays on
at volume zero (11.2). The HR_C6000's RX RAM carries the same audio ungated, for a later receive
path of the board's own.


---

## 9. Display

### 9.1 Panel and bus

| Item | Value |
|---|---|
| Panel | 160 x 128 color TFT, RGB565, 8-bit 8080 interface |
| DM-1701 visible area | 160 x 120: the panel's first 8 rows are hidden; RadioFoundry offsets all row addresses by 8 and clears rows 0-7 once |
| Controllers seen | an ST7735-class / Himax HX8353-E-class controller (types 0 and 1) and a different controller that needs the 0xFE/0xEF "inter command" unlock (types 2 and 3; GalaxyCore GC9106-like, unverified) |
| Interface | FSMC bank 1, sub-bank 1 (address 0x60000000), SRAM type, 16-bit width configured, byte accesses used on D0-D7 |
| Command address | 0x60000000 (A17 = 0 -> RS low) |
| Data address | 0x60040000 (with a 16-bit bank, HADDR bit 18 drives FSMC_A17 = PD12 = RS high) |
| Chip select | PD6, GPIO, active low, asserted around each burst |
| Reset | PD13, active low: hold low 20 ms, then high |
| FSMC BCR1 | MBKEN, MTYP = NOR (bit 3), MWID = 16 bit, FACCEN, WREN (value 0x10D9; bit 7 undocumented, unverified) |
| FSMC BTR1 | MBKEN-like value 0x00100517; the register bits written are MBKEN, MUXEN, MTYP_0, MWID_0, BURSTEN, WRAPMOD, CBURSTRW (meaning unclear, unverified) |
| Timing | a very short delay after each command/data byte was needed with byte-by-byte writes |
| Full refresh | 160 x 128 x 2 bytes = 40,960 bytes by DMA2 memory-to-memory to the data address |

After each burst RadioFoundry writes 0x00 to the data address so the shared data lines rest low before the keypad
is scanned.

The case's lens hides about 5 px at the left and right edges. RadioFoundry's safe-area insets default to 5 px left and right,
none top and bottom (`CONFIG_DISPLAY_INSET*` in `boards/dm1701/config.mk`, `ui.md` 1).

### 9.2 Controller type and orientation

The panel type is stored by the factory in SPI flash security register 3, byte 0x1D (read with command 0x48 at
address 0x00301D), low two bits:

| Value | DM-1701 meaning | MD-UV380 meaning |
|---|---|---|
| 0 | normal | rotated 180 degrees |
| 1 | rotated 180 degrees | normal |
| 2 | vertically flipped; treated as type 3 with RGB order | horizontally flipped |
| 3 | normal, BGR order | rotated 180 degrees |

MD-UV380 panels are always treated as RGB order. MADCTL (0x36) written at the end of init: 0x60 for type 1, 0xE0 for
type 2, 0xA0 for types 0 and 3. Color: RGB565 big-endian on the bus (high byte first); in BGR panels red and blue fields
are swapped.

On the MD-UV380, security register 3 byte 0x1C = 0xF4 identifies a Plus 10 W radio (seen, not used).

### 9.3 Init sequences (data)

Common: reset pulse (CS high, RST low 20 ms, RST high), CS low, then:

Types 2 and 3 (controller with inter-command unlock), command followed by parameter bytes:

| Command | Parameters |
|---|---|
| 0xFE | (none) |
| 0xEF | (none) |
| 0xB4 | 0x00 |
| 0xFF | 0x16 |
| 0xFD | 0x4F (type 2) or 0x40 (type 3) |
| 0xA4 | 0x70 |
| 0xE7 | 0x94, 0x88 |
| 0xEA | 0x3A |
| 0xED | 0x11 |
| 0xE4 | 0xC5 |
| 0xE2 | 0x80 |
| 0xA3 | 0x12 |
| 0xE3 | 0x07 |
| 0xE5 | 0x10 |
| 0xF0 | 0x00 |
| 0xF1 | 0x55 |
| 0xF2 | 0x05 |
| 0xF3 | 0x53 |
| 0xF4 | 0x00 |
| 0xF5 | 0x00 |
| 0xF7 | 0x27 |
| 0xF8 | 0x22 |
| 0xF9 | 0x77 |
| 0xFA | 0x35 |
| 0xFB | 0x00 |
| 0xFC | 0x00 |
| 0xFE | (none) |
| 0xEF | (none) |
| 0xE9 | 0x00 |
| wait 20 ms | |

Types 0 and 1 (ST7735-class):

| Command | Parameters | Meaning (ST7735 naming) |
|---|---|---|
| 0x11 | | sleep out |
| wait 120 ms | | |
| 0xB1 | 0x05, 0x3C, 0x3C | frame rate, normal mode |
| 0xB2 | 0x05, 0x3C, 0x3C | frame rate, idle |
| 0xB3 | 0x05, 0x3C, 0x3C, 0x05, 0x3C, 0x3C | frame rate, partial |
| 0xB4 | 0x03 | inversion control |
| 0xC0 | 0x28, 0x08, 0x04 | power control 1 |
| 0xC1 | 0xC0 | power control 2 |
| 0xC2 | 0x0D, 0x00 | power control 3 |
| 0xC3 | 0x8D, 0x2A | power control 4 |
| 0xC4 | 0x8D, 0xEE | power control 5 |
| 0xC5 | 0x1A | VCOM |
| 0x36 | 0x08 | MADCTL (overwritten below) |
| 0xE0 | 0x04, 0x0C, 0x07, 0x0A, 0x2E, 0x30, 0x25, 0x2A, 0x28, 0x26, 0x2E, 0x3A, 0x00, 0x01, 0x03, 0x13 | positive gamma |
| 0xE1 | 0x04, 0x16, 0x06, 0x0D, 0x2D, 0x26, 0x23, 0x27, 0x27, 0x25, 0x2D, 0x3B, 0x00, 0x01, 0x04, 0x13 | negative gamma |

Then for all types:

| Command | Parameters |
|---|---|
| 0x36 MADCTL | per 9.2 |
| 0x2A CASET | 0x00, 0x00, 0x00, 160 |
| 0x2B RASET | 0x00, 0x00, 0x00, 128 (DM-1701: 120 + 8) |
| 0x3A COLMOD | 0x05 (16 bit/pixel) |
| 0x11 | sleep out |
| 0x29 | display on |

Then CS high. Writing a band of rows: RASET start/end (DM-1701: add the 8-row offset), 0x2C RAMWR, pixel bytes.
Inversion: 0x21 / 0x20.

### 9.4 Backlight

PD8 (PC6 on Plus 10 W), active high. Dimming: a 101-entry table of 32-bit words is sent by DMA2 Stream1 (TIM1 CC1
request, circular, 100 transfers) to the port's BSRR register: entry 0 sets the pin, entry N resets it, all others are
0, so the duty is N %. 0 % and 100 % stop the DMA and drive the pin directly.


---

## 10. Keypad, side keys, encoder, PTT

### 10.1 Matrix

Rows are driven high one at a time (push-pull high for the row being read, input/floating for the others). Columns are
the eight LCD data lines read as inputs with pull-downs; a pressed key reads high. Sequence per scan:

1. Switch the eight data lines from FSMC to GPIO: first drive them low as outputs for a moment (about 10 loop
   iterations) to discharge them, then make them inputs with pull-down. Without the discharge step phantom key presses
   appear on some radios (a glitch when leaving FSMC mode).
2. For each row: drive it high, wait about 100 loop iterations to settle, read the columns, return the row to input.
3. The first key found wins (no multi-key support on the front keys).

DM-1701 / RT84 matrix:

| Row (pin) | D0 (PD14) | D1 (PD15) | D2 (PD0) | D3 (PD1) | D4 (PE7) | D5 (PE8) | D6 (PE9) | D7 (PE10) |
|---|---|---|---|---|---|---|---|---|
| Row 0 (PA6) | 1 | 4 | 7 | * | up arrow | right function key | left function key | - |
| Row 1 (PD2) | 2 | 5 | 8 | 0 | down arrow | red (back) | green (menu) | - |
| Row 2 (PD3) | 3 | 6 | 9 | # | - | top (orange) key | side key 2 | side key 1 |

The physical labels of the DM-1701's two "function" keys (right and left in the table, likely the P1/P2 or A/B keys)
are unverified. Side key 1 is the one just above the PTT (it is the one held with PTT to enter the bootloader); side key
2 is the lower one (unverified ordering on the RT84). The table's names are from before the bench check: section 20 has
the DM-1701's order, and the firmware names the side keys SK1 (D5), SK2 (D7) and SK3 (D6), top to bottom.

MD-UV380 matrix (for comparison):

| Row | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
|---|---|---|---|---|---|---|---|---|
| Row 0 | 1 | 2 | 3 | 4 | 5 | 6 | 0 | * |
| Row 1 | green | up | down | 7 | 8 | 9 | # | red |
| Row 2 | - | - | - | - | - | - | side key 2 | side key 1 |

The MD-UV380 has no orange key in the matrix.

Side keys are read in the same way (row 2 high, read D5/D6/D7) and can be held together with each other and with PTT.

### 10.2 PTT

PE11 (internal PTT) and PE12 (external PTT on the accessory jack): inputs with pull-ups, active low; either one means
PTT.

### 10.3 Rotary channel encoder

Quadrature, A on PE14, B on PB11 (both pull-up), with detents. The driver interrupts on both edges of B only
and counts one step per B edge (-1 if A equals B after the edge, else +1). One B edge per detent means two quadrature
steps per detent, resting where A equals B (00 and 11). Measured on the bench (2026-10-10, a76da0d): 10 slow and 10
fast detents each gave exactly 10 detents and 20 transitions, 11 counterclockwise gave -11 / 22, a wobble without a
detent gave 0 detents (14 transitions, 12 reversals), never an illegal state.

RadioFoundry interrupts on both edges of both lines (EXTI14 for A, EXTI11 for B, one handler) and feeds every sample of the two
levels to the table decoder in `drivers/input/quadrature`: each sample against the previous one is no change, one
step either way, or both lines changed (a missed sample, ignored). A detent counts only when the lines reach a rest
state with two steps the same way since the last one. The interrupt clears its pending flags before it reads the pins,
so an edge during the read interrupts again.

Why counting B edges went wrong (seen as missed steps and steps backward, 2026-10-10):

- Going one way, B is the first line to change when the knob leaves a detent. A light touch or a knob that settles
  back counts a step and then a step back: the focus jumps a row and returns.
- Contact bounce on B gives several edges. EXTI has one pending flag per line, so edges close together (or while the
  interrupt is masked or preempted) merge, and the handler reads the level after all of them: a rise-fall-rise reads
  "high" twice and counts two steps for one, never in step with the detent.
- Bounce on A was ignored, but A's level was sampled at B edges only, so a sample taken while A was still settling
  (a fast turn) gives the wrong direction.

The decoder makes all three harmless: bounce is a step and the same step back (net zero), merged edges are a repeated
level (no change), and only whole detents count. Clockwise is positive (forward): the next channel on Home, focus down in lists (confirmed on the radio).

### 10.4 Power switch

The volume knob's switch interrupts the battery sense line. The MCU holds its own supply with the latch (PA7 high).
When the battery reading drops below 5.5 V (11.1) the switch is treated as off: the firmware finishes its shutdown and
drives PA7 low, which removes power. This also means a deeply discharged battery looks like "switch off" (unverified
whether there is a second sense path).


---

## 11. Analog inputs

ADC1, 12-bit, PCLK2/8 = 9 MHz, scan of 4 ranks, 480-cycle sampling, triggered by TIM3 TRGO (about 70 Hz), DMA2
Stream4 circular into a 4-word array:

| Rank | Channel | Signal |
|---|---|---|
| 1 | IN0 (PA0) | volume pot |
| 2 | IN1 (PA1) | battery |
| 3 | IN3 (PA3) | mic level |
| 4 | IN16 | internal temperature sensor |

### 11.1 Battery

Voltage in 0.1 V units = raw / 37.5 on the DM-1701 / RT84 (raw / 40.3 on the MD-UV380), plus a user trim of -5..+10
(0.1 V steps). Thresholds (0.1 V): full 8.2 V; low-battery hysteresis 6.2 / 6.4 V; "power switch off" below 5.5 V.
Ignore readings for 1.5 s after power-up or wake (they settle); average with a long window (1000 samples) for display,
use the latest sample for decisions.

RadioFoundry's thresholds: `board.battery_low_mv` 6200, `battery_critical_mv` 6000 (`boards/dm1701/board.c`,
`radio-features.md` 5.1).

### 11.2 Volume pot

Log-taper pot read on PA0. Raw ranges (measured): DM-1701 min about 29, max about 2075,
breakpoint about 318; MD-UV380 22..2070, breakpoint 309; MD-UV390 Plus 10 W 19..2054, breakpoint 308; RT3S 23..2020,
breakpoint 317; MD-2017 32..2048, breakpoint 321. A two-segment linear approximation around the breakpoint maps it to
-31..+31 dB, with a "minimum = mute" zone at the bottom (with 5 counts of hysteresis).

### 11.3 Mic level

PA3 gives the microphone level; convert the raw value to dB (20 log10) for VOX.

RadioFoundry's `mic_level` (`boards/dm1701/power_dm1701.c`, VOX in `radio-features.md` 3.4) gives 0-100: 20 log10 of the raw
reading over the ADC's 72 dB, from the shared scale in `drivers/audio/miclevel/miclevel.h`. Not verified on the bench.

### 11.4 Temperature

Internal sensor: 943 counts at 25 C, 3.1 counts per C (datasheet nominal 0.76 V and 2.5 mV/C at
3.3 V reference). Uncalibrated, coarse.


---

## 12. Calibration data

### 12.1 Location and life cycle

| Copy | Location | Size |
|---|---|---|
| Factory | SPI flash security registers 1 and 2 (addresses 0x001000 and 0x002000, 256 bytes each, read with command 0x48 + 3 address bytes + 1 dummy byte) | 512 bytes |
| Working copy | SPI flash main array 0x010000 | 512 bytes |

At start-up: read the working copy; if bytes 0x1B0-0x1B7 are not the marker 00 25 00 40 00 45 01 40, read the factory
table, overwrite those 8 bytes with the marker, derive the very-low power row and apply the power conversion below, then
write the result to 0x010000. The firmware never writes the security registers. The STM32F4 stub can put a backup of
them back (`stubtool.py --write-secreg`, [stub-stm32f4.md](stub-stm32f4.md) 6.2), refusing a register whose one-time
lock bit is set; nothing ever writes status register 2.

Power conversion when deriving the working copy (empirical): very low = low x k0 (always), and if
converting: low x= k1, mid x= k2, high x= k3.

| Radio | Band | k0 | k1 | k2 | k3 |
|---|---|---|---|---|---|
| DM-1701 / RT84 | VHF | 0.61 | 1.00 | 0.85 | 1.30 |
| DM-1701 / RT84 | UHF | 0.65 | 1.00 | 0.86 | 1.70 |
| MD-UV380 5 W | VHF / UHF | 0.63 / 0.72 | 1.00 | 1.00 / 1.05 | 1.10 / 1.25 |
| MD-UV390 Plus 10 W | VHF / UHF | 0.35 / 0.40 | 0.65 / 0.70 | 1.25 / 1.20 | 1.40 / 1.35 |

### 12.2 Table layout (512 bytes)

UHF rows have 9 points (400-480 MHz), VHF rows 5 points. "Used" says whether the open firmware applies it.

| Offset | Size | Meaning | Used / how |
|---|---|---|---|
| 0x000 | 1 | VOX level 1 | no |
| 0x001 | 1 | VOX level 10 | no |
| 0x002 | 1 | RX low voltage (meaning unknown) | no |
| 0x003 | 1 | RX high voltage (meaning unknown) | no |
| 0x004 | 1 | RSSI reading at -120 dBm | read, not used (fixed formulas in 5.9) |
| 0x005 | 1 | RSSI reading at -70 dBm | read, not used |
| 0x006 | 2 | unknown | |
| 0x008 | 1 | unknown | |
| 0x009 | 1 | UHF reference oscillator tune | yes: signed offset = value - 128, see 12.4 |
| 0x00A | 2 | unknown | |
| 0x00C | 1 | VHF reference oscillator tune | yes, as UHF |
| 0x00D | 3 | unknown | |
| 0x010 | 9 | UHF high power | yes, DAC/16 |
| 0x019 | 5 | VHF high power | yes |
| 0x01E | 2 | unknown | |
| 0x020 | 9 | UHF low power | yes |
| 0x029 | 5 | VHF low power | yes |
| 0x02E | 2 | unknown | |
| 0x030 | 9 | UHF RX front-end tuning | no (no tuning DAC path used on this radio) |
| 0x039 | 5 | VHF RX front-end tuning | no |
| 0x03E | 2 | unknown | |
| 0x040 | 9 | UHF squelch level 9 open | no |
| 0x049 | 7 | unknown | |
| 0x050 | 9 | UHF squelch level 9 close | no |
| 0x059 | 7 | unknown | |
| 0x060 | 9 | UHF squelch level 1 open | no |
| 0x069 | 7 | unknown | |
| 0x070 | 9 | UHF squelch level 1 close | no |
| 0x079 | 7 | unknown | |
| 0x080 | 16 | unknown | |
| 0x090 | 9 | UHF CTCSS deviation, 67 Hz | no (fixed HR_C6000 values, see 12.5) |
| 0x099 | 2 | unknown | |
| 0x09B | 5 | VHF CTCSS deviation, 67 Hz | no |
| 0x0A0 | 9 | UHF CTCSS deviation, 151.4 Hz | no |
| 0x0A9 | 2 | unknown | |
| 0x0AB | 5 | VHF CTCSS deviation, 151.4 Hz | no |
| 0x0B0 | 9 | UHF CTCSS deviation, 254.1 Hz | no |
| 0x0B9 | 2 | unknown | |
| 0x0BB | 5 | VHF CTCSS deviation, 254.1 Hz | no |
| 0x0C0 | 16 | unknown | |
| 0x0D0 | 9 | UHF DCS deviation | no |
| 0x0D9 | 2 | unknown | |
| 0x0DB | 5 | VHF DCS deviation | no |
| 0x0E0 | 5 | VHF squelch 9 open | no |
| 0x0E5 | 5 | VHF squelch 9 close | no |
| 0x0EA | 5 | VHF squelch 1 open | no |
| 0x0EF | 5 | VHF squelch 1 close | no |
| 0x0F4 | 12 | unknown | |
| 0x100 | 40 | VHF calibration frequencies: 10 x 4 bytes BCD (5 RX/TX pairs) | no |
| 0x128 | 8 | unknown | |
| 0x130 | 9 | UHF DMR I gain | yes, HR_C6000 4:0x45 in DMR |
| 0x139 | 5 | VHF DMR I gain | yes |
| 0x13E | 2 | unknown | |
| 0x140 | 9 | UHF DMR Q gain | yes, HR_C6000 4:0x46 in DMR |
| 0x149 | 5 | VHF DMR Q gain | yes |
| 0x14E | 2 | unknown | |
| 0x150 | 32 | unknown | |
| 0x170 | 9 | UHF FM I gain | yes, HR_C6000 4:0x45 in FM TX |
| 0x179 | 5 | VHF FM I gain | yes |
| 0x17E | 2 | unknown | |
| 0x180 | 9 | UHF FM Q gain | yes, HR_C6000 4:0x46 in FM TX |
| 0x189 | 5 | VHF FM Q gain | yes |
| 0x18E | 2 | unknown | |
| 0x190 | 9 | UHF mid power | yes |
| 0x199 | 5 | VHF mid power | yes |
| 0x19E | 2 | unknown | |
| 0x1A0 | 9 | UHF very low power (derived, 12.1) | yes |
| 0x1A9 | 5 | VHF very low power (derived) | yes |
| 0x1AE | 2 | unknown | |
| 0x1B0 | 72 | UHF calibration frequencies: 18 x 4 bytes BCD (9 RX/TX pairs); the first 8 bytes double as the working-copy marker | no |
| 0x1F8 | 8 | unknown | |

BCD frequency format: 4 bytes little-endian BCD, 10 Hz units (00 25 00 40 = 400.02500 MHz).

### 12.3 Interpolation against frequency

Frequencies in 10 Hz units; above 300 MHz use the UHF row.

| Row kind | Band | First point | Step | Points |
|---|---|---|---|---|
| power | UHF | 400 MHz | 10 MHz | 9 |
| power | VHF | 135 MHz | 10 MHz | 5 |
| I/Q gains, RX tuning | UHF | 400 MHz | 10 MHz | 9 |
| I/Q gains, RX tuning | VHF | 136 MHz | 9.5 MHz | 5 |

index = (f - first) / step, clamped to the row; linear interpolation between point[index] and point[index+1]; above the
last point, extrapolate with the slope of the last segment. Power and RX-tuning points are scaled x16 (8-bit to 12-bit
DAC), gains stay 8-bit (clamp 0..255). Power uses the TX frequency; gains use the TX frequency too.

### 12.4 Reference oscillator offset

offset = (VHF or UHF tune byte) - 128, signed, for the band in use (TX band while transmitting, RX band otherwise).
Written to HR_C6000 4:0x47 = offset, 4:0x48 = 0x03 if offset < 0 else 0x00, 4:0x04 = offset. In FM TX, 4:0x48 = 0x00
and 4:0x04 = offset.

The HR_C6000 manual (4.7.2, two-point modulation): MOD1 drives the reference oscillator's control pin and MOD2 the VCO.
4:0x04 is the MOD1 offset in transmit, about +-422 mV in 3.3 mV steps. 4:0x47 and 4:0x48 bits 1:0 are a 10-bit level
MOD1 holds in receive, 0-3.3 V (the DAC kept on: 4:0x25 bit 5 = 1, bit 3 = 0). The radio's values are the offset
sign-extended to 10 bits, so receive and transmit sit at the same level.

The user's reference trim ([radio-features.md](radio-features.md) 6.1) adds to this offset at every tune:
`uvf_ref_trimmed` turns ppb into steps at 160 ppb a step and clamps to -128..127. The scale comes from the direct
keying measurement in section 6 (41 and 43 steps put the carrier 149 Hz apart at 461.1375 MHz, about 74 Hz a step).
A step is 0.16 ppm, so the 0.1 ppm trim steps round to it. With a blank band (12.1) the offset is 0 and the trim still
applies.

Checked on the radio (2026-10-30): DMR at 438.5 MHz, lowest power, read by an SDR's AFC once settled. +-5 ppm is 31
steps (4.96 ppm, 2175 Hz); the carrier moved +2155 and -2120 Hz from trim 0, after taking out the 86 Hz drift between
the trim-0 readings before and after, as linear in time. That is within 3% of the scale, and the sign is right.

### 12.5 FM deviation (fixed, not from the table)

HR_C6000 FM TX deviation coefficients used instead of the table rows:

| Band / bandwidth | 4:0x35 voice | 4:0xA0 CTCSS | 4:0xA0 DCS |
|---|---|---|---|
| UHF 25 kHz | 0x24 | 0x09 | 0x02 |
| UHF 12.5 kHz | 0x12 | 0x06 | 0x01 |
| VHF 25 kHz | 0x73 (0x68 noted as the start of distortion) | 0x18 | 0x06 |
| VHF 12.5 kHz | 0x37 | 0x10 | 0x04 |


---

## 13. SPI flash

### 13.1 Device and bus

| Item | Value |
|---|---|
| Part | Winbond W25Q128 class, 16 MB; JEDEC 0x9F returns manufacturer 0xEF (Winbond; others may appear) and device 0x4018 |
| Bus | SPI1 master, mode 0 (CPOL 0, CPHA first edge), MSB first, 8 bit, APB2/16 = 4.5 MHz; CS = PD7 (GPIO) |
| Commands used | 0x03 read, 0x02 page program (256 B), 0x20 4 KB sector erase, 0x06 write enable, 0x05/0x35/0x15 status 1/2/3, 0x9F JEDEC ID, 0x48 read security register (3 address bytes + 1 dummy) |
| Busy wait | poll status 1 bit 0 with 1 ms sleeps |
| Init check | treat "device ID != 0x4018" as a flash failure (safe mode) |

### 13.2 Map

The flash keeps the GD-77 codeplug layout so that CPS tools interoperate: the first 64 KB emulate the GD-77's 64 KB
EEPROM one-to-one, and the GD-77's 1 MB "flash" image starts at 0x020000 (offset +128 KB). Details of the codeplug
records are in `open-firmware-cps.md`.

| Address (SPI flash) | GD-77-relative address | Size | Content |
|---|---|---|---|
| 0x000000 | EEPROM 0x0000 | 64 KB | emulated EEPROM: general settings at 0x00E0 (callsign 0x00E0, DMR ID 0x00E8), DTMF contacts 0x2F88, signaling/DTMF 0x1400, APRS configs 0x1588, first 128 channels (header 0x3780, data 0x3790), last used channel per zone 0x6000, radio settings block at 0x604B, boot screen/password at 0x7518-0x755F, quick keys 0x7524, VFO A/B at 0x7590, zones 0x8000+ |
| 0x010000 | (local) | 512 B | calibration working copy (12.1). CPS reads/writes it through a 0x10000 window |
| 0x011000 | (ours) | 32 KB | RadioFoundry settings ring (`settings-store.md` 2); no open-firmware tool uses it, the CPS shim refuses writes |
| 0x019000 | (ours) | 8 KB | RadioFoundry sector journal (`settings-store.md` 4) |
| 0x020000 | flash 0x00000 | 4 KB | custom data: boot image and boot melody, themes (one 4 KB sector; erased when the user clears custom data) |
| 0x050000 | flash 0x30000 | 256 KB | DMR ID database part 1 (header "Id" + type byte + record length + entry count; see the CPS spec) |
| 0x09B1B0 | flash 0x7B1B0 | | channels 129-1024 (bank headers 16 B before each bank, data from 0x9B1C0) |
| 0x0A7620 | flash 0x87620 | | contacts |
| 0x0AD620 | flash 0x8D620 | | RX group list lengths (76 lists), lists from 0x0AD6A0 |
| 0x0AF400 | flash 0x8F400 | | voice prompts (header magic 'VP' = 0x5056); with the long ID-database format the ID database may overwrite this area |
| 0x0D8000 | flash 0xB8000 | to 0x120000 | DMR ID database part 2 |
| 0x100000 | flash 0xE0000 | | old voice prompt location (read for compatibility) |
| 0x120000 - 0xDFFFFF | | about 12.9 MB | unused by the codeplug layout; GPS logs use the ID database areas when enabled |
| 0xE00000 - 0xEFFFFF | (ours) | 1 MB | reserved for the native codeplug (`codeplug-native.md` 4); the CPS shim refuses writes from here up |
| 0xF00000 - 0xF3FFFF | (ours) | 256 KB | staging area for a codeplug written over TFTP (`tftp.md` 4): the commit record, then the staged image |
| 0xF40000 - 0xFFFFFF | | 768 KB | unused |
| security register 1-2 | | 512 B | factory calibration (read only) |
| security register 3 | | 256 B | factory flags: 0x1C (0xF4 = MD-UV390 Plus 10 W), 0x1D (LCD type) |


---

## 14. Other peripherals

| Item | Details |
|---|---|
| GPS (where fitted) | USART1 RX on PA10, 9600 8N1, DMA2 Stream2 circular, half/full callbacks; power on PA9 (high = on) |
| RTC | LSE 32.768 kHz, 24 h format; keeps time while the MCU has power (no separate backup battery confirmed, unverified) |
| LEDs | PE0 green, PE1 red, active high |
| Backup SRAM | unused |
| Watchdog | RadioFoundry starts the IWDG and feeds it from the idle task (`CONFIG_WATCHDOG`, [watchdog.md](watchdog.md)) |

### 14.1 Geehy APM32F407 in place of the STM32F405

Some DM-1701s are reported to carry a Geehy APM32F407VGT6, a pin-compatible STM32F4 clone, in place of the
STM32F405VGT6 (a user's chip list, unverified on a radio we have). It reports the same device ID (0x413), so firmware
can't tell the two apart that way. The differences that matter here:

| Difference | APM32F407 | Effect on RadioFoundry |
|---|---|---|
| NVIC priority bits | 3 (8 levels; Geehy datasheet V1.1, 4.3.1) | Each priority keeps its top 3 bits: 4 and 5, 6 and 7 share a level. The RTOS syscall level must be even, or the kernel's start-up check fails every boot ([gotchas.md](gotchas.md) 13.11) |
| ADC and temperature calibration in system memory | missing (user reports) | none: nothing reads them |
| Internal flash programming | differs (user reports) | the application never writes internal flash; the vendor bootloader programs it |
| ST-LINK | doesn't recognize the part (user reports) | debug probes only |

Up to the syscall level fix, a radio with this part boots to a black screen: the backlight is still off from
`board_early_init` when the scheduler starts, so no beep, no USB and no picture. The update mode, the vendor
bootloader's, still works.

### 14.2 GPS module

No module is fitted at the factory, but the GPS pins are there: USART1 RX on PA10 and the power switch on PA9.
Owners fit a small 3.3 V NMEA module (an ATGM336H or a GP-02, reported by users) and wire it like this:

| Module pin | Board | STM32 pin (LQFP100) |
|---|---|---|
| TX (NMEA out) | PA10, USART1 RX | 69 |
| power enable (where the module has one) | PA9 | 68 |
| 3.3 V, GND | a 3.3 V point on the board, such as the SPI flash's supply | |

PA9 and PA10 sit next to each other on the side of the package that holds pins 51-75. A module without an enable pin
runs from 3.3 V all the time and leaves PA9 unconnected. Nothing goes to the module's RX: the firmware only listens.

RadioFoundry builds GPS by default (`CONFIG_GPS`). At start-up it drives PA9 high and listens on PA10 in 5 s windows,
9600 first. One checksum-valid NMEA sentence marks the module present. Characters without a valid sentence move on to
the next rate (14400). A window of silence means no module: USART1 goes off, PA9 goes low, and the listening task ends
and frees its stack. The task runs at low priority in the background, so start-up doesn't wait for it. The code is
`boards/uv380_family/gps_uvf.c`, shared with the MD-UV380 G models ([hardware-mduv380.md](hardware-mduv380.md) 9).
Untested with a module on a DM-1701.

---

## 15. Bootloader and firmware images

### 15.1 Entering the bootloader

Hold **PTT + side key 1 (the key just above PTT)** while turning the power switch on. The radio enumerates as a USB DFU
device, VID 0x0483 PID 0xDF11 (ST's DFU IDs, but the TYT/AnyRoad bootloader, not ST's ROM loader). The device string at
index 1 reads "AnyRoad Technology". The screen stays dark. The interface string describes the memory layout in the
usual DfuSe "@name/0xADDR/NN*SSKg,..." form.

### 15.2 Flashing protocol (DfuSe-style, as the loader drives it)

| Step | Request (bmRequestType 0x21 unless noted) |
|---|---|
| 1 | wait for idle: GETSTATUS (0xA1, request 3, length 6); while state != dfuIDLE (2) send CLRSTATUS (4) and repeat |
| 2 | vendor command DNLOAD (request 1, wValue 0, 2 data bytes) 0x91 0x01; GETSTATUS, 100 ms, GETSTATUS, wait for idle |
| 3 | vendor command 0x91 0x31, same handling (meaning of both commands unknown; required) |
| 4 | erase: for each address 0x0800C000, 0x08010000, 0x08020000, 0x08040000, 0x08060000, 0x08080000, 0x080A0000, 0x080C0000, 0x080E0000 (the loader also lists 0x08100000), DNLOAD wValue 0 with 5 bytes 0x41 + address (little endian); GETSTATUS must report dnBUSY (4) then dnIDLE (5) |
| 5 | per region (same list; sizes 0x4000, 0x10000, then 0x20000 each): set address = DNLOAD 0x21 + address; then DNLOAD data blocks of 1024 bytes with wValue = block number starting at **2** and incrementing (region 0x0800C000 uses blocks 2..0x11, 0x08010000 blocks 2..0x41, the 128 KB regions 2..0x81); after each block GETSTATUS must be dnBUSY then dnIDLE. The last block is padded with 0xFF to 1024 bytes |
| 6 | exit: set address 0x08000000, then DNLOAD with zero length; the device resets (it may not answer the final GETSTATUS) |

The image is written as one contiguous stream starting at 0x0800C000: byte n of the file goes to 0x0800C000 + n.

Every listed region has the same rule: blocks 2..N, the key applied from the image start, which is block aligned at
every sector start (for example 0x080A0000 - 0x0800C000 = 0x94000, a multiple of 1024). rfoundry erases and writes a
region only when it has data, so an update leaves sectors 10 and 11 (the codec) out. The loader accepts a sector
written on its own, with sectors left out on both sides: a RadioFoundry image that wrote only sectors 3-6 and 9 flashed
through rfoundry, booted and passed the radio-only HIL group (2026-10-14).

### 15.3 Image encryption

The bootloader decrypts what it receives. The host XORs every byte of the plain application image with a 1024-byte
per-model key: encrypted[n] = plain[n] XOR key[n mod 1024], n counted from the start of the image (address 0x0800C000).
No other transformation, header or checksum. Keys (1024 bytes each, values not reproduced here; identify a key by its
SHA-256):

| Model | Key SHA-256 | First 4 bytes |
|---|---|---|
| DM-1701 (also used for the RT84, unverified) | d983ce95fbc19fc78b913d9afc0381e3ba4ffedb480798865292d05a2fb3deff | 3A 04 74 AD |
| MD-UV380 / MD-UV390 / MD-2017 | 739ce35cc8db6e72f95598effac1a01ae71dbe2a5792c1a69acaee5dbf634991 | 00 AA 89 89 |
| MD-380 | 5254139c6b596d9413d106e6b7c0685ec245d69555cb28df98f061d6b879c4cd | 2E DF 40 B5 |
| MD-9600 | c3d665b297651b52310ec098e87440b147136241359c1154d474e517b048c9ae | A2 FA BB 4B |

The key tables are identified by hash only. A key is derived on the user's machine from the manufacturer's firmware file, or read from the radio's bootloader (below).

**Derived from a firmware file, DM-1701.** The key falls out of any DM-1701 firmware file from the manufacturer. The payload is plain XOR key[i mod 1024], and in a firmware image 0x00 is the most common plain byte in every
key column, so the most common cipher byte of column j is key[j] for all but a few columns; those are settled by
trying each close runner-up byte until the SHA-256 of the result matches the table above. On every firmware file in
`codec-dm1701.md` ("Firmware files": V02.01, V02.02 and V02.03 CSV, V02.03 noCSV, V02.06 CSV and noCSV) this gives the key
with hash `d983ce95...` in under 0.1 s, needing at most a few hundred hash checks. `tools/rfoundry/rfoundry/stockfw.py`
(`derive_key`) does this on the user's machine; only the hash is in the repository. The same method should work for the
other models' files (unverified: no MD-UV380, MD-380 or MD-9600 file was tried).

**In the bootloader, DM-1701.** The bootloader (sectors 0-2) holds the key as one plain 1024-byte
block: in the bootloader exactly one 1024-byte window has the hash above. RadioFoundry can read those sectors (read
protection level 1 doesn't stop code running from user flash, `debuglink.md` 3.10), so the web flasher
(`bootkey.js`) and rfoundry (`bootkey.py`) read them over READ_MEM and take the block whose SHA-256 matches. Updating a
radio that runs RadioFoundry therefore needs no firmware file. The search tries every offset (4-byte aligned first) and assumes
no layout, so a bootloader version that keeps the key elsewhere works too. Unverified for the other models'
bootloaders; when no block matches, both clients fall back to the firmware file.

Other loader facts:

- A manufacturer's firmware file is recognized by its first 16 bytes "OutSecurityBin" followed by two zero bytes; it must
  never be flashed as an application image.


### 15.4 Software entry to a bootloader

Goal: re-flash over USB without someone holding PTT + side key 1 at power-on (15.1). There are two possible routes,
both reached from the running firmware through the debug link (`docs/spec/debuglink.md` 3.9-3.11).

**Read protection.** `rfoundry_debug.py chip` on a DM-1701 (2026-10-10) reads idcode
`0x0009A413` (STM32F405/407), 1024 KB flash, `FLASH_OPTCR` `0x0FFF55ED`: **RDP level 1** (byte `0x55`), no sector
write protection, BOR off, user bits all 1. Assume every DM-1701 is at RDP level 1 until one reads otherwise.
RDP level 1 does not stop the application from reading flash (only a debugger, RAM boot and system-memory boot are
locked out), so the bootloader (sectors 0-2) can be read with `rfoundry_debug.py dump`. It is proprietary and is never committed.

**Route 1: ST's ROM bootloader (USB DfuSe in system memory). Refused on this radio.** The firmware has it:
`REBOOT` mode 1 stores the flag `0xB007DF11` in `RTC_BKP19R` and resets. A backup register is used rather than RAM
because the bootloader runs in RAM before the application. First thing in `main()`,
`stm32_sysboot_check()` (`platform/stm32f4/sysboot.c`) finds the flag and then:

1. clears the flag and checks that it is clear, so a failed entry boots normally next time (and boots normally
   anyway if the flag won't clear);
2. sets the power-up pin levels (`board_sysboot_prepare`: power latch high, transmitter off, HR_C6000 powered down);
3. turns interrupts off, stops SysTick and clears every NVIC enable and pending bit;
4. returns RCC to HSI with HSE and the PLLs off at their reset values, sets `FLASH_ACR` to 0 and resets every
   peripheral except the GPIO ports;
5. checks that `0x1FFF0000` holds a plausible vector table, maps system memory at 0 (`SYSCFG_MEMRMP`), sets
   `VTOR` to 0, loads MSP from `0x1FFF0000` and jumps to the vector at `0x1FFF0004`.

The firmware and `rfoundry_debug.py` both refuse this mode unless RDP is level 0. At level 1 the ROM bootloader refuses
every flash access, and its only way out, the read-unprotect command, sets RDP to 0 by **mass-erasing all of flash,
the bootloader included**. After that the radio could be reloaded only over SWD. At level 2 the ROM
bootloader is disabled. **On the bench unit (RDP 1) this route is closed.** The code stays in the firmware because it
is gated and does nothing unless the flag is set. If a unit ever reads RDP 0, the procedure would be: `rfoundry_debug.py
chip` -> `rfoundry_debug.py reboot --rom-dfu` -> a DfuSe host that erases and writes only sectors 3-11 (`0x0800C000` and
up). No such host tool exists in `tools/`: `stm32_dfu.py` has no ROM mode.

Untested risks of route 1, found while writing it (they matter only on an RDP 0 unit):

- **Power latch.** The volume knob only breaks the battery-sense line (10.4); PA7 holds the supply. While the ROM
  loader runs, PA7 stays high, so the knob cannot turn the radio off. Getting out of a stuck ROM loader takes a
  DfuSe leave command or removing the battery. Because the flag is already cleared, the next start is normal.
- **HSE clock.** The board feeds an external clock in bypass mode (1.2), not a crystal. ST's ROM needs HSE for USB
  DFU on the F405 and may set it up in crystal mode. If that doesn't start, USB DFU never enumerates.
- **Interface probing.** The ROM also listens on USART1 (PA9 / PA10, wired to GPS power and GPS RX on GPS-fitted
  siblings) and on USART3 / CAN2. Activity on those pins can capture the loader before USB. The DM-1701 has no GPS
  fitted from the factory, so PA10 is idle on it. An owner-fitted module (14.2) talks on PA10 from power-up.
- **Leaving.** Where the ROM's DfuSe leave command jumps, and with which memory mapping, is unverified. A battery
  pull always returns to the normal start (bootloader, then the application).

**Route 2: a software entry into the bootloader's own DFU mode.** This route would leave RDP alone and keep
the XOR-keyed DfuSe path that `tools/stm32_dfu.py` already speaks (15.2, 15.3). It works only if the bootloader
checks something besides the PTT + side key 1 combination that firmware can set before a reset: a RAM word it reads
before clearing RAM, an RTC backup register, or a backup SRAM location.

**There is no such flag.** The bootloader decides
once, after its own startup has cleared and initialized main SRAM, and there is no timeout. It stays in DFU when either
of these holds; otherwise it loads MSP from 0x0800C000 and jumps to the word at 0x0800C004:

- GPIOE IDR bits 8 and 11 (PE8, keypad column 5 / side key 1; PE11, PTT) both read low, the power-on key combination.
- The application's initial stack pointer at 0x0800C000 fails `(SP & 0x2FFE0000) == 0x20000000`, so an erased
  sector 3 (0xFFFFFFFF) keeps it in DFU. The reset vector is not range checked.

It reads no RAM word, no RTC backup register or backup SRAM, and no reset-cause flag (`RCC_CSR`), and it doesn't set
`VTOR` (the application sets its own). So nothing firmware can set before a reset makes it stay in DFU, except making
the application look invalid. The least damaging way is a single flash word program that clears bit 29 of the word at
0x0800C000 (0x2001FFFC to 0x0001FFFC; programming can only clear bits, so no erase is needed). After that the radio
boots into DFU every time until a full image is written again, which `tools/stm32_dfu.py` does. Sectors 0-2 are never
touched, so the bootloader always stays reachable. This is not implemented; it is a candidate `REBOOT` mode 2 with the
same gate plus an explicit confirmation in the tool.

**Route 3: no bootloader at all, for updates.** A build with `CONFIG_STUB_ENTRY=y` loads a RAM flash stub over the
debug link and the stub writes the application sectors itself ([stub-stm32f4.md](stub-stm32f4.md)). It uses the DFU
check above as its safety net: it erases sector 3 first and programs the word at 0x0800C000 last, so an interrupted
update leaves the radio in DFU at the next power-on with no keys held.


---

## 16. USB

| Item | Value |
|---|---|
| Core | OTG_FS, device only, embedded PHY on PA11/PA12, VBUS sensing off, SOF off, no dedicated EP1 interrupt |
| FIFO RAM | 1.25 KB (0x140 words): RX 0x80 words, TX0 0x30, TX1 0x80, TX2 0x10 |
| Endpoints | the F405 OTG_FS has EP0 plus 3 IN and 3 OUT endpoints |
| Serial mode | CDC-ACM: interrupt IN 0x82 (8 bytes), bulk IN 0x81 / OUT 0x01 (64 bytes) |
| Network mode | CDC-NCM 1.0, NTB16 only: notification IN 0x82 (16 bytes, 16 ms interval), bulk IN 0x81 / OUT 0x01 (64 bytes); interfaces 0 (communication) and 1 (data, alternate setting 1 active), grouped by an IAD |
| Why a switch | ACM + NCM together need 4 IN endpoints (2 + 2) and there are only 3, so the radio is either a serial port or a network adapter; the choice is a stored setting applied at USB start (switching re-enumerates) |
| IDs used | VID 0x1FC9; PID 0x0094 serial, 0x0096 network (a separate PID so Windows binds a different driver) |
| Windows 10 NCM binding | Microsoft OS 1.0 descriptors: string index 0xEE, vendor code 0x47, extended compat ID "WINNCM"; Windows 11, Linux and macOS bind NCM by class |
| NTB sizes | IN NTBs small (one datagram each); accept OUT NTBs up to 2048 bytes (Linux needs at least one full Ethernet frame plus headers); NTB parameters: NdpIn/Out divisor 4, alignment 4, no datagram limit |
| Reported speed | 12 Mbit/s |

USB gotchas found on the DM-1701:

- macOS aggregates back-to-back datagrams into one NTB; the NTB parser must walk every datagram pointer (not stop after a
  fixed count) and also bound the number of NDPs to avoid loops.
- macOS only brings the link up after a NetworkConnection notification is sent once the device is configured. On a cold
  boot in network mode macOS sometimes never activates the link; re-enumerating (soft disconnect/connect) about 5 s
  after boot when the link is still down fixes it.
- Reassemble OUT NTBs from 64-byte packets using the NTB block length rather than relying on a zero-length packet, and
  re-arm the OUT endpoint only after the NTB has been consumed (natural flow control).
- The reduced-power clock must not be entered while USB is configured (1.3).


---

## 17. Variants

Build variants: DM1701, RT84, MDUV380, MDUV380_10W_PLUS. The MD-2017 is not built: its pins are not defined.

| Aspect | DM-1701 | RT84 | MD-UV380 / UV390 5 W | MD-UV390 Plus 10 W | MD-2017 (partial code only) |
|---|---|---|---|---|---|
| Visible LCD rows | 120 (8-row offset) | 128 | 128 | 128 | 128 |
| LCD type source | sec. reg 3 byte 0x1D, DM-1701 table (9.2) | as DM-1701 (unverified) | sec. reg 3 byte 0x1D, MD-UV380 table, always RGB | as MD-UV380 | fixed: type 3, RGB |
| Keypad matrix | 10.1 DM-1701 table | same as DM-1701 | 10.1 MD-UV380 table | as MD-UV380 | different (P1/P2 on row 1, up/down on row 2) |
| Side keys on row 2 | D5 top orange, D6 side 2, D7 side 1 | same | D7 side 1, D6 side 2, no orange | same | D5 trackball press, D7 side 2, extra orange button on its own GPIO (active low) |
| Navigation | rotary encoder | rotary encoder | rotary encoder | rotary encoder | trackball (4 direction pulse inputs) |
| PA band select / backlight | PC6 / PD8 | PC6 / PD8 | PC6 / PD8 | **PD8 / PC6 (swapped)** | as MD-UV380 (unverified) |
| Battery ADC divisor | 37.5 | 37.5 | 40.3 | 40.3 | 37.5 (unverified) |
| Volume pot range | 29..2075, break 318 | as DM-1701 | 22..2070 / 309 (RT3S 23..2020 / 317) | 19..2054 / 308 | 32..2048 / 321 |
| Max power | 5 W | 5 W | 5 W (a 10 W radio can be forced by setting) | 10 W | |
| Power fractions | 7, 12.1 DM-1701 rows | same | 5 W rows | 10 W rows | |
| AT1846S count | 1 | 1 | 1 | 1 | 2 (two independent receivers) |
| DFU key | DM-1701 key | DM-1701 key (unverified) | MD-UV380 key | MD-UV380 key | MD-UV380 key |
| 10 W identification | n/a | n/a | sec. reg 3 byte 0x1C = 0xF4 means Plus 10 W | | |
| Soft volume (volume shown on screen) | yes | yes | yes | yes | no |

Everything else (MCU pins for RF, HR_C6000, AT1846S, SPI flash, USB, clocks, flash layout, codec address) is the same
across the family.


---

## 18. Gotchas summary

1. The bootloader rejects an initial stack pointer equal to the top of RAM; use 0x2001FFFC.
2. Set VTOR to 0x0800C000; the vector table is not at 0x08000000.
3. The bootloader leaves HSE/PLL running: switch to HSI before reprogramming HSE/PLL.
4. CCM RAM is not zeroed or initialized by default startup code and is invisible to DMA. RadioFoundry puts the RTOS heap (every
   task stack), the slot engine, the radio state machine and the big CPU-only buffers there; `.ccmbss`
   (`RFOUNDRY_CPU_RAM`) is zeroed before `main` by a `.preinit_array` entry in `board.c`, `.ccmram` (`RFOUNDRY_CPU_RAM_NOINIT`)
   is not. DMA buffers stay static in main SRAM, and each stream's set-up checks that (`DMA_RAM_OK`).
5. The codec must be at exactly 0x0807537C; the code budget is the space below it. Keep large constant tables above the
   codec end if the code grows.
6. PA13 is SWDIO; driving it as mic power breaks SWD (use connect-under-reset).
7. LCD data lines double as keypad columns: discharge them before reading keys, and leave them low after LCD writes.
8. HR_C6000 config SPI and AT1846S I2C are touched from ISRs and tasks: make transactions atomic, and never block on I2C
   in an ISR.
9. The PA is switched per burst in DMR; "transmitting" must come from the modem's slot state, not from the PA pins.
10. The HR_C6000 RF_RX_EN / RF_TX_EN pins are not wired to the MCU; RF switching is done by the MCU (I2C + GPIO), about
    0.5 ms per switch.
11. The AT1846S TX frequency offset depends on the write sequence (about -3 kHz with the normal sequence, none with a
    direct switch); measure, do not copy.
12. DMR RX needs AT1846S AF gain at maximum (0x44 = 0x07FF) and reg 0x40 bit 0 set; FM needs bit 0 clear.
13. A volume-knob power-off looks like the battery dropping below 5.5 V; the firmware must then release the PA7 latch.

---

## 19. Datasheets

Public documents this page relies on: the AT1846S datasheet 0.3 and programming guide 1.4, the HR_C6000 datasheets, the
W25Q80BV datasheet, ETSI TS 102 361-1 v2.2.1.

Not available for this radio: the STM32F405 datasheet and RM0090 reference manual; the LCD controller datasheets
(ST7735S / HX8353-E, and the controller of panel types 2/3); the W25Q128 datasheet (the W25Q80BV covers the command
set); schematics or a service manual; audio power amplifier, RF PA and TCXO part data; GPS module data.

## 20. Bench findings (RadioFoundry on a DM-1701, 2026-10-09)

Verified on one radio with the new firmware; they override the unverified notes above where they differ.

- **Keypad**: the 10.1 matrix is right. The two "function" keys are labeled P1 (row 0, D6) and P2 (row 0, D5). There is
  no orange key: the three black side keys are, top to bottom, row 2 D5 (the "top key" position), row 2 D7, row 2 D6;
  PTT is separate, between the first and the second. The firmware names them SK1, SK2 and SK3 in that order. SK1 (D5),
  just above PTT, is the key held with PTT to enter the bootloader. A row released while still high stays charged and makes every key also read as the key below it:
  drive the row low before releasing it and keep idle rows pulled down.
- **Encoder**: clockwise counts up with the 10.3 rule, one count per detent.
- **Panel**: this radio reports type 2 (security register 3, byte 0x1D). Driving it with the type 2 MADCTL (0xE0) shows the
  picture mirrored top to bottom; treating it as type 3 with RGB order (as 9.2 says) is correct. The 8 hidden rows are
  panel rows 0-7 either way.
- **Volume pot**: raw 26 at the bottom, 2063 at the top.
- **Battery**: raw / 37.5 gives a plausible 7.1 V (not yet checked against a meter).
- **Calibration**: the working copy at 0x010000 carries the marker. The FM I gain rows (0x170 UHF, 0x179 VHF) are all
  0x00 on both bands while the other gain rows hold values: an all-zero gain row is valid data.
- **Soft volume**: HR_C6000 4:0x37 (RX gain) scales FM feed-through audio as well as DMR audio; -31 dB is still
  audible, so the bottom of the knob must also mute the speaker path. It mutes the amp only: the feed-through (0x36)
  and the gain (unity at the bottom) stay as for any FM receive, because the I2S copy of the FM audio that APRS receive
  reads is gated and scaled by both. Before this, a knob at zero turned the feed-through off and APRS heard nothing.
- **Volume override**: because the MCU reads the knob and sets this gain itself, the debug link can set the level
  instead (RADIO op 30, `radio-features.md` 4.2): a step 0..16 maps to the same 0..100 % scale as the knob, in force
  until it is cleared (RAM only). The knob has no analog path around the gain, so the override fully takes its place (HW,
  not yet run on a radio).
- **HR_C6000**: 4:0x10 reads back 0x80 after FM RX set-up (the config port works with the 4.2 timing).
- **FM TX** (VFO 461.8625 MHz, 12.5 kHz, 1 W, measured with an RTL-SDR, 2026-10-09): with the TX sequence of
  `radio_dm1701.c` (frequency written with the receiver off, straight to TX) the carrier is within about 50 Hz of the
  channel, so no TX offset is added. Unmodulated carrier residual FM 41 Hz RMS, no hum or tone. Voice at normal level:
  2.5 kHz peak deviation (99.5th percentile), 636 Hz RMS, about 7 kHz occupied at -26 dB. The FM I calibration row of 0
  (Mod2 amplitude) does not stop voice modulation.
- **DMR TX** (Ch 1 467.375 MHz, cc 2, 1 W, SDR bench + an MMDVM_HS receiver, 2026-10-10): switching per burst from
  RX on straight to TX on the already-tuned frequency put the carrier 3.07 kHz low (an ADF7021-based receiver
  decoded nothing; a wide-AFC SDR decoder still did). Retuning with the receiver off before every burst (as FM TX
  does) brings it to about +65 Hz, and the MMDVM_HS receiver decodes headers, all superframes with the embedded LC
  and the terminator. Burst spacing 60.000 ms (sd 0.002 ms), 31 ms PA per burst.

### 20.1 SFR user (SDR bench, 2026-10-19)

A bench call (TS1 sync) from the SDR bench through a standalone repeater was played whole, 24 of 24 voice bursts, no
frame lost. The radio never reported the repeated copy during the call: its HR_C6000 decodes the voice of the slot it
locked onto (gotchas 14.12). With `sfr_rx` 1 or 2 it falls over to the other copy only between calls or after a loss.

## 21. Audio paths for USB audio (bench study)

Which audio paths reach the MCU, for the USB audio design ([usb-audio.md](usb-audio.md)). Measured with probe firmware:
`CONFIG_AUDIO_PROBE=y` adds debug link command 0x10 ([debuglink.md](debuglink.md) 3.14), which reads and writes
HR_C6000 registers, runs the PCM stream with a test tone and reports both I2S channels' levels.
`tests/hil/test_audio_probe.py` runs the experiments.

| Question | Answer |
|---|---|
| DMR voice as PCM, both ways | **Yes, today.** The I2S link to the HR_C6000 already carries 8 kHz 16-bit PCM both ways, and the MCU runs the AMBE codec. Decoded calls can go to the host and host audio can be encoded and sent, at no new CPU cost (21.2) |
| FM receive audio to the MCU | **Yes, two ways** (bench, 2026-10-12, 21.3.1). The I2S stream carries it with the default settings, squelch gated. The HR_C6000's RX RAM carries it ungated, 8 kHz big endian 16-bit, one 256-sample half per 32 ms, with de-emphasis switchable (0x34 bit 5): the voice and flat modes of [usb-audio.md](usb-audio.md) 5.5, flat from about 300 Hz up |
| MCU audio into the FM modulator | **Yes: PWM on PC8 into the mic input (candidate E, to be built), or the modulator's offset register** (bench, 2026-10-12, 21.3.1): the firmware writes 0x04 (two-point MOD1 offset) about 37,000 times a second, about 57-75 Hz deviation a step, so about 6 bits of resolution at normal deviation, and none of the chip's FM processing (pre-emphasis, limiter, CTCSS: the MCU does them). No path makes the chip's own FM chain take MCU samples: the TX RAM is a tap the codec writes, external-codec mode reads pins 16-20, which don't reach the MCU. The chip's selcall tone generator takes phase steps from the MCU: clean AFSK |
| An MCU ADC on the RX audio line, or a free DAC on the mic line | **No.** ADC1 sees only the volume pot, the battery and a mic level tap; DAC1 drives the PA power, the DAC2 pin is a GPIO (21.1) |


### 21.1 What the board has

From sections 2, 4.4, 8 and 11 and the HR_C6000 user manual (the vendor
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
(`platform/stm32f4/hrc/i2s_pcm.c`, `hal/hal_audio.h`). This is the only audio data path between the MCU and the radio.

No other path exists on this board: ADC1's four inputs are the volume pot (PA0), battery (PA1), mic level (PA3) and
the temperature sensor; DAC channel 1 (PA4) sets the PA power (APC); the DAC channel 2 pin (PA5) is the VHF receiver
5 V switch. The RX audio mux (PD9) chooses between the HR_C6000 line out and the AT1846S audio output toward the
speaker amp only. A hardware modification (a wire from the AT1846S audio output to a free ADC pin) would work for
receive but is not something users should need, and there is no equivalent for transmit.

### 21.2 DMR

The DMR voice path already moves PCM between the MCU and the codec in both directions (`core/dmr/ambe/ambeVoice.c`):
decoded frames are played as 20 ms blocks, and microphone blocks are encoded while transmitting. For a USB voice
endpoint the audio router ([usb-audio.md](usb-audio.md) 5.1) takes the decoded block for the host as well as (or instead of) the speaker, and
feeds host PCM to the encoder instead of the microphone. CPU, codec stack and RAM are what a normal DMR call uses
(the AMBE codec runs for one direction at a time; a DMR radio is half duplex). DMR on this board was verified on air
in both directions (section 20).

### 21.3 FM

What the HR_C6000 manual says:

- The built-in codec's input mux takes MIC_P / MIC_N (differential) or LINEIN1 / LINEIN2 (single ended), then a gain
  stage, the ADC and a digital filter; its output path is a gain stage (page 4 0x37), the DAC and LINEOUT1 / LINEOUT2
  (manual 4.5.1, figure 4.12).
- Register 0xE0 (manual register table, Chinese section): bit 7 MCU controls the codec, **bit 6 LINEIN1 enable, bit 5
  LINEIN2 enable**, bit 4 LINEOUT1, bit 3 LINEOUT2, bit 2 MIC_N, bit 1 MIC_P, bit 0 I2S slave. Our 0xC9 (DMR, FM TX) is
  MCU control + LINEIN1 + LINEOUT2 + I2S; 0x89 (FM RX) the same without LINEIN1. So the microphone is wired to
  LINEIN1, and [modem-hrc6000.md](modem-hrc6000.md) 2 calls bit 6 "mic input enable", which is right in effect.
  On a DM-1701 board photo, pin 8 (LINEIN2) has no trace and pins 6 (MIC_N) and 7 (LINEIN1) share a via; the bench
  agrees (21.3.1, LINEIN2 loop). FM receive audio plays with 0xE0 = 0x89, both
  line inputs off, and the HR_C6000 detects the sub-audio tones, so the receive audio reaches its FM chain through
  the baseband ADC (the path DMR uses), not the codec inputs: section 8's "line in"
  for FM RX is probably that. Pin 8 is the free analog input for a mod (candidate D).
- 0xE3 bit 0: "codec internal DAC and ADC pass-through enable, normally 0". The direction is not stated.
- The FM transmit chain takes the codec ADC's samples; the FM receive chain ends at the codec DAC (manual 6, figure
  6.1). The I2S "vocoder interface" carries the codec ADC's samples to an external vocoder and the vocoder's PCM to
  the DAC (manual pin table, C_SDI / C_SDO; 0x22 bits 2-1 choose I2S and its master mode). Nothing says the FM chains
  can read from or write to the I2S interface.
- The chip has "FM data" RAM: two 512-byte buffers in the TX RAM ("can be written externally for FM send; voice data
  can also come internally from the codec") and two in the RX RAM (manual 5.3 tables), and FM-mode interrupts in 0x82:
  bit 7 "FM: sending data request", bit 1 "FM: receiving data". Register 0xD7 bit 1 (written 0xDB here before) selects "send FM voice" over MSK
  signaling. This looks like the MCU can exchange FM audio samples with the chip through its RAM pages, but the
  format, rate and control are not documented. Our FM RX set-up writes 0x81 = 0x19, which masks both FM data flags.
- The FM chain can use an external codec instead of the built-in one (0x06 bit 2, appendix A2), over a second I2S
  port on pins 17-20 with the chip as master. The MCU sits on the other one, the vocoder port (pins 34-37), so this
  only helps if pins 17-20 reach the MCU, which nothing we have shows. The block diagram's "I2S" next to the FM
  transmit chain is this port.
- A "prompt tone" path (0x06 bit 1, appendix A1) lets the MCU write DAC samples over U_SPI without I2S: speaker only,
  but with 0xE3 bit 0 it is another way into the ADC and so the FM transmit chain if the I2S clock stops in FM.
  [modem-hrc6000.md](modem-hrc6000.md) 8.1 has the details of all of these.

### 21.3.1 Bench results (2026-10-12)

Probe firmware, `tests/hil/test_audio_probe.py`, NOAA WX3 (162.475 MHz) for receive, 467.375 MHz into the SDR bench
for transmit.

| Question | Result |
|---|---|
| I2S clock in FM | runs: 50 blocks a second |
| FM RX on I2S | **yes, with the default settings** (0xE0 = 0x89): RMS about 2000 on NOAA, both channels the same, exactly 0 with the squelch closed (the feed-through, 0x36, gates the digital stream too). LINEIN1, LINEIN2 and 0xE3 bit 0 only lower it |
| FM RX in the RX RAM | **yes, ungated.** 0x82 bit 1 comes 31.0 times a second once 0x81 unmasks it (0x19 masks it, apart from about 1 a second): one 512-byte half per 32 ms, so 256 samples at 8 kHz. Samples are 16-bit **big endian**. The chip fills a half progressively: read both halves twice and take the one that didn't change (98-100 % of the audio captured at about 47 reads a second over the debug link). NOAA speech: 50 ms envelope CV 0.82; a quiet channel: steady noise, CV 0.06, louder than the signal (noise quieting) |
| What the RX RAM tap sits after | 0x34's processing: with de-emphasis (bit 5) the noise falls 12 dB from 500 Hz to 2.5 kHz, without it it is flat (-1 dB). Bit 7 adds the 300-3400 Hz band pass, bit 2 the wider RX filter. Below 300 Hz stays about 20 dB down in every setting: a fixed sub-audio high pass |
| FM TX from I2S (A, B) | **no.** 0xE3 bit 0 changes nothing. LINEIN2 on (0xE0 bit 5) puts a steady 2.49 kHz tone on air at about 1.7 kHz RMS deviation whether the probe sends nothing, 1 kHz or 600 Hz: something on that input, not MCU audio. Don't enable LINEIN2 |
| FM TX from the TX RAM (C) | **no: the TX RAM is a tap.** Keyed, 0x82 bit 7 ("sending data request") comes 31.0 times a second once unmasked. Read back through page 6 (U_SPI OPMode 110, manual 5.2), the FM halves hold the codec's samples, big endian 16-bit, one half per 32 ms, rewritten whether or not the microphone is on: only 0x60 bit 7 (the analog TX chain off) stops the writes (304 bit flips tried). The static-RAM tests (the settings below, the sweeps) were moot for that reason; the firmware's RAM feed (probe op 11) rewrites each half within 4 ms of the codec moving on, and with it a 1 kHz tone never reaches the air: the default settings, every FM value of 0x10, each bit of 0x01, 0x06, 0x0D, 0x0E, 0x11, 0x21, 0x22, 0x25, 0x26, 0x30-0x37, 0x40, 0x41, 0x50-0x52, 0x60, 0xA1, 0xD0-0xE5 and page 1 0x50 / 0x51 (525 settings), 0xD7 bit 1 (the manual's "send FM voice", alone and with MSK mode), and each FM data request answered with 0x41, 0x22, 0x60 or 0x50 writes. 0x01 bit 4, 0x25 bits 0 and 1 drop the carrier. Earlier: 0xDB bit 1 (0xDB isn't in the manual; the bit is 0xD7 bit 1), 0x60 = 0x40 or 0xC0, 0x06 bits 0, 2, 3, 4, 0xE0 bit 7 off, 0xE2 bit 4 |
| FM TX through the MOD1 offset (0x04) | **yes.** Stepped while keyed, 0x04 moves the carrier linearly, about 75 Hz a step as a signed byte (0xCF as set; +8 steps +595 Hz, +32 +2395 Hz, -32 -2395 Hz, bit 7 flipped +9.5 kHz). Manual 4.7.2: 0x04 is the MOD1 (reference crystal) offset, 0x02 the MOD2 (VCO) one; 0x02, 0x03, 0x05, 0x45-0x48 moved nothing. Probe op 10 writes 0x04 = as set + a sine back to back (37,500 writes a second): 300, 1000, 2000 and 3000 Hz tones on air, about 57 Hz deviation a step at 1 kHz (16 steps: 650 Hz rms), flat 1-2 kHz. 0x46 (MOD1 amplitude) doesn't scale it. The first SINAD figures (about 4 dB) came from phase jitter in the probe's millisecond-clock timing, not the path; the microsecond-clock build is not measured yet |
| FM TX from the chip's tone generator | **yes, AFSK-class.** 2-tone mode (0xA1 = 0x10, 0xA0 = 0x39), "long A", retriggered by 0x60 0x00 -> 0x80 while keyed: tone A comes from its 16-bit phase step at aux 0x11A (f / 32000 x 2^16, written through page 1 with an extended address). Rewritten every 100 ms from the host it switches cleanly between 1200 and 2200 Hz at about 2.2 kHz rms deviation: a phase-continuous AFSK generator inside the FM chain |
| FM TX source switch | 0x06 bit 2 (external codec) moves the FM chain's input off the built-in codec: LINEIN2's tone stops the moment it is set. The external port (pins 16-20) isn't wired to the MCU, and the MCU's I2S tone doesn't reach the air in that mode (with 0x36 bit 7 and 0x22 bits 1-2 too). A wire from the MCU's PCM output (PC11) to ADCDAT (pin 19) would make it a full-quality path (candidate D) |
| PC8 into MIC_P / MIC_N | **yes, no mod.** A 1 kHz square wave on PC8 (the beep PWM, TIM8 CH3, probe op 12) goes on air as a 1 kHz tone at 3.0 kHz rms deviation (SINAD 8.1 dB: a square wave's harmonics) once 0xE0 bits 1-2 (MIC_P, MIC_N) are on; nothing with only LINEIN1 or LINEIN2 on. The beep net reaches the differential mic input, so PWM at a high carrier with the duty set by each sample feeds the chip's own FM chain (pre-emphasis, limiter, CTCSS) through the codec ADC |
| PWM audio on PC8 | **the best path so far.** A sine by PWM (probe op 14: TIM8 CH3, the duty rewritten each carrier period by DMA) into MIC_P / MIC_N: 1 kHz at 1 kHz rms deviation (4 % of the PWM range) 14.1 dB SINAD; at 2.6 kHz rms (10 %, mic first stage -12 dB) 15.6 dB; more drives the limiter. The carrier (17.6-140 kHz) makes no difference: the board and the codec filter it. The floor with MIC_P / MIC_N on and no audio is 115 Hz rms in the voice band (50 Hz with them off). Flat (0x34 bits 7-5 off) the response is 300 Hz 0 dB, 1 kHz 0 dB, 2 kHz -3.5 dB, 2.5 kHz -5.3 dB, 3 kHz -7 dB, 3.5 kHz -14 dB, nothing at 3.8 kHz: the codec's 8 kHz sampling. Fine for voice and 1200 / 4800 baud (equalized); not 9600 |
| 0x04 response | **flat to 6 kHz**: 16 steps give 689, 661, 650, 633 and 599 Hz rms deviation at 1, 3, 3.8, 4.8 and 6 kHz. It skips the codec, so it is the only no-mod transmit path wide enough for 9600 baud FSK (flat to 4.8 kHz; a 25 kHz channel, 0x34 bit 4). Its resolution (about 6 bits) and the CPU it takes (32,700 bus writes a second) remain; receiving 9600 needs the AT1846S audio output on an MCU ADC (a mod), the RX RAM tap being 8 kHz |
| SINAD reference | not settled: the tone generator's "long A" plays in bursts of about 0.5 s, so its 10.6 dB SINAD (2.2 kHz rms) mixes tone and gaps. The bench's own ceiling still needs a steady clean source (a signal generator, or the bench's own modem board in FM) |
| Pins 16-20 wiring | **not wired to the MCU.** External-codec mode (0x06 bit 2) stops the I2S lines the MCU sees (PA15 WS, PC10 CK, PC12 SD) and starts none; pins 16-20 as the chip's timed GPIO outputs (0xC7-0xD0), keyed, move no MCU pin (probe op 13 scans) |
| 0x04 quality (microsecond timing) | 1 kHz, 16 steps: SINAD 12.7 dB at 720 Hz rms deviation; 32 steps 10.9 dB; noise shaping makes it 1.5 dB worse. About 32,500 writes a second |
| LINEIN2 loop | **no loop: an open input.** LINEIN2's tone stays with LINEOUT2 off (and with LINEOUT1 on instead) and stops when the input gain drops (0xE4 = 0x20); the 1 kHz I2S tone never reaches the air. A floating high-gain input oscillates; nothing on the board drives pin 8 |
| MSK mode | 0xA1 bit 6 with 0xD5 = 0x48 keys the chip's MSK modem: a steady 1800 Hz at 2.7 kHz rms deviation, not the RAM |

| FM TX from the prompt tone FIFO | **no.** The FIFO (page 3 address 0, 32 samples when 0x88 bit 0 reads 0) drains at exactly 8000 samples a second whether 0x06 bit 1 is set or not, and a 1400 Hz tone through it never reaches the air: path open, with 0xE3 bit 0, either byte order |
| Bit sweep while keyed | **no.** Each bit of 0x25, 0x26, 0xE1, 0xE2, 0xE3, 0xE5, 0x0D, 0x0E, 0x0F and 0x38 flipped for 0.5 s (80 flips, 10 key-ups) with a 600 Hz tone on I2S and a 1 kHz tone in the TX RAM: neither tone on air during any flip. LINEIN2's tone, used as the time marker, came at 2.59-2.61 kHz this time (2.49 kHz before): an oscillation on that input |

So the chip's FM chain takes audio only from its codec ADC (microphone, LINEIN1, LINEIN2) or the external codec
port; nothing the MCU writes over U_SPI or I2S reaches it. MCU audio goes on air through the modulator's MOD1 offset
(0x04) instead, with the MCU doing pre-emphasis, limiting and CTCSS, at about 6 bits; AFSK can use the chip's tone
generator. Full quality needs candidate D (the PC11 to pin 19 wire). The register reference with the manual's
details is [hrc6000-registers.md](hrc6000-registers.md).

Candidates, in order of preference:

| | Receive (FM audio to the MCU) | Transmit (MCU audio to the modulator) | Cost if it works |
|---|---|---|---|
| A | 0xE0 bit 5: LINEIN2 into the codec ADC, if the AT1846S audio (or the speaker line) is wired to it; I2S then carries it as it carries the microphone in DMR | (none) | nothing: same I2S stream |
| B | 0xE3 bit 0 with FM feed-through on: the DAC's signal (FM receive audio) looped into the ADC and out on I2S | 0xE3 bit 0 with feed-through off: I2S PCM to the DAC, looped into the ADC, which feeds the FM transmit chain | nothing: same I2S stream |
| C | FM data RAM (page 2, 0x030-0x42F) read on the "receiving data" flag | FM data RAM written on the "sending data request" flag | 16 KB/s each way over the bit-banged config port (about 1-2 Mbit/s, so roughly 8-16 % of the bus per direction, in 64-byte transactions); FM only, when the modem ISRs don't run. Format, rate and the TX source bit to be found on the bench |
| E | PWM on PC8 (TIM8 CH3) with 0xE0 bits 1-2 on: the beep net reaches MIC_P / MIC_N | (none) | TIM8 and one DMA stream; no bus traffic |
| D | a hardware modification | a wire from PC11 (I2S3ext_SD) to ADCDAT (pin 19) with 0x06 bit 2; or MCU PWM through an RC filter and a 10 µF capacitor into LINEIN2 (pin 8) with 0xE0 bit 5 (PC8, the old beep PWM, is a candidate) | not for users |

A, B and C don't work (21.3.1). Receive uses the I2S stream or the RX RAM tap. Transmit uses E, or the MOD1 offset
for content above the codec's band; AFSK can also use the chip's tone generator.

The probe experiments, in the order they were run:

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
   source settings (FM flags unmasked, 0xDB bit 1 (meant: 0xD7 bit 1), 0x60 = 0x40 or 0xC0, 0x06 bit 0, big endian samples): the FM flag
   rate and the voice band deviation against a silent-RAM baseline.

### 21.4 Running the probe

With the AMBE codec merged as usual:

```
make BOARD=dm1701 CONFIG_AUDIO_PROBE=y BUILD=build/dm1701-probe   # flash build/dm1701-probe/rfoundry_dm1701.bin
python3 -m pytest tests/hil/test_audio_probe.py -v -s -rs          # stream in FM, the receive variants (NOAA), RX RAM
python3 -m pytest tests/hil/test_audio_probe.py -v -s -rs --allow-rf --allow-service-stop -k tx   # I2S and RAM TX variants
```

By hand (an FM channel or the VFO):

```
tools/rfoundry_debug.py aprobe start            # stream with silence out; then:
tools/rfoundry_debug.py aprobe status           # blocks counting up = the I2S clock runs in FM
tools/rfoundry_debug.py aprobe reg 4 0xE0       # 0x89 expected in FM receive
tools/rfoundry_debug.py aprobe wreg 4 0xE0 0xA9 # LINEIN2 on (candidate A); status / samples with and without a signal
tools/rfoundry_debug.py aprobe wreg 4 0xE0 0x89
tools/rfoundry_debug.py aprobe wreg 4 0xE3 0x53 # candidate B (0xE3 is 0x52 at init: bit 0 set); watch status
tools/rfoundry_debug.py aprobe wreg 4 0xE3 0x52
tools/rfoundry_debug.py aprobe samples -o /tmp/blk.raw   # one block of both channels, tone estimate printed
tools/rfoundry_debug.py aprobe stop
tools/rfoundry_debug.py aprobe wreg 4 0x81 0x9B # unmask the FM data flags (0x19 | 0x82)
tools/rfoundry_debug.py aprobe watch 1000 0x82 0x030   # FM flag rates, RX RAM word changes
tools/rfoundry_debug.py aprobe ram 2 0x030 64   # the start of the RX FM buffer
```

Expected readings: in FM receive with the defaults, both channels near silence (the microphone is off). A candidate
works for receive if, with NOAA audible and the squelch open, one channel's RMS rises well above (4x or more) its value
on a quiet frequency. For transmit, the SDR bench's voice band RMS deviation with the tone and the candidate setting
is well above the "mic muted, no tone" row.


## 22. Facts moved from the shared specs

- Settings and storage: the core's map for the 16 MB SPI flash is settings 0x011000 (32 KB, 8 sectors), journal
  0x019000, open-firmware EEPROM 0x000000, open-firmware flash 0x020000 up to 0xE00000, native area 0xE00000 (1 MB),
  TFTP staging 0xF00000 (256 KB), calibration window 0x010000 (512 B). The range 0x011000-0x01AFFF has never been
  checked on a used radio, for the foreign-region guard of [settings-store.md](settings-store.md) 5.
- The board's `hal_radio` has `tx_tone`, so the MMDVM task keys the 1750 Hz burst ([mmdvm-protocol.md](mmdvm-protocol.md)).
- USB: the STM32F405 OTG_FS has three IN endpoints besides EP0, so serial and network are alternatives, not a composite
  ([usb-network.md](usb-network.md)).
- `board_rf_safe` pins: PA_EN1 (TX supply) and PA_EN2 (APC loop) low, PA_BAND low, APC DAC (DAC1 channel 1) 0.
- GPS: USART1 RX on PA10, with PA9 as the GPS power switch.
- Crash log ([crash-log.md](crash-log.md)): the backup SRAM kept the record through the bootloader. A trap test gave
  HardFault with CFSR UNDEFINSTR in the task host; a hang test ("CRASH TEST") was recorded at 3750 ms. The snapshots
  named radio_task, dmr_task, input_task, the UI task in input_next / ui_task and the USB task in tud_task_ext
  (2026-10-17/23, `test_crash.py`). The restart cause after a watchdog reset is `watchdog`.
- Roaming ([dmr-roaming.md](dmr-roaming.md) 3): a survey visit relies on the AT1846S RSSI plus HR_C6000 sync
  acquisition needing well under 100 ms on a continuous outbound (a burst every 30 ms), then four 25 ms readings.
- On air since 2026-10-10: receiver sleep (`test_battery_save*`: cycles when idle, monitor and a call wake it), the
  1750 Hz burst, the roger beep and APRS AFSK (HIL). VOX was checked in silence only (`test_vox_quiet`).
- AFSK TX retunes the AT1846S tone generator per symbol. The AFSK RX audio comes from the HR_C6000 FM audio on I2S
  (squelch gated, 8 kHz, de-emphasized). DMR LRRP and UDT passed on air at 467.375 MHz, CC 2, 50 mW. An AFSK tone level
  of 0x1C gives about 3.1 kHz peak deviation (0x40 gave 7.2 kHz).
- `REBOOT` mode 1 enters the ROM DFU with a flag in an RTC backup register (15.4). The spare beep PWM pin is PC8
  (TIM8 CH3, 72 MHz carrier, DMA2 stream 2), used by AUDIO_PROBE PIN_TONE / PIN_PWM at about 37,000 MOD writes a second.
- Release and flashing: application sectors 3-9 (720 KB), codec sectors 10 and 11 (1.4), keep supported, profile
  `dm1701-v0206-csv-s11`. Releases before the move linked read-only data into sector 9 as a second range, profile
  `dm1701-v0206-csv`. An interrupted update writes sector 3 (the vector table) last, so a cut before it leaves the
  bootloader in DFU; a later half application may crash but never bricks. Enter the bootloader by hand (PTT and side
  key 1 at power-on) and flash again.
