<!-- SPDX-License-Identifier: MIT -->
# Radioddity GD-77 hardware

The GD-77 (also sold as the TYT MD-760) pairs an NXP Kinetis K22F microcontroller with the HR_C6000 DMR chip and the
AT1846S transceiver, the same two radio chips as the DM-1701 ([hardware-dm1701.md](hardware-dm1701.md)). This page
records the hardware facts the port uses. ODR has not run on a GD-77 yet, so every fact here is unverified until a
radio is on the bench. The family and its rebadges are in [radios.md](radios.md#radioddity-gd-77-family).

| Tag | Meaning |
|---|---|
| **U** | Unverified: the port relies on it, nobody has checked it on a radio running ODR |
| **I** | Inferred from other facts on this page or from a datasheet; the reasoning is stated |
| **R** | Checked on a radio running ODR (none yet) |
| **?** | Open: two readings conflict, or the role isn't known (section 11) |

## 1. Block diagram

```
                       12.288 MHz reference (I)
                        |                 |
                     EXTAL0            XTALI
 +----------------------+---+       +-----+------------+  I2S0 (codec audio)   speaker amp (PTB0 ?)
 | MK22FN512 (K22F)         |-SPI0--| HR_C6000         |---------------------- mic
 | Cortex-M4F               |-SPI1--|  configuration,  |
 | 512 KB flash, 128 KB RAM |-I2S0--|  vocoder port    |-- FM / DMR baseband --+
 |                          |       +------------------+                       |
 |                          |-I2C bus A (soft)--+-- AT1846S (0x71) ------------+-- RF switches, PA (?)
 |                          |                   +-- EEPROM (0x50, 64 KB)
 |                          |-I2C bus B (soft)----- device at 0x3D (no radio function)
 |                          |-SPI (soft)----------- SPI NOR data flash
 |                          |-serial (soft)-------- 128x64 LCD, backlight
 |                          |-GPIO----------------- 4x5 key matrix, 4 direct keys, LEDs, power hold, power switch
 |                          |-ADC0, DAC0----------- battery (?), TX drive (?)
 +--------------------------+
```

## 2. MCU and clocks

### 2.1 MCU

| Item | Value | Tag |
|---|---|---|
| Part | MK22FN512 (Cortex-M4F with FPU, 120 MHz max), 512 KB program flash, 128 KB SRAM | U |
| Package | 100-pin class; the exact suffix isn't confirmed | ? |
| SRAM | two 64 KB halves: SRAM_L at 0x1FFF0000, SRAM_U at 0x20000000; no object may cross 0x20000000 | U |
| Reset state | watchdog on, core on the FLL from the slow internal reference (20.97 MHz) | U |

### 2.2 Clock plan

One 12.288 MHz reference feeds the K22F's EXTAL0 and the HR_C6000's XTALI (I). The reasoning:

- The K22F's PLL needs a reference of 2 to 4 MHz after its divider. 12.288 MHz divided by 4 gives 3.072 MHz and by 6
  gives 2.048 MHz, both in range. A 24.576 MHz source would need a divider the target clock doesn't allow.
- The HR_C6000 manual's clock table lists a 12.288 MHz XTALI row (PLL words 0x0B = 0x40, 0x0C = 0x32, dividers
  0xB9 = 5, 0xBA = 4, 0xBB = 2). ODR uses that row (`hrc6000_clock_12m288`); the DM-1701 keeps its 29.4912 MHz row.
- PTA19 is the data flash's chip select (section 3), and PTA19 is also XTAL0. A crystal would need that pin, so the
  reference is a clock on EXTAL0 alone, with the oscillator off (`crystal = false` in the plan).

| Plan | Mode | PLL | Core | Bus, FlexBus | Flash | Status |
|---|---|---|---|---|---|---|
| Bring-up | RUN | 12.288 / 6 = 2.048 MHz, x 24 | 49.152 MHz | 49.152 MHz | 24.576 MHz | in `board.c` |
| Target | HSRUN | 12.288 / 4 = 3.072 MHz, x 31 | 95.232 MHz | 47.616 MHz | 23.808 MHz | defined, not used yet |

