<!-- SPDX-License-Identifier: MIT -->
# Radioddity DB25-D PRO hardware

Status: research, 2026-10-12. The facts a fresh firmware needs to bring up the DB25-D PRO mobile: SoC, memory, pins,
the two RF chips, display, storage, calibration, the PC link and the update path. The survey entry, with sources for
the model and FCC facts, is in [radios.md](radios.md). The HR_C7000 itself (UARTs, timers, ADC, DAC, PIC, GPIO) is in
[hardware-dm32.md](hardware-dm32.md) and its modem in [modem-hrc7000.md](modem-hrc7000.md); this page only says
where the DB25-D differs.

Facts tagged **RADIO** were checked on the owner's DB25-D PRO (stock application 909E.D6x.EARSAB.020) on 2026-10-16;
the rest still come from the stock images and documents.

## Confidence tags

| Tag | Meaning |
|---|---|
| **BIN** | Observed in the stock application images by disassembly (facts only: addresses, register values, pin numbers, strings; no code carried over). Six images, 2022-10 to 2025-01 |
| **FCC** | FCC internal photos of the DB25-D PRO (2AN62-DB25DPRO) |
| **COM** | Radioddity manuals, release notes and update guide |
| **DS** | A chip data sheet (the NAND: Kioxia TC58CVG1S3HRAIJ, 2019-10-01) |
| **DM32** | The same fact as on the DM-32UV ([hardware-dm32.md](hardware-dm32.md)) |
| **INF** | Inferred by us; not seen directly |
| **RADIO** | Seen on the owner's radio (program flash read through the stub, the loader's replies on the cable) |

---

## 1. Models and batches

| Item | Finding | Tag |
|---|---|---|
| SoC | One HR_C7000 (CK803S core), marked "HR_C7000 D56323MP1" | FCC |
| Boards (PRO) | Control "CDR300UV-MCU-BT-AI-TDA-1.0", display "C7DR300UV-TFT-51M-FD-2.0", RX "C7DR300UV-RX-BT-AD-3.0", TX "C7DR300UV-TX-RD35-BT-AD-3.5" | FCC |
| One image for every board | The firmware identifies both RF chips at boot (section 4.2) and writes two letters after the "D" of its version string | BIN |
| Batch codes | D64, D66, D6T: FD6818 main transceiver (2021-2023). **DBB: two BK4819, the PRO** | BIN, COM |
| Minimum stock firmware on a PRO | The 2024-05-09 build is the first that knows the BK4819. Older stock images must never be loaded on a PRO | BIN |

## 2. Memory and image

| Item | Finding | Tag |
|---|---|---|
| Program flash | XIP at 0x03000000. Boya, 2 MB (JEDEC ID 68 40 15), 4 KB sectors. QE (status register 2 bit 1) is set: the stock application sets it before it switches the controller to quad mode | RADIO, BIN |
| Loader | 0x03000000-0x0300BFFF (48 KB, 36 KB used), the vendor IAP loader. It's never in an update package | RADIO |
| Application | Linked at **0x0300C000**; the loader jumps to its first byte. Raw image: no header, no checksum, no encryption. On a normal power-on the loader starts it only if its first 4 bytes are **2C 11 01 C0** (the stock entry, `lrw r1, ...; mtcr r1, psr`), else it stays in update mode; right after an update it starts it without looking | BIN, RADIO |
| ODR's layout | The first stage (boot/db25d, [boot/README.md](../../boot/README.md)) in the 4 KB sector at 0x0300C000, carrying that signature; the application from 0x0300D000 | RADIO |
| Clocks | The loader leaves the PLL at 72 MHz (24 MHz × 24 / 2 / 4), APB 36 MHz, on both paths into the application. The stock application never writes the clock manager (no store to CLK_MGR 0x004, 0x010, 0x024 or 0x028 in it), so it runs on those. ODR re-programs the PLL to the DM-32's 168 MHz CPU / 84 MHz APB (section 2.1) | RADIO, BIN |
| Start-up state | PSR 0x80000100, SP 0x50000, VBR 0x10000, cache set as on the DM-32 | BIN, DM32 |
| SRAM | 0x10000-0x57FFF as on the DM-32. The stock image uses 0x10000-0x525E0 | BIN, DM32 |
| Modem SRAM | The stock image copies its modem and vocoder tables to 0x18000800-0x18004F90 | BIN |

### 2.1 Clocks in ODR

| Item | Setting | Tag |
|---|---|---|
| Switch | `DB25D_FULL_CLOCK=1` (default, `board.mk`): CPU / AHB 168 MHz, APB 84 MHz as on the DM-32 (hardware-dm32.md 1.1). `DB25D_FULL_CLOCK=0`: no PLL change, 72 / 36 MHz as the loader leaves them. Both builds define `HRC_LOADER_CPU_HZ` = 72 MHz, which the stub hand-over uses | RADIO |
| Limits | The user guide (4.2.2.2) gives CPU and AHB up to 192 MHz, APB up to 96 MHz (typical 120 / 60). 168 / 84 is inside them. The NAND takes 133 MHz, the ST7735S writes at up to 15 MHz | DS |
| Start-up | The loader runs the CPU from the BPLL, which must not be reprogrammed under it. `hrc_clock_init` (RAM code, interrupts off, before the vector table is set up) saves CLK_MGR 0x010 / 0x024 / 0x028, sets the SoC clock select (CLK_MGR_CTRL bit 3) to the oscillator, clears the BPLL configure bit (bit 1), writes the 24 MHz / 1 × 28 / 2 / 2 word, USB / SDIO dividers and AHB:APB = 1:2 as the DM-32 does, sets bit 1 again, waits 100 us for the lock flag, then clears bit 3. The APLL (modem clock, bit 0) is left alone. If the PLL never locks within 2 s, the saved words go back. The XIP flash is switched to quad mode at divider 2 before, as on the DM-32; its SCLK follows the AHB clock | RADIO, DS |
| Timers, UART, SSI | All derive from `HRC_APB_HZ` at build time: timer 2 (microseconds), the tick, the one-shots, UART divisor 46 for 115200 (0.9 % slow; 20 at 36 MHz is 2.3 % slow), NAND SPI0 divider 4 (21 MHz, 9 MHz at 36), display SPI2 divider 6 (14 MHz, 6 MHz at 36; stock divider 2 is 18 MHz) | RADIO |
| Bit-banged display | Each phase of the clock line is written `hrc_bitbang_repeat(APB)` times (3 at 84 MHz, 2 at 36), so the phases stay over about 30 ns. Used only when the ADC 2 strap reads 50 or more (3.1) | RADIO |
| Not clock dependent | The BK4819 3-wire bus, key scan, APC ramp and TX settling use `hal_delay_us` (timer 2). ADC conversions run from the 24 MHz oscillator (user guide clock tree: oscillator / 10); the ADC and UART wait loops are counted, and are shorter in wall time at 168 MHz, still several ms | DS, RADIO |
| Modem | The modem and codec clocks come from the APLL and the oscillator, not the BPLL (user guide clock tree), and `hrc_clock_init` doesn't touch them; the DMR code's assumptions are the DM-32's, which are 168 / 84 | DS |
| Hand-over | `enter_stub` and `hrc_soft_reset` (the REBOOT path) call `hrc_clock_restore_loader()` first: SoC clock to the oscillator, the saved CLK_MGR words back, lock, SoC clock to the PLL, so the loader (which finds the clock manager as it left it, a soft reset only resets the CPU side) and the runtime stub see the clocks they expect (the universal stub measures timer 2 anyway and gets the rates in CONFIG, 7; it measured 35.6 MHz). Before it the UART drains; after it the divisor is set for 36 MHz (20). A power cycle resets the clock manager (hard reset), and the first stage (boot/db25d) runs before any ODR code, so the power-on stage catch always sees the loader's clocks even if ODR hangs at 168 MHz | RADIO |

Check the clock on a radio: the debug link's `uptime_ms` has to advance one second per second (timer 2 counts APB cycles, so a
wrong APB shows at once) and the host link has to answer at 115200 (the UART divisor assumes 84 MHz).

## 3. Pin map

GPIO index as on the DM-32: 0-31 PTA, 32-63 PTB, 64-95 PTC (DM32). Levels are "high = on" unless stated.

### 3.1 Buses

| Bus | Pins | Protocol | Tag |
|---|---|---|---|
| Display | PTC2 (66) reset, PTC3 (67) CS, PTC4 (68) SCL, PTC6 (70) D/C (low = command); data on PTC7 (71) or PTC8 (72) | 4-wire serial, 8 bits MSB first, data set while SCL is low and taken on the rising edge. ADC channel 2 is a board strap. Reading ≥ 50: bit-banged GPIO, data on PTC7. Below 50: the SPI2 controller (0x140E0000), SCLK on PTC4 (IO_DIPLEX2 [6:5] = 1) and MOSI on **PTC8** (IO_DIPLEX2 [12:11] = 1); CS, D/C and reset stay GPIO. Stock SPI2: CTRLR0 7, BAUDR 2, one byte per transfer, polled. The owner's radio reads about 2 (SPI2) | BIN, DS, RADIO |
| SPI NAND | SPI0 (0x140A0000), CS PTA18 (18) by GPIO | Mode 0, 8-bit frames, BAUDR 4 (21 MHz) | BIN |
| RF chips | SCLK PTB16 (48), SDIO PTB24 (56) bidirectional, SEN main PTB25 (57), SEN LO PTC20 (84) | 3-wire, as the DM-32's FD6818: 8-bit address (bit 7 = read), 16 data bits, MSB first. One lock around every access | BIN |
| RF chip lines | PTB27 (59) input with interrupt; PTB26 (58) input | ? (interrupt / sub-audio detect, status) | BIN, INF |
| FM broadcast (RDA5802N) | Bit-banged I2C, SCL PTA7 (7), SDA PTA8 (8), 7-bit address 0x11 | | BIN, FCC |
| RTC | SoC I2C2, address 0x70 | | BIN, DM32 |
| PC link | **UART1** (0x14040000), RXD PTA9 (9), TXD PTA10 (10), 115200 8N1. The stock debug output goes to the same port. ODR's host port, where a CPU fault's trace lines appear ([crash-log.md](crash-log.md) 2.3) | | BIN, COM |
| GPS | UART2 (0x14050000, interrupt source 11), 9600 8N1 receive only; RXD pad PTA11 (IO_DIPLEX0 bit 11 = 0, HR_C7000 user guide 4.4.4.4; the stock firmware's own mux write hasn't been traced); power PTB17 (49), **high = on**, confirmed on the radio 2026-10-17 (the stock firmware clears it at GPS init and power-off and sets it when GPS is on; ODR's single constant is `GPS_POWER_ON_LEVEL` in `boards/db25d/pins.h`). Module: the green daughter board (CC1157G) | | BIN, DS, RADIO |

The DM-32's programming cable is on UART0. The DB25-D application never uses UART0 (BIN).

### 3.2 RF switching and the PA

| Pin | Use | Tag |
|---|---|---|
| PTC19 (83) | RX front end, on in both RX paths | BIN |
| PTB4 (36) / PTA25 (25) | RX path VHF / UHF | BIN |
| PTB3 (35) / PTA26 (26) | TX path (PA select) VHF / UHF | BIN |
| PTA23 (23) | High in RX, low in TX | BIN |
| PTB2 (34), PTA27 (27) | Low in RX. TX supply, **one pin per band**, raised 200 us into TX: UHF PTA27 high with PTB2 driven low, VHF PTB2 high with PTA27 driven low | BIN |
| DAC A | RX front-end tuning: the calibration value for the RX frequency, written at each receive set-up (6.4) | BIN |
| DAC B | PA power control. Ramped over 180 steps of 4 us at the start and end of each burst | BIN |
| DAC C | 1762 (0x06E2) at init, as on the DM-32; 1780 at each receive set-up | BIN, DM32 |
| DAC init | IO_DIPLEX2 bits 27–29 cleared (all three outputs on their pins), PD_MODE_EN = 0, PD_CTRL = 0 (all channels on), every data register 0, then DAC C | BIN |

To TX: RX paths off, 50 us, tune, the page's TX-on delay, PA select of the band (UHF PTA26 high and PTB3 low, VHF the
reverse) with PTA23 low, 200 us, the band's TX supply, then the PA ramp up (4.4). To RX: the main chip to its RX state, the
page's TX-off delay, the ramp down, both supplies and both PA selects low, PTA23 high, 300 us, the retune, the RX path of
the band, 200 us (BIN).

