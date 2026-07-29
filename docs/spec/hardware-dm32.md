<!-- SPDX-License-Identifier: MIT -->
# Baofeng DM-32UV hardware

Status: research, 2026-10-09. Everything a fresh firmware needs to bring up the DM-32UV: SoC, memory, peripherals,
pins, RF chip, audio, flash, calibration, GPS and the update paths. The DMR modem inside the SoC is in
`modem-hrc7000.md`; the AMBE codec placement is in `codec-dm32.md`.

**Nothing in the DM-32 tree this was taken from has been flashed to or tested on a radio.** The only DM-32 builds
known to work on air are other firmware releases (July, August, September 2026) and the stock
Baofeng firmware.

## Confidence tags

Every fact carries one tag.

| Tag | Meaning |
|---|---|
| **DS** | HR_C7000 user guide (Chinese original with English translation, v2.7, 2016), or another datasheet |
| **BIN** | Observed in the stock DM32.01.01.049 / 046 image or the 2026 OpenDM32 images by disassembly |
| **KP** | Taken from another firmware's source port (FM reportedly worked; DMR did not). Unverified on hardware |
| **COM** | Community pin map (infotex58.ru forum topic 1168, `pinmap_14.06.26`), bench probing by others, unverified |
| **INF** | Inferred by us from the above; not seen directly |

Where a KP value was later confirmed in an OpenDM32 binary it is tagged BIN.

---

## 1. SoC

| Item | Value | Tag |
|---|---|---|
| SoC | Hongruan HR_C7000, sold by Dahua as DH4570 | DS, KP |
| CPU | C-SKY CK803S (32-bit, little endian, 16/32-bit mixed instruction set); toolchain target `-mcpu=ck803s` | KP |
| ELF e_flags that make objdump decode CK803 32-bit opcodes | 0x21002009 | BIN |
| Interrupt controller | Dahua PIC at 0x17000000, 64 sources, normal interrupts only (no fast interrupts on C7000) | DS |
| Crystal | 24 MHz system crystal; 32.768 kHz RTC crystal with coin cell backup | DS, KP |
| DMR/FM baseband | on chip ("modem", registers at 0x11000000, buffers at 0x16000000) | DS |
| Audio codec | on chip (byte registers at 0x160009C0) | DS |

### 1.1 Clocks and PLL (at boot, before anything else)

All registers are in the modem/system block at 0x11000000. Sequence as done by the port (KP); the resulting 168 MHz
core clock matches what UART divisors and timers assume and FM worked in another firmware's build.