The limits the plans are checked against (`platform/k22f/clock_math.h`, host test `k22fPlatformTest`): PLL output 48
to 120 MHz; RUN core at most 80 MHz and bus at most 50 MHz; HSRUN core at most 120 MHz and bus at most 60 MHz; flash
at most 25 MHz; FlexBus at most 50 MHz. The switch goes FBI, then FBE, PBE and PEE, with every wait bounded. When the
reference never comes, the radio stays on the internal FLL and the diagnostics page shows `fll`.

## 3. Pin map

### 3.1 Pins

"Level" is the start-up level ODR sets. Every row is **U**; rows with a **?** have an open role (section 11).

| Port, pin | Dir | Level | Function | Tag |
|---|---|---|---|---|
| PTE26 | out | high | power hold: high keeps the radio on | U |
| PTA13 | in | | power knob switch, high = on | U, ? |
| PTC8 | out | high | LCD chip select, low = selected | U |
| PTC9 | out | low | LCD reset, low = in reset | U |
| PTC10 | out | high | LCD A0: low = command, high = data | U |
| PTC11 | out | high | LCD clock, data taken on the rising edge | U |
| PTC12 | out | high | LCD data, MSB first | U |
| PTC4 | out | low | LCD backlight, high = on | U |
| PTC0..PTC3 | in, pull-up | | key matrix columns, driven low one at a time while scanned | U |
| PTB19..PTB23 | in, pull-up | | key matrix rows, low = key closed | U |
| PTB9, PTB1 | in, pull-up | | side keys, low = pressed (the pair held at power-on enters update mode) | U, ? |
| PTA2 | in, pull-up | | top key candidate | ? |
| PTA1 | in | | PTT candidate, no internal pull | ? |
| PTA19 | in | | data flash chip select: driven low to select, released to deselect | U |
| PTE4, PTE5, PTE6 | out, out, in | high, low | data flash MOSI, SCK, MISO | U |
| PTE24, PTE25 | out, open drain | high, released | I2C bus A clock and data | U |
| PTB2, PTB3 | out, open drain | high, released | I2C bus B clock and data | U |
| PTD0 | out | high | HR_C6000 configuration port chip select | U |
| PTD1, PTD2, PTD3 | ALT2 | | SPI0 SCK, SOUT, SIN to the HR_C6000 | U |
| PTE0 | out | high | HR_C6000 control, reset candidate (held high) | ? |
| PTE1 | out | high | HR_C6000 power-down candidate: high 10 ms, then low to run | ? |
| PTC7, PTC16, PTC17 | in | | HR_C6000 interrupts: RF receive, RF transmit, system (falling edge) | U |
| PTC18 | in | | HR_C6000 time slot interrupt (either edge) | U |
| PTB10, PTB11, PTB16, PTB17 | ALT2 | | SPI1 PCS0, SCK, SOUT, SIN: the HR_C6000 vocoder port (set up with DMR) | U |
| PTA12, PTA14, PTA15, PTA16 | ALT6 | | I2S0 TXD0, RX_BCLK, RXD0, RX_FS to the HR_C6000 codec | U, ? |
| PTB0 | out | low | speaker amplifier candidate | ? |
| PTA17 | out | low | TX / PA enable candidate | ? |
| PTE2, PTE3, PTC13, PTC15 | out | low | RF switch candidates (PTC13 and PTC15 switch as a pair) | ? |
| PTC14, PTB18 | out | low | LED candidates | ? |
| PTC5, PTC6 | out | high, low | indicator pair candidates (PTC5 active low) | ? |
| ADC0_DP1 | analog | | battery candidate | ? |
| ADC0_DP2, ADC0_DP3 | analog | | roles open (DP3 is a level compared with thresholds) | ? |
| DAC0_OUT | analog | 0 | TX power drive candidate | ? |

