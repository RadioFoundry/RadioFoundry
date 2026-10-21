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
| Target | HSRUN | 12.288 / 4 = 3.072 MHz, x 31 | 95.232 MHz | 47.616 MHz | 23.808 MHz | with `GD77_CLOCK_TARGET=1` |
| RUN step of the target | RUN | as the target | 47.616 MHz | 47.616 MHz | 23.808 MHz | around flash commands (15.1) |

The limits the plans are checked against (`platform/k22f/clock_math.h`, host test `k22fPlatformTest`): PLL output 48
to 120 MHz; RUN core at most 80 MHz and bus at most 50 MHz; HSRUN core at most 120 MHz and bus at most 60 MHz; flash
at most 25 MHz; FlexBus at most 50 MHz. The switch goes FBI, then FBE, PBE and PEE, with every wait bounded. When the
reference never comes, the radio stays on the internal FLL and the diagnostics page shows `fll`.

Start-up tries the plans in order (`k22f_clock_init_first`). The default build has the bring-up plan alone. A build
with `GD77_DEFS="-DGD77_CLOCK_TARGET=1"` tries the target first, then the bring-up plan, then stays on the FLL. HSRUN
is requested while the core runs from the 4 MHz internal reference, before the PLL is set up for 95.232 MHz; a fallback
returns to RUN after the clocks are back within the RUN limits. The fallback switches the clock source before the
dividers, so no clock passes above its limit. The millisecond clock (the core's cycle counter) carries on across every
clock change; every plan has a whole number of cycles per millisecond.

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
build can change it (`make BOARD=gd77 GD77_DEFS="-DGD77_I2C_A_HW=1"`). `gd77_bus_cfg_check` refuses a peripheral
chosen twice, and a pin that doesn't carry the function at the ALT number given: every hardware choice lists its pins,
and each must be in the K22F alternate table (`platform/k22f/k22f_mux.h`). A refused set leaves those buses refusing
every transfer, shown on the diagnostics page (`bus` line) and in the debug link's board view (`board.bus.*`, 14).

| Knob | Default | Effect |
|---|---|---|
| `GD77_I2C_A_HW` | 0 | I2C bus A on I2C0 (`platform/k22f/i2c.c`), 100 kHz, open drain with the pull-ups |
| `GD77_I2C_B_HW` | 0 | I2C bus B on I2C0 (refused together with bus A) |
| `GD77_C6K_HW_CS` | 0 | the HR_C6000 port's chip select as SPI0 PCS0 on PTD0, 1 µs select setup, hold and idle time |
| `GD77_BACKLIGHT_PWM` | 0 | the backlight dimmed by an FTM channel (section 7) |
| `GD77_CLOCK_TARGET` | 0 | the HSRUN target plan first (section 2.2) |

Bring-up drives each bus the way the wiring was designed to be driven, since those timings are the known ones.

### 4.2 Improvement candidates

Only pins that carry a peripheral's function on the K22F's pin mux qualify. All of this is unverified until a radio is
on the bench.

| Bus | Hardware option | Constraint | Tag |
|---|---|---|---|
| I2C bus A (PTE24, PTE25) | I2C0 on ALT5 (`GD77_I2C_A_HW`) | I2C0 is also the only controller on PTB2 / PTB3 (ALT2), so only one of the two buses can have it | U |
| I2C bus B (PTB2, PTB3) | I2C0 on ALT2 (`GD77_I2C_B_HW`) | as above; bus B has no radio function, so bus A is the better use of I2C0 | U |
| HR_C6000 configuration (PTD0..PTD3) | SPI0 PCS0 on PTD0 (ALT2), the chip select in hardware (`GD77_C6K_HW_CS`) | each byte but a transaction's last carries CONT, so the select stays low across header and data | U |
| Backlight (PTC4) | FTM0 channel 3 on ALT4 (`GD77_BACKLIGHT_PWM`) | the alternate number is unverified; check it against the data sheet before turning it on | U |
| Data flash (PTE4..PTE6, PTA19) | none | PTE4..PTE6 carry SPI1 chip selects, not its clock and data; PTA19 is XTAL0 | U |
| LCD (PTC8..PTC12) | none | no SPI function on these pins | U |

## 5. Radio chain

### 5.1 AT1846S

On I2C bus A at 7-bit 0x71. Register 0x04 = 0x0FD0, the 26 MHz reference setting. Bands 136 to 174 MHz and 400 to
470 MHz (U). The shared driver (`drivers/rf/at1846s`) runs with this radio's profile (`boards/gd77/rf_gd77.c`), all
(U):

| Item | Value |
|---|---|
| Init | soft reset, 0x30 = 0x0004, 0x04 = 0x0FD0, 27 register values, then the chip's calibration: 0x30 = 0x40A4 (50 ms), 0x40A6 (100 ms), 0x4006 (10 ms) |
| GPIO | 0x1F = 0x1000: GPIO6 is the squelch output, the others Hi-Z |
| Bandwidth | 9 page 0 and 11 page 1 registers per bandwidth (0x15, 0x32, 0x3A, 0x3F, 0x3C, 0x48, 0x62, 0x65, 0x66; page 1 0x06..0x0F, 0x12) |
| FM / DMR mode tables | none: both modes use the same AT1846S values |
| Reg 0x30, 12.5 kHz | idle 0x4006, receive 0x4026, transmit 0x40C6 (FM and DMR) |
| Reg 0x30, 25 kHz | idle 0x7006, receive 0x7026, transmit 0x70C6 |
| Reg 0x05 | 0x8763; 0x8543 at exactly 136.5, 147.875, 159.25, 409.5, 420.875, 432.25, 443.625, 466.375, 477.75, 489.125 and 500.5 MHz (multiples of 11.375 MHz); 0x8643 at 170.625, 504.63417 and 511.875 MHz |
| Frequency | 0x29 / 0x2A = the frequency in 62.5 Hz units (MHz x 16000), as on the DM-1701 |
| Tuning | 0x30 idle, 0x05, 0x29, 0x2A, then 0x30 for the new state |

CTCSS and DCS use the shared driver's tone registers. The RSSI scale (reg 0x1B high byte) is the DM-1701's: UHF
rssi - 151 dBm, VHF -164 + rssi x 32 / 27 dBm (U). Squelch is the core's, on RSSI and noise from reg 0x1B.

### 5.2 HR_C6000 control

PTE0 and PTE1 both start high. PTE1 goes low after 10 ms and the chip runs; PTE0 stays high (U). Which of the two is
reset and which is power-down isn't settled (?). Configuration access waits 10 ms after PTE1 goes low, then runs the
power-on sequence of [modem-hrc6000.md](modem-hrc6000.md) with the 12.288 MHz clock row (section 2.2).

After the shared power-on sequence the board sets (`boards/gd77/c6k_gd77.c`, all U):

| Register | Value | Meaning |
|---|---|---|
| 0x12 | 0x17 | RF TX interrupt advance (about 2.3 ms before a burst) |
| 0xC0 | 0x0A | RF RX interrupt advance |
| 0x0D..0x0F | 0x10, 0x8E, 0xB8 | voice superframe |
| 0x27..0x29 | 0x40, 0x7D, 0x40 | |
| 0x2A..0x2F | 0x0B, 0x0B, 0x17, 0x05, 0x04, 0x0B | 0x2B Sql_l, 0x2C Sql_h, 0x2E tx_pre_on, 0x2F I2S bit clock 512 kHz |
| 0x32, 0x33 | 0x02, 0xFF | I2S frame sync 8 kHz |
| 0x4B | 0x1B | |
| 0xE0 | 0x8B with the microphone on | codec under MCU control, LINEOUT2, microphone on MIC_P |
| 0xE1..0xE3, 0xE5 | 0x0F, 0x06, 0x52, 0x1A | codec |
| 0xE4 | 0x40 + mic gain | LINEOUT +2 dB, first mic stage 0 dB |
| page 1 0x10, 0x30, 0x40, 0x51 | 32, 16, 7 and 5 bytes | auxiliary tables (0x10 as the shared table but byte 24 = 0x10) |

The microphone input (MIC_P rather than LINEIN1) and the 0xE4 base are per board in the driver (`hrc_t.mic_input`,
`mic_gain_base`).

The voice port (V_SPI) is SPI1: PTB10 chip select (driven as a GPIO), PTB11 clock, PTB16 out, PTB17 in, mode 1,
about 1.2 MHz (U). One frame is 0x03 / 0x83, address 0, 27 bytes.

### 5.3 Interrupts

PTC7 (RF receive), PTC16 (RF transmit), PTC17 (system) on the falling edge, PTC18 (time slot) on either edge (U).
One PORTC handler (`boards/gd77/dmr_gd77.c`) at the RTOS syscall priority serves all four and feeds the shared slot
engine (`hrc6000_dmr`). The time slot line interrupts on both edges, so an edge within 5 ms of the last accepted one
is dropped: one interrupt per 30 ms slot. Configuration port transactions mask interrupts up to that level.

### 5.4 Transmit path

Written, off by default. Build variable `CONFIG_GD77_TX=y` marks both bands as transmit capable; without it `tx()`
refuses and the transmit code isn't built. With it `tx()` still refuses unless the calibration point
(`gd77_cal_at`, 6.4) is valid for the frequency: a valid block for the band and the frequency inside its TX range.

| Line | Role | Tag |
|---|---|---|
| PTA17 | TX enable | U |
| PTE2 | UHF TX path | U |
| PTE3 | VHF TX path | U |
| PTC15 | UHF RX path | U |
| PTC13 | VHF RX path | U |
| DAC0 | power drive: 8-bit drive value x 16 | U |

The band split for the path lines is 188.74368 MHz (U). The other band's RX line is held low.

### 5.5 RF switching order

| To | Steps |
|---|---|
| Transmit | RX paths off, tune, reg 0x30 to transmit, the band's TX path on, PTA17 high, DAC0 ramp up |
| Receive | PTA17 low, DAC0 ramp down, TX paths off, tune, reg 0x30 to receive, the band's RX path on |

DAC0 stays at 0 while receiving (U). Before the transmit order `tx()` writes the calibration's transmit values
(6.4): the AT1846S gains, PA driver current and deviation, and the HR_C6000 modulation amplitude, bias and offset. In
DMR it writes them when it arms; the per-burst switching writes only the tuning registers, so they hold for every
burst. Before the receive order `rx()` writes the receive values (6.4).

The ramp has 19 steps, value = drive x 16 x k / 19, 10 µs apart and 40 µs for the five steps nearest full drive
(U). In DMR, `tx()` only arms: the HR_C6000's RF TX interrupt runs the transmit order before each burst and the RF RX
interrupt the receive order after it, both from the interrupt with the AT1846S written over I2C at 100 kHz (about
1.7 ms, inside the 2.3 ms advance).

### 5.6 Vocoder

The codec is the radio's own AMBE+2 library, run on the MCU from flash: [codec-gd77.md](codec-gd77.md). The HR_C6000
only frames the voice. Without the codec merged in, the vocoder reports none (`present()` false), so DMR carries data and
signaling but no voice, and FM is unaffected. Beeps and tones use the PCM stream (section 8).

## 6. Memory map

### 6.1 Program flash and SRAM

| Range | Use | Tag |
|---|---|---|
| 0x00000000..0x00003FFF | the radio's bootloader; never written | U |
| 0x00004000..0x00055CFF | the ODR application, vector table first (`boards/gd77/gd77.ld`) | U |
| 0x00055D00..0x0007B7FF | the radio's AMBE+2 codec, merged in on the user's machine: code, table window, descriptor ([codec-gd77.md](codec-gd77.md)) | U |
| 0x0007B800..0x0007F7FF | not used | U |
| 0x0007F800..0x0007FFFF | the last 2 KB sector: radio data that must survive; never linked into or written | U |
| 0x1FFF0000..0x1FFFFFFF | SRAM_L: .data, .bss, newlib's small heap | U |
| 0x20000000..0x2000FEEF | SRAM_U: `ODR_CPU_RAM` state, the RTOS heap with every task stack, the main stack | U |
| 0x2000FEF0..0x2000FFFF | left unused: margin for anything the bootloader keeps there | U |

The initial stack pointer is 0x2000FEF0.

### 6.2 Data flash (SPI NOR)

| Item | Value | Tag |
|---|---|---|
| Part | 25-series SPI NOR, 16 to 128 Mbit (capacity code 0x15 to 0x18 in the JEDEC ID); the fitted part is not known | U |
| Commands | 0x9F JEDEC ID, 0x05 status, 0x06 write enable, 0x0B fast read (one dummy byte), 0x02 page program (256 bytes), 0x20 4 KB erase, 0xD8 64 KB erase, 0xC7 chip erase | U |
| ODR's driver | `drivers/flash/spi_nor` with `fast_read` set, through `gd77_nor_bus` (section 4) | |

The first megabyte holds the factory header (magic "TYT" at 0x000000) and the radio's codeplug as it came, the part
above the EEPROM's share at 0x07B1B0-0x08FFFF (U). ODR never erases or programs below 0x100000: `flash_gd77.c`
refuses it whatever asks, and counts the refusals (diagnostics line 8, `nor-`). Going back to the radio's original
firmware therefore finds all of that as it was.

ODR's areas follow the STM32 radios' layout ([settings-store.md](settings-store.md) 1), moved up by 1 MB
(`boards/gd77/storage_gd77.h`):

| Range | Area | Parts |
|---|---|---|
| 0x100000-0x10FFFF | open-firmware EEPROM image (64 KB) | 2 MB and up |
| 0x110000-0x110FFF | unused | |
| 0x111000-0x118FFF | settings ring, 8 sectors | 2 MB and up |
| 0x119000-0x11AFFF | sector journal, 2 sectors | 2 MB and up |
| 0x11B000-0x11FFFF | unused | |
| 0x120000-0x1FFFFF | open-firmware flash image, image offsets 0x00000-0xDFFFF (`openfw_end` 0x200000) | 2 MB and up |
| 0x200000-0x2FFFFF | native codeplug | 4 MB and up |
| 0x300000-0x33FFFF | codeplug staging | 4 MB and up |

The flash image is 128 KB after the EEPROM image, as on the STM32 radios, so ODR's own tools (fast CPS, TFTP) see the
same image offsets. The core gets this map only when the probed part holds it: a part under 2 MB, or one whose JEDEC
ID gives no size, gets no map, and the radio then runs its settings from RAM. The settings ring, the journal, the
foreign region guard, the first-boot ownership mark, record versions and CRCs are the core's
([settings-store.md](settings-store.md)); the board adds nothing to them.

### 6.3 EEPROM

| Item | Value | Tag |
|---|---|---|
| Part | 24C512 class: 64 KB, 16-bit memory address, 64-byte write page, 5 ms write cycle, erased value 0xFF | U |
| Address | 7-bit 0x50 (0xA0 / 0xA1) on I2C bus A, shared with the AT1846S | U |
| ODR's driver | `drivers/flash/i2c_eeprom`, acknowledge polling, reads in 64-byte transactions so the AT1846S never waits long | |

ODR only reads the EEPROM: the factory calibration (6.4) and, for diagnostics and a future import, the radio's own
logical codeplug space (`gd77_logical_read`):

| Logical address | Stored in | Tag |
|---|---|---|
| 0x00000-0x0B1AF | EEPROM, same address | U |
| 0x0B1B0-0x1FFFF | data flash at logical + 0x70000 (0x07B1B0-0x08FFFF) | U |

EEPROM 0x0B1B0-0x0B1B3 holds the target address of an interrupted rewrite of a data flash sector of that codeplug (0 or
0xFFFFFFFF when none), to be rebuilt from the copy at data flash 0x00D000 (U). ODR leaves both alone.

Settings stay on the data flash, not in the EEPROM's free top (0xB1B4-0xFFFF). The core's settings ring already
spreads wear over 128 slots. At the bus's 100 kHz, reading even a 2-sector ring (8 KB) from the EEPROM takes about
0.75 s, and start-up reads the ring twice (the region guard, then the settings load).

### 6.4 Factory calibration

Two blocks in the EEPROM, one per band, 0x70 bytes each: an 8-byte header, 103 bytes of fields in table order and one
spare byte. ODR reads both at start-up (`board_init`, before the radio is set up), twice each and compared, and
never writes them (`boards/gd77/cal_blocks_gd77.h`, on the generic loader `drivers/cal/cal_block`).

| Block | EEPROM | Used for | Tag |
|---|---|---|---|
| UHF | 0x7C00 | 180 MHz and up | U |
| VHF | 0x7C70 | below 180 MHz | U |

Header: two ranges of two 16-bit little-endian values in 100 kHz units, low then high. The range at +0 is the RX range
and the one at +4 the TX range (I: the TX values are spread over +4's range and the squelch nodes over +0's). The
block check below tests only the TX range.

A block is used only when it reads the same twice, isn't all 0xFF or all 0x00, and passes these checks:

| Check | Limit |
|---|---|
| field 0x50 bytes 0, 1, 2 | below 0x20, 0x40, 0x10 |
| TX range, each value | 0x550-0x1450 (1360-5200), not 0xA29-0xF9F (2601-3999) |
| TX range | low below high |

Fields (offsets from the block start), all U. "Per node" fields hold one value per frequency node (below);
"25 / 12.5" fields hold the 25 kHz value first, then the 12.5 kHz one (I):

| Id | Offset | Size | Holds | Goes to | Out of range |
|---|---|---|---|---|---|
| 0x40 | 0x08 | 3 | u16 LE two-point bias (at most 0x400), then an 8-bit DC offset | HR_C6000 4:0x47 = bias bits 7:0, 4:0x48 = the bits above; 4:0x04 = offset | bias above 0x400: no TX point |
| 0x41 | 0x0B | 48 | TX drive per node, two bytes each: power flag 0 (low), then 1 (high) (I: polarity) | DAC0 = byte x 16 | - |
| 0x42 | 0x3B | 4 | four values over the RX range | not used (?) | - |
| 0x43 | 0x3F | 20 | 8 squelch base bytes per node on the RX range; then three (high, low) pairs for 25 kHz and three for 12.5 kHz | the pairs: AT1846S regs 0x48, 0x60, 0x3F = high << 7 \| low; the base: not applied (squelch is the core's) | a pair with high or low 0x7F or above, or high below low: 0x213F, 0x1930, 0x29D0 |
| 0x44 | 0x53 | 2 | two squelch table bytes | not used (?) | - |
| 0x01 | 0x55 | 8 | modulation amplitude per node | HR_C6000 4:0x46, FM and DMR | - |
| 0x30 | 0x5D | 1 | codec DAC level | not applied: ODR sets 4:0x37 itself | - |
| 0x31 | 0x5E | 1 | unknown | not used (?) | - |
| 0x32 | 0x5F | 1 | voice gain for tone and DTMF transmit | not applied | - |
| 0x33 | 0x60 | 2 | CTCSS deviation, 25 / 12.5 | AT1846S reg 0x59 bits 5:0 with CTCSS or no tone | 0x40 and up: 0x0B |
| 0x34 | 0x62 | 2 | DCS deviation, 25 / 12.5 | AT1846S reg 0x59 bits 5:0 with DCS | 0x40 and up: 0x0B (I) |
| 0x50 | 0x64 | 8 | PA driver current, microphone PGA gain, TX voice gain, gain after the TX ADC; then u16 LE FM deviation, 25 / 12.5 | AT1846S reg 0x0A bits 14:11, 0x0A bits 10:6, 0x41 bits 6:0, 0x44 bits 11:8; reg 0x59 bits 15:6 | 16 and up: 5; 0x20 and up: 0x10; 0x80 and up: 0x1E; 16 and up: 2; 0x400 and up: 0x3A |
| 0x51 | 0x6C | 3 | RX DAC analog gain, RX digital volume, a third byte | AT1846S reg 0x44 bits 7:4, 3:0; the third byte not used (?) | 16 and up: 0xD, 0xC |

The register writes keep the other bits of each register. Reg 0x0A has no default in this radio's profile, so its
other bits are read from the chip first. DMR transmits with the 12.5 kHz values. 4:0x45, the HR_C6000's other
modulation register, has no calibration: ODR reads it once after the chip's power-on sequence and writes that value
back with every 4:0x46.

Frequency nodes. The frequency is taken in 100 kHz units, rounded down. The node layout depends on the TX range (ids
0x41, 0x01) or the RX range (id 0x43); a range not in this table has no layout and gives no TX point:

| Range (MHz) | Id 0x41 nodes | Ids 0x01 and 0x43 nodes |
|---|---|---|
| low end to 520 | 24: from the low end every 5 MHz, the last at 520 | 8, every 15 MHz from the low end |
| low end to 480 | 16: from the low end every 5 MHz, the last at 480 | 8, every 10 MHz |
| low end to 470 | 14: from the low end every 5 MHz, the last at 470 | 7, every 10 MHz |
| 320 to 400 | 16: every 5 MHz, the last at 400 | 8, every 10 MHz |
| 300 to 400 | 20: every 5 MHz, the last at 400 | 7, every 15 MHz |
| 136 to 174 | 8: 136.0, 140, 145 .. 165, 174 | 8: 136.0, 140, 145 .. 170 |

The 320 and 300 MHz low ends fail the block check, so only an RX range can use those rows. For 136 to 174 the nodes
count from 135.0 MHz, and the first stands at 136.0.

| Rule | Value |
|---|---|
| Between two nodes | linear, rounded toward the lower node's value |
| Id 0x41 at or above its last node | the last node is the range's high end, so the value is the last byte |
| Id 0x01 above its last node, up to the high end | the line through the last two node values spread over [last node, high end], plus their difference; saturated to 0..255 |
| Id 0x43 | the byte of the node at or below the frequency, no interpolation |
| Outside the range | ids 0x41 and 0x01: no TX point (the block's own rule for id 0x41 there is the byte 0xA0, which ODR doesn't use); id 0x43: no squelch base |

The calibration point. `gd77_cal_at(hz, out)` (`boards/gd77/cal_gd77.c`) picks the block by frequency and converts
it (`gd77_cal_convert`, pure, host tested in `tests/host/gd77CalTest.c`). Its validity:

| Flag | True when | Then |
|---|---|---|
| `valid` (the return value) | the block for the band is valid, its TX range has a node layout, the frequency is inside it and the bias is at most 0x400 | `tx()` may transmit with the TX values |
| `rx_valid` | the block for the band is valid | the receive values come from the block |
| `sq_valid` | also the frequency is inside the RX range and that range has a node layout | `sq_open` is the block's |

Without `valid` every TX value is the safe one (drive 0) and `tx()` refuses. Without `rx_valid` the receive values are
the fallbacks of the field table, which `rx()` writes all the same, so a band without a valid block doesn't keep the
other band's values. Board power level 0 is low and level 1 high (I). The raw bytes stay readable with
`gd77_cal_field(hz, id, buf, max)`. Diagnostics line 8 shows each block's state (`cal` UHF then VHF: `o` ok, `f`
read failed, `b` blank, `r` rejected).

## 7. Display

128x64 monochrome, UC1701 / ST7567 class controller on the shared driver (`drivers/display/st7567`). Init: 0x2F
(regulator, follower and booster on), 0x81 0x11 (electronic volume), 0xE3, 0xA2 (bias 1/9), 0xA1 (segment
reverse), 0xC0 (common normal), 0xA4, 0xA7, 0xAF; column offset 4 (U). Whether the panel needs 0xA7 (inverse) is open
(?); the driver's default (normal) is used. The backlight on PTC4 is on or off. A build with `GD77_BACKLIGHT_PWM`
dims it instead: FTM0 channel 3 on the pin's ALT4, edge-aligned at 1 kHz from the bus clock, the brightness setting as
the duty cycle (U, 4.2). The pin starts as a GPIO driven low; it moves to the FTM only once the timer runs at 0 %.

## 8. Audio path

The HR_C6000's codec handles the speaker and the microphone, as on the DM-1701. For FM receive the chip passes audio
through (`hrc6000_fm_audio`) and PTB0 switches the speaker amplifier (?). For PCM (beeps, DMR voice) the board sets
0xE2 bit 1 (DAC to the line out) and 0xE0 bit 3 (LINEOUT2), and in DMR keeps 0x22 bits 1 and 2 set (U). The
microphone is on MIC_P (U, section 5.2).

Digital audio runs over I2S0 with the HR_C6000 as master (U): a 512 kHz bit clock, an 8 kHz frame sync one bit early,
two 32-bit words a frame, MSB first, the 16-bit sample in the low half of each word. The K22F's receiver takes the
clocks and the transmitter runs synchronous to it. DMA channel 0 (DMAMUX source 12) drains the receiver and channel 1
(source 13) fills the transmitter (`platform/k22f/i2s.c`). An alternative I2S pin set is on PTC1, PTC5..PTC7 (?).

Each ring is 320 halfwords (160 frames, 20 ms) with an interrupt at half and full. Receive keeps slot 0; transmit
writes the mono sample into both slots (`boards/gd77/pcm_gd77.c`). Two halves make one 160-sample block, handed to
the core through `hal_audio` from an audio task, as on the DM-1701. A block the task hasn't taken when the next one
completes is dropped and counted; a transmit half with no block is sent as silence. The stream start waits up to
25 ms for the first half and fails if none comes.

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
back, C up, D down, E left, F right (`boards/gd77/keymap_gd77.h`, the only file that changes when a radio shows
otherwise). The scan drives one line low at a time, waits 5 µs and reads the rows; a change
counts after two equal scans 10 ms apart. The diagnostics page shows the raw bits (`keys`): bit = column x 5 + row,
direct keys at bits 20 to 23.

| Direct key | Pin | Provisional role | Tag |
|---|---|---|---|
| Side key 1 | PTB1 | monitor | ? |
| Side key 2 | PTB9 | talk target; held: scan | ? |
| Top key | PTA2 | power level; held: emergency where it is built, else speak status | ? |
| PTT | PTA1 | PTT | ? |

### 9.2 Power, battery, LEDs

PTE26 holds power. The power knob switch on PTA13 reads high while on; three low polls in a row start the shutdown
(U). The battery reading takes ADC0_DP1 (channel 1) with a provisional 3:1 divider and a 3.3 V reference (?), so the
board sets no low-battery thresholds yet. The LEDs are the PTC14 / PTB18 pair (?), red on PTB18 in the code. The
power-off sequence is in 15.3.

## 10. Update path and image packaging

Holding both side keys at power-on starts the radio's update mode (U). `make BOARD=gd77` produces the raw application
image (`odr_gd77.bin`, linked for flash 0x4000) with its `.elf` and `.map`, and, with a stock firmware file in
`codecs/`, `odr_gd77_codec.bin`: the same image with the codec merged in (`tools/gd77_codec.py`), a flat image from
flash 0x4000 up to 0x7B800. The radio's loader takes an update file over USB HID: odrflash and the web flasher write the image into a copy of an
update file the user supplies and send it ([update-gd77.md](update-gd77.md)). Unverified on a radio.

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
| Transmit path | PTA17, RF switches, DAC0 as in 5.4 and 5.5 | with the radio on a dummy load, after the receive path works |
| Microphone input | MIC_P (0xE0 bit 1) or LINEIN1 | speak into the microphone with the `pcm` line running; compare levels |
| Voice port | SPI1 on PTB10, PTB11, PTB16, PTB17 | the `vp` count on the diagnostics page stays 0 during DMR receive |
| Time slot edges | both edges of PTC18 | the `ts` counts: one accepted per 30 ms, the other edge dropped |
| Bus pin mux (4.2) | the hardware options listed | check against the K22F data sheet's pin mux table, then try one |
| Backlight PWM (4.2, 7) | FTM0 channel 3 on PTC4 ALT4, or another timer | the data sheet's pin mux table, then `GD77_BACKLIGHT_PWM=1` |
| HSRUN target (2.2) | 95.232 MHz in HSRUN | `GD77_CLOCK_TARGET=1`: `board.clk.hsrun=1` and the core clock in the board view (14) |
| USB clock (13) | IRC48M with clock recovery | enumeration, then `board.usb.trim_error=0` |
| Crash record (15.4) | kept across a reset in SRAM_U | a test crash (`odr_debug.py crash test`) and the record shown after the restart |
| Data flash size | 16 to 128 Mbit; ODR needs 2 MB for its areas (6.2) | the `nor` diagnostics line's JEDEC ID |
| EEPROM | 24C512 class at 0x50 | the `eep` diagnostics line |
| Calibration blocks | 0x7C00 / 0x7C70 hold valid blocks on a factory radio | the `cal` diagnostics line shows `oo` |
| Calibration fields | the field table in 6.4: power flag 0 = low, sub-index 0 = 25 kHz, +4 = TX range, +0 = RX range; ids 0x31, 0x42, 0x44 and byte 2 of 0x51 unused | with a factory block loaded, compare the AT1846S and HR_C6000 registers and DAC0 with the table at each power level and bandwidth, then measure power and deviation on a dummy load |
| Calibration values | real value ranges; whether the RX range always equals the TX range | read the blocks of several factory radios (EEPROM 0x7C00-0x7CDF) |

## 12. Bring-up status

| Area | State |
|---|---|
| SoC: start-up, vector table, clocks, SysTick, GPIO / PORT, DSPI, PIT, ADC, DAC, watchdog off | written, builds |
| SoC: I2S0 with DMA | written, started by the board's audio stream |
| SoC: flash self-programming (15.1) | written, host tested (window, command object), unverified |
| SoC: I2C controller, DSPI chip select, FTM PWM (4.1) | written, off by default, unverified |
| SoC: HSRUN target, RUN step, crash capture (2.2, 15) | written, target off by default, unverified |
| Display, keys, power hold, power switch, backlight, LEDs | written, unverified |
| FM receive (AT1846S, HR_C6000 FM path) | written, unverified |
| Audio: PCM stream, speaker amplifier, beeps (8) | written, host tested (framing), unverified |
| AT1846S profile, RF switching order, DAC0 ramp (5.1, 5.5) | written, host tested, unverified |
| Transmit (FM and DMR) | written; refused unless built with `CONFIG_GD77_TX=y` and calibrated |
| DMR receive (interrupts, slot engine, voice port) | written, unverified |
| DMR voice codec (5.6) | the radio's own codec run in place, merged on the user's machine ([codec-gd77.md](codec-gd77.md)); host checked, unverified |
| Storage: settings and codeplug on the data flash (6.2), EEPROM read only (6.3) | written, host tested, unverified |
| Factory calibration: both blocks loaded and checked, converted and applied (6.4) | written, host tested, unverified |
| USB serial: host link, debug link, board view (13, 14) | written, unverified; the CPS and the network adapter off |
| Update image packaging and flashing (odrflash, web flasher) | written and host tested; unverified on a radio |

## 13. USB

The K22F's full-speed USB controller in device mode on its dedicated USB0_DP / USB0_DM pins (U), driven by TinyUSB's
buffer descriptor table driver (`portable/chipidea/ci_fs`) behind the repository's USB device layer
(`drivers/usb`), as on the STM32 radios.

| Item | Value | Tag |
|---|---|---|
| Clock | IRC48M, the internal 48 MHz oscillator, as the USB clock (SIM_SOPT2 PLLFLLSEL = 3, USBSRC = 1, CLKDIV2 = 0); the controller's clock recovery trims it to the host's start-of-frame packets. No PLL plan gives 48 MHz | I |
| Regulator | the controller's regulator on (SIM_SOPT1 USBREGEN, after SOPT1CFG URWE) | U |
| Interrupt | USB0 (IRQ 53) at priority 6 | U |
| Identity | 1FC9:0094 serial, 1FC9:0096 network, the same as the other ODR radios with USB; product "GD-77"; serial number: the low 96 bits of the chip's unique ID as 24 hex digits | |
| Personality | CDC-ACM: the host link (`core/hostlink`) and the debug link on it. The network adapter (CDC-NCM) and the open-firmware CPS are off in this build | |
| Endpoints | interrupt IN 0x82 (8 bytes), bulk OUT 0x01 / IN 0x81 (64 bytes), control 64 bytes | |

The selector the USB clock uses (PLLFLLSEL) also feeds the I2S master clock, which this radio doesn't use: the
HR_C6000 drives the I2S clocks (section 8). The clock switch at start-up leaves the selector alone once USB has it.
The radio's own update mode (section 10) is a different USB device, from the bootloader.

## 14. Debug link board view

The debug link's `board` view section ([debuglink.md](debuglink.md) 3.7, section 22) carries the readings a first
session on a radio needs, read without side effects. `tests/hil/test_gd77.py` checks them.

| Key | Value |
|---|---|
| `board.clk.core_hz`, `board.clk.bus_hz` | the clocks now |
| `board.clk.source` | `pll`, or `fll` after a fallback |
| `board.clk.hsrun`, `board.clk.stepped_down` | HSRUN mode; in the RUN step of 15.1 |
| `board.reset.cause`, `board.reset.srs` | the last reset's source by name (`por`, `pin`, `sw`, `wdog`, `lockup`, ...), RCM_SRS0 \| RCM_SRS1 << 8 |
| `board.chip.sdid`, `board.chip.flash_kb` | SIM_SDID; the program flash size from SIM_FCFG1 |
| `board.pins.a` .. `board.pins.e` | each port's input data register (GPIO_PDIR): the level of every digital pin |
| `board.keys.raw` | the raw key bits of 9.1 |
| `board.adc.dp1` .. `board.adc.dp3` | the last ADC0 readings of DP1 (battery candidate), DP2, DP3; 65535 = none yet |
| `board.chip.at1846s` | `ok:<reg 0x00>` or `none:` |
| `board.chip.hrc6000`, `board.chip.eeprom` | `ok` or `none` |
| `board.chip.nor_jedec` | the data flash's JEDEC ID, 0 = no answer |
| `board.bus.i2c_a`, `i2c_b`, `nor`, `lcd`, `c6000` | `soft` or `hw`, then the bus state (`ok`, `refused`, `not_implemented`, ...) |
| `board.i2c0.errors` | failed I2C0 transactions |
| `board.usb.trim_error` | the clock recovery ran out of trim range |
| `board.backlight.pwm` | the backlight runs on the FTM channel |

## 15. System

### 15.1 Flash self-programming

`platform/k22f/ftfa.c`: erase a 2 KB sector (FTFA command 0x09) and program longwords (0x06). The window is
0x4000..0x7F7FF: the bootloader below 0x4000 (with the flash configuration field at 0x400) and the last sector
(6.1) are refused (`ftfa_math.h`, host test `k22fSystemTest`). The command loop runs from RAM (`.ramfunc`, copied with
`.data`) with interrupts off, since the flash can't be read while a command runs. Waits are bounded by the cycle
counter: 300 ms for an erase, 5 ms for a longword. The flash cache and prefetch buffer are invalidated after each
command. The flash controller doesn't run commands in HSRUN, so around each one an HSRUN plan steps down to RUN: the
core drops to the bus clock (47.616 MHz) and the bus, flash and peripheral clocks stay as they are. Nothing in this
build calls the API yet; it is for an update path and the radio's data sector, both open.

### 15.2 Watchdog and standby

The watchdog is switched off at start-up and stays off, as on the other ODR radios: the debug link, flash commands
and bring-up waits block for long stretches, and a watchdog would need a policy for the whole tree first. Standby is
the same RUN step as 15.1 (the core at the bus clock, no peripheral change); no core hook calls it yet. Wait and stop
modes aren't used: the cycle counter the millisecond clock runs on doesn't count while the core sleeps.

### 15.3 Power off

The core saves and drops the power hold (PTE26). With the knob switch off (PTA13 low) the supply should go; if the
radio still runs 500 ms later, it resets, so it comes back in its start-up state instead of staying dark. When this
start was already a software reset, it doesn't reset again: it stays down and dark until the knob turns it on. With the
knob on (auto power off), the core carries on and says so. (U)

### 15.4 Reset cause and crash log

The reset cause is read at start-up from RCM_SRS0 and RCM_SRS1 and named in the board view (14) and on the
diagnostics page (`rst`). The crash log ([crash-log.md](crash-log.md)) is on: the Cortex-M4 fault capture
(`platform/k22f/crash_k22f.c`) keeps the record in `.crash_noinit` in SRAM_U, which start-up doesn't clear. The K22F has
no backup RAM; the bootloader runs between the reset and the next start and may use that RAM (U). The record's CRC
decides: a record it overwrote is ignored, never shown wrong.