| Step | Register (offset) | Write / wait | Meaning |
|---|---|---|---|
| 1 | SYS_SOFT_RSTN (0x000) | 0x180 | CPU and system out of reset (bits 8, 7); modem sub-blocks held (low-active resets, DS) |
| 2 | QUAD_ENABLE (0x05C) | 0x01000001 | XIP flash controller (LCSFC) quad mode on, input sample delay 1 |
| 3 | LCSFC_BAUDR (0x030) | 0x02 | XIP flash SCLK divider (reset default 4) |
| 4 | CLK_MGR 0x010 | clear bits 23:0, then set refdiv=1 at bit 18, fbdiv=28 at bits 6+, postdiv1=2 at bits 5:3, postdiv2=2 at bits 2:0 | 24 MHz / 1 x 28 = 672 MHz VCO, / 2 / 2 = 168 MHz |
| 5 | CLK_MGR 0x028 | (7 << 16) or 6 | USB clock = VCO / 2 / 7 = 48 MHz; SDIO divider 6 |
| 6 | CLK_MGR 0x024 | clear bits 3:0, set bit 1 | PLL:AHB = 1:1. The port's comment says AHB:APB = 1:4 but every peripheral divisor in the port assumes **APB = 84 MHz (sys / 2)** |
| 7 | CLK_MGR 0x004 | set bits 1:0 | release APLL / BPLL reset |
| 8 | CLK_MGR 0x004 | poll bit 31 every 100 us | PLL locked |
| 9 | CLK_MGR 0x004 | clear bit 2, then bit 3 | switch the system clock from the 24 MHz oscillator to the PLL |
| 10 | CLK_MGR 0x004 | poll bit 30 every 100 us | switch done |
| 11 | CLK_MGR 0x02C | 0xFFFFFFFF | all peripheral clock gates on |
| 12 | IO_DIPLEX2 (0x03C) | clear bits 1:0 | CLK_OUT pin = PLLA clock out (DS: 0 = PLLA, the port's comment says PLLB) |

Resulting clocks used everywhere below: CPU/AHB 168 MHz, APB 84 MHz (timers, UARTs, SPI, I2C), SoC reference
24 MHz. Soft reset of the whole chip: QUAD_ENABLE = 0, then SYS_SOFT_RSTN = 0 (KP).

### 1.2 Boot header and start-up

| Item | Value | Tag |
|---|---|---|
| ROM boot flow | After reset the ROM offers a serial download window; if no host answers it checks flash address 0 for the Dahua flag and jumps to the 32-bit address stored at flash offset 0x04 | DS (flow chart) |
| Dahua flag bytes | 0x74 0x68 0x64 0x0C (`thd\x0c`), followed by the little-endian entry address; also present in the miniBoot image (entry 0x00015C7C) and inside stock 049 | KP, BIN |
| Where an app needs it | Only an image that sits at flash 0. Apps started by the Baofeng bootloader at 0x0300C000 do not carry it (stock 049 and OpenDM32 start directly with code) | BIN |
| Initial PSR | 0x80000100 (supervisor mode, exceptions enabled) | KP |
| Stack top | 0x00057FFC (port); OpenDM32 uses 0x000478FC because its codec lives at 0x47900 | KP, BIN |
| Vector base | VBR points at a 1 KB aligned table copied into RAM (OpenDM32 Aug: VBR = 0x00011800) | KP, BIN |
| Start-up copies | .data and RAM code from flash to SRAM 0x10000+; codec tables to modem SRAM 0x18000800 (see section 2) | KP, BIN |

### 1.3 Cache controller (set before running from XIP flash)

Controller at 0xE000F000 (KP, same sequence in stock-style start-up):

| Order | Address | Write | Meaning |
|---|---|---|---|
| 1 | 0xE000F004 | 0x1 | invalidate all |
| 2 | 0xE000F000 | 0x04 | write-back, 4-way, instruction and data cached |
| 3 | 0xE000F008 | 0x0300002D | cacheable region base 0x03000000, size code 0x2D (8 MB per the port's comment) |
| 4 | 0xE000F000 | set bit 0 | enable |

---

## 2. Memory map

| Range | Size | What | Tag |
|---|---|---|---|
| 0x00010000 – 0x00057FFF | 288 KB | Main SRAM ("IRAM"): data, bss, heap, stacks, RAM-resident code | DS, KP |
| 0x03000000 – 0x030FFFFF | 1 MB | Program flash (GD25Q80 class, 1 MB), memory-mapped, execute in place through LCSFC | DS, BIN |
| 0x03000000 – 0x0300BFFF | 48 KB | Baofeng bootloader (PTT + SK1 update mode). Never overwrite when using the Baofeng update path | BIN, INF (exact end) |
| 0x0300C000 | | Application base for stock 049, 046 and every OpenDM32 release; what the Baofeng bootloader writes and starts | BIN |
| 0x03001000 | | Application base used by the dh4570 source with the ROM serial boot script (overwrites the Baofeng bootloader area: INF) | KP |
| 0x03061000 | 76577 B | Where the dh4570 codec pair is linked to run in place; apps using it must end below | KP |
| 0x030C1000 – 0x030D16D8 | | OpenDM32 codec code (load image), copied to RAM 0x47900 at boot | BIN |
| 0x030D16D8 – 0x030D5478 | | OpenDM32 codec tables, copied to 0x18000800 | BIN |
| 0x030BFBA0 → 0x10000 | 0x1F098 | Stock 049 RAM-code block (copied by stock crt0) | BIN |
| 0x04000000 | | LCSFC flash controller register window (port name FLASH_SPI_BASE; not used directly) | KP |
| 0x11000000 – 0x1100FFFF | | Modem / system registers (clock, reset, IO mux, baseband) | DS |
| 0x12000000 – 0x1200FFFF | | i8080 LCD controller | DS |
| 0x13000000 – 0x1303FFFF | | USB controller (not wired to anything useful on the DM-32: programming is over UART0) | DS, INF |
| 0x14000000 – 0x1415FFFF | | APB peripherals (section 3) | DS |
| 0x15000000 | | SDIO | DS |
| 0x16000000 – 0x1600FFFF | | Modem buffer RAM (DMR TX/RX RAM, AMBE voice RAM, prompt FIFO, codec registers; `modem-hrc7000.md`) | DS |
| 0x17000000 | | PIC | DS |
| 0x18000000 – 0x18007FFF | 32 KB | SRAM on the system AHB, shared with the modem | DS |
| 0x18000000 – 0x1800009F | 160 B | Microphone PCM for the encoder (written by hardware) | DS |
| 0x180000A0 – 0x1800013F | 160 B | Decoded PCM for the speaker (written by the CPU) | DS |
| 0x18000800 – 0x18007FFF | 30 KB | Free for software; the codec's tables are copied to 0x18000800 (OpenDM32: up to 0x180045A0; stock 049: up to 0x18004BFC) | BIN |
| 0xE000F000 | | Cache controller | KP |
| 0xE000E000 | | C-SKY core timer (not used by the port) | KP |

Flash file offset in a Baofeng firmware file = runtime address − 0x0300C000 + 0x100 (BIN).

---

## 3. Peripheral base addresses

| Base | Block | Notes | Tag |
|---|---|---|---|
| 0x14000000 | Timers 1–6 (DesignWare style), 0x14 bytes each | Timer 1 at +0x00, 2 at +0x14, 3 at +0x28, 4 at +0x3C | DS, KP |
| 0x14010000 | Watchdog | | KP |
| 0x14020000 | GPIO A (pins 0–31) | | DS |
| 0x14030000 | UART0 | programming cable | DS |
| 0x14040000 | UART1 | GPS | DS |
| 0x14050000 | UART2 | unused | DS |
| 0x14060000 | I2C0 | ALPU-MP (hardware I2C selected by the IO mux) | DS, COM |
| 0x14070000 | I2C1 | pins used as GPIO for the FD6818 instead | DS |
| 0x14080000 | I2C2 | internal: the RTC sits on it | DS |
| 0x14090000 | UART3 | unused | DS |
| 0x140A0000 | SPI master 0 (DesignWare SSI) | 16 MB data flash | DS, BIN |
| 0x140B0000 | SPI master 1 | unused | DS |
| 0x140C0000 | PWM | LCD backlight | DS, KP |
| 0x140D0000 | ADC controller | | DS, KP |
| 0x140E0000 | SPI2 | unused (shares LCD pins) | DS |
| 0x140F0000 | DAC controller (3 channels) | The manual's register table header says 0x140D0000; the memory map, the port and OpenDM32 use 0x140F0000 | DS, BIN |
| 0x14100000 | GPIO B (pins 32–63) | | DS |
| 0x14110000 | GPIO C (pins 64–95) | | DS |
| 0x14120000 – 0x14140000 | SPI slaves 0–2 | unused | DS |
| 0x14150000 | SPI master 3 | eFuse only | DS |

### 3.1 UARTs (16550 style)

Registers at base +: RBR/THR/DLL 0x00, DLH/IER 0x04, IIR/FCR 0x08, LCR 0x0C, LSR 0x14, USR 0x7C (DS, KP).

| Item | Value | Tag |
|---|---|---|
| Divisor | APB / 16 / baud, APB = 84 MHz (115200 → 45, 1.3 % fast; 9600 → 546) | KP |
| Divisor latch | LCR bit 7 set, write DLL then DLH, clear bit 7 | KP |
| Format | LCR |= 0x03 (8N1) | KP |
| FIFO | FCR = 0x67: FIFOs enabled and reset, RX trigger 1 char (bits 7:6 = 01), TX trigger code 2 | KP |
| TX ready | USR bit 1 (TX FIFO not full) | KP |
| RX ready | LSR bit 0 | KP |
| Interrupts | GPS uses IER = 0x05 (RX data + line status); on stop, IER = 0 and FCR bits 1, 2 to flush | KP |
| UART0 | 115200 8N1: programming cable (Baofeng CPS, legacy-protocol CPS, hotspot) | KP, BIN |
| UART1 | 9600 8N1: GPS RX only | KP |

### 3.2 SPI master (DesignWare SSI)

Registers at base +: CTRLR0 0x00, CTRLR1 0x04, SSIENR 0x08, SER 0x10, BAUDR 0x14, TXFTLR 0x18, RXFTLR 0x1C, TXFLR
0x20, RXFLR 0x24, SR 0x28, IMR 0x2C, ISR 0x30, DMACR 0x4C, IDR 0x58, version 0x5C, DR 0x60 (DS, KP).

Set-up used for the data flash (KP): SSIENR = 0; CTRLR0 = 0x07 (8-bit frames, mode 0, TX/RX); CTRLR1 = 0; BAUDR =
2 (APB / 2 = 42 MHz SCLK, INF); thresholds 0; IMR = 0; DMACR = 0; SSIENR = 1. Per byte: wait SR bit 1 (TX not
full), write DR, wait SR bit 2 (TX empty), wait SR bit 0 clear (not busy), read DR. SER = 1 for the transfer, 0
after. Chip select is also driven by hand on GPIO 18 (see 8.1).

### 3.3 I2C2 and the internal RTC

The RTC hangs off the internal I2C2 at 7-bit address 0x70 (DS: device address 1110000b; KP uses TAR = 0x70).

I2C2 set-up (KP): ENABLE = 0; CON = 0x65; INTR_MASK = 0x40; FS_SCL_HCNT = 0x61; FS_SCL_LCNT = 0x68; TX_TL = RX_TL = 6.
Registers at base +: CON 0x00, TAR 0x04, DATA_CMD 0x10, FS_SCL_HCNT 0x1C, FS_SCL_LCNT 0x20, INTR_MASK 0x30, RX_TL 0x38,
TX_TL 0x3C, CLR_INTR 0x40, ENABLE 0x6C, STATUS 0x70, ENABLE_STATUS 0x9C, and a non-standard START at 0xA0 (KP).

Write of one RTC register: ENABLE = 0, read CLR_INTR, TAR = 0x70, ENABLE = 1, DATA_CMD = register, DATA_CMD = value,
START = 1, wait STATUS & 0x05 == 0x04, START = 0, ENABLE = 0, wait ENABLE_STATUS bit 0 clear. Read: drain RX FIFO
(STATUS bit 3), same opening, DATA_CMD = register, DATA_CMD = 0x100 (read), START = 1, wait STATUS bit 3, read
DATA_CMD (KP).

| RTC register | Use | Tag |
|---|---|---|
| 0x00 / 0x01 / 0x02 | current seconds / minutes / hours (6, 6, 5 bits) | KP, DS |
| 0x03 / 0x04 | current day count, low / high byte (16 bits, days since an epoch the firmware chooses) | KP, DS |
| 0x0C / 0x0D / 0x0E | load seconds / minutes / hours | KP |
| 0x0F / 0x10 | load day low / high | KP |
| 0x11 | RTC_CCR: bit 0 interrupt enable, bit 1 run, bit 2 load (self clearing); port writes 0x07 | DS, KP |
| 0x12 | RTC_LOAD_VSTAT bit 0: set to 0 before loading, 1 after (marks the time valid) | DS, KP |

DS first-power-up order: reset the RTC I2C interface, soft-reset the RTC (RTC_SOFT_RST_N = 0, wait 25 ms, = 1),
optionally set the match (alarm) registers, load time, CCR.

### 3.4 Timers

DesignWare-style, clocked from APB 84 MHz (KP). Per timer: LOADCOUNT +0x00, CURRENTVALUE +0x04, CONTROLREG +0x08, EOI
+0x0C (read to clear), INTSTATUS +0x10.

CONTROLREG: bit 0 enable, bit 1 user-defined (periodic reload) mode, bit 2 interrupt mask (0 = interrupt on) (DS
convention, KP use). Start: CONTROLREG = 0x02, LOADCOUNT = 84 000 000 / Hz, CONTROLREG |= 0x01.

| Timer | Vector | Use in the port | Tag |
|---|---|---|---|
| 1 (0x14000000) | 33 | Registered as the RTOS tick, **but never started** in the port (see section 16) | KP |
| 2 (0x14000014) | 34 | 1 kHz system tick | KP |
| 3 (0x14000028) | 35 | 1 Hz battery ADC poll in the port; **stock and OpenDM32 use Timer 3 as the DMR PA one-shot** | KP, BIN |
| 4 (0x1400003C) | 36 | DMR PA one-shot in the port | KP |
| 5, 6 | 37, 38 | unused | DS |

### 3.5 Watchdog (0x14010000)

LOCK +0x00, OT_LOAD_H +0x04, OT_LOAD_L +0x08, RST_LOAD +0x0C, EN +0x10, KNOCK +0x14. Unlock key 0x5ADA7200, feed value
0x55AADD22. Disable: LOCK = 0x5ADA7200, EN = 0, LOCK = 0 (KP). Whether the stock bootloader leaves it running is
unknown.

### 3.6 PWM (backlight)

PWM0: CFG +0x00, RPT +0x04, PER +0x08, FP +0x0C, STATUS +0x10. PWM0 comes out on PTA15 (= GPIO 0x0F, LCD backlight)
when IO_DIPLEX0 bit 17 = 0 (DS). Dimmed mode used by the port (KP): IO_DIPLEX0 bit 17 = 0, CFG = 0x328, PER = 0xC8
(200), FP = percentage, CFG = 0x32C, CFG = 0x32D. Full on or off: IO_DIPLEX0 bit 17 = 1 and drive GPIO 0x0F as an
output. With PER = 200 and FP = 0..100 the duty only reaches about 50 % (INF: check on hardware). Key backlight
(GPIO 0x28) is on/off only.

### 3.7 ADC (0x140D0000)

10-bit, 8 channels A–H wired to ADC0_IN..ADC7_IN on PTC15..PTC22 when the IO mux selects them (DS).

Registers (DS): CTRL 0x00 (bit 3 soft reset, bit 2 continuous scan, bit 1 glitch filter, bit 0 low power), INTR_DELTA
0x04, INTR 0x08 (bit 16 + per-channel enables), INTR_STA 0x0C, START 0x14, CTRL_STATE 0x18, INTR_THRESHOLD 0x1C,
CTRL_STOP 0x20, CH_VLD 0x24, PD_SEOC_TIME 0x28, P2S_EN 0x2C, DATA_AB 0x30 (A in 9:0, B in 25:16), DATA_CD 0x34 (C in
9:0, D in 25:16). In single scan only the lowest enabled channel is converted (DS).

| Signal | How the port reads it | Scale | Tag |
|---|---|---|---|
| Battery | Single scan, CH_VLD = 0x04 (channel C = ADC2_IN), INTR = 0x10004, PD_SEOC_TIME = 0xA20, wait INTR_STA bit 2, DATA_CD bits 9:0 | volts × 10 = raw / 8.2 (so raw 672 = 8.2 V). Thresholds used: 8.2 V full, 6.4 / 6.2 V low-battery hysteresis, 5.5 V power-off | KP |
| Mic level (VOX) | CH_VLD = 0x05, INTR = 0x10005, reads DATA_CD bits 25:16 (channel D) | **Suspect**: single scan converts only channel A, and IO_DIPLEX2 leaves ADC0/ADC3 pins as GPIO, so this probably reads nothing useful | KP, INF |
| Volume knob | **Not read at all** (the variable for it is never written) | — | KP |

IO_DIPLEX2 = 0x0FC80100 at boot leaves ADC1_IN and ADC2_IN analog (bits 20, 21 clear) and ADC0, ADC3..7 as GPIO (KP,
DS bit map). So the analog inputs actually available are channels B and C. Which one is the volume pot is unknown.

### 3.8 DAC (0x140F0000, 3 channels, 12-bit)

Registers (DS): PD_CTRL 0x00 (bit n = 1 powers channel n down; A=0, B=1, C=2), PD_MODE_EN 0x04 (low-power mode per
channel), DATA_A 0x08, DATA_B 0x0C, DATA_C 0x10. Output pins DAC_VOUT_MCUA/B/C on PTC23/24/25 when IO_DIPLEX2 bits
27/28/29 are 0.

| Channel | Use | Values | Tag |
|---|---|---|---|
| A | unused (powered down, pin left as GPIO) | 0 | KP |
| B | **PA drive / APC bias**. 0 = PA off. TX value from the factory calibration (section 9), 0..4095 | port fallback without calibration: 1408 | BIN, KP |
| C | unknown analog control, set once | 0x06E2 at init | KP |

At init the port writes PD_CTRL = 0x01 (A off, B and C on), PD_MODE_EN = 0x01, clears IO_DIPLEX2 bit 28 (channel B
pin to DAC). FM receive sets PD_CTRL bit 1 (B off); DMR and FM transmit must clear it again (BIN: the OpenDM32 DMR fix).

### 3.9 GPIO banks and the pin index scheme

Pin index 0–95: 0–31 GPIO A (PTA0–31), 32–63 GPIO B (bit = index − 32), 64–95 GPIO C (bit = index − 64). All tables
below use this index (KP, COM).

Per bank (DesignWare APB GPIO, DS/KP): DR +0x00, DDR +0x04 (1 = output), INTEN +0x30, INTMASK +0x34, INTTYPE_LEVEL
+0x38 (1 = edge), INT_POLARITY +0x3C (1 = rising/high), INTSTATUS +0x40, RAW_INTSTATUS +0x44, DEBOUNCE +0x48,
PORTA_EOI +0x4C, EXT_PORTA +0x50 (input level), INT_BOTHEDGE +0x78.

Bank interrupt vectors: GPIO B = 46, GPIO A = 47, GPIO C = 49 (DS).

### 3.10 i8080 LCD controller (0x12000000)

| Offset | Use | Tag |
|---|---|---|
| 0x00 | write: command byte to the panel (RS low) | KP |
| 0x04 | write: data byte to the panel (RS high) | KP |
| 0x10 | configuration; port writes 0x303 | KP |
| 0x28 | bit 0 enable (set), bit 1 ready (poll until 1 after enabling) | KP |

The bus pins (CS, RS, WR, DB0–DB7) are handed to the controller by clearing IO_DIPLEX2 bits 3–7 and 9–18 (port:
IO_DIPLEX2 &= 0xFFF80107). RD (PTC6) stays a GPIO held high; NRESET is GPIO 0x42. A short delay (a few cycles) after
each byte was needed in the port (KP).

---

## 4. Interrupt controller and vectors

### 4.1 PIC (0x17000000)

| Offset | Register | Use | Tag |
|---|---|---|---|
| 0x00 / 0x60 | PIC_MODE / PIC_MODE_1 | sources 0–31 / 32–63: 1 = edge, 0 = level (reset all edge) | DS |
| 0x04 / 0x64 | PIC_PO / PIC_PO_1 | polarity | DS |
| 0x08 / 0x68 | PIC_MASK / PIC_MASK_1 | 1 = masked (reset all masked) | DS |
| 0x0C | PIC_VECTOR | vector base | DS |
| 0x10 | PIC_COW1 | end of interrupt: write 0x04 at the start of every handler | DS, KP |
| 0x14–0x30, 0x6C–0x88 | PIC_PRIORn | per-source priority counters (smaller = higher) | DS |
| 0x3C / 0x8C | PIC_FFLAG_L / H | fast-interrupt select (not supported on C7000) | DS |
| 0x44 / 0x48 | PIC_INT_ST / _1 | request record, write 1 to clear | DS |

Init used (KP): MODE = 0 (level), PO = 0xFFFFFFFF (high active), MASK = 0xFFFFFFFF, the same for the _1 registers,
INT_ST = INT_ST_1 = 0xFFFFFFFF. (The port writes PIC_MASK twice and never PIC_MASK_1 at init: a typo, harmless only
because the reset value is all masked.) Enabling vector v: clear bit (v − 32) in PIC_MASK for v < 64, bit (v − 64) in
PIC_MASK_1 for v ≥ 64.

Dispatch: all 64 user vectors (32–95) point at one common entry; it saves context, writes 0x04 to PIC_COW1, takes the
vector number from PSR bits 23:16, subtracts 32 and calls through a 64-entry software table (KP).

### 4.2 Vector table

Vectors 0–31 are C-SKY exceptions: 0 reset, 1 misaligned access, 2 access error, 3 divide by zero, 4 illegal
instruction, 5 privilege violation, 6 trace, 7 breakpoint, 8 unrecoverable error, 9 soft reset, 10 normal interrupt,
11 fast interrupt, 12 hardware accelerator, 16–19 trap 0–3 (KP). User interrupts are vector = PIC source + 32 (DS
table 30), all level-triggered, high active:

| Source | Vector | Interrupt | Used by | Tag |
|---|---|---|---|---|
| 1–6 | 33–38 | TIMER_1 .. TIMER_6 | see 3.4 | DS |
| 7 / 8 / 9 | 39 / 40 / 41 | I2C1 / I2C0 / I2C2 | — | DS |
| 10 | 42 | UART1 | GPS receive | DS, KP |
| 11 | 43 | UART2 | — | DS |
| 12 | 44 | UART0 | programming cable (CPS, hotspot) | DS, KP |
| 13 | 45 | UART3 | — | DS |
| 14 | 46 | GPIO B | rotary encoder (and every other GPIO B input with its interrupt enabled, see 16) | DS, KP |
| 15 | 47 | GPIO A | — | DS |
| 16 | 48 | SPI0 | — | DS |
| 17 | 49 | GPIO C | — | DS |
| 18 / 19 / 20 / 21 | 50–53 | PWM / RTC / SDIO / USB | — | DS |
| 22 / 23 / 24 | 54–56 | SPI1 / SPI2 / LCSFC | — | DS |
| 25 / 26 | 57 / 58 | DAC / ADC | — | DS |
| 27 | 59 | PCM_rd_data_interp: decoded-audio buffer read, every 10 ms | speaker PCM refill | DS, KP |
| 28 | 60 | PCM_wr_data_interp: mic PCM buffer full | (cleared only) | DS, KP |
| 29 | 61 | System_inter (modem system interrupt) | DMR frame received etc. | DS, BIN |
| 30 | 62 | Time_slot_inter_tx (30 ms TX slot) | DMR TX burst scheduling | DS, BIN |
| 31 | 63 | Time_slot_inter_rx (30 ms RX slot) | DMR RX scheduling | DS, BIN |
| 32 | 64 | Rf_tx_inter (RF TX start, ahead of a burst) | key the FD6818 to TX | DS, BIN |
| 33 | 65 | Rf_rx_inter (RF RX start) | unused by port and OpenDM32 | DS, BIN |
| 34–36 | 66–68 | SPI slaves | — | DS |
| 37 | 69 | Codec_interp | unused | DS |

The four modem interrupts are masked as a group by PIC_MASK bits 29–31 and PIC_MASK_1 bit 0 (DS mapping; the Dec 2025
code used PIC_MASK_1 bits 29–31, which are vectors 93–95, and so masked nothing).

---

## 5. Pin map

Index = GPIO pin index (3.9). "Dir" is as configured; "Act" is the active level.

| Index | Port pin | Function | Dir | Act / notes | Tag |
|---|---|---|---|---|---|
| 0x00–0x04 | PTA0–4 | JTAG TMS, TCK, TDI, TDO, RST_N | — | IO_DIPLEX0 bits 4:0 = 0 keeps JTAG | COM, DS |
| 0x05 / 0x06 | PTA5 / 6 | ALPU-MP I2C SCL / SDA (I2C0 hardware) | — | IO_DIPLEX0 bits 5, 6 = 0 | COM, KP |
| 0x07 | PTA7 | FD6818 serial clock (bit-banged) | out | | COM, KP |
| 0x08 | PTA8 | FD6818 serial data (bit-banged, bidirectional) | in/out | | COM, KP |
| 0x09 | PTA9 | UART1 RXD ← GPS TXD | in | IO_DIPLEX0 bit 9 = 0 | COM, KP |
| 0x0F | PTA15 | LCD backlight (GPIO or PWM0) | out | high = on | COM, KP |
| 0x10 | PTA16 | Power switch monitor | in | 1 = knob turned off | COM, KP |
| 0x11 | PTA17 | Power latch | out | 1 = keep power on; 0 = power off | COM, KP |
| 0x12 | PTA18 | Data flash chip select (SPI0 CS0 as GPIO) | out | low = selected | COM, KP |
| 0x14 / 0x15 / 0x16 | PTA20 / 21 / 22 | Data flash SCLK / MOSI / MISO (SPI0 master) | — | IO_DIPLEX1 bits 8:3 = 0 | COM, DS |
| 0x17 | PTA23 | SPI1 CSN0 | — | unused | COM |
| 0x18 | PTA24 | FD6818 chip select | out | low = selected | COM, KP |
| 0x19 / 0x1A | PTA25 / 26 | RDA5807M broadcast FM receiver SDIO / SCLK | — | not used by any firmware seen | COM |
| 0x20 | PTB0 | Green LED (port) / Red LED (community map) | out | high = on; **the two sources disagree** | KP, COM |
| 0x21 | PTB1 | Red LED (port) / Green LED (community map) | out | high = on | KP, COM |
| 0x22 | PTB2 | VHF LNA / RX supply (R5_V) | out | high = on | COM, KP |
| 0x23 | PTB3 | UHF PA select | out | high = on | COM, BIN |
| 0x24 | PTB4 | UHF LNA / RX supply (R5_U) | out | high = on | COM, KP |
| 0x26 / 0x27 | PTB6 / 7 | Rotary encoder B / A | in | | COM, KP |
| 0x28 | PTB8 | Keypad backlight | out | high = on | COM, KP |
| 0x2B | PTB11 | PTT | in | low = pressed | COM, KP |
| 0x2C | PTB12 | GPS power | out | **low = GPS on**, high = off | COM, KP |
| 0x2E | PTB14 | Audio path select: mic to analog FM path vs to DMR codec | out | 1 = analog (FM TX), 0 = DMR; set at each TX start | COM, BIN |
| 0x2F | PTB15 | Speaker source switch (PWM/codec vs FD6818 audio) | out | port: 0 when audio on, 1 when muted | COM, KP |
| 0x31–0x34 | PTB17–20 | Keypad columns 1–4 | in | low = key closed | COM, KP |
| 0x35 | PTB21 | Audio power amplifier enable | out | high = on | COM, KP |
| 0x37 | PTB23 | VHF PA select | out | high = on | COM, BIN |
| 0x3A / 0x3B | PTB26 / 27 | FD6818 GPIO1 / GPIO0 | in | function unknown (configured as inputs with interrupts) | COM, KP |
| 0x42 | PTC2 | LCD NRESET | out | low = reset | COM, KP |
| 0x43–0x46 | PTC3–6 | LCD CS, RS, WR, RD | — | i8080 controller (RD as GPIO high) | COM, DS |
| 0x47–0x4B | PTC7–11 | LCD DB0–DB4, shared with keypad rows 0–4 | — | | COM, KP |
| 0x4C–0x4E | PTC12–14 | LCD DB5–DB7 | — | | COM |

### 5.1 IO mux and pull set-up at boot (KP values, field meanings from DS)

| Register (0x11000000 +) | Value | Effect |
|---|---|---|
| IO_DIPLEX0 (0x034) | 0xFFFDFD80 | JTAG and I2C0 as peripherals; PTA7/8 GPIO; UART1 RXD on PTA9; PWM0 on PTA15; rest GPIO |
| IO_DIPLEX1 (0x038) | 0xFFFFFE07, then |= 0x38000000 | SPI0 SCLK/MOSI/MISO to the SPI master; CS0, SPI1 and the modem interrupt-out pins as GPIO |
| IO_DIPLEX2 (0x03C) | 0x0FC80100 | LCD bus pins as GPIO until the display init hands them over; ADC1/ADC2 analog; DAC B/C outputs |
| IOMGR_REN_REG0 (0x040) | 0xFFFBFFEF, then &= 0xFEFFFFFF | pull-down enables (0 = pull enabled); exact per-pin meaning not mapped |
| IOMGR_REN_REG1 (0x044) | 0x00FFFFFF | |
| IOMGR_REN_REG2 (0x048) | 0x0FFFFF70, then |= 0x0C000000 | |
| GPIOA_DDR / GPIOB_DDR / GPIOC_DDR | 0x070604E0 / 0x00A0D01F / 0x00007FC4 | |
| GPIOA_DR |= 0x0060; GPIOB_DR = 0x0000C005; GPIOC_DR |= 0x7044 | | initial levels (power latch, LCD control high etc.) |

Then individually: audio path pin 0x2E = 1; rows 0x47–0x4B = 1; LCD DB6 (0x4D) = 0; power monitor 0x10 input; power
latch 0x11 = 1; flash CS 0x12 = 1; columns 0x31–0x34 inputs; PTT 0x2B input; LNA and PA selects (0x22, 0x24, 0x37,
0x23) = 0; FD6818 GP0/GP1 inputs.

---

## 6. Keys, encoder, LEDs, power

### 6.1 Keypad matrix

Rows are LCD data lines DB0–DB4 (GPIO 0x47–0x4B), columns are 0x31–0x34. To scan: save IO_DIPLEX2, set its bits
9–15 (DB0–DB6 back to GPIO), then for each row drive it low, wait a few microseconds, read the four columns (low =
pressed), drive it high again; restore IO_DIPLEX2. Rows idle high. (KP)

| Row \ Column | col 1 (0x31) | col 2 (0x32) | col 3 (0x33) | col 4 (0x34) |
|---|---|---|---|---|
| row 0 (0x47) | green (menu / OK) | 3 | 2 | 1 |
| row 1 (0x48) | left arrow | 6 | 5 | 4 |
| row 2 (0x49) | right arrow | 9 | 8 | 7 |
| row 3 (0x4A) | red (back) | # | 0 | * |
| row 4 (0x4B) | up | down | — | — |

The physical labels of the row 1 / row 2 column 1 keys and the up/down pair are from the port's naming (KP); check
against the keypad.

### 6.2 Side keys and PTT

| Key | Read | Tag |
|---|---|---|
| PTT | GPIO 0x2B low | KP, COM |
| SK1 (below PTT) | column 3 (0x33) low **with no row driven** | KP |
| SK2 | column 4 (0x34) low with no row driven | KP |

Because SK1/SK2 pull a column low on their own, a matrix scan must ignore columns that are already low before any row
is driven (KP). The radio's third programmable key (P1/P2, orange/top) has not been located.

### 6.3 Rotary encoder

A = GPIO 0x27, B = 0x26, inputs with GPIO B interrupts (vector 46). On every change of B: direction = −1 if A == B
else +1 (KP). One count per B edge.

### 6.4 LEDs and backlights

Two LEDs, red and green, active high on GPIO 0x20/0x21 (which is which: see the pin map conflict). LCD backlight GPIO
0x0F / PWM0, keypad backlight GPIO 0x28 (KP).

### 6.5 Power

Power knob: the firmware holds GPIO 0x11 high to stay on; GPIO 0x10 reads 1 when the knob is turned off, and the
firmware then saves state and drops 0x11 (KP).

---

## 7. LCD panel

240 × 320 color TFT with an ST7789-class controller on the 8-bit i8080 bus (KP; command set matches ST7789, no
datasheet in hand). RGB565, two bytes per pixel.

Reset: NRESET high, wait 120 ms, low, 120 ms, high, 120 ms. Then:

| # | Command | Parameters | Meaning |
|---|---|---|---|
| 1 | 0x11 | — | sleep out; then wait 120 ms |
| 2 | 0xB2 | 0C 0C 00 33 33 | porch control |
| 3 | 0x35 | 00 | tearing effect line on |
| 4 | 0x36 | C0 | memory data access control (MY, MX: rotated 180°) |
| 5 | 0x3A | 05 | 16 bits per pixel |
| 6 | 0xB7 | 56 | gate control |
| 7 | 0xBB | 17 | VCOM |
| 8 | 0xC0 | 2C | LCM control |
| 9 | 0xC2 | 01 | VDV/VRH from command |
| 10 | 0xC3 | 0B | VRH |
| 11 | 0xC6 | EF | frame rate control (normal mode) |
| 12 | 0xD0 | A7 | power control 1 (first, short form) |
| 13 | 0xD0 | A4 A1 | power control 1 |
| 14 | 0xD6 | A1 | (undocumented in the port) |
| 15 | 0xE0 | 70 03 08 07 08 15 30 54 47 3A 15 14 2B 2E | positive gamma |
| 16 | 0xE1 | 70 06 0A 09 06 03 2F 44 47 38 12 15 29 2F | negative gamma |
| 17 | 0x2A | 00 00 00 EF | columns 0–239 |
| 18 | 0x2B | 00 00 01 3F | rows 0–319 |
| 19 | 0x21 | — | display inversion on (the panel needs inversion for normal colors) |
| 20 | 0x2C | then 2 × 240 × 320 bytes | clear frame memory |
| 21 | 0x29 | — | display on |

Inverse video is done by toggling 0x21 / 0x20 with 10 ms around it (KP).

---

## 8. FD6818 RF transceiver

Single-chip VHF/UHF transceiver; register-compatible in spirit with the RDA1846S/AT1846S family but with its own
addresses for frequency and band (KP). **No FD6818 datasheet has been found**; all values are from the port and
OpenDM32 disassembly.

### 8.1 Bus and timing

Three-wire serial, bit-banged on GPIO: CS = 0x18 (active low), clock = 0x07, data = 0x08 (KP).

| Item | Value | Tag |
|---|---|---|
| Frame | CS low, 8-bit address, 16-bit data MSB first, CS high | KP |
| Write | address with bit 7 = 0, then data high byte, low byte | KP |
| Read | address with bit 7 = 1, release data line to input, wait 2 us, clock in 16 bits | KP |
| Bit timing | data set while clock low; 1 us low, 1 us high | KP |
| Data sampled by the FD6818 on | the rising clock edge | KP |
| Read sampling | MCU reads data while clock is high | KP |

The stock firmware never reads registers 0x65 / 0x67 (forum observation: S-meter is fake on stock). COM.

### 8.2 Register tables (address: high byte, low byte)

**Power-on init** (in order; KP; the port's DMR worked only in OpenDM32 builds, which kept this table):

| # | Reg = value | # | Reg = value | # | Reg = value |
|---|---|---|---|---|---|
| 1 | 00 = 8000 (soft reset) | 20 | B4 = 0000 | 39 | 4F = 302F |
| 2 | 00 = 0000 | 21 | 34 = 0009 | 40 | 52 = 028F |
| 3 | 37 = 1D0F | 22 | B3 = FE00 | 41 | 7B = 8420 |
| 4 | 13 = 03BE | 23 | 33 = FC00 | 42 | 09 = 006F |
| 5 | 12 = 037B | 24 | B4 = 0009 | 43 | 09 = 106B |
| 6 | 11 = 027B | 25 | 34 = 0019 | 44 | 09 = 2067 |
| 7 | 10 = 007A | 26 | 10 = 007A | 45 | 09 = 3062 |
| 8 | 14 = 0019 | 27 | 11 = 027B | 46 | 09 = 4050 |
| 9 | 49 = 2A38 | 28 | 12 = 037B | 47 | 09 = 5047 |
| 10 | 7B = 8420 | 29 | 13 = 03BE | 48 | 09 = 603A |
| 11 | 48 = 33EF | 30 | 14 = 0019 | 49 | 09 = 702C |
| 12 | 1F = 5454 | 31 | 1F = 5454 | 50 | 09 = 8041 |
| 13 | 25 = C1BA | 32 | 30 = BFF1 | 51 | 09 = 9037 |
| 14 | 77 = 88EF | 33 | 37 = 1D1F | 52 | 09 = A025 |
| 15 | 3E = A037 | 34 | 3E = A037 | 53 | 09 = B017 |
| 16 | 13 = 037E | 35 | 47 = 0100 | 54 | 09 = C0E4 |
| 17 | 7D = E35E | 36 | 48 = BBBF | 55 | 09 = D0CB |
| 18 | B3 = FF00 | 37 | 49 = 2A38 | 56 | 09 = E0B5 |
| 19 | 33 = FE00 | 38 | 4E = 4708 | 57 | 09 = F09F |

Register 0x09 is written 16 times with the top nibble as an index: a 16-entry internal table (purpose unknown; looks
like an AGC/gain table).

**Post-init** (after init, KP): 47=0000, 13=037E, 47=6042, 7E=302E, 2B=C000, 43=3028, 78=6968, 30=0000, 38=BA00,
39=00DB (144.000 MHz), 32=0244, 07=0470, 07=21CE, 51=0000, 3F=0000, 37=1F0F, 30=0000, 30=BFF1, 47=6042 (AF mute),
48=BBBF (volume).

**Enter FM** (KP): 30=0000, 13=037E, 47=6042, 7E=302E, 2B=C000, 43=3028, 07=0470, 07=21CE, 51=0000, 3F=0000,
37=1F0F, 30=0000, 30=BFF1, 47=6042, 48=BBBF.

**Enter DMR** (KP): 47=0000, 30=0000, 30=0000, 13=037E, 47=0140, 48=BBBF, 7E=B03F, 2B=0700, 43=4048, 78=6968,
30=0000, 32=0244, 37=1F1F, 30=0000, 30=BFF1, 47=0140, 3F=0000, C7=0140, 47=0140.

### 8.3 Frequency, band, state, bandwidth

| Item | Value | Tag |
|---|---|---|
| Frequency | 32-bit value in **10 Hz units**: register 0x39 = high 16 bits, 0x38 = low 16 bits (144.000 MHz = 0x00DBBA00) | KP |
| Order | mute (0x30 = 0000), band select (0x1A and 0x40), 0x38, 0x39, bandwidth (0x43), state (0x30) | KP |
| State register 0x30 | 0x0000 idle; 0xBFF1 RX on; 0xC1FE TX on. Always go through idle between RX and TX | KP, BIN |
| Bandwidth 0x43 | 0x3028 = 25 kHz FM; 0x4008 = 12.5 kHz FM; **0x4048 = DMR** (12.5 kHz with bit 6 set) | KP, BIN |
| FM TX extra writes | 0x3F = 0000, 0x40 = 34EA, 0x37 = 1D0F before switching to TX (analog only) | BIN |

Band select by frequency (f in 10 Hz units; first matching row wins; KP):

| f ≥ | 0x1A | 0x40 |
|---|---|---|
| 73 999 999 (740 MHz) | 1F80 | 3AF0 |
| 36 999 999 (370 MHz) | 2F50 | 39F0 |
| 26 000 000 (260 MHz) | 9F30 | 385C |
| 18 499 999 (185 MHz) | 3F48 | 38F0 |
| 12 399 999 (124 MHz) | AF28 | 375C |
| 9 299 999 (93 MHz) | 4F44 | 37F0 |
| 6 199 999 (62 MHz) | BF24 | 365C |
| 4 699 999 (47 MHz) | 5F42 | 36F0 |
| 3 099 999 (31 MHz) | CF22 | 355C |
| 2 399 999 (24 MHz) | 6F41 | 35F0 |
| 1 599 999 (16 MHz) | DF21 | 345C |

Below 16 MHz nothing is written (KP).

### 8.4 RSSI, noise, sub-audio

| Item | Register | Use | Tag |
|---|---|---|---|
| RSSI | 0x67 bits 8:0 | signal = value >> 1 (scale to dBm not calibrated) | KP, COM |
| Noise | 0x65 low byte | | KP |
| CTCSS mode | 0x51 | 0x9045 TX CTCSS on with gain 0x45; 0x1000 CTCSS RX / TX off; 0x0000 sub-audio off | KP |
| CTCSS frequency | 0x07 | intended value = Hz × 20.64888 (e.g. 88.5 Hz → 1827). The port computes it with an integer division that truncates the tone to whole tens of Hz: **wrong** | KP |
| CTCSS detect thresholds | 0x52 = 028F | | KP |
| Tone generator 1 | 0x71 | tone in Hz × 10 (1750 Hz burst) | KP |
| CSS status | 0x0C high byte | bit 2 CTCSS detected; bit 7 DCS (normal) detected; bit 6 DCS (inverted) detected | KP |
| DCS | — | no FD6818 DCS registers known; every DCS write in the port is commented out | KP |

Note: in this radio the C7000 does the FM modulation (two-point, `modem-hrc7000.md`), and the C7000 has its own CTCSS
and CDCSS generators and detectors. Whether sub-audio set in the FD6818 reaches the air in TX is untested.

### 8.5 TX/RX switching, PA and LNA

| Action | Steps | Tag |
|---|---|---|
| Enter RX | DAC B = 0; PA selects 0x23 and 0x37 low; LNA of the RX band on (UHF: 0x24 high, 0x22 low; VHF: 0x22 high, 0x24 low); FD6818 0x43 = bandwidth; 0x30 = BFF1 | KP, BIN |
| Enter FM TX | both LNAs off; FD6818 0x3F, 0x40, 0x37 as above; C7000 FM TX mode; audio path 0x2E = 1; 0x30 = 0000, 0x43 = bandwidth, 0x30 = C1FE; then PA select of the TX band high; DAC B = power | KP, BIN |
| Enter DMR TX | both LNAs off; C7000 DMR mode; audio path 0x2E = 0; FD6818 0x43 = 4048, 0x30 = 0000, 0x30 = C1FE. **PA select and DAC B are not set here** | BIN |
| Each DMR burst | at the modem RF TX interrupt: FD6818 0x30 = 0000 then C1FE. A one-shot timer started when the burst is armed (in the TX timeslot interrupt) fires after the DMR PA delay and then raises the band's PA select and writes DAC B (section 8.6) | BIN |
| After each DMR burst | DAC B = 0; both PA selects low; FD6818 0x43 = 4048 (OpenDM32) or the RX bandwidth (port), 0x30 = 0000, 0x30 = BFF1 | BIN, KP |

Band for PA/LNA purposes: VHF below 280 MHz, UHF from 280 MHz (the 220 MHz range shares the VHF path) (KP).

### 8.6 DMR PA timer

| Item | Value | Tag |
|---|---|---|
| What | one-shot hardware timer, started each time a DMR TX burst is armed (TXRX_CTRL = 0x80 in the TX timeslot interrupt) | BIN |
| Delay | user setting "DMR PA", 1–35 ms, default **28 ms** (OpenDM32 settings default and menu limit) | BIN |
| On expiry | read EOI, stop the timer, if still transmitting DMR: PA select of the TX band high, DAC B = TX drive | BIN |
| Timer | stock / OpenDM32: Timer 3 (vector 35); port: Timer 4 (vector 36) because Timer 3 polls the ADC | BIN, KP |
| Why | keying the PA at a fixed delay after the slot interrupt kept low-power bursts inside their timeslot; keying at the RF TX interrupt did not (forum report "TS bad at 1 W", fixed in the Aug 2026 build) | BIN, COM |

The PA is switched off from the next timeslot interrupt once the burst has run at least 20 ms (KP; the RF RX interrupt
is not used).

---

## 9. Audio path

| Element | Detail | Tag |
|---|---|---|
| Mic | into the C7000 codec ADC (MIC input 2: CR_MIC2 bit 6 = 1) | KP |
| Mic routing switch | GPIO 0x2E: 1 = analog FM path, 0 = DMR path | BIN |
| Speaker | codec DAC line out 2 (LINEOUT_CTRL bit 0, `line2out_en`) → speaker source switch GPIO 0x2F → audio PA (enable GPIO 0x35) | KP, DS |
| Speaker on | 0x2F = 0, 0x35 = 1; off: 0x2F = 1, 0x35 = 0 | KP |
| FM RX audio | demodulated by the C7000 FM block and played through the codec DAC when AUDIO_CONTROL bit 6 (`fm_play_ctrl`) is set | DS, KP |
| DMR RX audio | CPU writes 160 bytes of decoded 8 kHz PCM to 0x180000A0 per 10 ms and sets AUDIO_CONTROL bit 5; PCM_rd interrupt (vector 59) asks for the next block; clear with INTERP_CLEAR bit 5 | DS, KP |
| Beeps / prompts | 32 bytes (16 samples, high byte first, 8 kHz) at a time to the prompt FIFO 0x16000980 while AUDIO_CONTROL bit 31 (`ring_full_two`) is 0; enable with AUDIO_CONTROL bit 7 (`ring_play_ctrl`) | DS, KP |

### 9.1 Internal audio codec registers (byte registers at 0x160009C0 + offset)

Values written at init by the port (KP); field meanings from DS:

| Offset | Register | Value | Meaning |
|---|---|---|---|
| — | SYS_SOFT_RSTN bit 4 | cleared, wait 2 us | codec soft reset pulse (then released by the later 0x190 write) |
| 0x08 | AICR_DAC | 0xC0 | 24-bit, master, interface active, parallel |
| 0x09 | AICR_ADC | 0xC0 | same for ADC |
| 0x0B | CR_MIC1 | 0x00 | single ended, bias on, MIC1 on, bias 2.08 V |
| 0x0C | CR_MIC2 | 0x70 | input pair 2 (AIPL2/AINL2) |
| 0x0D | CR_DAC | 0x20 | soft mute off, DAC on; bit 7 = 1 mutes (line out off) |
| 0x0F | CR_ADC | 0x20 | soft mute off, left only, ADC on; bit 7 = 1 mutes the mic |
| 0x12 | CR_VIC | 0x0C | codec active (not sleep, not powered down) |
| 0x13 | CR_CK | 0x40 | MCLK / 2, 24 MHz |
| 0x1F | GCR_DACL | 0x87 | left shared, −7 dB (bits 5:0: 0..31 = 0..−31 dB, 63..32 = +1..+32 dB) |
| 0x23 | GCR_MIC1 | 0x00 | 0 dB |
| 0x25 | GCR_ADCL | 0x8B | shared, +11 dB (bits 5:0 = 0..42 dB). Mic gain settings for FM and DMR are written here with bit 7 set |
| 0x2B | CR_DAC_AGC | 0x00 | DAC AGC off |
| 0x2F | CR_ADC_AGC | 0x00 | ADC AGC off |
| 0x30 | DR_ADC_AGC | 0x34 | AGC data byte |
| modem 0x088 | LINEOUT_CTRL | 0x01 | line out 2 enabled |

---

## 10. Data flash (16 MB)

| Item | Value | Tag |
|---|---|---|
| Chip | GigaDevice GD25Q128 class, 16 MB; later radios use Puya PY25Q128HA (OpenDM32 added support for it in Sep 2026) | COM, BIN |
| Bus | SPI master 0 (0x140A0000), CS on GPIO 0x12 (manual, low active) plus SER | KP, BIN |
| JEDEC ID (0x9F) | GD25Q128: C8 40 18; Puya: 85 20 18 (INF from the release name "addid_2018"). The port accepts only part ID 0x4018, so a Puya radio fails the check | INF, KP |
| Read | 0x03 + 24-bit address (OpenDM32 uses the same) | BIN |
| Fast read | 0x0B (not used) | — |
| Page program | 0x06 (write enable), then 0x02 + address + up to 256 bytes within one page | KP |
| Sector erase 4 KB | 0x06, then 0x20 + address | KP |
| Block erase | 0x52 (32 KB), 0xD8 (64 KB); chip erase 0xC7 / 0x60 | datasheet convention |
| Status | 0x05 SR1 (bit 0 busy/WIP, bit 1 WEL), 0x35 SR2, 0x15 SR3; writes 0x01 / 0x31 / 0x11 | KP |
| Security registers | 0x48 + 24-bit address + dummy byte, three 256-byte registers at 0x1000, 0x2000, 0x3000. **No DM-32 data is known to be stored there** (the port's reader is a stub) | KP |
| Unique ID | 0x4B | KP |
| Power down / release | 0xB9 / 0xAB | KP |
| Busy timeouts | page program ≤ 3 ms (port waits 10 ms); 4 KB erase about 400 ms worst (port waits 600 ms, polling every 100 us). Stock-style driver polls up to 7000 × 1 ms | KP |
| Start-up | 160 us after raising CS before the first command | KP |

### 10.1 Data flash map

| Range | Content | Rule | Tag |
|---|---|---|---|
| 0x000000 – 0x000FFF | stock | never write | BIN |
| 0x001000 – 0x0C8FFF | stock codeplug: 200 × 4 KB pages with a small translation layer; the last byte of each page (+0xFFF) is its logical block ID; unallocated IDs point at the sentinel 0xFFF001. **Factory calibration is the page whose ID is 2** | never write | BIN |
| 0x0C9000 – 0x149FFF | stock | never write | BIN |
| 0x150000 – 0x175FFF | stock "boot image" | never write | BIN |
| 0x180000 – 0x27EFFF | stock (several regions) | never write | BIN |
| 0x278000 – 0x6DBFFF | stock contact database (50 000 entries) | never write | BIN |
| 0x6DC000 – 0xFFFFFF | stock, purpose undocumented: most likely voice recordings on 049 and the extended contact DB on L01 firmware | stock may overwrite anything here | BIN, INF |
| 0x700000 – 0xFFFFFF | used by OpenDM32 for its own data; usable by new firmware | see caveat in the row above | BIN |
| 0x710000 (0x175 bytes) | OpenDM32's local copy of the decoded calibration (header 00 40 00 47 FF FF FF FF) | read only if no factory page is found | BIN |

The stock CPS protocol on UART0 (115200) is a raw 24-bit window on this flash: `52 addr3 len2` reads, `57 addr3 00 10
<4096 bytes>` writes, entered with `PSEARCH` / `PASSSTA` / `SYSINFO` and `FF FF FF FF 0C "PROGRAM"` (BIN, COM).

---

## 11. Factory calibration

### 11.1 Location

Scan 4 KB pages 0x1000, 0x2000, … 0xC8000; the first whose byte at +0xFFF equals **2** is the calibration page
(BIN, both OpenDM32 and the stock codeplug ID scheme). It is never written.

Read three pieces into a 0x175-byte working image:

| From page offset | Length | To image offset |
|---|---|---|
| +0x000 | 0x108 | 0x000 |
| +0x200 | 0x015 | 0x108 |
| +0x300 | 0x050 | 0x11D |

### 11.2 Fields (offsets in the working image)

| Offset | Size | Field | Tag |
|---|---|---|---|
| 0x000 | 8 | header `00 40 00 47 FF FF FF FF` (looks like BCD 400 / 470 MHz band limits) | BIN, INF |
| 0x016 | 1 | **TX center** (I/Q bias): a signed offset around 128 (stock menu edits value XOR 0x80). 128 if absent | BIN |
| 0x017 – 0x01B | 5 | UHF modulation points (with frequency table 0) | BIN |
| 0x040 / 0x045 / 0x04A | 3 × 5 | UHF PA drive low / mid / high, 5 points each, value × 16 = DAC | BIN |
| 0x0A6 – 0x0AA | 5 | VHF modulation points (with frequency table 2) | BIN |
| 0x0D0 / 0x0D5 / 0x0DA | 3 × 5 | VHF PA drive low / mid / high, × 16 | BIN |
| 0x108 | 0x15 | from page +0x200, unused | BIN |
| 0x11D | 4 × 5 × 4 | four frequency tables of five 32-bit little-endian entries, **8 BCD digits in flash**, frequency in 10 Hz units after conversion. Table 0 (0x11D) UHF TX, 1 (0x131), 2 (0x145) VHF TX, 3 (0x159) | BIN |
| 0x16D | 4 | computed K_VHF = (Σ over the 5 points of (freq_MHz × VHF modulation point)) / 5, freq from table 2, freq_MHz = 10 Hz units / 100000 (integer) | BIN |
| 0x171 | 4 | computed K_UHF, same with table 0 and the UHF points | BIN |

### 11.3 How the values are used

| Register / output | Formula | Tag |
|---|---|---|
| SIG_CENTER (modem 0x108) | (cal << 24) or (cal << 16): TX I and Q center = cal | BIN |
| RF_MOD_BIAS_CTRL (modem 0x114) | ((cal + 384) << 16) or modcal, where modcal = b × 0x0101 and b = round(K / f_MHz), K = K_UHF if f > 299.99999 MHz else K_VHF. Before a frequency is known, modcal = 0x4D4D. Recompute when the TX frequency changes; written at init and every DMR entry | BIN |
| PA drive points for a TX frequency f | VHF table base 136 MHz, step 9.5 MHz (136, 145.5, 155, 164.5, 174); UHF base 400 MHz, step 17.5 MHz (400, 417.5, 435, 452.5, 470). idx = (f − base) / step capped at 4, linear interpolation to the next point; at idx 4 extrapolate with 2·v4 − v3; clamp 0..4096. f < 136 MHz: VHF point 0, no interpolation. UHF is f > 280 MHz | BIN |
| Power levels | low L, mid M, high H from above. Level 0 = 0.90·L; 1 = L; 2, 3 = L + (M − L)·f1, f2; 4 = M; 5, 6, 7 = M + (H − M)·f3, f4, f5; 8 = H; 9 = user value; clamp 4095 | BIN |
| Fractions f0..f5 | VHF 0.90, 0.41, 0.76, 0.34, 0.60, 0.81; 220 MHz 0.90, 0.42, 0.75, 0.31, 0.55, 0.78; UHF 0.90, 0.43, 0.74, 0.27, 0.51, 0.75 | BIN |
| Fallback without calibration | DAC B = 1408 for every level; SIG_CENTER = 0x6D6C0000, RF_MOD_BIAS_CTRL = 0x01F0FFFF (dh4570 constants) | KP |

OpenDM32 bug noted: for a UHF frequency below 400 MHz its unsigned subtraction gives idx 4 (BIN). Clamp to 0.

---

## 12. GPS

| Item | Value | Tag |
|---|---|---|
| Module | ATGM336H (NMEA, GPS + BeiDou) | COM |
| Data | module TXD → UART1 RXD (PTA9), 9600 8N1, NMEA sentences; MCU→module TX not wired (INF: not in the map) | COM, KP |
| Power | GPIO 0x2C: low = on, high = off | KP |
| Interrupt | UART1, vector 42, IER = 0x05 | KP |

---

## 13. Firmware file format and update paths

### 13.1 Baofeng firmware file (`BFUV32-V2`)

| Offset | Content | Tag |
|---|---|---|
| 0x000 | ASCII `BFUV32-V2` (9 bytes) | BIN |
| 0x009 – 0x0FC | 0xFF padding | BIN |
| 0x0FD | ASCII `END` | BIN |
| 0x100 | raw application image, linked for 0x0300C000; no checksum, no length field | BIN |

Stock 049 unpacks and repacks byte for byte with only these rules (BIN).

### 13.2 Baofeng bootloader (update mode)

Enter by holding **PTT + SK1** while turning the radio on; the green LED lights (COM). Protocol on the programming
cable, 115200 8N1 (COM, from a community loader that works with OpenDM32):

| Step | Host sends | Radio answers |
|---|---|---|
| 1 | 0x52 | 0x06 |
| 2 | 0x4D 00 00 00 01 | 0x4D 01 09 |
| 3 | 0x06 | `BFUV32-V2` |
| 4 | 0x45 00 00 00 00 | 0x06 |
| 5 | 0x31 | 0x43 |
| 6 | 0x01 | 0x43 |
| 7 | 0x00 0xFF, then a 128-byte block: name, 0x00, decimal size as ASCII, zero padded; then CRC-16/XMODEM of the block, big endian. The radio erases the app area (up to about 15 s) | 0x06 0x43 |
| 8 | per 1 KB block: 0x02, wait 0x43; block number n, 0xFF − n (n starts at 1, wraps 0xFF → 0x00); 1024 bytes (last block zero padded); CRC-16/XMODEM big endian | 0x06 |
| 9 | 0x04 | (reboots) |

The image sent is the raw application (the 256-byte header is stripped). The file name and size are not used. The
stock CPS also sends seven 130-byte zero blocks before rebooting; they are not needed (COM).

If the radio does not boot the new image, remove and refit the battery; update mode still works (COM).

### 13.3 ROM serial boot (recovery, no bootloader needed)

The HR_C7000 ROM listens on the serial port after reset (DS). Dahua's DH_SOC_TOOLS (Windows) with a script first loads
a "miniBoot" image to RAM 0x00014000 and starts it, then writes the application to flash (dh4570 used 0x03001000)
(KP). Works with FTDI-based cables only; CH340 cables, which work with the Baofeng bootloader and CPS, do not reach the
ROM loader (COM). The ROM protocol itself is not documented here. Production radios are programmed over JTAG (COM).

### 13.4 Backing up

Back up the program flash (1 MB) and the data flash (16 MB) before flashing anything; community scripts read both
through the bootloader/ROM path (COM).

---

## 14. Other chips

| Chip | Where | Notes | Tag |
|---|---|---|---|
| ALPU-MP | I2C0 on PTA5/PTA6 | AES-128 copy-protection authenticator (datasheet ALPU-MP v2.16). Stock may check it; a new firmware does not need it | COM, DS |
| RDA5807M | PTA25/26 | broadcast FM receiver; no firmware seen uses it | COM |

---

## 15. AMBE codec placement (summary)

The codec is not in this repository. Two working placements are known (details in `codec-dm32.md`):

| Variant | Code | Tables | Entry points | Tag |
|---|---|---|---|---|
| dh4570 pair | XIP at 0x03061000 (0x12B21 bytes) | copied to 0x18000800 (0x7803 bytes) | +0x0 decode, +0x260 encode, +0x1E34 ECC | KP |
| OpenDM32 | copied to RAM 0x47900–0x57FD8 | copied to 0x18000800–0x180045A0 | 0x47900, 0x47B14, 0x4953C; first word at 0x47900 = 0x152514D6 when present | BIN |

---

## 16. Known wrong or unfinished in the source tree

These are traps in the old port, listed so a fresh implementation does not copy them.

| # | Problem | Effect |
|---|---|---|
| 1 | The HR-C6000 "SPI0 page register" access layer is kept as empty stubs. Every C7000 CTCSS / DCS / DTMF / RX gain call that still goes through it (RX CTCSS, RX and TX DCS code, DTMF mode/deviation save/restore, DMR RX gain) is a **no-op** | C7000 sub-audio receive, DCS and DMR volume do nothing |
| 2 | DCS never finished: TX DCS sets FM_MOD bits but writes the code to the stub instead of CDCSS_ADDR; RX DCS writes are commented out (FD6818 side too). OpenDM32 Aug 2026 added DCS TX ("Check DCS TX") but it was not ported | No DCS |
| 3 | FD6818 CTCSS tone value truncates the tone to whole tens of Hz before scaling | Wrong CTCSS frequency |
| 4 | Volume pot level variable is never written (ADC channel not read) | No volume control from the knob |
| 5 | Mic/VOX ADC read uses a channel set-up that single-scan mode cannot deliver | VOX and mic level meter wrong |
| 6 | Timer naming clash: the SDK calls the timer at 0x14000000 "Timer 0", the port's header and the manual call it "Timer 1"; both "VECTOR_TIMER0" and "VECTOR_TIMER1" are 33. The RTOS tick is hooked on vector 33 but that timer is never configured, and a 10 ms-tick assumption (delay = (ms + 9) / 10 ticks) sits next to a 1000 Hz tick setting | RTOS timing source unclear; check on hardware |
| 7 | The port's DMR PA one-shot is Timer 4 because Timer 3 polls the ADC; stock uses Timer 3 | Fine if consistent; do not mix |
| 8 | UART1 write sends to UART3's THR | No GPS commands could be sent (none are needed) |
| 9 | Every GPIO input set-up also enables its pin interrupt; only GPIO B (encoder) has a handler, which never acknowledges the interrupt (PORTA_EOI), and PTT, keypad columns and FD6818 GP0/GP1 are on GPIO B with level triggering | Possible interrupt storm on GPIO B |
| 10 | PIC init writes PIC_MASK twice and never PIC_MASK_1 | Harmless only because of the reset value |
| 11 | Data flash accepts only JEDEC part 0x4018 | Puya-flash radios fail flash init |
| 12 | Calibration menu still edits the MD-UV380 table, which the DM-32 does not use | |
| 13 | SYS_SOFT_RSTN values 0x180 / 0x190 hold most modem blocks in reset if the manual's "low active" is right, yet FM worked | Bit meanings need checking on hardware |
| 14 | LED color of GPIO 0x20 / 0x21 disagrees between the port and the community map | Check on hardware |
| 15 | PWM backlight duty only spans half the range (FP 0..100 with PER 200) | Check on hardware |