### 3.2 Start-up order

1. `board_early_init`: PTE26 high first, so the radio stays on once the knob is released.
2. Every other pin from `boards/gd77/pin_table.h`. Nothing that could key the transmitter or switch the RF path is
   driven active. The HR_C6000 stays powered down and the LCD stays in reset. Host test `gd77BoardTest` checks these
   levels.
3. The software buses idle (section 4), then the clock switch (section 2.2).

## 4. Buses

| Bus | Pins | Devices | Mode | Implementation now |
|---|---|---|---|---|
| I2C bus A | PTE24 SCL, PTE25 SDA | AT1846S at 7-bit 0x71 (8-bit register, 16-bit value high byte first); EEPROM at 0x50 (16-bit address, 64-byte page, 5 ms page write) | 100 kHz | software |
| I2C bus B | PTB2 SCL, PTB3 SDA | one device at 0x3D, authentication, no radio function | 100 kHz | software, idle |
| Data flash | PTE4 MOSI, PTE5 SCK, PTE6 MISO, PTA19 CS | SPI NOR | mode 0 | software |
| LCD | PTC8..PTC12 | UC1701 / ST7567 class controller | write only, mode 3 | software |
| HR_C6000 configuration | PTD1..PTD3, CS PTD0 | HR_C6000 U_SPI | mode 1, about 770 kHz | SPI0, chip select by GPIO |
| HR_C6000 vocoder | PTB10, PTB11, PTB16, PTB17 | HR_C6000 V_SPI | mode 1 | not set up (DMR) |
| I2S0 | PTA12, PTA14..PTA16 | HR_C6000 codec | slave (section 8) | driver written, not started |

### 4.1 Software or hardware per bus

Each bus reaches the chip code through a generic interface: `drivers/bus/i2c_bus.h` for the two I2C buses and
`drivers/bus/spi_bus.h` for the data flash, the LCD and the HR_C6000 port. The software masters are
`drivers/bus/i2c_gpio` and `drivers/bus/spi_gpio`. Which one a bus uses is an entry in `boards/gd77/bus_cfg.h`, and a
build can change it (`MCU_DEFS += -DGD77_I2C_A_HW=1`). `gd77_bus_cfg_check` refuses a choice the pins can't carry or a
peripheral chosen twice. A hardware choice whose driver isn't written yet reports `K22F_ERR_NOT_IMPLEMENTED` on the
diagnostics page (`bus` line) and refuses every transfer.

Bring-up drives each bus the way the wiring was designed to be driven, since those timings are the known ones.

### 4.2 Improvement candidates

Only pins that carry a peripheral's function on the K22F's pin mux qualify. All of this is unverified until a radio is
on the bench.

| Bus | Hardware option | Constraint | Tag |
|---|---|---|---|
| I2C bus A (PTE24, PTE25) | I2C0 on ALT5 | I2C0 is also the only controller on PTB2 / PTB3 (ALT2), so only one of the two buses can have it | U |
| I2C bus B (PTB2, PTB3) | I2C0 on ALT2 | as above; bus B has no radio function, so bus A is the better use of I2C0 | U |
| HR_C6000 configuration (PTD0..PTD3) | SPI0 PCS0 on PTD0 (ALT2), the chip select in hardware | needs a PCS mode in `platform/k22f/dspi.c` | U |
| Data flash (PTE4..PTE6, PTA19) | none | PTE4..PTE6 carry SPI1 chip selects, not its clock and data; PTA19 is XTAL0 | U |
| LCD (PTC8..PTC12) | none | no SPI function on these pins | U |

## 5. Radio chain

### 5.1 AT1846S