Transmitter safe state (`board_rf_safe`, [crash-log.md](crash-log.md) 8), at every fault, reset and stub entry and first
in `main()`: TX supplies PTA27 and PTB2 low, PA selects PTA26 and PTB3 low, DAC B 0 and powered down, PTA23 high; on
the fault and reset paths also the modem's FM_PTT and LAYER2_TXRX_CTRL 0. The first stage (`boot/db25d`) drives the
same pins (not the DAC) right after the power latch. The BK4819s keep their state until the next start. Found on the
radio (2026-10-21): a fault while transmitting, then the soft reset, left the PA keyed at about 15 W of supply draw with
the radio stopped before the display was lit, until a power cycle.

### 3.3 Keys, encoder, power, LEDs, audio

| Pin | Use | Tag |
|---|---|---|
| PTB18, PTB20, PTB21, PTB22, PTB23 (50, 52-55) | Key matrix drive lines, idle high, driven low one at a time | BIN |
| PTB9, PTB10, PTB11 (41-43) | Key matrix sense lines, low = closed. PTB9 and PTB10 also carry one key each wired straight to ground (low with every drive line high) | BIN |
| PTA9 (9) | Also read as a GPIO by the key code: the mic connector's line that carries PTT and the radio's RXD. ODR counts a low PTA9 as PTT only 50 ms or more after the last good UART byte (`drivers/input/sharedptt`, [gotchas](gotchas.md) 16.2), then on three scans in a row; the diagnostics line `mic` ends with `ign N`, the low scans ignored. The three-scan filter alone let debug link polling key the radio; with the gate, 30 s of polling gave `ign 5` and no PTT press (2026-10-19, RADIO) | BIN, COM |
| PTC11 (75) | Hand mic PTT: high at idle, low while held. The mic's digit and P keys change neither PTA9 nor PTC11 at a 10 ms scan; how they reach the radio is open | BIN, RADIO |
| PTB5 / PTB8 (37 / 40) | Rotary encoder A / B, GPIO B interrupt | BIN |
| PTB12 (44) | Power key sense, low = pressed | BIN |
| PTB13 (45) | Power latch: set at start-up, cleared to switch off | BIN |
| PTA16 (16) | LCD backlight: PWM1 output (IO_DIPLEX0 bit 18 clear) | BIN, DS |
| PTA17 (17) | Unused, set high at init | BIN |
| PTB0, PTB1 (32, 33) | Status LEDs, high = on: PTB0 green, PTB1 red | BIN, RADIO |
| PTB14, PTB15 (46, 47) | Audio path selects: audio on = PTB14 high, PTB15 low; off = PTB14 low, PTB15 high | BIN |
| PTA15, PTA24 (15, 24) | Speaker enables, low = on: PTA24 the body speaker, PTA15 the mic speaker. Both high when audio is off. IO_DIPLEX1 bits 11 and 12 set (GPIO). **PTA15 low also darkens the LCD backlight** on the owner's radio, so ODR keeps it high and drives the body speaker only | BIN, RADIO |
| PTC18 (82) | Speaker source: high next to every main-chip AF on and side tone (analog squelch open, tone burst), low for the modem's audio (DMR call start, beeps, call end). Taken to select the main chip's AF (high) or the modem's line out (low) for the amplifier. ODR drives it low in DMR, high otherwise | BIN, INF |
| PTA19 (19) | FM broadcast audio enable | BIN, INF |
| ADC 2 | Board ID: display bus mode | BIN |
| ADC 6 | RSSI (squelch and channel busy) | BIN |
| ADC 0, ADC 7 | Read periodically by the stock firmware. Neither is shown to be the supply: the stock image has no supply-voltage read. ADC 7 is a hysteresis input (below 465 / above 527) that sets a GPIO of the LO BK4819; its source is unknown. On the owner's radio at a 13.5 V supply: ADC 0 reads 0, ADC 2 about 0, ADC 6 594 (no signal), ADC 7 742 | BIN, RADIO |

**Speaker volume.** The stock BK4819 init (0x03018000 main, 0x03018924 LO, the table in 4.3) writes register 0x48 as
0xB3BF and then 0xB3B3 near the end, so the AF DAC gain nibble ends at 3. The only other write of 0x48 with a chip
register address is a constant (0xB174, in a routine at 0x03018B1C). No code that was found computes 0x48 from a level,
and neither ADC 0 nor ADC 7 is read as a volume (BIN). How the stock radio changes its loudness, if it does, is not known; the VOL/SQL key may be its squelch key, not volume (?). ODR gives the radio a volume of its own: 16 steps (`RADIO_VOLUME_MAX`) on the AF DAC gain nibble (bits 3:0, the public BK4819 meaning), with the other fields of 0x48 left at the stock value (0xB3B3 upper bits). Step 0 is mute (register 0x47 to its mute word, and the chip's output off); steps 1..16 give nibble 0..15, the table being `nibble = (step - 1) * DB25D_VOL_GAIN_TOP / 15` in `radio_db25d.c` (`DB25D_VOL_GAIN_TOP` is 15; lower it if the top steps distort). The stock nibble 3 is step 4; the default step 8 gives nibble 7. The dB per nibble step, and whether the top steps distort the speaker, are not measured (INF). The debug link can set a step (`radio-features.md` 4.2). In DMR the chip's
output is the modem's input (4.5): it gets the stock digital gain word 0xB174 (nibble 4) and the volume step comes back when the radio
returns to FM.

The front keys are power (PTB12), P1, P2, P3, VOL/SQL, TONE/T.SEL, B/E and the encoder push; there's no separate
Menu/OK key (RADIO). Their matrix positions, found by pressing each key on the radio, with the stock firmware's key code
for the same position (BIN):

| Drive \ sense | PTB9 | PTB10 | PTB11 |
|---|---|---|---|
| PTB18 | - (9) | - (10) | - (11) |
| PTB20 | - (15) | - (16) | - (17) |
| PTB21 | - (27) | P1 (24) | P2 (25) |
| PTB22 | P3 (26) | VOL/SQL (28) | TONE/T.SEL (29) |
| PTB23 | B/E (6) | Encoder push (5) | - (17) |
| Direct (every drive line high) | - (25) | - (24) | none |

The stock firmware's row table repeats PTB18 where the 2023 build had PTB19, so stock codes 12-14 can't occur (BIN).
The hand mic's keys don't use the matrix (3.4). The 16-key hand microphone has its own MCU (FCC); how its keys reach the radio isn't
known (?).

### 3.4 Hand microphone

The 16-key hand mic has its own MCU (FCC). Its PTT is a plain contact on PTC11 (3.3). Its keys are serial: while a key
is held the mic sends three ASCII bytes, "K" and the key number as two digits, on PTA9 at 115200 baud 8N1, every few
milliseconds (a frame takes about 300 us). Nothing is sent when the key is let go. PTA9 is also the PC link's RXD, so
the host UART receives the frames, and the mic and the PC cable can't be used at the same time (RADIO).

| Key | 0-9 | * (DWN) | # (UP) | P4 | P5 | P6 | P7 |
|---|---|---|---|---|---|---|---|
| Number | 00-09 | 10 | 11 | 12 | 13 | 14 | 18 |

Numbers 15-17 weren't seen (RADIO).

## 4. RF chain

### 4.1 Architecture (PRO)

Superheterodyne with a **51.550 MHz IF** (COM, manual section 18.2). One BK4819 is the receiver and transmitter
("main"), the other is the local oscillator ("LO") (BIN):

| State | Main BK4819 | LO BK4819 |
|---|---|---|
| RX on f | Tuned to 51.550 MHz, RX state | Synthesizer at **f + 51.550 MHz** (high side): frequency in 0x38 / 0x39, state 0x30 = TX enable value so its output stage drives the mixer, level in 0x36 |
| TX on f | Tuned to f, TX state | Off (0x30 = 0, 0x37 = 0x1D00) |

Analog FM is modulated and demodulated by the main BK4819, not by the HR_C7000: the stock firmware never puts the C7000
in its FM modem mode (BIN). How the main chip's audio and modulation lines reach the C7000 for DMR needs the board (?).

### 4.2 Chip identification

Register 0 of each chip is read at boot (BIN):

| Register 0 | Main transceiver (letter) | LO (letter) |
|---|---|---|
| 0x6818 (FD6818) | '6' | '6' |
| 0x7813 (?) | '4' | '4' |
| 0x1846 | 'A' | - |
| 0x1100 (AT1141 PLL) | - | 'T' |
| 0x4819 (BK4819) | 'B' | 'B' |
| other | soft reset and read again | same |

### 4.3 BK4819 registers (address: value, in order)

The register meanings are the BK4819's public ones; the values are what the stock firmware writes (BIN).

| Register | Meaning |
|---|---|
| 0x00 | bit 15 soft reset |
| 0x30 | Enables. **Main chip** (bit 9 set in every state): 0x0200 idle, 0xBFF1 RX, 0xC3FE TX (VCO calibration, PLL / VCO, PA gain, mic ADC, TX DSP), 0xC3FA TX with the mic ADC (bit 2) off for a tone, 0x0302 during init. **LO chip** (bit 9 never set): 0x0000 idle, 0xC1FE its carrier-only transmitter stage (BIN) |
| 0x36 | PA bias and gain. Both chips: 0xBF (level 15 of 15), written once at detection / init and never changed, in TX too: the main chip drives the PA at 0xBF (BIN) |
| 0x37 | Power-up of the LDOs, bandgap and crystal (0x1D0F normal, 0x1D00 off) |
| 0x38 / 0x39 | Frequency, 10 Hz units, low / high 16 bits |
| 0x43 | Receive filter bandwidth: 0x3028 and 0x4048 (wide / narrow, INF). The channel set writes 0x30 = 0x0200, 0x52 = 0x028F, then 0x43 and 0x4F together: 0x3028 with 0x3332, or 0x4048 with 0x3D3C (BIN) |
| 0x47 | AF output select. The stock word is 0x6042 \| select << 8 \| (1 on a digital channel): 0x6042 muted, 0x6142 FM audio on, 0x6342 side tone, 0x6143 AF on in digital. ODR's FM uses 0x6140 (bit 1 clear, heard working) |
| 0x65 / 0x67 | Noise / RSSI indicators (as the FD6818, [hardware-dm32.md](hardware-dm32.md) 8.4) |

**Main chip init:** 00:8000, 00:0000, 33:0003, 34:0000, 35:0000, 37:1D0F, 47:6042, 13:03BE, 12:037B, 11:027B,
10:007A, 14:0019, 49:2A38, 7B:8420, 7D:E950, 48:B3BF, 1F:5454, 3E:A037, 3F:07EE, 2A:4F18, 28:6B00, 53:C074, 40:352A,
29:AB24, 4B:7126, 4E:6F15, 4F:3332, 27:A430, 48:B3B3, 37:1D9F, 30:0302, 47:6042, 30:0200, 33:0000, 30:0200, 47:6042,
30:0200, 37:1D0F, 7D:E34A.

**LO chip init:** the same up to 37:1D9F, except 4F:2F2E; then 30:0302, 47:6042, 30:0000, 33:0000, 30:0000, 47:6042,
30:0000, 37:1D0F, 51:0000, 40:0000, 73 |= 0x0010 (read, bit 4 set, written back: AFC off), 47:6943,
7E:3006, 43:4048, 47:6042, then **36:00BF**: an output level of 15 on a 0-15 scale, written as level x 63 / 15 with
bit 7 set (BIN).

**LO tune to f_LO:** 30:0000, 38 / 39 = f_LO / 10 Hz, 37:1D0F, 30:0000, 30:C1FE (BIN).

**LO off** (in TX, and while idle): 30:0000 only. 0x37, 0x36 and the frequency stay (BIN).

**Analog mode set, main chip** (stock, on entering an analog channel): 2A:4F18, 73 bit 4 cleared (AFC on; a digital channel sets it), 47 select 0
(6042), 7E:302E, 2B:C000, 50:3B18 (BIN). It doesn't restore 0x27, 0x40, 0x43, 0x48 or 0x51 after a digital channel; the channel set
rewrites 0x43 / 0x4F, the TX and sub-audio set 0x40 / 0x51, and 0x27 = 0x0430 and 0x48 = 0xB174 stay (BIN). ODR puts back the values it had.

**Digital mode set, main chip** (stock, on entering a digital channel, 4.5): 2A:4711, 51:0000, 73 bit 4 set (AFC off), 52:028F, 47:6943,
7E:3006, 43:4444, 40:0000, 27:0430, 2B:8707, 48:B174, 47:6143 (BIN).

### 4.4 FM transmit

The main chip transmits on f itself (4.1). The stock firmware's RF record carries the PA drive, the modulation pair
and the two TX delays per frequency (6.4); only the PA drive and the delays act on the BK4819 path. The modulation
pair and the TX center go to the C7000 modem's DMR registers, and the stock firmware never puts the modem in FM mode
(4.1), so analog deviation on this board comes from the chip's own registers: 0x40 (written before each transmission,
0x34D0 or 0x34A8 with a sub-audio tone), microphone gain 0x7D = 0xE34A, the same for 12.5 and 25 kHz (INF).