On I2C bus A at 7-bit 0x71. Register 0x04 = 0x0FD0, the 26 MHz reference setting, which is what the shared driver
(`drivers/rf/at1846s`) writes. Bands 136 to 174 MHz and 400 to 470 MHz (U). The RSSI scale is the DM-1701's (U).

### 5.2 HR_C6000 control

PTE0 and PTE1 both start high. PTE1 goes low after 10 ms and the chip runs; PTE0 stays high (U). Which of the two is
reset and which is power-down isn't settled (?). Configuration access waits 10 ms after PTE1 goes low, then runs the
power-on sequence of [modem-hrc6000.md](modem-hrc6000.md) with the 12.288 MHz clock row (section 2.2).

### 5.3 Interrupts

PTC7 (RF receive), PTC16 (RF transmit), PTC17 (system) on the falling edge, PTC18 (time slot) on either edge (U). No
handler is attached yet: they come with DMR.

### 5.4 Transmit path

Not enabled. The candidates are PTA17 (TX / PA enable), PTE2, PTE3, PTC13, PTC15 (RF switches) and DAC0 (power
drive), all **?**. The factory calibration hasn't been located either. `tx()` refuses and both bands are marked
receive only until both are settled on a radio.

## 6. Memory map

### 6.1 Program flash and SRAM

| Range | Use | Tag |
|---|---|---|
| 0x00000000..0x00003FFF | the radio's bootloader; never written | U |
| 0x00004000..0x0007F7FF | the ODR application, vector table first (`boards/gd77/gd77.ld`) | U |
| 0x0007F800..0x0007FFFF | the last 2 KB sector: radio data that must survive; never linked into or written | U |
| 0x1FFF0000..0x1FFFFFFF | SRAM_L: .data, .bss, newlib's small heap | U |
| 0x20000000..0x2000FEEF | SRAM_U: `ODR_CPU_RAM` state, the RTOS heap with every task stack, the main stack | U |
| 0x2000FEF0..0x2000FFFF | left unused: margin for anything the bootloader keeps there | U |

The initial stack pointer is 0x2000FEF0.

### 6.2 Data flash and EEPROM

The SPI NOR chip and the 64 KB EEPROM on I2C bus A hold the radio's codeplug and settings between them (U). This
build reads the flash's JEDEC ID and nothing else: the board gives the core no flash map, so nothing is stored and
settings don't survive a power cycle. Mapping ODR's storage onto the two chips is an open task (section 12).

## 7. Display

128x64 monochrome, UC1701 / ST7567 class controller on the shared driver (`drivers/display/st7567`). Init: 0x2F
(regulator, follower and booster on), 0x81 0x11 (electronic volume), 0xE3, 0xA2 (bias 1/9), 0xA1 (segment
reverse), 0xC0 (common normal), 0xA4, 0xA7, 0xAF; column offset 4 (U). Whether the panel needs 0xA7 (inverse) is open
(?); the driver's default (normal) is used. The backlight on PTC4 is on or off; the pin has no PWM function in use.

## 8. Audio path

The HR_C6000's codec handles the speaker and the microphone, as on the DM-1701. For FM receive the chip passes audio
through (`hrc6000_fm_audio`) and PTB0 switches the speaker amplifier (?).

Digital audio runs over I2S0 with the HR_C6000 as master (U): a 512 kHz bit clock, an 8 kHz frame sync one bit early,
two 32-bit words a frame, MSB first, the 16-bit sample in the low half of each word. The K22F's receiver takes the
clocks and the transmitter runs synchronous to it. DMA channel 0 (DMAMUX source 12) drains the receiver and channel 1
(source 13) fills the transmitter (`platform/k22f/i2s.c`). An alternative I2S pin set is on PTC1, PTC5..PTC7 (?).

## 9. User interface hardware

### 9.1 Keys

The matrix reads as 4 driven lines (PTC0..PTC3) by 5 read lines (PTB19..PTB23).

| Driven | PTB19 | PTB20 | PTB21 | PTB22 | PTB23 |
|---|---|---|---|---|---|
| PTC0 | 1 | 2 | 3 | nav A | nav E |
| PTC1 | 4 | 5 | 6 | nav B | nav F |
| PTC2 | 7 | 8 | 9 | nav C | |
| PTC3 | * | 0 | # | nav D | |

The digit positions follow the keypad layout (U). The six navigation keys (green, red, up, down and the two
left / right keys) haven't been matched to their positions (?), so the board's key map is provisional: A select, B
back, C up, D down, E left, F right. The scan drives one line low at a time, waits 5 µs and reads the rows; a change
counts after two equal scans 10 ms apart. The diagnostics page shows the raw bits (`keys`): bit = column x 5 + row,
direct keys at bits 20 to 23.

| Direct key | Pin | Provisional role | Tag |
|---|---|---|---|
| Side key 1 | PTB1 | monitor | ? |
| Side key 2 | PTB9 | talk target | ? |
| Top key | PTA2 | emergency on long press | ? |
| PTT | PTA1 | PTT | ? |

### 9.2 Power, battery, LEDs

PTE26 holds power. The power knob switch on PTA13 reads high while on; three low polls in a row start the shutdown
(U). The battery reading takes ADC0_DP1 with a provisional 3:1 divider and a 3.3 V reference (?), so the board sets no
low-battery thresholds yet. The LEDs are the PTC14 / PTB18 pair (?), red on PTB18 in the code.

## 10. Update path and image packaging

Holding both side keys at power-on starts the radio's update mode (U). `make BOARD=gd77` produces the raw application
image (`odr_gd77.bin`, linked for flash 0x4000) with its `.elf` and `.map`. The update path needs a packaging step
that this repository doesn't have yet (section 12).

## 11. Open items

Each needs a radio on the bench.

| Item | Readings | How to settle it |
|---|---|---|
| PTA13 | power knob switch, or an HR_C6000 interrupt | read it on the diagnostics page while turning the knob |
| I2S0 pins | PTA12 / PTA14..PTA16, or PTC1 / PTC5..PTC7 | probe the bit clock |
| Reference clock | 12.288 MHz external clock on EXTAL0 (section 2.2) | measure EXTAL0; the `clk` diagnostics line shows `pll` once the PLL locks |
| LED pins | PTC14 / PTB18, or the PTC5 / PTC6 pair | drive each and look |
| PTT, side keys, top key | PTA1, PTB1, PTB9, PTA2 as in 9.1 | the `keys` diagnostics line |
| Navigation keys | positions nav A..F | the `keys` diagnostics line |
| PTE0 / PTE1 | which is reset, which is power-down | the HR_C6000 answering (`c6000 ok`) with each order |
| K22F package | 100-pin class, suffix unknown | read the part marking |
| Battery divider | channel and scale | measure the pack against the raw reading (`bat raw`) |
| Data flash chip select | PTA19 released to a pull-up | the `nor` diagnostics line shows a JEDEC ID |
| Transmit path | PTA17, RF switches, DAC0, calibration location | with the radio on a dummy load, after the receive path works |
| Bus pin mux (4.2) | the hardware options listed | check against the K22F data sheet's pin mux table, then try one |

## 12. Bring-up status

| Area | State |
|---|---|
| SoC: start-up, vector table, clocks, SysTick, GPIO / PORT, DSPI, PIT, ADC, DAC, watchdog off | written, builds |
| SoC: I2S0 with DMA | written, not started by the board |
| SoC: flash self-programming | stub, returns not implemented |
| SoC: I2C controller, USB device | not written |
| Display, keys, power hold, power switch, backlight, LEDs | written, unverified |
| FM receive (AT1846S, HR_C6000 FM path) | written, unverified |
| Transmit | refused |
| DMR (vocoder port, interrupts, codec) | not wired |
| Storage (EEPROM, data flash map) | read-only probe |
| USB (programming, debug link) | not wired |
| Update image packaging | open |