TX on f (the stock order, BIN; ODR differs where noted):

| Step | Action |
|---|---|
| 0 | Still receiving: main chip 0x40 = 0x34D0 and 0x51 = 0x1050 with no sub-audio; with CTCSS 0x40 = 0x34A8, 0x51 = 0x9045 and 0x07 = Hz x 20.64888 truncated (100.0 Hz = 2064); with DCS 0x40 = 0x34A8, 0x07 = 0x0AD7, 0x51 and the code (4.7) |
| 1 | ODR only: main chip AF muted, then the stock "audio off" pins: PTB14 low, PTB15 high, PTA24 high (3.3). The first ODR build raised PTB14 here and the speaker popped at key-up (RADIO); whether this order removes the pop and still passes the microphone is not yet checked |
| 2 | RX front end (PTC19), UHF RX (PTA25) and VHF RX (PTB4) off |
| 3 | 50 us |
| 4 | Main chip: 38 / 39 = f / 10 Hz (truncated), 37:1D0F, 30:0200, 30:C3FE (ODR writes the idle word before the frequency, not after). Then the LO chip 30:0000 only. 0x36 stays 0xBF |
| 5 | The page's TX-on delay for f (1 us UHF, 1000 us VHF on the first unit). The stock has no other wait. ODR adds 3 ms (`TX_SETTLE_MS`) for the VCO calibration: no measurement says it can go, and a PA that comes up before the PLL has settled puts a carrier off frequency |
| 6 | DAC B powered up at 0 (ODR only: the stock leaves it on), PA select of the band (UHF PTA26 high, PTB3 low; VHF PTB3 high, PTA26 low), PTA23 low, 200 us, the band's single TX supply (UHF PTA27 high, PTB2 low; VHF PTB2 high, PTA27 low) |
| 7 | DAC B ramps from 0 to the drive in 180 steps of 4 us |

TX end: the main chip to its RX state on the TX frequency (47:6042, 50:3B18, 70:0, 37:1D0F, 30:0200, 30:BFF1), the page's
TX-off delay (1700 us UHF, 1600 us VHF), the ramp back down, DAC B 0, both TX supplies and both PA selects low, PTA23
high, 300 us. Then the receive set-up (4.1): the main chip back to the IF, the LO re-enabled (37:1D0F, 30:0000,
30:C1FE, its frequency not rewritten), 300 us, the RX path of the band, 200 us. ODR then powers DAC B down. 0x36 and 0x40 stay as they
are (0x40 only matters in TX, INF).

The stock firmware shapes the ramp with a 180-entry table that ODR doesn't have; it uses a smoothstep over the same 180
steps. `DB25D_TX_DRIVE_PCT` defaults to 100 (the page's value, the PA measured working); the stock levels are table
0x045 ("H") and 0x056 ("L").

**FM TX tone** (`tx_tone` / `tx_tone_key`: the 1750 Hz burst, roger beep, CW ID, a test tone). Stock single tone (BIN), in TX
with the main chip at 0xC3FE:

| Step | Registers |
|---|---|
| Tone on | 47:6342 (AF select 3, the side tone), 70:B000 (tone 1 on, gain 0x30 in bits 14:8), 50:BB18, 37:1D0F, 30:0200, 30:C3FA (microphone ADC off), 71 = Hz x 10.3244 truncated (1750 Hz = 18067), 50:3B18 |
| Key up (silent carrier) | 70:0; the chip stays at 0xC3FA, so the microphone is still out (ODR's choice, INF) |
| Key down | 70 as for tone on (ODR's choice, INF) |
| Tone off | 47:6042, 50:3B18, 70:0, 37:1D0F, 30:0200, 30:C3FE, 40:34A8 |
| End of the transmission | The same without 30:C3FE: it ends in 30:BFF1 (TX end above), so a tone never survives a transmission |

The stock firmware never leaves 0x50 bit 15 set after the sequence; whether that bit mutes the tone as well as the microphone is
not readable from the code (INF), so ODR doesn't keep it. DTMF (not used by ODR) writes 70:9090 and 40:348A at 0xC3FA.

The level is `DB25D_TX_TONE_GAIN` (radio_db25d.c, default 0x30: the stock single tone). Measure the deviation of the 1750 Hz
burst on the SDR bench (`odr_debug.py burst on`, FM channel) and scale the gain; the target is about 60 % of the nominal
deviation (1.5 kHz on 12.5 kHz). The first on-air run of the earlier sequence (0xC1FE, 0x50 bit 15 set) gave no modulation;
the corrections above are from the stock trace and not yet on air.

### 4.5 DMR: the main chip and the modem

ODR's DMR (`CONFIG_DMR=y`, on by default; `boards/db25d/dmr_db25d.c`) receives and transmits (`DB25D_DMR_TX`, on by default, below). The HR_C7000 runs the DMR layer 2 as on
the DM-32UV ([modem-hrc7000.md](modem-hrc7000.md)). This section is the stock firmware's set-up, traced on the 2025-01 image,
and what ODR copies.

**Channel type.** Byte 6 of the stock channel record: 2 is digital (the BK4819 digital set below, no bandwidth, sub-audio or squelch,
the modem's WORK_MODE bit 7 cleared, the DMR microphone gain from the settings), 1 is analog (bandwidth, sub-audio, squelch)
(BIN, INF for the names).

**Stock sequence for a digital channel** (the receive set-up of 6.4, in order; BIN):

| Step | Action |
|---|---|
| 1 | DAC B 0, both TX supplies low, RX path pins (3.2) |
| 2 | Modem SIG_CENTER and RF_MOD_BIAS_CTRL from the page (4.6), DAC A = RX tune, DAC C = 1780 |
| 3 | Main chip: 30:0200, 52:028F, then **the digital mode set** (only when the previous channel was analog): 2A:4711, 51:0000, 73 bit 4 set (AFC off), 52:028F, 47:6943, 7E:3006, 43:4444, 40:0000, 27:0430, 2B:8707, 48:B174, 47:6143 |
| 4 | Main chip RX tune as for FM: 38 / 39 = 51.55 MHz, 37:1D0F, 30:0200, 30:BFF1 |
| 5 | LO tune to f + 51.55 MHz (4.3), RX path of the band, main chip GPIO 0x33 band bit (VHF only) |

Nothing on the LO chip changes for digital. No switch pin moves for receive; the only audio pins are the speaker ones below.

**Modem.** The stock DMR initialization runs once, at start-up, in every mode (BIN):

| Register | Stock | ODR |
|---|---|---|
| RF_MODE (0x104) | Built by read-modify-write from the reset value 0x10E0: b7 cleared, b25 and b24 set (AF receive with averaging), **AF gain 0x4C** (4.75) in b23:16, TX mode 3, RX mode 0, b15 = a flag the RF record build sets (the LO is high side; the start-up builds a record before this, so b15 = 1), b11 set (dual receive; cleared only for a channel value of 2 or 3, INF privacy), b31 / b29 only in a test mode. Result **0x034C9860** | Same word (`DB25D_DMR_RF_MODE`). The DM-32UV's is 0x03269860: only the gain differs |
| RF_CONTROL (0x110) | Reset 0x00040A0A ORed with 0x1512: 0x00041F1A | Same |
| DAC_CONTROL (0x070) | Reset ORed with b31, b5 cleared: 0x80000016 | 0x8000001B (b2 and the reserved b3 / b0 differ) |
| ADC_CONTROL, 0x168, RF_RECV_DELAY, RX / TX_IF_FREQ, THRESHOLD, sync words, CRC masks, LAYER2_SLOT_PRE, interrupt mask | As modem-hrc7000.md 4 (0x41C3 ORed, 0x14, 0x36B0, 0xBB800, 0x0978786F, ...) | Same |
| Sync enables (b31 of the sync words) | MS and BS; TDMA 1 / 2 only for the direct-mode channel value | All four |
| DEV_LIMITER | Never written | Its reset value |

No other AF level, DC offset or deviation register is written for receive (BIN).

**The modem's input.** The main chip's AF output (0x47 select 1, AF DAC gain 0x48) goes to the modem's AF ADC (INF: the only AF
the stock firmware leaves on in digital; nothing mutes 0x47 on a digital channel). AF level: 0x48 = 0xB174 (nibble 4, RX gain field
0x17), the AF filters 0x2B = 0x8707.

**Speaker.** Decoded voice leaves the modem's codec line out. The stock call start drives PTC18 low and runs the audio-on pins (PTB14
high, PTB15 low, speaker enables, 3.3); the call end runs audio off (BIN). The stock image never uses the CPU-fed speaker buffer; its
beeps go through the prompt path (AUDIO_CONTROL b7) (BIN). PTC18 is high for the main chip's audio (3.3).

ODR (`bk4819_dmr_rx_enter`, `db25d_dmr_enter`, tests/host/bk4819Test.c):

| Item | ODR |
|---|---|
| Main chip | The stock digital set, in its order, on entering DMR. Leaving: the stock analog set, then 0x27 and 0x48 as they were |
| AF gain | `DB25D_DMR_AF_GAIN` (default 4, the stock nibble) over the 0xB174 word, whatever the volume step. A volume of 0 entering DMR left the modem with no input before this |
| AF output | 0x47 = 0x6143 for as long as the radio is in DMR |
| RF_MODE | `DB25D_DMR_RF_MODE` (default 0x034C9860), written after the DMR entry |
| PTC18 | Low in DMR, high in FM; `DB25D_DMR_SPK_SWITCH=1` keeps it high |
| Speaker in DMR | The PCM stream (`db25d_speaker_pcm`, from the modem's playback callback) turns the amplifier on and PTC18 low, as `audio()` does for an open squelch. The radio core never opens the squelch path for DMR voice, so before this every call played into a muted amplifier; beeps went silent the same way |
| Voice-access slot | `HRC7000_RX_NEXT_SLOT_XOR=0` (the driver's default since 2026-10-22; it was 1): the flag (modem-hrc7000.md 5.3) belongs on the slot LAYER2_STATUS b30 shows at this board's RX timeslot interrupt. With 1 the chip never wrote received voice to its voice RAM: every frame was the same stale bytes, decoded to silence (peak 6 of 32767) with every counter clean. With 0, bench speech decodes with peaks near 16,000 (RADIO, 2026-10-17) |

Radio results before the stock set (bench MS voice, 461.4625 MHz, CC 2): RF_MODE 0x03269860 gave no sync; 0x03261860 (b15 clear) gave
sync, the right color code and slot, but no usable burst; a larger AF gain held sync on noise. The PHASE_OUT range read exactly 32 times
the RF_MODE gain field (saturated), so on this board it doesn't measure the AF level (RADIO).

With the stock set and word above (2026-10-16, same bench call, 191 bursts): 180 voice bursts, 8 LC headers, 3 terminators, CC 2,
no CRC errors, the call decoded (source, talkgroup 201, group), and 540 of 540 AMBE frames with none lost or concealed (RADIO). With
the stock BK4819 set, b15 set is right; b15 clear (0x034C1860) syncs but deframes no usable burst.

The diagnostics page (CONFIG_DIAG) lists the modem counters after the TX line 11 and the storage lines 12-15 (from line 12 when
`CONFIG_NAND_STORAGE` is off): ticks, slot interrupts, bursts by type, CRC errors, and last the codec in the slot with the AF phase
range (HR_C7000 PHASE_OUT, 0x128).

**DMR transmit.** On by default (`DB25D_DMR_TX=1`); `DB25D_DMR_TX=0` builds receive only, and then `tx()` refuses in DMR
mode. Checked on the SDR bench 2026-10-16 (RADIO): a 3 s key-up on 461.4625 MHz, low power into a dummy load, decoded by the
bench modem as one call from the radio's ID to TG 201 (3 LC headers, 54 voice bursts, terminator, 162 AMBE frames with 12 FEC
bits corrected); the diagnostics line showed 57 bursts keyed and ended, each ended by the RF RX interrupt, none late. Analog FM transmit is 4.4. What the stock firmware does, and what ODR does:

| Item | Stock firmware | Tag |
|---|---|---|
| 4FSK path | **Single-point modulation on the reference oscillator.** The modem runs in two-point mode (RF_MODE b6:5 = 3) but only its MOD1 output is used: RF_MOD_BIAS_CTRL b15:8 (MOD2 amplitude) is 0, b7:0 (MOD1 amplitude) is the page's modulation byte for the TX frequency (6.4), and MOD1 drives the control pin of the TCXO both BK4819s run from (the DC level of 4.6). The HR_C7000 user guide describes this case (single-point modulation with an RF chip that has no modulation input). The deviation therefore scales with the carrier frequency, which is why the page holds the modulation byte per frequency point | BIN, DS, INF |
| RF_MODE | Built bit by bit at DMR set-up: bit 7 (TX symbol mapping inverted) **cleared**, bits 25 and 24 set (AF receive with averaging), AF gain field 0x4C, TX mode 3, RX mode 0, bit 15 (RX IQ inverted) set when the LO is on the high side (always here), bit 11 (dual receive) set except for two values of a setting. With the reset value 0x10E0 that gives 0x034C9860. The DM-32UV's 0x03269860 has bit 7 set and AF gain 0x26 | BIN, DS |
| RF_CONTROL | The reset value (0x00040A0A) OR-ed with 0x1500 and 0x12: 0x00041F1A, the DM-32UV's value (RF TX interrupt 3.1 ms before the burst, RF RX interrupt 2.6 ms before the next slot). With one value of a setting byte, 0x00041A1A instead | BIN, DS |
| Main chip, digital channel | The channel set (0x30 = 0x0200, 0x52 = 0x028F) calls the mode routine with "digital" when the channel's type is 2: 0x2A = 0x4711, 0x51 = 0, 0x73 bit 4 set (AFC off), 0x52 = 0x028F, 0x47 = 0x6943, 0x7E = 0x3006, 0x43 = 0x4444, **0x40 = 0x0000 (TX deviation off)**, 0x27 = 0x0430, 0x2B = 0x8707, 0x48 = 0xB174, 0x47 = 0x6143 (AF on, bit 0 = digital). Only on a change of mode. This corrects the receive table above: the stock firmware does have a digital set | BIN |
| Main chip in TX | 0x30 = 0xC3FE (microphone ADC on), 0x36 = 0xBF as always. With 0x40 = 0 the microphone doesn't reach the carrier: the BK4819 sends a plain carrier that the TCXO modulates | BIN, INF |
| Per-burst switch | **Every burst runs the whole analog TX sequence.** The RF TX and RF RX interrupt handlers only post to a task ("rfint", priority 15 of the RTOS) and record which one fired. On a digital channel the task runs TX on (3.2: RX paths off, 50 us, main chip to f, LO 0x30 = 0, the page's TX-on delay, PA select, 200 us, TX supply, DAC B ramp up) at the RF TX interrupt, and TX off (the page's TX-off delay, ramp down, supplies and PA selects off, PTA23 high, 300 us, main chip to the IF and the LO on again, 300 us, the RX path, 200 us) at the RF RX interrupt. So the radio receives between its bursts, on the RX frequency | BIN |
| Burst timing | TX on starts 3.1 ms before the slot; with the UHF page values (1 us TX-on delay) the PA is fully up about 1.1 ms later. TX off starts at the RF RX interrupt, 30.5 ms after the RF TX interrupt; with the page's 1700 us TX-off delay (UHF) the ramp down starts 32.2 ms after the RF TX interrupt and ends about 0.2 ms before the next slot. Waits of 50 us and more are timer waits that let other tasks run; shorter ones spin | BIN, INF |
| Retune per burst | Main chip: 0x38 / 0x39 = f, 0x37 = 0x1D0F, 0x30 = 0x0200, 0x30 = 0xC3FE; back: 0x38 / 0x39 = 51.55 MHz, 0x37, 0x30 = 0x0200, 0x30 = 0xBFF1. LO: 0x30 = 0 at TX on; 0x37 = 0x1D0F, 0x30 = 0, 0x30 = 0xC1FE at TX off, its frequency not rewritten | BIN |
| PA keying | DAC B ramp (180 steps of 4 us) and the band's single TX supply pin inside the burst sequence, from the page's PA table for the power level. No PA timer: the timer the DM-32UV uses for its PA one-shot isn't used for that here | BIN |
| Modem bias | SIG_CENTER and RF_MOD_BIAS_CTRL as in 4.6 and 6.4, written at every receive set-up from the channel's RF record, which holds the modulation byte for the TX frequency | BIN |
| Mic and vocoder | The microphone reaches the modem's codec ADC with no pin to switch: PTB14 / PTB15 appear only in the audio on / off routines (3.3), never in a TX path. Mic gain (GCR_ADCL) on a digital channel comes from a setting (0 dB when unset); an analog channel sets +12 dB. WORK_MODE bit 7 cleared (DMR modulator) | BIN |
| Analog PTT routine | Not used for a digital channel: the RF follows the bursts only (INF from the task's channel-type test) | INF |

ODR (`boards/db25d/dmr_db25d.c`, `radio_db25d.c`; tests in `tests/host/bk4819Test.c` and `db25dCalTest.c`):

| Step | ODR |
|---|---|
| `tx()` in DMR mode | Checks band and page as FM does, keeps the TX plan (PA drive, TX-on and TX-off delays), writes RF_MOD_BIAS_CTRL with the modulation byte for the TX frequency, main chip 0x40 = 0 and 0x51 = 0 (`bk4819_dmr_tx_prepare`), powers DAC B at 0, clears RF_MODE bit 7 (`make DB25D_DMR_TX_INVERT=1` sets it, to try the other 4FSK polarity) and arms. The RF stays receiving until the first burst |
| RF TX interrupt | The glue's `rf_switch(true)` records the time and wakes the RF task (priority above every other task), which runs the stock TX on (`db25d_burst_on`) |
| End of a burst | Woken by the modem's RF RX interrupt (as stock, enabled while armed) or by the engine's `rf_switch(false)` at the first slot tick 20 ms after the RF TX interrupt, whichever comes first. The task then waits until 30.5 ms + the page's TX-off delay after the RF TX interrupt (`db25d_dmr_hold_us`) and runs the stock TX off (`db25d_burst_off`). A wake-up after that point ramps down at once and counts as late |
| Waits | Below 50 us a spin; from 50 us TIMER_BOARD (timer 5) while the task sleeps, as stock |
| `tx_off()`, `rx()`, leaving DMR | Disarm: TX off at once (no hold) if a burst is on, wait for the task, DAC B powered down, then the normal receive set-up |
| PA one-shot | The glue still arms it per burst; the board's hook does nothing |
| Diagnostics | The line after the codec line: `b<on>/<off> rx<RF RX interrupts> <R/T/A> L<late>/<last late us>`. R, T or A says what woke the last TX off. `rx` that stays at 0 while transmitting means the RF RX interrupt doesn't fire in TX, and the slot tick ends the bursts; L counts the ramp downs that started after the stock point |

Answered by the bench check above: 0x40 = 0 with the rest of the analog set gives a carrier the modem's 4FSK decodes,
the stock polarity (RF_MODE bit 7 clear) is right, and the RF RX interrupt ends every TX burst. Deviation with the
page's modulation byte (2026-10-17, 461.4625 MHz, low power, SDR bench, instantaneous frequency inside the bursts): the
four symbol levels at -1970, -653, +676 and +1970 Hz around the carrier, so the outer deviation is 101 % of the
standard's 1944 Hz and the outer / inner ratio 2.97 (3.00 ideal); carrier 187 Hz high (0.4 ppm) (RADIO).

The receive and transmit checks are at the end of section 9.

### 4.6 Frequency reference: the modem's bias

On the owner's radio both receive and transmit read 23 ppm low (10.6 kHz at 461.4625 MHz) and DAC C changes nothing. The
stock firmware writes the C7000 modem's DC bias registers at every receive set-up, in FM as well as DMR (BIN): SIG_CENTER
0x11000108 and RF_MOD_BIAS_CTRL 0x11000114 as in 6.4. The BK4819s have no VCO input, so the modem's two-point modulation
puts its low-frequency half on the TCXO's control pin and this DC level sets the TCXO frequency; both chips share that TCXO,
so receive and transmit move together (INF, matches the measurement). The factory tunes the TX center byte with the radio transmitting, and
nothing else on the page trims the frequency (BIN). Before this was written only with `CONFIG_DMR`; ODR now powers the modem
(`hrc7000_power_on_init`) at init in every build and writes both words at init and at every `rx()`.

| Page TX center c (0x43F) | m | 0x11000108 | 0x11000114 (mod byte 0x21 at 461.4625 MHz) |
|---|---|---|---|
| 106 (owner's radio) | 489 (0x1E9) | 0x6A6AE9E9 | 0x01E90021 |

`make BOARD=db25d DB25D_TX_CENTER=N` replaces c, to sweep it on the radio: a few ppm per step is expected.

### 4.7 DCS

The main chip sends and detects DCS itself, with the code as the 23-bit Golay word (code bits first,
`subaudio_dcs_word`) in two writes to 0x08 (BIN):

| Register | Value |
|---|---|
| 0x07 | 0x0AD7 (the 134.4 bit/s rate) while the code is set; 0x4AD7 after it on receive |
| 0x08 | word bits 11:0, then bits 22:12 with bit 15 set (D023: 0x0813, 0x8763; D754: 0x09EC, 0x820F) |
| 0x51 | 0x8030 normal, 0xA030 inverted (bit 15 on, bit 13 invert, bit 12 = 0 selects DCS, the rest the gain) |
| 0x40 | 0x34A8 on transmit, as with CTCSS |
| 0x0C | received: bit 14 the code as set, bit 15 its inverse (bit 10 CTCSS) |

| Order | Steps |
|---|---|
| TX (4.4 step 0) | 0x40, 0x07 = 0x0AD7, 0x51, 0x08 twice (BIN) |
| RX | 0x07 = 0x0AD7, 0x51, 0x08 twice, 0x07 = 0x4AD7 (BIN) |

The receive chain inverts the audio: the LO sits above the signal (f + 51.55 MHz, 4.1), so the IF spectrum is turned
over and the deviation's sign flips. A normal code arrives as its inverse. ODR sets 0x51 for the code as sent and
reads 0x0C's other bit: a normal code sets bit 15, an inverted one bit 14 (`bk4819_t.rx_inverted`). The stock
firmware instead inverts its software decoder's polarity, on PB26 / PB27 (BIN). On the SDR bench another radio's
D023N and D023I opened the matching setting only, and D754 and CTCSS settings stayed shut; the radio's D023N went out
as D023N (RADIO). The diagnostics page's `rssi` line shows 0x0C as `c` (hex), polled with the RSSI.

At the end of a DCS transmission the stock firmware writes 0x52 = 0x828F, waits 300 ms and writes 0x028F (a turn-off
code, BIN). ODR doesn't send it yet.

## 5. Display

| Item | Finding | Tag |
|---|---|---|
| Panel | 1.77" color TFT, ST7735S-class controller, 160 x 128 landscape (MADCTL 0x60) | COM, BIN, INF for the size |
| Reset | high 10 ms, low 10 ms, high 130 ms | BIN |
| Init | ST7735 frame-rate and power commands as ODR's `st7735` type 1 except VCOM (C5 = 0x12) and the first positive gamma pair (E0: 04 22 ...) | BIN |
| Backlight | PWM1 (0x140C0020) on PTA16 at 1 kHz: PER = 24,000, FP = on-time. Which clock the PWM counts is not established: the user guide says only "system clock" and a 54 MHz maximum, so 24,000 is 1 kHz from a 24 MHz clock and 1.5 kHz from the loader's APB; ODR keeps the stock PER, so the rate is 1 kHz to 3.5 kHz whichever it is. Stock levels 0-6 give 10, 25, 40, 55, 70, 85, 100 % duty; 0 only when switched off. Stock set-up: CFG bits 5, 8, 9, RPT 5, PER, FP, then CFG bit 2 (load) and bit 0 (enable). The light also needs PTA15 high (3.3) | BIN, DS, RADIO |

## 6. Storage and calibration

| Item | Finding | Tag |
|---|---|---|
| NAND | Kioxia TC58CVG1S3HRAIJ, 2 Gbit SPI NAND: 2 KB pages, 128 KB blocks. Standard SPI NAND opcodes (0x9F ID, 0x13 page read, 0x06, 0x84, 0x0F / 0xC0 status, 0xD8 erase). The stock firmware builds a bad-block table at boot (6.5) | FCC, BIN, DS |
| Calibration | A 2 KB page at **NAND 0x280000** (block 20) starting with "TUNE", and a copy in program flash at 0x031FE000 (4 KB sector). The stock firmware repairs one copy from the other at boot | BIN |
| Calibration layout | 6.1 – 6.4 | BIN |
| Stock NAND areas | 0x40000, 0x110000, 0x3C0000, 0x1100000, 0x2980000 (codeplug and databases, INF) | BIN |

**Rule:** ODR reads both calibration copies and never writes them. It never writes program flash at 0x031FE000 or NAND
block 20.

### 6.1 Page header and the two copies

| Offset | Size | Field | Tag |
|---|---|---|---|
| 0x000 | 4 | Magic "TUNE" | BIN |
| 0x004 | 4 | Save counter, u32 little-endian. The stock tuning mode adds 1 on every save | BIN |

At boot the stock firmware reads 8 bytes of each copy. If both carry "TUNE", the copy with the higher counter wins and
is copied over the other. If only one does, it's copied over the other. If neither does, the radio runs untuned ("No
tune"). There's no checksum. ODR applies the same choice when it reads, without writing anything back (BIN).

### 6.2 Bands and frequency points

The page has two 1 KB halves with the same layout: **0x000–0x3FF for UHF and 0x400–0x7FF for VHF**. The stock
firmware picks the half from the band of the frequency in use. Offsets below are UHF; add 0x400 for VHF. The few
fields that exist only once are marked (BIN).

Most tables hold one value per **frequency point**, 8 points per band (BIN):

| Point | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| UHF (MHz) | 403.175 | 414.175 | 425.175 | 436.625 | 447.775 | 458.775 | 469.825 | 478.825 |
| VHF (MHz) | 136.125 | 142.125 | 146.125 | 152.125 | 156.125 | 162.125 | 166.125 | 173.125 |

A per-point table is one leading byte, then 8 entries in point order, so entry p (1–8) of size n starts at
table + 1 + n·(p − 1). The leading byte isn't used by any per-point lookup (BIN).

**Interpolation (BIN):** for f at or below point 1, use point 1. Otherwise take the first point p ≥ 2 at or above f
and interpolate linearly between points p − 1 and p, rounding half up. Above point 8 (478.825–480 MHz, 173.125–174
MHz), the stock code reads past the table into the next one. ODR uses point 8 there instead.

The squelch table works by **segment**: 8 segments per band. VHF: below 142, 142–146, 146–152, 152–156, 156–162,
162–166, 166–173, from 173 MHz. UHF: below 410, then 10 MHz steps (410–420 … 460–470), from 470 MHz. A segment's
upper edge belongs to the next segment (BIN).

### 6.3 Fields

| Offset (UHF) | Size | Content | Used for | Tag |
|---|---|---|---|---|
| 0x02C | 1 + 8 × u16 | RX front-end tuning, per point | DAC A, written when the receiver is set up for a frequency. The tuning voltage of the receive band-pass filter (INF) | BIN |
| 0x43E (VHF half only) | 7 | Global bytes, both bands. Byte 0x43F is the **TX center** (128 = mid-scale, as on the DM-32); the other six aren't traced | Modem SIG_CENTER and RF_MOD_BIAS_CTRL, 6.4 | BIN |
| 0x045 | 1 + 8 × u16 | **PA drive, power level 1** (H, INF), per point. DAC B value: the APC ramp's end value | TX power | BIN |
| 0x056 | 1 + 8 × u16 | **PA drive, every other power level** (L, INF), per point | TX power | BIN |
| 0x067 | 1 + 8 × (u8, u8) + 1 | Modulation, per point. The first byte of each pair goes to the modem (6.4). The second is read but then cleared before use | TX modulation level | BIN |
| 0x09B | 1 + 8 × u8 | A per-point value capped at 15, written into the RF chip driver's state (use not traced). The leading byte is read on its own and kept with the channel's RF parameters | ? | BIN |
| 0x0AE | 1 + 8 × (u16, u16) | TX timing in µs, per point: the delay between tuning the transmitter and selecting the PA, and the delay at TX end before the ramp down. A value above 2000 counts as 0 | TX switching | BIN |
| 0x0CF | 1 + 8 × 9 × u16 | **Squelch**, per segment: one value for each squelch level 1–9 (level 0 doesn't use the table) | RF chip squelch, 6.4 | BIN |
| 0x160, 0x1B1 | 1 + 8 × 5 × u16 each | Five values for each of 8 ranges: up to point 2, then between points (2–3 … 7–8), then above point 8. No interpolation. Two variants chosen by channel mode (use not traced) | ? | BIN |
| 0x202 / 0x206 / 0x207 / 0x208 / 0x20B | 4 / 1 / 1 / 3 / ? | Small items the tuning mode reads and writes. 0x20B is in the UHF half only (use not traced) | ? | BIN |

The tuning mode moves each table as a unit (the sizes above), plus 198 bytes from the start of each half. Everything
not listed is unused as far as the image shows.

### 6.4 What the values drive

| Output | Value | Tag |
|---|---|---|
| PA drive (DAC B, 12 bit) | The interpolated 0x045 or 0x056 value for the TX frequency, chosen by power level. The APC ramp runs up to it at TX start and down from it at TX end, as on the DM-32 | BIN |
| RX front end (DAC A) | The interpolated 0x02C value for the RX frequency | BIN |
| DAC C | 1780 when receive is set up, not from the page. It has no effect on the frequency (RADIO); the TCXO trim is the modem bias below (4.6) | BIN, RADIO |
| SIG_CENTER (modem 0x108) | Written at every receive set-up, FM and DMR (4.6). (c << 24) or (c << 16) or ((m & 0xFF) << 8) or (m & 0xFF), with c = the TX center byte and m below | BIN |
| RF_MOD_BIAS_CTRL (modem 0x114) | ((m & 0x3FF) << 16) or a. m = round((1.228 + c × 0.422 / 128) × 1023 / 3.3): 381 at c = 0, 512 at c = 128, close to the DM-32's c + 384. a = the interpolated first byte of 0x067 | BIN, DM32 |
| BK4819 squelch (reg 0x78) | For level n (1–9), v = low byte of entry n of the segment's 0x0CF table: v is the open threshold in dB below 0 dBm at the main chip. Register = (o << 8) or (o − 4), o = 2 × (160 − v), i.e. open at −v dBm and close 2 dB lower. Level 0 uses v = 136 | BIN |
| Untuned radio | No page: the stock firmware keeps running, values unknown | BIN |

ODR's use: the reader (`boards/db25d/calibration_db25d.c`) copies the chosen page into RAM at boot and never writes
either copy. The program-flash copy is memory-mapped, so it's read directly: the stock firmware reads it with a plain
byte copy from 0x031FE000 (BIN). The NAND copy is read through the SPI NAND driver (6.5): its 8-byte header first, the
whole page only when it wins, and a NAND page that can't be read gives way to the flash copy. Interpolation is the
shared calibration curve (`core/radio/cal_curve.h`). Receive sets DAC A and DAC C from it now. TX power comes from 0x045 ("H") or 0x056 ("L"), and
TX refuses when the page is missing or the PA value for the frequency is 0 or above the DAC's range. On the first
unit's page, table 0x045 is the higher DAC value on both bands (UHF 1430-2001 against 999-1193, VHF 916-1512 against
590-928), so it is the high level; that a higher DAC B value is more output depends on the APC loop and isn't measured
(INF). The RSSI that ODR shows stays the chip's own reading: the page has no RSSI
offset, only squelch thresholds (INF).

### 6.5 NAND access

How the stock firmware drives the NAND (driver 0x0300F494 … 0x03010600), which ODR's read side follows, and what the
data sheet adds:

| Step | Sequence | Tag |
|---|---|---|
| Bus | SPI0, IO_DIPLEX1 bits 3–8 cleared (SCLK / MOSI / MISO to the SPI master), CS PTA18 by GPIO, CTRLR0 7, BAUDR 4 | BIN |
| ID | 0x9F, one dummy byte, then 0x98 (Kioxia), 0xEB (device), 0x40 (organization: 2 KB pages, 128 KB blocks) | BIN, DS |
| Organization | (2048 + 64) bytes × 64 pages × 2048 blocks. At least 2008 good blocks over the chip's life; blocks 0–7 are good when shipped | DS |
| Page read | 0x13 and a 3-byte page number (17 bits), then 0x0F 0xC0 (status) until bit 0 (busy) clears: 180 µs at most. The stock firmware remembers the last page read and doesn't read it again | BIN, DS |
| Read from cache | 0x03, a 2-byte column (12 bits; the stock firmware masks 13), one dummy byte, then data. Byte address = page × 2048 + column | BIN, DS |
| Status (0xC0) | Bit 0 busy, 1 write enabled, 2 erase failed, 3 program failed. Bits 4–5 ECC: 0 clean, 1 corrected, 2 uncorrectable, 3 corrected at or above the bit-flip threshold (feature 0x10) | BIN, DS |
| ECC | On by default (feature 0xB0 bit 4): 8 bits corrected per 512 + 16 bytes. Spare bytes 2048–2111 are readable, the parity at 2112–2175 isn't | DS |
| Block lock | Every block locked at power-up (feature 0xA0); reads don't need it cleared, program and erase do | DS |
| ID pages | Feature 0xB0 bit 6 (IDR_E) set, then a page read: page 0 is the unique ID, page 1 the parameter page (data sheet 4.12, 4.14); clear the bit after. The stock firmware reads 256 bytes of page 0 at boot, so the unique ID, not the parameter page as an earlier version of this table said | BIN, DS |
| Unique ID | Page 0 with IDR_E: 16 copies of 32 bytes, each the 16-byte ID followed by its complement. ODR reads it at boot (`spi_nand_read_uid`, the first copy whose complement matches) as the radio's UID for the debug link's INFO_CHIP ([debuglink.md](debuglink.md) 3.9) | DS |
| Spare bytes | The stock firmware keeps its own block marks at spare offset 4 (column 2052) | BIN |
| Bad blocks | Factory-bad blocks read 0x00 in every page; the mark is judged from the data whatever the ECC status says | DS |
| Stock bad-block skip | The stock firmware builds a bad-block list at boot and, for some address ranges, skips bad blocks when it maps an address to a page. Which ranges, and whether that ever moves block 20, isn't decoded | BIN |

The write side, from the data sheet:

| Step | Sequence | Tag |
|---|---|---|
| Unlock | 0x1F 0xA0 0x00 before a program or erase; 0x1F 0xA0 0x38 (every block locked, the power-up value) after it | DS |
| Program | 0x06 (write enable), 0x02 and a 2-byte column with data (clears the rest of the buffer to 0xFF), 0x10 and the 3-byte page number, then 0x0F 0xC0 until not busy: 500 µs at most. Pages of a block are programmed in order | DS |
| Internal data move | 0x13 of the source page, then 0x06, 0x84 and a column with data (keeps the rest of the buffer), 0x10 to the new page. Needs feature 0xB0 bit 1 (HSE) clear. The page's ECC parity is computed again | DS |
| Erase | 0x06, 0xD8 and the 3-byte page number of the block, then status until not busy: 7 ms at most | DS |
| Partial programs | The chip allows 4 per page, but with ECC on each 512 + 16 byte unit takes one; ODR programs every page once | DS |
| While busy | Only 0x0F (status) and the reset commands may be sent | DS |
| Stock configuration | The stock firmware writes feature 0xB0 = 0x14 | BIN |

ODR's driver is `drivers/flash/spi_nand` (generic). It takes the geometry from the board and treats a block whose
first page's first spare byte isn't 0xFF as bad. The DB25-D reads the calibration copy at its physical address: if a
skip ever moved it, its header won't say "TUNE" and the program-flash copy is used. With `CONFIG_NAND_STORAGE` the
driver also programs and erases: at probe it writes feature 0xB0 = 0x10 (ECC on, HSE off for the internal data move)
and 0xA0 = 0x38, unlocks only around each operation, and refuses any program or erase outside ODR's range (6.6). One
lock serializes every NAND user (calibration read, storage).

### 6.6 ODR storage on the NAND

ODR keeps its own data in physical blocks **1920-1951** (32 blocks, 4 MB), through `drivers/flash/nand_sectors`
([nand-sectors.md](nand-sectors.md)), which turns the region into 4 KB sectors that behave like NOR flash. Why these
blocks are safe from the stock firmware and the calibration:

| Area | Where | Tag |
|---|---|---|
| Calibration | Block 20 | BIN |
| Stock data (codeplug, contacts, resources) | Logical addresses up to about 0x5000000: block 640, a few blocks higher after the bad-block skip | BIN, INF |
| Stock scratch blocks | The top 10 good blocks of the chip: 2038-2047 on a chip with no bad blocks, never below about 1998 with the most bad blocks the data sheet allows (40) | BIN, DS, INF |
| ODR | 1920-1951: far above the stock data, below the lowest place the scratch blocks can reach | INF |

The stock firmware's use of this range isn't proven: only its logical data area and its scratch blocks are known,
and nothing above the data area is decoded. What makes the range safe is the layer's foreign-data guard
([nand-sectors.md](nand-sectors.md) 4): a block whose first or last page reads back clean, isn't erased and carries no
ODR record makes the mount fail. Then nothing in the range is programmed or erased, the radio runs without stored
settings, and the diagnostics page names the block. Clearing it is the owner's call. ODR never programs or erases
outside the range, and never touches the factory bad blocks in it. The layer uses spare bytes 8-23 of each page and
leaves 0-7 alone (the factory mark and the stock marks at 4-5). A block the stock firmware marked bad at bytes 4-5
counts as foreign.

Logical map (the core's `storage_map_t`, [settings-store.md](settings-store.md)): 608 sectors, 2.375 MB.

| Item | Logical address | Size |
|---|---|---|
| Open-firmware codeplug, EEPROM part | 0x000000 | 64 KB |
| Settings ring | 0x011000 | 32 KB |
| Sector journal | 0x019000 | 4 KB |
| Open-firmware codeplug, flash part | 0x020000 | 1 MB |
| Native codeplug | 0x120000 | 1 MB |
| Staging | 0x220000 | 256 KB |
| Calibration | None: read from block 20 and the program flash (6.1) | |

Codeplug writes (`CONFIG_CPS_OPENFW`, section 9): the CPS shim commits one 4 KB sector at a time with
`hal_flash_t.replace` (`nand_sectors_replace`): one record of two pages, atomic, with no journal and no erase first.
The core's journaled rewrite (`core/storage`, [settings-store.md](settings-store.md) 4) written for NOR would take
about 11 page programs per sector here. The host test (`tests/host/nandSectorsTest.c`) counts them:

| Write | Page programs | Block erases (32-block region) |
|---|---|---|
| One sector, `replace` | 2 | |
| One sector, journaled erase and program | 10.9 | |
| What qdmr transfers (33 sectors: EEPROM 0x80-0x6060 and 0x7500-0xB000, custom data, codeplug 0x9B000-0xAEE60) | 66 | about 1 |
| The whole open-firmware image (the 64 KB EEPROM part and the 1 MB flash part, 271 sectors) | 542 | 8 to 10, with garbage collection |

A sector whose content doesn't change programs nothing. The calibration can't be reached: the layer works only inside
blocks 1920-1951, so neither NAND block 20 nor the program flash copy is ever written, and the CPS window in the image
(flash image 0x10000, wire 0x30000) is plain stored data that nothing reads. Speed on the radio is not measured; the
UART (115200 baud, 32-byte blocks) is the limit, not the NAND: a qdmr write is about 20 s.

The region holds the device twice over (each sector can need two copies while garbage collection runs) plus the
reserve. RAM: 2 bytes per sector and 6 per block, about 1.5 KB in all, no page buffer. The mount reads every
programmed page's spare area and the data of every current record, so it takes longer as more data is stored.

The diagnostics page (`CONFIG_DIAG`) shows the storage on lines 12-15, after the TX line 11 and before any DMR lines:

| Line | Shows |
|---|---|
| `nst u<used> f<free> bad<bad>` | Blocks holding data, free, bad. `nst foreign <block> n<count>`: the physical block holding data ODR didn't write, and how many such blocks; nothing was touched and the radio runs without stored settings. `nst none`: no storage for another reason (line 13 says which) |
| `nst w<writes> e<erases> gc<blocks> x<verify fails>` | Records written, block erases, blocks emptied by garbage collection, pages that read back wrong |
| `nst st<status>` | Without storage, line 13 shows this instead: the mount status (0 never mounted, no NAND; 2 bad set-up; 3 foreign data) |
| `nst t<torn> r<retired> #<seq>` | Torn pages the mount set aside, blocks retired this session, the next sequence number |
| `set <state> #<seq> s<saves> f<failures>` | Where the settings came from (`ram`, `dflt`, `load`, `upgr`, `frgn`), the settings record's sequence, saves and failed saves this session |

## 7. PC link, update and recovery

| Item | Finding | Tag |
|---|---|---|
| Connectors | RJ-45 mic: 1 power off, 2 PTT / radio RXD, 3 mic, 4 shield, 5 radio TXD, 6 ground, 7 +8 V, 8 speaker. 3.3 V logic. The side K1 jack carries the same serial pair | COM |
| Cable | USB-serial (Prolific PL2303 originally; FTDI accepted from 909E.D4.EARSAB.018) | COM |
| Stock PC protocol | UART1, 115200. Identifies as "+DRS-300UV" / "+136-480MHZ+". 2 KB packets with an additive checksum | BIN |
| IAP loader entry | Radio off, cable in, hold P1 and press power: red LED, blank display with backlight. The vendor "IAP" tool sends the raw .bin; the radio turns off when done. The loader also stays in update mode when the application lacks the signature (2) | COM, RADIO |
| IAP protocol | UART1, 115200. The loader prints its NAND state and "IAPReady". Host: `Erase      ` (Erase + 6 spaces) + u32 BE address 0 + u32 BE N = length / 2048; the loader answers its version string, then "Erase ok" about 1.5 s later. Then N + 1 raw 2048-byte pages, the last padded with 0xFF, each answered "kyd". About 1.4 s after the last page: `ChecksumI` + u32 BE additive sum of every byte sent, and the loader starts the application at once. It erases only as far as the image reaches | RADIO |
| Stuck in update mode | White (blank, backlit) screen and a solid red LED at every power-on without P1 held: the application at 0x0300C000 no longer starts with 2C 11 01 C0 (ODR's first stage was overwritten, or a write was interrupted), so the loader stays in update mode. Nothing answers the debug link, USMART or the first stage's window. The loader still answers the IAP protocol: `Flash Read ` + address 0 + count 1 gives its version string ("Read_2M_V1.08B..."), a page, `ChecksumR` and "IAPReady", and changes nothing. The web flasher uses that as a read-only probe (never Erase). Fix: write the install image (`odr_db25d_install.bin`, stage and application, starts 2C 11 01 C0) through the IAP route; the checksum matches and the radio starts ODR. No P1 is needed in this state; holding P1 at power-on also reaches it | RADIO |
| Loader reads | `Flash Read ` + u32 BE address + u32 BE page count, then one `Read` per 2048-byte page and one more for `ChecksumR` + u32 BE: reads the **NAND**, not the program flash | RADIO |
| Same as the DM-32's loader? | No. The DM-32UV and the DM-1801 Pro share an update-loader protocol keyed by a model string ([hardware-dm32.md](hardware-dm32.md) 13.2; `tools/dm32_flash.py --probe` checks for it without writing); the DB25-D's loader speaks the IAP protocol above instead | RADIO |
| Loader rewrite | The stock application accepts writes to program flash at 0x03000000 / 0x03001000 (the vendor ".flash" loader update). A bad transfer there can erase the loader | BIN |
| JTAG | The application never changes the IO mux of PTA0-PTA4, the SoC's JTAG pins. Pads: ? | BIN, DM32, INF |
| ROM serial boot | The HR_C7000 boot ROM offers a download on UART0 at reset. Where UART0 goes on this board: not found (no pads traced) | DM32 |
| Open-firmware CPS | The same UART1 link speaks the open-firmware CPS protocol ([open-firmware-cps.md](open-firmware-cps.md), `core/cps`) next to the debug link; the host link sorts them by the first byte (`docs/dev/cps-and-codeplug.md`). Section 9 | RADIO |
| Web flasher | `site/static/js/flasher/db25d.js` and `db25dui.js` load the universal stub through a stock radio's USMART console (Stub from stock, below), then back up and install through the stub as on a radio that runs ODR. The IAP loader above is the recovery route: a reinstall, or a backup put back, when nothing answers on the cable. The stock firmware has no boot ROM window (that exists only once ODR's first stage is installed) | RADIO |
| USMART console | UART1, the same port. A command is one frame in one write: `@AT`, one filler byte (not CR or LF), the text, CR LF; each byte must follow within 20 ms, and an unframed line is ignored. `read_addr(0xADDR)` answers `read_addr(0XADDR)=0XVALUE;`, `write_addr(0xADDR,0xVALUE)` echoes `write_addr(0XADDR,0XVALUE);`; the application's own log lines follow on the same port. The console's command table is copied to RAM 0x18000840 at boot; its `cmd_rec` entry (+8, 0x18000848) is called through the table for every frame except `01` and `DMRCALL`. The version string `EARSAB.0NN` is at 0x03079F3D (018) and 0x0307DCCD (020) | RADIO (018), BIN (020) |
| Stub at power-on | ODR's first stage offers the boot ROM's handshake on UART1 for about 0.5 s at every power-on and takes blocks below 0x4E000. The universal stub loads at 0x10000 there as on the other radios: `stubtool PORT --wait 120 --board db25d --write-app IMAGE`, then a power cycle (older stubs: add `--legacy-stub --load-base 0x30000`). The radio programming utility's Recover at Power-On does the same (boot/README.md) | RADIO |
| Stub from a running ODR | CONFIG_STUB_ENTRY is on: `stubtool --odr-enter` asks the firmware for its stub span (debug link STUB INFO) and loads the universal stub there (ODR restores the loader's clocks before the jump, section 2.1). The span is 0x30000-0x4FFFF in a build without DMR (`HRC_STUB_BASE` in `config.mk`; db25d.ld keeps ODR's RAM below it), 0x1D000-0x2EFFF with `CONFIG_DMR=y` (8). The stub copies itself from the load address to 0x30000 and runs there (in a DMR build from 0x1D000; in a build without DMR it is already there), so one command (`stub/db25d_update.sh IMAGE PORT`) installs a DMR or a non-DMR image from either kind of build. The tool reads `board=` from VERSION and sends the DB25-D profile in CONFIG (power latch PTB13, power key PTB12, UART1 only, NAND), and checks the echo before writing. `--legacy-stub` and `--odr-stub FILE` load a position-dependent stub instead, refused if linked for another span; the power-on stage catch recovers a hang. `--boot reset` restarts into ODR: the soft reset leaves the power latch on, and the loader goes through the stage again. No keys or power cycle for an update | RADIO |
| Stub RAM | The universal stub runs at 0x30000-0x37978 on every route (image 7,004 B, `.bss` to 0x36974, 4 KB stack; `stub/README.md`, RAM placement). Deepest stack use measured on this radio: 252 B, NAND reads included. Its SPI NAND reads are read only: `stubtool --nand F --nand-range A:B`, `--verify-nand F` | RADIO |
| Stub from stock (018, 020) | Run on a radio with 018 (2026-10-18): stub loaded and started, 2 MB program flash read in 68 s, the stock image in it byte for byte; 020 not yet. Stock RAM at the hand-over: 018 code and data 0x10000-0x345C4, `.bss` to 0x525E0, interrupt stack to 0x535E0, heap to 0x53DE0; 020 `.bss` to 0x4F150, interrupt stack to 0x50150, heap to 0x50950. Both: VBR 0x10000, start-up stack top 0x50000, modem tables and the console's state in the modem SRAM at 0x18000800 and up. The free window both share is 0x54000-0x58000. The console writes the universal stub there word by word (`write_addr`, little endian, unpatched), checks it with `read_addr`, and starts it by writing 0x00054000 to `cmd_rec` (0x18000848) and sending one more frame; that word is never read back, since the read would start it too. The stub masks interrupts, hands the program flash back from execute-in-place, and copies itself to 0x30000 (stock `.bss`, dead once interrupts are off; the 7,004-byte source at 0x54000 doesn't overlap). Then `stubtool PORT --attach --board db25d`. Stock leaves the loader's clocks and uses UART1 as ODR does, so the same profile applies; the host measures the clock. An address inside the interrupt stack (020: 0x4F150-0x50150) is not safe for the load | BIN |

## 8. Codec

The stock image links the HR_C7000 vocoder library (V1.9.6); AMBE+2 runs in software on the CK803S and the frames pass
through the CPU, as on the DM-32 (BIN). ODR hosts it as on the DM-32UV ([codec-dm32.md](codec-dm32.md)): the user's
machine carves the codec out of the radio's own stock file or program flash into a slot in the image, and the firmware
copies it to the board's codec window at the top of RAM at boot and moves the addresses its codec map lists
([codec-placement.md](codec-placement.md)). No codec bytes are in the repository or in a release.

### 8.1 Stock files

The 2024-05-09 build is the first a PRO can run (1), and older images must never be loaded on one, so no recipe is kept
for them. Three 018 files are supported, all plain images linked at 0x0300C000 (no header, no container, no key), and
the 020 build from a read of the radio's program flash. The three 018 files carry the same codec code, built from the
same library, with different literal pool words.

| File | Size | sha256 | Profile id |
|---|---|---|---|
| `DB25-D PRO 909E.Dx.EARSAB.018_2025-01-21.bin` | 846,700 | `341536cbac0c1b2471f8198ebc456946e5fe026d96d153df14d6aa5566304f8b` | `db25d-2501` |
| `DB25-D PRO 909E.Dx.EARSAB.018_2024-09-23.bin` | 845,900 | `f2a4b214720b971b58cdadcc53d44695e1462b75296afc41f2a0b53a704d36b4` | `db25d-2409` |
| 2024-05-09 build (`909E.D6x.EARSAB.018`) | 843,944 | `9e199f54cd6545d153d39542ca5be3ac1076ee215eb93ca630a83cbdbbff4606` | `db25d-2405` |
| 909E.DBB.EARSAB.020, a 1 MB program flash read (`stub/stubtool.py --progmem`) | 1,048,576 | none: found by the codec's own hashes | `db25d-020` |

The owner's radio runs 020, whose codec is a different compile. No stock file of that version is at hand, so its
profile works from a read of the radio. A radio can run any supported codec whatever its own firmware version, because
the codec is plain code and tables. What the read of the 020 radio shows (BIN):

| | 020 |
|---|---|
| Code start (the two signature words) | flash 0x030C142C |
| Code | Another compile: 32 of the 19,144 words match 018's at the same offset. Most 32-byte runs of the 018 code are there, shifted by 0x4F0, 0x430 and other amounts |
| Code run address | 0x180B8, inferred: the RAM image starts at flash 0x030B9374 (its first 256 bytes are 018's, and occur once), and 020's start-up copy is in the 4 KB the read lacks |
| Tables | Byte-identical to 018's (same sha256), at flash 0x0307E760 |

0x180B8 lay inside ODR's RAM in the layout from before the move. With the codec copied to the board's window (8.3), the
run address only sets the offset added to the three words its codec map lists. The 020 code is 78,172 bytes, the
largest of the four builds, and sizes the window.

The web flasher finds the codec by content: the two signature words, then the SHA-256 of the code and the tables against
every profile, wherever an image or a backup holds them. A stock file not known by its own SHA-256 is placed as the
radio would hold it and searched the same way, so a re-release that carries a known codec unchanged works. When the
signature is there but no profile matches, the page says that the radio's codec is a build ODR can't use and asks for
a supported file; the backup is not affected.

### 8.2 Where the codec is

| | 2025-01-21 | 2024-09-23 | 2024-05-09 |
|---|---|---|---|
| Code: file offset / flash address | 0xB5C58 / 0x030C1C58 | 0xB592C / 0x030C192C | 0xB5130 / 0x030C1130 |
| Code: length | 0x12B20 (76,576) | same | same |
| Code: run address (RAM) | 0x1CEB0 | 0x1CEB0 | 0x1CEB0 |
| Tables: flash address | 0x0307A8EC | 0x0307A614 | 0x0307A150 |
| Tables: length | 0x2D50 (11,600) | same | same |
| Tables: run address (modem SRAM) | 0x180014AC | 0x180014B4 | 0x180014B0 |
| First two code words | 0x152514D8, 0x74C776CD | same | same |
| sha256 of the carved tables | `e1e4358224...` | same | same |

How it was found and checked:

* The stock start-up copies the RAM block (flash 0x030B4DA8 in 2025-01-21) to 0x10000, and the modem SRAM image (0x03079C40)
  to 0x18000800. The codec's run addresses follow from where its flash bytes sit in those blocks. The first function
  (decode) is at 0x1CEB0 in the disassembly, and the stock vocoder init calls the encoder init (0x1D1C4), the FEC init
  (0x1EC7C), the encoder set (0x1D194), the decoder init (0x1CF64), the FEC init again, one decode (0x1CEB0) and the
  decoder set (0x1CF34), the order of codec-dm32.md C, with the same state offsets and parameters.
* The end of the code is the start of the stock `memcpy` that follows it, RAM 0x2F9D0: the last codec function returns at
  0x2F9C4 and its two literal-pool words end at 0x2F9D0. No function reachable from the entry points calls anything
  outside the window (161 functions from decode, encode, the FEC and the init and set entries). The two library
  functions that touch the 0x1415xxxx peripherals sit inside it, unreachable, as in the DM-32UV carve.
* The tables are the two object groups of the DM-32 049 carve (752 + 10,844 bytes), byte-identical in all three files;
  here they sit 4 bytes apart instead of 0x22C. All 58 distinct modem SRAM addresses the codec's literal pools hold lie
  inside the window, and the tables are identical across the three files (same sha256).
* The entry points, as offsets from the code start, match the DM-32 046 build except the FEC init: decode 0x0, encode
  0x260, decoder init 0xB4, decoder set 0x84, encoder init 0x314, encoder set 0x2E4, FEC init 0x1DCC. The stock
  init sequence (the state offsets, the 14 encoder parameters, the order of the calls, the first decode) is the one
  `platform/hrc7000/vocoder.c` runs.
* The first 4 KB of the owner's 020 image isn't in the read of the radio; it doesn't matter here, because the 018 codec
  lies far above it and 020's own codec is a separate recipe (above).

The code's literal pools hold three absolute codec addresses and 58 table addresses, so the code runs only at these
addresses (as the DM-32UV's). The build reserves them.

### 8.3 Build and flash

| Step | What |
|---|---|
| Stock file | Put one of the files above in `codecs/` (any name; the build recognizes it by sha256) |
| Build | `make BOARD=db25d`. Next to `odr_db25d_install.bin` (first stage + application) it writes `build/db25d/odr_db25d_codec.bin`: the same, padded with 0xFF to 0x030E8000, then the 128-byte slot header, the code and the tables. With no supported file in `codecs/` the build says so and writes only the plain images |
| Slot | 0x030E8000..0x03100000 (96 KB; 88,316 bytes used for 018, 89,912 for 020): the version 2 header of `platform/hrc7000/codec_slot.h`, the code and tables as stock has them, and the relocation list. `tools/hrc7000_codec.py --board db25d --stock FILE` writes it from a stock file or a program flash read |
| Flash | The merged image goes where `_install.bin` goes (the loader's update mode or `stubtool --write-app`, 7): it is the stage, the application and the slot in one file. The loader writes as far as the image reaches, about 1 MB, so the first flash takes a while; `stubtool --write-app` writes only the sectors that changed |
| Check | The debug link's VERSION reports `codec=db25d-2501` (or `-2409`, `-2405`, `-020`) when the slot is valid, `absent` when it's built with `CONFIG_DMR=y` and the slot is missing, damaged or version 1 |
| RAM | `CONFIG_DMR=y` links `db25d_dmr.ld`: one region for ODR, 0x10000..0x44E00 (216,576 B: the vectors, the slot engine, the core's CPU state, data, bss with the RTOS heap, the start-up stack), and the codec window 0x44E00..0x58000 (78,336 B) at the top. The tables go to modem SRAM 0x180014AC..0x18004360, which the script leaves out. The runtime stub window is 0x45000..0x58000 (inside the codec window, after the codec is parked; `HRC_STUB_BASE` in `config.mk`). The universal stub loads there and copies itself to 0x30000, which is ODR's RAM and dead after the jump (7). Without DMR the span is 0x30000 (`db25d.ld`) |

## 9. How ODR uses it (`make BOARD=db25d`)

Bring-up state (the port has run on a radio):

| Part | State |
|---|---|
| Platform | `platform/hrc7000`, image at 0x0300C000 behind the IAP loader; the host port on UART1 |
| RF | `drivers/rf/bk4819`: main and LO on one 3-wire bus. FM receive by the main chip at the IF with the LO at f + 51.55 MHz. RX sub-audio: CTCSS and DCS from the chip's detector (4.7) |
| Calibration | Both copies are read at boot, the higher save counter wins (6.1, 6.4); DAC A and DAC C are set from it for receive. The diagnostics page shows the copy used, its save counter and the PA values at 446 and 146 MHz, and a line with the PA drive for the tuned frequency |
| NAND | `drivers/flash/spi_nand` on SPI0 (6.5). Programs and erases only inside ODR's range (6.6). The diagnostics page shows its ID and the page reads ECC had to correct |
| TX | Analog FM (4.4, the stock sequence from the 2026-10-16 trace) and DMR (4.5), both measured on the SDR bench with the frequency trim of 4.6. Refused without a valid calibration page or outside the bands. The diagnostics page shows the PA drive for the tuned frequency, low / high |
| Display | `st7735` on a 4-wire serial bus: SPI2 or bit-banged, chosen by the ADC 2 strap as the stock firmware |
| Power | No battery (`CAP_NO_BATTERY`): a mobile on a fixed 13.8 V supply with no supply reading (3.3). The core shows no battery icon, percentage, Tools > Battery page, Battery saver or Low battery warning setting, and has no battery monitor, so it never warns or refuses to transmit for the battery. `hal_power_t` has no `battery_mv` or `battery_pct`. The diagnostics page shows the raw ADC channels |
| Keys | Matrix and encoder positions checked on the radio. VOL/SQL is `ACT_VOLUME`: the volume screen, then the encoder (`radio-features.md` 4.2) |
| Volume | No knob (`hal_power_t.volume()` is -1): the stored step (0..16) goes to `hal_radio_t.volume()` and sets the BK4819 AF DAC gain (3.3); the debug link sets it too (RADIO op 30). Checked on the radio: the steps change the level and step 0 is silent |
| Storage | `CONFIG_NAND_STORAGE` (on): settings kept in NAND blocks 1920-1951 through `drivers/flash/nand_sectors` (6.6). On the owner's radio the region mounted clean (no foreign data, no bad blocks) and the VFO frequency survived a chip reset |
| Codeplug over the cable | `CONFIG_CPS_OPENFW` (on): the open-firmware CPS on the host link (UART1, 115200) over the codeplug areas of 6.6, at the STM32 radios' addresses (EEPROM part at 0, flash part at 0x20000, no `--offset`). Radio info reports type 6 (MD-UV380); no MCU ROM or flash security register area; no calibration window. `tools/cps_client.py --port /dev/ttyUSB1 read / write / verify`, qdmr's MD-UV380 driver should work as well (not tried). The channels and zones come from the stored codeplug (`core/codeplug`), which the radio re-reads at the end of a session. Host-tested on a simulated NAND with power cuts; read, write and verify run on the owner's radio (read 63 s, write 76 s). `CONFIG_APRS_RX` and `CONFIG_APRS_IGATE` are off (no FM receive PCM on this radio) |
| Codec | Profiles for three 018 stock files and the 020 program flash read (8); `make` merges one into `odr_db25d_codec.bin` when its file is in `codecs/`. On the owner's radio (2026-10-19), with the code moved to the codec window: the 020 read and the 2024-09-23 file each give VERSION `codec=db25d-020` / `db25d-2409` and decode a bench voice call with every one of its 234 AMBE frames |
| DMR | `CONFIG_DMR=y` (default on): the shared HR_C7000 glue of the DM-32UV with the main BK4819's stock digital set (4.5). Receive works on the radio: a bench voice call decodes with every AMBE frame, and a bench SMS arrives (4.5). Speaker audio for DMR not yet heard. Transmit (`DB25D_DMR_TX=1`, default on): the stock per-burst TX sequence from an RF task (4.5), checked on the SDR bench; `DB25D_DMR_TX=0` builds receive only. Builds clean with every DMR feature, the network ones included (the next rows) |
| Hotspot | `CONFIG_HOTSPOT=y` (default on): the MMDVM host port on UART1, simplex. The core's hotspot drives the same `hal_dmr_modem_t` as a call (`tx_begin`, `tx_queue`, `tx_end`) and keys through `hal_radio_t.tx()` in DMR mode, so it runs on the per-burst TX path of 4.5 (the `dmrrf` RF task) with nothing added to the board: whole calls and data go out as the host sends them, and a retune (`rx()`) disarms the keying until the hotspot arms it again. A split RX / TX frequency pair works as a call's does (each burst tunes to the TX frequency, then back). Not offered: the split-duplex roles (`CONFIG_HOTSPOT_SPLIT=n`: the driver has no continuous BS outbound, so the core would refuse the role) and SFR / `tx_listen` per-slot switching (`CONFIG_SFR=n`). Checked on the radio with `tools/bench/hotspot_cases.py` against the SDR bench modem (frequency 461.4625 MHz, color code 2): GET_VERSION, SET_FREQ, SET_CONFIG, SET_MODE and GET_STATUS answer (status DMR, TS2 space 22); all 13 cases pass (`rf_voice8`, `rf_late`, `rf_long`, `rf_lost`, `rf_b2b`, `rf_csbk`, `rf_data_unconf`, `rf_data_conf` for uplink frames; `net_voice`, `net_voice_ctl`, `net_data`, `net_data_conf`, `net_b2b` for calls sent from the host and heard on air). The radio is back on its channel within 25 s of the last frame |
| DMR VFO | `CONFIG_VFO_DMR=y` follows the default: the core's DMR VFO (`dmr-voice.md` 2.1; the VFO quick menu's Mode tile switches it FM / DMR). On the radio (2026-10-17, debug link): mode, color code, slot and a group or private target set and kept across leaving the VFO and a restart; at 461.4625 MHz CC 2 TS 1 TG 201 a bench call received with every voice burst (120 voice, 8 headers, 3 terminators), and a 4 s key-up decoded by the bench modem as a group call to TG 201 (72 voice bursts, 216 AMBE frames). A record written by an older build keeps the color code its FM VFO stored (often 0) until the user sets one |
| IP link | `CONFIG_NET_PPP=y` (default on): PPP on the host port's UART1 (`board_t.netif` is `hostlink_ppp_netif`, as on the DM-32UV), and with it `CONFIG_DMR_IP_GATEWAY` and the services on it: UDP compression, DMR confirmed data, TCP (3 connections, `TCP_CONNS`), KISS, HTTP (gzip), rigctl, APRS-IS, mDNS, TFTP, SNMP and the control station. USRP is off (`CONFIG_NET_USRP=n`): its 8 kHz 16-bit PCM is 128 kbit/s plus headers, more than the 115200 baud host port carries, and a heard call started the stream. Checked on the radio with pppd and `tools/odr_ppp.sh`: LCP and IPCP come up, ping 30 ms, 5/5; HTTP 200 (also gzip); rigctl `f` answers the channel frequency over TCP 4532; SNMP `sysName.0` answers; the debug link over UDP answers; 48 HTTP requests in six parallel loops all succeed with no PPP errors and no TCP retransmits (six clients against three connection slots: `tcp.no_slot` counted 7 SYNs that found no slot, and curl retried them); `odr_ppp.sh --stop` sends the LCP Terminate and the debug link answers on the serial port again. Found on the radio: with the USRP stream on, a bench DMR call heard while PPP was up wedged the host link task (`send_rx()` in `core/usbaudio/usrp_net.c` never left its loop, because a frame takes 30 ms to clock out of the UART and holds 20 ms of audio); the radio stopped answering LCP and the debug link until a power cycle. `send_rx()` now gives up after 20 ms of sending per poll (host test `usrpNetTest`). Not checked: mDNS, TFTP, APRS-IS, KISS and the control station; a DMR call heard while PPP is up, after the fix |
| RAM | Main RAM (above the codec window, 165,424 B) was 139,952 B used with DMR alone and is 150,080 B with the network and hotspot, USRP off (15.3 KB free; 155,704 B with USRP, 9.5 KB free); RAM_LO (52,912 B) went from 19,024 B to 28,584 B used (24 KB free). The network's buffers sit in RAM_LO (`.ram_lo`, as `ODR_CPU_RAM` marks them). The RTOS heap is 40 KB, the DM-32UV's, and holds the task stacks. Costs from the ci builds: IP gateway and PPP 15.0 KB main + 6.7 KB RAM_LO; TCP 5.0 KB + 3.4 KB; USRP 5.6 KB; SNMP 3.2 KB; HTTP 2.5 KB; hotspot 2.6 KB + 2.8 KB; roaming 2.2 KB + 0.9 KB; the rest under 1 KB each |
| GPS | `CAP_GPS`, reported once the module is heard. `platform/hrc7000/gps_hrc.c` (shared with the DM-32UV, [hardware-dm32.md](hardware-dm32.md) 12) powers it in `board_init`, listens 5 s on UART2 for one checksum-valid sentence, feeds every sentence to the APRS position source, and powers it off again when nothing is heard. Once heard, the module is powered only while Settings > Location > GPS is on (`board_t.gps_enable` = `hrc_gps_enable`, applied at start-up and on each change). The diagnostics page line `gps yes` (or `gps no`) `GOOD/BAD bBYTES on|off` (after the storage lines) shows valid and bad sentences, bytes received and whether the module is powered now. Run on the radio 2026-10-17: module heard, `gps yes 171/0 b7735` (171 good, 0 bad sentences); the switch turned it off and on again live, and the setting held across a restart. `b0` with the module fitted would mean the RXD mux (PTA11) or the power level is wrong |
| Left off | `CONFIG_SFR` (per-slot RX / TX retune not timed on this radio), `CONFIG_APRS_RX` and `CONFIG_APRS_IGATE` (no FM receive PCM), USB and USB network (no data port), broadcast FM and air band AM (not wired), `FONT_XL` and `AUDIO_PROBE` |
| Older batches (FD6818 main) | Not supported yet |

**DMR receive check** (passed on the owner's radio, 4.5): build with the stock file in `codecs/` and `make BOARD=db25d`; flash `odr_db25d_codec.bin`; with a DMR repeater or the SDR bench's DMR source on a channel of the zone, read the
diagnostics page. In order: the codec line shows `db25d-...` and a non-zero tag; the sync counters move (the AF phase range isn't a level check on this board, 4.5; if nothing syncs, try the AF gain
and RF_MODE knobs of 4.5); `ts` and `si` climb (the modem has slot timing); then V / H / T counts rise on a talk burst.
Voice and the speaker route come last. `stubtool --odr-enter` loads the stub into a DMR build's 0x1D000 span by itself (7).

**DMR transmit check** (passed on the owner's radio, 4.5; on a dummy load or into the SDR bench): key a
DMR channel on the low power level. On the diagnostics page `b` (bursts keyed) and its pair (bursts ended) climb by 16
or 17 a second and stay equal after the key-up; `L` stays 0 or small. On the SDR bench: bursts in one timeslot, carrier
on frequency, the ramp ending before the other slot, then sync and the voice header decoded. No sync with a clean
carrier: try `DB25D_DMR_TX_INVERT=1`. Low deviation: compare the page's modulation byte (6.3, 0x067) with the stock
radio on the same channel.

## 10. Open questions

| # | Question | Needs the radio? |
|---|---|---|
| 1 | Firmware version in Device Info (expect 909E.DBB...) | Yes, menu only |
| 2 | Program flash part and size; JTAG and UART0 pads | Yes, photos of the control board |
| 3 | Panel size and which bus mode the PRO uses (ADC 2) | Yes |
| 4 | Calibration page: the 0x09B / 0x160 / 0x1B1 / 0x2xx items (6.3). Real values are known: 0x045 is the higher DAC value, so the high level; that this is more output needs a power measurement | A power meter or the SDR bench |
| 5 | Can a stock CPS read back the calibration page (a backup path)? | Yes, serial capture of a CPS read |
| 6 | Any model check in the IAP loader (the protocol is in 7) | A foreign image sent through it |
| 7 | Hand-mic key protocol on RJ-45 pin 2 | Yes, logic analyzer |
| 8 | Hand mic key numbers 15-17, if any | Yes |
| 9 | Band assignment of the RF switch pins (UHF = PTB3 / PTB4 is by analogy) | No, then a meter |
| 10 | Supply voltage: no ADC channel known (3.3). ADC 7 read at two supply voltages would show whether it follows the supply | No, then a meter |
| 11 | Which NAND address ranges the stock bad-block skip covers (6.5) | No, the stock image |
| 12 | Answered (4.5): the stock digital set with RF_MODE 0x034C9860 decodes every frame | - |
| 13 | The modem's input is the main chip's AF output (4.5, decodes on the radio). The voice output's speaker route (PTC18 low) still needs a listen | Yes, a listen |
| 14 | Answered (4.5): single-point modulation on the TCXO, the stock per-burst sequence, decoded on the SDR bench; deviation 101 % of nominal at 461.4625 MHz | - |
| 15 | The 020 stock codec's windows, entry points and a RAM layout for its run address (a separate compile at 0x030C142C, 8.1) | A 020 stock file, or a read of 0x0300C000-0x0300CFFF |
